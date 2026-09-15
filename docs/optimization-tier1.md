# 优化方案 · 第一梯队：低风险高收益

> **定位**：不改任何引擎行为、可被现有回归测试完全验证的纯改造。四项任务相互独立，
> 可单独提交、单独回退。
>
> **基线**：动手前先在真机（闪亮女友）+ 全量 ctest 上建立绿色基线；每个任务完成后
> 复跑同一组验证。

## 总览

| 编号 | 任务 | 类型 | 主要收益 |
|---|---|---|---|
| T1-1 | `l_tag` 分发表化 + `LuaEngine` 按域拆文件 | 可维护性 | 消灭 947 行单函数，tag 派发 O(1) |
| T1-2 | 文本字形缓存（glyph cache） | 性能 | 消除文本场景全量重栅格化与纹理重建 |
| T1-3 | 解开 script↔render 循环依赖 | 架构 | 模块可独立编译，重编译级联缩短 |
| T1-4 | pf8 查找索引化 + 句柄复用 | 性能 | 大包查找 O(1)，消除重复 fopen |

---

## T1-1 `l_tag` 分发表化 + `LuaEngine` 按域拆文件

### 问题与证据

- `src/script/lua_engine.cpp:882-1828`：单函数 `LuaEngine::l_tag` 约 **947 行**，
  80+ 个 `tagname == "..."` 的线性 if-链，每次派发全串比较。
- 职责混杂在同一函数内：
  - 变量/随机/字符串工具（`:892-985`：var / random / substr / explode / base64）
  - 字体排版（`:1049-1085`：font / prohibit / wordparts / indent）
  - 导航控制（约 `:1291-1330`：jump / call / stop / reset / save / load）
  - 音频（约 `:1368-1396`：splay / seplay / allsoundstop）
  - 图层、视频、automode 状态机……全部内联。
- `lua_engine.h`（411 行、约 50 个成员）+ `lua_engine.cpp`（3236 行）整体是 God object：
  E-mote 代理绑定（前段约 100-530）、字符串工具（`:791-880`）、输入/拖拽状态机
  （约 `:2119-2540`）、等待状态机（约 `:2542-2740`）、存档快照（约 `:2755-2870`）。

### 方案

**第一步：分发表。** 在 `lua_engine.cpp` 顶部建立静态注册表：

```cpp
// 每个 handler 签名统一；inst 为引擎实例（automode 等标签依赖实例状态判断，
// 见 :1157 的 `if (inst && tagname == "automode")`，不能是纯自由函数）
using TagHandler = bool (LuaEngine::*)(lua_State *L,
                                       const std::map<std::string, std::string> &m);
static const std::unordered_map<std::string, TagHandler> kTagHandlers;
```

- 建表在静态初始化（unordered_map 的 ctor 开销一次性）。
- 派发顺序语义：现状 if-链存在**同前缀重叠标签**（如 `automode` 与 `exec`+`command=automode`
  的组合判断 `:1173`），提取时保持"一个 tag 一个 handler"，组合判断收进 handler 内部，
  不改判定顺序。
- 未注册 tag 走现有 UNIMPLEMENTED 日志路径。

**第二步：按域拆文件**（仅搬运代码体，函数签名不变）：

| 新文件 | 收编内容（对应现有行号区间） |
|---|---|
| `src/script/lua_tags_text.cpp` | font / glyph / ruby / print / prohibit / wordparts / indent |
| `src/script/lua_tags_audio.cpp` | splay / seplay / voplay / allsoundstop / sfade 系列 |
| `src/script/lua_tags_layer.cpp` | lyc / lyprop / lydel / lyevent / lytween / trans / lyshader / anime / video |
| `src/script/lua_tags_nav.cpp` | jump / call / stop / reset / save / load / wait |
| `src/script/lua_tags_var.cpp` | var / random / substr / explode / base64 / debug |
| `src/script/lua_tags_input.cpp` | automode 系列 / 按键 override / 输入查询 |

- `lua_engine.cpp` 保留：引擎装配、Lua 桥表注册、生命周期。
- 字符串工具（UTF8/Base64/URL，`lua_engine.cpp:791-880`）移入 `src/util/`（见 T1-3）。

### 实施步骤

