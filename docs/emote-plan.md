# E-mote 插件剩余工作计划

> **定位**：E-mote 兼容面（PSB 模型解码 → 场景求值 → `EmotePlayer` 播放 →
> Lua `e:createEmoteLayer` 代理）的未完成项清单。按优先级排列，每项独立
> 可立项，不阻塞其余工作。
>
> **依据**：对官方驱动 `emotedriver.dll`（PE32 x86，2018-12-21，M2 E-mote 运行时
> + 记录/回放驱动）的静态分析；`IEmotePlayer` 94 槽 / `IEmoteDevice` 25 槽
> 虚表；18 条 record/replay 格式串确定了官方方法名与参数类型。
> 分析产物为 `/tmp/emote_*.txt`（临时，重新分析见附录 C）。
>
> **维护约定**：改动 E-mote 子系统必须先扩 `tests/emote_regressions.cpp` 或
> `tests/runtime_regressions.cpp`（AGENT.md §8）；物理/网格等无法渲染的行为
> 保持显式不实现，不做"假装"。

## 当前基线（已完成，勿重复实现）

| 子系统 | 状态 |
|---|---|
| PSB/MDF | v2–v4 解码、zlib MDF、名称/资源/数组；**加密头部**（密钥推导 + adler32 校验，`src/pack/psb.cpp:147-189`） |
| 贴图 | RGBA8、CI8/调色板、DXT5、BC7；`none`/`RL` 压缩；桌面 A8R8G8B8 通道交换 |
| 场景 | 图层/关键帧求值、`parameterize` 变量绑定、子动作、per-icon 贝塞尔网格（`bp`）、按 ID 的 z 序 |
| 播放器 | 时间轴播放/停止/顺序队列、hold-end、差分（diff）时间轴、变量、坐标/缩放/旋转/颜色/透明度、镜像、显隐、fade in/out、**blend ratio 过渡**、Skip/Pass/Step、**灰度**、**差分变量** |
| Lua 代理 | `createEmoteLayer`/`getEmoteLayer`/`getEmoteVersion`；set*/get* 全套；`setScale`（3 参官方形式 + 4 参扩展）、fade/blendRatio 已按驱动契约带 ease/flags |
| 回归 | `emote_regressions`（模型/播放器/PSB）、`emote_mesh_regressions`、`emote_timeline_regressions`、`runtime_regressions`（Lua 全链路） |

---

## 总览

| 编号 | 任务 | 优先级 | 依赖 | 主要依据 |
|---|---|---|---|---|
| E1 | 官方 `IEmotePlayer` 94 槽契约逐项归档 | P0 | 无 | 虚表 + 反汇编（仅 18 槽有名字） |
| E2 | `SetVariableDiff`/`GetVariableDiff` 语义核对 | P0 | 真机/真实模型 | 驱动 pair 控制、`variableMatchList` |
| E3 | `PlayTimeline` flags / `Pass`/`Step` 语义核对 | P0 | 真机 | 驱动 `0x10019960`、`0x10018dd0` |
| E4 | 灰度 GL 观感验证 + 分数灰度回归 | P0 | GL 环境 | `layer_shader.cpp` 已改 float |
| E5 | 物理（`SetOuterForce`/`SetOuterRot`/`startWind`）决策与实现 | P1 | E1、真实模型 | `bustControl` 等元数据 |
| E6 | 加密 PSB 整包（`EmoteFilterTexture`） | P1 | 样例文件 | 导出 `0x10003920` |
| E7 | 多文件（拆分）PSB 归档 | P1 | 样例文件 | `createEmoteLayer` 显式拒绝 |
| E8 | 贴图格式补齐（DXT1/3、16bit、ETC1/PVRTC…） | P2 | E6 | 驱动格式串 |
| E9 | 场景节点：模板/遮罩合成、网格扩展、粒子 | P2 | 真实模型 | 加载期显式拒绝清单 |
| E10 | 控制类元数据（眨眼/口型/选择器/orbit…） | P1 | 真实模型 + krkrsdl3 对照 | `*Control` 元数据键 |
| E11 | `SetColor` RGB tint（colormultiply）/ 模型镜像位 | P2 | 无 | 驱动 `SetColor %d %f %f`、`metadata.mirror` |
| E12 | `CalcLayerFrameInfo` 是否需要暴露 | P2 | E1 | `0x1000b370` |
| E13 | 驱动 record/replay 用作差分验证工具 | P3 | Windows 参照环境 | `0x1001ae50` 命令解析 |
| E14 | 实测游戏与 GL 冒烟矩阵（持续） | P0 | 真机 | AGENT.md §9 |
| E15 | 同步点与动画结束语义（`syncTime`/`skipToSync`） | P1 | 真实模型 | krkrsdl3 `GenerateAniTree` |
| E16 | 命中测试（`contains`/`hitTest`、shape/blank 源） | P2 | 真实游戏调用 | krkrsdl3 `EmoteHitFrame` |
| E17 | 播放状态序列化（`serialize`/`unserialize`） | P3（按需） | 存档系统 | krkrsdl3 `EmotePlayer` |

> 外部参考：`/Users/weiss/github- engine/krkrsdl3/plugins/emoteplayer`（KiriKiri Z，
> BSD 三条款式）。只提取行为/接口事实，不复制代码。对照明细见
> 「附录 D — krkrsdl3 对照」。E10/E15 的依据来自该实现。
>
> 可执行细化（WP0–WP11：契约对齐/眨眼/选择器/attrcomp/镜像/PSB/图集/曲线/命中测试…）
> 见 [`docs/emote-detail-plan.md`](emote-detail-plan.md)。
>
> **契约修订（WP0，真实游戏脚本证据）**：Lua 层 transition/fade/progress 单位是
> **帧**（60fps，`ex.frametime=16.666`）而非毫秒；`setScale(scale, origin_x,
> origin_y)`、`setCoord(x, y, z, angle)`；`getEmoteLayer` 接受表
> `{id=…, next=…}`；甜蜜女友3 还调用 `setMeshDivisionRatio/setHairScale/
> setBustScale`。落地前先做 WP0。

