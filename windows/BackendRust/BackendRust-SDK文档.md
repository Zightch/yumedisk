# BackendRust SDK文档

本文档面向 `BackendRust` 的宿主接入方，描述当前 Rust 版后端核心的职责边界和使用口径。

## 1. 目标与边界

`BackendRust` 是 `BackendCore` 的 Rust 版重建项目，当前目标是：

- 保持与现有 `BackendCore` 同等级的运行时职责。
- 继续坚持“只做块设备编排，不理解具体介质实现细节”。
- 为后续 `Tauri + Rust` 宿主提供更自然的内聚边界。

它负责：

- `AppKernel` session 打开、关闭、状态查询。
- N 盘 runtime 持有。
- staged write / commit / reject。
- 建盘、删盘、全删盘。
- 运行时日志、统计、调试快照。
- 可见盘扫描。

它不负责：

- UI
- 表单输入解析
- 路径选择、对话框流程
- 具体 `Media` 子类实现
- `NetworkMedia` 的内部确认链、缓存、预读、压缩、共享传输层

## 2. 当前固定边界

- 具体 `Media` 实例由宿主侧创建，再把所有权移交给 `BackendRust`。
- `BackendRust` 只把 `Media` 看作随机读写逻辑块设备。
- `Media::write_locked()` 返回成功，即表示该介质实例已经接受并承担后续一致性责任。
- `BackendRust` 不追踪更深层的网络确认、落稳确认或缓存刷写确认。

## 3. 当前保留的 `NetworkMedia` 关键争议

当前只在边界上保留两个后续必须明确的问题：

1. `write_locked()` 返回成功的严格语义是什么。
2. 是否要使用可选 `flush()` 作为宿主主动要求介质尽量落稳的能力。

除此之外，缓存、预读、压缩、共享传输、重试等都留到真正做 `NetworkMedia` 时再定。

## 4. 公开接口

当前 crate 对外公开：

- `SessionConfig`
- `DiskConfig`
- `ManagedDiskSnapshot`
- `BackendStatsSnapshot`
- `DebugSnapshot`
- `Media`
- `BackendContext`

## 5. 与现有 C++ BackendCore 的关系

- `windows/BackendCore/` 当前继续保留，作为现有稳定参考实现。
- `windows/BackendRust/` 当前按相同职责边界重建。
- 当前阶段文档收口已明确：这是“重建并并行保留”，不是覆盖删除。
