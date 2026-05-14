# AppKernel SDK文档

本文档面向 `AppKernel` 的宿主接入方，描述当前唯一有效的 `DLL + C ABI` 使用方式。

如果你在看的是：

- `TestApp` 如何接入 `AppKernel`
- 业务宿主需要实现哪些回调
- 写暂存、最终提交、最终拒绝如何闭环
- `AkOpen / AkCreateDisk / AkWaitEvent / AkClose` 应该怎么用

看这份文档即可。

如果你需要的是：

- 为什么系统整体要拆成 `业务宿主 -> AppKernel -> KMDF -> SCSI`
- 各层职责为什么这样划
- 未来结构为什么不能回到旧模型

请看 [AppKernel设计文档](./AppKernel设计文档.md)。

## 目录

1. [目标与边界](#1-目标与边界)
2. [交付物](#2-交付物)
3. [当前版本规则](#3-当前版本规则)
4. [快速接入路径](#4-快速接入路径)
5. [对外接口总览](#5-对外接口总览)
6. [Session API](#6-session-api)
7. [Disk API](#7-disk-api)
8. [宿主必须实现的 AK_MEDIA_OPS](#8-宿主必须实现的-ak_media_ops)
9. [事件模型](#9-事件模型)
10. [写路径真实语义](#10-写路径真实语义)
11. [推荐宿主线程模型](#11-推荐宿主线程模型)
12. [常用状态码](#12-常用状态码)
13. [最小接入示例](#13-最小接入示例)
14. [宿主接入检查清单](#14-宿主接入检查清单)

## 1. 目标与边界

`AppKernel` 是纯 `C` 的用户态数据面内核，以 `Windows DLL` 形式交付。

它只负责：

- `KMDF` 控制会话打开、heartbeat、关闭。
- 按盘维护 read worker、write worker、write ACK flush worker。
- 预投 `POST_READ_SLOT` / `POST_WRITE_SLOT`。
- 驱动协议推进：`READ_ACK` / `WRITE_ACK_BATCH`。
- 将写路径收束为“先 `stage_write`，再等最终事件”。
- 通过 session 级事件队列把最终状态回传给宿主。

它不负责：

- 驱动安装、卸载、修复。
- 系统设备路径枚举。
- 正式介质、暂存层、overlay 读视图。
- 系统取消的全链路上浮。
- CLI、压测、业务逻辑。

当前分层固定为：

1. 业务宿主
2. `AppKernel`
3. `YumeDiskKMDF`
4. `YumeDiskSCSI`

补充固定约束：

- `AppKernel` 不承接系统可见盘枚举。
- 当前正式宿主链路不枚举系统设备路径，建盘成功以 target/runtime/lifecycle/online 为准。

## 2. 交付物

`AppKernel SDK` 的交付边界固定为：

- `AppKernel.dll`
- 对应 import library
- 公开头文件 [include/appkernel.h](./include/appkernel.h)
- 本文档

公开 ABI 约束固定为：

- 纯 `C ABI`
- opaque handle：`AK_SESSION*`、`AK_DISK*`
- 公开结构只使用 `POD + Win32 基础类型`
- 调用约定固定为 `AK_CALL`，当前值为 `__cdecl`
- 宿主侧不要定义 `AK_BUILD_DLL`

当前要求同时兼容：

- `MSVC`
- `MinGW`

仓内最小接入方式可直接参考 [windows/TestApp/CMakeLists.txt](../TestApp/CMakeLists.txt)。

## 3. 当前版本规则

版本规则已经收束为唯一口径，不再单独维护“协议版本”。

- 版本格式：`A.B.C.D`
- 每段范围：`0-255`
- 底层表示：大端序 `UINT32`
- 当前版本：`0.1.0.0`
- 当前常量：`AK_VERSION_BE == 0x00010000`

`AkOpen` 会在打开时做硬校验：

1. 查询 `KMDF` 版本。
2. 查询 `SCSI` 版本。
3. 检查 `KMDF == AppKernel`。
4. 检查 `SCSI == AppKernel`。

只要上述任意一步失败，`AkOpen` 就直接失败，不允许降级继续跑。

固定规则：

- 运行时兼容性只看 `VersionBe`。
- 不再额外查询或判定 `SCSI Features`。
- 不再维护第二套 `ProtocolVersion` / capability gate。

注意：

- `AK_SESSION_STATE` 里的 `AppKernelVersionBe / KmdfVersionBe / ScsiVersionBe` 都是原始 `UINT32`。
- `AppKernel` 不负责把它们转成点分十进制字符串。
- 版本格式化属于业务宿主展示层职责。

## 4. 快速接入路径

宿主的最小接入顺序固定为：

1. 准备日志函数 `AK_LOG_FN`。
2. 调用 `AkOpen` 打开 session。
3. 启动一个专门的事件消费线程，持续 `AkWaitEvent` 或 `AkPollEvent`。
4. 为每个盘准备自己的 `media_ctx` 和 `AK_MEDIA_OPS`。
5. 调用 `AkCreateDisk` 建盘。
6. 收到 `AkEventDiskOnline` 后，更新该 target 的 runtime 状态。
7. 收到 `AkEventWriteFinalCommitted / AkEventWriteFinalRejected` 后，分别做提交或丢弃。
8. 退出时先 `AkRemoveDisk`，最后 `AkClose`。

不要把事件消费做成“有空再看”的附属逻辑。对写路径来说，最终事件是正确性链路的一部分。

## 5. 对外接口总览

公开头文件见 [include/appkernel.h](./include/appkernel.h)。

当前导出函数只有这几组：

- session 生命周期
  - `AkOpen`
  - `AkClose`
  - `AkRemoveAllDisks`
- session 观测
  - `AkQuerySessionState`
  - `AkQuerySessionStats`
  - `AkWaitEvent`
  - `AkPollEvent`
- disk 生命周期
  - `AkCreateDisk`
  - `AkRemoveDisk`
- disk 观测
  - `AkQueryDiskState`
  - `AkQueryDiskStats`

如果后续代码与本文档出现冲突，以 [include/appkernel.h](./include/appkernel.h) 为准；如果头文件变化，本文档也必须同步更新。

## 6. Session API

### 6.1 `AkOpen`

函数：

```c
AK_STATUS AK_CALL AkOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session);
```

输入：

日志回调签名：

```c
typedef VOID(AK_CALL* AK_LOG_FN)(
    void* log_ctx,
    INT level,
    const char* text);
```

结构：

```c
typedef struct AK_OPEN_PARAMS {
    UINT32 HeartbeatIntervalMs;
    UINT32 InitialEventQueueCapacity;
    AK_LOG_FN LogFn;
    void* LogCtx;
} AK_OPEN_PARAMS;
```

约束：

- `params` 不能为空。
- `out_session` 不能为空。
- `HeartbeatIntervalMs` 必须大于 `0`。
- `InitialEventQueueCapacity` 必须大于 `0`。
- `LogFn` 可以为空。
- 如果提供 `LogFn`，它必须是线程安全且尽量短路径的；`AppKernel` 可能从调用线程、heartbeat 线程和 disk worker 线程并发调用它。

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `HeartbeatIntervalMs` | heartbeat 周期，单位毫秒 | session 内唯一 heartbeat 时间源 |
| `InitialEventQueueCapacity` | session 事件队列初始容量 | 不是硬上限；当前实现会按需自动扩容 |
| `LogFn` | 可选日志回调 | 为 `NULL` 时表示宿主不接收 `AppKernel` 日志 |
| `LogCtx` | 原样回传给 `LogFn` 的宿主私有上下文指针 | 常见做法是传宿主 logger、运行时上下文或 `this` 指针 |

`AK_LOG_FN` 参数含义：

| 参数 | 含义 | 备注 |
| --- | --- | --- |
| `log_ctx` | 就是 `AK_OPEN_PARAMS.LogCtx` | 宿主透传上下文 |
| `level` | 日志级别整数值 | 当前实现主要使用 `1=信息`、`3=错误`，其他值暂时按保留处理 |
| `text` | 日志文本 | 窄字符串；只保证本次回调期间可读，宿主若需长期持有应自行复制 |

成功后保证：

- 控制设备已打开。
- `KMDF / SCSI` 版本已完成校验。
- 首次 heartbeat 已发送成功。
- heartbeat 线程已经启动。
- session 已进入 `AkStateRunning`。

常见失败含义：

- `AK_STATUS_NOT_FOUND`
  - 控制设备不存在，通常表示驱动未安装或 devnode 未创建成功。
- `AK_STATUS_NOT_SUPPORTED`
  - 协议或能力路径当前不可用。
  - `AkOpen` 阶段最常见含义是 `KMDF / SCSI / AppKernel` 版本不一致。
- `AK_STATUS_INSUFFICIENT_RESOURCES`
  - 本地对象、事件队列或线程资源不足。
- 其他错误
  - 可通过日志和 `NTSTATUS` 十六进制值继续排查。

### 6.2 `AkClose`

函数：

```c
VOID AK_CALL AkClose(
    AK_SESSION* session);
```

语义：

- 同步关闭。
- 会停止 heartbeat。
- 会销毁 session 下残留的 disk runtime。
- 返回后不再有任何 `AppKernel` 后台线程访问你的 `media_ctx`。

宿主侧固定要求：

- `AkClose` 返回前，所有 `media_ctx`、介质对象、暂存记录都必须继续有效。
- `AkClose` 返回后，宿主才可以安全释放这些资源。

### 6.3 `AkRemoveAllDisks`

函数：

```c
AK_STATUS AK_CALL AkRemoveAllDisks(
    AK_SESSION* session);
```

当前语义要特别注意：

- 它是一个“向驱动发送 `REMOVE_ALL`”的 session 级控制命令。
- 它不是宿主本地 disk runtime 的主生命周期接口。
- 它不会替代 `AkRemoveDisk` 对本地 `AK_DISK*`、worker、`media_ctx` 的同步收口。

推荐用法：

- 作为会话级紧急控制命令使用。
- 正常删盘仍然优先逐盘 `AkRemoveDisk`。
- 最终退出仍然优先 `AkClose` 完整收尾。

### 6.4 `AkQuerySessionState` / `AkQuerySessionStats`

函数：

```c
AK_STATUS AK_CALL AkQuerySessionState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state);

AK_STATUS AK_CALL AkQuerySessionStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats);
```

`state` 和 `stats` 是两类不同信息：

- `state` 只回答“现在是什么状态”
- `stats` 只回答“累计发生过多少次”

`AK_LIFECYCLE_STATE` 枚举值：

| 枚举值 | 数值 | 含义 | 典型出现位置 |
| --- | --- | --- | --- |
| `AkStateInit` | `0` | 对象刚创建，还没开始启动 | session/disk 初建后 |
| `AkStateStarting` | `1` | 正在打开 session 或正在建盘 | `AkOpen`、`AkCreateDisk` 过程中 |
| `AkStateRunning` | `2` | 当前对象处于正常运行态 | 稳态运行期间 |
| `AkStateRemoving` | `3` | 盘对象正在删盘收口 | `AkRemoveDisk` 过程中 |
| `AkStateClosing` | `4` | session 正在关闭收口 | `AkClose` 过程中 |
| `AkStateClosed` | `5` | 对象已经关闭完成 | close/remove 收尾后 |
| `AkStateBroken` | `6` | 运行时已经损坏，不能再继续信任 | heartbeat/transport/协议失效后 |

`AK_SESSION_STATE`：

```c
typedef struct AK_SESSION_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT64 SessionId;
    BOOLEAN HeartbeatRunning;
    BOOLEAN TransportReady;
    UINT32 DiskCount;
    UINT32 AppKernelVersionBe;
    UINT32 KmdfVersionBe;
    UINT32 ScsiVersionBe;
    AK_STATUS LastError;
} AK_SESSION_STATE;
```

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `Lifecycle` | 当前 session 生命周期 | 取值见上面的 `AK_LIFECYCLE_STATE` |
| `SessionId` | 驱动侧 session 标识 | 宿主通常只用于观测和日志 |
| `HeartbeatRunning` | heartbeat 线程是否仍在运行 | `TRUE` 不代表盘一定健康，只表示线程仍在跑 |
| `TransportReady` | 当前控制通道是否仍可用 | 变成 `FALSE` 后当前 session 不再可信 |
| `DiskCount` | 当前 `AppKernel` 已注册的盘数 | 是 runtime 内已注册盘数，不是系统可见盘数 |
| `AppKernelVersionBe` | `AppKernel` 自身版本号，原始大端序 `UINT32` | 需要宿主自己格式化成点分十进制 |
| `KmdfVersionBe` | 当前已连接 `KMDF` 组件版本号，原始大端序 `UINT32` | `AkOpen` 成功后应与 `AppKernelVersionBe` 相等 |
| `ScsiVersionBe` | 当前已连接 `SCSI` 组件版本号，原始大端序 `UINT32` | `AkOpen` 成功后应与 `AppKernelVersionBe` 相等 |
| `LastError` | 最近一次会话级错误状态 | 常用于 `Broken` 后排查 |

`AK_SESSION_STATS`：

```c
typedef struct AK_SESSION_STATS {
    UINT64 HeartbeatSent;
    UINT64 CommandFailures;
    UINT64 ProtocolFailures;
    UINT64 EventsQueued;
    UINT64 EventsDropped;
} AK_SESSION_STATS;
```

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `HeartbeatSent` | 已成功发送的 heartbeat 次数 | 包含 `AkOpen` 内的首次 heartbeat |
| `CommandFailures` | 运行命令失败累计次数 | 包含控制命令、slot 投递、ACK 发送、wait/cancel 等失败 |
| `ProtocolFailures` | 协议层约束失败累计次数 | 例如版本不匹配、事件队列保序失败、协议不支持 |
| `EventsQueued` | 已成功压入 session 事件队列的事件总数 | 不是已消费事件数 |
| `EventsDropped` | 无法保留的事件总数 | 常见于扩容失败等资源异常场景 |

### 6.5 `AkWaitEvent` / `AkPollEvent`

函数：

```c
AK_STATUS AK_CALL AkWaitEvent(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event);

AK_STATUS AK_CALL AkPollEvent(
    AK_SESSION* session,
    AK_EVENT* out_event);
```

行为：

- `AkWaitEvent`
  - 阻塞等待一个事件。
  - 超时返回 `AK_STATUS_TIMEOUT`。
- `AkPollEvent`
  - 非阻塞。
  - 没有事件时返回 `AK_STATUS_NO_MORE_ENTRIES`。

固定规则：

- 事件队列是 session-owned FIFO。
- `AppKernel` 不会帮宿主二次分发事件。
- 最好只让一个宿主线程直接消费 `AkWaitEvent / AkPollEvent`。
- 如果宿主需要 fan-out，请在宿主层自行转发。

## 7. Disk API

### 7.1 `AkCreateDisk`

函数：

```c
AK_STATUS AK_CALL AkCreateDisk(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk);
```

参数：

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

边界约束：

- `TargetId <= 254`
- `SectorSize > 0`
- `DiskSizeBytes > 0`
- `DiskSizeBytes % SectorSize == 0`
- `QueueDepth > 0`
- `WriteSlotBytes > 0`
- `ReadWorkerCount > 0`
- `WriteWorkerCount > 0`
- `AckBatchMaxRanges > 0`
- `ReadOnly == 0` 表示读写盘，`ReadOnly != 0` 表示只读盘
- `media_ops->read_bytes` 不能为空
- `media_ops->stage_write` 不能为空

字段解释：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `TargetId` | 驱动目标盘号 | 同一 session 内必须唯一 |
| `SectorSize` | 对系统暴露的逻辑扇区大小，单位字节 | 例如 `512`、`4096` |
| `DiskSizeBytes` | 虚拟盘总容量，单位字节 | 必须按 `SectorSize` 对齐 |
| `QueueDepth` | 该盘独占的队列深度 | 不与其他盘共享 |
| `WriteSlotBytes` | 单次 slot 数据载荷最大值 | 当前同时约束 write slot 缓冲区和 read payload buffer 上限 |
| `ReadWorkerCount` | 读 worker 数量 | 只用于分摊该盘 `QueueDepth`，不是拿来硬堆 QD |
| `WriteWorkerCount` | 写 worker 数量 | 只用于分摊该盘 `QueueDepth`，不是拿来硬堆 QD |
| `AckBatchMaxRanges` | 单次 `WRITE_ACK_BATCH` 最多携带的 ACK range 数 | 影响 ACK flush 粒度 |
| `ReadOnly` | 是否对系统暴露为只读盘 | `0 = 读写盘`，非 `0 = 只读盘` |

当前建盘顺序固定为：

1. 创建 disk runtime。
2. 启动 read/write/ack worker。
3. 先让 read slot availability 就绪。
4. 再发送 `CREATE_DISK`。
5. 最后入队 `AkEventDiskOnline`。

`AkEventDiskOnline` 表示：

- `AppKernel` 的该盘 runtime 已经跑起来。
- 驱动侧 `CREATE_DISK` 已完成。
- 读路径已具备 probe read 能力。
- 如果 `ReadOnly != 0`，只读语义会由 `SCSI` 统一对系统宣告并拦截系统写请求。

它不表示：

- 系统一定已经完成盘符/磁盘管理器可见性刷新

当前正式宿主链路不以系统设备路径作为建盘成功判据。

### 7.2 `AkRemoveDisk`

函数：

```c
AK_STATUS AK_CALL AkRemoveDisk(
    AK_DISK* disk);
```

固定语义：

1. 将该盘置为 `Removing`。
2. 停止继续投新 slot。
3. 停止并回收该盘 worker。
4. 向驱动发送 `REMOVE_DISK`。
5. 入队 `AkEventDiskRemoved`。
6. 销毁本地 runtime。

返回后保证：

- 该盘的后台线程已经退出。
- `media_ctx` 不会再被访问。
- 宿主可以安全释放该盘介质对象和 staged write 记录。

### 7.3 `AkQueryDiskState` / `AkQueryDiskStats`

函数：

```c
AK_STATUS AK_CALL AkQueryDiskState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state);

AK_STATUS AK_CALL AkQueryDiskStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats);
```

`AK_DISK_STATE`：

```c
typedef struct AK_DISK_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    BOOLEAN ReadWorkersRunning;
    BOOLEAN WriteWorkersRunning;
    BOOLEAN AckFlusherRunning;
    AK_STATUS LastError;
} AK_DISK_STATE;
```

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `Lifecycle` | 当前盘 runtime 生命周期 | 取值见 `AK_LIFECYCLE_STATE` |
| `TargetId` | 该盘 target 编号 | 与建盘时传入的 `AK_DISK_PARAMS.TargetId` 对应 |
| `DiskRuntimeId` | `AppKernel` 为该盘分配的 session 内运行时唯一编号 | 不是 target 的别名 |
| `ReadWorkersRunning` | read worker 线程组是否已经启动并保持运行 | `FALSE` 时该盘读路径不可再视为稳态可用 |
| `WriteWorkersRunning` | write worker 线程组是否已经启动并保持运行 | `FALSE` 时该盘写路径不可再视为稳态可用 |
| `AckFlusherRunning` | write ACK flush worker 是否已经启动并保持运行 | `FALSE` 时写最终裁决无法继续推进 |
| `LastError` | 最近一次盘级错误状态 | 常用于盘级 `Broken` 排查 |

`AK_DISK_STATS`：

```c
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

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `ReadSlotPosts` | 已成功发出的 `POST_READ_SLOT` 次数 | 表示读 slot 预投成功次数 |
| `ReadSlotCompletions` | 已完成返回给用户态的 read slot 次数 | 不等于系统最终读完成次数，但稳态下通常接近 |
| `ReadAckCommands` | 已成功发出的 `READ_ACK` 次数 | 用于观测读 ACK 推进情况 |
| `WriteSlotPosts` | 已成功发出的 `POST_WRITE_SLOT` 次数 | 表示写 slot 预投成功次数 |
| `WriteSlotCompletions` | 已完成返回给用户态的 write slot 次数 | 表示已有多少 write fragment 到达用户态 |
| `WriteAckFlushes` | 已成功执行的 `WRITE_ACK_BATCH` flush 次数 | 用于观测 ACK flush 频率 |
| `WriteAckRanges` | 所有 `WRITE_ACK_BATCH` 累计提交过的 ACK range 数 | 是 range 数，不是系统写数 |
| `WriteAckRangeFailures` | `WRITE_ACK_BATCH` 中被判定失败的 ACK range 累计数 | 包含 stale/cancelled/not found 等失败 |
| `FinalWriteCommitted` | 已生成 `AkEventWriteFinalCommitted` 的系统写总数 | 粒度是整笔系统写 |
| `FinalWriteRejected` | 已生成 `AkEventWriteFinalRejected` 的系统写总数 | 粒度是整笔系统写 |

这些统计主要用于：

- 吞吐观测
- 挂死定位
- ACK 路径排查
- 验证 worker 是否真的在跑

## 8. 宿主必须实现的 `AK_MEDIA_OPS`

`AppKernel` 不持有介质，它只回调宿主：

```c
typedef struct AK_MEDIA_OPS {
    AK_STATUS(AK_CALL* read_bytes)(
        void* media_ctx,
        const AK_READ_OP* op,
        void* out_buffer,
        UINT32* out_data_length);

    AK_STATUS(AK_CALL* stage_write)(
        void* media_ctx,
        const AK_WRITE_OP* op,
        const void* data_buffer,
        UINT32 data_length);
} AK_MEDIA_OPS;
```

回调参数语义：

| 参数 | 出现位置 | 含义 | 备注 |
| --- | --- | --- | --- |
| `media_ctx` | `read_bytes` / `stage_write` | 宿主自有介质对象指针 | 由 `AkCreateDisk` 原样透传 |
| `out_buffer` | `read_bytes` | 需要写入读结果的输出缓冲区 | 宿主负责填充 |
| `out_data_length` | `read_bytes` | 需要回填的实际返回字节数 | 当前实现要求与请求长度匹配 |
| `data_buffer` | `stage_write` | 当前 write fragment 数据载荷 | 长度由 `data_length` 指定 |
| `data_length` | `stage_write` | 当前 write fragment 数据长度 | 应与 `AK_WRITE_OP.DataLength` 一致 |

### 8.1 `read_bytes`

职责：

- 返回宿主当前“对系统可见”的读视图。

这句话的真实含义是：

- 不能只读正式介质。
- 如果请求区间命中了尚未最终裁决的 staged write，必须先覆盖 staged 数据。
- 未命中的区域再回落到正式介质。

也就是宿主自己负责 overlay read view。

`AK_READ_OP`：

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
```

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `TargetId` | 当前读请求所属 target | 用于宿主区分盘 |
| `DiskRuntimeId` | 当前读请求所属 `AppKernel` 盘 runtime 编号 | 可用于日志和宿主内部关联 |
| `EventId` | 当前系统读事件编号 | 宿主通常只用于日志、排查和关联，不需要自行完成它 |
| `Lba` | 本次读请求起始逻辑块地址 | 单位是逻辑块，不是字节 |
| `OffsetBytes` | 本次读请求相对磁盘起点的字节偏移 | 当前实现满足 `OffsetBytes == Lba * SectorSize` |
| `BlockCount` | 本次读请求覆盖的逻辑块数 | 与 `DataLength`、`SectorSize` 对应 |
| `DataLength` | 本次请求要读取的字节数 | 宿主应按这个长度返回数据 |
| `Flags` | 读请求标志位 | 当前保留，按 `0` 处理即可 |

返回要求：

- 成功时返回 `AK_STATUS_SUCCESS`
- 同时写出 `*out_data_length`
- 当前实现要求返回的长度与本次请求匹配

### 8.2 `stage_write`

职责：

- 只把本次 write fragment 写入宿主暂存层
- 不直接改正式介质

`AK_WRITE_OP`：

```c
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
```

字段含义：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `TargetId` | 当前写 fragment 所属 target | 用于宿主区分盘 |
| `DiskRuntimeId` | 当前写 fragment 所属 `AppKernel` 盘 runtime 编号 | 可用于日志和宿主内部关联 |
| `EventId` | 一笔系统 write 的事件编号 | 同一笔系统写的所有 fragment 共用同一个 `EventId` |
| `Seq` | 当前 fragment 在该笔系统写中的序号 | 范围是 `[0, TotalSeq)` |
| `TotalSeq` | 该笔系统 write 总共会拆成多少个 fragment | 宿主可据此判断何时收齐 |
| `Lba` | 当前 fragment 的起始逻辑块地址 | 单位是逻辑块 |
| `OffsetBytes` | 当前 fragment 相对磁盘起点的字节偏移 | 单位字节 |
| `ByteOffsetInWrite` | 当前 fragment 在整笔系统写中的字节偏移 | 不是相对磁盘起点的偏移 |
| `DataLength` | 当前 fragment 数据长度 | 应与 `stage_write` 的 `data_length` 一致 |
| `Flags` | 写请求标志位 | 当前保留，按 `0` 处理即可 |

宿主应按 `EventId` 归并这些 fragment，直到最终事件出来为止。

### 8.3 并发要求

这两个回调都可能被同盘并发调用。

宿主必须自己保证：

- 介质锁正确
- staged write 记录正确
- overlay 读视图正确
- LBA overlap 策略正确

`AppKernel` 不帮你做这些策略层决策。

### 8.4 回调内禁止事项

为了避免把 `AppKernel` worker 自己卡死，宿主回调里不要做这些事：

- 不要在 `read_bytes / stage_write` 里调用 `AkClose`
- 不要在 `read_bytes / stage_write` 里调用 `AkRemoveDisk`
- 不要在回调里长时间等待宿主自己的全局控制锁
- 不要把磁盘文件枚举、驱动安装、CLI 交互塞进回调

回调应该只做：

- 介质访问
- staged write 记录维护
- 必要的轻量日志

## 9. 事件模型

事件结构：

```c
typedef struct AK_EVENT {
    AK_EVENT_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 TotalSeq;
    UINT32 Flags;
    AK_STATUS Status;
} AK_EVENT;
```

事件类型：

```c
typedef enum AK_EVENT_TYPE {
    AkEventDiskOnline = 0,
    AkEventDiskRemoved = 1,
    AkEventWriteFinalCommitted = 2,
    AkEventWriteFinalRejected = 3,
    AkEventSessionBroken = 4
} AK_EVENT_TYPE;
```

`AK_EVENT_TYPE` 枚举值：

| 枚举值 | 数值 | 含义 | 宿主典型动作 |
| --- | --- | --- | --- |
| `AkEventDiskOnline` | `0` | 该盘 runtime 已启动且 `CREATE_DISK` 已完成 | 更新宿主 runtime 状态 |
| `AkEventDiskRemoved` | `1` | 该盘删除收口已经走完 | 释放本地磁盘对象 |
| `AkEventWriteFinalCommitted` | `2` | 一笔系统写已被最终接受 | 将 staged write 提交到正式介质 |
| `AkEventWriteFinalRejected` | `3` | 一笔系统写已被最终拒绝 | 丢弃 staged write |
| `AkEventSessionBroken` | `4` | 当前 session 已损坏不可继续使用 | 停止新流程并进入关闭/重建 |

字段补充：

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `Type` | 当前事件类型 | 决定其余字段如何解释 |
| `TargetId` | 该事件所属 target | `AkEventSessionBroken` 下通常为 `0`，宿主不应依赖它 |
| `DiskRuntimeId` | 该事件所属 `AppKernel` 盘 runtime 编号 | `AkEventSessionBroken` 下通常为 `0` |
| `EventId` | 事件编号 | 对写最终事件表示那笔系统写的 `EventId`；非写事件通常为 `0` |
| `TotalSeq` | 该笔系统写总 fragment 数 | 只对写最终事件有实际意义；非写事件通常为 `0` |
| `Flags` | 事件标志位 | 当前保留，宿主按 `0` 处理即可 |
| `Status` | 事件附带状态码 | `DiskOnline` 通常成功；`DiskRemoved` 表示删盘返回状态；写最终事件表示最终裁决；`SessionBroken` 表示致命错误状态 |

### 9.1 `AkEventDiskOnline`

表示：

- 该盘 runtime 已启动
- `CREATE_DISK` 已完成
- probe read 应该已经能通

宿主典型动作：

- 更新该 target 的 runtime 状态
- 通知上层该盘已进入运行态

### 9.2 `AkEventDiskRemoved`

表示：

- 该盘的 `AppKernel` 生命周期已走到删除收口点

宿主典型动作：

- 释放本地磁盘对象
- 丢弃 staged write
- 刷新可见盘缓存

### 9.3 `AkEventWriteFinalCommitted`

表示：

- 该 `EventId` 下全部 fragment 都已经被 `SCSI` 接受

宿主必须做：

- 把该 `EventId` 的 staged write 合并到正式介质
- 然后删除对应 staged 记录

### 9.4 `AkEventWriteFinalRejected`

表示：

- 该 `EventId` 已经被最终判定失败，不能再提交

这类拒绝可能来自：

- 真实写错误
- 取消后的晚到 ACK
- stale ACK
- not found

宿主必须做：

- 直接丢弃该 `EventId` 的 staged write

### 9.5 `AkEventSessionBroken`

表示：

- 会话已经失效，当前 `AppKernel` session 不再可信

宿主必须做：

1. 停止继续创建新盘或发起新流程。
2. 触发自己的退出或重建流程。
3. 最终调用 `AkClose` 回收本地 session。
4. 对所有还没有最终裁决的 staged write，按宿主清理策略统一丢弃。

要点：

- `AkEventSessionBroken` 只保证“会话坏了”这个事实。
- 它不保证每笔未完成写都会再收到一个 `Rejected` 事件。
- 宿主不能等待“所有未决写都被补齐最终事件”再退出。

## 10. 写路径真实语义

`AppKernel` 的写路径已经固定为 staging 模型：

1. `AppKernel` 收到 `POST_WRITE_SLOT`
2. 调用宿主 `stage_write`
3. 发送 `WRITE_ACK_BATCH`
4. 等待 `SCSI` 最终裁决
5. 通过最终事件通知宿主 `commit` 或 `discard`

这意味着：

- `stage_write` 成功，不代表系统写已经正式成立。
- 只有 `AkEventWriteFinalCommitted` 才允许落正式介质。
- `AkEventWriteFinalRejected` 到来前，宿主必须保留 staged 记录。

当前取消模型也固定为：

- 系统取消只收口到 `SCSI`
- `AppKernel` 不再把“某个系统请求取消”单独上浮给宿主
- 宿主只看最终事件做 `commit / discard`

这套模型正是为了解决“取消时全链路状态分叉”的问题。

## 11. 推荐宿主线程模型

推荐最小宿主线程模型：

- 1 个控制线程
  - 打开 session
  - 建盘删盘
  - 退出收尾
- 1 个事件消费线程
  - 持续 `AkWaitEvent`
  - 在宿主侧做 `commit / discard / runtime 状态刷新`

不要再在宿主层额外包一层 read/write worker pool 去和 `AppKernel` 抢职责。

数据面线程的唯一归属已经固定在 `AppKernel` 内部。

## 12. 常用状态码

当前公开的常用状态码如下：

| 名称 | 值 | 典型含义 |
| --- | --- | --- |
| `AK_STATUS_SUCCESS` | `0x00000000` | 成功 |
| `AK_STATUS_TIMEOUT` | `0x00000102` | `AkWaitEvent` 超时 |
| `AK_STATUS_NO_MORE_ENTRIES` | `0x8000001A` | `AkPollEvent` 无事件 |
| `AK_STATUS_UNSUCCESSFUL` | `0xC0000001` | 泛化失败 |
| `AK_STATUS_INVALID_PARAMETER` | `0xC000000D` | 参数不合法 |
| `AK_STATUS_NOT_FOUND` | `0xC0000225` | 目标不存在，常见于控制设备未就绪 |
| `AK_STATUS_NOT_SUPPORTED` | `0xC00000BB` | 当前协议/能力不支持；`AkOpen` 场景下通常表示版本不匹配 |
| `AK_STATUS_INSUFFICIENT_RESOURCES` | `0xC000009A` | 内存、句柄或线程资源不足 |
| `AK_STATUS_DEVICE_NOT_READY` | `0xC00000A3` | session 或 transport 未准备好 |
| `AK_STATUS_ALREADY_EXISTS` | `0xC0000035` | target 已存在 |
| `AK_STATUS_CANCELLED` | `0xC0000120` | 请求被取消或 slot 收尾取消 |

## 13. 最小接入示例

下面给出一个简化宿主骨架，只表达接线关系，不表达完整业务实现。

```c
static AK_STATUS AK_CALL HostReadBytes(
    void* media_ctx,
    const AK_READ_OP* op,
    void* out_buffer,
    UINT32* out_data_length)
{
    return HostReadOverlay(media_ctx, op, out_buffer, out_data_length);
}

static AK_STATUS AK_CALL HostStageWrite(
    void* media_ctx,
    const AK_WRITE_OP* op,
    const void* data_buffer,
    UINT32 data_length)
{
    return HostStageWriteFragment(media_ctx, op, data_buffer, data_length);
}

void RunHost(void)
{
    AK_OPEN_PARAMS open_params = {0};
    AK_DISK_PARAMS disk_params = {0};
    AK_MEDIA_OPS media_ops = {0};
    AK_SESSION* session = NULL;
    AK_DISK* disk = NULL;

    open_params.HeartbeatIntervalMs = 1000;
    open_params.InitialEventQueueCapacity = 256;
    open_params.LogFn = HostLog;
    open_params.LogCtx = NULL;

    if (AkOpen(&open_params, &session) != AK_STATUS_SUCCESS) {
        return;
    }

    StartEventThread(session);

    media_ops.read_bytes = HostReadBytes;
    media_ops.stage_write = HostStageWrite;

    disk_params.TargetId = 0;
    disk_params.SectorSize = 4096;
    disk_params.DiskSizeBytes = 64ull * 1024ull * 1024ull;
    disk_params.QueueDepth = 32;
    disk_params.WriteSlotBytes = 1024u * 1024u;
    disk_params.ReadWorkerCount = 4;
    disk_params.WriteWorkerCount = 2;
    disk_params.AckBatchMaxRanges = 64;
    disk_params.ReadOnly = 0;

    if (AkCreateDisk(session, &disk_params, &media_ops, host_disk_ctx, &disk) != AK_STATUS_SUCCESS) {
        AkClose(session);
        return;
    }

    WaitForStopSignal();

    AkRemoveDisk(disk);
    AkClose(session);
}
```

事件线程最小处理模型：

```c
for (;;) {
    AK_EVENT event_record = {0};
    AK_STATUS status = AkWaitEvent(session, 1000, &event_record);

    if (status == AK_STATUS_TIMEOUT) {
        continue;
    }
    if (status != AK_STATUS_SUCCESS) {
        break;
    }

    switch (event_record.Type) {
    case AkEventDiskOnline:
        HostMarkDiskOnline(event_record.TargetId);
        break;

    case AkEventWriteFinalCommitted:
        HostCommitStagedWrite(event_record.TargetId, event_record.EventId);
        break;

    case AkEventWriteFinalRejected:
        HostDiscardStagedWrite(event_record.TargetId, event_record.EventId);
        break;

    case AkEventSessionBroken:
        HostStop();
        break;

    default:
        break;
    }
}
```

## 14. 宿主接入检查清单

- 是否先完成驱动安装/修复，再调用 `AkOpen`？
- 是否只用一个线程直接消费 `AkWaitEvent / AkPollEvent`？
- 是否实现了 overlay 读视图，而不是只读正式介质？
- 是否把 write 先写到暂存层，而不是直接落正式介质？
- 是否只在 `AkEventWriteFinalCommitted` 后提交正式介质？
- 是否在 `AkEventWriteFinalRejected` 后丢弃 staged write？
- 是否在 `AkEventSessionBroken` 时停止等待“补齐所有最终事件”？
- 是否保证 `media_ctx` 在 `AkRemoveDisk / AkClose` 返回前一直有效？
- 是否避免在宿主回调里反向阻塞 `AppKernel` 控制 API？
- 是否避免把系统设备路径枚举作为建盘成功判据？