## 落地状态

| 项 | 状态 |
|---|---|
| E1 | **完成**：`docs/emote-driver-contract.md`（18 项命名方法 + 导出/容器/未恢复清单） |
| E2 | 保持"等值反向"推断；未观察到真实调用，待含匹配变量的样本 |
| E3 | Lua 面按脚本证据对齐（WP0）；驱动 flags bit0 清空语义与 Pass/Step 差异仍待真机复核 |
| E4 | **完成**：新增 `ARTC_TEST_CGL`（macOS 离屏 CGL，无需 ANGLE/窗口）跑通 `compositor_regressions`，含分数灰度与 MODULATE2X `colormultiply` 像素断言 |
| E5 | **决策 A**：物理显式不支持；`setHairScale/setBustScale` 已注册并记录 |
| E6 | **完成**：v2 body 解密、宿主 seed 钩子、`lzfs` LZ4 帧；整包 `EmoteFilterTexture` 固定 key 路径按需 |
| E7 | 维持单文件（参考宿主同样要求 exactly one）；无拆分样本 |
| E8 | **完成（桌面格式）**：图集裁切 + DXT1/DXT3/16bit（4444/5551/5650）/A8L8/RGBX8 + mip 链容错，均有合成回归；ETC1/PVRTC 无命中样本，维持显式拒绝 |
| E9 | **完成（本机验证）**：per-frame `color`、blank 描述符、`priority`、`bm` 混合、stencil/mask 合成（按标签集独立 stage 蒙版 + alpha 裁剪 + 未绘制则禁用）、HOLD 语义与 1ms 时间容差；meshSync 无载荷（结项）、粒子无样本 |
| E10 | **完成**：眨眼/选择器/attrcomp；clampControl/talkLabel 等待证据 |
| E11 | **完成**：MODULATE2X 颜色 tint + 模型镜像位 |
| E12 | **决策**：由 EmoteScene 内部覆盖（游戏脚本无调用），Lua 不暴露 |
| E13 | 推迟（需 Windows 参照运行 + 工具） |
| E14 | **部分完成**：host 冒烟通过（常轨脱离 300 帧、甜蜜女友3 prologue 1200 帧，立绘模型加载成功；仅遗留缺片 movie 告警）；**TyranorNext 实机**（`libartemis-clean.so`，甜蜜女友3 存档点）已验证立绘渲染，并据实机修复 inheritMask/bm/盒中心偏移/`Animated::Finish` 归零/HOLD 子树等 6 项；stencil mask 合成仍缺（脸部被未裁剪 mask 块覆盖）；官方内核 A/B 需要用户切换存档 |
| E15 | **部分**：sync 推导 + C++ `SkipToSync` 完成；Lua 注册待真机证据 |
| E16 | **完成**：`contains`/`hitTest` + shape/blank/clip 源 |
| E17 | 按需（未观察到写档调用），暂不实现 |

---

## E1 — 官方 `IEmotePlayer` 契约逐项归档（P0）

### 问题与证据

- 驱动 `PEmotePlayer` 虚表（`0x1008ba88`，95 槽）中只有 18 个方法有
  record/replay 名字（附录 A），其余槽位（getter、层信息、命中测试、
  `MMotionPlayer` 转发等）尚未命名。
- 项目 Lua 代理是按"游戏脚本实际调用面"反推的，可能与官方槽位存在遗漏
  或语义错位（如 flags、时序）。

### 方案

1. 用 r2 对每个槽位函数做行为分类（getter/setter/转发/复杂逻辑），
   结合被调 trace 字符串、`axt` 调用图与字段偏移，给出槽位→候选名映射。
2. 汇总为 `docs/emote-driver-contract.md`：槽位号、字节偏移、函数地址、
   参数、返回值、语义与证据。
3. 与现有 Lua 方法表逐项 diff，产出"缺失/差异"清单，作为 E2–E12 的输入。

### 验收标准

- 契约文档覆盖全部 94/25 槽，未知槽显式标注"未命名/存疑"。
- 清单中每个差异项有对应回归或明确的"不实现"结论。

---

## E2 — `SetVariableDiff` / `GetVariableDiff` 语义核对（P0）

### 问题与证据

- 驱动：pair 条目以第一个 label 为 key、内含单一控制值；`GetVariableDiff(a,b)`
  读回该值（`0x1000d980`）；模型 `variableMatchList` 由驱动解析
  （`0x10016390`）但运行时用途未定。
- 当前实现（`src/render/emote_player.cpp` `SetVariableDiff`）采用推断的
  "等值反向"（`label=+v`，`pair=-v`），符号方向未经验证；`getVariableDiff`
  getter 未暴露。

### 方案

1. 找含匹配变量的真实模型（`variableMatchList` 非空），观察官方驱动/游戏
   对同一调用的轨迹（可用 E13 的记录文件）。
2. 必要时修正符号/组合方式，并在模型中解析 `variableMatchList` 作为合法
   pair 的白名单（当前为"两 label 都须为模型变量"）。
3. 验证通过后补 `getVariableDiff` Lua 代理。

### 验收标准

- 真实模型上 `setVariableDiff` 后渲染与官方一致（截图/逐帧对比）。
- 回归覆盖：符号、过渡 ease、未知变量报错、`getVariableDiff` 读回。

---

## E3 — `PlayTimeline` flags / `Pass` / `Step` 语义核对（P0）

### 问题与证据

- 驱动：`PlayTimeline` flags bit0=保留现有播放（否则先 `StopTimeline`），
  bit1=重置逐层控制（`0x10018dd0`）；`FadeInTimeline` 内部用 3。
- 项目：bit1 被解释为"顺序队列"（kTimelineSequential=2），该语义来自游戏
  行为观察、属引擎插件层；`Pass` 项目实现为"立即完成过渡"，驱动实现为对
  标记轨道做 20 ms 淡出（`0x10019960`）；`Step` 驱动为准一步控制过渡，
  项目实现为推进一帧。
- 这三处是"引擎插件语义 vs 驱动语义"的分层问题，需确认项目按哪一层对齐。

### 方案

