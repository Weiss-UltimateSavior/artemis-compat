# Android纯修复PR审阅顺序（2026-10-19）

根据维护者[拆分要求](https://github.com/Weiss-UltimateSavior/artemis-compat/pull/11#issuecomment-6230862902)，PR #11只保留修复。基准更新为上游 `31099b581a2f2355d0a32ac2d840a3a387dec62e`，共7条代码修复与1条截图/审阅文档提交。

|顺序|提交|修复|旧系列来源|
|---|---|---|---|
|1|`ca1f8d8`|fix(script): 保留游戏框架的系统语音路径|`1f4775d`|
|2|`11198d1`|fix(script): 保留失败调用状态并解绑退休执行器|`ac5a565`|
|3|`d25c333`|build: 在Android上遵守显式HEADLESS配置|`6fd8b1b`|
|4|`20e6170`|fix(script): 排除无限动画等待并保留有限延迟|`a4c119e`|
|5|`5d683da`|fix(text): 对齐像素字号并保留完整文本布局空间|`5f8fe30`|
|6|`6980165`|fix(android): 保留后台会话并规范窗口和输入恢复|`a7cd5c4`|
|7|`89e9690`|fix(android): 串行保护DebugBridge自有音频会话|`0d9f0a3`|
|8|本文档提交|限定纯修复范围，记录验证和9张历史文本原图|旧 `8cc2ec3` 的截图与风险说明|

上游已提供PAD双描述符登记、MOVE完整事件发布和系统音量默认分发，旧 `40072a1` / `41c9ad3` / `d9d8d2c` 不再重复提交。保留上游JString空安全、对话框退出保护、mangled/unmangled音频入口、图层索引/变换缓存，以及CreateContext/CreateSurface与持久视口映射。未另加按键动作策略；旧输入策略专用套件不再注册，上游jni_util覆盖mapped/unmapped键ID。

后台修复在上游已有surface保留的基础上补parking pbuffer、错误分类、窗口引用所有权和会话暂停：短暂窗口失败不销毁健康context，不由失败回退重跑boot；真实context loss保留CPU会话并停帧。视口回归覆盖重附着letterbox、sidecut及Resize。音频修复统一四个导出到同一线程/会话所有权策略，合成回归直接链接四入口。

本PR不含完整舞台C ABI、滤镜回调、资源generation通知或GLES3偏好。旧功能提交 `eb388dc` 另存候选，须在修复合并后基于新main适配并单独提案；不能把仍包含旧修复的分支直接对main创建新PR。滤镜算法、监控UI和暂停中的文本排除均不在此PR内。

## 验证与边界

- NDK30、arm64-v8a、API26、FFmpeg OFF：HEADLESS/NULL全部目标编译链接成功，登记34个用例；GLES/OpenSL的artemis、Renderer生命周期、font_style和compositor目标编译链接成功。
- 实际原生库14个既有Android动态入口检查通过，不含 `artc_register_presentation_v1`。附带Python导出检查器回归尝试执行，但其合成nm脚本依赖Unix shebang，Windows返回WinError193；该项不计为通过，也没有为此修改上游工具。
- 两名独立代理交叉复核未发现阻断项；模块方向和git diff --check核查通过。交叉编译的C++用例未执行，fake EGL不等于真实驱动。
- 没有新ADB、游戏、JVM、实际出声、CGL/ANGLE测试或新APK；原图及历史包保持原样。用户历史验收仅覆盖文本、普通后台、实体音量和游戏语音，不扩大到本次适配版。
- 真实context loss无损GPU重建、系统杀进程恢复、游戏内设置/YES-NO无响应或延迟仍未解决或未覆盖。菜单候选不带回，见[风险记录](menu-wait-review-20261011.md)。
