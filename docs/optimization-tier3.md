# 优化方案 · 第三梯队：长期方向

> **定位**：改变项目质量水位与可持续性的投资项。不阻塞第一、二梯队，但显著
> 决定项目能走多远——渲染路径可测、关键逻辑有回归网、帧节奏现代化。
>
> 建议节奏：每项独立立项，与功能迭代交替推进，不要求一次性完成。

## 总览

| 编号 | 任务 | 类型 | 主要收益 |
|---|---|---|---|
| T3-1 | 渲染条件编译收敛（no-GL 桩与 GL 实现拆分） | 构建/测试 | GL 路径默认可编译可测试，桩/实现漂移编译期暴露 |
| T3-2 | 关键路径测试补齐 | 质量资产 | AsbRunner / 存档 / iet / JNI 有回归网 |
| T3-3 | 帧节奏 vsync 对齐 | 体验/功耗 | 消灭 16ms sleep 忙等，功耗与流畅度双赢 |

---

## T3-1 渲染条件编译收敛

### 问题与证据

- `src/render/compositor.cpp:1714-1917`：**整段 no-GL 桩**与 GL 实现在同一文件
  内以 `#ifdef` 交错。桩与真实实现是两套并行代码，改 GL 路径极易忘改桩，
  反之亦然——漂移只能靠运行时肉眼发现。
- 默认构建（CLI / tests）里 GL 路径编译为桩 → **真实渲染代码默认零编译零测试**。
- `compositor_regressions` 需要手动开 ANGLE（`tests/CMakeLists.txt:95-115`），
  常规贡献者不会跑 → GL 路径回归实际长期空白。
- `artemis-mac` 目标把全部核心源以 `ARTC_HAS_GLES=1` 重编译一遍
  （`CMakeLists.txt:113-118`）——同源码两种宏配置并存，证明"桩/实现拆分"在
  构建层面早已需要。

### 方案

**物理拆分，共享内部头。**

```
src/render/
  compositor.cpp          # 调度层：图层状态、tween、文本布局（无 GL 调用）
  compositor_internal.h   # 共享：Compositor 私有区、GlProgram 等结构
  compositor_gl.cpp       # ARTC_HAS_GLES=1：InitGl / CreateTexture / Draw / ReleaseGl
  compositor_stub.cpp     # ARTC_HAS_GLES 未定义：桩实现（薄，多数 no-op 返回）
```

- CMake：`artemis_core` 默认编 `compositor_stub.cpp`；Android 目标与
  `artemis-mac` 通过 target 级源选择换 `compositor_gl.cpp`（源文件选择替代
  宏交错，桩与 GL 不再同文件）。
- **更进一步（目标态）**：macOS 已验证 legacy GL 2.1 兼容层可行
  （`gles2_headers.h` / `shader_compat.h`），host 默认构建直接编 GL 路径，
  桩仅保留给无头环境（交叉编译、嵌入式 CI）。这样：
  - 默认构建即覆盖真实渲染代码（编译期暴露桩/实现签名漂移）；
  - `compositor_regressions` 在 macOS/Linux CI 免 ANGLE 直跑（GL 路径）。
- Linux CI 加 ANGLE preset 作为无 GPU runner 的兜底（现有 ANGLE 条件配置
  保留，从"唯一路径"降级为"备选路径"）。

### 实施步骤

1. 纯移动拆分（不改任何代码行）→ 四目标全绿。
2. CMake 源选择逻辑 + host 默认切 GL 路径 → `compositor_regressions` 直跑。
3. Linux CI（或本地 Docker）加 ANGLE 兜底 preset。
4. 顺带处理 `CMakeLists.txt:63-70` 的 `GLOB_RECURSE`：改为显式源列表或
   `CONFIGURE_DEPENDS` 审计（GLOB 对新文件静默、对删除文件迟钝，显式列表
   更适合此规模）；补 `install()` target（当前无安装规则，下游集成只能手拷
   `build-android/libartemis.so`）。

### 验收标准