1. 在实测游戏上开 `tag[trace]`/emote 日志，记录实际调用序列与视觉结果。
2. 若发现走样，优先按"Lua 可见行为"对齐，并在 `emote_player.h` 注释中
   记录与驱动的差异；不得静默混用两套 flags。

### 验收标准

- 三处语义各有结论（保持/修改）与至少一条回归或真机记录。

---

## E4 — 灰度 GL 观感验证 + 分数灰度回归（P0）

### 问题与证据

- `LayerEffect::grayscale` 已由 bool 改为 0..1 float，着色器本来就是
  `uniform float`；现有 GL 回归只测了 `grayscale=1`（`compositor_regressions.cpp:541`）。
- `emote_regressions` 只在桩合成器上验证了 prop 下发，未验证像素结果。

### 方案（已落地）

1. `compositor_regressions.cpp` 新增分数灰度用例（`grayscale=0.5` →
   `mix(luma,c,0.5)`）与 `colormultiply=0x808080`（MODULATE2X 中性 0x80）
   的像素断言，归属 GL 回归。
2. 新增 `ARTC_TEST_CGL` 构建开关：macOS 用离屏 CGL 上下文跑同一份
   `compositor_regressions`，无需 ANGLE 或窗口。无 drawable 的 CGL 默认
   帧缓冲不可用，测试把 "屏幕"（含 `LayerShaders::Begin/End` 的
   `parent`）指向自建 FBO，等价 EGL pbuffer。
3. 真机/窗口观感仍建议在 mac 宿主复看。

### 验收标准

- `ARTC_TEST_CGL=ON` 的 `compositor_regressions` 通过（本机 Apple M1 Pro
  已过）；`ARTC_TEST_GLES` 的 ANGLE 路径保持不变。

---

## E5 — 物理：`SetOuterForce` / `SetOuterRot` / `startWind`（P1）

### 问题与证据

- 驱动：`SetOuterForce %s %f %f %f %f`（part = `bust`/`hair`/`parts`）、
  `SetOuterRot %f %f %f`；模型元数据含 `bustControl`/`hairControl`/
  `partsControl` 与 `gravity`/`spring`/`friction`/`bendR`/`bendS`/`b_rate`/
  `v_bound`/`ud_eft`/`bend_spd`/`bend_vol`/`length`/`var_lr`/`var_ud`/`var_lrm`。
- 项目当前不注册这些方法（logging stub）。

### 方案

分支决策，按实测游戏占比选择：

- **A（保守）**：保持显式不支持，补齐 `setOuterForce`/`setOuterRot` 的
  参数校验与首见日志，文档化。
- **B（近似）**：实现 bust/hair 一维弹簧-阻尼跟随（gravity/spring/
  friction/bendR），只驱动对应 part 链的旋转；`SetOuterForce` 作为外力注入。
- **C（完整）**：按驱动 `EPWindControl`/`EPBustControl` 语义建模，代价最高。

krkrsdl3 对照：其物理同样未实现（`updatePhysics` 空、`setOuterForce`/
`startWind`/`initPhysics` 为 TODO 桩），说明"显式不支持"是与现有成熟实现一致的
务实选择；**默认选 A**，仅当实测游戏确需物理时再启动 B。

### 验收标准

- 选定分支有 ADR/注释依据；B/C 需要真实模型逐帧对比截图。

---

## E6 — 加密 PSB 整包解密（`EmoteFilterTexture`）（P1）

### 问题与证据

- 导出 `EmoteFilterTexture(uint8_t*, size, callback)`（`0x10003920`）用固定
  key `851083516` + 与 `PsbCipher` 相同的 xorshift 对整包做过滤；
  `EmoteCheckValidObject(const uint8_t*, size)`（`0x10003ed0`）做
  "解密+可解析"校验；内部类 `PSBFilter`/`StructCryptFilter`。
- 项目目前只解密头部（`psb.cpp:147-189`），README 标注"加密 PSB 不支持"。
- krkrsdl3 对照（`emotefile::load`）：密钥 = `{0x075BCD15, 0x159A55E5,
  0x1F123BB5, seed}`；seed 由宿主注入（`setEmotePSBDecryptSeed`），另提供
  `setEmotePSBDecryptFunc(buffer,len)` 自定义解密回调（游戏可在 XP3 过滤之外
  再叠一层）；**PSB v2 额外解密 `[offsetEncrypt, offsetChunkOffsets)`**；
  另支持 `lzfs`（`04 22 4D 18`）LZ4 帧容器。

### 方案

1. 用样例确定流的起点/长度语义（头部解密后 cipher 是否继续覆盖 body），
   在 `psb.cpp` 增加整包 filter 路径（复用 `PsbCipher`）。
2. `DecodePsb` 失败信息区分"头部校验失败"与"body 解密失败"。
3. 合成回归：用已知明文按该算法加密 body，验证能解回；`psb_encrypted_regressions` 扩展。

### 验收标准

- 真实整包加密模型可加载；合成加密回归通过；README 能力表更新。

---

## E7 — 多文件（拆分）PSB 归档（P1）

### 问题与证据

- `createEmoteLayer{files={a,b,...}}` 当前显式拒绝（`lua_engine.cpp`
  `multi-file E-mote archives are not supported`）。
- 驱动侧有 `sourceIconRenameMap`、多 motion 资源合并迹象，官方引擎支持
  拆分导出。

### 方案

1. 从样例确定拆分格式（各部分 PSB 的资源如何拼接/重命名）。
2. 在 `l_createEmoteLayer` 中合并多文件为一个 `PsbDocument`（资源偏移重写 +
   `sourceIconRenameMap` 应用），失败时事务性拒绝。
3. krkrsdl3 走的是"附加文件"路线（`_attach` + `addEmoteFile`：主文件 + 附加
   文件共同参与 chara/motion/变量查找，不做物理合并）——可作为备选实现路线，
   先看样例文件的实际拆分语义再二选一。

### 验收标准

- 拆分模型与合并模型渲染一致；错误路径不破坏已有图层。

---

## E8 — 贴图格式补齐（P2）

### 问题与证据

