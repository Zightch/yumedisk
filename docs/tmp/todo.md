# 当前总目标

重建 `windows/tauri-client` 的正式 UI 视觉层，把概念页中的已确认样式效果迁移到真实客户端。

目标收口为：

- 保持 `Element Plus` 作为基础布局、表单、弹层、消息和交互框架。
- 在 `Element Plus` 基础上叠加概念页已经确认的视觉效果，而不是重写基础组件。
- 不改变当前已打通的功能闭环语义，只重建视觉层、主题层和页面呈现层。
- 不引入概念页之外的新入口、新页面块、新调试信息和新交互。
- 以 `docs/tauri-client-ui-design.md`、`tmp/tauri-client-concept` 概念项目和当前截图基线为唯一视觉参考。

参考：

- `docs/workflow.md`
- `docs/development-principles.md`
- `docs/progress/README.md`
- `docs/tauri-client-ui-design.md`
- `tmp/tauri-client-concept`

# 子步骤

1. 执行整体视觉验收
   - 对照概念页和正式 UI 设计文档检查一致性。
   - 校验不破坏现有功能闭环。

# 当前轮边界

- 当前只做 UI 视觉重建，不重做功能语义。
- 当前必须以 `Element Plus` 为基础，不重写一套自定义基础组件库。
- 当前允许新增必要的全局样式、主题变量和局部样式覆盖，但不建立失控的样式散点。
- 当前允许引用 `tmp/tauri-client-concept` 作为视觉实现参考，但不把该概念项目直接嵌入或作为运行时代码依赖。
- 当前不改 BackendRust、Tauri command、磁盘配置语义和连接语义。
- 当前不引入主题系统之外的新设置项。
- 当前不继续处理主窗口白屏优化。
- 当前不做网络盘 UI 深化，仍只保留禁用占位。
- 当前不增加概念页之外的统计、调试字段、日志视图和 session 详情块。

# 当前唯一下一步

执行整体视觉验收：逐项对照 `tmp/tauri-client-concept`、`docs/tauri-client-ui-design.md` 和当前正式客户端页面，检查初始化页、主页头部、列表面板、磁盘卡片、创建/编辑弹层、设置页是否一致，同时确认不破坏现有功能闭环。

# 进度归档

完成项即时归档到 `docs/progress/*.md`，当前轮执行只看 `docs/tmp/todo.md`，归档规则见 `docs/progress/README.md`。
