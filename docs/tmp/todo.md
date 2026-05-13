# 当前总目标

完成 `tauri-client` 当前轮控件状态样式收口，消除单选按钮首项边框差异和连接按钮禁用黑边。

## 子步骤

1. 重建 `ElRadioButton` 与磁盘卡片主按钮的全状态样式接管。

## 当前轮边界

- 只处理 chip 单选按钮与磁盘卡片主按钮的状态样式残留。
- 不改页面结构，不改功能语义，不扩展到无关控件。
- 完成后立即归档到 `docs/progress/2026-05-13.md` 并提交。

## 当前唯一下一步

重建 `RadioButton` 的 first/last/checked/focus/disabled 全状态样式，并接管磁盘卡片主按钮的 loading/disabled/focus-visible 状态边框与阴影。

参考：

- `docs/development/workflow.md`
- `docs/progress/README.md`
