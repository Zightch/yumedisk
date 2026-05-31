# AppKernel 设计文档

本文档定义 `AppKernel` 的正式目标方案。

宿主侧接入、导出 ABI 和调用语义见 [AppKernel-SDK文档](./AppKernel-SDK文档.md)。

当前代码里，`AppKernel` 已经作为独立用户态数据面内核落地；当前正式主宿主由 `BackendRust` 直接承接，`cpp-cli` 和 `rust-cli` 作为辅助宿主保留。本文件描述的是必须长期保持的唯一边界，而不是历史草案。

## 1. 目标

`AppKernel` 的职责只有一件事：把用户态数据面从业务宿主里剥出来，形成一个只负责 `KMDF` 会话、per-disk slot runtime、ACK 和线程调度的纯 `C` 内核，并以 `DLL` 形式存在。

重构后，用户态分层固定为：

1. 业务宿主层
2. `AppKernel`
3. `YumeDiskKMDF`
4. `YumeDiskSCSI`

其中：

- 当前正式业务宿主主线由 `BackendRust + tauri-client` 承载。
- `AppKernel` 是唯一用户态数据面内核。
- 不保留“宿主直连驱动实现”和“`AppKernel` 实现”双轨长期并存。

## 2. 分层与职责

## 2.1 业务宿主层

业务宿主层负责：

- 单实例。
- 驱动包安装、自修复、devnode 收敛。
- 介质对象创建与销毁。
- 正式介质和暂存层管理。
- 对外可见读视图实现。
- 写暂存提交与丢弃。
- LBA overlap 一致性策略。
- CLI、服务控制、压测辅助。

业务宿主层不负责：

- `POST_READ_SLOT` / `POST_WRITE_SLOT` 预投。
- `READ_ACK` / `WRITE_ACK_BATCH` 协议推进。
- per-disk 数据面线程管理。
- `KMDF` 控制会话细节。

## 2.2 AppKernel

`AppKernel` 负责：

- 打开、维护、关闭 `YumeDiskKMDF` 会话。
- heartbeat。
- 短控制命令转发。
- 为每个盘创建独立 runtime。
- 为每个盘维护 read workers、write workers、write ACK flush worker。
- 预投 `POST_READ_SLOT` 和 `POST_WRITE_SLOT`。
- 收到 read slot 后调用业务宿主 `read` 介质操作，再发 `READ_ACK`。
- 收到 write slot 后调用业务宿主 `stage_write` 写入暂存层，再发 `WRITE_ACK_BATCH`。
- 根据 `WRITE_ACK_BATCH` 结果聚合每笔系统写的最终状态。
- 维护 session 级 finalize/event 队列，向业务宿主报告写最终裁决和运行结果。
- 暴露 session/disk 的状态快照和统计快照。

`AppKernel` 不负责：

- 持有介质字节。
- 持有介质锁。
- 解释系统取消语义给业务层。
- 枚举系统设备路径。
- 驱动安装与设备实例修复。
- CLI 与 benchmark 控制。

## 2.3 YumeDiskKMDF / YumeDiskSCSI

驱动侧职责保持不变：

- `YumeDiskKMDF` 只负责 session、watchdog、direct-I/O transport、miniport 代理。
- `YumeDiskSCSI` 只负责 per-target queue、系统 SRB、ACK 判定和取消收口。

`AppKernel` 不能把驱动侧职责重新抬回用户态。

当前实现进度补充：

- `session` 最小闭环已经落地。
- `session-owned` response / notice 队列已经落地。
- `protocol transport` 已经开始独立成层：
  - 同步短命令统一走一套内部封装。
  - 异步 slot I/O 的 `OVERLAPPED` 生命周期和 wait/finish/cancel 已统一收口。
  - 后续 per-disk runtime 只调用 protocol transport，不再自行拼底层 `IOCTL`。

## 3. 单一真实来源

状态归属固定如下：

- `KMDF session` 生命周期：只在 `AppKernel session` 内。
- per-disk worker、slot、ACK flush 运行态：只在 `AppKernel disk runtime` 内。
- 正式介质、暂存层、叠加读视图和介质锁：只在业务宿主 disk object 内。
- 设备实例收敛：只在业务宿主控制层内。
- 系统请求取消最终判定：只在 `YumeDiskSCSI` 内。

禁止出现：

- `AppKernel` 再持有一份 `medium`。
- 业务宿主再镜像一份 slot runtime。
- 业务宿主和 `AppKernel` 同时维护 pending ACK 状态。
- 业务宿主自己再包一层外部数据面线程池。

## 4. 对外模型

