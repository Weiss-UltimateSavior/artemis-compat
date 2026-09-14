# artemis-compat — clean-room Artemis 兼容引擎

自研、可自行编译的 Artemis Engine 兼容运行时：产出安卓 `libartemis.so`（导出与官方一致的 JNI 六接口，
可被官方 `ArtemisActivity.jar` 壳直接加载）。

> **clean-room**：实现规格来自**行为级逆向**（格式/标签/接口/时序，"引擎怎么响应输入"而非"引擎怎么写的"）
> 与官方公开规格，不转写任何反编译伪代码；不含 emote（M2 闭源中间件）等闭源组件；不分发任何游戏资产。

目前实测游戏：**《闪亮女友》**  **《常轨脱离Creative》**  **《常轨脱离Creative 凸》** **《甜蜜女友3》**

处于初期，希望各位能够参与建设，帮助完善。整个项目代码基于DeepSeek-V4f与Glm-5.3f产出。原谅我的囊中羞涩。

---

## 当前能力

| 模块 | 能力 |
|---|---|
| **包层** | pf8 读取器 + 自动派生密钥（`SHA1(file[7:7+index_size])`，20B 周期 XOR）+ 多包补丁链（`.000/.001/…` 覆盖）；链条未命中时回退读取包旁散装文件（`movie/*.mp4` 等，拒绝绝对路径与 `..`） |
| **配置** | system.ini 全平台节（WINDOWS/ANDROID/IOS/WASM/SWITCH/PS4） |
| **脚本** | Lua 5.1.5 嵌入 + `e` 桥表；`system/init.lua` 启动链、adv 框架；.iet 解释器（`[lua]`/标签/文本）；可重入原生 .asb runner（`ExecuteLine` + 事件返回帧 + 脚本栈、跨文件 call/return）；`$` 表达式求值（32 位整数/比较/逻辑短路/位运算/字符串/变量引用）；按键 override 与逐帧边沿派发；事件过滤器；auto-read |
| **文本** | 消息层 `chgmsg`/`print`/`rt` → stb_truetype **多行栅格化**；按层 font rect + wrap 折行；对齐/描边；ruby 注音与横排基本禁则/悬挂；逐字入场与批量字形绘制；点击补全再推进；按层多页 |
| **图层** | lyc/lyprop/lydel/lyevent；z 按官方 `spec/layer.md` **图层 ID 排序**；父子变换（位移 + 锚点缩放/旋转/翻转，逆矩阵命中测试）；draggable/dragarea 拖拽；真实 `lytween` 补间与 `trans` 过渡（rule 阈值擦除 + vague 羽化）；`lyshader` 移动端 GLSL 与中间层蒙版/裁剪 |
| **渲染** | GLES2 层合成器（stage 坐标 + SIDECUT letterbox 视口）；保留场景 FBO 供过渡与截图；`takess`/`savess` 快照 PNG（原子替换）；draw[] 层诊断日志 |
| **音频** | **stb_vorbis 分块流 + OpenSL ES 播放器**：BGM(循环)/SE/语音（`splay`/`seplay`/`voplay`）；`_a`→`_b` 曲目接续；逻辑声道 + 定时增益/声像与交叉淡化（`sfade`/`sxfade`/`sepan`…）；`[wait se=]`、`setonsoundfinish`；生命周期静音 |
| **视频** | **FFmpeg（可选）**解复用/解码：全屏或指定图层、循环、等待与取消键 |
| **存档** | Pluto 值图原生编解码；BOWS/1003 变量/图层日志与 BOWG 全局银行导入（经游戏 `onLoad` 恢复）；`save` 写独立 ARCV 兼容检查点（原子写入 + 校验）；场景 PNG 缩略图 |
| **E-mote** | PSB/MDF 解码、RL/raw/CI8 贴图、有限场景求值与 GLES 绘制、`EmotePlayer` 播放器 + `createEmoteLayer`/`getEmoteLayer`（复杂模型/网格/物理/加密 PSB 明确不支持） |
| **输入** | 归一化键事件（`isDown`/`isPush`/`isDecide`/边沿）、触摸/tap、slider 拖拽、事件过滤器 |
| **JNI** | 六接口导出 + ANativeActivity + DebugBridge.nativeInstall 引导 |
| **日志** | `OutputLog(level,msg)` → `Artemis` tag / stderr；宿主 `SetLogSink` 次级输出钩子；tag[trace] 便于移植排障 |

## 构建

依赖：CMake ≥ 3.16、C++17 编译器、**zlib**（存档/PSB/截图）。**FFmpeg**（libavformat/
libavcodec/libavutil/libswscale/libswresample）为可选项——未找到时视频解码编译为桩。

### 宿主测试构建（macOS / Linux）

```bash
cmake -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j8
```

### 宿主回归测试

合成夹具（自绘矩形字体 + 生成音调），不含任何商业游戏资源：

```bash
cmake -B build-test -DCMAKE_BUILD_TYPE=Release -DARTC_BUILD_TESTS=ON
cmake --build build-test -j8
ctest --test-dir build-test --output-on-failure
```

### macOS 原生宿主（`artemis-mac`）

