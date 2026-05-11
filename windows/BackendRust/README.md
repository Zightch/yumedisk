# BackendRust

`BackendRust` 是 `BackendCore` 的 Rust 版重建项目。

## 当前定位

- 只做块设备编排核心
- 不做 UI、表单、路径选择、具体介质实现
- 宿主创建 `Media`，`BackendRust` 只负责 session / runtime / staging / scan

## 稳定公开面

当前建议宿主只从 crate 根使用这些导出：

- `BackendContext`
- `BackendError`
- `Media`
- `SessionConfig`
- `DiskConfig`
- `ManagedDiskSnapshot`
- `BackendStatsSnapshot`
- `DebugSnapshot`
- `DiskIdentity`
- `enumerate_visible_yumedisks()`
- `make_physical_drive_path()`
- `validate_session_config()`
- `validate_disk_config()`
- `validate_create_disk_inputs()`
- 默认参数常量和 target 常量

其余模块当前都视为内部实现细节，不建议直接依赖。

## 详细文档

- SDK 详见：`windows/BackendRust/BackendRust-SDK文档.md`
