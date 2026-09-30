# E-mote 官方驱动契约（emotedriver.dll）

> 静态分析笔记，供 artemis-compat 的 E-mote 兼容实现与回归设计使用。
> 仅为行为/接口事实，不含第三方代码；分析对象是本机用于兼容研究的官方
> `emotedriver.dll`（PE32 x86，2018-12-21，MSVC，D3D9/D3DX9）。
> 复现命令见文末；本机分析产物在 `/tmp/emote_*.txt`（临时）。

## 1. 二进制概览

| 项 | 值 |
|---|---|
| 格式 | PE32 DLL（x86），GUI 子系统，依赖 `d3dx9_43.dll` |
| 导出 | `EmoteCreate`、`EmoteCheckValidObject`、`EmoteFilterTexture`、两张接口 vtable 符号 |
| 基址/版本 | `0x10000000`；MSVC 2018 工具链（具体版本未记） |
| RTTI 类名 | `PEmoteDevice/MEmoteDevice`、`PEmotePlayer/MEmotePlayer`、`AEmotePlayer/AMotionPlayer`、`EP*Control@emote`（Eye/Bust/Wind/Selector/Transition/Graph/Pend…）、`PSBFilter/StructCryptFilter`、`PSBObject/APSBObject` |

### 导出入口

| 符号（demangle 后） | 地址 | 说明 |
|---|---|---|
| `IEmoteDevice* EmoteCreate(const IEmoteDevice::InitParam&)` | `0x100038d0` | 构造 `PEmoteDevice`；`InitParam` 至少 12 字节：`{host 接口指针, alloc(size), free(ptr)}`（alloc/free 存全局 `0x101064c4/c8`） |
| `bool EmoteCheckValidObject(const uint8_t*, size_t)` | `0x10003ed0` | 用固定 key `851083516` + xorshift 解密并尝试解析，返回是否有效 |
| `void EmoteFilterTexture(uint8_t*, size_t, void(*)(uint8_t*,size_t))` | `0x10003920` | 构造 `PSBFilter/StructCryptFilter`，过滤后回调输出；key 同为 `851083516` |
| `??_7IEmoteDevice@@6B@` | `0x1008b898` | 接口基类 vtable（25 槽：dtor + purecall） |
| `??_7IEmotePlayer@@6B@` | `0x1008b900` | 接口基类 vtable（94 槽：dtor + purecall） |
| 实现类 vtable | `PEmoteDevice 0x1008bc08`、`PEmotePlayer 0x1008ba88`（95 槽） | 实际对象由工厂安装 |

### device 接口（`PEmoteDevice`，25 槽）

引用计数（slot 1/2 + refcount getter）、`InitParam` 拷贝 getter（slot 4：
`return this+0xc`）、若干开关 setter/getter（slot 5–24：`[device+8]` 即
`MMotionDevice` 上的 `0x270/0x274/0x2ec/0x2f4/0x2f0/0x448/0x444` 等字段）、
以及两个 `CreatePlayer` 风格工厂（`0x10001710`、`0x10001660`，分配 0x24 字节
并调用内部构造）。完整命名未恢复；调用面在 Artemis 引擎里由插件封装。

## 2. `IEmotePlayer` 方法契约（record/replay 格式串恢复，18 项）

驱动内建记录/回放：`player+0x1d8` 模式 0=normal / 1=record / 2=replay，
日志缓冲在 `+0x1dc`，解析器 `fcn.1001ae50`。因此下列**方法名与参数类型**
来自二进制中的格式串，逐字可信：

