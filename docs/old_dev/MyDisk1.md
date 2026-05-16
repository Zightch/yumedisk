# MyDisk1

KMDF 虚拟磁盘驱动，在内存中模拟一个固定磁盘设备，对 Windows 呈现为标准磁盘。

## 功能

- 创建 64MB 虚拟磁盘（131072 个 512 字节扇区，物理扇区 4096 字节）
- 处理标准磁盘 IOCTL：
  - `IOCTL_DISK_GET_DRIVE_GEOMETRY` — 返回磁盘几何信息
  - `IOCTL_DISK_GET_LENGTH_INFO` — 返回磁盘大小
  - `IOCTL_STORAGE_GET_DEVICE_NUMBER` — 返回设备编号
  - `IOCTL_STORAGE_GET_HOTPLUG_INFO` — 返回热插拔信息
  - `IOCTL_DISK_GET_PARTITION_INFO` / `IOCTL_DISK_GET_PARTITION_INFO_EX` — 分区信息
  - `IOCTL_VOLUME_GET_GPT_ATTRIBUTES` — GPT 属性
  - `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` — 卷磁盘范围
  - `IOCTL_MOUNTDEV_QUERY_DEVICE_NAME` — 设备名称查询
- 读写操作直接在内存缓冲区上完成

## 技术要点

- **框架**: KMDF (Kernel-Mode Driver Framework)
- **设备类型**: `FILE_DEVICE_DISK`，PnP 设备
- **I/O 模型**: `DO_BUFFERED_IO`，通过 WDF IO Queue 分发请求
- **内存管理**: `ExAllocatePool2` / `ExFreePoolWithTag` 封装为 `malloc` / `free`
- **设备接口**: 设置 `GUID_DEVINTERFACE_DISK` 和友好名称，可在磁盘管理中识别
- **INF 安装**: Class=System, Hardware ID `Root\MyDisk1`

## 文件结构

```
MyDisk1/
├── main.c         # DriverEntry, 设备创建, IO Queue, IRP 分发
├── Disk.c/h       # 各 IOCTL 处理函数实现
├── DiskIoctl.c    # 实验性 IOCTL 实现（WIP）
├── Diskutils.c    # 内存分配工具封装
├── define.h       # 常量定义, DEVICE_CONTEXT, DISK_PROPERTY
└── MyDisk1.inf    # 安装信息文件
```