## 4.1 句柄模型

`AppKernel` 只暴露两个 opaque handle：

```c
typedef struct AK_SESSION AK_SESSION;
typedef struct AK_DISK AK_DISK;
```

业务宿主只能通过这两个句柄和 `AppKernel` 导出的 `C ABI` 交互，不允许直接穿透到底层 `DeviceIoControl`。

## 4.1.1 ABI 交付边界

`AppKernel` 的交付形式固定为：

- 一个 `Windows DLL`
- 一套稳定的 `C` 导出函数
- 一份与导出接口对应的公开头文件
- 一份与公开头文件和 DLL 导出边界对应的 `SDK` 文档

固定约束：

- `AppKernel` 内部实现继续按纯 `C` 收敛，不引入第二套 `C++` 宿主包装 API。
- 导出边界只暴露 opaque handle、POD 结构和函数指针回调。
- 宿主只能依赖公开头文件和 DLL 导出，不允许依赖内部结构布局。
- 当前阶段只承诺“本仓当前唯一实现”的导出边界，不额外承诺历史兼容 ABI。
- `AppKernel` 的 DLL 构建和宿主接入必须同时兼容 `MSVC` 和 `MinGW`。
- 公开头文件不得依赖只对单一工具链成立的扩展语义；导出宏、调用约定、整数类型和结构体布局必须走两种工具链都能成立的 `C ABI` 写法。

为此固定要求：

- 公开头使用统一的 `AK_API` / `AK_CALL` 一类宏收口 `dllexport/dllimport` 和调用约定。
- 导出函数必须使用 `extern "C"` 语义，避免 C++ name mangling。
- 公开结构只使用稳定的 Win32/C 基础类型和明确宽度整数类型，不暴露 STL、异常、RTTI 或编译器私有对象模型。
- `CMake` 和样例宿主在落地阶段必须至少验证一次 `MSVC` 构建与一次 `MinGW` 构建。
- `SDK` 文档必须与当前导出函数、结构定义、状态模型、事件模型和宿主接入方式保持一致，不记录历史分叉。

## 4.2 介质操作

业务宿主需要提供“可见读视图”和“写暂存”两类最小能力。

```c
typedef struct AK_READ_OP {
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT64 Lba;
    UINT64 OffsetBytes;
    UINT32 BlockCount;
    UINT32 DataLength;
    UINT32 Flags;
} AK_READ_OP;

typedef struct AK_WRITE_OP {
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 Seq;
    UINT32 TotalSeq;
    UINT64 Lba;
    UINT64 OffsetBytes;
    UINT32 ByteOffsetInWrite;
    UINT32 DataLength;
    UINT32 Flags;
} AK_WRITE_OP;

typedef struct AK_DISK_OPS {
    LONG (*read_bytes)(
        void* disk_ctx,
        const AK_READ_OP* op,
        void* out_buffer,
        UINT32* out_data_length);

    LONG (*stage_write)(
        void* disk_ctx,
        const AK_WRITE_OP* op,
        const void* data_buffer,
        UINT32 data_length);

    VOID (*on_event)(
        void* disk_ctx,
        const AK_DISK_EVENT* event_record);
} AK_DISK_OPS;
```

约束：

- `read_bytes` / `stage_write` 可以被同盘并发调用。
- `read_bytes` 必须返回业务宿主当前“对系统可见”的读视图，而不是只读正式介质。
- 如果对应区间存在尚未最终裁决的 staged write，`read_bytes` 必须先命中暂存层，再回落到正式介质。
- `stage_write` 只写暂存层，不直接改正式介质。
- 介质锁、staging journal 和一致性策略由业务宿主自己保证。
- `AppKernel` 不参与 LBA overlap 排序。

## 4.3 事件模型

`AppKernel` 当前公开三条事件语义：

- `AK_RESPONSE`
  - 承接宿主主动发起流程后的结果返回。
- `AK_SESSION_NOTICE`
  - 承接 session 级异步通知。
- `AK_DISK_EVENT`
  - 承接单盘级驱动主动事件。

```c
typedef enum AK_RESPONSE_TYPE {
    AkResponseDiskOnline,
    AkResponseDiskRemoved,
    AkResponseWriteFinalCommitted,
    AkResponseWriteFinalRejected
} AK_RESPONSE_TYPE;

typedef struct AK_RESPONSE {
    AK_RESPONSE_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 TotalSeq;
    UINT32 Flags;
    LONG Status;
} AK_RESPONSE;

typedef enum AK_SESSION_NOTICE_TYPE {
    AkSessionNoticeBroken
} AK_SESSION_NOTICE_TYPE;

typedef struct AK_SESSION_NOTICE {
    AK_SESSION_NOTICE_TYPE Type;
    UINT32 Flags;
    LONG Status;
} AK_SESSION_NOTICE;

typedef enum AK_DISK_EVENT_TYPE {
    AkDiskEventSystemEjected
} AK_DISK_EVENT_TYPE;

typedef struct AK_DISK_EVENT {
    AK_DISK_EVENT_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT32 Flags;
    LONG Status;
} AK_DISK_EVENT;
```

