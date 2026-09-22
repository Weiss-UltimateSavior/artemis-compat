# AGENT.md — artemis-compat 仓库工作规范

> 面向在此仓库工作的 AI 代理与人类贡献者。规范源于第一梯队优化
> （docs/optimization-tier1.md，已全部落地）与项目既有传统。
> 违反「红线」的改动不予合入。

## 1. 项目定位（不可动摇）

- **clean-room**：实现规格来自行为级逆向（格式/标签/接口/时序）与公开规格，
  不转写反编译伪代码；不含闭源组件；不分发任何游戏资产、官方二进制、按标题密钥。
- **行为保持高于一切**：重构的纪律是「纯搬运」——不改判定顺序、不合并条件、
  不做顺手化简。行为等价性由回归测试与真机冒烟背书。
- 实测游戏（闪亮女友 / 常轨脱离系列 / 甜蜜女友3）是最终验收基准。

## 2. 模块分层与依赖方向

```
jni / host（宿主壳）
 └── core（EngineContext：唯一装配点与所有者）   [仅宿主可 include]
      └── script（LuaEngine + tag 分发）    [实现层 .cpp 可单向使用 render]
           ├── render（Compositor/GL/文本）
           ├── audio / pack / config
           └── 共享底座：util / input / save
```

**依赖规则（grep 可断言，写进提交前自查）：**

1. `src/render/` 不得 `#include "script/…"`（模块级循环，已清零，不得回归）。
2. `src/script/*.h` 不得 `#include "render/…"`（头文件层不反向依赖渲染；
   `lua_engine.cpp` 等实现文件可单向 include render——那是合法分层方向）。
3. `core/` 只被宿主（`host/`、`jni/`、`cli/` 或外部嵌入适配器）引用；任何引擎模块（script/render/audio/pack）
   不得 `#include "core/…"`。
4. 跨模块共享的类型一律下沉到最低公共层：
   - `util/text.h`：`TextRuby` + UTF8/Base64/URL/SplitEscaped 工具；
   - `util/snapshot_image.h`：纯像素 + PNG 编码（无 GL）；
   - `util/save_storage.h`：原子写 + 变量银行编解码；
   - `input/`：`InputState`、`AutoReadTimer`（jni 喂、render 消费）；
   - `save/`：BOWS/BOWG 反序列化、存档元数据修复。
5. 新增模块目录必须同步 `CMakeLists.txt` 的 GLOB 列表（`src/save/`、`src/input/`、
   `src/core/` 即此例），否则静默漏编。

## 3. e:tag 派发规范（T1-1 落地模式）

`l_tag` 是唯一入口，位于 `src/script/lua_tag_dispatch.cpp`，双阶段：

1. **raw 表**（`RawTagTable()`）：直读 Lua 表的早退 handler（var/prohibit/
   wordparts/indent/lyrename/allsoundstop/debug）。`font` 的 raw handler
   （`TagFontHeightRaw`）刻意返回 false 继续走公共路径——高度记账 + 完整处理
   是**双重处理**，勿改。
2. **精确名表**（`TagTable()`）：公共路径（trace + 属性表 `m`，值经
   `ResolveValue` 解析）后一次哈希查找；表未命中再走 `TagGenericSetOn`
   前缀注册（seton\*/delon\*）。

**新增/修改 tag 的规则：**

- handler 是 `LuaEngine` 私有成员函数，签名统一
  `bool TagXxx(const std::string &tag, TagAttrs &m)`；返回 false 语义 =
  原 if 链的 fall-through（门控未满足、无操作），**不是错误**。
- handler 声明按域分组写入 `lua_engine.h` 私有区，实现放对应域文件：
  `lua_tags_var / input / nav / audio / layer / text.cpp`。
- 音频 13 个标签共用 `TagAudio`（共享 channel/time/gain 参数计算），
  同类共享参数的标签组照此办理，不逐个拆。