> **落地状态（本次）**：仅完成低风险切片 —— `cmake --install` 规则（含 core 静态库、
> Android `.so`、CLI/mac 宿主、公开头）。**物理拆分暂缓**：GL 与共享函数并非按行
> 连续分布（`LoadShader` 在共享段且需 stub/GL 双实现，`CreateTexture`/`SetText`/
> `DrawTransitionOverlay` 等在两段各有一份），必须逐函数分类后再移动，否则默认构建
> 会重复定义或链接失败。拆分按 §"实施步骤 1" 独立立项进行。

- [ ] 桩与 GL 实现不同文件，`grep -c '#ifdef' src/render/compositor.cpp` 大幅下降。
- [ ] 默认构建（macOS/Linux host）编译的是 GL 路径，`compositor_regressions`
      免 ANGLE 全绿。
- [ ] Android 产物 `libartemis.so` 符号面无变化（`nm` diff 断言——行为等价证据）。
- [x] `cmake --install` 可用（artemis_core / artemis(.so) / artc / artemis-mac + 头）。

### 风险与缓解

- 拆分本身低风险（纯移动）；风险在 CMake 源选择遗漏 Android 特有源。
  缓解：四个目标全量编译 + 真机 so 符号 diff。

---

## T3-2 关键路径测试补齐

### 问题与证据

**无测试的核心子系统**（tests/ 现有 17 个文件未覆盖）：

| 子系统 | 文件 | 为什么回归风险高 |
|---|---|---|
| **AsbRunner** | `asb_parser.cpp` | 可重入原生 runner + 事件返回帧 + 脚本栈 + 跨文件 call/return——叙事推进核心，任何状态机回归 = 游戏卡死或跳剧情 |
| **save_storage** | `save_storage.cpp` | O_EXCL+fsync+rename 原子写——存档损坏是不可恢复损失 |
| **native_save** | `native_save.cpp` | BOWS zlib + 1003 变量导入（经游戏 `onLoad` 恢复）——读档黑屏根因常在此 |
| **iet_interpreter** | `iet_interpreter.cpp` | `[lua]`/标签/文本混合解释 |
| **JNI/宿主** | `jni_bridge.cpp`、`native_activity.cpp` | boot 序列、GL 丢失三态交接、输入 looper——真机专属但状态逻辑可合成测 |

佐证：第二梯队多项改造（T2-1 装配收敛、T2-3 线程模型）的验收都依赖这些
子系统的回归网，**先有网才敢动结构**。

### 方案

**夹具原则不变**：全部合成（沿用现有"自绘矩形字体 + 生成音调"传统，
README「宿主回归测试」一节），不含任何商业游戏资源。

**优先级排序（按回归风险 × 变更频率）：**

1. **AsbRunner 状态机**（最高优先）
   - 合成 .asb 夹具：嵌套 call/return（3 层深）、跨文件 call、jump 前后
     脚本栈一致性、事件返回帧可重入（中断→恢复同一 runner）。
   - 断言：执行轨迹（tag 序列）与预期逐一对应。
2. **save_storage 原子性**
   - 临时目录故障注入：写中途 kill 进程（`fork` + `_exit` 模拟）→ 重启后
     旧存档完好（rename 原子性证据）；满盘（`ulimit -f`）→ 写失败不损旧档；
     权限拒绝 → 明确错误返回。
3. **native_save BOWS roundtrip**
   - 合成 BOWS 二进制（变量图 + 图层日志）→ 解码 → 断言变量值/图层状态；
     配合 `pluto_codec` 已有异常安全注释（`pluto_codec.cpp:129-131`）补
     longjmp 边界用例。
4. **iet_interpreter**
   - `[lua]` 块/标签/文本交替的合成剧本；与 asb 路径的语义对齐用例
     （`asb_parser.cpp:93-98` 的 ParseIetScript 与 IetRunner 两条路径——
     对同一输入断言一致输出，为未来路径合并兜底）。
5. **T1-1 联动**：`l_tag` 拆分时每个 handler 一条最小用例（分发正确性 +
   UNIMPLEMENTED 回退），随拆分增量提交。
