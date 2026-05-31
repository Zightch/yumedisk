# Windows 单盘系统弹出重建执行清单

## 0. 当前范围

本清单只处理 Windows 本地虚拟 SCSI 盘“像 U 盘一样由系统发起弹出”的能力，不覆盖网络协议、`gateway/storer`、共享盘 `rw/ro` 语义等其他主线。

本轮只处理一条新增主线：

- 系统对某一个 `PhysicalDrive` 发起设备弹出
- `YumeDiskSCSI` 只清理对应单盘
- 事件沿 `SCSI -> KMDF -> AppKernel -> BackendRust -> app` 上报
- app 对对应盘做自动清理，把“已挂载”收成“未挂载”

本轮明确保持不变：

- 主动删盘仍走现有 `AkRemoveDisk -> RemoveDisk` 主线
- 主动删盘现有行为、现有清理、现有 UI/CLI 入口不改

`tauri-client` 属于最后一阶段。前面各阶段一律先用 `rust-cli` 做验证。

## 1. 当前总目标

按最小核心闭环完成“系统单盘弹出”正式收口：

1. `YumeDiskSCSI` 对系统注册“单盘可弹出”能力，而不是把整个 SCSI 控制器做成可弹出。
2. 系统弹出只作用于一个 `target/lun` 对应的单盘，不影响同一控制器下的其他盘。
3. 被弹出的单盘由 `SCSI` 先同步清理 pending I/O、队列和可见性，再向上发事件。
4. 新增一条专用 `event slot` 上行链路，用于驱动把单盘弹出事件送到 `AppKernel`。
5. `BackendRust` 接到该事件后，做“被动拔出”清理，不再调用主动 `AkRemoveDisk`。
6. app 收到后只把对应盘从“已挂载”收成“未挂载”，不做“删除盘”，不做“置无效”。
7. 前期全部黑盒验证以 `rust-cli` 为准；`tauri-client` 在最后再接 UI/runtime 状态。

## 2. 固定边界

### 2.1 本轮只做的事

- `YumeDiskSCSI` 单盘系统弹出能力
- 单盘系统弹出后的 `SCSI` 同步清理
- `KMDF` 专用 `event slot` 上行链
- `AppKernel` 专用 `event slot` worker 与 host event 收束
- `BackendRust` 被动拔出清理
- `rust-cli` 事件接入与黑盒验证
- `tauri-client` 最后接状态同步与 UI 刷新

### 2.2 本轮明确不做

- 修改现有主动 `RemoveDisk` 主线
- 把系统弹出做成“整个 SCSI 控制器移除”
- 因单盘系统弹出而关闭整个 `AppKernel session`
- 因单盘系统弹出而影响其他已挂载盘
- 因单盘系统弹出而自动删除 runtime
- 因单盘系统弹出而把盘置为 `invalid`
- 在前期为了验证先改 `tauri-client`
- 把该事件并入网络 `SessionCloseNotice` / `ConnectionLost`

### 2.3 当前正式口径

- 新增语义名固定为“系统单盘弹出”，与主动 `RemoveDisk` 明确分开。
- “系统单盘弹出”是 target 级语义，不是 adapter 级语义。
- 该语义的最终 app 结果固定为：对应盘从 `mounted` 变为 `unmounted`。
- `event slot` 是一条独立上行链，不与读写 slot、ACK、心跳混用。
- `event slot` 在 session 内始终只保持 `1` 个在途请求。
- `event slot` 只是传输窗口，不是真实状态源；真实状态源固定在 `KMDF` session 侧的 pending event FIFO。
- `tauri-client` 不是前置依赖；前期一律由 `rust-cli` 做行为验证。

## 3. 适用原则

本清单严格按 [开发原则](../development/development-principles.md) 执行，重点如下：

- 极简核心原则
  - 先只做“单盘系统弹出 -> 盘级清理 -> 事件上报 -> app 转未挂载”。
  - 不提前做批量事件、复杂去重、交互式确认。
- 激进更新原则
  - 不在旧主动删盘语义上叠补丁，直接新增独立“系统单盘弹出”主线。
- 单一真实来源原则
  - 单盘是否已被系统弹出，以 `SCSI` target 生命周期为真。
  - 待上报事件以 `KMDF` session pending event FIFO 为真。
  - app 最终盘状态以自身 runtime 为真。
- 边界闸口原则
  - 系统弹出事件只允许从 `SCSI` 进入，不能由 app 反向伪造。
  - app 不参与系统是否允许弹出的同步决策，只消费结果事件。