| 方法 | 参数 | 语义要点 | 实现地址 |
|---|---|---|---|
| `SetCoord` | `x, y, time_ms, ease` | 容器位移；`time` 与 `ease` 走内部 `fcn.10029f10`（`xmm2=time`、`xmm3=ease`） | `0x100140d0` |
| `SetScale` | `scale, time_ms, ease` | 单值等比；初始化路径 `SetScale(1,0,0)` | `0x10014280` |
| `SetRot` | `angle, time_ms, ease` | 旋转 | `0x10014410` |
| `SetColor` | `argb(int), time_ms, ease` | 乘色 + alpha；初始化 `0x808080FF`；format 串为 `%d` | `0x100145e0` |
| `SetGrayscale` | `value(0..1), time_ms, ease` | 亮度混合 | `0x100147e0` |
| `SetVariable` | `label, value, time_ms, ease` | 变量；两个日志点（timeline 内部也调用） | `0x1000bf30` / `0x1000c0e0` |
| `SetVariableDiff` | `label, pair, value, time_ms, ease` | 每个 (label) 一个 pair 控制、单一值；`GetVariableDiff(label,pair)`（`0x1000d980`）读回；`variableMatchList` 由 `0x10016390` 解析 | `0x1000d630` |
| `SetOuterForce` | `part("bust"/"hair"/"parts"), 4×float` | 外力/物理 | `0x10014ca0` |
| `SetOuterRot` | `3×float` | 外旋 | `0x10015290` |
| `PlayTimeline` | `label, flags` | bit0=1 保留现有播放（否则先 `StopTimeline(label)`）；bit1 触发逐层控制重置（`0x10018dd0`）；`FadeInTimeline` 内部用 flags=3 | `0x100171b0` |
| `StopTimeline` | `label` | | `0x10017500` |
| `SetTimelineBlendRatio` | `label, ratio, time_ms, ease, flags` | flags=1 → 比例到 0 后移除（FadeOut 用 `0,time,ease,1`） | `0x10017710` |
| `FadeInTimeline` | `label, time_ms, ease` | 未在播则 `PlayTimeline(label,3)`，再 `SetTimelineBlendRatio(label,0,0,0,0)` → `(1,time,ease,0)` | `0x10017a50` |
| `FadeOutTimeline` | `label, time_ms, ease` | 等价 `SetTimelineBlendRatio(label,0,time,ease,1)` | `0x10017bc0` |
| `Skip` | — | 控制过渡立即完成 + 整体重算 | `0x1000b730` |
| `Step` | — | 推进一次控制过渡 + 整体重算 | `0x1000b920` |
| `Pass` | — | 对标记轨道做 20 ms 淡出（`SetTimelineBlendRatio(...,0,20,0,1)`） | `0x1000bb10` |
| `CalcLayerFrameInfo` | `time` | 逐层帧信息（引擎合成用）；`0x1000b490` 为实际计算 | `0x1000b370` |

ease 权重公式（多处实现一致）：`ease >= 0 ? ease + 1 : 1 / (1 - ease)`。

### 断言与判定的实现细节（可复核）

- `PlayTimeline`：`al = flags & 1`；为 0 时先 `StopTimeline(label)`。
- `FadeInTimeline`：`fcn.100173f0` 判断在播，不在播则 `PlayTimeline(label,3)`。
- `SetTimelineBlendRatio` 的 flags=1 分支：blend 到 0 后条目被移除（项目同款语义）。
- `Pass`：遍历控制表，命中 `(flags>>1)&1` 且 `!(flags&4)` 的条目，调用
  `SetTimelineBlendRatio(label,0,20.0,0,1)` 并置位 `flags|=4`。
- `Skip`/`Step` 的区别仅在第一个控制动作（`0x10019660` vs `0x100197a0`），
  之后都会走同一套"整体重算"链（`0x10015540`、`0x1000e8c0`、层级更新…）。
- `CalcLayerFrameInfo`：`player+0x1d8==2`（replay）时先执行回放解释器，
  否则以传入 time 计算。

### 与 Artemis 引擎 Lua 契约的分层差异

驱动 native 方法与游戏脚本看到的 Artemis Lua API 不是一层（见
`docs/emote-detail-plan.md` WP0）：脚本侧 `setCoord(x,y,z,angle)`、
`setScale(scale,origin_x,origin_y)`，transition/fade/progress 以**帧**为单位；
native 侧对应 `time_ms`/`ease` 与单值 scale。本项目实现 Lua 面契约，
native 表仅用于语义核对。

## 3. 加密与容器

- 加密：xorshift 流密码，key = `{123456789, 362436069, 521288629, seed}`，
  与项目 `PsbCipher` 一致。
- 头部：字段按 `offsetEncrypt…offsetEntries` 顺序解密；v3+ 校验 adler32，
  v4+ 含 extra 三字段；v2 无校验且**额外解密 body 到 chunk 偏移表**
  （参考 `art3m1s-core`/`krkrsdl3` 的加载器）。
- `EmoteFilterTexture`/`EmoteCheckValidObject` 使用固定 key `851083516`
  的整包过滤路径（与头部 seed 推导路径不同，按需启用）。
- 容器：`MDF`(zlib) 与 `lzfs`（LZ4 帧）两种包装；项目两者均支持。

## 4. 未恢复项（后续可继续）

- `IEmotePlayer` 94 槽中除上述 18 项外的名称：getter、层信息、命中测试、
  `MMotionPlayer` 转发等（静态分析可按 vtable 槽逐一分类，但无 name 字符串）。
- `IEmoteDevice` 25 槽的完整语义（CreatePlayer 入参、配置位含义）。
- `CalcLayerFrameInfo` 输出结构（层数组布局）。
- `SetVariableDiff` 的 pair 解析细节（`variableMatchList` 运行时作用）。

## 5. 复现

```bash
rabin2 -I -s -E emotedriver.dll
rabin2 -z emotedriver.dll | grep -E 'Set|Fade|Play|Skip|Step|Pass|Calc'
r2 -q -e bin.relocs.apply=true -c 'aaa; axt 0x1008cf20; pdf @ 0x100140d0' emotedriver.dll
```

关键虚表：`IEmotePlayer 0x1008b900`（94 槽）、`PEmotePlayer 0x1008ba88`（95 槽）、
`IEmoteDevice 0x1008b898`、`PEmoteDevice 0x1008bc08`（25 槽）。
