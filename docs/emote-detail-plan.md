# E-mote 详细实施计划（参考实现对照落地）

> **定位**：`docs/emote-plan.md`（主计划，E1–E17）的执行细化。本文覆盖
> 「可落到代码与夹具」的工作包（WP0–WP11），每项包含：目标、现状证据、
> 设计、改动文件、实施步骤、回归夹具、验收标准、风险。
>
> **参考实现（只取行为/接口事实，不复制代码，AGENT.md clean-room 约束）**：
> - `art3m1s-core`（`/Users/weiss/github- engine/art3m1s-core`，Artemis 生产宿主）：
>   `crates/asb-interpreter/src/lua_engine.rs`（**Artemis Lua 契约**）、
>   `crates/art3m1s-emote`（Rust 实现）、`src/runtime/emote.rs`（集成/眨眼）。
> - `krkrsdl3`（KiriKiri Z，BSD 三条款式）：`plugins/emoteplayer`。
> - `crates/eluna`（MPL-2.0，外部 `xmoezzz/eluna`）：实验后端。
> - **实测游戏脚本**：甜蜜女友3 `system/image/emote.lua`（经 `artc` 从
>   root.pfs 提取到临时目录审阅；游戏资产不进入仓库）。
>
> **完成定义（DoD，所有 WP 通用）**：
> 1. `build-test` 全绿 + 新增回归；2. `build-mac` / `build-android` 编译通过；
> 3. 不改变既有 Lua 行为（除本 WP 声明的）；4. 契约来源写进代码注释；
> 5. 涉及渲染的项在 mac 宿主（有真机则加真机）冒烟并记录。
> 验证命令见主计划 §E14。

## 0. 批次划分

| 批次 | 工作包 | 前置条件 |
|---|---|---|
| **P0（先做，证据已闭环）** | WP0 Artemis Lua 契约对齐（单位/签名/表参数/新方法） | 无需新素材 |
| **A（立即可做，合成夹具可闭环）** | WP1 自动眨眼、WP2 选择器、WP3 attrcomp、WP4 模型镜像位、WP6 PSB 加固、WP7 图集裁切 | 无 |
| **B（需真实模型/真机确认）** | WP8 sync/skipToSync、WP9 命中测试、WP5 帧曲线与颜色、WP10 网格扩展 | 样例模型 + 实测游戏 |
| **C（按需）** | WP11 serialize、物理（主计划 E5） | 游戏需求证据 |

映射：WP0 → E3/E10；WP1/WP2/WP3 → E10；WP4 → E11；WP5/WP10 → E9；WP6 → E6；
WP7 → E8；WP8 → E15；WP9 → E16；WP11 → E17。

## 进度

| WP | 状态 | 备注 |
|---|---|---|
| WP0 | **已落地（合成回归通过）** | 帧单位、setCoord/setScale/表参数/progress 选项/新 knobs/setColor RGBA；PlayTimeline flags 留给 E3；真机回归留 E14 |
| WP1 自动眨眼 | **已落地** | 40/20/40 相位模型、等待不计动画、重载重置 |
| WP2 选择器 | **已落地** | 交叉淡化公式 + 渲染断言 + selector 变量可写 |
| WP3 attrcomp | **已落地** | value<=0 移除、Validate/Evaluate 同步跳过 |
| WP4 模型镜像位 | **已落地** | metadata.mirror 默认生效，Lua 可覆盖 |
| WP6 PSB 加固 | **已落地** | v2 body 解密、宿主 seed 钩子、lzfs LZ4 帧（独立块） |
| WP7 图集裁切 | **已落地** | win/common 共享图集 + left/top 裁切 |
| WP11/E11 颜色 tint | **已落地** | MODULATE2X（0x80 中性）→ colormultiply |
| WP5 曲线/帧色 | **帧色已落地**：`color`（MODULATE2X，2× 折算后与播放器 tint 相乘）；曲线字段仍只接受不应用（真实模型无 `zcc/ccc/cc` 命中） | |
| WP8 sync/skipToSync | **部分**：推导 + C++ SkipToSync + 回归；Lua 注册待真机证据 | |
| WP9 命中测试 | **已落地** | shape/blank/clip 源、rect/circle/point/quad、contains/hitTest |
| WP10 网格细分 | **部分**：细分比率接通网格输出（默认 1.0 不变）；priority 已落地（时间变序重排）；meshSync 未做 | |
| E1 契约文档 | **完成**：`docs/emote-driver-contract.md` | |
| E5/E12/E13/E17 | **决策完成**（见主计划「落地状态」） | |
| E14 真机冒烟 | **部分完成**：host 冒烟（常轨脱离/甜蜜女友3）通过；macOS CGL GL 回归（`ARTC_TEST_CGL`）通过；**TyranorNext 实机立绘已渲染**（`libartemis-clean.so` 17 版） | |
| 真实模型对齐（甜蜜女友3） | **已落地**：win-split 内容（src/icon）、motion 级 parameterize、stencil 子树、深度忽略、空 mesh 占位、blank `w:h:ox:oy`、per-frame color、priority、`bm` 混合、盒中心原点、HOLD 子节点；合成回归覆盖 | |
| E8 桌面贴图格式 | **已落地**：DXT1/DXT3/16bit/A8L8/RGBX8 + mip 容错（合成回归） | |
| 实机（TyranorNext） | **已落地**：`Animated::Finish` 归零修复（立绘可见性的主因）、inheritMask 放宽、bm 混合、盒中心偏移、stencil 子树/HOLD 递归；stencil mask 合成为剩余主项 | |