- 驱动支持字符串：`A8L8`、`RGBA5650`、`RGBA5551`、`RGBA4444`、`RGBX8`、
  `DXT1`、`DXT3`、`ETC1`、`PVRTC_4BPP`、`PVRTC_2BPP`、`BC7`、`DXT5`，
  以及 `mipMap`/`mipMapLevel`。
- 项目仅 RGBA8/CI8/DXT5/BC7（`emote_model.cpp:78-115`）。

### 方案（已落地）

按实测游戏命中顺序补齐：

- **DXT1/DXT3**：`block_decode.cpp` 解块（BC1 含 1-bit alpha 打孔模式，
  BC2 显式 4bit alpha），合成回归覆盖不透明/打孔/alpha 三种情况。
- **16bit**：`RGBA4444`/`RGBA5551`/`RGBA5650`，以及 `A8L8`（亮度+alpha）、
  `RGBX8`（丢弃 X 字节，桌面序仍按 A8R8G8B8 换序）。
- **mipMap/mipMapLevel**：带 mip 描述符时接受 level 0 之后的附加数据并只读
  level 0；无描述符时仍要求精确长度。
- **ETC1/PVRTC**：实测（甜蜜女友3 DXT5、krkrsdl3 桌面路径）均无命中样本，
  维持 `unsupported E-mote pixel format` 显式拒绝；待移动端图集样本再补。

krkrsdl3 对照：其只支持 `RL`/`none` 与 RGBA8/调色板展开，格式上不提供借鉴；
但暴露了一个本项目缺的**图集裁切**语义：`spec=win` 的 icon 带
`clip{left,top,right,bottom}` + `texWidth/texHeight`，`pixel` chunk 是整张
图集，需按 `left/top` 裁到 icon 尺寸（`readIconTobuffer` 的裁切分支）。
本项目现在把 chunk 当整图解析，遇到图集模型会报长度不符或错位，需要一并补。

### 验收标准

- 对应格式的裁剪图标像素与官方对照一致（色彩序/alpha 语义）；
  `tests/emote_regressions.cpp` 的合成块已覆盖。

---

## E9 — 场景节点：模板/遮罩合成、网格扩展、粒子（P2）

### 问题与证据

- 加载期显式拒绝（`emote_scene.cpp`）：`stencilType!=0`、
  `coordinate`/`groundCorrection`、`inheritMask` 非默认、
  `transformOrder` 非默认排列、motion variable initializer、
  非 0/2/3 节点类型、child motion `mask`/未知控制字段。
- 驱动元数据出现：`stencilCompositeMaskLayerList`、`meshTransform`、
  `meshSyncChildMask`、`meshDivision`、`meshCombinator`、`meshCombine`、
  `objTriPriority`、`particleMotionList`、`particleMaxNum`…。

### 方案

1. 先收集实测游戏中触发的拒绝日志（错误信息已带节点 label），按出现频次
   排序。
2. 优先实现最常见的 1–2 项（大概率是 `stencilType`/遮罩合成或网格扩展），
   其余维持显式失败。
3. krkrsdl3 已有可参照的实现面：`meshDivision` 运行时 CPU 细分缓存、
   `meshCombine`/`meshTransform`/`meshSyncChildMask`、节点 `priority`
   重排（`motion.priority[0].content` 的索引列表决定求值顺序）、帧曲线
   `zcc/ccc/cc`、per-frame `color`（缺省 `0xff808080`，`hasColor` 区分）、
   `blank:w:h:ox:oy`/`shape/...`/`clip` 源、`stencilCompositeMaskLayerList`
   + maskTarget 渲染。实现前先对照本项目拒绝清单，按需选做。

### 已落地（甜蜜女友3 立绘证据）

- **per-frame `color`**：8 处全为中性 `0x808080ff`，确认是 MODULATE2X 空间；
  按 `2*c` 折算到直线因子并与播放器 tint 相乘，逐 part 下发
  `colormultiply`（数值/数组两种写法都接受）。
- **`blank` 描述符**：真实格式是 `icon="w:h:ox:oy"`（无 `blank:` 前缀），
  解析后作为布局原点；`blank:w:h:ox:oy` 写法兼容保留。
- **`priority`**：每个 motion 都有。按参考语义（content 反向枚举，
  末项 rank 0 先画；缺项顺延）对直接子节点做稳定重排。该作 content 均为
  恒等反序，视觉不变；其他游戏若真重排也按参考行为落地。

### 实机修复（TyranorNext + 甜蜜女友3，2026-09-30）

- `inheritMask` 白名单过严：真实立绘使用 `0x7FC/0x20007FC/0x200020C/
  0x24007FC`，放宽为接受任意数值；部分继承（低 0x1FC 位不全）暂按全继承并
  一次性日志。
- 内容 `bm` 不再拒绝：低半字节映射 add(1)/reverse-subtract(2,5)/multiply(3)/
  screen(4)，`layer_shader` 新增 multiply 与 reverse-subtract 混合函数。
- `createEmoteLayer` 的 width/height 现在生效：模型原点位于盒中心
  （参考宿主 `model_origin`），修复立绘整体偏移。
- **`Animated::Set(v, 0, …)` 与 `Animated(v)` 现在同步写 `target`**：此前
  `Skip()/Pass()` 的 `Finish()` 会把 setScale/setCoord/setColor 的即时值
  全部归零，立绘整体不可见（实机主症状）。
- HOLD（type 0）帧：节点自身不绘制，但子节点照常递归（对齐参考宿主
  `visit_layer` 无条件访问 children）。

### stencil/mask 合成（已落地）

- 求值期按参考语义传播 `stencilType & 0x4 + stencilCompositeMaskLayerList`：
  后代继承，声明节点替换整张列表；容器自身不绘制、子节点照常渲染。
- 绘制期把 mask 标签解析到最近的同名可绘制 part（参考宿主按 source_label +
  中心距离匹配），将该 part 及其子树渲染进 **stage 空间蒙版纹理**
  （`Compositor::RenderStageMask(key, ids)`，每个标签集一张，`SetLayerStageMask`
  挂到被裁剪 part；`LayerEffect.stage_mask` 走 coverage 通道，按蒙版
  **alpha** 乘算）。