1. 先只做分发表：每个 if 块原样提取为 `LuaEngine::TagXxx` 成员函数，注册表指向它。
   **每提取 10-15 个 tag 跑一次全量回归**（`runtime_regressions` + `expression_regressions`）。
2. 全部提取完、l_tag 缩为纯派发壳（< 30 行）后，再按上表移动文件。
3. `CMakeLists.txt:63-70` 是逐目录 GLOB，新文件仍在 `src/script/` 下，无需改构建。

### 验收标准

- [ ] `l_tag` 函数体 < 30 行；tag 派发为一次哈希查找。
- [ ] `grep -c 'tagname ==' src/script/*.cpp` 为 0（判定逻辑移入 handler 的除外）。
- [ ] 全量 ctest 绿；真机（闪亮女友）标题→对话→推进→CG 切换冒烟无差异。
- [ ] 新增回归：每个已实现 tag 至少一条合成用例（为 Tier 3 测试补齐打底）。

### 风险与回退

- **顺序依赖陷阱**：if-链中若有靠前的宽泛判断"吃掉"靠后的窄判断，提取后语义漂移。
  缓解：提取时逐块对照原文，不做任何条件合并/化简；diff 审查以"纯搬运"为纪律。
- 回退：单任务分支，revert 即可，无数据/格式影响。

---

## T1-2 文本字形缓存（glyph cache）

### 问题与证据

- `src/render/compositor.cpp:1051-1338`：`SetText` 每次调用对**全串**重新栅格化
  （`stbtt_GetGlyphBitmap`，`:1259`）、上传新纹理（`:1305`）、删除旧纹理（`:1310`）。
- 触发频率：对话推进每行新文本、逐字打字机重设、消息层换页、菜单项高亮——
  全部走全量重建。stbtt 栅格化 + 纹理上传是移动端文本场景最大 CPU/GPU 热点。
- **有利条件**：`TextGlyph` 已携带 per-glyph UV（`compositor.h:25-30`），Draw 路径
  已按 glyph quad 绘制——图集方案与现有绘制结构天然契合，无需改 Draw 主路径。

### 方案

新增 `src/render/glyph_atlas.{h,cpp}`：

```cpp
struct GlyphKey {          // 哈希：全部字段参与
    uint32_t codepoint;
    float    size;
    uint32_t fill_color;   // 第一期颜色进 key（见"二期"）
    uint16_t outline;      // 描边宽度（0 = 无）
    uint32_t outline_color;
    // font 由 atlas 所属的 font 实例隐含（一个 Compositor 一个字体）
};
struct GlyphEntry {
    uint32_t atlas_tex;    // 所在页的 GL 纹理
    float u0, v0, u1, v1;  // 页内 UV
    int w, h;              // 栅格化尺寸
    float xoff, yoff;      // stbtt 度量
};
class GlyphAtlas {
public:
    const GlyphEntry &Get(const GlyphKey &k);   // 未命中则栅格化并装入页
    void ReleaseGl();                          // 接入 GL 丢失三态交接
    size_t Hits() const, Misses() const;        // 命中率诊断（draw[] 日志）
private:
    std::unordered_map<GlyphKey, GlyphEntry> cache_;
    std::vector<AtlasPage> pages_;              // 512×512 RGBA，满页开新页
    // LRU：简单水位策略（缓存满 N 页时整页回收重建）
};
```

- `SetText` 改为：折行/禁则/ruby 分配逻辑**完全不动**（`compositor.cpp:295-410` 一带），
  仅把"栅格化+建大纹理"替换为"查 atlas → 组装 TextGlyph（UV 指向图集页）"。
- **多页绘制**：一个文本层可能引用多页纹理。Draw 的 glyph 批处理按 `atlas_tex`
  分组绑定（现有逐 glyph 循环内加分组即可）。
- **GL 丢失**：`ReleaseGl()`（`compositor.cpp:1438-1458`）清空 atlas 页纹理、保留
  度量缓存可复用（重 boot 后仅重上传）。初版直接全清也可以。
- **兜底路径**：图集页数超上限（极端样式爆炸）时，回退现有"整串单纹理"路径，
  保证任何场景不劣化。
- **二期（本任务不做）**：栅格化与颜色解耦（缓存 A8 coverage，颜色在组装时着色），
  先用命中率数据决定是否值得。