- 结构重构与层次依赖原则
  - 事件链必须清晰分层：`SCSI -> KMDF -> AppKernel -> BackendRust -> app`。
  - 不把系统弹出逻辑塞进现有网络事件面，也不把 `event slot` 塞进读写 slot。
- 删除优先原则
  - 不新增与主动删盘并行的兼容桥接层。
  - 能复用现有 host event queue 的地方，直接复用，不再造第二套 host 事件系统。

## 4. 当前现状与缺口

### 4.1 `YumeDiskSCSI` 当前还是固定盘口径

当前已有：

- `INQUIRY` 返回固定盘能力
- 主动 `CreateDisk / RemoveDisk / RemoveAllDisks`
- 盘级 `BusChangeDetected`

当前缺失：

- 单盘可弹出设备能力
- target 级系统弹出/移除处理
- 系统弹出后的盘级同步清理与上报

### 4.2 当前没有 `driver -> app` 的正式上行事件链

当前已有：

- `AppKernel` 自己的 host event queue
- `AkWaitEvent / AkPollEvent`
- `DiskOnline / DiskRemoved / WriteFinal* / SessionBroken`

当前缺失：

- 驱动主动把“系统单盘弹出”送给 `AppKernel` 的正式通道
- `KMDF` session 侧 pending disk-event FIFO
- 常驻 `1` 个在途的 `event slot` 机制

### 4.3 当前 `BackendRust` 只有主动移除语义

当前已有：

- `remove_managed_disk_with_media(...)`
- app 主动拔出时把 media 返还给上层

当前缺失：

- 内核已经先弹出后，`BackendRust` 的“被动拔出”路径
- 不调用 `AkRemoveDisk` 的 media handoff / runtime handoff

### 4.4 `rust-cli` 现在只有“mounted network disks”模型

当前已有：

- `rust-cli` 已能消费 `ManagedDiskEvent`
- `rust-cli` 已有网络挂载注册表和 `data changed` 处理

当前缺失：

- “系统单盘弹出”专用 host event 处理
- 对本地盘、网络盘的被动拔出清理
- 前期验证所需的状态打印与黑盒验证命令

### 4.5 `tauri-client` 现在只同步网络事件

当前已有：

- `network_runtime::sync_runtime_state(...)`
- 网络盘 invalidation / session close / connection lost 收束

当前缺失：

- 本地 `BackendRust` 盘事件 watcher
- “系统单盘弹出”后从 `mounted` 改为 `unmounted` 的 UI/runtime 收束

## 5. 目标链路

正式链路固定为：

```text
Windows 弹出单个 PhysicalDrive
  -> YumeDiskSCSI 识别对应 target/lun
  -> 只清理该 target 的 pending I/O / 队列 / 可见性
  -> KMDF session pending event FIFO 入队一条 DiskSystemEjected
  -> AppKernel 常驻 event slot 收到并翻译成 AkEventDiskSystemEjected
  -> BackendRust 收到后执行被动拔出清理
  -> rust-cli / tauri-client 把对应盘从 mounted 收成 unmounted
```

固定要求：

- 该链路只对一个 target 生效。
- 该链路不影响 session 中的其他盘。
- 该链路不要求 app 先确认，驱动清理必须先发生。
- app 只承接结果，不参与内核同步决策。

## 6. `event slot` 链路定义

### 6.1 设计目标

`event slot` 的职责只有一个：把驱动侧盘级系统事件送上来。

它不是：

- 读写 slot
- ACK
- 心跳
- 网络事件

### 6.2 真实状态源

真实状态源固定为：

- `KMDF` session 侧 pending disk-event FIFO

固定要求：

- `event slot` 只是消费这个 FIFO 的一个在途窗口
- 不能把“slot 是否在飞”当成事件真状态
- 不能让 `AppKernel` 自己再维护第二份驱动事件镜像

### 6.3 在途数量

当前最小闭环固定为：

- session 内始终只保留 `1` 个在途 `event slot`
- 消费完一条后立即补投下一条

当前明确不做：

- 多个并行 `event slot`
- 事件批处理
- 事件合包

### 6.4 事件负载

当前最小闭环建议只带固定最小字段：

- `event_kind`
- `target_id`
- `disk_runtime_id`
- `status`
- `flags`

当前只定义一个盘级事件：

- `DiskSystemEjected`

当前明确不做：

- 容量变化
- 介质变化
- 复杂调试文本
- 批量 target 列表

