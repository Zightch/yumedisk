# MyApp3

同步 I/O 用户态测试客户端，用于与内核驱动进行交互式通信。

## 功能

- 通过命令行参数指定设备路径，打开驱动设备文件
- 提供交互式 REPL 命令行，支持以下命令：
  - `read <size>` — 从设备读取指定字节数
  - `write <data>` — 向设备写入数据
  - `IOCTL read <size>` — 通过 `DeviceIoControl` 读取
  - `IOCTL write <data>` — 通过 `DeviceIoControl` 写入
  - `exit` — 退出程序

## 技术要点

- **语言/构建**: C++11, CMake (MinGW 工具链)
- **I/O 模型**: 同步 `ReadFile` / `WriteFile` / `DeviceIoControl`
- **设备访问**: 通过 `CreateFile` 打开设备符号链接路径
- **缓冲区管理**: 栈上分配读写缓冲区，使用 `memset` 清零

## 文件结构

```
MyApp3/
├── main.cpp           # 主程序，交互式命令循环
└── CMakeLists.txt     # CMake 构建配置，静态链接
```