- **门控语义必须保持**：原链中位于 compositor 块内的 tag（print/chgmsg/
  calllua/setonpush 等）handler 必须自带 `if (!compositor_) return false;`。
- click-wait 集合只有 `@` / `p` / `clickwait`；`rp` **不得**加入（官方定义
  为翻页标签，误当等待会每段死锁）。
- 未注册 tag 走静默 no-op + `tag[trace]` 首见日志（M0 发现机制），不要
  添加 UNIMPLEMENTED 报错。

## 4. 渲染与 GL 资源

- **条件编译**：`ARTC_HAS_GLES`（Android/OHOS/mac 宿主开，CLI/tests 默认关）。
  无 GL 构建走 compositor.cpp 尾部桩实现；**桩与 GL 实现的签名必须同步修改**。
- **stb 实现归属唯一**：`STB_TRUETYPE_IMPLEMENTATION` 在 `glyph_atlas.cpp`
  （无条件编译，缓存单元 GL-free、可无上下文回归测试）；`STB_IMAGE_IMPLEMENTATION`
  在 compositor.cpp GL 段。新增 stb 使用遵守同一归属，不得重复实例化。
- **glyph cache**（`render/glyph_atlas.{h,cpp}`）：
  - key = (glyph index, stbtt scale, outline, fill color, outline color)；
    一期颜色计入 key，命中率数据决定是否做 coverage/颜色解耦二期。
  - 页 1024×1024，水位 6 页：超限整体回收重建（`WasReset()` 通知调用方
    丢弃旧 Entry 拷贝）。SetText 的双尝试循环处理回收重试，单文本超预算
    才返回 false（等价旧退化失败）。
  - **失效点**：`LoadFont`（换字体→glyph index 全变）与 `ReleaseGl`
    （GL 丢失）都会 `DropGlyphGl()`；新增缓存消费路径必须接入这两个钩子。
  - GL 侧：页纹理按 generation 增量上传（`GlyphPageTexture`）——重复文本
    零栅格化、零合成、零上传。
- **GL 丢失三态交接**：窗口丢失 → `ReleaseGl` 全量释放（含 glyph 页）→
  重 boot 经 tag 重建。任何新持久的 GL 资源必须进 `ReleaseGl`。
- 文本层不再持有 per-SetText 纹理（`l.texture == 0`，glyph 引用共享图集页）；
  判定「层是否有内容」用 `!l.texture && l->glyphs.empty()`，不能只看 texture。

## 5. pf8 包读取

- 查找走 `Open()` 建的归一化索引（unordered_map，O(1)）；
  **「首条记录优先」是官方历史行为**，建表用 `emplace` 不覆盖。
- Shift_JIS 回退保留：UTF-8 查询未命中 → CP932 转换后再查同一索引。
- 读取用常驻 fd + `pread`（Windows 走旧 fopen 回退）；fd 生命周期 =
  reader 生命周期，**Pf8Reader 不可拷贝**（持有 fd，拷贝会双重 close）。
- XOR 相位按文件数据起点重启（`DecryptRange` 带偏移相位），流式读
  `ReadRange` 依赖此语义。

## 6. 存档与原子写

- `save_storage.cpp`：O_EXCL + fsync + rename 原子写管线——**不得以任何
  理由绕过**（直接 ofstream 写存档 = 数据损坏事故）。
- BOWS/BOWG 是**导入器**语义：不声称兼容存档可导出回官方引擎、不丢弃/
  覆写未知原生字段、整结构校验通过才替换输出。
- ARCV 检查点是自有格式（onLoad 重构场景），非 BOWS 等价物。

## 7. 错误处理与日志

- Lua 调用统一走 `PCallTraceback`（带 debug.traceback）；新增 pcall 不得
  裸调（`RunPackScript`/`ValidateBankGraphs` 的裸 pcall 是待修历史遗留）。
- 日志经 `OutputLog(level,msg)` → `Artemis` tag / stderr / 宿主 sink。
  高频路径（每帧/每 glyph）禁止拼接字符串——参考 draw[] 的 300 帧节流与
  `seen_tags_` 首见去重。
