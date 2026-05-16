# MyApp1

最早的用户态测试客户端，用于验证基础的驱动设备通信。

## 功能

- 打开 `\\.\MyDriver1` 设备符号链接
- 写入字符串 `"Hello, driver!"` 到设备
- 从设备读取数据并输出
- 包含被注释的 `DeviceIoControl` 调用（未启用）

## 技术要点

- **语言/构建**: C++11, CMake (MinGW)
- **I/O 模型**: 同步 `WriteFile` / `ReadFile`
- **简单验证**: 主要用于验证 `DO_BUFFERED_IO` 驱动的基本读写功能

## 文件结构

```
MyApp1/
├── main.cpp           # 主程序
└── CMakeLists.txt     # CMake 构建配置
```
