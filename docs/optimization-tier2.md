# 优化方案 · 第二梯队：中等工作量

> **定位**：结构性改造，涉及所有权、线程与生命周期。每项都改变引擎的装配方式或
> 运行时行为，**必须以真机验证收尾**，不能只靠合成回归。
>
> **前置**：第一梯队中 T1-3（模块解耦）是 T2-1 的硬前置；T1-4 的 pread 改造是
> T2-3 的前置。

## 总览

| 编号 | 任务 | 类型 | 主要收益 |
|---|---|---|---|
| T2-1 | EngineContext 装配收敛 | 架构 | 消灭双引擎、裸全局、四份 boot 重复 |
| T2-2 | Draw() 热路径零分配 + 重绘门控 | 性能 | 每帧堆分配清零，静止场景免重绘 |
| T2-3 | 音频解码移出回调线程 | 稳定性/性能 | 消除低端机欠载风险，音频抖动根治 |

---

## T2-1 EngineContext 装配收敛

### 问题与证据

- **双引擎并存**：`src/jni/jni_bridge.cpp:149-156` new 出一套**无 Compositor** 的
  PackManager+LuaEngine；`src/jni/native_activity.cpp:336`（`g_state.lua`）又一套。
  两者状态互不相通。
- **boot 逻辑四份重复**：
  - `jni_bridge.cpp:157-170`
  - `native_activity.cpp:328-416`
  - `src/host/mac_main.mm:132-358`
  - （CLI `host_drive` 亦有一份装配序列）
- **所有权倒挂**：script 模块裸 `new` 持有 audio（`lua_engine.cpp:618,620`），
  手动 delete（`:526-528`），`:2844` 处 `delete sounds_; sounds_ = new ...`
  无异常安全；并以 `unique_ptr` 持有 render 的 VideoPlayer/EmotePlayer
  （`lua_engine.h:348-351`）——脚本引擎成了渲染/音频对象的工厂。
- **裸全局**：`jni_bridge.cpp:28-30` 的 `g_packs/g_lua/g_vm`（nativeInstall 中
  裸 new/delete 无清理）；`native_activity.cpp:214` 的 `EngineState g_state`
  （线程/mutex/窗口/引擎全塞一个全局）。

### 方案

新增 `src/core/engine_context.{h,cpp}`，成为**唯一装配点与唯一所有者**：

```cpp
class EngineContext {
public:
    // data_dir：pf8 包所在目录；os_id：ios 节选择（windows/android/…）
    bool Boot(const std::string &data_dir, const std::string &os_id,
              const std::vector<uint8_t> &explicit_key = {});
    void Shutdown();

    // 访问（生命周期内非空）
    PackManager &packs();     Audio &audio();      AudioChannels &sounds();
    Compositor &compositor(); LuaEngine &lua();

private:
    std::unique_ptr<PackManager>   packs_;
    std::unique_ptr<Audio>        audio_;
    std::unique_ptr<AudioChannels> sounds_;
    std::unique_ptr<Compositor>    compositor_;
    std::unique_ptr<LuaEngine>     lua_;
    // boot 序列（原四份重复的收编点）：ini 解析 → pack 链 → audio →
    // compositor(stage 尺寸) → lua 装配 → init.lua 启动链 → adv 框架
};
```

关键决策：

1. **所有权归 EngineContext，LuaEngine 只持引用**：
   `LuaEngine::Init`（现为六参数 + 8 个 Set*Handler，`lua_engine.h:59`）改为收
   `Audio*` / `AudioChannels*` / `Compositor*` 非拥有指针；析构删除所有
   `delete audio_` / `delete sounds_`。
2. **VideoPlayer/EmotePlayer 下放 Compositor**：二者与 GL 生命周期同进退
   （GL 丢失时一并释放、重 boot 重建），挂在 Compositor（T3-1 拆分后更自然）
   或 EngineContext 均可——建议 Compositor，让"GL 资源全在一个类里释放"
   的现有设计（`ReleaseGl`，`compositor.cpp:1438-1458`）保持完整。
