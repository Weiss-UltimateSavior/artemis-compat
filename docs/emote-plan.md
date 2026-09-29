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
| E10 | 控制类元数据（眨眼/口型/选择器/orbit…） | P2 | 真实模型 | `*Control` 元数据键 |
| E11 | `SetColor` RGB tint（colormultiply） | P2 | 无 | 驱动 `SetColor %d %f %f` |
| E12 | `CalcLayerFrameInfo` 是否需要暴露 | P2 | E1 | `0x1000b370` |
| E13 | 驱动 record/replay 用作差分验证工具 | P3 | Windows 参照环境 | `0x1001ae50` 命令解析 |
| E14 | 实测游戏与 GL 冒烟矩阵（持续） | P0 | 真机 | AGENT.md §9 |

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

### 方案

1. 在 `compositor_regressions.cpp` 增加分数灰度用例（如 0.5，期望
   `mix(luma, c, 0.5)` 的通道值），归属 `ARTC_TEST_GLES` 构建。
2. mac 宿主 + 实测游戏做灰度过渡冒烟（记录前后帧）。

### 验收标准

- `ARTC_TEST_GLES=ON` 的 compositor 回归通过；mac 冒烟截图可辨。

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

### 验收标准

- 拆分模型与合并模型渲染一致；错误路径不破坏已有图层。

---

## E8 — 贴图格式补齐（P2）

### 问题与证据

- 驱动支持字符串：`A8L8`、`RGBA5650`、`RGBA5551`、`RGBA4444`、`RGBX8`、
  `DXT1`、`DXT3`、`ETC1`、`PVRTC_4BPP`、`PVRTC_2BPP`、`BC7`、`DXT5`，
  以及 `mipMap`/`mipMapLevel`。
- 项目仅 RGBA8/CI8/DXT5/BC7（`emote_model.cpp:78-115`）。

### 方案

按实测游戏命中顺序补：DXT1/DXT3（CPU 解块，现成风格）→ 16bit 格式 →
ETC1/PVRTC（出现在移动端图集时）→ mipmap 读取策略（当前直接报错或忽略）。
每种格式补 `block_decode` 风格合成回归。

### 验收标准

- 对应格式的裁剪图标像素与官方对照一致（色彩序/alpha 语义）。

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

### 验收标准

- 打开失败的模型；被修复项在 GL 下与官方对照；未修复项仍在加载期明确报错。

---

## E10 — 控制类元数据：眨眼/口型/选择器/orbit（P2）

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

### 验收标准

- 判定表齐全；被判定为运行时的项有实现或明确的排期。

---

## E11 — `SetColor` RGB tint（P2）

### 问题与证据

- 驱动 `SetColor %d %f %f` 会乘色（着色器有 `colorMultiply`）；
  项目 `EmotePlayer::SetColor` 只改 alpha（注释称"合成器没有 RGB tint"，
  但 `Compositor` 已有 `colormultiply` prop 与着色器 uniform）。

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
./build-host/artc drive <游戏目录>/root.pfs --frames 400
```

---

## 风险与未决

- **E2/E3 的推断语义**（差分符号、flags/Pass/Step 分层）是本计划最大的
  不确定性来源；在真机验证前不要对外宣称完全兼容。
- **E5 物理**投入产出比低且无官方公式文档，建议先维持显式不支持，按实测
  游戏需要再启动。
- **E6/E7 依赖样例文件**：没有真实加密/拆分模型时无法闭环，需在任务开始
  时先收集样本。
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
