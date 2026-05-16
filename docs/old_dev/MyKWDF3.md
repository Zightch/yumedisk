# MyKWDF3

KMDF 控制设备驱动，作为 MyDisk2 StorPort Miniport 的用户态代理/中继层。

## 功能

- 创建 WDF 控制设备，供用户态应用打开通信
- 查找并打开 StorPort Miniport 设备接口
- 将用户态 IOCTL 命令代理转发到 Miniport（通过 `IOCTL_SCSI_MINIPORT`）
- 会话管理：
  - 独占打开（同一时间仅允许一个客户端连接）
  - 自动生成会话 ID
  - 连接建立时发送握手/探测命令
  - 连接关闭时发送会话清理命令

## 技术要点

- **框架**: KMDF，非 PnP 控制设备
- **设备创建**: `WdfControlDeviceInitAllocate` 创建控制设备
- **Miniport 通信**:
  - `IoGetDeviceInterfaces` 查找 SCSI Adapter 设备接口
  - `WdfDeviceMiniportCreate` + `WdfDeviceOpenDevicemapKey` 打开 Miniport 句柄
  - 构造 `SRB_IO_CONTROL` + 命令缓冲区，通过 `WdfIoTargetSendInternalIoctlSynchronously` 发送
- **IOCTL 分发**: `IOCTL_YUMEDISK_APP_COMMAND` 接收用户态命令，转发至 Miniport 并返回结果
- **会话上下文**: `CTRL_DEVICE_CONTEXT` 保存打开锁、打开计数、文件对象、会话 ID
- **协议依赖**: 引用外部 `yumedisk\shared\yumedisk_proto.h`
- **INF 安装**: Class=System, Hardware ID `Root\MyKWDF3`

## 文件结构

```
MyKWDF3/
├── myctl_main.c       # DriverEntry, EvtDeviceAdd
├── myctl_ctrl.c/h     # 控制设备核心逻辑（创建, 打开/关闭, IOCTL 代理, Miniport 通信）
├── myctl_defs.h       # CTRL_DEVICE_CONTEXT, 协议头文件引用
├── myctl_utils.c/h    # 内存分配工具
└── MyKWDF3.inf        # 安装信息文件
```
