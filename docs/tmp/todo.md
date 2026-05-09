# YumeDisk Client 结构重建 Todo

本文档只保留当前轮未完成执行项。历史完成事实与验收结果请归档到 [../progress/README.md](../progress/README.md) 对应日期文件。

## 1. 当前总目标

- 按 `docs/tmp/yumedisk-client-formal-proposal.md` 与 `docs/development-principles.md`，重建 `windows/client` 第一阶段最小闭环结构，把控制链、数据链、持有链和介质继承链重新收口到唯一主线。

## 2. 当前轮边界

- 当前目标是重建现有单进程本地客户端结构，不引入 `IPC`、后台服务、网络和远程形态。
- 当前继续以“先固定用户要完成什么事”为主线，只服务建盘、删盘、看盘、看状态、看日志这五类能力。
- 当前只保留 `UI -> Backend -> BackendContext -> map<targetId, DiskRuntime>` 这一条主控制链。
- 当前按 `N` 盘模型重建：每盘各自持有一份 `DiskRuntime / StagingStore / Media`，不再维护全局共享介质或第二份盘状态真相。
- 当前介质主线固定为 `Media -> MemoryMedia / FileMedia -> RawFileMedia`；当前实现只要求 `denseMem / sparseMem / rawFile`。
- 当前不显式建宿主 `worker` 池，不模拟 `AppKernel` 内部 `Q*2 worker`；宿主侧只处理按盘运行时与竞争收口。
- 当前重建动作遵守“最小组件目录”口径，类使用类名目录承接，功能组件使用小驼峰目录承接。
- 历史完成事实只进 `docs/progress/*.md`，不回填到当前 `todo`。

## 3. 当前未完成子步骤

### Step 1. 重建全局控制面骨架

目标：

- 重建 `Backend` 与 `BackendContext` 的唯一控制入口；
- 重建 `session` 生命周期、事件泵、日志入口和 `map<targetId, DiskRuntime>` 的唯一归属；
- 重建建盘、删盘、全部删盘、状态查询的统一控制面。

完成定义：

- 全局控制职责不再散落在 `Widget`、临时 `helper` 或其他组件中；
- `BackendContext` 成为唯一 `session` 与运行时持有者；
- `UI` 只通过 `Backend` 调用，不直连 `AppKernel` 细节。

### Step 2. 重建按盘运行时骨架

目标：

- 重建 `DiskRuntime`，按盘承接 `metadata / staging / media`；
- 重建 `N` 盘展开关系与生命周期边界。

完成定义：

- 每个 `target` 各自持有独立 `DiskRuntime`；
- 单盘状态不再混在全局杂项字段里；
- 按盘资源可随建盘、删盘完整创建与释放。

### Step 3. 重建按盘暂存层

目标：

- 重建 `StagingStore`，统一承接 `stageWrite / commit / reject`；
- 重建暂存读叠加与提交应用的唯一落点。

完成定义：

- 暂存状态只有一份真实来源；
- `commit / reject` 不再分散在多个函数和临时容器中；
- 读写链路按 `targetId` 进入对应单盘暂存层。

### Step 4. 重建介质抽象主线

目标：

- 重建 `Media / MemoryMedia / FileMedia / RawFileMedia`；
- 重建 `denseMem / sparseMem / rawFile` 的统一介质主线。

完成定义：

- `denseMem / sparseMem` 统一落到 `MemoryMedia`；
- `rawFile` 统一落到 `RawFileMedia`；
- 介质差异不再散落在后端其他组件中。

### Step 5. 重建 UI 与运行时分离边界

目标：

- 重建 `Widget / CreateDiskDialog` 对新后端的调用面；
- 重建“整改 `UI` 不影响核心功能主线”的边界。

完成定义：

- `UI` 只消费快照与命令接口，不持有第二份业务真相；
- `UI` 调整不需要改动 `AppKernel` 接入和数据主线；
- 最小闭环 `create -> list -> remove -> quit` 继续只经过统一后端入口。

## 4. 当前唯一下一步

- 先重建全局控制面骨架，把 `Backend / BackendContext / map<targetId, DiskRuntime>` 的唯一归属收紧到一条主线。
