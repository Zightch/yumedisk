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
- 当前统一主线已经覆盖：
  - `dense`
  - `sparse`
  - `raw`

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
- 当前 `raw` 路径继续保持最小输入面：
  - 只接受已存在 `raw` 文件路径
  - 不提前补文件选择器或更多向导步骤
- 历史完成事实只进 `docs/progress/*.md`，不回填到当前 `todo`。

## 3. 当前未完成子步骤

### Step 4. 做 `raw` 纳入后的整轮验收收口

目标：

- 基于同一套 UIA / 最小调试入口，把当前客户端最小闭环正式收口为统一主线：
  - `full_shell`
  - `minimal_loop`
  - `quit_cleanup`
  - `raw` create / list / remove / quit cleanup
- 确认 `raw` 文件盘没有把现有 session、托盘、显式退出、删盘收口语义带偏。
- 把这一轮验收后的固定口径同步回测试文档与当前待办边界。

完成定义：

- `raw` 文件盘回归进入固定测试入口。
- 当前客户端最小闭环正式收口为：
  - `dense / sparse / raw` 统一主线。
- 当前总目标完成后，按工作流清空当前 `todo`，等待下一轮。

## 4. 当前唯一下一步

- 先把 `raw` 纳入后的整轮验收入口补齐并顺序跑完，再据结果收口测试文档与当前 `todo`。