- tag 流派发的 `tag[trace]` 首见日志是移植排障核心手段，保留。

## 8. 测试规范

- 夹具**全部合成**（tests/data：rectangle.ttf 矩形字形字体 + 生成音调；脚本类
  夹具用内置 pf8 写入器现造容器），不含商业资源——新测试沿用此传统。
- 关键路径专属套件：`asb_runner_regressions`（runner 状态机：嵌套/跨文件 call、
  `[return]` 后的 pc_pending_ 恢复点、事件帧、DiscardFlow）、`save_regressions`
  （原子写不变量 + 故障注入：崩溃留痕/RLIMIT_FSIZE/只读目录）、`iet_regressions`
  （.iet 行模型 + IetRunner 与 AsbRunner 两条路径一致性）。改这些子系统必须
  先扩对应套件。
- 命名 `*_regressions.cpp`，一个子系统一个可执行 + 一个 ctest 用例；
  目标注册进 `tests/CMakeLists.txt`。
- **行为保持类重构**（tag 拆分、缓存引入）必须扩展对应回归：
  glyph_atlas_regressions 是范式（命中/未命中计数断言 + 页代数 + 不重叠）。
- GL 路径默认零编译覆盖（tests 走桩）；涉及 GL 的改动在 mac 宿主
  （`./build-mac/artemis-mac <游戏目录>`）冒烟，必要时开 `ARTC_TEST_GLES`
  （需 ANGLE）。
- 已知缺口（Tier 3 补齐中）：AsbRunner / save_storage 故障注入 / iet /
  JNI 生命周期暂无专属回归——改到这些区域时至少跑真机或 mac 宿主冒烟。

## 9. 提交前验证清单（全部通过才可交付）

```bash
cmake -B build-test -DCMAKE_BUILD_TYPE=Release -DARTC_BUILD_TESTS=ON
cmake --build build-test -j8 && ctest --test-dir build-test --output-on-failure
cmake --build build-mac -j8 --target artemis-mac     # GL 路径编译
# Android（NDK 路径按本机调整）:
cmake --build build-android -j8                      # libartemis.so
grep -rn '#include "script/' src/render/             # 必须为空
grep -rn '#include "render/' src/script/*.h          # 必须为空
rg 'make_unique<PackManager>|make_unique<LuaEngine>' src  # 只应命中 core/engine_context.cpp
./build-host/artc drive <游戏目录>/root.pfs --frames 400  # 帧循环冒烟（含 click-wait）
```

- 涉及剧情推进/文本/音频/图层的改动：真机（或 mac 宿主 + 实测游戏目录）
  跑「标题→对话→点击推进→CG→BGM/语音」冒烟。
- 涉及 JNI/生命周期的改动：补验窗口旋转、GL 丢失恢复、退后台/回前台。
- 涉及渲染/门控的改动：mac 宿主冒烟并按 §14 的五个场景核对有无漏重绘。
- 涉及装配/音频线程的改动：核对 §12/§13 的自查项（单一装配、回调零解码）。

## 10. 既有高质量设计（重构红线，逐项核对）

- `expression.cpp`：递归下降 + 短路求值 + 深度限 128。
- `pluto_codec.cpp:129-131`：longjmp 前的 C++ 对象析构顺序约定。
- `save_storage.cpp`：O_EXCL + fsync + rename 原子写。
- E-mote proxy：userdata + `__gc` + placement new + live registry 防悬挂。
- GL 丢失三态交接 + `ReleaseGl` 全量释放（`native_activity.cpp:465-476`）。
- 输入独立 looper 线程防 ANR（`native_activity.cpp:295-319`）。
- AsbRunner `current_file_` 缓存避免每帧重解析（`asb_parser.cpp:123`）。
- `InputState` 的 Decide 语义：指针激活在**释放**沿、键盘在**按下**沿。

