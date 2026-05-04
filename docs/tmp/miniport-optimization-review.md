# YumeDisk Miniport 优化后全链路审查报告

审查范围: `opus改版` commit 引入的 KMDF transport 层 miniport handle 缓存优化, 以及 SCSI miniport / RWTestApp 的完整 I/O 链路.

审查路径: App 启动 → ct 建盘 → 系统读写 → exit/rm 删盘 → 程序退出

---

## 变更摘要

`opus改版` 主要改动:

1. **KMDF transport.c**: 将每次 IOCTL 打开/关闭 miniport handle 改为缓存到 `Context->MiniportHandle`, 使用 `InterlockedCompareExchangePointer` 做 lazy init
2. **KMDF transport.c**: `ZwCreateFile` 去掉 `FILE_SYNCHRONOUS_IO_NONALERT`, 改为异步 I/O + `KEVENT` + `KeWaitForSingleObject`
3. **KMDF file.c**: 文件 cleanup 时调用 `ControlCloseMiniportHandle` 关闭缓存句柄
4. **KMDF defs.h**: `CTRL_DEVICE_CONTEXT` 新增 `volatile HANDLE MiniportHandle`

工作树未提交改动: `ZwCreateFile` access mask 加回 `SYNCHRONIZE`

---

## 问题列表

### P0-1: ZwDeviceIoControlFile Event 参数类型错误

**文件**: `windows/YumeDiskKMDF/YumeDiskKMDF/transport/transport.c:54-66`

**现状**:

```c
KEVENT event;
KeInitializeEvent(&event, NotificationEvent, FALSE);
status = ZwDeviceIoControlFile(
    Handle,
    &event,    // KEVENT* 传入了需要 HANDLE 的位置
    NULL, NULL, &ioStatus,
    IOCTL_SCSI_MINIPORT,
    ioctlBuffer, ioctlBufferSize,
    ioctlBuffer, ioctlBufferSize
);

if (status == STATUS_PENDING) {
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    status = ioStatus.Status;
}
```

**问题**: `ZwDeviceIoControlFile` 第二个参数类型是 `HANDLE` (事件句柄), 不是 `PKEVENT`. 传入栈上 `KEVENT` 指针属于类型误用.

**影响分析**:
- 同步完成的命令 (QueryInfo, CreateDisk, RemoveDisk): miniport 立即返回, `ZwDeviceIoControlFile` 返回 `STATUS_SUCCESS`, KEVENT 从未被使用 → **表面正常**
- WaitEvent 当 miniport 返回 `SRB_STATUS_PENDING`: `ZwDeviceIoControlFile` 返回 `STATUS_PENDING`, 代码进入 `KeWaitForSingleObject`. 如果 I/O Manager 不能正确识别这个 "event handle", 事件永远不会被 signal → **线程永久阻塞**
- 即使在某些 Windows 版本上碰巧能工作 (内核可能对 KernelMode 调用者做了特殊处理), 这也是未文档化行为, 不可依赖

**修复方案 A — 改用 IoBuildDeviceIoControlRequest (推荐)**:

需要获取 miniport 的 `PDEVICE_OBJECT` (而非 HANDLE), 用 `IoGetDeviceObjectPointer` 或在 `ZwCreateFile` 后通过 `ObReferenceObjectByHandle` 获取 FILE_OBJECT 再取 DeviceObject.

```c
KEVENT event;
IO_STATUS_BLOCK ioStatus;
PDEVICE_OBJECT deviceObject;  // 从缓存的 FILE_OBJECT 获取

KeInitializeEvent(&event, NotificationEvent, FALSE);

PIRP irp = IoBuildDeviceIoControlRequest(
    IOCTL_SCSI_MINIPORT,
    deviceObject,
    ioctlBuffer, ioctlBufferSize,
    ioctlBuffer, ioctlBufferSize,
    FALSE,
    &event,        // IoBuildDeviceIoControlRequest 原生接受 PKEVENT
    &ioStatus);

if (irp == NULL) {
    status = STATUS_INSUFFICIENT_RESOURCES;
} else {
    status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
}
```