- mask 源未绘制时按参考（krkrsdl3）"蒙版层未绘制 → hasStencil=false"处理：
  不挂蒙版、保持未裁剪，而不是用空蒙版把部件裁没。
- 回归：合成标签传播（emote_regressions）、GL 蒙版裁剪像素断言
  （compositor_regressions `ARTC_TEST_CGL`）。

### meshSyncChild / 粒子（结项结论）

- 全部 132 个真实立绘都带 `meshTransform=1`、`meshSyncChildMask`
  （0x8 形状位 10804 处）、`meshCombine=1`（2966 处），但 **全部模型的
  `mesh.bp`/`mesh.cc` 载荷为空（bp_nonempty=0, cc_nonempty=0）**——没有任何
  可传播/可合并的形变数据，故 meshSync 在上述样本上是 no-op；若未来样本带
  非空 bp 且形状位开启，Evaluate 会一次性日志提示"child mesh deformation
  not propagated"，避免静默。
- 粒子：`particleMotionList`/`particleMaxNum` 在真实模型中零命中，维持显式
  跳过（文档化）。

### 本机（macOS 宿主）验证与修复（2026-09-30 晚间）

- 给 `artemis-mac` 增加开发用注入：`--tap x,y@frame`（按 60fps 帧号注入
  stage 坐标点击，模拟真实 touch：hover + press + release + click）与
  `--snapshot out.png@frame`（全分辨率合成快照）。本地即可复现存/读档与
  渲染问题，不再依赖真机。
- 用官方内核同场景存档缩略图对照，确认脸部被盖的根因是两处缺陷并已修复：
  1. **stage 蒙版纹理被所有标签集共用**：每次 `RenderStageMask` 覆盖同一张
     纹理，所有被裁剪部件实际采样到"最后一次渲染的蒙版" → 脸部被错误裁剪/
     出现色块。改为按标签集分配独立蒙版纹理（`RenderStageMask(key, ids)`）。
  2. **蒙版解析是 O(部件×图层) 的 `EffectiveRect`**（每次调用都走祖先链）：
     双立绘场景掉到约 3fps（300 帧耗时 100 秒），表现为"点击卡死"。改为
     每帧先缓存所有部件中心（O(n)），再按标签分组匹配，恢复满帧。
- 本机复现结论（全分辨率快照）：标题→读取存档→槽位→YES 确认→读档成功
  （`load: checkpoint restored`）；游戏内 Save→槽位→覆盖确认→写档成功
  （`save: checkpoint save0004.dat committed`），存档缩略图正常。
- **meshSyncChild 形状形变已落地**（此前的"无载荷"结论是错的：`eye_pos`/
  `mabuta`/`hi_pos`/`eyebrow_pos` 等帧携带 4x4 Bezier 控制网格）：
  求值期按 `meshTransform` + 形状位建立形变作用域（含 blank/图标
  extent、`inheritMask` 形状位挂起），渲染期对子部件生成 8x8 形变网格。
- **变换模型按官方重构（本轮根因）**：官方在一个 motion player 内按
  `inheritMask` 组合线性状态（翻转移位/旋转/缩放/剪切各自独立），部分掩码
  （如 0x200081B 不含缩放位）要相对 **motion 根** 重建，嵌套 motion 以进入
  节点的仿射为新根；子层继承父层仿射，除非父层带 0x400000（透明父级）。
  旧实现"父链全量乘法"把表情/头发的旋转缩放错误地压到脸部件/脖子/追加
  部件上，导致贴图错位成硬边色块。现在求值期计算每个节点的绝对仿射并由
  部件层携带（布局层恒等），颈部/眼皮/追加部件的位置、朝向与官方一致。
- **后层绘制序**：官方同场景基准里后发在人物之后、脖子/下巴之后；新增合成器
  `paint` 提示（`SetLayerPaint`，小于 0 先画/在后）：`後髪`=-1、
  头发下的 `追加パーツ`=-2、`■首`=-3（均为经验规则，待官方 A/B 后固化）。
- 本机验证：`say_6`/`shi_36` 单模型预览（CGL）和游戏内快照（1920×1080）
  脸部完整、发型层次正确、无整块色斑；`g1.png` 与修复前对比立绘完全改观。
- 仍存差异：个别角色的少量 `■追加パーツ` 硬边补片（如 `shi_36` 侧发下的
  脸脖补片）——其官方的蒙版/层序语义需要官方内核在同一行对话的存档截图
  对照后定规则（设备接回后验证）。

### 备注

- 尾部 HOLD 语义：动作用尽后**保持上一有效帧继续绘制**（官方立绘长时间常驻），
  并用 1ms 帧时间容差消除嵌套时钟浮点误差（此前会冻结在 60.999 帧、恰好
  掩盖该语义）。
- 真机"点击没反应"实为上面的 3fps 回归；"画面被缩小/切后台重启"属于宿主的
  窗口尺寸与进程策略（引擎 stage 固定 1920x1080，本机快照已确认），待设备
  接回后在 TyranorNext 侧核对。
- 早期用 `emote_preview` 默认姿势推断的"后发绘制序"问题，在全流程（带游戏
  时间线/变量）下不再出现，判定为预览工具默认状态差异，不再作为引擎缺陷。

### 验收标准

- 打开失败的模型；被修复项在 GL 下与官方对照；未修复项仍在加载期明确报错。

---

## E10 — 控制类元数据：眨眼/口型/选择器/orbit（P1）

### 问题与证据

- 元数据键：`transitionControl`、`selectorControl`、`immediateControl`、
  `clampControl`、`mirrorControl`、`eyeControl`、`eyebrowControl`、
  `mouthControl`、`bustControl`、`hairControl`、`partsControl`、
  `orbitControl`、`charaProfile`、`variableMatchList`、`optionList`/
  `offValue`/`onValue`、`blinkIntervalMin/Max`、`blinkFrameCount`、
  `blinkEnabled`、`talkLabel`、`face_cheek`/`face_tears`/`face_eye_hi`。
