# MyApp2

针对 MyDriver2 的测试客户端，验证 Direct I/O 和 METHOD_IN/OUT_DIRECT IOCTL 模式。

## 功能

- 打开 `\\.\MyDriver2` 设备
- 通过 `ReadFile` 测试 `DO_DIRECT_I/O` 读取
- 通过 `DeviceIoControl` 测试两种 IOCTL 模式：
  - `MSG_CODE_WRITE` — `METHOD_IN_DIRECT`，用户态写数据到内核
  - `MSG_CODE_READ` — `METHOD_OUT_DIRECT`，内核返回数据到用户态

## 技术要点

- **语言/构建**: C++11, CMake (MinGW)
- **I/O 模型**: 同步 I/O + `DeviceIoControl`
- **测试重点**: 验证 `METHOD_IN_DIRECT` 和 `METHOD_OUT_DIRECT` 的缓冲区传递方式

## 文件结构

```
MyApp2/
├── main.cpp           # 主程序
└── CMakeLists.txt     # CMake 构建配置
```
