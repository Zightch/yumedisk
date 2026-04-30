# App主导内存虚拟磁盘文档

本文档集对应当前仓库中的三个现有项目，后续改造时直接在原项目上演进：

- `MyKWDF3` -> `MyCtl`
- `MyDisk2` -> `MyDisk`
- `MyApp5` -> `MyApp`

文档列表：

- [项目概述](./项目概述.md)
- [功能文档](./功能文档.md)
- [技术文档](./技术文档.md)
- [开发文档](./开发文档.md)

当前方案的核心原则：

- `MyApp` 不直接访问 `GUID_DEVINTERFACE_STORAGEPORT`
- `MyApp` 只打开 `MyCtl` 暴露的自定义设备接口
- `MyCtl` 在内核中枚举 `GUID_DEVINTERFACE_STORAGEPORT`，定位 `MyDisk2`
- `MyDisk2` 负责真实虚拟盘和系统SCSI I/O
- `MyApp` 启动时先完成驱动与设备自检、自安装、自修复
- `MyApp` 不在线时，`MyDisk2` 不暴露任何盘
- `MyCtl` 和 `MyDisk2` 在系统中都只能存在一个有效设备实例
- `MyApp` 在系统中只能存在一个活动进程
- 该虚拟盘只用于普通数据盘，不支持系统盘、启动盘、分页盘