## 11. 优化路线图状态

- **第一梯队（已完成）**：T1-3 模块解耦 / T1-1 tag 分发表化 /
  T1-4 pf8 索引化 / T1-2 glyph cache。方案与验收见 docs/optimization-tier1.md。
- **第二梯队（已完成）**：T2-1 EngineContext 装配收敛 / T2-2 Draw 零分配 +
  重绘门控 / T2-3 音频解码移出回调线程。规范见 §12–§14，方案与验收见
  docs/optimization-tier2.md。
- **第三梯队（部分落地）**：T3-2 关键路径测试补齐（19/19，新增 asb_runner/
  save/iet 三套件）、T3-3 vsync（eglSwapInterval + 帧节奏，真机量化待做）、
  T3-1 仅完成 `cmake --install`；**compositor 物理拆分暂缓**——GL 与共享函数
  逐函数交错（`LoadShader` 共享、`CreateTexture`/`SetText` 双实现），需先分类
  再移动。见 docs/optimization-tier3.md 各节「落地状态」。
- 已知计划偏差（有意为之）：
  - T1-3 验收从「src/script 全目录零 render include」放宽为「头文件零」
    （.cpp 实现层单向使用渲染是合法分层，接口抽象留给 T2-1）；
  - T1-1 的「每 tag 一条合成用例」并入 T3-2 统一补齐；
  - glyph cache 一期颜色计入 key（解耦二期看命中率）；
  - T2-3 一期保留压缩数据整段常驻，分块流式读并入 Tier 3；
  - T2-1 未把 VideoPlayer/EmotePlayer 迁到 Compositor：它们由 LuaEngine 持有，
    而 LuaEngine 归 EngineContext，单一所有权已成立；GL 丢失时整上下文重建，
    `ReleaseGl` 完整性不受影响。

---

## 12. 引擎装配规范（T2-1 落地模式）

- **`core/EngineContext` 是唯一装配点与唯一所有者**：`PackManager` / `Ini` /
  `Audio` / `AudioChannels` / `Compositor` / `LuaEngine` / `AsbRunner` 只允许在
  `src/core/engine_context.cpp` 创建。提交前自查：
  `rg 'make_unique<PackManager>|make_unique<LuaEngine>' src` 只应命中该文件。
- 宿主（jni/host/cli）一律持有 `std::unique_ptr<EngineContext>`，通过访问器
  （`packs()/ini()/audio()/sounds()/compositor()/lua()/runner()`）取用；**不得**
  再出现引擎裸全局（历史上的 `g_packs/g_lua/g_vm` 已清零）。
- 三段式生命周期：
  1. `Open(data_dir, os_id, key)`（无 GL）：解析包链（目录或 .pfs 直路径，
     root.pfs 优先）、解析 system.ini、建 Audio/AudioChannels、定 stage 尺寸
     （ANDROID 节优先，WINDOWS 兜底）。幂等，进程内只开一次。
  2. `Start(with_compositor=true)`（GL 上下文当前）：建 Compositor 并 `Init`，
     建 LuaEngine 并 `Init`。GL 丢失/[reset] 后再次调用即可重建会话（内部先
     `ReleaseGl`）。DebugBridge 这类无 GL 路径用 `Start(false)`。
  3. `BootFramework(drain_boot_queue=false)`：建 AsbRunner、跑
     `system/first.iet`、接 jump/call/stop。`drain_boot_queue=true` 是 CLI
     专用（boot 跳转立即执行，处理器此时尚未安装——保持历史顺序）。
- 会话级重启（GL 丢失 / `[reset]` tag）统一走 `ResetSession()`，不要手动
  `lua.reset()`；`Shutdown()` 才释放 compositor/audio/packs。
- 跨宿主查询（JNI 音频暂停钩子等）用 `EngineContext::Current()/SetCurrent()`
  注册表；它是引用登记，不承担所有权，`Shutdown()` 会自动摘除。
