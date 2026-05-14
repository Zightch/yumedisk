# `windows/BackendRust` 开发约束

本文件作用域为 `windows/BackendRust` 整个目录树。

## 目标

- 本项目是 `BackendCore` 的 Rust 版重建实现。
- 当前阶段按现有 `BackendCore` 的职责边界完整重建到 Rust，并继续保留现有 C++ `BackendCore` 作为并行参考实现。
- 保持核心抽象极简，不为未来 `NetworkMedia` 提前补完整确认链。

## 目录组织

- `src/lib.rs`
  - crate 对外导出面，只做模块汇总和稳定 re-export。
- `src/types.rs`
  - 对外基础类型、常量、快照 DTO。
- `src/error.rs`
  - core 错误枚举与稳定错误文本。
- `src/config.rs`
  - session / disk 配置校验。
- `src/appkernel.rs`
  - `AppKernel` FFI 定义。
- `src/media.rs`
  - `Media` trait 抽象。
- `src/staging.rs`
  - staged write / commit / reject 相关数据结构和状态机。
- `src/runtime.rs`
  - `BackendContext`、单盘 runtime 持有、事件线程、日志、快照。
- `src/win32.rs`
  - 当前阶段自行声明的最小 Win32 FFI。
- `build.rs`
  - 编译并链接 `AppKernel` C 源码。

## 命名规范

- Rust 模块文件使用 `snake_case`。
- 类型、trait、enum 使用 `UpperCamelCase`。
- 函数、字段、局部变量使用 `snake_case`。
- 常量使用 `UPPER_SNAKE_CASE`。

## 抽象与边界原则

- `BackendRust` 只把 `Media` 当作随机读写逻辑块设备。
- `BackendRust` 不理解 `Media` 内部的文件、内存、网络、缓存、压缩、预读细节。
- 具体 `Media` 实例由外部创建，再把所有权移交给 `BackendRust`。
- `BackendRust` 只负责：
  - session 级运行时编排
  - N 盘 runtime 持有
  - staging 生命周期
  - 快照与统计
  - `AppKernel` FFI 接入
- `BackendRust` 不负责系统可见盘路径枚举，也不输出系统盘路径文本；运行态真状态以 target、lifecycle、online 为准。
- 当前不提前补完整 `NetworkMedia` 确认链。

## 实现原则

- 先用同步线程模型、锁和显式状态，不先引入 async runtime。
- 不为未来场景过度抽象，先把当前最小闭环收清。
- 如果后续接 `AppKernel`，优先通过清晰 FFI 模块显式收口，不把 FFI 细节散落到 runtime 主线。