---

## WP0 — Artemis Lua 契约对齐（P0，先于一切）

**目标**：把 Lua 代理的签名、单位与参数形式对齐真实 Artemis 游戏脚本与
`art3m1s-core` 的引擎绑定；这是"实测游戏能正确显示"的前置条件。

### 证据（甜蜜女友3 `system/image/emote.lua`，共 1605 行）

- `ex.frametime = 16.666` —— 引擎 E-mote 为 60fps；
  **所有 transition/fade/progress 参数单位是「帧」，不是毫秒**：
  - `local fr = p.frame or p.time or init.fg_fade; if fr > 0 then fr = floor(fr / ex.frametime) end; em:setVariable(k, v, fr, es)`
  - `local tm = ...; tm = floor(tm / ex.frametime); em:fadeOutTimeline(as, tm, 0); em:fadeInTimeline(alw, tm, 0)`
  - `fr = es * 0.06  -- player->Progress(elapse * (60.0f / 1000.0f)); em:progress(fr)`
  - `em:setColor(no, 0, 0)`（颜色 + 帧 + ease）
- 签名（与 `art3m1s-core` 绑定一致）：
  - `em:setScale(sc, 0, 0)` → `setScale(scale, origin_x, origin_y)`（缩放 + 枢轴，
    **不是** time/ease）
  - `em:setCoord(sx, sy, 0, 0)` → `setCoord(x, y, z, angle)`（**不是** time/ease）
  - `em:setVariable(label, value, frames, easing)`、`fadeIn/fadeOutTimeline(label, frames, easing)`
  - `em:playTimeline(label, 1)` —— flags bit0=1 表示"保留现有时间轴"；0 时参考实现
    会先清空全部时间轴（`art3m1s-core play_model_timeline`）
  - `em:getVariable(label)`、`em:isTimelinePlaying(label)`、`em:stopTimeline(label)`、
    `em:pass()/step()/skip()`、`em:getEmoteVersion()`
  - `e:getEmoteLayer{ id=(id..".0"), next=true }` —— **表参数**（兼容现有字符串形式）
  - `e:createEmoteLayer{ id, files={px}, width, height, progress=(bool) }` ——
    `progress=true` = 引擎自动推进（脚本不调 `progress()`）；`false` = 脚本手动推进。
    `ex.progressos = { windows=true, ps4=true, switch=true }`（**android 未列 → 引擎自动**）
- 甜蜜女友3 还使用本项目没有注册的方法：
  - `em:setMeshDivisionRatio(ratio)` —— 网格细分倍率（性能档位 1.0/0.8/0.4…）
  - `em:setHairScale(scale)` / `em:setBustScale(scale)` —— 摇动幅度缩放（物理相关）

### 现状与差距

- `m_setVariable/m_setColor/m_fadeIn/m_fadeOut/m_progress` 把帧当毫秒 →
  过渡快 ~16.7 倍（`setVariable(fr=20)` 应为 333ms，当前 20ms）；
- `m_setScale` 把 (origin_x, origin_y) 当 (time, ease)；
- `m_setCoord` 把 (z, angle) 当 (time, ease)，旋转被丢弃；
- `l_getEmoteLayer` 只接受字符串，游戏用表 `{id=, next=}`；
- 未注册 `setMeshDivisionRatio/setHairScale/setBustScale`（走 stub）；
- `createEmoteLayer{progress=}` 未解析（Android 恰好与自动推进一致，但语义要落实）。

### 设计

1. **单位边界**：Lua 层把帧 → 毫秒（`frames * 1000.0/60.0`）后再调 C++；
   C++ API 保持毫秒（既有测试不动）。`progress(frames)` 同法换算。
2. **setScale**：改为 `(scale, origin_x, origin_y)`；`EmotePlayer` 增加枢轴字段，
   渲染时通过容器变换表达（`translate(-ox,-oy)`，与
   `art3m1s-core` 的 `Affine2::from_translation(-origin)` 等价）。
   旧的 4 参 per-axis 扩展如需保留，放到独立方法名或按参数个数严格区分
   （不得再与官方 3 参冲突）。
