# AppKernel 与驱动事件语义重建执行清单

## 0. 当前范围

本清单只处理一件事：在正式做“系统单盘弹出”之前，先把 `AppKernel <-> KMDF <-> SCSI` 这一段的语义拆干净。

本轮目标不是直接做弹出功能，而是先把当前混杂的 `AkEvent` 与驱动上行事件面重建为清晰的三条链：

- `AkResponse`
- `AkSessionNotice`
- 盘级 `event slot`

本清单完成后，`todo-eject.md` 再基于新的盘级 `event slot` 去实现真正的系统单盘弹出。

## 1. 版本目标

本轮重建完成后，统一版本号收口为 `0.1.1.0`：

- `shared/yumedisk_proto.h`
- `YumeDiskKMDF`
- `YumeDiskSCSI`
- `AppKernel`

固定要求：

- 不做旧版本兼容
- 不保留 `AkEvent` 旧接口别名
- 不保留“旧 event 语义 + 新 event 语义”双轨并存

## 2. 为什么要先重建

当前 `AkEvent` 把三类完全不同的东西混在了一起：

- app 发起任务后的完成回应
- `AppKernel` 自己观察到的 session 故障
- 后续计划引入的驱动主动盘级事件

这会直接造成边界模糊：

- `AkEvent` 看起来像“驱动事件”，但当前其实主要是 `AppKernel` 自产自销
- `SessionBroken` 不是任务回应，却和 `DiskOnline / DiskRemoved / WriteFinal*` 放在同一个队列
- 后续如果再把 `DiskSystemEjected` 塞进 `AkEvent`，会让“任务回应”和“驱动主动上报”继续混在一起

按当前开发原则，正确做法不是继续往旧 `AkEvent` 上打补丁，而是先拆语义，再接新功能。

## 3. 重建后的正式模型

### 3.1 三条公开语义链

#### A. `AkResponse`

职责：

- 承接 app 主动发起的 session/disk 任务回应

当前只允许包含：

- `AkResponseDiskOnline`
- `AkResponseDiskRemoved`
- `AkResponseWriteFinalCommitted`
- `AkResponseWriteFinalRejected`

固定口径：

- 它是“任务回应面”，不是“驱动主动事件面”
- 它仍然是 session 级消费入口
- 它允许带 `TargetId / DiskRuntimeId / EventId / Status`

#### B. `AkSessionNotice`

职责：

- 承接 session 级异步通知

当前只允许包含：

- `AkSessionNoticeBroken`

固定口径：

- 它不是任务回应
- 它不归属任何单盘
- 它和 `AkResponse` 分开维护、分开消费

#### C. 盘级 `event slot`

职责：

- 承接 `driver -> AppKernel` 的单盘主动事件上报

当前只允许包含：

- `AkDiskEventSystemEjected`

固定口径：

- 它是盘级主动上报面，不走 `AkResponse`
- 它不走 `AkSessionNotice`
- 它不与读写 slot、ACK、heartbeat 混用

### 3.2 三类盘级通道

对单盘固定收成三类通道：

- `read slot`
- `write slot`
- `event slot`

语义说明：

- `read slot`：驱动向上取读任务
- `write slot`：驱动向上投递写任务
- `event slot`：驱动向上报告该盘生命周期事件

当前最小闭环固定为：

- 每盘 `event slot` 始终只保持 `1` 个在途
- `event slot` 只承接一个事件记录，不做批处理
- 当前盘级事件类型只有 `SystemEjected`

## 4. 公开 API 重建

### 4.1 `AkEvent` 整体删除

本轮直接删除旧公开语义：

- `AK_EVENT_TYPE`
- `AK_EVENT`
- `AkWaitEvent`
- `AkPollEvent`

不做：

- `AkWaitEvent` 保留为兼容壳
- 旧 `AK_EVENT_TYPE` 继续承载新旧两种语义

### 4.2 新的 `AkResponse`

建议公开面：

