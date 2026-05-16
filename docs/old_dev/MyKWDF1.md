# MyKWDF1

第一个 KMDF 实验，非 PnP 控制设备驱动，支持动态创建和移除子设备。

## 功能

- 创建非 PnP 控制设备 `\\Device\\MyKWDF1CtrlDev`
- 通过 IOCTL 命令动态管理子设备：
  - `"create dev"` — 创建新的磁盘子设备
  - `"remove dev"` — 移除已创建的子设备
- 使用 `RTL_AVL_TABLE` 管理设备索引
- 子设备注册 `GUID_DEVINTERFACE_DISK` 接口

## 技术要点

- **框架**: KMDF，非 PnP 驱动
- **控制设备**: `WdfControlDeviceInitAllocate` 创建
- **设备管理**: `RTL_AVL_TABLE` 作为设备索引表
- **磁盘接口**: `GUID_DEVINTERFACE_DISK` 使子设备可被系统识别
- **代码结构**: 多文件模块化（CtrlDev / DiskDev / Utils / define）
- **状态**: 磁盘设备处理器大部分为 `// TODO` 存根，属于探索性实验

## 文件结构

```
MyKWDF1/
├── main.c         # DriverEntry, EvtDeviceAdd
├── CtrlDev.c/h    # 控制设备逻辑（IOCTL 分发, 子设备管理）
├── DiskDev.c/h    # 磁盘子设备逻辑（大部分 TODO）
├── define.h       # 常量和结构定义
├── Utils.c/h      # 工具函数
└── MyKWDF1.inf    # 安装信息文件
```
