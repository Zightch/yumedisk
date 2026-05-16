# MyDriver4

进阶 WDM 驱动，实现控制端口架构和内核到应用的请求推送机制（WIP）。

## 功能

- 创建控制设备 `\\Device\\MyDev4Ctrl` 作为管理通道
- 内核到应用通信：
  - `MSG_CODE_REQUEST_APP` — 内核将请求推送到应用（通过挂起的 IRP 队列）
- 应用到内核通信：
  - `MSG_CODE_CMD_WRITE` — 应用向内核发送命令
- 待处理 IRP 队列 + 取消例程，实现异步请求匹配
- 多文件模块化架构

## 技术要点

- **框架**: WDM
- **架构模式**: 控制端口 + 请求推送，是 MyKWDF3 原型的前身
- **IRP 管理**: 挂起队列 + 自定义 `CancelRoutine`
- **代码结构**: 多文件分离（Core / Dispatch / Utils / Define）
- **状态**: 包含大量 `// TODO` 注释，属于开发中的实验项目

## 文件结构

```
MyDriver4/
├── main.c         # DriverEntry
├── Dispatch.c/h   # IRP 分发例程
├── Core.c/h       # 核心业务逻辑
├── Define.h       # 常量和结构定义
├── Utils.c/h      # 工具函数
└── MyDriver4.inf  # 安装信息文件
```