固定规则：

- response 队列和 session notice 队列分开维护、分开消费。
- `AkResponseDiskOnline`、`AkResponseDiskRemoved`、`AkResponseWriteFinalCommitted`、`AkResponseWriteFinalRejected` 只允许进入 response 队列。
- `AkSessionNoticeBroken` 只允许进入 session notice 队列。
- `AK_DISK_EVENT` 只允许从单盘 `event slot` 上行，不回塞到 response 或 notice。
- 当前已经打通 `AkWaitResponse / AkPollResponse` 和 `AkWaitSessionNotice / AkPollSessionNotice` 两条公开消费口。
- 当前最小闭环下只定义 `AkDiskEventSystemEjected` 类型，不在本节承诺完整系统弹出实现。

## 5. 对外接口

## 5.1 打开参数

```c
typedef VOID (*AK_LOG_FN)(
    void* log_ctx,
    INT level,
    const char* text);

typedef struct AK_OPEN_PARAMS {
    UINT32 HeartbeatIntervalMs;
    UINT32 InitialResponseQueueCapacity;
    UINT32 InitialSessionNoticeQueueCapacity;
    AK_LOG_FN LogFn;
    void* LogCtx;
} AK_OPEN_PARAMS;
```

约束：

- `HeartbeatIntervalMs` 是 session 内唯一 heartbeat 周期来源。
- `InitialResponseQueueCapacity` 必须能覆盖稳态下的 in-flight 写最终事件缓存。
- `InitialSessionNoticeQueueCapacity` 是 session notice 队列的初始容量。

## 5.2 磁盘参数

```c
typedef struct AK_DISK_PARAMS {
    UINT32 TargetId;
    UINT32 SectorSize;
    UINT64 DiskSizeBytes;
    UINT32 QueueDepth;
    UINT32 WriteSlotBytes;
    UINT16 ReadWorkerCount;
    UINT16 WriteWorkerCount;
    UINT32 AckBatchMaxRanges;
    UINT32 ReadOnly;
} AK_DISK_PARAMS;
```

约束：

- `QueueDepth` 按盘解释，不与其他盘共享。
- `ReadWorkerCount` / `WriteWorkerCount` 是小常数配置，不是拿来硬堆 QD 的扩展点。
- `WriteSlotBytes` 和 `AckBatchMaxRanges` 由 `AppKernel` 统一执行，不允许业务宿主绕过。
- `ReadOnly` 只是建盘静态属性透传，`AppKernel` 不单独维护一套只读数据面分支。

## 5.3 会话接口

```c
LONG AkOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session);

VOID AkClose(
    AK_SESSION* session);

LONG AkRemoveAllDisks(
    AK_SESSION* session);

LONG AkQuerySessionState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state);

LONG AkQuerySessionStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats);

LONG AkWaitResponse(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_RESPONSE* out_response);

LONG AkPollResponse(
    AK_SESSION* session,
    AK_RESPONSE* out_response);

LONG AkWaitSessionNotice(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_SESSION_NOTICE* out_notice);

LONG AkPollSessionNotice(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice);
```

语义：

- `AkClose` 是同步 drain；返回后，不再有任何介质回调和后台 worker 存活。
- `AkQuerySessionState` / `AkQuerySessionStats` 只读快照，不驱动状态流转。
- `AkWaitResponse` 取到一个 response 就返回；超时返回 `STATUS_TIMEOUT`。
- `AkPollResponse` 不阻塞；无 response 返回 `STATUS_NO_MORE_ENTRIES`。
- `AkWaitSessionNotice` / `AkPollSessionNotice` 只消费 session notice。

## 5.4 磁盘接口

```c
LONG AkCreateDisk(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_DISK_OPS* disk_ops,
    void* disk_ctx,
    AK_DISK** out_disk);

LONG AkRemoveDisk(
    AK_DISK* disk);

LONG AkNotifyDiskDataChanged(
    AK_DISK* disk);

LONG AkQueryDiskState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state);

LONG AkQueryDiskStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats);
```

`AkCreateDisk` 的固定顺序必须是：