### 实施步骤

1. 实现 GlyphAtlas + 单测（构造 font 夹具：复用 tests/data 的合成 TTF 路径）。
2. SetText 接入，保留旧路径为 `#if` 分支或运行时开关，便于 A/B。
3. `Draw()` glyph 批处理按页分组。
4. `ReleaseGl()` 接入。

### 验收标准

- [ ] 扩展 `text_rules_regressions`：同一文本二次 `SetText` 零 `Misses` 增量
  （用 Hits/Misses 计数器断言）。
- [ ] `artc drive --frames N` 输出中纹理创建次数显著下降（draw[] 诊断日志）。
- [ ] 真机对白连续推进 100 行：肉眼字形/描边/ruby 无差异，帧率平稳。
- [ ] 描边、ruby、禁则、多页文本各一条回归用例（旧路径行为 = 新路径行为）。

### 风险与回退

- ruby 注音与复合描边使 key 复杂、失效率高 → 先带颜色进 key 观测命中率；
  命中率 < 70% 再做 coverage 解耦二期。
- 图集碎片 / 页泄漏 → 水位整页回收，避免 per-glyph 指针管理。
- 回退：保留的旧路径开关一关即回。

---

## T1-3 解开 script↔render 循环依赖 + 头文件瘦身

### 问题与证据

- **模块级循环**：`src/script/lua_engine.h:17-19` 全量 include
  `render/compositor.h` / `render/video_player.h` / `render/emote_player.h`；
  反向 `src/render/compositor.cpp:29` include `script/save_storage.h`
  （`:946` 读存档回退）。两侧无法独立复用/测试。
- 冗余 include：`lua_engine.h:51` 已前向声明 Compositor 却仍全量 include；
  `asb_parser.h:12` 同病。
- **职责错位**（都在 script/ 下但与脚本无关）：
  - `input_state.h`：纯输入位模型，被 jni 喂、render 命中测试消费；
  - `native_save.cpp`：zlib + BOWS 二进制反序列化；
  - `save_storage.cpp`：文件系统原子写（O_EXCL+fsync+rename）；
  - `save_metadata.cpp`：存档元数据。
  - render 反向消费 `save_storage`（compositor.cpp:29）本身就是层级错位的证据。
- `layer_kind.h:6` 为一个枚举 include 整个 compositor.h。
- 字符串工具内嵌 lua_engine.cpp:791-880，而 `util/encoding.cpp` 已存在。

### 方案

**纯移动 + include 修改，零行为变化。**

1. 文件迁移：

   | 从 | 到 | 理由 |
   |---|---|---|
   | `src/script/save_storage.*` | `src/util/` | render 已消费，层级应低于 render |
   | `src/script/native_save.*`、`save_metadata.*` | `src/save/`（新目录） | 与脚本无关的序列化层 |
   | `src/script/input_state.h`、`auto_read.h` | `src/input/`（新目录） | 输入子系统，jni↔render 的共享底座 |
   | lua_engine.cpp 内 UTF8/Base64/URL 工具 | `src/util/text.cpp` | 与 encoding.cpp 并列 |

2. **同步 `CMakeLists.txt:63-70`**：GLOB 是逐目录显式列举，新增 `src/save/`、
   `src/input/` 目录必须手动加入列表，否则静默漏编。

3. 头文件瘦身：
   - `lua_engine.h:17-19` 改前向声明（`unique_ptr` 成员的析构已定义在 .cpp 中，
     不完整类型合法；若报错，把用到的成员析构也移到 .cpp）。
   - `DialogRequest`（`lua_engine.h:40` 起）拆独立 `src/script/dialog_request.h`，
     jni 消费它时不再拖进 Lua 头。
   - `layer_kind.h` 改为自含（枚举定义不依赖 compositor.h）。

4. 验收断言：`grep -r '#include "render/' src/script/` 与
   `grep -r '#include "script/' src/render/` 均为空。

### 实施步骤

迁移与瘦身分两个 commit：先迁移（include 路径全仓替换），跑全量回归；
再瘦身 lua_engine.h，跑全量回归 + 编译时间记录（`artemis-mac` 目标重编译全部
核心源，头瘦身收益直接体现在增量编译上）。