- **LuaEngine 不再拥有 Audio/AudioChannels**：`Init` 收非拥有指针，析构只清
  `videos_/emotes_`；`[alldelete]` 用 `Audio::StopAll()` + `AudioChannels::Reset()`
  而不是 `delete/new`。音频指针为空时音频 tag 自然 no-op（门控已存在）。
- 新增宿主必须复用 EngineContext；新增引擎子系统时把所有权挂进 EngineContext，
  不要新增裸全局或让 script 模块当工厂。

## 13. 音频解码规范（T2-3 落地模式）

- **OpenSL 回调线程禁止解码**：回调函数体只允许「ring 拷贝 + 静音填充 +
  `Enqueue`」；任何 `PcmStream::ReadStereo` 调用都属于 feed 线程。
- `audio/pcm_ring.h` 是唯一缓冲：SPSC、容量向上取 2 的幂、空出一槽区分满/空。
  **单生产者**（feed 线程）——需要预填时只在 voice 发布（进 `voices`）之前做，
  且不得注册回调（这就是 `PlayStream` 里 prime 的顺序含义）。
- Voice 用 `shared_ptr` 持有：feed 线程的快照与 voices 表各持一份引用，
  `Stop` 只需从表里 `erase`；`~Voice` 的 OpenSL `Destroy` 负责等待回调退出。
- 流尾语义：ring 空且 `source->Ended()` → 停止入队（让队列计数归零，
  `IsPlaying` 变 false）；未结束但 ring 空 → 填静音并累加 underrun 计数
  （feed 线程每 5s 汇总进 OutputLog，热路径不拼字符串）。
- 所有 `Enqueue`/`Queue` 返回值必须检查（历史缺陷：第二次 `Queue()` 被忽略）。
- 增删声道/接续（`_a`→`_b`）、`sfade/sxfade/sepan`、`[wait se=]`、
  `setonsoundfinish` 的语义不改：只换数据供给方式，混音仍在消费侧。
- `pcm_ring` 属 GL-free/平台无关单元，新行为（水位/回绕/SPSC 顺序）扩展
  `audio_stream_regressions`，不要依赖真机才能跑。

## 14. 渲染热路径与重绘门控（T2-2 落地模式）

- **Draw 路径零每帧堆分配**：`draw_sorted_` / `draw_textures_` /
  `draw_glyph_runs_` / `draw_mesh_vertices_` 是 compositor 成员 scratch；
  `std::vector::clear()` 保容量，禁止在 Draw 里构造临时容器/字符串。
- 纹理查表（effect 绑定）用 `std::vector<std::pair<id,tex>>` + 线性扫描，
  只在 `revision_` 变化时重建（`draw_textures_rev_` 记忆）——稳态零分配。
  `LayerShaders::End` 的 textures 参数类型与之同步；改签名必须同时改桩实现。
- 诊断输出（`draw[]`）保持 300 帧节流；非诊断路径不得拼字符串。
- **`revision_` 是重绘门控的唯一依据**，所有改变输出的入口必须 bump：
  `LoadImage / SetProps / SetText / SetLayerMesh / SetPixels / DeleteLayer /
  RenameLayer / ReleaseGl / AddTween / BeginTransition / Update(有变化时)`。
  新增 mutation 入口时先核对这一点。
- 宿主帧循环门控（保守条件，全静止才跳过）：
  `rev != drawn_rev || PendingAnimationMs(now)>0 || TransitionActive() ||
  PendingTextMs(now)>0` 才 `Clear+Draw+Present`；`Update(now)` 与 `EndFrame()`
  必须每帧执行。窗口重建 / GL 丢失 / `[reset]` 后必须 `drawn_rev = ~0ull`。
- 视频帧不需要单独信号：`VideoPlayer` 每帧走 `SetPixels → SetProps`，天然 bump。
  若未来新增不 bump 的帧源，必须补 bump 而不是加宽门控。