### 6.5 协议落点

本轮建议新增 dedicated 上行命令，而不是把它并进现有读写 slot 类型：

- `YumeDiskCommandPostEventSlot`
- `YumeDiskCommandCompleteEventSlot`
- `YUMEDISK_DISK_EVENT`

当前不建议：

- 把 `event slot` 做成 `YumeDiskSlotTypeRead/Write` 的第三种变体

原因：

- 读写 slot 是数据面
- 单盘系统弹出是生命周期事件
- 两者混在同一 slot 类型集合里会拉糊边界

## 7. SCSI 单盘弹出语义

### 7.1 作用范围

系统弹出语义固定为：

- Windows 暴露出来的单个 `PhysicalDrive`
- 对应一个 `target/lun`

它不等于：

- 整个 Storport adapter
- 整个 `YumeDiskSCSI` 控制器
- 整个 `AppKernel session`

### 7.2 target 级清理要求

当某个 target 被系统弹出时，`SCSI` 必须同步完成：

- 将该 target 标记为 removing / not present
- 停止继续接受该 target 的新 I/O
- 失败完成该 target 现有 pending I/O
- 清理该 target 队列、pending UA、关联运行态
- 让该 target 不再被继续枚举
- 触发该 target 从系统视图中消失

固定要求：

- 只清理该 target
- 不碰其他 target
- 不发全局 `RemoveAll`
- 不结束整个进程/控制器

### 7.3 事件发出时机

盘级系统弹出事件固定为：

- 先完成 `SCSI` 盘级同步清理
- 再上报 `DiskSystemEjected`

不能反过来：

- 不能先通知 app 再等 app 指挥 `SCSI` 清理

## 8. KMDF / AppKernel / BackendRust 工作拆分

### 8.1 `YumeDiskSCSI`

目标：

- 注册单盘可弹出能力
- 处理 target 级系统弹出
- 清理后发出盘级系统事件

建议关注文件：

- `windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c`
- `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c`
- `windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.c`
- `windows/YumeDiskSCSI/YumeDiskSCSI/adapter/adapter.c`

### 8.2 `YumeDiskKMDF`

目标：

- 增加 session pending disk-event FIFO
- 增加 `event slot` 请求承接与完成
- 维持 session 内固定 `1` 个在途 `event slot`

建议关注文件：

- `windows/YumeDiskKMDF/YumeDiskKMDF/control/ioctl.c`
- `windows/YumeDiskKMDF/YumeDiskKMDF/transport/transport.c`
- `windows/YumeDiskKMDF/YumeDiskKMDF/transport/runtime.c`
- `windows/YumeDiskKMDF/YumeDiskKMDF/session/session.c`

### 8.3 `AppKernel`

目标：

- 新增 `event slot` worker
- 把驱动事件翻译进现有 host event queue
- 对外新增 `AkEventDiskSystemEjected`

建议关注文件：

- `windows/AppKernel/include/appkernel.h`
- `windows/AppKernel/src/protocol/ak_protocol.c`
- `windows/AppKernel/src/protocol/ak_protocol.h`
- `windows/AppKernel/src/session/ak_session.c`
- `windows/AppKernel/src/event/ak_event.c`
- `windows/AppKernel/src/disk/ak_disk.c`

### 8.4 `BackendRust`

目标：

- 新增 `ManagedDiskEventType::DiskSystemEjected`
- 新增被动拔出路径
- 不走主动 `AkRemoveDisk`

建议关注文件：

- `windows/BackendRust/src/appkernel.rs`
- `windows/BackendRust/src/types.rs`
- `windows/BackendRust/src/runtime.rs`

## 9. app 清理语义

### 9.1 统一口径

收到 `DiskSystemEjected` 后，app 固定动作是：

- 清理对应盘当前 mounted 运行态
- 收成 `unmounted`

明确不做：

- 自动删除盘
- 自动置 `invalid`
- 自动关闭整个 app session

### 9.2 `rust-cli`

`rust-cli` 是前期唯一验证入口。

当前最小闭环要求：

- 能收到并打印 `DiskSystemEjected`
- 能对对应 target 做被动拔出清理
- 后续能继续观察其他 target 正常工作

对不同盘型的收口：

- 本地内存盘
  - 解除 target 绑定
  - 保留内存介质本体
  - 后续允许重新挂载
- 本地文件盘
  - 解除 target 绑定
  - 文件仍留在原路径
  - 后续允许重新挂载