```c
typedef enum AK_RESPONSE_TYPE {
    AkResponseDiskOnline = 0,
    AkResponseDiskRemoved = 1,
    AkResponseWriteFinalCommitted = 2,
    AkResponseWriteFinalRejected = 3
} AK_RESPONSE_TYPE;

typedef struct AK_RESPONSE {
    AK_RESPONSE_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 TotalSeq;
    UINT32 Flags;
    AK_STATUS Status;
} AK_RESPONSE;

AK_STATUS AK_CALL AkWaitResponse(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_RESPONSE* out_response);

AK_STATUS AK_CALL AkPollResponse(
    AK_SESSION* session,
    AK_RESPONSE* out_response);
```

固定要求：

- `AkResponse` 队列只承接任务回应
- `SessionBroken` 不再进入 `AkResponse`

### 4.3 新的 `AkSessionNotice`

建议公开面：

```c
typedef enum AK_SESSION_NOTICE_TYPE {
    AkSessionNoticeBroken = 0
} AK_SESSION_NOTICE_TYPE;

typedef struct AK_SESSION_NOTICE {
    AK_SESSION_NOTICE_TYPE Type;
    UINT32 Flags;
    AK_STATUS Status;
} AK_SESSION_NOTICE;

AK_STATUS AK_CALL AkWaitSessionNotice(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_SESSION_NOTICE* out_notice);

AK_STATUS AK_CALL AkPollSessionNotice(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice);
```

固定要求：

- 当前 notice 只有 `Broken`
- `Broken` 只入一次队
- session 状态依旧以 `AK_SESSION_STATE` 为真，不在外面再维护一份镜像

### 4.4 `AK_MEDIA_OPS` 重命名为 `AK_DISK_OPS`

本轮直接重命名：

- `AK_MEDIA_OPS` -> `AK_DISK_OPS`
- `media_ctx` -> `disk_ctx`

建议公开面：

```c
typedef enum AK_DISK_EVENT_TYPE {
    AkDiskEventSystemEjected = 0
} AK_DISK_EVENT_TYPE;

typedef struct AK_DISK_EVENT {
    AK_DISK_EVENT_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT32 Flags;
    AK_STATUS Status;
} AK_DISK_EVENT;

typedef struct AK_DISK_OPS {
    AK_STATUS(AK_CALL* read_bytes)(
        void* disk_ctx,
        const AK_READ_OP* op,
        void* out_buffer,
        UINT32* out_data_length);

    AK_STATUS(AK_CALL* stage_write)(
        void* disk_ctx,
        const AK_WRITE_OP* op,
        const void* data_buffer,
        UINT32 data_length);

    VOID(AK_CALL* on_event)(
        void* disk_ctx,
        const AK_DISK_EVENT* event_record);
} AK_DISK_OPS;
```

固定要求：

- `on_event` 是盘级主动事件入口
- `on_event` 不返回状态，不向驱动回 ACK
- `on_event` 不承接长阻塞行为，宿主需要自行转异步

## 5. 协议与缓冲区重建

### 5.1 当前口径

本轮只新增盘级 `event slot` 通道，不重写现有读写 slot 主线。

也就是说：

- 现有 `PostReadSlot / PostWriteSlot` 保持
- 现有读写 ACK 保持
- 新增一条独立的 `PostEventSlot / SubmitEventSlot`

这样收的原因：

- 当前重建重点是语义拆分与驱动主动上行
- 不在本轮同时重写整条读写 slot 协议
- 保持改动聚焦，避免重建范围失控

### 5.2 新增协议命令

建议新增：

- `YumeDiskCommandPostEventSlot`
- `YumeDiskCommandSubmitEventSlot`

固定要求：

- 不把 `event slot` 塞进现有 `YumeDiskCommandSubmitSlot`
- 不新增 `YumeDiskSlotTypeEvent` 去糊现有读写数据面
- `event slot` 独立成 dedicated 命令对

### 5.3 新增驱动事件负载

建议新增 transport 负载：

```c
typedef enum _YUMEDISK_DISK_EVENT_KIND {
    YumeDiskDiskEventSystemEjected = 0
} YUMEDISK_DISK_EVENT_KIND;

typedef struct _YUMEDISK_DISK_EVENT {
    UINT32 EventKind;
    UINT32 Flags;
    LONG Status;
    UINT32 Reserved0;
} YUMEDISK_DISK_EVENT, *PYUMEDISK_DISK_EVENT;
```

### 5.4 缓冲区大小

