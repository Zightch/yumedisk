# YumeDisk Client 最小闭环 Todo

本文档只保留当前轮未完成执行项。历史完成事实与验收结果请归档到 [../progress/README.md](../progress/README.md) 对应日期文件。

## 1. 执行口径

本轮工作固定遵守以下口径：

- `极简核心原则`：当前任务本质上只是把 `TestApp` 从 `CLI` 升级为 `UI` 客户端，不额外引入不必要结构。
- `激进更新原则`：客户端新路径一旦能承接，就直接删除 `Widget` 示例壳和旧 `CLI` 交互壳，不保留双轨。
- `单一真实来源原则`：session 状态只归后端宿主持有，单盘状态只归 `ManagedDisk` 持有，`UI` 不维护第二份真状态。
- `边界闸口原则`：参数校验、建盘参数组合、退出流程和 `AppKernel` 生命周期都收在唯一入口，不散落到控件回调里。
- `文档跟随实现原则`：每完成一个子步骤，先归档到 `docs/progress/*.md`，再重写当前 `todo`。

当前路线固定为：

1. 先以功能目标为核心收口；
2. 再整理 `AppKernel` 接入调用面；
3. 最后补 `UI` 完整度和交互体验。

## 2. 当前总目标

当前唯一总目标：

- 完成 `windows/client` 的客户端最小闭环。

这里的“最小闭环”固定指：

- 用户不再依赖 `CLI`；
- 客户端能作为本地桌面程序常驻运行；
- 客户端能完成最小核心操作：
  - 查看 `AppKernel session` 状态；
  - 创建磁盘；
  - 删除磁盘；
  - 查看当前受管磁盘列表；
  - 查看关键日志；
  - 显式退出并完成删盘、关 session 收口。

## 3. 当前轮边界

本轮实现边界固定如下：

- 基于现有 `windows/client` Qt Widgets 空壳推进，不另起新工程。
- 底层继续复用当前真实链路：
  - `AppKernel`
  - `YumeDiskKMDF`
  - `YumeDiskSCSI`
  - 宿主侧 `read_bytes / stage_write / commit / reject`
- 当前后端继续只保留两个核心对象：
  - `BackendContext`
  - `ManagedDisk`
- 可见盘枚举继续留在宿主控制层，不下沉到 `AppKernel`。
- 为避免 `TestApp` 和 `client` 各自复制扫描逻辑，设备可见性扫描收敛为宿主侧共享静态库。
- 当前不引入：
  - `YumeAgent`
  - 本地 `IPC`
  - 独立后台服务进程
  - 额外 `runtime/store/viewmodel` 层级树
- `UI` 只负责操作面和展示面，不直接碰 `AK_SESSION`、`AK_DISK`。
- 当前本地介质支持范围不退化，至少承接：
  - 稠密内存盘
  - 稀疏内存盘
  - `raw` 文件盘
- 当前不改正式文档，只推进 `docs/tmp/todo.md` 和 `docs/progress/*.md`。
- `backend` 下没有明显类归属、但有明确功能边界的代码，继续按最小组件子目录收纳，例如：
  - `config/`
  - `media/`
  - `runtime/`
  - `types/`

## 4. 子步骤

### Step 1. 暴露 GUI 可调用后端接口

目标：

- 把当前命令式能力收成 `UI` 可直接调用的方法：
  - `QuerySessionState`
  - `SnapshotManagedDisks`
  - `CreateManagedDisk`
  - `RemoveManagedDisk`
  - `RemoveAllManagedDisks`
  - `QueryBackendStats`
  - `QueryDebugSnapshot`
- 收一个统一日志入口给 `UI` 展示。
- 保证 `UI` 读取状态而不复制状态。

完成定义：

- `UI` 不需要自己解释 `AppKernel` 细节。
- 后端能力已经具备稳定调用面。
- 客户端状态真相仍只在后端。

### Step 2. 完成最小 UI 闭环

目标：

- 落一个最小主窗口，至少包含：
  - session 状态区
  - 磁盘列表
  - 建盘按钮
  - 删盘按钮
  - 日志面板
- 落一个最小建盘对话框，至少支持：
  - 容量
  - 介质模式
  - 只读开关
  - `target id`
- 承接现有托盘常驻行为：
  - 关主窗口隐藏
  - 托盘打开主窗口
  - 托盘退出客户端

完成定义：

- 用户无需命令行即可完成核心操作。
- `UI` 已经成为最小可用桌面入口，而不是示例窗口。

### Step 3. 完成最小闭环验收

目标：

- 至少完成一轮最小行为验收：
  - 启动客户端
  - 成功打开 `AppKernel session`
  - 创建磁盘
  - 列出受管磁盘和可见盘信息
  - 删除磁盘
  - 查看日志
  - 显式退出客户端
- 确认关闭主窗口不会直接退出进程。
- 确认显式退出时会先删盘再关 session。

完成定义：

- 当前客户端已经形成真实最小闭环。
- 可以在此基础上再继续做 UI 完善，而不是回头重建主干。

## 5. 验收标准

本轮总目标完成时，必须同时满足：

- `windows/client` 已不再是默认 Qt 示例壳。
- 客户端已真实链接并调用 `AppKernel`。
- `UI` 可以查看 session、建盘、删盘、看盘、看日志。
- 现有本地介质主线没有被 UI 化过程削弱。
- 主窗口关闭仅隐藏到托盘，不直接退出。
- 显式退出会完成 `RemoveAllManagedDisks -> AkClose` 收口。
- 文档状态符合工作流：
  - 已完成事实进入 `docs/progress/*.md`
  - 当前未完成项只留在 `docs/tmp/todo.md`

## 6. 当前唯一下一步

当前唯一下一步：

- 把 `windows/client/backend/` 当前已接入的宿主能力整理成 `UI` 可直接调用的方法面：先落 `QuerySessionState / SnapshotManagedDisks / CreateManagedDisk / RemoveManagedDisk / RemoveAllManagedDisks / QueryBackendStats / QueryDebugSnapshot` 与统一日志读取入口，不接着改主窗口交互，不扩额外层级。