3. **双引擎合并**：`DebugBridge.nativeInstall` 路径与 ANativeActivity 路径
   **先取证再合并**——两份 boot 的初始化顺序可能有刻意差异（install 提前装配
   供 Java 侧 `artc` 式查询）。方法：在两条路径打点日志，真机比对初始化顺序与
   ini/包访问序列，确认无隐藏依赖后统一为 EngineContext 一份。
4. **g_state 收缩**：`EngineState` 中线程/mutex/窗口等宿主设施留在
   native_activity（那是 ANativeActivity 的职责），引擎指针改存
   `std::unique_ptr<EngineContext>`。多 Activity 场景（分屏/多窗口）暂不承诺，
   但全局裸指针清零后不再结构性排除。

### 实施步骤

1. （前置）完成 T1-3。
2. 引入 EngineContext，`native_activity` 先切换（最复杂路径先行）。
3. `mac_main`、`jni_bridge`、`host_drive` 依次切换，每切换一处跑对应冒烟
   （mac 宿主跑真游戏目录、jni 路径真机安装启动）。
4. 删除 `jni_bridge` 第二套引擎与 `g_packs/g_lua/g_vm`；`nativeInstall`
   改为创建/查询 EngineContext。
5. LuaEngine 去所有权化（delete 删除、Init 签名调整）。
6. 全局符号清点：`nm` 检查 so 导出面无新增意外符号。

### 验收标准

> **落地状态（本次）**：装配收敛与所有权改造已完成并本地验证；标 [真机] 的项
> 需设备复验（mac 宿主与 `artc drive` 已冒烟通过）。

- [x] boot/装配序列全仓唯一（grep 不到第二份 `new PackManager` + `new LuaEngine` 组合）。
- [x] `g_packs/g_lua/g_vm` 删除；`g_state` 不再持有引擎裸指针（改持
      `unique_ptr<EngineContext>`）。
- [ ] [真机] 全流程：首启（含 `nativeInstall` 路径）、窗口旋转、GL 丢失恢复
      （三态交接）、退后台/回前台。
- [x] mac 宿主与 Android 行为一致（同一游戏目录冒烟 + 两目标构建）。
- [x] 全量 ctest 绿（16/16）。

### 风险与缓解

- **boot 顺序差异是行为风险**（见关键决策 3）——取证驱动，不盲并。
- 回退：EngineContext 是新增类，宿主逐个切换可停在任意中间态。

---

## T2-2 Draw() 热路径零分配 + 重绘门控

### 问题与证据

- `src/render/compositor.cpp` 每帧堆分配（Draw 路径上）：
  - `:1517` 排序用的临时 vector（每帧构造）；
  - `:1548` / `:1585` 每层 vertices vector；
  - `:1620` 每帧构造的纹理表（`std::map`）；
  - `:1663-1683` 诊断字符串拼接。
- **无条件全量重绘**：Android 循环每帧 `Draw()`（`native_activity.cpp:603-605`），
  `Revision()`（`compositor.h:184`，每次图层变更单调递增）只有宿主回读在用。
  静止场景（对话等待点击、标题待机）白白重绘全部层。

### 方案

**第一步：帧内复用（无行为变化）**
- `sorted` 层序 vector、vertices buffer 提为成员 scratch（`std::vector` reserve
  一次，之后 `clear()` 复用容量）。
- 每帧纹理表 → 成员缓存 + 显式失效点（`LoadImage`/`DeleteLayer`/`ReleaseGl`
  已是所有变更入口，在入口处打脏标记）。
- 诊断字符串：确认 `:1663-1683` 的触发频率（每 300 帧），改惰性构造 +
  帧率门控不变即可，重点是确保非诊断路径零字符串分配。

**第二步：重绘门控（行为有变化，需观测期）**

