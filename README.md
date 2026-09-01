# artemis-compat — clean-room Artemis 兼容引擎

自研、可自行编译的 Artemis Engine 兼容运行时：产出安卓 `libartemis.so`（导出与官方一致的 JNI 六接口，
可被官方 `ArtemisActivity.jar` 壳直接加载）。

> **clean-room**：实现规格来自**行为级逆向**（格式/标签/接口/时序，"引擎怎么响应输入"而非"引擎怎么写的"）
> 与官方公开规格，不转写任何反编译伪代码；不含 emote（M2 闭源中间件）等闭源组件；不分发任何游戏资产。

目前实测游戏：**闪亮女友**（安卓，真机可玩闭环）、Real Hentai Situation! DT（PC 脚本解析/解码）。

处于初期，希望各位能够参与建设，帮助完善。整个项目代码基于DeepSeek-V4f与Glm-5.3f产出。原谅我的囊中羞涩。

---

## 当前能力（M0 → M3）

| 模块 | 能力 |
|---|---|
| **包层** | pf8 读取器 + 自动派生密钥（`SHA1(file[7:7+index_size])`，20B 周期 XOR）+ 多包补丁链（`.000/.001/…` 覆盖） |
| **配置** | system.ini 全平台节（WINDOWS/ANDROID/IOS/WASM/SWITCH/PS4） |
| **脚本** | Lua 5.1.5 嵌入 + `e` 桥表；`system/init.lua` 启动链、adv 框架；.iet 解释器（`[lua]`/标签/文本）；原生脚本 runner（call/return、eqtag、estag） |
| **文本** | 消息层 `chgmsg`/`print`/`rt` → stb_truetype **多行栅格化**；按层 font rect + wrap 折行（解决长句溢出/偏位） |
| **图层** | lyc/lyprop/lydel/lyevent/lytween；z 按官方 `spec/layer.md` **图层 ID 排序**；父层坐标继承（绝对+继承混合模型） |
| **渲染** | GLES2 层合成器（stage 坐标 + SIDECUT letterbox 视口）；draw[] 层诊断日志 |
| **音频** | **stb_vorbis + OpenSL ES 播放器**：BGM(循环)/SE/语音（`splay`/`seplay`/`voplay`）；生命周期静音 |
| **输入** | 归一化键事件（`e:isDown` 系轮询）、触摸/tap、slider 拖拽（dragarea） |
| **JNI** | 六接口导出 + ANativeActivity + DebugBridge.nativeInstall 引导 |
| **日志** | `OutputLog(level,msg)` → `Artemis` tag / stderr；tag[trace] 便于移植排障 |

## 构建

### 宿主测试构建（macOS / Linux）

```bash
cmake -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j8
```

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

- E-mote（M2 闭源中间件）演出；不追求逐像素渲染一致
- **tween 动画引擎**（`lytween` 当前"到达即终值"、`trans` 过渡无动画 → 无 CG 点击过渡效果）
- fade（`sepan`/`sfade`/`sxfade`）、语音多段叠加、存档 BOWX 字节兼容、选择肢
- `.050+.051…` 巨大数据卷（`pf6` 旧代格式）读取器
- 密钥派生算法为独立课题；个别标题自定义魔数/密钥不在自动派生范围

## 许可

- 本仓库源代码（`src/`、`CMakeLists.txt`、`docs` 等）：**GNU GPL v3**（见 `LICENSE`）
- `third_party/lua-5.1.5`：MIT
- `third_party/stb_vorbis/stb_vorbis.c`：public domain / MIT（见文件头）

GPL 不涵盖第三方组件——各组件按其自身许可分发（见 `THIRD_PARTY_NOTICES.md`）。
不含任何游戏资产、官方二进制或按标题的密钥。

## 目录结构

```text
src/
  pack/     pf8 读取器 + 多包链
  config/   system.ini 解析
  script/   Lua 引擎 + iet/asb 解释器 + pluto 序列化
  render/   GLES2 合成器 + stb_truetype 文本
  audio/    OpenSL ES 播放器（host 为桩）
  log/      OutputLog 管线
  jni/      Android JNI 六接口 + ANativeActivity
  cli/      artc 宿主工具
third_party/  lua-5.1.5 (MIT) · stb_vorbis (public domain)
```