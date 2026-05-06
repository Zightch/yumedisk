# YumeDisk App-owned 内存虚拟磁盘文档

本文档集采用最终收口口径，描述下一阶段要落到的唯一多盘数据面：

- `YumeDiskKMDF`：KMDF 控制驱动，负责 App 会话、watchdog、direct I/O slot transport 和 miniport 代理。
- `YumeDiskSCSI`：Storport miniport，负责向 Windows 暴露 SCSI target、处理系统 READ/WRITE SRB、维护 per-target 数据面队列。
- `RWTestApp`：用户态 App/测试程序，负责实际介质、建盘删盘、per-disk slot engine 和压测验证。

早期文档中的 `MyCtl`、`MyDisk`、`MyApp` 只作为历史逻辑名保留；正式文档不再用它们描述新的多盘数据面目标。

文档列表：

- [项目概述](./项目概述.md)
- [功能文档](./功能文档.md)
- [技术文档](./技术文档.md)
- [开发文档](./开发文档.md)
- [Windows驱动问题排查笔记](./Windows驱动问题排查笔记.md)
- [执行工作流](./workflow.md)
- [进度归档](./progress/README.md)

最终方案的核心原则：

- App 持有唯一真实介质；驱动不把介质所有权搬回内核。
- App 只打开 `YumeDiskKMDF` 暴露的自定义设备接口，不直接访问 `GUID_DEVINTERFACE_STORAGEPORT`。
- `YumeDiskKMDF` 在内核中定位并持有同会话生命周期的 `YumeDiskSCSI` miniport handle。
- `YumeDiskSCSI` 负责真实虚拟盘、系统 SCSI I/O、per-target 队列和 SRB completion。
- 数据面只保留 `POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH` 这一条 app-owned slot queue 链路。
- 旧 `WAIT_EVENT` inline payload 路径不再作为目标实现保留。
- `POST_WRITE_SLOT` 只提交 write slot；写确认统一通过独立 `WRITE_ACK_BATCH`。
- `YumeDiskKMDF` 不维护自己的数据面状态机，只做 session/watchdog/direct-I/O transport。
- `RWTestApp` 按磁盘独立维护 slot engine、queue depth、介质和 ACK flush，不回到全局共享 worker pool。
- 该虚拟盘只用于普通数据盘，不支持系统盘、启动盘、分页盘。