```cpp
// native_activity 帧循环
const bool animating = ctx.lua().PendingAnimationMs(now) > 0
                    || ctx.compositor().TransitionActive()
                    || ctx.compositor().PendingTextMs(now) > 0
                    || ctx.video_active();           // 视频帧到达
if (ctx.compositor().Revision() != last_drawn_rev_ || animating)
    ctx.compositor().Draw();
```

- 全部用**现成 API** 组合（`compositor.h:109, 184, 209, 217`），不发明新状态。
- **视频是陷阱**：视频帧到达必须触发重绘——确认 VideoPlayer 解码出帧时是否
  bump Revision；若不 bump，补上（视频帧本身是图层纹理变更，语义上就该 bump）。
- **保守灰度**：先只在"完全静止"场景生效（`!animating && rev unchanged`），
  真机日志观测误判（漏重绘）一周再扩大门控范围。
- Android 的 `Revision()` 读回路径（宿主帧读回跳过）已有先例，语义对齐。

### 实施步骤

1. scratch 复用 + 纹理表成员化 → `compositor_regressions`（ANGLE 路径）+
   真机肉眼冒烟。
2. VideoPlayer 出帧 bump Revision 取证（日志确认）。
3. 门控上线（保守条件）→ 真机长跑：对话推进、lytween 长背景动画与对话并行、
   trans 过渡等待、视频播放、E-mote 播放五个场景。
4. 帧率与 GL 调用数记录（draw[] 日志 / GAPID 抽查），量化收益。

### 验收标准

> **落地状态（本次）**：scratch 复用、revision 门控、宿主门控已完成；mac 宿主
> 冒烟（boot logo → 标题动画 → 静止跳过）正常。GPU 量化与五场景全量复验留
> [真机]。

- [x] Draw 路径零每帧堆分配（scratch 成员 + 纹理表 revision 门控重建；
      `std::vector::clear()` 保容量）。
- [x] 静止场景 GL 绘制调用降为 0（不再 `Clear+Draw+Present`；动画/过渡/文本
      期间照常每帧绘制）。
- [ ] [真机] 五个重点场景（含动画+过渡+视频）无漏帧、无卡死；`dumpsys gfxinfo`
      对比 GPU 占用。
- [ ] `compositor_regressions`（需 ANGLE，`ARTC_TEST_GLES=ON`）——本机未跑。

### 风险与缓解

- 漏重绘 = 画面冻结假死。缓解：保守条件 + 观测期 + Revision 覆盖面审计
  （所有公共变更入口——LoadImage/SetProps/SetText/AddTween/DeleteLayer/
  ReleaseGl/SetPixels/RenameLayer——逐一核对 bump）。
- 回退：门控是独立 if，一行 revert。

---

## T2-3 音频解码移出回调线程

### 问题与证据

- **OpenSL 回调线程内联执行 Vorbis 解码**：`src/audio/audio.cpp:92-96` 回调直接
  调 `vorbis_stream.cpp:59-80` 的解码。回调线程有硬实时约束（BufferQueue 到期
  就必须给数据），在其上跑解压缩是欠载根因。
- 双缓冲仅 2×4096 samples（`audio.cpp:66`，44.1kHz 下约 85ms 余量）——低端机
  CPU 被渲染/脚本占用时，一次解码抖动即爆音。
- 内存：Vorbis 压缩文件**整段常驻内存**（`vorbis_stream.cpp:13-21`），多轨长 BGM
  叠加压力大。
- 顺带缺陷：`audio.cpp:150-151` 第二次 `Queue()` 返回值被忽略。

### 方案

**解码与回调解耦：专用 feed 线程 + 环形缓冲**