固定为：

- `sizeof(YUMEDISK_DISK_EVENT) == 16`

固定要求：

- 当前 `event slot` 直接回填一个固定 16 字节事件记录
- 不做变长 payload
- 不做批量事件数组
- 不在 transport payload 中重复携带 `TargetId / DiskRuntimeId`

原因：

- `event slot` 本身已经绑定到单盘
- `TargetId / DiskRuntimeId` 应由 `AppKernel` 在本地 runtime 上下文中补齐
- 当前只有 `SystemEjected` 一种事件，不需要更大缓冲区

### 5.5 在途数量

固定为：

- 每盘 `1` 个在途 `event slot`

不做：

- 每盘多个并行 `event slot`
- session 级全局 `event slot`
- `event slot` cancel 语义

## 6. 驱动侧需要做的重建

### 6.1 `shared proto`

需要修改：

- `YUMEDISK_COMPONENT_VERSION_BE` -> `0.1.1.0`
- 新增 `PostEventSlot / SubmitEventSlot`
- 新增 `YUMEDISK_DISK_EVENT_KIND`
- 新增 `YUMEDISK_DISK_EVENT`

需要删除或避免的做法：

- 不给旧 `AkEvent` 留协议兼容桥
- 不给 `event slot` 做“也能走 SubmitSlot”的双入口

### 6.2 `YumeDiskKMDF`

需要新增：

- 每盘 pending `event slot` request 状态
- `PostEventSlot` 的 request 校验、入挂与关闭清理
- `SubmitEventSlot` 的 miniport 异步提交通道
- file/session 关闭时对 pending `event slot` 做一致清理

固定要求：

- `event slot` 按盘绑定，不做 session 级混合队列
- 同一盘同时最多挂一个 pending `event slot`
- 如果重复投递，边界入口直接拒绝

### 6.3 `YumeDiskSCSI`

本轮只做“盘级 `event slot` 基础设施”，不在本任务内实现真正的系统弹出源。

需要新增：

- 单盘 pending `event slot` 记录
- `SubmitEventSlot` 的接收、挂起、完成
- 盘删除 / session close / 驱动故障时的 pending `event slot` 清理

当前明确不做：

- 真正的 Windows 系统弹出接入
- `SCSIOP_START_STOP_UNIT` 的完整弹出语义
- 设备可弹出能力注册

这些都属于后续 `todo-eject.md`。

## 7. AppKernel 需要做的重建

### 7.1 session 公开面拆分

需要新增：

- response queue
- session notice queue

需要删除：

- 旧统一 `event queue` 公开语义

固定要求：

- `DiskOnline / DiskRemoved / WriteFinal*` 走 response queue
- `SessionBroken` 走 notice queue
- 不允许一个类型同时进两条队列

### 7.2 disk runtime 结构重建

需要修改：

- `AK_MEDIA_OPS` -> `AK_DISK_OPS`
- `media_ctx` -> `disk_ctx`
- `AkCreateDisk(...)` 签名及其内部引用

需要新增：

- 每盘 event worker
- 每盘固定 `1` 个 posted `event slot`
- `event slot` 完成后调用 `AK_DISK_OPS.on_event`

固定要求：

- event worker 属于 disk runtime，而不是 session 全局 worker
- 盘关闭时必须先停 event worker，再做 disk destroy
- `on_event` 只在盘仍然存活时调用

### 7.3 response 生产端调整

需要重命名并重接：

- 原 `AkEventDiskOnline` -> `AkResponseDiskOnline`
- 原 `AkEventDiskRemoved` -> `AkResponseDiskRemoved`
- 原 `AkEventWriteFinalCommitted` -> `AkResponseWriteFinalCommitted`
- 原 `AkEventWriteFinalRejected` -> `AkResponseWriteFinalRejected`

### 7.4 session broken 生产端调整

需要重命名并重接：

- 原 `AkEventSessionBroken` -> `AkSessionNoticeBroken`

固定要求：

- 只允许由 session 失效路径注入
- 不允许 disk runtime 伪造 session notice

### 7.5 本轮不做的 AppKernel 行为

本轮重建阶段不做：

- 任何真实 `AkDiskEventSystemEjected` 生产
- 任何系统弹出后的宿主策略
- 任何 `tauri-client` UI 变化

