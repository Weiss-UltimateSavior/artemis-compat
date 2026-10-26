# ARCV v2 活动动画 checkpoint 设计

## 目标

允许在普通语义场景存在活动 tween、循环 anime 或视觉 transition 时生成并恢复 ARCV v2 checkpoint。读档后，有限动画从原相位继续，无限动画不冻结、不从头重播；已完成有限 anime 保持末帧。修复必须位于通用引擎，不依赖具体游戏、图层 ID 或资源名。

## 兼容性

ARCV 版本号和现有 variables／flow／scopes／scene 四个 section 保持不变，只在 scene 中增加 `lytween` 与 `anime` 命令。旧引擎遇到这些命令会因未知场景命令而明确拒绝，不会静默丢弃动画。

现有 v2 文件不含这些命令，读取行为不变。视频、E-mote、mesh、pixel-only、自定义 affine、运行时蒙版继续明确拒绝。正在通过 tween set 组装但尚未提交的动画也拒绝，因为此时场景不是稳定语义状态。所有拒绝发生在写文件之前，旧档保持不变。

## Tween 模型

每个活动 tween 保存为一条 `lytween` 命令，包含：

- `id`、`param`、`from`、`to`；
- `time`、`delay`、`ease`；
- `repeat`／`yoyo` 对应的原始循环语义；
- 有符号相位时间 `elapsed = capture_now - start_ms`；顺序 tween set 中尚未到达的后续段允许为负；
- `started` 与 `from_current`，避免未开始动画提前固化起点，也避免已开始动画在读档后重新取错起点。

恢复时以恢复时钟减去 `elapsed` 重建 `start_ms`，保留原相位。有限动画按原方向到达既有 `FinalValue()`；无限及 yoyo 动画从原周期位置继续。先恢复所有图层，再恢复 tween，保证属性读取针对正确的语义图层。

## Anime 模型

每个 anime 状态保存 `init`、若干 `add` 和一个 `end` 命令，包含全部帧的文件、属性、帧时间、总时长、循环方式及 `elapsed`。文件仍通过现有 pack／save 资源验证。

恢复后重建有序帧表，并以恢复时钟减去 `elapsed` 设置动画起点，立即求值到当前帧。无限 anime 继续循环；已完成有限 anime 保持末帧，不再因残留在 `anime_` 而被误判为不可保存的活动状态。

## Transition 模型

视觉 transition 只涉及旧帧纹理、规则纹理与进度，不改变转场后的语义场景。checkpoint 保存转场后的图层，不保存旧帧像素或运行时蒙版；恢复时不再绘制旧帧 overlay。

runner 中已保存的剩余等待继续按原 deadline 恢复，但不得要求已不存在的视觉转场来释放等待。脚本在剩余等待结束后正常继续。

## 验证与原子性

读取阶段先验证全部动画命令：

- 数值必须有限，`time`、`delay` 和帧时间不得为负；`elapsed` 只要求有限并允许顺序段的负值；
- repeat／loop 不得低于 `-1`，索引和时间总和必须能安全表示；
- tween 必须引用已知参数，anime 必须引用可读取的图像资源；
- 帧序列、循环语义和 `from_current` 必须自洽；
- 未知命令、缺失属性和非法数值均拒绝。

验证全部通过后才替换当前场景。恢复过程先建图层和文本，再建动画，避免动画初始化提前覆盖最终属性。任何失败都保留当前 runner、图层、事件和旧档。

## 测试

合成回归必须覆盖：

1. 有限 tween 在中途保存，恢复后相位与最终值正确；
2. 无限 yoyo 从原周期位置继续，而非冻结或重启；
3. 循环 anime 恢复当前帧并继续；
4. 已完成有限 anime 可保存并保持末帧；
5. 活动 transition 恢复最终语义场景且 runner 能继续；
6. 非法动画数据在改变当前状态前被拒绝；
7. 现有无动画 v2 文件、32 项 CTest、嵌入消费者和 Android 导出保持通过。

设备验收至少包括：两款 Artemis 游戏的普通 SAVE／LOAD、Q-SAVE／Q-LOAD、无限光标继续、剧情继续、同游戏重进、Artemis→KiriKiri2→Artemis 和后台／前台。实际截图与引擎日志之外，还必须核对生成文件确为 ARCV v2，不能用界面出现槽位代替成功结论。

## 提交边界

设计、容器／模型、场景恢复、宿主集成分别使用独立 Conventional Commit。公开提交和测试夹具不包含商业游戏名或商业资源；设备日志仅保存在本地诊断目录。
