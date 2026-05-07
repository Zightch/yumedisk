# AppKernel 重建 Todo

本文档用于收束 `windows/AppKernel` 子项目的实现步骤。当前任务不是在 `RWTestApp` 现有数据面上继续打补丁，而是按照 [开发原则](../development-principles.md) 直接重建唯一正确实现。

同时，当前 `todo` 既要保留完整执行细节，也要明确当前阶段、固定边界和当前唯一下一步，避免为了“收口”把关键约束删掉。

## 1. 执行口径

本轮工作必须同时满足以下原则：

- `极简核心原则`：先打通 `AppKernel -> KMDF -> SCSI` 的最小可编译闭环，再补充宿主接入和调试能力。
- `激进更新原则`：不保留“`RWTestApp` 旧数据面”和“`AppKernel` 新数据面”长期双轨；新路径能承接后，旧路径直接删除。
- `单一真实来源原则`：`KMDF session`、per-disk runtime、slot/ACK/event 状态只能归 `AppKernel` 持有；介质、暂存层和 overlay 读视图只能归业务宿主持有。
- `边界闸口原则`：参数校验、协议校验、生命周期校验全部收在 `AppKernel` 边界入口，内部 worker 不重复散落兜底。
- `结构重构与层次依赖原则`：按“公开 API / session / disk runtime / protocol transport / event finalize”分层，不允许继续把线程、介质、CLI、设备收敛揉进一个文件。
- `删除优先原则`：凡是已经和 `AppKernel` 正式方案冲突的旧数据面结构，后续切换时直接删除，不保留兼容桥。
- `测试覆盖原则`：每一步至少保证编译通过；接入宿主后再按真实风险补并发、取消、关闭、late ACK、QD 压测验证。
- `文档跟随实现原则`：实现边界一旦变化，同步回写正式文档和 `progress`，不保留历史草案描述。

## 2. 当前目标

当前唯一总目标：

- 落成 `windows/AppKernel` 纯 `C` DLL 子项目。
- 让 `AppKernel` 成为唯一用户态数据面内核。
- 为后续把 `RWTestApp` 收缩成“宿主控制层 + 介质层”做好结构承接。
- 从第一天起保证 `MSVC` / `MinGW` 两种工具链兼容。
- 同步产出 `AppKernel SDK` 文档。

当前明确不做：

- 不做 `C/C++` 双实现。
- 不做旧接口兼容层。
- 不做业务层抽象扩展点。
- 不在 `RWTestApp` 旧数据面上继续叠功能。

## 3. 当前阶段与固定边界

### 3.1 当前阶段

当前已经完成的基础：

- `Step 1` 的 DLL 工程骨架已经落地。
- `include/appkernel.h` 已经固定 `AK_*` / `Ak*` 的公开 `C ABI` 边界。
- `src/common`、`src/session`、`src/disk`、`src/protocol`、`src/event` 已经建好分层骨架。
- `MSVC` / `MinGW` 双工具链的 DLL 编译已经实际通过。

当前还没有完成的真实能力：

- 还没有真正打开 `KMDF control device`。
- 还没有真正拿到 `SessionId`。
- 还没有 heartbeat 线程。
- 还没有真实事件队列。
- 还没有真实协议 transport。
- 还没有真实 per-disk runtime。

### 3.2 固定边界

以下边界在本轮实现中固定，不允许回退：

- `AppKernel` 必须继承当前 `RWTestApp` 已验证通过的多盘并发模型，不回退到旧的单盘 `Q` 队列模型。
- 多盘模型的含义是：
  - 每个盘有自己独立的 runtime。
  - 每个盘有自己独立的 `read workers`、`write workers`、`write ACK flush worker`。
  - 每个盘有自己独立的 slot 深度、pending 状态、统计和生命周期。
  - 多盘之间应是完全并发，而不是共享一套“全局单盘调度核心”后再在外面挂多个盘。
- 不允许回退成旧模型的任何变体：
  - 不允许回退成“单盘 + 全局 `Q` 读 / `Q` 写队列”的数据面核心。
  - 不允许回退成“多盘只是复用同一套单盘调度器轮流跑”的伪多盘结构。
  - 不允许为了暂时压 `CPU` 或回避并发问题，把多盘模型拆回单盘中心模型。
- 当前写路径边界继续保持：
  - `POST_WRITE_SLOT`
  - `stage_write`
  - `WRITE_ACK_BATCH`
  - `AkEventWriteFinalCommitted / AkEventWriteFinalRejected`
- 当前取消边界继续保持：
  - 系统侧取消只追到 `SCSI`。
  - `AppKernel` 不新增一条全链路取消协议。
  - 业务宿主只根据最终事件决定 `commit` 或 `discard`。

## 4. 分步 Todo

### Step 1. 建立 AppKernel 子项目骨架

状态：

- 已完成。

目标：