3. **setCoord**：改为 `(x, y, z, angle)`；z 在 2D 合成路径忽略（记录一次），
   angle 走现有旋转。`setRotate` 作为本项目扩展保留。
4. **getEmoteLayer**：同时接受字符串与 `{id=…, next=…}`；`next=true` 先返回同一
   实例（Artemis 的 pending/promotion 语义单列，见风险）。
5. **新方法**：注册 `setMeshDivisionRatio/setHairScale/setBustScale`；
   一期为"接受 + 参数校验 + 记录一次日志"（不假装物理/细分），二期再实现。
6. **progress 选项**：`EmotePlayer::SetAutoProgress(bool)`；`UpdateEmotes` 仅对
   auto-progress 的实例做墙钟推进；`progress=false` 的实例只响应显式
   `progress()`/`step()`（Windows/PS4/Switch 脚本路径）。Android 默认 true。
7. `setColor` 字节序待验证：游戏构造 `0xRRGGBBff`（RGB+不透明 alpha），与
   DLL 初始化值 `0x808080FF` 一致指向 **0xRRGGBBAA** 而非当前假设的
   0xAARRGGBB；用真机灰度/colortone 场景验证后修正。

### 实施步骤

1. 先加 Lua 端回归：把提取出的游戏调用序列写成合成脚本断言（不进游戏资产）。
2. 单位换算（setVariable/setColor/fadeIn/fadeOut/progress）。
3. setScale/setCoord 语义重构（含容器枢轴变换）+ mac 渲染冒烟。
4. getEmoteLayer 表参数；createEmoteLayer progress 解析 + auto-tick 开关。
5. 新方法注册（stub + 校验）；setColor 字节序验证与修正。

### 回归与验收

- `runtime_regressions`：新增"帧单位"用例（`setVariable(label, v, 60, 0)` 应产生
  ~1s 过渡）、表参数 `getEmoteLayer`、`progress` 选项停止自动推进、
  `setScale/setCoord` 参数不干扰过渡时间。
- `emote_regressions`：枢轴变换几何断言；帧→毫秒换算数值断言。
- 真机：甜蜜女友3 立绘缩放/位置/头部转身/淡入淡出速度与官方一致。

### 风险

- `next=true` 的 pending/promotion 语义（转场时旧实例保留、新实例接管）需要
  与 `trans` 联调；一期可先返回现有实例并记录日志，不改变现有替换行为。
- setColor 字节序与 alpha 语义未闭环前不要改动画笔预设值。

---

## WP1 — 自动眨眼（eyeControl）

**目标**：解析模型眨眼控制并自动播放，且**不影响动画结束判定**。

### 现状与证据

- `src/render/emote_model.cpp` 只解析 `variableList` / `instantVariableList` /
  `timelineControl`；眨眼、选择器、attrcomp 均未解析。
- krkrsdl3 `updateEyeControl`（较简）：随机等待 → 三角波闭开 → 恢复 `baseVal`。
- **art3m1s-core（Artemis 生产宿主，优先参照）** `EmoteEyeBlink`：
  - 解析 `enabled`/`blinkEnabled`（缺省 1）、`blinkFrameCount(max(1))`、
    `blinkIntervalMin/Max(max(0))`、`beginFrame`/`endFrame`；另有无运行时用途的
    `edge`/`node` 字段；
  - 状态机 `Idle → Closing → ClosedHold → Opening → Idle`，按原注释：
    **native EPEyeControl 相位为 40% 闭 / 20% 保持 / 40% 睁，运动相位速度
    2.5×（span*2.5/blinkFrameCount）**；
  - `apply(vars)`：仅当变量当前值落在 `[begin,end]` 时，
    `value' = base + (end - base) * amount`，`amount = (blink_frame - begin)/span`；
  - 等待时间：LCG（1664525/1013904223）在 `[min,max]` 上取值；
  - 大步长时要跨相位继续推进，避免眼睛卡在半闭。
- krkrsdl3 的真实事故：带眨眼/时间线控制的立绘若让 `animating` 恒真，等待
  动画结束的对话流程会永久挂起（猫娘乐园2）。

### 设计

- `EmoteModel`：
  ```cpp
  struct EmoteBlink { std::string variable;
                      double begin=0, end=0, frames=1;
                      double interval_min=0, interval_max=0;
                      bool enabled=true, blink_enabled=true; };
  std::vector<EmoteBlink> blinks_;           // Blinks()
  ```
  解析 `metadata.eyeControl[]`；校验 `label` 存在于 `variables_`（否则加载失败，
  保持"显式拒绝"风格）。
- `EmotePlayer` 每眨眼条目运行态 `{wait, blink_frame, phase, rng}`（`Load` 重建）；
  按 art3m1s-core 的相位/速度模型推进（tick=frames，ms 经
  `kFramesPerMillisecond` 换算）。
