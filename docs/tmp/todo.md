# YumeDisk Client 最小闭环 Todo

本文档只保留当前轮未完成执行项。历史完成事实与验收结果请归档到 [../progress/README.md](../progress/README.md) 对应日期文件。

## 1. 当前总目标

- 完成 `windows/client` 的客户端最小闭环。

这里的“最小闭环”固定指：

- 用户不再依赖 `CLI`；
- 客户端能作为本地桌面程序常驻运行；
- 客户端能完成：
  - 查看 `AppKernel session` 状态；
  - 创建磁盘；
  - 删除磁盘；
  - 查看当前受管磁盘列表；
  - 查看关键日志；
  - 显式退出并完成删盘、关 `session` 收口。

## 2. 当前轮边界

- 基于现有 `windows/client` Qt Widgets 工程推进，不另起新工程。
- 当前后端继续只保留两个核心对象：
  - `BackendContext`
  - `ManagedDisk`
- 可见盘枚举继续留在宿主控制层，不下沉到 `AppKernel`。
- `UI` 只负责操作面和展示面，不直接碰 `AK_SESSION`、`AK_DISK`。
- 当前不引入：
  - `YumeAgent`
  - 本地 `IPC`
  - 独立后台服务进程
  - 额外 `runtime/store/viewmodel` 层级树
- 历史完成事实只进 `docs/progress/*.md`，不回填到当前 `todo`。

## 3. 当前未完成子步骤

### Step 2. 完成最小闭环验收

目标：

- 完成一轮保留受管磁盘的显式退出验收：
  - 启动客户端
  - 成功打开 `AppKernel session`
  - 创建磁盘
  - 保留该磁盘不手动删盘
  - 直接显式退出客户端
- 明确验收到真实收口信号：
  - `removedAll=true`
  - `[backend] closing session`
  - 退出后进程消失
  - 再次启动不会因为旧 `KMDF session` 占用导致 `AkOpen` 失败

完成定义：

- 已确认显式退出路径会执行 `RemoveAllManagedDisks -> AkClose`。
- 当前客户端最小闭环验收完成，可以在此基础上继续做 UI 完善。

## 4. 当前唯一下一步

- 先补一轮“保留磁盘直接显式退出”的 UIA 验收，确认 `RemoveAllManagedDisks -> AkClose` 在真实运行路径上成立。