本轮只把语义、接口、线程模型、transport 链路搭好。

## 8. BackendRust / rust-cli 需要同步的最小改动

虽然本清单聚焦 `AppKernel + driver`，但公开 API 改名后，消费端必须同步收口。

### 8.1 BackendRust

需要同步：

- FFI 绑定从 `AkEvent` 切到 `AkResponse`
- 新增 `AkSessionNotice` FFI 绑定
- `DiskRuntime` 创建时把 `AK_DISK_OPS.on_event` 接上
- 当前先把 `AkDiskEventSystemEjected` 作为空实现入口收好，不在本轮做完整 eject 行为

### 8.2 rust-cli

需要同步：

- 原 host event 消费口改为 response + notice 两条消费口
- 编译通过当前命令集
- 不在本轮加入最终 eject 交互逻辑

## 9. 与 `todo-eject.md` 的关系

本清单是 `todo-eject.md` 的前置重建任务。

顺序固定为：

1. 先完成本清单
2. 确认 `AkResponse / AkSessionNotice / event slot` 三条链都已经收口
3. 再进入 `todo-eject.md`

进入 `todo-eject.md` 后，才开始补：

- `YumeDiskSCSI` 接受 Windows 系统弹出
- 单盘清理
- 完成 app 预投的 `event slot`
- `AK_DISK_OPS.on_event` 收到 `AkDiskEventSystemEjected`

## 10. 分阶段执行建议

### Phase A. 文档与协议口径重建

- 更新 `docs/tmp/todo-reconstruction.md`
- 更新 `shared proto` 命名与版本口径
- 更新 `AppKernel` 头文件与 SDK 命名草案

完成标准：

- 公开命名全部切到 `Response / SessionNotice / DiskEvent`
- 版本号全部收口到 `0.1.1.0`

### Phase B. AppKernel 内部语义拆分

- 拆 response queue
- 拆 session notice queue
- 重命名现有生产端
- `AK_MEDIA_OPS` 重命名为 `AK_DISK_OPS`

完成标准：

- `AkWaitResponse / AkPollResponse`
- `AkWaitSessionNotice / AkPollSessionNotice`
- 无 `AkWaitEvent / AkPollEvent` 留存

### Phase C. 驱动盘级 `event slot` 骨架

- `KMDF` 接 `PostEventSlot`
- `SCSI` 接 `SubmitEventSlot`
- 每盘 `1` 个 pending `event slot`
- 关闭 / 删盘 / session close 清理路径补齐

完成标准：

- 整条 `event slot` 链能建立、能关闭、能编译
- 当前允许“无事件长期挂起”

### Phase D. AppKernel 每盘 event worker

- disk runtime 启动 event worker
- 常驻投递 `event slot`
- 完成后调用 `AK_DISK_OPS.on_event`

完成标准：

- 每盘具备独立 `event slot` 生命周期
- 不影响现有读写主线

### Phase E. 消费端编译收口

- BackendRust 适配新 FFI
- rust-cli 适配 response + notice
- 编译通过

完成标准：

- `cargo test`
- `go test` 不受影响
- Windows 相关编译可过

### Phase F. 进入 `todo-eject.md`

- 在已经存在的盘级 `event slot` 之上实现真正的系统单盘弹出

## 11. 验证要求

本清单完成时至少要验证：

- 公开 API 中已不存在 `AkEvent` 旧语义
- `DiskOnline / DiskRemoved / WriteFinal*` 仍能正常返回到 response 面
- `SessionBroken` 能正常从 notice 面取到
- 每盘 `event slot` 能成功建立并在关闭路径被正确取消/清理
- 不做真实 eject 时，整条 `event slot` 骨架长期空闲不应破坏读写

## 12. 明确禁止项

- 不保留 `AkEvent` 兼容别名
- 不让 `SessionBroken` 继续塞在 response 里
- 不把 `DiskSystemEjected` 继续塞回 response 或 notice
- 不把 `event slot` 做成 session 级全局事件口
- 不给 `event slot` 做批处理和复杂 payload
- 不在本轮顺手实现系统弹出行为
- 不在本轮同时重写全部读写 slot 协议