- 应用顺序对齐 art3m1s-core：时间轴采样 → 用户变量覆盖 → 眨眼最后应用；
  当前 `ComposeVariables` 以 `variables_` 为基准再加时间轴混合，接入时需保持
  既有 mixer 回归不变（新值只影响眨眼变量）。
- `IsAnimating()` **不含**眨眼（等待中的眨眼不算动画，避免挂起）。
- 随机：`std::mt19937`；测试用 `interval_min==interval_max` 保证确定性。

### 实施步骤

1. `emote_model.h/.cpp` 增加结构、解析、访问器与校验。
2. `emote_player.h/.cpp` 增加运行态、`Progress` 推进、`ComposeVariables` 合并。
3. 夹具：`tests/emote_scene_fixture.h` 新增 `BlinkDocument()`
   （`Scene()` + `eyeControl{expression, 0, 10, 4, 10, 10}`）。
4. 回归：`emote_regressions.cpp` 新增 PlayerTests 段。
5. 注释记录契约来源（krkrsdl3 `updateEyeControl` + DLL `blink*` 字符串）。

### 回归夹具与断言

- 等待期（未到 10 帧）：渲染后 `expression` 仍为 0（face 图标）。
- 第 10+2 帧（blink 中点）：`expression`=10（wide 图标），用现有 `picture_sum`
  断言宽度。
- `blinkFrameCount` 结束后：回到 base 值；再次等待。
- `Progress(5000)` 后 `IsAnimating()==false`（等待中的眨眼不得挂起动画判定）。

### 验收标准

- 合成夹具全过；真机立绘自然眨眼、对话推进不挂起、`[wait]` 不超时。

### 风险

- 眨眼变量若同时被时间轴/用户设置驱动，覆盖顺序需按参考实现（眨眼先、时间轴后）；
  用户 `setVariable` 为最高优先（写在 `variables_` 层）。

---

## WP2 — 选择器（selectorControl）

**目标**：支持多选项切换变量组，且时间轴不覆盖选择器接管的变量。

### 现状与证据

- krkrsdl3 `emoteselect`：`selectorControl[].{label, optionList[].{label,
  offValue, onValue}}`；`selectValue(opt)` 选中项写 `onValue`、其余写
  `offValue`（**二值切换**）；加载时先 `selectValue(0)`；`updateTimelineControl`
  跳过所有 selector item 变量。
- **art3m1s-core（Artemis 生产宿主，优先参照）** `EmoteSelectorControl`：
  - 解析 `enabled`（缺省 1）与 `optionList`；
  - `apply(selector_value, vars)`：`opt` 夹到 `[0, n-1]`，每个 option 取
    `distance = min(|opt - index|, 1)`，写
    `on + (off - on) * distance` —— **相邻选项线性交叉淡化**（选中项精确为
    `onValue`，相邻项部分混合），而不是二值切换。
- 本项目未解析该元数据；`setVariable` 对未知变量返回 false。

### 设计

- `EmoteModel`：
  ```cpp
  struct EmoteSelectorItem { std::string label; double on=1, off=0; };
  struct EmoteSelector { std::string label; std::vector<EmoteSelectorItem> items;
                         bool enabled=true; };
  std::vector<EmoteSelector> selectors_;
  ```
  `variables_` 同时纳入 selector 自身 label（"虚拟变量"，值为当前选项）与所有
  item label（供 `SetVariable` 校验与时间轴跳过判定）。
- `EmotePlayer::SetVariable`：命中 selector label 时保存浮点选项值并按下发公式
  计算每个 item 的目标：优先采用 art3m1s-core 的交叉淡化（
  `on + (off-on)*min(|v-i|,1)`，支持小数/过渡）；krkrsdl3 的二值切换作为
  退化情况（整数取值）。越界夹取（对齐 art3m1s-core）而不是报错。
- `GetVariable(selector label)` 返回当前选项值（默认 0）。
- `ComposeVariables()`：时间轴采样跳过 label 属于任一 selector item 的轨道。
- Lua 无需新方法（`setVariable`/`getVariable` 覆盖）。

### 实施步骤

1. 模型解析 + 校验（`optionList` 空、label 重复、item label 不存在 → 加载失败）。
2. `EmotePlayer` 路由与状态；加载时执行 `select(0)` 初始化。
3. 夹具 `SelectorDocument()`：`Scene()` + `selectorControl{label='clothes',
   enabled=1, optionList=[{expression,on=10,off=0},{partner,on=10,off=0}]}`
   （partner 加入 `variableList`）。
4. 回归：`emote_regressions.cpp` + `runtime_regressions.cpp`（Lua
   `setVariable('clothes',0/0.5/1)` 后渲染/`getVariable` 断言交叉淡化数值）。