- 这些多数由模型编辑器生成；需判定哪些是**引擎/驱动运行时功能**
  （自动眨眼、口型同步、选项选择），哪些只是编辑器标注。

### 方案

逐项找证据（驱动代码引用 / 游戏脚本调用 / E-mote 官方手册），产出
"运行时 vs 编辑器"判定表；运行时项进入实现队列，编辑器项明确忽略。

krkrsdl3 已证实两项是运行时功能，建议优先实现：

- **自动眨眼（eyeControl）**：参数 `label/beginFrame/endFrame/
  blinkFrameCount/blinkIntervalMin/Max`；空闲时随机等待 `[min,max]` 帧，
  到时按 `blinkFrameCount` 做"闭→开"两端线性动画写入变量，播完后恢复
  `baseVal` 并重新等待。注意：带眨眼/口型的立绘不能让 `animating` 恒真，
  否则等待动画结束的对话流程会挂起（krkrsdl3 有 Nekopara2 类事故记录）。
- **选择器（selectorControl）**：每项 `{label,onValue,offValue}`；
  `setVariable(selectorLabel, opt)` 选中项写 `onValue`、其余写 `offValue`
  （加载时先 select 0）；时间轴控制跳过被 selector 接管的变量。
- **attrcomp**：`{chara,motion,layer,value}` 规则，`value<=0` 时在加载期把
  目标节点标记为不渲染（本项目未解析，会导致应隐藏的层仍然显示）。
- `eyebrowControl` krkrsdl3 只解析未驱动（与本项目现状相当），可暂缓；
  `mouthControl`/`talkLabel`/`face_*` 在 krkrsdl3 中无运行时实现，仍需实测
  游戏确认是否由引擎侧口型驱动。

### 验收标准

- 判定表齐全；被判定为运行时的项有实现或明确的排期。

---

## E11 — `SetColor` RGB tint（P2）

### 问题与证据

- 驱动 `SetColor %d %f %f` 会乘色（着色器有 `colorMultiply`）；
  项目 `EmotePlayer::SetColor` 只改 alpha（注释称"合成器没有 RGB tint"，
  但 `Compositor` 已有 `colormultiply` prop 与着色器 uniform）。
- 相关：模型自带的 `metadata.mirror` 位（krkrsdl3 在渲染矩阵上做
  `scale(-1,1,1)` 自动镜像）本项目未解析，只支持 Lua `setMirror`。

### 方案

1. 在 `EmoteScene::Render` 的 part 层 props 中下发 `colormultiply`
   （由 `EmotePlayer` 传入的 RGB）。
2. 更新 `emote_player.h` 注释与回归（round-trip + GL 像素）。

### 验收标准

- `setColor(0x80ABCDEF)` 的 RGB 分量在 GL 回归中可见；alpha 行为不变。

---

## E12 — `CalcLayerFrameInfo` 暴露决策（P2）

### 问题与证据

- 驱动 `CalcLayerFrameInfo %f`（`0x1000b370` → 实现 `0x1000b490`）计算
  逐层帧信息，供引擎侧合成；`fcn.1001ae50` 记录/回放中也有该命令。
- 本项目自行完成场景求值与合成（`EmoteScene`），Lua 代理目前未暴露。

### 方案

在 E1 契约表中明确该方法的输出结构；确认游戏脚本/引擎层是否调用。
若不需要则文档化"由 EmoteScene 覆盖"的结论并关闭。

### 验收标准

- 有结论（实现/不实现）及依据。

---

## E13 — 驱动 record/replay 作为差分验证工具（P3，可选）

### 问题与证据

- 驱动内建记录/回放：`player+0x1d8`（0=normal/1=record/2=replay），文本
  缓冲区 `+0x1dc`，解析器 `0x1001ae50`（命令即附录 A 的 18 个 Set* 调用）。
- 可借此把官方运行中的 API 调用录成文本，在本项目端逐条回放并比对。

### 方案

1. 在 Windows 参照环境用官方驱动录一段游戏会话（或用 `EmoteCheckValidObject`
   自测样本）。
2. 写一个离线回放夹具（tests 内），把记录文本转换为 `EmotePlayer` 调用，
   对逐帧图层变换做数值 diff。

### 验收标准

- 至少一段真实会话的轨迹 diff 无未解释差异；工具脚本纳入 `tools/`。

---

## E14 — 实测游戏与 GL 冒烟矩阵（持续）

| 场景 | 检查点 |
|---|---|
| 标题 → 对话推进 | E-mote 图层存在、推进无卡死 |
| 灰度/回忆闪回 | `setGrayscale` 过渡平滑，退出后复原 |
| 表情/变量切换 | `setVariable`、`setVariableDiff` 渲染正确 |
| 时间轴淡入淡出 | `fadeInTimeline`/`fadeOutTimeline` ease 生效 |
| 混合过渡 | `setTimelineBlendRatio(label, ratio, time, ease, flags)` 过渡与自动移除 |
| `step()` 驱动 | 逐帧动画推进正常，无双重计时观感 |
| 复杂模型 | 记录加载期"unsupported E-mote …"日志，回流 E8/E9 |

必跑命令（AGENT.md §9）：

```bash
cmake -B build-test -DCMAKE_BUILD_TYPE=Release -DARTC_BUILD_TESTS=ON
cmake --build build-test -j8 && ctest --test-dir build-test --output-on-failure
cmake --build build-mac -j8 --target artemis-mac
cmake --build build-android -j8
# 有 ANGLE 时：
# cmake -B build-gles -DARTC_BUILD_TESTS=ON -DARTC_TEST_GLES=ON && ctest --test-dir build-gles
# macOS 无 ANGLE/无窗口（离屏 CGL，等价 EGL pbuffer）：
cmake -B build-cgl -DARTC_BUILD_TESTS=ON -DARTC_TEST_CGL=ON
cmake --build build-cgl -j8 --target compositor_regressions && ./build-cgl/tests/compositor_regressions
./build-host/artc drive <游戏目录>/root.pfs --frames 400
```

---

## E15 — 同步点与动画结束语义（P1）

### 问题与证据

