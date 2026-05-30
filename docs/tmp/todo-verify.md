# Unit Attention 28/00 验证补齐方案评估（DbgPrint 版）

日期：`2026-05-30`

## 1. 目标

当前缺口不是 `data_changed` 主链没通，而是：

- 还缺一次稳定、可复现、可归档的 `28/00` 证据。

当前已经确认的事实：

- `target 3` 真实写入 committed 后，`rust-cli` 会对 sibling `target 4` 下发 `notify_managed_disk_data_changed(...)`
- `target 4` 下一次 `TEST UNIT READY` 会返回一次 `CHECK CONDITION`
- 第二次 `TEST UNIT READY` 恢复 `GOOD`
- `target 4` 后续读能看到新数据

所以现在要补的，不是功能实现，而是“这一次 `CHECK CONDITION` 确实对应 `UNIT ATTENTION / 28 00`”的 runtime 证据。

## 2. 先回答结论

结论分两层：

1. 如果目标只是补齐“`SCSI` 层确实吐出过一次 `28/00`”的证据：
   - 只给必要路径加 `DbgPrint`，可以完成。
2. 如果目标是补齐“完整 end-to-end runtime 验收”：
   - 不能只看 `DbgPrint`
   - 仍然需要现有用户态测试步骤去建盘、触发真实写入、再主动发一次探测命令

更准确地说：

- `DbgPrint` 可以补齐“缺失的 `28/00` 明文证据”
- 但 `DbgPrint` 本身不会替代现有 `rust-cli + 真实写入 + scsi_ua_probe` 这条运行时触发链
- 不需要为此新增应用层功能代码，但需要继续跑应用层/用户态测试流程

## 3. 为什么单靠当前用户态还差一点

现在已有的用户态工具已经能看到：

- `TUR: GOOD -> CHECK CONDITION -> GOOD`

但还没稳定拿到：

- `REQUEST SENSE: 06 / 28 / 00`

当前限制不在驱动链本身，而在 `\\.\PhysicalDriveN` 这条 Windows class path 上：

- `TEST UNIT READY` 可以把“有一次异常状态”暴露出来
- 但显式 `REQUEST SENSE` 可能被 disk class path 拒绝、吞掉或改写

因此，仅靠继续硬啃用户态 `REQUEST SENSE`，不能保证每次都拿到 `28/00` 明文。

## 4. 为什么 DbgPrint 可以补这块

`28/00` 真正被构造出来的地方已经在 `YumeDiskSCSI` 内部，而且路径很集中：

- [`windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c`](/C:/Users/0/Desktop/yumedisk/windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c)
  - `DiskHandleNotifyDataChanged(...)`
- [`windows/YumeDiskSCSI/YumeDiskSCSI/core/protocol.c`](/C:/Users/0/Desktop/yumedisk/windows/YumeDiskSCSI/YumeDiskSCSI/core/protocol.c)
  - `DiskTryMarkPendingDataChangedUa(...)`
  - `DiskTryConsumePendingDataChangedUa(...)`
  - `DiskNotifyTargetMediaStatus(...)`
- [`windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c`](/C:/Users/0/Desktop/yumedisk/windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c)
  - `DiskFillUnitAttentionSenseDataChanged(...)`
  - `DiskTryReturnPendingDataChangedUa(...)`
  - `DiskHandleRequestSense(...)`

也就是说：

- 内核里已经知道这次返回的是 `sense key=06, asc=28, ascq=00`
- 只是当前这份 sense 没有稳定原样穿回到 `PhysicalDriveN` 的用户态 `REQUEST SENSE`

因此，只要在“真正构造或返回 `28/00`”的那几个分支上打精确 `DbgPrint`，就能把这份证据抬到观测面。

## 5. 最小 DbgPrint 打点建议

这里的原则是：

- 只打稀有分支
- 不打热路径
- 不打每次读写 dispatch
- 每个关键事实只打一条

### 5.1 必要打点一：NotifyDataChanged 首次把 target 标成 pending

位置：

- `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c`
- `DiskHandleNotifyDataChanged(...)`

建议只在 `DiskTryMarkPendingDataChangedUa(disk)` 返回 `TRUE` 时打印一次：

- `target_id`
- `generation`
- `source=notify_data_changed`
- `pending=true`

目的：

- 证明 sibling notify 已经到达 `SCSI`
- 证明这次确实把 target 从“无 pending”推进到了“有 pending”

### 5.2 必要打点二：第一次前置 UA 被非 REQUEST SENSE 命令消费

位置：

- `windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c`
- `DiskTryReturnPendingDataChangedUa(...)`

这个点最关键。

建议只在 `DiskTryConsumePendingDataChangedUa(Disk)` 成功时打印一次：

- `target_id`
- `opcode`
- `path=pre-check`
- `sense_key=0x06`
- `asc=0x28`
- `ascq=0x00`
- `scsi_status=CHECK_CONDITION`

目的：

- 直接证明“有某个系统探测命令第一次消费了 pending UA”
- 直接证明这一跳返回的就是 `28/00`

这条日志一旦出现，`28/00` 证据就已经成立。

### 5.3 必要打点三：REQUEST SENSE 路径返回 28/00

位置：

- `windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c`
- `DiskHandleRequestSense(...)`

建议只在 `DiskTryConsumePendingDataChangedUa(Disk)` 成功时打印一次：