5. 时间轴跳过：用 `PlayerDocument()` + selector 包住 `expression`，断言时间轴
   播放不覆盖选择结果。

### 验收标准

- 夹具通过；真机服装/表情选项切换与官方一致；`setVariable(selector)` 越界报错。

### 风险

- "selector 变量被时间轴跳过"是参考实现的选择，需真机确认（若某游戏时间轴
  确实驱动 selector item，需要加白名单）。

---

## WP3 — attrcomp 节点移除

**目标**：按模型规则在加载期隐藏节点，与官方一致。

### 现状与证据

- krkrsdl3 `emoteattrcomp`：`metadata.attrcomp[].{label, data.remove[].{value,
  id:{chara, motion, layer}}}`；`value<=0` 时在加载期把目标节点标记
  `removed=true`（不渲染）。
- 本项目不解析该元数据 → 本应隐藏的层会显示。

### 设计

- `EmoteModel`：
  ```cpp
  struct EmoteRemoveRule { std::string chara, motion, layer; double value; };
  std::vector<EmoteRemoveRule> removals_;   // 仅收录 value<=0 的规则
  ```
- `EmoteScene::Evaluate` / `Validate`：遍历节点时对 `(chara, motion, node.label)`
  命中移除规则的节点直接跳过（含其子树）。被跳过节点内的不支持字段不得导致
  加载失败（Validate 同步跳过）。
- `value>0` 的规则：记录一次 `OutputLog` 并忽略（保持显式而非猜测）。

### 实施步骤

1. 模型解析；`EmoteScene::Load` 构建按 motion 索引的移除集合。
2. `Validate`/`Nodes` 跳过逻辑（两处必须一致，避免"能加载不能渲染"）。
3. 夹具 `AttrcompDocument()`：`Scene()` + `attrcomp` 移除 face 子节点。
4. 回归：断言层数/图标宽度变化；被移除节点带未知 content 字段仍可加载。

### 验收标准

- 夹具通过；真机隐藏层不出现。

### 风险

- 规则里的 `layer` 是节点 label；确认与 PSB 的 label 完全一致（大小写/空格），
  必要时先做 trim 兼容。

---

## WP4 — 模型镜像位（metadata.mirror）

**目标**：模型自带的镜像标志自动生效，Lua `setMirror` 仍可覆盖。

### 现状与证据

- krkrsdl3：`metadata.mirror==1` → `isMirror`，渲染矩阵乘 `scale(-1,1,1)`。
- 本项目只支持 Lua `setMirror`，不读元数据（`grep mirror src/render` 仅见
  玩家侧）。

### 设计

- `EmoteModel`：`bool mirror_` + `Mirror()`；解析 `metadata.mirror`（数值 1）。
- `EmotePlayer::Load`：`mirror_ = model->Mirror()`（失败重载时随新模型重置）。
- 渲染路径复用现有 `reversex` 容器属性，无需新代码。

### 实施步骤与回归

1. 模型解析与访问器；2. 夹具 `MirrorDocument()`；
3. 断言 `Render` 后容器层 `Layer::reverse_x==true`；`SetMirror(false)` 可覆盖。

### 验收标准

- 夹具通过；真机镜像模型朝向正确。

---

## WP5 — 帧曲线（zcc/ccc/cc）与 per-frame color（调查后实现）

**目标**：不因新格式字段拒绝加载；确认语义后应用数值。

### 现状与证据

- krkrsdl3 `emoteframe`：`zcc/ccc/cc`（缩放/坐标/网格曲线：2 系数 + 4+4 控制点），
  per-frame `color`（缺省 `0xff808080`，`hasColor` 区分"未写"）。
- **art3m1s-core**：已实现帧曲线求值（`render.rs` `curve_ratio(...)`，含
  coord/angle/zoom 通道插值）与 per-frame 颜色规范
  （`color_component` 支持 0..255 或 0..1，帧色缺省不乘）；其已知缺口里
  没有曲线，说明曲线路径已在真实游戏（NekoMiko）上验证。
- 本项目 `Validate` 的 supported 集合不含这些字段 → 直接抛
  `unsupported E-mote content`。

### 设计（分两步）

1. **兼容不拒绝**：解析字段（结构化保存），Validate 白名单；求值暂按"无曲线"
   处理，`OutputLog` 首见提示一次。
2. **应用数值**：以 art3m1s-core 的曲线/颜色实现为语义参照（曲线对
   coord/angle/zoom 的通道插值、帧色乘算），本项目实现并加数值单测；
   `color` 接 `colormultiply`（主计划 E11；帧色缺省不乘）。

### 实施步骤

