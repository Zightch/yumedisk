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
- 在此基础上，继续把 `raw` 文件盘纳入同一条最小闭环，而不是另起第二套路径。

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
- `raw` 文件盘也继续沿现有 `BackendContext -> ManagedDisk -> media` 主线推进，不新增第二套后端壳。
- 历史完成事实只进 `docs/progress/*.md`，不回填到当前 `todo`。

## 3. 当前未完成子步骤

### Step 3. 把 `raw` 文件盘纳入最小闭环

目标：

- 先固定用户这一步真正要完成的事情：
  - 选一个现有 `raw` 文件作为后备介质；
  - 在 `client` 里创建该文件盘；
  - 在主窗口看到它进入受管磁盘列表；
  - 保持与现有 dense / sparse 相同的删盘、日志、显式退出收口语义。
- 路线继续保持“从中间向两边”：
  - 先把“用户要完成什么事”收口成单一路径；
  - 再补 `Backend` 对 `raw` 文件盘的最小承接；
  - 最后再补最小 UI 输入面。

完成定义：

- `raw` 文件盘已经进入当前客户端唯一建盘主线，而不是额外分叉流程。
- `raw` 文件盘沿现有最小闭环可完成：
  - 创建
  - 列表展示
  - 删除
  - 显式退出收口

### Step 4. 再做 `raw` 文件盘后的整轮验收

目标：

- 基于同一套 UIA / 最小调试入口，补一轮 `raw` 文件盘回归：
  - create raw
  - list
  - remove
  - quit cleanup
- 确认 `raw` 文件盘没有把当前 dense / sparse 主线退化。

完成定义：

- `raw` 文件盘回归加入固定测试入口。
- 当前客户端最小闭环从“dense / sparse”扩成“dense / sparse / raw”统一主线。

## 4. 当前唯一下一步

- 先把“用户如何创建一个 `raw` 文件盘”收口成单一路径说明，并据此补进当前 `client` 的建盘参数面与 `Backend` 请求面，不提前做额外 UI 美化。