- 网络盘
  - 解除当前 target 绑定
  - 当前阶段只要求不再把它视作 mounted target
  - 前期不要求在 `rust-cli` 内补完整 UI 式未挂载列表
  - 如需保留 live session 以供重新挂载，可在该阶段一并整理 mounted/opened 状态拆分

建议关注文件：

- `windows/rust-cli/src/cli/host.rs`
- `windows/rust-cli/src/cli/shell.rs`

### 9.3 `tauri-client`

`tauri-client` 属于最后一阶段，不作为前期依赖。

最终要求：

- 补本地 `BackendRust` 事件 watcher
- 收到 `DiskSystemEjected` 后，把对应 runtime 从 `mounted` 改为 `unmounted`
- 内存盘恢复 media
- 文件盘释放 mounted media 并恢复本地未挂载状态
- 网络盘释放 mounted `NetworkMedia` 并恢复网络未挂载状态
- 持久化配置并刷新主页

建议关注文件：

- `windows/tauri-client/src-tauri/src/lib.rs`
- `windows/tauri-client/src-tauri/src/workflow/runtime_disk.rs`
- `windows/tauri-client/src-tauri/src/backend/disk_service.rs`
- `windows/tauri-client/src-tauri/src/network/runtime_flow/mod.rs`
- `windows/tauri-client/src-tauri/src/state/disk_runtime.rs`

## 10. 分阶段执行方案

### 第一阶段：收 `event slot` 协议与正式口径

目标：

- 正式确定 `event slot` 是 dedicated 上行命令
- 正式确定 `DiskSystemEjected` 事件名和盘级语义

产出：

- `docs/tmp/todo-eject.md`
- 后续正式文档待实现落地后再同步

### 第二阶段：补 `SCSI` 单盘系统弹出与盘级清理

目标：

- 让某个 target 对应的 `PhysicalDrive` 能被系统弹出
- 弹出后只清理该 target

验收：

- 两个本地盘同时存在时，弹出 A 不影响 B
- A 对应 `PhysicalDrive` 从系统视图消失

### 第三阶段：补 `KMDF` pending event FIFO 与 `event slot`

目标：

- 驱动能把 `DiskSystemEjected` 送到用户态链路
- session 内始终只维持 `1` 个在途 `event slot`

验收：

- 单盘系统弹出后，`event slot` 能收到一条盘级事件
- 非弹出盘不收到误事件

### 第四阶段：补 `AppKernel` / `BackendRust` 事件收束

目标：

- 新增 `AkEventDiskSystemEjected`
- 新增 `ManagedDiskEventType::DiskSystemEjected`
- `BackendRust` 能执行被动拔出

验收：

- `BackendRust` 收到事件后不调用 `AkRemoveDisk`
- 被弹出的盘完成被动拔出清理

### 第五阶段：接 `rust-cli` 并做黑盒验证

目标：

- `rust-cli` 能观察到该事件
- `rust-cli` 能完成最小 app 清理

验收：

- 单本地内存盘：系统弹出后自动解除绑定，后续可重挂
- 单本地文件盘：系统弹出后自动解除绑定，后续可重挂
- 双盘并存：弹出 A，B 继续正常
- 如网络盘也纳入本阶段，则验证其 mounted target 被动收口正确

### 第六阶段：最后接 `tauri-client`

目标：

- 正式桌面客户端完成 runtime/UI 收束

验收：

- 卡片从“已挂载”自动转为“未挂载”
- 不误删盘
- 不误置 `invalid`

## 11. 测试矩阵

### 11.1 必测黑盒

- 单盘系统弹出
- 双盘同时存在，只弹出其中一个
- 弹出后其他盘继续读写
- 弹出后对应 target 不再可见
- 弹出事件只上报一次

### 11.2 `rust-cli` 阶段必测

- 本地内存盘
- 本地文件盘
- 同控制器多盘互不影响

### 11.3 `tauri-client` 阶段必测

- 卡片状态自动切换
- 配置持久化后不分叉
- 重扫后仍保持正确状态

## 12. 当前阶段结论

当前最合理的推进顺序是：

1. 先补 `event slot` 正式口径和 `SCSI` 单盘弹出语义
2. 再补 `KMDF -> AppKernel -> BackendRust`
3. 先用 `rust-cli` 做全链路黑盒验证
4. 最后才接 `tauri-client`

这样可以把“驱动链是否真正支持系统单盘弹出”先独立验明，避免前期把工作量堆进 UI/runtime，而底层语义还没站稳。
