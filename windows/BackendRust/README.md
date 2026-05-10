# BackendRust

`BackendRust` 是 `BackendCore` 的 Rust 版重建项目，当前按现有 `BackendCore` 的同级职责边界完整落地。

## 当前定位

- `windows/BackendCore/` 继续保留。
- `windows/BackendRust/` 负责把同一套运行时职责迁到 Rust。
- 当前以“块设备编排内核”为边界，不把 UI、表单、具体文件/网络介质细节混进来。

## 主要组成

- `build.rs`
  - 直接编译并链接 `AppKernel` C 源码。
- `src/appkernel.rs`
  - `AppKernel` FFI 定义。
- `src/config.rs`
  - 配置校验与 `AppKernel` 参数构造。
- `src/media.rs`
  - `Media` trait。
- `src/scan.rs`
  - 可见盘枚举。
- `src/staging.rs`
  - staged write / commit / reject。
- `src/runtime.rs`
  - `BackendContext`、N 盘 runtime、事件线程、日志、快照。

## 当前接口口径

- `BackendRust` 只把 `Media` 看作随机读写逻辑块设备。
- 具体 `Media` 实例由宿主侧创建，再把所有权移交给 `BackendRust`。
- `BackendRust` 不理解 `Media` 内部是内存、文件还是网络。
- `Media::write_locked()` 返回成功，即表示该介质实例已经接受并承担后续一致性责任。
- `BackendRust` 不为未来 `NetworkMedia` 预埋完整网络确认链。