6. **JNI 冒烟（host 侧）**
   - `host_drive` 已是帧循环驱动器——扩展为生命周期冒烟：boot → N 帧 →
     模拟 GL 丢失（ReleaseGl + 重 boot）→ 继续帧循环不死锁。
   - 真机专属部分（ANativeActivity 回调序列）保持真机验收，不强行合成。

**测试组织**：新文件命名沿用 `*_regressions.cpp` 惯例；AsbRunner 用例放
`asb_runner_regressions.cpp`，存档三件套合并一个 `save_regressions.cpp`。

### 实施步骤

按优先级逐个立项；每补一个子系统，对应第二梯队任务的前置就绪一项
（T2-1 需要 JNI 冒烟、T2-3 需要 audio 已有网、T2-2 需要 compositor 默认可测
即 T3-1）。

### 验收标准

> **落地状态（本次）**：新增 `asb_runner_regressions` / `save_regressions` /
> `iet_regressions` 三套件，ctest 16 → 19。覆盖 AsbRunner 状态机（嵌套/跨文件/
> pc_pending_/事件帧/DiscardFlow）、存档原子性与故障注入（崩溃留痕/RLIMIT 半途失败/
> 只读目录）、iet 两条路径一致性。**`native_save` BOWS 与 `host_drive` 生命周期
> 冒烟未做**（前者需合成 BOWS 固件，后者需合成最小游戏包），留作独立立项。

- [x] `ctest` 16 → 19；AsbRunner/save_storage/iet 各有专属套件。
- [x] 故障注入用例（写中途失败 / 满盘 via RLIMIT_FSIZE / 权限拒绝）证明存档原子性。
- [x] asb 与 iet 两条脚本路径的一致性用例绿。
- [ ] `native_save` BOWS roundtrip 套件；`host_drive` 生命周期冒烟进默认 ctest。

### 风险与缓解

- 合成 .asb/BOWS 夹具需要逆向格式知识沉淀（格式注释已在
  `asb_parser.h` / `pf8_reader.h` 头部，按同风格补夹具生成注释）。
- 无本质回退风险（纯增量）。

---

## T3-3 帧节奏 vsync 对齐

### 问题与证据

- Android 帧循环以 **16ms 固定 sleep** 定帧（`src/jni/native_activity.cpp:608`）：
  - 与显示刷新率无同步：60Hz 屏上因 sleep 精度漂移产生节拍抖动（judder）；
  - 120Hz 屏上白白锁死 60fps，且 sleep 期间线程空转唤醒；
  - 帧率与功耗强耦合：静止场景（T2-2 门控前）每 16ms 全量重绘。
- 现有 present 钩子：`Compositor::SetPresent`（`compositor.h:126`）绑定
  Renderer::Present——`[flip]` 语义 = 绘制 + 交换，替换交换策略不影响 tag 层。

### 方案

**首选：eglSwapInterval + 移除 sleep**

```cpp
// EGL context 初始化后（一次性）
eglSwapInterval(egl_display, 1);   // eglSwap 阻塞至下一次 vsync

// 帧循环（替换 16ms sleep）
while (running) {
    PumpEventsAndAdvanceEngine();   // 输入事件、脚本推进、tween 更新
    if (ShouldDraw()) {             // T2-2 门控（依赖到位后）
        compositor.Draw();          // 内部经 present_cb -> eglSwap 阻塞至 vsync
    } else {
        AChoreographer_postFrameCallback(...);  // 见"门控联动"
    }
}
```

- `eglSwapInterval(1)` 后 `eglSwap` 自带节拍——删除 sleep 不等于忙等，
  交换本身提供背压。
- **线程时序变更审查**（这是本任务唯一真风险）：现状 Draw 在引擎线程、
  `eglSwap` 阻塞会占住该线程至 vsync。影响面：
  - 输入事件走独立 looper 线程（`native_activity.cpp:295-319`），不受阻塞——已隔离，安全；
  - 音频回调独立线程（T2-3 后解码也在 feed 线程）——不受影响；
  - 脚本推进与 Draw 同线程现状下，"推进→绘制→交换"串行本来就是帧内顺序，
    vsync 阻塞等价于把 sleep 挪到帧尾（更优：绘制结果即时上屏，无 sleep 漂移）。