1. 创建 disk runtime。
2. 启动 read/write/ack workers。
3. 先把该盘 read slot 预投到可用状态。
4. 再向 `SCSI` 发送 `CREATE_DISK`。

这样可以避免再次回到“有设备无盘”的老问题。

`AkRemoveDisk` 的固定语义必须是：

1. 停止该盘继续预投新 slot。
2. drain 或取消该盘用户态 in-flight 请求。
3. 发送 `REMOVE_DISK`。
4. 停止该盘 worker 并回收 runtime。

返回后，业务宿主可以安全销毁自己的 `disk_ctx`。

`AkNotifyDiskDataChanged` 的固定语义必须是：

1. 只接受仍处于 `Running` 的盘 runtime。
2. 只表达“底层内容已变”，不表达 remove / recreate / invalid。
3. 同步下发 `YumeDiskCommandNotifyDataChanged` 到下层驱动链。
4. 不新增上行 event，不引入 generic change kind。

## 5.4.1 会话版本准入

`AppKernel` 不再单独维护 `ProtocolVersion`。版本号本身就是协议和行为契约的硬门槛。

三方组件：

- `YumeDiskKMDF`
- `YumeDiskSCSI`
- `AppKernel`

都只维护一个 `VersionBe`。

规则固定为：

- 版本格式是 `A.B.C.D` 四段十进制。
- 每段范围 `0-255`。
- 底层存储为大端序 `UINT32`。
- 这个版本同时代表：
  - 二进制版本
  - 协议版本
  - 行为契约版本
- 兼容性只通过 `VersionBe` 相等判定，不再额外引入 `Features`、`ProtocolVersion` 或其他能力位准入。

`AkOpen` 的固定准入顺序必须是：

1. 打开 `YumeDiskKMDF` 控制设备。
2. 查询 `KMDF.VersionBe`。
3. 查询 `SCSI.VersionBe`。
4. 校验 `KMDF.VersionBe == AppKernel.VersionBe`。
5. 校验 `SCSI.VersionBe == AppKernel.VersionBe`。
6. 只有全部通过，才允许 session 进入 `Running`。

下列任一情况都必须让 `AkOpen` 直接失败，不允许降级继续工作：

- `KMDF` 版本查询失败。
- `SCSI` 版本查询失败。
- `KMDF` 版本与 `AppKernel` 不一致。
- `SCSI` 版本与 `AppKernel` 不一致。

查询接口也必须按组件拆分，不允许继续依赖“单个模糊 `QueryInfo` 再推断版本来源”的旧做法。至少要有：

- `QueryKmdfInfo`
- `QueryScsiInfo`

版本常量必须只有一个共享真源头；`KMDF / SCSI / AppKernel` 都从同一处编译进来，不允许三处各自手填。

## 5.5 状态与统计接口

`AppKernel` 必须把“当前状态”和“累计统计”分开，不允许混成一个大杂烩结构。

状态只回答“现在是什么样”：

```c
typedef enum AK_LIFECYCLE_STATE {
    AkStateInit,
    AkStateStarting,
    AkStateRunning,
    AkStateRemoving,
    AkStateClosing,
    AkStateClosed,
    AkStateBroken
} AK_LIFECYCLE_STATE;

typedef struct AK_SESSION_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT64 SessionId;
    BOOLEAN HeartbeatRunning;
    BOOLEAN TransportReady;
    UINT32 DiskCount;
    LONG LastError;
} AK_SESSION_STATE;

typedef struct AK_DISK_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    BOOLEAN ReadWorkersRunning;
    BOOLEAN WriteWorkersRunning;
    BOOLEAN AckFlusherRunning;
    LONG LastError;
} AK_DISK_STATE;
```

统计只回答“累计发生了多少事”：

```c
typedef struct AK_SESSION_STATS {
    UINT64 HeartbeatSent;
    UINT64 CommandFailures;
    UINT64 ProtocolFailures;
    UINT64 ResponsesQueued;
    UINT64 ResponsesDropped;
    UINT64 SessionNoticesQueued;
    UINT64 SessionNoticesDropped;
} AK_SESSION_STATS;

typedef struct AK_DISK_STATS {
    UINT64 ReadSlotPosts;
    UINT64 ReadSlotCompletions;
    UINT64 ReadAckCommands;
    UINT64 WriteSlotPosts;
    UINT64 WriteSlotCompletions;
    UINT64 WriteAckFlushes;
    UINT64 WriteAckRanges;
    UINT64 WriteAckRangeFailures;
    UINT64 FinalWriteCommitted;
    UINT64 FinalWriteRejected;
} AK_DISK_STATS;
```

