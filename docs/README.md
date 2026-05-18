# YumeDisk 文档

本文档集采用当前正式口径，区分“当前已落地主线”“参考资料”“临时工作区”和“历史归档”。

当前仓内主线分为两部分：

- 驱动与用户态数据面内核：
  - `YumeDiskKMDF`：KMDF 控制驱动，负责控制接口、单会话、watchdog、direct-I/O slot transport 和 miniport 代理。
  - `YumeDiskSCSI`：Storport miniport，负责向 Windows 暴露 SCSI target、处理系统 READ/WRITE SRB、维护 per-target 数据面队列。
  - `AppKernel`：纯 `C` 用户态数据面内核，负责 session、per-disk workers、slot 投递、ACK 推进和事件队列。
- 当前正式宿主主线：
  - `windows/tauri-client`：当前正式桌面客户端。
  - `windows/BackendRust`：当前正式 Rust 宿主运行时，直接承接 `AppKernel`。

仓内同时保留两条辅助宿主/工具线：

- `windows/cpp-cli` + `windows/BackendCore`：并行 C++ 宿主线和调试入口。
- `windows/rust-cli`：最小 Rust 控制台验证入口。

正式文档只使用当前组件名，不再引用已废弃的 dev 项目名或旧宿主目录。

## 目录结构

### 网络盘正式主线

- [Network Protocol](./network/README.md)
- [Protocol Define](./network/define/README.md)
- [Client 实现文档](./network/client/README.md)
- [Server 实现文档](./network/server/README.md)

### 架构与目标

- [项目概述](./architecture/项目概述.md)
- [功能文档](./architecture/功能文档.md)
- [技术文档](./architecture/技术文档.md)

### 开发规则与流程

- [开发文档](./development/开发文档.md)
- [开发原则](./development/development-principles.md)
- [执行工作流](./development/workflow.md)

### UI 与前端设计

- [Element Plus UI 规范提示](./ui/Element-Plus-UI规范提示词.md)
- [Tauri Client UI 设计方案](./ui/tauri-client-ui-design.md)
- [Tauri Client Element 落地说明](./ui/tauri-client-element-grounding-notes.md)
- [Tauri Client UI 规范](./ui/tauri-client-ui-guidelines.md)

### 排查与归档

- [Windows驱动问题排查笔记](./troubleshooting/Windows驱动问题排查笔记.md)
- [进度归档](./progress/README.md)

### 临时与历史

- [临时工作区](./tmp/todo.md)
- `old_dev/`：早期驱动探索归档，保留不动

### 组件与 SDK 文档

- [AppKernel设计文档](../windows/AppKernel/AppKernel设计文档.md)
- [AppKernel SDK文档](../windows/AppKernel/AppKernel-SDK文档.md)
- [BackendCore SDK文档](../windows/BackendCore/BackendCore-SDK文档.md)

当前正式口径下的核心原则：

- 用户态宿主持有唯一真实介质；驱动不把介质所有权搬回内核。
- `AppKernel` 只打开 `YumeDiskKMDF` 暴露的自定义设备接口，不直接访问 `GUID_DEVINTERFACE_STORAGEPORT`。
- `YumeDiskKMDF` 在内核中定位并持有同会话生命周期的 `YumeDiskSCSI` miniport handle。
- `YumeDiskSCSI` 负责真实虚拟盘、系统 SCSI I/O、per-target 队列和 SRB completion。
- 数据面只保留 `POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH` 这一条 app-owned slot queue 链路。
- 旧 `WAIT_EVENT` inline payload 路径不再作为目标实现保留。
- `POST_WRITE_SLOT` 只提交 write slot；写确认统一通过独立 `WRITE_ACK_BATCH`。
- `YumeDiskKMDF` 不维护自己的数据面状态机，只做 session/watchdog/direct-I/O transport。
- 当前正式桌面主线不枚举系统设备路径，不以 `PhysicalDrive` 或可见盘路径作为挂载成功判据。
- 宿主按磁盘独立维护 runtime、介质和配置状态，不回到跨盘共享 worker pool 或第二份真状态。
- 该虚拟盘只用于普通数据盘，不支持系统盘、启动盘、分页盘。