- **门控联动（依赖 T2-2）**：静止场景不 Draw 也不应空转——用
  `AChoreographer_postFrameCallback` 或轻量条件变量等待"Revision 变更 /
  输入事件 / 动画到期"再醒。一期可先做 vsync，二期做休眠唤醒。

**兜底方案**（eglSwapInterval 在个别驱动无效时）：
- `AChoreographer`（API 24+）postFrameCallback 驱动帧循环——需要回调→引擎
  线程的投递管道，工作量大一档；仅在 swap interval 实测失效的设备启用。

**测量先行**：改造前先在目标真机记录 `dumpsys gfxinfo`（帧时间分布、
judder 比例）与电流，作为前后对比基线。

### 实施步骤

1. 基线测量（gfxinfo + 功耗，目标机型 2-3 台）。
2. EGL 初始化处加 `eglSwapInterval(1)`，删 sleep → 真机观测帧率锁定
   刷新率、无撕裂。
3. 输入延迟主观评测（对话推进手感）+ 自动化：`artc drive` 注入 tap 的
   响应延迟分布对比。
4. （二期）静止场景 Choreographer/条件变量休眠，与 T2-2 门控合流。

### 验收标准

> **落地状态（本次）**：实现 `eglSwapInterval(1)`（每次 EGL 上下文重建都重设）+
> 帧循环改为「绘制帧由 swap 背压、空闲帧/未启用 vsync 时才 sleep」；`ARTC_SWAP_INTERVAL=0`
> 为设备级回退。帧率/judder/功耗量化需 [真机]。

- [ ] [真机] 帧率与屏幕刷新率一致（gfxinfo 确认）。
- [x] 固定 16ms sleep 仅保留给空闲/回退路径（动画帧不再 sleep）。
- [ ] judder（帧间隔标准差）较基线显著下降。
- [ ] 输入→响应延迟分布不劣化（tap 到 tag 派发时延对比）。
- [ ] 静止场景功耗下降（二期休眠合流后进一步下降）。
- [ ] 真机五个重点场景（对话/动画/过渡/视频/E-mote）无卡顿回归。

### 风险与缓解

- eglSwap 阻塞时长不可控（低端机个别驱动 swap 间隔异常）→ 真机矩阵实测；
  异常设备记录并保留 `swap_interval=0 + sleep` 的设备级回退开关（ini 或
  构建配置，**不做**通用运行时开关——按设备白名单收敛）。
- 若 GL context 与 ANativeActivity 窗口三态交接（`native_activity.cpp:465-476`）
  重建 EGL，swap interval 需在重建处重设——加断言点。

---

## 三梯队总依赖图

```
T1-3(解耦) ──> T2-1(EngineContext) ──┐
T1-4(pread) ──> T2-3(音频线程) ────────┤
T1-2(glyph) ──> T2-2(零分配+门控) ─────┼──> T3-3(vsync，门控联动二期)
T1-1(tag拆分) ──> T3-2(测试补齐·联动项) │
T3-1(条件编译收敛) ──> T2-2(compositor默认可测) 
T3-2(AsbRunner/存档测试) ──> T2-1 的安全网
```

## 不动摇的既有设计（重构红线）

以下设计是同类项目少有的高质量实现，任何梯队改造**不得破坏**，验收时逐一核对：

- `expression.cpp`：递归下降 + 短路求值 + 深度限 128 的表达式求值；
- `pluto_codec.cpp:129-131`：longjmp 前的 C++ 对象析构顺序注释与异常安全；
- `save_storage.cpp`：O_EXCL + fsync + rename 的原子写管线；
- E-mote proxy：userdata + `__gc` + placement new + live registry 防悬挂
  （`lua_engine.cpp:494-509`）；
- GL 丢失三态交接 + `ReleaseGl` 全量释放（`native_activity.cpp:465-476`、
  `compositor.cpp:1438-1458`）；
- 输入独立 looper 线程防 ANR（`native_activity.cpp:295-319`）；
- AsbRunner `current_file_` 缓存避免每帧重解析（`asb_parser.cpp:123`）。
