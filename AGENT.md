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
 └── script（LuaEngine + tag 分发）    [实现层 .cpp 可单向使用 render]
      ├── render（Compositor/GL/文本）
      ├── audio / pack / config
      └── 共享底座：util / input / save
```

**依赖规则（grep 可断言，写进提交前自查）：**

1. `src/render/` 不得 `#include "script/…"`（模块级循环，已清零，不得回归）。
2. `src/script/*.h` 不得 `#include "render/…"`（头文件层不反向依赖渲染；
   `lua_engine.cpp` 等实现文件可单向 include render——那是合法分层方向）。
3. 跨模块共享的类型一律下沉到最低公共层：
   - `util/text.h`：`TextRuby` + UTF8/Base64/URL/SplitEscaped 工具；
   - `util/snapshot_image.h`：纯像素 + PNG 编码（无 GL）；
   - `util/save_storage.h`：原子写 + 变量银行编解码；
   - `input/`：`InputState`、`AutoReadTimer`（jni 喂、render 消费）；
   - `save/`：BOWS/BOWG 反序列化、存档元数据修复。
4. 新增模块目录必须同步 `CMakeLists.txt` 的 GLOB 列表（`src/save/`、`src/input/`
   即此例），否则静默漏编。

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

- 夹具**全部合成**（tests/data：rectangle.ttf 矩形字形字体 + 生成音调），
  不含商业资源——新测试沿用此传统。
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
```

- 涉及剧情推进/文本/音频/图层的改动：真机（或 mac 宿主 + 实测游戏目录）
  跑「标题→对话→点击推进→CG→BGM/语音」冒烟。
- 涉及 JNI/生命周期的改动：补验窗口旋转、GL 丢失恢复、退后台/回前台。

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
- **第二梯队（待做）**：EngineContext 装配收敛（硬前置=T1-3，已满足）、
  Draw() 零分配 + 重绘门控、音频解码线程化。见 docs/optimization-tier2.md。
- **第三梯队（待做）**：渲染条件编译收敛、关键路径测试补齐、vsync。
  见 docs/optimization-tier3.md。
- 已知计划偏差（有意为之）：
  - T1-3 验收从「src/script 全目录零 render include」放宽为「头文件零」
    （.cpp 实现层单向使用渲染是合法分层，接口抽象留给 T2-1）；
  - T1-1 的「每 tag 一条合成用例」并入 T3-2 统一补齐；
  - glyph cache 一期颜色计入 key（解耦二期看命中率）。
