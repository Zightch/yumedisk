# `windows/rust-cli` 开发约束

本文件作用域为 `windows/rust-cli` 整个目录树。

## 目标

- 本项目是 `BackendRust` 的最小宿主 CLI。
- 当前只负责把最小控制闭环跑通：启动、打开、建盘、查询、删盘、退出。
- 当前只实现 `denseMem`，不引入文件盘、网络盘、UI。

## 目录组织

- `src/main.rs`
  - CLI 入口、命令分发、生命周期控制。
- `src/dense_mem.rs`
  - `denseMem` 介质实现。

## 命名规范

- Rust 模块文件使用 `snake_case`。
- 类型、trait、enum 使用 `UpperCamelCase`。
- 函数、字段、局部变量使用 `snake_case`。
- 常量使用 `UPPER_SNAKE_CASE`。

## 实现原则

- 先确保最小闭环可运行，不加交互式花样抽象。
- CLI 输出优先面向调试与验收，保持稳定、直接。
- 优先复用 `BackendRust` 的正式公开面，不直接侵入其私有实现。
- CLI 不枚举系统可见盘路径，不等待系统盘路径出现，不承担系统盘读写压测入口。