1. 逐字段确认：先用真实模型统计哪些字段实际出现（决定优先级）。
2. 第 1 步落地 + 夹具 `CurveDocument()`（含三类曲线与 color）；
3. 第 2 步落地 + 数值断言（曲线端点/中点、color 乘法）。

### 验收标准

- 含曲线模型可加载；数值与参考实现一致（对照脚本或截图）。

### 风险

- 曲线语义属较新 PSB 版本，未见 Artemis 实测游戏使用；不要在无证据时投入
  第 2 步。

---

## WP6 — PSB 加固：seed 钩子 / v2 body / lzfs

**目标**：覆盖官方驱动的整包加密路径与 LZ4 容器（不含 `EmoteFilterTexture`
固定 key 的"强制解密"入口，后者单独评估）。

### 现状与证据

- `src/pack/psb.cpp`：仅解头部（`[8, header_len)`），seed 由 adler32 推导或
  `ARTC_EMOTE_SEED` 覆盖；PSB v2 无校验，失败信息不区分阶段。
- krkrsdl3 `emotefile::load`：
  - key = `{0x075BCD15, 0x159A55E5, 0x1F123BB5, seed}`；
  - 头部按固定字段顺序解密（v3+ checksum、v4+ extra 三个字段）；
  - **v2 额外解密 `[offsetEncrypt, offsetChunkOffsets)`**；
  - 宿主可用 `setEmotePSBDecryptSeed` / `setEmotePSBDecryptFunc(buffer,len)`；
  - `lzfs`（magic `04 22 4D 18`）LZ4 帧容器（块大小/独立块/块校验/内容长度/字典标志）。

### 设计

- 解密 seed 优先级：**显式宿主钩子 > `ARTC_EMOTE_SEED` > 头部推导**。
  宿主钩子：`DecodePsb(bytes, doc, error, const PsbDecryptOptions* = nullptr)`
  或 `PackManager::SetPsbDecryptHook(std::function<bool(std::vector<uint8_t>&)>)`；
  先内部 API + CLI/env，Lua 暴露不在本期。
- v2：解析头后按 krkr 顺序继续解密 body 区间；`checksum_ok` 对 v2 恒真，
  因此 v2 的"是否加密"用 `encryption_flags&1` + 解密后可解析性判定。
- LZ4：实现最小 `lzfs` 帧解压（仅独立块 + 块校验），或引入许可兼容的单文件
  实现；先支持必要特性，遇到字典/链接块明确报错。
- 错误分级：`"encrypted PSB: header key mismatch"` / `"... body decrypt failed"` /
  `"unknown PSB container"`。

### 实施步骤

1. `psb.cpp` 重构解密流程为"字段顺序表 + 可选 body 区间"，保持现有 v3/v4
   行为不变（先补回归再改）。
2. 增加 v2 分支与合成夹具（v2 头 + body 加密）。
3. 宿主钩子 API + `ARTC_EMOTE_SEED` 优先级测试。
4. `lzfs` 解压 + 合成 LZ4 帧夹具（手工构造小帧）。
5. `runtime_regressions.cpp`：加密 v2 模型经 `createEmoteLayer` 全链路。

### 验收标准

- `psb_encrypted_regressions` 全绿（v2/v3/v4、seed、坏流拒绝）；
- 真实加密模型可加载；README 能力表更新。

### 风险

- `EmoteFilterTexture` 的固定 key `851083516` 是"驱动自带强制解密"路径，是否
  需要由实测游戏决定；不要与 seed 路径混用。

---

## WP7 — 图集裁切（win/common spec）

**目标**：`win`/`common` 模型的共享图集 + 图标裁切可正确解码。

### 现状与证据

- krkrsdl3：非 krkr 时 `source.<name>.texture{width,height,type,pixel}` 是共享
  图集；每个 icon `{left,top,width,height,originX,originY}` 是裁切矩形；
  `readIconTobuffer` 从图集 `(left,top)` 起按行拷贝 `width×height`。
  （图标另有 `clip{left,top,right,bottom}` 字段，krkrsdl3 解析但未使用，语义待确认。）
- 本项目 `EmoteModel::Image` 只读 `icon.pixel` 并整图解析 → `win/common` 报
  `missing E-mote pixel resource` 或长度不符。

### 设计

- `Image()` 分支：
  - `krkr`：现状（每 icon 自带 pixel/pal/compress）。
  - `win`/`common`：读 `source.texture`（与 icon 同层）得到图集参数，裁切
    `left/top/width/height`；颜色序沿用现规则（win 交换 R/B，common 不换）。
- `EmoteImage` 保持输出 `width/height/origin_x/origin_y/rgba`，上层无感。

### 实施步骤与回归

1. 夹具 `AtlasDocument()`：2×2 图集 + 1×1 图标（left/top=1,0 等）验证裁切。
2. 断言 RGBA 像素、尺寸、origin；破坏性用例（越界 left/top 拒绝）。
3. 真机 `common`/`win` 模型冒烟。