- krkrsdl3：`syncTime` = 该 motion 下**非 `parameterize` 节点**的最大有内容帧时刻
  （递归子 motion），`selfSyncTime` 为文件层值；播放结束判定优先取
  `syncTime`，其次 `selfSyncTime`，最后 `lastTime`；`skipToSync()` 直接把
  `clockPassed` 跳到 `getSyncTime()`。
- 本项目目前只按 `timeline.last_time`/`loop_end` 判断时间轴结束，没有"同步点"
  概念；对话等待（`[wait emote]` 之类）在带眨眼/口型控制的立绘上可能挂起或
  提前结束。

### 方案

1. 解析 motion 的 sync 相关字段（`syncTime`/`selfSyncTime`；krkrsdl3 的
   `syncTime` 由帧数据推导，若 PSB 另有 `sync` 字段需一并确认），
   在 `EmoteModel` 中建立 motion → sync 表。
2. 在 `EmotePlayer` 增加 `SkipToSync()` 与结束判定顺序（sync → last），
   并在 Lua 代理注册 `skipToSync`。
3. 回归：合成 motion 含 sync 帧，验证结束时刻与 `skipToSync` 落点。

### 验收标准

- 带同步点的实测游戏对话流程不挂起、不提前；合成回归覆盖三种结束来源。

---

## E16 — 命中测试：`contains` / `hitTest` / shape 源（P2）

### 问题与证据

- krkrsdl3：渲染时收集 shape 节点几何（`shape/rect|circle|point|quad`，
  单位方格 16×16）与非 shape 的可见网格，构建 `EmoteHitFrame`；
  `hitTest([label,] x, y)` 用目标像素坐标、`contains` 保留旧仿射坐标；
  越界几何按渲染目标裁切。
- 本项目目前不注册 `contains`/`hitTest`（走 logging stub），也不支持
  `blank:w:h:ox:oy`、`shape/...`、`clip` 源。

### 方案

1. `EmoteScene::Evaluate` 输出中标记 shape/blank 层并保留其变换与网格。
2. 在 `EmotePlayer` 提供点包含查询（网格三角形判定），Lua 侧
   `contains(label?,x,y)`/`hitTest(label?,x,y)` 两种重载。
3. 回归：合成 shape 节点（矩形/圆/四边形）做包含/排除断言。

### 验收标准

- 实测游戏中立绘点击/触摸判定与官方一致；回归覆盖点/边/顶点与缩放旋转。

---

## E17 — 播放状态序列化（P3，按需）

### 问题与证据

- krkrsdl3 暴露 `serialize()/unserialize()`，把播放进度/变量/时间轴状态存入
  存档并在读档后恢复。
- 本项目存档（BOWS/BOWG 导入器）不含 E-mote 播放状态；读档后立绘会从初始
  状态重建。是否需要在存档中恢复 E-mote 状态取决于实测游戏的存档脚本。

### 方案

先用真机确认游戏是否有 emote 状态写档/读档需求；有则按 `EmotePlayer` 内部
状态设计紧凑序列化（变量/时间轴位置/blend），挂到现有存档管线。

### 验收标准

- 存档→读档后立绘状态（表情/时间轴/blend）与官方一致（或明确不需求）。

---

## 风险与未决

- **E2/E3 的推断语义**（差分符号、flags/Pass/Step 分层）是本计划最大的
  不确定性来源；在真机验证前不要对外宣称完全兼容。
- **E5 物理**投入产出比低且无官方公式文档；krkrsdl3（KiriKiri 侧成熟实现）
  同样未实现物理，佐证"显式不支持"是当前务实选择。
- **E6/E7 依赖样例文件**：没有真实加密/拆分模型时无法闭环，需在任务开始
  时先收集样本。
- **E15 syncTime 语义待真机确认**：krkrsdl3 的"结束判定优先 sync"是为
  KiriKiri 游戏流程服务的；Artemis 侧是否同语义需用实测游戏日志验证。
- 分析产物是 `/tmp` 临时文件，长期结论必须落盘到 `docs/emote-driver-contract.md`（E1）。

---

## 附录 A — 已恢复的官方方法契约（18 项，record/replay 格式串）

| 方法 | 参数 | 语义要点 | 驱动实现 |
|---|---|---|---|
| `SetCoord` | `x, y, time_ms, ease` | 容器位移 | `0x100140d0` |
| `SetScale` | `scale, time_ms, ease` | 单值等比（无 per-axis） | `0x10014280` |
| `SetRot` | `angle, time_ms, ease` | 角度 | `0x10014410` |
| `SetColor` | `argb(int), time_ms, ease` | 乘色 + alpha（初始化 `0x808080FF`） | `0x100145e0` |
| `SetGrayscale` | `value(0..1), time_ms, ease` | 亮度混合 | `0x100147e0` |
| `SetVariable` | `label, value, time_ms, ease` | 变量 | `0x1000bf30` / `0x1000c0e0` |
| `SetVariableDiff` | `label, pair, value, time_ms, ease` | pair 控制（单一值） | `0x1000d630` |
| `SetOuterForce` | `part(bust/hair/parts), 4×float` | 外力/物理 | `0x10014ca0` |
| `SetOuterRot` | `3×float` | 外旋 | `0x10015290` |
| `PlayTimeline` | `label, flags` | bit0 保留播放、bit1 逐层重置 | `0x100171b0` |
| `StopTimeline` | `label` | | `0x10017500` |
| `SetTimelineBlendRatio` | `label, ratio, time_ms, ease, flags` | flags=1 淡出到 0 后移除 | `0x10017710` |
| `FadeInTimeline` | `label, time_ms, ease` | 未播放则 `PlayTimeline(label,3)`，0→1 过渡 | `0x10017a50` |
| `FadeOutTimeline` | `label, time_ms, ease` | 等价 blendRatio(0,time,ease,1) | `0x10017bc0` |
| `Skip` | — | 立刻完成控制过渡并整体重算 | `0x1000b730` |
| `Step` | — | 推进一次控制过渡并整体重算 | `0x1000b920` |
| `Pass` | — | 对标记轨道做 20 ms 淡出 | `0x1000bb10` |
| `CalcLayerFrameInfo` | `time` | 逐层帧信息（引擎合成用） | `0x1000b370` |