- `target_id`
- `opcode=REQUEST_SENSE`
- `sense_key=0x06`
- `asc=0x28`
- `ascq=0x00`

目的：

- 如果这次 UA 不是被 `TUR/READ CAPACITY/...` 消费，而是被显式 `REQUEST SENSE` 消费，这条日志能覆盖到

### 5.4 可选打点：异步通知失败

位置：

- `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c`
- `DiskHandleNotifyDataChanged(...)`
或
- `windows/YumeDiskSCSI/YumeDiskSCSI/core/protocol.c`
- `DiskNotifyTargetMediaStatus(...)`

建议只在 `StorPortAsyncNotificationDetected(...MEDIA_STATUS)` 返回非成功状态时打印：

- `target_id`
- `storport_status`

目的：

- 只用于排查“为什么系统没及时来探测”
- 不属于 `28/00` 证据主路径

## 6. 明确不该打的路径

不应该在这些路径上加 print：

- 每次 `READ/WRITE` dispatch
- 每次 slot submit/complete
- 每次 ACK
- 每次 `DiskTryConsumePendingDataChangedUa(...)` 调用都打
- 每次 `REQUEST SENSE` 都打空路径日志

原因已经在 [Windows驱动问题排查笔记.md](/C:/Users/0/Desktop/yumedisk/docs/troubleshooting/Windows驱动问题排查笔记.md) 里验证过：

- 热路径 `DbgPrint` 会明显拖垮虚拟机吞吐
- 性能噪声会干扰本轮验证

所以当前验证方案必须坚持：

- 只在 `pending` 首次置位时打印
- 只在 `28/00` 真正被返回时打印
- 只在异步通知失败时打印

## 7. 仅靠 DbgPrint 能完成到什么程度

### 7.1 能完成的部分

如果 acceptance 是：

- “runtime 中确实发生过一次 `UNIT ATTENTION / 28 00`”

那么只要 `DbgPrint` 打在第 `5.2` / `5.3` 的分支上，就已经足够。

因为那条日志来自真正的 `SCSI` 返回分支，而不是宿主猜测。

### 7.2 不能替代的部分

仅靠 `DbgPrint` 不能替代下面这些 runtime 事实：

- 是谁触发了这次写入
- sibling 是否继续存活，没有被 remove / invalid
- sibling 后续是否真的能读到新内容
- 当前 `rust-cli` 的 sibling fanout、target 解绑、`smid` 删除拒绝等用户态口径是否仍成立

这些都不是 `DbgPrint` 自己能证明的，仍然需要用户态测试链来触发和观察。

## 8. 所以应用层需不需要同步做测试

需要。

但这里要区分“两种需要”：

### 8.1 不需要的事

不需要为了这次验证再新增应用层功能代码，例如：

- 不需要再给 `rust-cli` 补新的 debug 命令
- 不需要改 `BackendRust`
- 不需要改 `AppKernel`
- 不需要改 `tauri-client`

### 8.2 仍然需要的事

仍然需要继续跑现有用户态测试流程，用来制造场景并收集旁证：

1. `rust-cli shell`
2. `sm 64`
3. `ct smid=1 target=3 ro=false`
4. `ct smid=1 target=4 ro=false`
5. 对 `PhysicalDrive1` 发真实写入
6. 对 `PhysicalDrive2` 发一次主动探测
   - 当前优先用 `windows/tests/scsi_ua_probe`
   - 至少要看到第一次 `TUR = CHECK CONDITION`
7. 同时抓 kernel log
   - WinDbg 或 DbgView 类工具均可

所以更准确的表述是：

- 不需要新增应用层代码
- 但需要同步跑应用层/用户态测试步骤

## 9. 推荐验收口径

如果本轮想用最小代价把 `28/00` 证据补齐，推荐把验收口径收成：

### 9.1 主证据

- `scsi_ua_probe`：
  - baseline `TUR = GOOD`
  - 写入后第一次 `TUR = CHECK CONDITION`
  - 第二次 `TUR = GOOD`

### 9.2 补强证据

- `DbgPrint`：
  - `NotifyDataChanged` 首次把 `target 4` 标成 pending
  - 某次 `opcode` 消费 pending 时返回 `sense_key=06 asc=28 ascq=00`

### 9.3 旁证

- `PhysicalDrive2` 后续能读到新字节
- `target 4` 保持在线
- `events_queued` 在首次写入后增加

这样三层证据拼起来，已经足够说明：

- 上层真实写入发生了
- sibling notify 到了 `SCSI`
- `SCSI` 确实返回过一次 `28/00`
- 这次 UA 只返回一次
- 后续盘继续可用且可见新内容

## 10. 最终建议

最终建议如下：

1. 仅为验证补齐，在 `YumeDiskSCSI` 必要分支上加极少量 `DbgPrint`
2. 不新增应用层功能代码
3. 继续使用现有 runtime harness：
   - `rust-cli`
   - 真实 raw write
   - `scsi_ua_probe`
4. 把“`DbgPrint` 日志里出现 `06/28/00`”作为本轮缺失证据的补齐标准

一句话总结：

- `DbgPrint` 能补齐缺的 `28/00` 证据
- 但不能单独替代用户态测试链
- 最合理的做法是“驱动侧最小日志 + 现有用户态触发与探测”