固定规则：

- `state` 里不放累计 counter。
- `stats` 里不放 `Running/Closing/Broken` 之类生命周期字段。
- 业务宿主拿 `state` 做决策，拿 `stats` 做观测和调试。

## 6. 写路径语义

写路径固定分成三件事：

1. `stage_write`
2. `WRITE_ACK_BATCH`
3. 最终提交或丢弃事件

严格顺序必须是：

1. `AppKernel` 从 `POST_WRITE_SLOT` 收到 fragment。
2. `AppKernel` 先调用业务宿主 `stage_write`，把 fragment 写入暂存层。
3. `AppKernel` 再发送 `WRITE_ACK_BATCH`。
4. `AppKernel` 根据 batch 结果更新该 `EventId` 的内部 finalize 状态：
   - 全部 `seq` 都被 `SCSI` 接受后，入队一次 `AkResponseWriteFinalCommitted`。
   - 任一 `seq` 被最终拒绝后，入队一次 `AkResponseWriteFinalRejected`，并终止该 `EventId` 的后续 committed 可能。
5. 业务宿主收到最终事件后：
   - `AkResponseWriteFinalCommitted`：把该 `EventId` 的 staged write 合并到正式介质。
   - `AkResponseWriteFinalRejected`：把该 `EventId` 的 staged write 直接丢弃。

这里的最终事件不是“附加通知”，而是 staging 模型的一部分。

固定语义：

- `stage_write` 成功不代表这笔系统写已经最终成立。
- 只有 `AkResponseWriteFinalCommitted` 才允许业务宿主把 staged write 转成正式介质。
- `AkResponseWriteFinalRejected` 到达前，业务宿主必须保留该 `EventId` 的 staged write 记录。

## 7. 取消模型

`AppKernel` 不改变现有取消边界：

- 系统侧取消只追到 `SCSI`。
- `AppKernel` 不向业务宿主上浮“某个系统请求已取消”的额外协议。
- 业务宿主继续只实现可见读视图、暂存写入、暂存提交和暂存丢弃。
- `READ_ACK` / `WRITE_ACK_BATCH` 可以晚到。
- stale / cancelled / not found 仍由 `SCSI` 在 ACK 到达时最终判定。

因此：

- `AkResponseWriteFinalRejected` 可能对应真正失败，也可能对应取消后的晚到 ACK。
- 业务宿主只能把它当作“这笔 staged write 不能再提交”，不能反推系统仍然保留原始请求。

## 8. 线程与并发规则

`AppKernel` 是用户态唯一数据面线程所有者。

固定线程模型：

- per-disk read workers。
- per-disk write workers。
- per-disk write ACK flush worker。
- session 级 heartbeat 线程。
- session 级 response 队列。
- session 级 session notice 队列。

固定规则：

- 业务宿主不允许再包一层外部 read/write worker pool。
- `AppKernel` 内部线程只负责协议推进和调度，不持有介质锁。
- `disk_ctx` 必须在 `AkRemoveDisk` 或 `AkClose` 返回前一直有效。
- `AkClose` / `AkRemoveDisk` 返回后，不再有任何后台线程访问对应 `disk_ctx`。
- 业务宿主必须保证有专门的消费路径持续调用 `AkWaitResponse` 或 `AkPollResponse`，及时处理 staged write 的最终事件。

## 9. 当前不做的事情

本方案明确不做：

- plugin 化。
- 多宿主 ABI 稳定承诺。
- “AppKernel C + AppKernel C++ 双实现”。
- 业务宿主直通底层 `IOCTL` 的旁路接口。
- 旁路绕过 staging/finalize 语义直接改正式介质。

## 10. 落地要求

当前阶段按以下顺序继续收口：

1. 保持 `AppKernel` 作为唯一用户态数据面内核，不把协议运行时重新散回宿主。
2. 保持正式介质、暂存层、overlay 读视图和介质锁只归业务宿主所有。
3. 继续让正式主宿主和辅助宿主都通过 `AppKernel` 接入，不再新增旁路实现。
4. 同步维护 `AppKernel SDK` 文档，收敛导出接口、调用时序、宿主职责和错误语义。

收口后的最终形态必须是：

- 业务宿主只拥有控制和介质。
- `AppKernel` 只拥有协议和运行时。
- 驱动只拥有 session、target queue 和系统 I/O。
- DLL 导出边界在 `MSVC` / `MinGW` 下都可构建、可链接、可被宿主调用。
- `SDK` 文档能够单独指导宿主完成接入，而不需要回头翻实现源码猜接口语义。