ease 权重公式（两处实现一致）：`ease>=0 ? ease+1 : 1/(1-ease)`。

## 附录 B — 导出入口

| 符号 | 地址 | 说明 |
|---|---|---|
| `?EmoteCreate@@YAPAVIEmoteDevice@@ABUInitParam@1@@Z` | `0x100038d0` | `InitParam{host, alloc, free}` → `IEmoteDevice`（`PEmoteDevice`，vtable `0x1008bc08`，25 槽） |
| `?EmoteCheckValidObject@@YA_NPBEK@Z` | `0x10003ed0` | 解密/校验 PSB 缓冲 |
| `?EmoteFilterTexture@@YAXPAEKP6AX0K@Z@Z` | `0x10003920` | 整包 PSB 过滤（key `851083516`） |

## 附录 C — 复现分析

```bash
rabin2 -I -s -E emotedriver.dll            # 概览 / 符号 / 导出
rabin2 -z emotedriver.dll | grep -E 'Set|Fade|Play|Skip|Step|Pass|Calc'   # API 格式串
r2 -q -e bin.relocs.apply=true -c 'aaa; axt 0x1008cf20; pdf @ 0x100140d0' emotedriver.dll
```

关键虚表：`IEmotePlayer 0x1008b900`（94 槽）、`PEmotePlayer 0x1008ba88`
（95 槽实现）、`IEmoteDevice 0x1008b898`、`PEmoteDevice 0x1008bc08`。

---

## 附录 D — krkrsdl3 `plugins/emoteplayer` 对照

> 路径：`/Users/weiss/github- engine/krkrsdl3/plugins/emoteplayer`
> （KiriKiri Z，BSD 三条款式许可）。仅提取行为/接口事实用于兼容规划，
> 不复制代码。规模：`emotefile.cpp` 2831 行（PSB 解析）、`emoterunner.cpp`
> 1251 行（求值/控制）、`emoteplayerclass.cpp` 1239 行（TJS 绑定）、
> `D3DEmotePlayer.cpp` 471 行（D3D 绘制）。

### D.1 该实现已做、本项目缺失（可借鉴）

| 能力 | 关键事实 | 计划项 |
|---|---|---|
| 自动眨眼 | `eyeControl` 参数 `label/beginFrame/endFrame/blinkFrameCount/blinkIntervalMin/Max`；随机等待→两端线性闭开→恢复 `baseVal` | E10 |
| 选择器 | `selectorControl` 每项 `{label,onValue,offValue}`；`selectValue(opt)` 只给选中项写 `onValue`；时间轴跳过 selector 变量；加载时先 select 0 | E10 |
| attrcomp | `{chara,motion,layer,value}` 规则，`value<=0` 在加载期把节点标记为不渲染 | E10 |
| 同步点 | `syncTime` = 非 parameterize 节点的最大内容帧时刻（递归子 motion）；结束判定 sync → selfSync → lastTime；`skipToSync()` | E15 |
| 命中测试 | shape/blank 源、单位方格 16×16、矩形/圆/点/四边形子类型、网格三角形包含、越界按目标裁切 | E16 |
| PSB 解密钩子 | 密钥 `{0x075BCD15,0x159A55E5,0x1F123BB5,seed}`；宿主 seed + 自定义回调；v2 解密 `[offsetEncrypt,offsetChunkOffsets)` | E6 |
| LZ4 容器 | `lzfs` magic `04 22 4D 18`，块校验/内容长度/字典标志解析 | E6 |
| 多文件 | `_attach` + `addEmoteFile`：主文件与附加文件共同查 chara/motion/变量（不物理合并） | E7 |
| 图集裁切 | `spec=win` icon 的 `clip{left,top,right,bottom}` + `texWidth/texHeight`，pixel chunk 为整图集按 left/top 裁切 | E8 |
| 网格/节点扩展 | `meshDivision`（运行时 CPU 细分缓存）、`meshCombine`/`meshTransform`/`meshSyncChildMask`、`priority` 列表重排 nodeList、帧曲线 `zcc/ccc/cc`、per-frame `color`（缺省 `0xff808080`，`hasColor` 区分）、stencil mask 列表 + maskTarget | E9 |
| 运动级变量 | `"motionName/varName"` 寻址与 `parameterCache`；变量→tick 映射 `division*(v-lo)/(hi-lo)`（与本项目一致，互相印证） | E10 |
| 状态持久化 | `serialize()/unserialize()`（变量/进度/时间轴） | E17 |
| 额外变换/查询 | `setSlant/setZoom/setFlip/setCameraOffset`、`getVariableFrameList`、`getCommandList`、`getLayerGetter/getLayerMotion`、`getPlayingTimelineInfoList` | 按需 |

### D.2 本项目更强 / 该实现未做（不必借鉴）

- 贴图：仅 `RL`/`none` + RGBA8/调色板展开；本项目另有 DXT5/BC7/CI8。
- 时间轴插值：仅线性，忽略 easing；本项目与官方 DLL 一致带 ease 权重。
- 时间轴淡入淡出 / `setTimelineBlendRatio` / `pass` / `skip` / `playTimeline`
  flags：均为 TODO 或直接忽略 flags。
- 物理：`updatePhysics` 空实现，`setOuterForce`/`startWind`/`stopWind`/
  `initPhysics` 全为 TODO 桩。
- 时钟：`speedRatio=20`（约 20ms/tick）与本项目 60fps 帧钟不一致，属 KiriKiri
  插件选择；以 Artemis 真机为准，不照搬。
- 基础运动求值：该实现在模型带变量时把基础 motion 固定在 tick 0（只用控制
  驱动）；本项目始终推进基础 motion。差异需用 Artemis 实测确认，勿直接改。

### D.3 对计划的修订

- E5 默认选项确认为 A（显式不支持）。
- E10 提升为 P1，并把自动眨眼 / 选择器 / attrcomp 列为最先实现项。
- 新增 E15（同步点）、E16（命中测试）、E17（按需持久化）。
- E6/E8/E9 的方案补充了上述对照细节。