```
feed 线程（新增，per AudioChannels 一条或全局一条）:
    loop {
        for 每个 active channel:
            while (ring 可写 && 未到流尾) ring.push(decode_chunk())
            // decode_chunk 内部做 stb_vorbis 分块解码（现有逻辑）
        wait(水位事件 / 新流事件 / stop 事件)
    }

OpenSL 回调:
    ring.pull(silence_if_empty)   // 纯内存拷贝，O(1)
    Queue(...)                    // 返回值检查（修 :150-151）
```

- **环形缓冲**：每声道 lock-free SPSC ring（回调单读者、feed 单写者），
  容量 4-8 × 4096 samples（约 0.4-0.8s 余量，覆盖低端机解码抖动）。
- **水位事件**：ring 用量 < 50% 时 feed 线程唤醒补齐；补满再睡。
- **生命周期**：feed 线程归 AudioChannels（或 EngineContext，与 T2-1 协调），
  `Shutdown` 时 stop 事件 + join；GL 丢失重 boot 不影响（音频不依赖 GL，
  现状生命周期静音逻辑保持）。
- **策略开关**：`_a`→`_b` 曲目接续、`sfade`/`sxfade`/`sepan` 增益声像、
  `[wait se=]`、`setonsoundfinish` 等逻辑全部不动——只换数据供给方式，
  混音（逻辑声道 + 定时增益）留在消费侧现状位置。
- **内存（可选二期）**：压缩数据改分块 `ReadRange` 流式读（T1-4 的 pread 已
  铺路），淘汰整段常驻。一期先不动，避免与 feed 线程耦合过多变更。
- **诊断**：underrun 计数器（ring 空时回调拉到静音的次数）进 OutputLog，
  真机观测用。

### 实施步骤

1. `pcm_stream` / `vorbis_stream` 保持解码接口，新增 ring + feed 线程骨架，
   `audio_stream_regressions` 扩展：水位、流尾、`_a`→`_b` 接续、stop。
2. OpenSL 回调改纯拷贝消费 + Queue 返回值检查。
3. 压测：合成夹具中人为压低 feed 线程优先级（`nice`），断言 ring 水位下降
   但零 underrun。
4. 真机：长 BGM + 双 SE 并发 30 分钟，日志零 underrun；`sfade` 交叉淡化、
   语音连播、`[wait se=]` 全过。

### 验收标准

> **落地状态（本次）**：feed 线程 + SPSC ring 已实现，回调只剩拷贝；ring 行为
> 已入 `audio_stream_regressions`。OpenSL 真机指标留 [真机]。

- [x] OpenSL 回调内无解码调用（回调函数体只剩 ring 拷贝 + 静音填充 + `Queue`）。
- [x] `audio.cpp` 两次启动 `Queue()` 返回值均已检查。
- [x] 回归：`audio_stream_regressions` 全绿 + 新增水位/回绕/SPSC 顺序用例。
- [ ] [真机] 30 分钟并发播放零 underrun（`audio: underruns=` 日志断言）。
- [ ] [真机] 低端机（或限频真机）对话推进 + BGM 场景无爆音。

### 风险与缓解

- 回调与 feed 的竞态：SPSC ring 无锁约束下天然安全，但 `sfade` 交叉淡化若
  跨线程改增益需原子化（确认现状增益参数是否回调侧读取——是则保持只读即可）。
- 线程优先级：feed 线程设 `SCHED_OTHER` 默认即可，勿用实时优先级（解码抖动
  靠缓冲吸收，不靠抢占）。
- 回退：feed 线程模型在 `AudioChannels` 内部，接口不变，回退为直调模式需
  保留小开关（编译期或运行时）。

---

## 任务依赖与顺序建议

```
T1-3 ──> T2-1（硬前置）
T1-4 ──> T2-3（pread 铺路；T2-3 二期流式读依赖）
T1-2 ──> T2-2（同改 compositor 文件，glyph cache 先行减少冲突）
```

建议顺序：**T2-1 → T2-3 → T2-2**（先定装配与线程模型，最后做门控这类
行为敏感的性能开关；门控依赖 T2-1 的 `ctx` 访问面）。