需要配套修改:
- `CTRL_DEVICE_CONTEXT` 中缓存 `PFILE_OBJECT MiniportFileObject` 和 `PDEVICE_OBJECT MiniportDeviceObject`
- `ControlOpenMiniportHandle` 改为同时获取 FileObject/DeviceObject
- `ControlCloseMiniportHandle` 改为 `ObDereferenceObject(FileObject)` + 置空

**修复方案 B — 恢复同步句柄 (简单但性能退步)**:

```c
// ControlOpenMiniportHandle 中恢复:
openStatus = ZwCreateFile(
    &handle,
    GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
    &attributes, &ioStatus, NULL,
    FILE_ATTRIBUTE_NORMAL,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    FILE_OPEN,
    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,  // 恢复同步
    NULL, 0);

// ControlSendMiniportBuffer 中去掉 KEVENT:
status = ZwDeviceIoControlFile(
    Handle,
    NULL,          // 同步句柄不需要 event
    NULL, NULL, &ioStatus,
    IOCTL_SCSI_MINIPORT,
    ioctlBuffer, ioctlBufferSize,
    ioctlBuffer, ioctlBufferSize);
// 同步句柄下 ZwDeviceIoControlFile 直接阻塞到 I/O 完成, 不会返回 STATUS_PENDING
if (NT_SUCCESS(status)) {
    status = ioStatus.Status;
}
```

注意: 同步句柄 + 缓存 handle 复用 = 串行化所有 miniport 通信. 64 个 worker 并发 WaitEvent 会被串行化, 性能退步到比优化前更差. 如果选方案 B, 建议**不缓存句柄**, 每次打开新同步句柄 (即回退到优化前逻辑), 或配合连接池.

---

### P0-2: 退出路径 worker 线程卡死

**文件**: `windows/tests/RWTestApp/main.cpp:1133-1152`

**退出流程**:

```
用户输入 exit
  → SetEvent(g_StopEvent)
  → RunCommandLoop 返回
  → WaitForSingleObject(g_StopEvent) 立即返回
  → backend.stop = true
  → RemoveAllManagedDisks        // 发 RemoveAllDisks, 不带 SESSION_CLOSE_FLAG
  → CloseHandle(control.file)   // 触发 KMDF cleanup
  → worker.join()               // 等待所有 worker 退出
```

**问题**:

1. `RemoveAllManagedDisks` 发送 `YumeDiskCommandRemoveAllDisks` 不带 `SESSION_CLOSE_FLAG`, miniport 只删盘不关 session, 不会完成 waiter SRBs
2. 假设有 1 个盘, miniport queue 1 个 `DiskRemoved` 事件, 最多唤醒 1 个 worker. **其余 63 个 worker 的 WaitEvent SRB 仍然 pending**
3. `CloseHandle` 触发 `ControlEvtFileCleanup`:
   - `ControlSendSessionCleanup` (带 SESSION_CLOSE_FLAG) → miniport 完成所有 waiter → 所有 WaitEvent IOCTL_SCSI_MINIPORT IRP complete
   - 但 KMDF IOCTL 回调阻塞在 `KeWaitForSingleObject(&event, ...)`, 如果 P0-1 的 Event bug 导致 KEVENT 无法被 signal, **线程永久阻塞**
   - `worker.join()` 永远等不到

**即使 P0-1 修复后**: 每个 WaitEvent IOCTL 仍然占用一个内核线程同步阻塞. 64 worker = 64 kernel threads blocked. 系统线程资源被大量占用.

**修复方案**:

修复 P0-1 后, cleanup 路径的 session cleanup 可以正确唤醒所有阻塞的 KMDF 回调, 退出流程可以正常完成.

长期优化: 考虑让 KMDF 层对 WaitEvent 做异步分发 — 不要在 IOCTL 回调中同步阻塞, 而是 pend WDF request 并在 miniport 完成时异步完成. 这需要较大的架构改动.

---

### P1-1: Miniport 无 SRB 取消处理

**文件**: `windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.c:654-658` 以及 `adapter/adapter.c`

**现状**:

```c
// queue.c — WaitEvent 入队 waiter
waiterNode->Srb = Srb;
InsertTailList(&Extension->PendingWaiters, &waiterNode->Link);

// adapter.c — 未注册 HwStorAbortCommand
hwInitData.HwResetBus = DiskResetBus;    // 只注册了 ResetBus
// 没有 HwAbortCommand / HwCancelIo
```