- 门控是行为开关：上线按「完全静止」灰度，五个重点场景（对话推进、长 tween、
  trans 过渡、视频、E-mote）在 mac 宿主 + 真机各冒烟一次。

## 15. 测试与帧节奏规范（T3 落地模式）

- **关键子系统先有网再动结构**：AsbRunner / save_storage / iet / (待补 native_save)
  的改动必须落到 `tests/*_regressions.cpp`；合成脚本夹具用内置 pf8 写入器现造
  容器（`BuildPack` 模式：file_count + records 的 SHA1 派生密钥），不得引入游戏资产。
- **执行轨迹即断言**：runner 类测试以「唯一 tag 的 `tag[trace]` 首见顺序」为轨迹
  （未注册 tag 的 no-op 语义是 M0 发现机制，保留），辅以 `asb: load/return to`
  日志断言；不要为测试新增引擎日志。
- **存档安全网**：`save_storage` 的原子性（O_EXCL temp + fsync + rename）通过
  故障注入证明——崩溃留痕不提交、RLIMIT_FSIZE 半途写失败、只读目录明确失败，
  且三种情况旧档必须完好。
- **帧节奏**：Android 用 `eglSwapInterval(1)` 让 swap 提供背压；只有「未绘制帧」
  或 `Renderer::SwapPaced()==false`（`ARTC_SWAP_INTERVAL=0` 设备回退）才 sleep。
  上下文重建（窗口丢失/重 init）处必须重设 interval——`Renderer::Init` 是唯一
  设置点，不要在别处复制这段逻辑。
- **门控与 vsync 的耦合**：跳过绘制的帧没有 swap 背压，必须保留一个空闲等待
  （当前 16ms sleep），否则会忙等烧 CPU；二期若换 Choreographer/条件变量，
  替换的是这个空闲等待，不是 swap 路径。
- **安装**：`cmake --install` 必须保持可用（core/so/CLI/mac + `src/**/*.h`），
  下游（壳工程/打包脚本）不再手拷 `build-android/libartemis.so`。

## 16. 嵌入宿主与官方 Android 产物

- 同一份内核同时服务官方 Android 壳和外部嵌入宿主。原 `libartemis.so`、
  六个 JNI 方法的完整签名、`JNI_OnLoad`、`ANativeActivity_onCreate` 以及已有
  额外宿主入口保持；不能因下游改用静态库而删除原产品。
- `AGENTS.md` 是规范入口，本文件是完整规则。修改平台/宿主边界时同步更新。
- 上游负责引擎语义与可复用平台后端；外部宿主负责窗口、Flutter 纹理、授权和 UI。
  不引入对 NextScene、Flutter 或某个产品的依赖，不在外部重建第二套引擎对象图。
- CMake 子工程默认只构建库，独立构建保持原默认宿主。图形提供者与音频后端
  显式选择，库依赖必须传递到最终链接；生产缺依赖时报错，不能暗中降为静音/无 GL。
- 桌面 GL 适配只对该渲染后端启用，不能将 `__APPLE__` 等同于 macOS OpenGL。
  iOS/ANGLE 使用 GLES 头和 GLSL ES，不能照搬 Cocoa macOS 宿主。
- 新宿主配置采用可选参数，现有调用保持默认行为。独立存档目录、外部帧驱动、
  异步 UI 和场景恢复分别验证，不在构建重构里改变官方 Android 宿主策略。
- 验证至少包括：宿主回归、真实 GL、嵌入消费者最终链接、Android 官方 `.so`
  构建及必要动态导出；涉及 OHOS 后端时增加 SDK20 编译及可用真机测试。
  `nm` 的符号检查不等于 jar 行为兼容，原始 jar/Android 设备验证单独记录。
- 缺 SDK/设备时交付可复现命令和明确未验证项，不得把桩测试或另一平台成功当作通过。
- 通用修复在上游独立小提交并附合成测试；下游固定已发布 commit，不能依赖
  只存在开发机的 submodule 指针。引擎提交发布后再更新下游指针。