- 新建 `windows/AppKernel` 的 `CMakeLists.txt`。
- 建立公开头文件 `include/appkernel.h`。
- 固定 DLL 导出边界和 `C ABI`。
- 固定 `MSVC` / `MinGW` 兼容的导出宏、调用约定和类型口径。
- 建立内部目录分层：
  - `src/common`
  - `src/session`
  - `src/disk`
  - `src/protocol`
  - `src/event`
- 先把 opaque handle、公共结构、状态结构、统计结构、错误返回口径固定下来。

完成定义：

- `AppKernel` 能以 DLL 方式参与编译。
- 公开 API 名称、前缀、结构体边界全部收束为 `AK_*` / `Ak*`。
- 不引入第二套 `C++` 包装导出层。
- 公开头和导出符号在 `MSVC` / `MinGW` 下都成立。
- 不把宿主介质结构、CLI 结构、SetupAPI 设备收敛逻辑带进 `AppKernel`。

### Step 2. 实现 session 层最小闭环

状态：

- 当前唯一下一步。

目标：

- 落 `AkOpen` / `AkClose` / `AkQuerySessionState` / `AkQuerySessionStats`。
- 把 `OpenFirstDeviceInterface`、`QuerySessionId`、`SendHeartbeat` 这一组 `KMDF session` 能力抽到 `AppKernel`。
- 建立 session-owned heartbeat 线程、stop event、基础日志入口。
- 固定 `session open -> query info -> query session id -> heartbeat ready` 的启动顺序。
- 固定 `close -> stop heartbeat -> close handles -> release session object` 的关闭顺序。

完成定义：

- `AkOpen` 后可以得到有效 `SessionId`。
- `AkClose` 返回后不再有 session 级后台线程存活。
- 生命周期状态只在 session 内维护，不向宿主复制一份镜像状态。
- `LogFn` 能覆盖 session open/close/heartbeat 的关键路径。

### Step 3. 实现 session 级事件队列

目标：

- 落 `AkWaitEvent` / `AkPollEvent`。
- 建立 growable FIFO 事件队列和 wait primitive。
- 先支持：
  - `AkEventDiskOnline`
  - `AkEventDiskRemoved`
  - `AkEventSessionBroken`
  - `AkEventWriteFinalCommitted`
  - `AkEventWriteFinalRejected`

完成定义：

- 事件队列成为写最终裁决和生命周期通知的唯一出口。
- 如果事件无法保留，session 进入 `Broken`，而不是静默丢事件。
- 宿主不需要再额外维护一套写完成回调链。

### Step 4. 实现协议基础层

目标：

- 把 `DeviceIoControl` / overlapped 发送、完成、同步短命令封装收进 `AppKernel`。
- 建立 `QUERY_INFO`、`CREATE_DISK`、`REMOVE_DISK`、`REMOVE_ALL_DISKS`、`QUERY_DEBUG_STATE` 的短命令路径。
- 建立 `POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH` 的异步基础设施。
- 固定同步短命令和异步 slot transport 的内部职责边界，避免后续再混成一锅。

完成定义：

- 协议收口只留一套 `AppKernel` 内部实现。
- 宿主不再自己直通 `DeviceIoControl` 操作这些数据面命令。
- 后续多盘 runtime 只调 `AppKernel` 内部 transport，不再各盘各自拼 `IOCTL`。

### Step 5. 实现 per-disk runtime 骨架

目标：

- 落 `AkCreateDisk` / `AkRemoveDisk` / `AkQueryDiskState` / `AkQueryDiskStats`。
- 建立 per-disk runtime、read workers、write workers、ack flush worker。
- 固定建盘顺序：
  1. 创建 runtime
  2. 启动 worker
  3. 先让 read slot 进入可用
  4. 再发 `CREATE_DISK`
- 把当前 `RWTestApp` 已验证通过的多盘模型原样迁入 `AppKernel`：
  - 按盘持有 runtime
  - 按盘持有 slot 深度
  - 按盘持有 read/write/ack worker
  - 按盘独立统计和生命周期

完成定义：

- 结构上彻底避免再回到“有设备无盘”的旧问题。
- `QueueDepth` 明确按盘解释，不与其他盘共享。
- 多盘完整并发成立，不回退到旧单盘 `Q` 队列模型。

### Step 6. 实现读路径

目标：

- `POST_READ_SLOT` 完成后，调用宿主 `read_bytes` 取得 overlay 后可见读视图。
- 随后发 `READ_ACK`。
- 读错误只影响单个事件，不放大成整盘失败。
- 读路径继续按盘并发推进，不引入跨盘共享热点队列。

完成定义：

- `AppKernel` 不持有介质数据。
- `read_bytes` 并发调用成立。
- late ACK / stale ACK 仍交由 `SCSI` 最终判定。

### Step 7. 实现写路径和最终裁决

目标：