**问题**: `SRB_IO_CONTROL.Timeout = YUMEDISK_MINIPORT_TIMEOUT_SEC (30s)`. StorPort 在超时后会取消 SRB. 但 miniport 的 `PendingWaiters` 和 `PendingIo` 列表中仍持有被取消 SRB 的指针. 后续事件到达尝试完成这个 SRB 时 → **use-after-free → BSOD**.

同样影响 `PendingIo` 列表中的 READ/WRITE SRBs.

**修复方案**:

方案 A: 增大超时或设为无限:
```c
srbIoControl->Timeout = 0;  // 某些 StorPort 版本 0 = 无限
```

方案 B: 注册 `HwStorAbortCommand` 回调:
```c
// adapter.c
static BOOLEAN DiskAbortCommand(
    PVOID DeviceExtension,
    PSTORAGE_REQUEST_BLOCK Srb)
{
    // 从 PendingWaiters / PendingIo 中找到并移除匹配的 SRB
    // 释放对应的 waiterNode / pendingIoNode
    return TRUE;
}

// DiskDriverEntry:
hwInitData.HwAbortCommand = DiskAbortCommand;
```

方案 C: 给每个 pending SRB 加引用计数或 generation 标记, 在完成前校验 SRB 是否仍然有效.

---

### P1-2: Session 关闭与并发 WaitEvent 的竞态 — waiter 孤儿化

**文件**: `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c:200-209` vs `control.c:138-143`

**竞态时序**:

```
Thread A (WaitEvent IOCTL):
  1. DiskClaimSessionLocked → 成功 (session 有效)
  2. 释放 ControlLock
  3. 进入 DiskHandleWaitEvent

Thread B (Session Close):
  4. DiskHandleRemoveAllDisks with SESSION_CLOSE_FLAG
  5. CurrentSessionId = 0
  6. DiskCompleteAllPending → 完成所有当前 waiter

Thread A (继续):
  7. DiskHandleWaitEvent 获取 ControlLock, 入队一个新 waiter
  → 这个 waiter 永远不会被完成 (session 已关闭, 不会有新事件)
```

**修复方案**:

在 `DiskHandleWaitEvent` 入队 waiter 前, 持锁检查 session:

```c
KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
if (Extension->CurrentSessionId == 0) {
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
    return STATUS_DEVICE_NOT_CONNECTED;
}
// ... 原有的 event/waiter 逻辑 ...
KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
```

当前代码已经在锁内做了 event 检查和 waiter 入队, 只需在入队前加 session 检查即可.

---

### P2-1: Session Close 的事件投递被 DiskFreeQueuedState 丢弃

**文件**: `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c:126-143`

**现状**:

```c
for (index...) {
    DiskQueueSyntheticEvent(..., YumeDiskEventDiskRemoved, index);  // ① 入队或直接投递
}
if (SESSION_CLOSE_FLAG) {
    DiskQueueSyntheticEvent(..., YumeDiskEventShutdown, 0);         // ② 入队或直接投递
    Extension->CurrentSessionId = 0;
    DiskFreeQueuedState(DeviceExtension);                           // ③ 释放所有未投递的事件
    DiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_NOT_CONNECTED); // ④ 完成剩余 waiter
}
```

**问题**: 如果 ① ② 的事件没有足够的 waiter 可以直接投递 (waiter 少于事件数), 它们会被放入 `PendingEvents` 队列. ③ `DiskFreeQueuedState` 会释放 `PendingEvents` 中所有事件, 包括刚刚入队的 `DiskRemoved` 和 `Shutdown`. 用户空间可能**永远收不到 Shutdown 事件**.

**影响**: RWTestApp 的 worker 依赖 `YumeDiskEventShutdown` 来设置 `context->stop = true` (main.cpp:758). 如果 shutdown 事件被丢弃, worker 不知道 session 已关闭. 不过 ④ 会用错误状态完成所有 waiter, worker 的 `SendWorkerCommand` 返回 false, worker 也会退出. 所以实际影响有限, 但逻辑不正确.

**修复方案**:

调整顺序 — 先释放无关事件, 再完成 waiter, 确保 Shutdown 事件能投递:

```c
if (SESSION_CLOSE_FLAG) {
    Extension->CurrentSessionId = 0;
    DiskFreeQueuedState(DeviceExtension);       // 先清空残留事件
    DiskQueueSyntheticEvent(..., Shutdown, 0);   // 再投递 shutdown (此时队列干净, 一定能投递给 waiter)
    DiskCompleteAllPending(DeviceExtension, ...); // 完成剩余 waiter
}
```

---

### P3-1: CDB6 READ/WRITE LBA 解码移位错误

**文件**: `windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c:280-283` 和 `324-327`

**现状**:

```c
case SCSIOP_READ6:
    startBlockIndex = (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb1) << 13) |
        (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb0) << 8) |
        Cdb->CDB6READWRITE.LogicalBlockLsb;
```

**问题**: `LogicalBlockMsb1` 是 5-bit 字段, 代表 LBA bits[20:16], 应该左移 16 位. `<< 13` 导致 LBA > 8192 时解码错误, 读写到错误的偏移.

WRITE6 (line 324) 有相同错误.

**影响**: CDB6 在现代 Windows 中极少使用 (一般用 CDB10/CDB16), 对 64MB 测试盘 (16384 sectors, 4096 sector size) 的大 LBA 访问可能出错. 实际测试中可能不会触发.

**修复**:

```c
startBlockIndex = (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb1) << 16) |
    (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb0) << 8) |
    Cdb->CDB6READWRITE.LogicalBlockLsb;
```

READ6 和 WRITE6 两处都要改.

---

### P3-2: DiskQueueSyntheticEvent 读 CurrentSessionId 无锁保护

**文件**: `windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.c:494-497`

```c
void DiskQueueSyntheticEvent(...) {
    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (extension->CurrentSessionId == 0) {  // 无锁读取 64-bit 值
        return;
    }
    // ...
}
```

**问题**: `CurrentSessionId` 是 64-bit, 在 32-bit 平台上非原子读取. 在 x86-64 上对齐的 64-bit 读是原子的, 问题不大. 但严格来说应持 ControlLock 或使用 `InterlockedCompareExchange64` 读取.

**影响**: 轻微. 最坏情况是读到半更新的值, 导致多入队一个无害事件或少入队一个事件. 在 x86-64 上不会出现.

---

## 修复优先级总览

| 优先级 | 编号 | 问题 | 影响 | 工作量 |
|--------|------|------|------|--------|
| **P0** | P0-1 | ZwDeviceIoControlFile Event 类型错误 | WaitEvent 可能永久阻塞 | 中 (方案A) / 小 (方案B) |
| **P0** | P0-2 | 退出路径 worker 卡死 | 程序无法正常退出 | 依赖 P0-1 修复 |
| **P1** | P1-1 | Miniport 无 SRB 取消处理 | SRB 超时后 use-after-free → BSOD | 中 |
| **P1** | P1-2 | Session 关闭竞态 waiter 孤儿化 | WaitEvent SRB 泄漏 | 小 |
| **P2** | P2-1 | Session Close 事件被丢弃 | Shutdown 事件丢失 (有兜底) | 小 |
| **P3** | P3-1 | CDB6 LBA 移位错误 | 大 LBA 读写位置错误 | 小 |
| **P3** | P3-2 | SessionId 无锁读取 | x86-64 上无影响 | 小 |

---

## 建议修复顺序

1. **先修 P0-1** — 这是根因, 决定后续方案走向
   - 如果选方案 A (IoBuildDeviceIoControlRequest): 需要重构 transport 层, 缓存 FileObject/DeviceObject
   - 如果选方案 B (恢复同步句柄): 简单但需评估是否放弃 handle 缓存优化
2. **修 P1-2** — 在 `DiskHandleWaitEvent` 入队前加 session 检查, 一行代码
3. **修 P2-1** — 调整 `DiskHandleRemoveAllDisks` 中 DiskFreeQueuedState 和 DiskQueueSyntheticEvent 的顺序
4. **修 P1-1** — 根据方案选型, 增大超时或实现 HwStorAbortCommand
5. **修 P3-1** — CDB6 移位 `<< 13` → `<< 16`