Cocoa 窗口 + legacy OpenGL 2.1 上下文，用真实 GL 渲染运行游戏。GLES2 入口与
GLSL ES 源码在 `src/render/gles2_headers.h` / `src/render/shader_compat.h` 中适配到
桌面 GL。该目标把核心以 `ARTC_HAS_GLES` 重新编译；`artemis_core`（CLI/回归）保持
无 GL 的数学实现，二者互不影响。

```bash
cmake -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target artemis-mac -j8
./build-mac/artemis-mac "/path/to/game"      # 目录或 root.pfs；默认 --os windows
```

鼠标左键=点击/拖拽，方向键/回车/退格/Esc 映射官方 key id。

### Android 构建（NDK）

```bash
NDK=$HOME/Library/Android/sdk/ndk/<version>
cmake -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DARTC_BUILD_JNI=ON -DARTC_BUILD_CLI=OFF
cmake --build build-android -j8
# 产物: build-android/libartemis.so
```

## CLI 用法

```bash
artc list    <pack> [--key <hex>]      # 列出包内文件（不传 key = 自动派生）
artc verify  <pack>                    # 抽验首文件解密魔数
artc ini     <pack> system.ini         # 打印包内 ini
artc extract <pack> <name> [-o out]    # 抽取单个文件
artc runlua  <pack> system/init.lua    # 试运行启动脚本
artc runiet  <pack> <script>           # 运行 iet 剧本
artc asb     <pack> <name>             # 解码编译脚本
artc drive   <pack> --frames N --tap x,y@f   # host 帧循环驱动器（注入触摸复刻移动端）
```

## 实测记录

| 包 | 结果 |
|---|---|
| 闪亮女友 `root.pfs`（安卓） | 真机**可玩闭环**：标题→故事→台词（坐标/折行）→点击推进→CG 切换→BGM/语音/SE 出声 |
| RealHentaiSituationDT `reaanidt.pfs`（PC，1047 文件） | 自动派生 ✓；ini 全平台节干净；148 个 .ast 多语言行格式完整解码 |
| カラーマリス `main.16.*.obb`（Android OBB，829 文件） | 自动派生 ✓；TTF/JPG 魔数全对 | 

## 接入官方模板壳

把 `build-android/libartemis.so` 放入官方模板 `app/src/main/jniLibs/arm64-v8a/`（替换官方库），
并在入口调用：

```java
com.ies_net.artemis.debug.DebugBridge.nativeInstall(
    getExternalFilesDir(null).getPath(),  // 数据目录（pf8 包所在）
    null);                                // 密钥 hex；null = 自动派生
```

> `nativeInstall` 是本项目自有的引导入口（官方 jar 之外的附加类，随本项目提供）。

## 非目标 / 已知缺口

- E-mote 完整 SDK 等价：网格/stencil 变形、复杂继承/深度/混合、物理与公开复杂模型仍不支持（加密头 PSB v2–v4、HOLD/插值、嵌套 motion 与 id 链变换继承已支持）
- 原版任意 VM 状态快照、BOWS/BOWG **双向**写出、内嵌截图导入（自身 ARCV 检查点已可重启回读）
- HLSL 仅覆盖已验证游戏用到的 Artemis 运行时子集（register sampler、`ps()`、`tex2D`、常用内建），非通用 D3D HLSL 编译器
- 混合字体/嵌套样式、竖排、复杂 ruby 分配与完整字形塑形
- `.050+.051…` 巨大数据卷（`pf6` 旧代格式）读取器（`pf2`/明文 PF6 布局已支持）
- 密钥派生算法为独立课题；个别标题自定义魔数/密钥不在自动派生范围

## 许可

- 本仓库源代码（`src/`、`CMakeLists.txt`、`docs` 等）：**GNU GPL v3**（见 `LICENSE`）
- `third_party/lua-5.1.5`：MIT
- `third_party/stb_vorbis/stb_vorbis.c`：public domain / MIT（见文件头）

部分引擎行为修补与新增模块（音频分块流/声道、图层变换与补间、表达式求值、文本
/ruby、存档、视频、GLSL、E-mote 基础）吸收自 [NextScene](https://github.com/reAAAq/KrKr2-Next)
对 artemis-compat 的 vendor 修补（其 `cpp/artemis/upstream/UPSTREAM.md`）。

GPL 不涵盖第三方组件——各组件按其自身许可分发（见 `THIRD_PARTY_NOTICES.md`）。
不含任何游戏资产、官方二进制或按标题的密钥。

## 目录结构

```text
src/
  pack/     pf8 读取器 + 多包链 + 散装文件回退 · PSB/MDF
  config/   system.ini 解析
  script/   Lua 引擎 + iet/asb 解释器 · 表达式 · Pluto 编解码
            · 原生存档(BOWS/BOWG/ARCV) · 输入状态 · auto-read
  render/   GLES2 合成器 + stb_truetype 文本 · 补间/过渡 · ly shader
            · 快照 PNG · 视频解码/播放（FFmpeg 可选）· E-mote(PSB/场景/播放器)
  audio/    声道混音 + Vorbis 分块流 + OpenSL ES 播放器（host 为桩）
  log/      OutputLog 管线 + 宿主 sink
  jni/      Android JNI 六接口 + ANativeActivity
  cli/      artc 宿主工具
  host/     macOS 原生宿主（Cocoa 窗口 + GL）
tests/      宿主回归（合成夹具）
third_party/  lua-5.1.5 (MIT) · stb_vorbis (public domain)
```