- `POST_WRITE_SLOT` 完成后，调用宿主 `stage_write` 先写暂存层。
- 然后发 `WRITE_ACK_BATCH`。
- 在 `AppKernel` 内按 `EventId` 聚合整笔系统写的 finalize 状态。
- 全部 accepted 才入队 `AkEventWriteFinalCommitted`。
- 任一 fragment 最终 rejected 就入队 `AkEventWriteFinalRejected`。
- 写路径也继续保持按盘并发，不引入全局串行 finalize 核心。

完成定义：

- 写路径严格收束为：
  1. `stage_write`
  2. `WRITE_ACK_BATCH`
  3. 最终事件
- 宿主只根据最终事件决定 `commit` 或 `discard`。
- 不恢复 piggyback ACK，不增加第二套写完成通知模型。

### Step 8. 宿主接入与旧路径删除

目标：

- 把 `RWTestApp` 改成 `AppKernel` 宿主。
- 宿主只保留：
  - 驱动安装/收敛
  - 介质对象
  - staging journal
  - overlay read 视图
  - CLI/压测辅助
  - 可见盘枚举
- 删除 `RWTestApp` 里旧的数据面线程、slot runtime、ACK flush、相关镜像统计。
- 切换时要保证宿主语义不退化：
  - 多盘并发模型保持不变
  - 单盘 `Q1/Q2/Q8/Q32` 行为保持可承接
  - 多盘同时压测的运行方式保持可承接

完成定义：

- `RWTestApp` 不再直接持有 `POST_*_SLOT` / `READ_ACK` / `WRITE_ACK_BATCH` 数据面实现。
- 新旧双轨不存在。
- `RWTestApp` 只是宿主，不再是另一套数据面内核。

### Step 9. 编译与验证口径

目标：

- 每完成一个可闭环步骤就保证编译通过。
- DLL 骨架阶段就至少验证一次 `MSVC` 构建和一次 `MinGW` 构建。
- 接入宿主后再做最小验证闭环：
  - `ct`
  - 系统可见盘
  - 基础读写
  - `rm`
  - `exit`
- 之后再做高风险验证：
  - `Q1/Q2/Q8/Q32`
  - `Ctrl+C`
  - session close
  - late ACK
  - 多盘并发
  - 多盘同时压测下的吞吐与稳定性

完成定义：

- 当前阶段先以“编译通过 + 结构正确”为 gate。
- 压测和取消验证放在宿主切换后统一收口。
- 验证口径必须覆盖“单盘稳定”和“多盘模型未退化”两个维度。

### Step 10. 编写 SDK 文档

目标：

- 在 `windows/AppKernel` 下补正式 `SDK` 文档。
- 文档至少覆盖：
  - DLL 交付内容
  - 头文件与链接方式
  - `MSVC` / `MinGW` 接入方式
  - `AK_SESSION` / `AK_DISK` 生命周期
  - `AK_MEDIA_OPS` 语义
  - 事件消费模型
  - 写暂存与最终裁决模型
  - 状态与统计接口用途区别
  - 常见错误码和调用约束
- 给出最小宿主接入示例。
- 明确写出多盘模型约束，避免后续宿主接入者误以为 `AppKernel` 是单盘核心外包一层多盘外壳。

完成定义：

- 宿主接入不需要回头翻 `AppKernel` 内部源码猜接口。
- `SDK` 文档与导出头文件、实现、正式设计文档保持一致。

## 5. 删除清单

后续切换 `RWTestApp -> AppKernel` 时，以下旧结构必须进入删除范围：

- `RWTestApp` 内现有 `ManagedDisk` 数据面 runtime。
- `RWTestApp` 内现有 read/write worker 实现。
- `RWTestApp` 内现有 write ACK flush worker。
- `RWTestApp` 内现有 slot/ACK 统计和调试快照镜像状态。
- 宿主直通 `DeviceIoControl` 的数据面命令路径。

保留项只包括：

- 驱动收敛。
- 介质与暂存层。
- overlay 读视图。
- CLI。
- 可见盘/`PhysicalDriveX` 枚举。

## 6. 验收标准

本轮 `todo` 收口后的最终验收标准：

- `windows/AppKernel` 独立成型，能够单独参与构建。
- `windows/AppKernel` 以 DLL + 公开头文件形式交付。
- `windows/AppKernel` 同时附带可直接用于宿主接入的 `SDK` 文档。
- `MSVC` / `MinGW` 都能完成构建和链接。
- `AppKernel` 成为唯一用户态数据面内核。
- 宿主和 `AppKernel` 的职责边界与正式设计文档一致。
- 当前 `RWTestApp` 的多盘并发模型被完整迁入 `AppKernel`，而不是回退成旧单盘 `Q` 队列模型。
- 旧数据面结构在切换完成后被删除，而不是挂着不用。
- 文档、实现、`progress` 三者描述一致。

## 7. 当前唯一下一步

当前唯一下一步：

- 实现 `Step 2`：把 `AkOpen / AkClose / AkQuerySessionState / AkQuerySessionStats` 从当前占位实现推进到真实 `KMDF session` 最小闭环，收口 `control device open`、`QueryInfo / SessionId`、heartbeat 线程与同步关闭链路。