### 验收标准

- [ ] 两个方向的跨模块 include 均清零（grep 断言写进 CI 或 presubmit 脚本）。
- [ ] 全量 ctest 绿；四个构建目标（CLI/JNI/mac/tests）全绿。
- [ ] `touch src/render/compositor.h` 后 `src/script/*.cpp` 无需重编译（级联缩短证据）。

### 风险与回退

- 唯一风险是 include 路径漏改导致隐式传递依赖暴露（原本经由 A 捎带 include B），
  编译错误集中暴露，逐个补直连 include 即可，属一次性阵痛。
- 回退：revert 提交。

---

## T1-4 pf8 查找索引化 + 文件句柄复用

### 问题与证据

- `src/pack/pf8_reader.cpp:96-113`：`Find()` **线性扫描**全部条目，且每条目调用
  `NormalizeLookupKey` 构造临时 string。829 文件的包（卡拉玛里斯 OBB）每次查找
  都付 O(n×name_len) 代价，音频/图像/脚本按名解析全部经过这里。
- `src/pack/pf8_reader.cpp:127`：每次 `Read` 重新 `fopen`，包括 `ReadRange`
  流式读（视频/音频分块）。

### 方案

**查找索引**：
- `Open()` 解析完 entries 后建 `std::unordered_map<std::string, size_t> index_`：
  normalized name → **首个**命中条目的下标。
- 语义保持："首条优先"是官方历史行为（`pf8_reader.h:25-26` 注释明示），建表时
  重复键**不覆盖**即可与线性扫描等价。
- `NormalizeLookupKey` 的临时 string 构造移到建表时一次性付出。
- `Find()` 的精确匹配 → 归一化匹配两段逻辑合并进索引（观察 `:96-113` 的两段式
  查找实际差异，若精确匹配也走同一张表则语义等价；若"精确优先、归一化次之"
  有可观察差异，则建两张索引，保持顺序）。

**句柄复用**：
- `Open()` 成功后持有 `int fd_`（`open` + `pread`，避免 FILE* 锁与缓冲双重拷贝），
  析构 / `Open` 重入时关闭。
- `Read` / `ReadRange` 全部改 `pread`（天然支持并发读，为 Tier 2 音频线程化铺路）。
- **线程性**：当前 vorbis_stream 一次性整文件载入（`vorbis_stream.cpp:13-21`），
  无跨线程并发读；改 `pread` 后即使未来引入 feed 线程也无锁安全（fd 共享、偏移
  显式传递）。

### 实施步骤

1. 加索引（保持 Find 双段语义），`pf8_regressions` 全绿。
2. 改 pread + fd 成员，检查所有调用点（PackManager 链、ReadRange 流式路径）。
3. 大包冒烟：卡拉玛里斯 829 文件 `artc list` + `extract` 抽验。

### 验收标准

- [ ] `Find` 无线性扫描（代码断言：`pf8_reader.cpp` 不再在查找路径上构造临时 string）。
- [ ] pf8_regressions 全绿；首文件解密魔数抽验（`artc verify`）不变。
- [ ] 大包查找延迟：`artc list` 前后对比（829 条目级别应有可感知差异）。
- [ ] `lsof` 观察长会话无 fd 泄漏（Open/析构配对）。

### 风险与回退

- 双段查找语义等价性是唯一风险点——用现有三个实测包（root.pfs / reaanidt.pfs /
  OBB）的 `artc list` 输出做前后 diff 断言。
- fd 生命周期：注意 GL 丢失重 boot 时 PackManager 是否会被重复 Open 同一包
  （配合 Tier 2 EngineContext 一并收敛）。

---

## 任务依赖与顺序建议

```
T1-1 ──┐
T1-2 ──┼── 相互独立，任意顺序/并行
T1-3 ──┤   （T1-2 与 T1-3 同改 compositor 文件，注意 rebase 协调）
T1-4 ──┘
```

T1-3 完成后，第二梯队的 EngineContext（T2-1）才有干净的前置；T1-4 的 pread
改造是 T2-3 音频线程化的前提。建议顺序：**T1-3 → T1-1 → T1-4 → T1-2**
（先解耦、再拆函数、再加速，glyph cache 最后做以便独占 compositor 文件）。
