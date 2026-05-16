# MyDriver1

第一个内核驱动实验，演示 WDM 基础知识和常用内核 API。

## 功能

- 创建单个设备 `\\Device\\MyDriver1`，使用 `DO_BUFFERED_IO`
- 演示的内核编程技术：
  - Unicode 字符串操作 (`UNICODE_STRING`)
  - 内存分配 (`ExAllocatePool2`)
  - Lookaside List (`NPAGED_LOOKASIDE_LIST`)
  - 链表 (`LIST_ENTRY`) 遍历与管理
  - 泛型表 / AVL 树 (`RTL_GENERIC_TABLE`)
  - 文件 I/O (`ZwCreateFile` / `ZwWriteFile`)
  - 注册表操作 (`ZwCreateKey` / `ZwSetValueKey` / `ZwQueryValueKey`)
  - IOCTL 处理 (`METHOD_BUFFERED`)

## 技术要点

- **框架**: WDM
- **I/O 模型**: `DO_BUFFERED_IO`，`METHOD_BUFFERED` IOCTL
- **代码结构**: 单文件驱动 (`Driver.c`)，约 437 行
- **用途**: 学习/教程性质，涵盖内核开发常用 API

## 文件结构

```
MyDriver1/
├── Driver.c           # 驱动全部逻辑
└── MyDriver1.inf      # 安装信息文件
```