### 验收标准

- 夹具通过；真实模型不再报 missing resource。

---

## WP8 — sync / skipToSync（验证优先）

**目标**：先建立 sync 事实，再决定是否接入结束判定。

### 现状与证据

- krkrsdl3：`syncTime` = 非 parameterize 节点最大内容帧（递归子 motion）；
  `selfSyncTime` 文件层；结束判定 `syncTime → selfSyncTime → lastTime`；
  `skipToSync()` 把时间跳到 `getSyncTime()`。
- 本项目：基础 motion 常驻推进，无 `play(motion)`/sync 概念；时间轴结束按
  `last_time`/`loop_end`。

### 设计（两步，默认只做第 1 步）

1. `EmoteModel` 计算并暴露 `SyncFrame(chara, motion)`（推导规则同上），
   仅在诊断日志使用，不改行为；增加 `EmotePlayer::SkipToSync()`（把
   `base_frame_` 跳到 sync 帧）但 **Lua 暂不注册**。
2. 真机确认 Artemis 游戏是否存在"等待立绘动画结束/跳到同步点"的流程；有则
   接入结束判定并注册 `skipToSync`，无则保持冻结。

### 回归与验收

- 合成 motion 推导 sync 值断言（含子 motion 递归、parameterize 排除）。
- 验收：第 1 步只要求数值正确；第 2 步需真机流程证据。

### 风险

- 该语义来自 KiriKiri 侧插件，Artemis 引擎插件是否采用未证实；**不得**先改
  结束判定。

---

## WP9 — 命中测试：shape/blank 源

**目标**：Lua `contains(label?, x, y)` / `hitTest(label?, x, y)` 与官方一致。

### 现状与证据

- krkrsdl3：`shape/<rect|circle|point|quad>`（单位方格 16×16，乘 zx/zy）、
  `blank:w:h:ox:oy`；`EmoteHitFrame::contains` 用网格三角形判定并按目标裁切；
  `contains` 保留旧仿射坐标，`hitTest` 用目标像素坐标。
- 本项目：`src` 仅接受 `src/…`/`motion/…`/`layout`，其余抛
  `unsupported E-mote source`；proxy 不注册 `contains`/`hitTest`（logging stub）。

### 设计

- `EmoteSceneLayer` 增加 `enum ShapeKind {None, Rect, Circle, Point, Quad}`、
  `shape` 字段与几何（顶点/半径/矩形）；`Evaluate` 收集 shape/blank 层
  （不绘制，`visible=false` 或单独输出）。
- `EmotePlayer::Contains(label?, x, y)`：无 label 时遍历全部 shape；有 label
  时命中该层。坐标空间=合成器 stage 像素（与输入层一致）。
- Lua：`contains([label,] x, y)` / `hitTest([label,] x, y)` 两种重载，
  返回 boolean。

### 实施步骤与回归

1. 场景解析 shape/blank（Validate 同步放开）；
2. 夹具 `ShapeDocument()`：rect/circle/point/quad 各一，含缩放/旋转；
3. 断言：中心/边/外点、旋转后判定、blank 尺寸/原点。
4. 真机点击判定冒烟。

### 验收标准

- 夹具通过；真机点击与官方一致（坐标对齐由引擎输入层验证）。

---

## WP10 — 网格扩展（调查）

**目标**：确认是否需要 `meshDivision` / `meshTransform` / `meshSyncChildMask`。

### 现状与证据

- krkrsdl3：节点带 `meshDivision`（运行时 CPU 细分缓存，`_meshDivX/Y`）、
  `meshCombine/meshTransform/meshSyncChildMask`；`priority` 列表重排 nodeList。
- **art3m1s-core**（语义参照最全，含 native 符号注释）：
  - `inheritMask`/`inheritParent` 走查、`motionIndependentLayerInherit`、
    `meshSyncChildMask` 低 3 位的 coord/angle/zoom 通道
    （注释标注 native `sub_10335500` 语义）；
  - `priority` 是**按时间变化**的排序表（`motion.priority_at(time)`，帧
    `content` 数组给出 rank），不是静态列表；
  - `stencilType & 0x4` 才启用 `stencilCompositeMaskLayerList`；节点 `type==1`
    为 shape 判定层；`bm` 每帧混合模式（0=alpha/1=add/2,5=rev-sub/3=multiply/4=screen）。
- 本项目：固定 8×8 warped grid；不解析 priority/细分/混合模式；`bm!=0` 直接拒绝。

### 步骤

1. 真实模型统计字段出现率与观感影响（截图对比）；甜蜜女友3 脚本已确认游戏侧
   会调 `setMeshDivisionRatio`（性能档位），优先支持"接受参数 + 可配细分"。
