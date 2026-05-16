# MyDisk2

StorPort 虚拟 SCSI Miniport 磁盘驱动，实现完整的 SCSI 命令集，支持动态磁盘管理和事件驱动架构。

## 功能

- 完整的 SCSI Miniport 实现，对 Windows 呈现为 SCSI 磁盘控制器
- 支持的 SCSI CDB 命令：
  - `INQUIRY` — 设备查询
  - `READ_CAPACITY` — 容量查询
  - `READ` / `WRITE` (6/10/12/16) — 多种长度的读写命令
  - `REPORT_LUNS` — LUN 报告
  - `TEST_UNIT_READY` — 设备就绪检测
  - `REQUEST_SENSE` — 错误信息
  - `MODE_SENSE` — 模式信息
- 通过自定义协议 (`yumedisk_proto.h`) 实现控制通道：
  - 动态创建/移除磁盘
  - 事件等待与通知机制
  - 读写数据的请求/应答
  - 心跳保活
  - 会话管理
- 支持多虚拟磁盘 Target，每个 Target 独立管理
- 待处理 I/O 队列和 Waiter 节点事件架构

## 技术要点

- **框架**: StorPort Miniport
- **初始化数据**: `VIRTUAL_HW_INITIALIZATION_DATA`，注册 `HwFindAdapter` / `HwInitialize` / `HwStartIo` / `HwAdapterControl` / `HwFreeAdapterResources` 回调
- **同步机制**: `STORPORT_SPINLOCK` 保护共享数据结构
- **数据结构**:
  - `YUME_DISK` — 虚拟磁盘实例，含 LUN、容量、缓冲区等
  - `YUMEDISK_EVENT_NODE` — 事件队列节点
  - `YUMEDISK_WAITER_NODE` — 等待者节点（等待事件的请求）
  - `YUMEDISK_PENDING_IO_NODE` — 待处理 I/O 节点
- **协议依赖**: 引用外部 `yumedisk\shared\yumedisk_proto.h`，定义控制命令结构和 GUID
- **INF 安装**: Class=SCSIAdapter, Hardware ID `Root\MyDisk2`

## 文件结构

```
MyDisk2/
├── main.c         # DriverEntry, StorPort 回调注册
├── disk.c/h       # SCSI CDB 处理, IOCTL SRB 处理, 磁盘管理, 事件/IO 队列
├── define.h       # DEVICE_CONTEXT, 链表结构定义
├── utils.c/h      # 内存分配工具
└── MyDisk2.inf    # 安装信息文件
```
