# MyDriver2

第二个驱动实验，验证 Direct I/O 和 METHOD_IN/OUT_DIRECT 的缓冲区传递机制。

## 功能

- 创建设备 `\\Device\\MyDriver2`，使用 `DO_DIRECT_IO`
- 读操作：通过 `MmGetSystemAddressForMdlSafe` 获取 MDL 映射的系统地址，直接读取数据
- IOCTL 支持两种模式：
  - `MSG_CODE_WRITE` (`METHOD_IN_DIRECT`) — 用户态缓冲区通过 `AssociatedIrp.SystemBuffer` 传入
  - `MSG_CODE_READ` (`METHOD_OUT_DIRECT`) — 内核通过 `AssociatedIrp.SystemBuffer` 返回数据

## 技术要点

- **框架**: WDM
- **I/O 模型**: `DO_DIRECT_IO`，MDL 映射
- **关键 API**: `MmGetSystemAddressForMdlSafe` 将 MDL 映射为内核虚拟地址
- **IOCTL 模式**: `METHOD_IN_DIRECT` / `METHOD_OUT_DIRECT`
- **代码结构**: 单文件驱动 (`Driver.c`)

## 文件结构

```
MyDriver2/
├── Driver.c           # 驱动全部逻辑
└── MyDriver2.inf      # 安装信息文件
```