2. 若需要：`emote_mesh` 支持可配细分（沿用 8×8 的顶点/索引构建），
   `EmoteScene` 按节点传递；priority 先按 `priority_at` 求值顺序实现，再对照
   截图验证绘制 z 序；`bm` 分帧混合按 art3m1s-core 的映射表接入合成器。

### 验收标准

- 有明确结论（做/不做）与证据；做则对照截图通过。

---

## WP11 — serialize / 物理（按需）

见主计划 E17（存档恢复）与 E5（物理，默认 A 显式不支持）。启动条件：
实测游戏脚本确实调用对应的 Lua 方法/存档需要恢复立绘状态。

---

## 里程碑

| 里程碑 | 内容 | 出口条件 |
|---|---|---|
| M1 | WP1+WP2+WP3（立绘正确性三件套） | 三平台构建 + ctest 全绿 + 真机立绘冒烟 |
| M2 | WP4+WP6+WP7（元数据/加密/图集） | 真实模型与加密样本可加载 |
| M3 | WP5+WP9+WP8 验证（新格式/命中/sync 结论） | 每项有实现或冻结结论 |
| M4 | WP10+WP11（按需） | 游戏需求证据驱动 |

## 风险总表

- **WP0**：单位为帧、签名按真实脚本/`art3m1s-core` 绑定对齐；`setColor` 字节序
  与 `getEmoteLayer{next}` 的转场语义未闭环前只做兼容读取，不改现有替换行为。
- **WP1**：眨眼与 `IsAnimating` 的耦合（参考实现出过挂起事故）——必须加
  "等待期不忙"回归。
- **WP2/WP3**：交叉淡化/隐藏语义来自 Artemis 侧参考实现，先按本文实现并用真机
  复核；发现冲突时以实测游戏为准。
- **WP6/WP7**：依赖样例文件；先收集加密/win-spec 模型再开工。
- **WP8/WP10**：语义未证实，禁止先改行为。
- 所有 WP 完成后同步更新 `docs/emote-plan.md` 状态与 README 能力表。

---

## 附录 A — 参考实现与证据来源

> 只做行为/接口核对，不复制代码；游戏资产与提取物不进仓库。

| 来源 | 路径 | 用途 | 已知缺口（同样值得参考优先级） |
|---|---|---|---|
| **Artemis 生产宿主** | `/Users/weiss/github- engine/art3m1s-core` | Lua 契约（`crates/asb-interpreter/src/lua_engine.rs`）、眨眼/选择器/diff 时机（`crates/art3m1s-emote`）、native 语义注释（`render.rs`，含 `sub_10335500` 等） | `pass/step/skip` 只记录不改变采样；stencil wipe/粒子/相机层；外部纹理 PSB；非 DXT5 图集；`cc` 网格 |
| **KiriKiri 实现** | `/Users/weiss/github- engine/krkrsdl3/plugins/emoteplayer` | 自动眨眼（较简）、选择器二值、attrcomp、sync、PSB seed/回调/v2 body、LZ4、图集裁切、网格细分 | 物理空实现；fade/blendRatio/pass/skip 为 TODO；插值无 easing；仅 RL/none + RGBA8 |
| **Eluna**（实验后端） | `art3m1s-core/crates/eluna`（外部 `xmoezzz/eluna`，MPL-2.0） | 第三方 PSB/渲染原语交叉验证 | 实验性 |
| **官方驱动** | `~/Desktop/emote-test/emotedriver.dll` | 18 项 native 方法契约（主计划附录 A/B/C） | x86 D3D9 闭源，仅本地分析 |
| **实测游戏脚本** | 甜蜜女友3 `system/image/emote.lua`（root.pfs 内，本机提取审阅） | WP0 单位/签名/方法面；眨眼/选择器等引用需求 | 资产不提交仓库 |

复现游戏脚本提取（只读，输出到临时目录）：

```bash
./build-host/artc list  "<游戏>/root.pfs" | grep -i emote
./build-host/artc ini   "<游戏>/root.pfs" "system\image\emote.lua" > /tmp/emote_scan/game_emote.lua
```

`art3m1s-core` 关键位置速查：

- 绑定：`crates/asb-interpreter/src/lua_engine.rs`（`EmoteLayerCommand`、`EmoteLayerApi`、
  `createEmoteLayer`/`getEmoteLayer`）
- 播放器：`crates/art3m1s-emote/src/player.rs`（`set_scale`/`set_coord`/`set_variable`/
  `play_model_timeline`/diff 时间轴时机）
- 时间轴与选择器/眨眼参数：`crates/art3m1s-emote/src/timeline.rs`
- 眨眼状态机：`src/runtime/emote.rs`（`EmoteEyeBlink`）
- 渲染语义：`crates/art3m1s-emote/src/render.rs`（inherit/meshSync/priority/曲线/混合）
