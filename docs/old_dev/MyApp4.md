# MyApp4

异步 I/O 用户态测试客户端，MyApp3 的进阶版本，支持重叠 I/O 和请求管理。

## 功能

- 以 `FILE_FLAG_OVERLAPPED` 模式打开设备，支持异步读写
- 每个 I/O 请求分配唯一 ID，支持追踪和管理
- 交互式 REPL 命令：
  - `read <size>` / `write <data>` — 异步读写设备
  - `IOCTL read <size>` / `IOCTL write <data>` — 异步 IOCTL 操作
  - `cancel <id>` — 取消指定的待处理请求
  - `stat <id>` — 查询指定请求的状态
  - `ls` — 列出所有待处理的 I/O 请求
  - `clear` — 清屏
  - `help` — 显示帮助
  - `exit` — 退出

## 技术要点

- **语言/构建**: C++11, CMake (MinGW 工具链)
- **I/O 模型**: 异步重叠 I/O (`OVERLAPPED` 结构体)
- **请求管理**: 使用 `std::map<int, OVERLAPPED*>` 追踪待处理请求
- **取消机制**: `CancelIoEx` 取消指定请求
- **状态查询**: `GetOverlappedResult` 轮询请求完成状态
- **资源释放**: 程序退出前自动取消所有待处理请求并释放 `OVERLAPPED` 内存

## 文件结构

```
MyApp4/
├── main.cpp           # 主程序，异步 I/O REPL
└── CMakeLists.txt     # CMake 构建配置
```
