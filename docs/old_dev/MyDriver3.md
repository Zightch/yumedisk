# MyDriver3

WDM 管道驱动，实现两个设备之间的内核态 IPC 机制。

## 功能

- 创建两个设备对象 `\\Device\\MyDev1` 和 `\\Device\\MyDev2`，及其符号链接
- 两个设备形成一对连接：向一个设备写入的数据可从另一个设备读取
- 实现完整的 IRP 分发：`IRP_MJ_CREATE` / `CLOSE` / `READ` / `WRITE` / `DEVICE_CONTROL`
- 支持取消的待处理 IRP 队列，用于异步读写匹配

## 技术要点

- **框架**: WDM (Windows Driver Model)，纯 IRP 驱动
- **I/O 模型**: 基于 IRP 队列的异步管道
- **读写匹配**: 写操作将数据附加到读操作的待处理 IRP 上；若无匹配的读 IRP，则将写 IRP 入队等待
- **取消机制**: 自定义 `CancelRoutine`，在 IRP 被取消时从队列中移除
- **同步**: 自旋锁 (`KSPIN_LOCK`) 保护 IRP 队列操作
- **代码结构**: 单文件驱动 (`main.c`)，所有逻辑集中实现
- **签名**: 构建输出包含 `.cer` 证书文件

## 文件结构

```
MyDriver3/
├── main.c                  # 驱动全部逻辑（DriverEntry, 分发例程, 读写, 取消）
├── MyDriver3.sln           # VS 解决方案
└── MyDriver3.vcxproj       # VS 项目文件
```
