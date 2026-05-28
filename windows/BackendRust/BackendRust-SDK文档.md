# BackendRust SDK 文档

本文档面向 `BackendRust` 的宿主接入方，描述当前稳定公开面、参数口径、调用流程和接入注意事项。

## 1. 定位

`BackendRust` 是 `windows/BackendCore/` 的 Rust 版重建实现。

当前边界：

- 负责：
  - `AppKernel` session 打开、关闭、状态查询
  - N 盘运行时持有
  - staged write / commit / reject
  - 建盘、删盘、全删盘
  - 日志、统计、调试快照
- 不负责：
  - UI
  - 表单解析
  - 路径对话框
  - 具体介质类型选择
  - 介质内部的缓存、压缩、预读、确认链
  - 系统可见盘路径枚举
  - 系统盘路径发现

核心原则：

- `BackendRust` 只把 `Media` 看作“随机读写逻辑块设备”。
- 具体 `Media` 由宿主创建，再把所有权移交给 `BackendRust`。
- 建盘成功的 runtime 真状态由 `target_id / lifecycle_text / online` 表达，不依赖系统设备路径枚举。

## 2. 稳定公开面

当前 crate 根导出仅保留以下稳定接口：

### 2.1 类型

- `BackendContext`
- `BackendError`
- `Media`
- `SessionConfig`
- `DiskConfig`
- `ManagedDiskSnapshot`
- `BackendStatsSnapshot`
- `DebugSnapshot`
- `ComponentVersionSnapshot`

### 2.2 常量

- `SECTOR_ALIGNMENT_BYTES`
- `DEFAULT_SECTOR_SIZE`
- `DEFAULT_QUEUE_DEPTH`
- `DEFAULT_WRITE_SLOT_BYTES`
- `DEFAULT_READ_WORKER_COUNT`
- `DEFAULT_WRITE_WORKER_COUNT`
- `DEFAULT_ACK_BATCH_MAX_RANGES`
- `DEFAULT_HEARTBEAT_INTERVAL_MS`
- `DEFAULT_INITIAL_EVENT_QUEUE_CAPACITY`
- `YUMEDISK_MIN_TARGET_ID`
- `YUMEDISK_MAX_USABLE_TARGET_ID`
- `YUMEDISK_MAX_TARGETS`

### 2.3 函数

- `validate_session_config()`
- `validate_disk_config()`
- `validate_create_disk_inputs()`

以下内容不视为宿主稳定 API：

- `appkernel::*`
- `config::build_ak_*`
- `runtime` 内部类型
- `types::DiskMetadata`
- `types::DiskQueueConfig`
- `staging::*`
- `win32::*`

## 3. `Media` trait

```rust
pub trait Media: Send + Sync + 'static {
    fn size_bytes(&self) -> u64;
    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError>;
    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError>;
}
```

- `BackendRust` 不会替宿主做介质范围扩容。
- `read_locked` / `write_locked` 必须自行保证范围合法。
- `Media` 必须线程安全，因为 `AppKernel` 会并发触发读写。
- `write_locked()` 返回 `Ok(())` 表示介质实例已接受本次写入，并承担后续一致性责任。

## 4. `SessionConfig`

```rust
pub struct SessionConfig {
    pub heartbeat_interval_ms: u32,
    pub initial_event_queue_capacity: u32,
}
```

| 字段 | 默认值 | 允许值 | 说明 |
| --- | --- | --- | --- |
| `heartbeat_interval_ms` | `1000` | `> 0` | session 心跳周期 |
| `initial_event_queue_capacity` | `1024` | `> 0` | 初始事件队列容量 |

`BackendContext::open()` 前可以设置；`open()` 后调用 `set_session_config()` 会被忽略并保留旧值。

## 5. `DiskConfig`

```rust
pub struct DiskConfig {
    pub target_id: u32,
    pub sector_size: u32,
    pub disk_size_bytes: u64,
    pub queue_depth: u32,
    pub write_slot_bytes: u32,
    pub read_worker_count: u16,
    pub write_worker_count: u16,
    pub ack_batch_max_ranges: u32,
    pub read_only: bool,
}
```

| 字段 | 默认值 | 允许值 | 说明 |
| --- | --- | --- | --- |
| `target_id` | `255` | `0..=254` 或 `255` | `255` 表示自动分配首个空闲 target |
| `sector_size` | `512` | `> 0` 且按 `512` 对齐 | 逻辑扇区大小 |
| `disk_size_bytes` | `0` | `> 0` 且能整除 `sector_size` | 盘总大小 |
| `queue_depth` | `96` | `> 0` | 队列深度 |
| `write_slot_bytes` | `1048576` | `> 0` | 单写槽字节数 |
| `read_worker_count` | `12` | `> 0` | 读 worker 数 |
| `write_worker_count` | `12` | `> 0` | 写 worker 数 |
| `ack_batch_max_ranges` | `96` | `> 0` | 单次 ack 最多 range 数 |
| `read_only` | `false` | `true/false` | 是否系统级只读 |

`disk_size_bytes` 必须和 `Media::size_bytes()` 完全相等。

## 6. `ManagedDiskSnapshot`

```rust
pub struct ManagedDiskSnapshot {
    pub target_id: u32,
    pub disk_size_bytes: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub lifecycle_text: String,
    pub online: bool,
}
```

| 字段 | 说明 |
| --- | --- |
| `target_id` | target 编号 |
| `disk_size_bytes` | 盘总字节数 |
| `sector_size` | 逻辑扇区大小 |
| `read_only` | 是否只读 |
| `lifecycle_text` | `init/starting/running/removing/closing/closed/broken/unknown` |
| `online` | 当前是否处于 `running` |

快照不包含系统设备路径。宿主不应把系统盘可见性当成建盘成功判据。

## 7. `BackendContext`

`BackendContext` 是宿主的主入口对象。

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `new()` | `BackendContext` | 创建上下文 |
| `set_session_config()` | `Result<(), BackendError>` | 仅在 `open()` 前有效 |
| `session_config()` | `SessionConfig` | 取当前配置副本 |
| `open()` | `bool` | 打开 session 并启动事件线程 |
| `close()` | `()` | 关闭 session、事件线程和所有盘 |
| `query_session_state_text()` | `String` | 文本化 session 状态 |
| `query_component_version_snapshot()` | `ComponentVersionSnapshot` | 查询 `AppKernel/KMDF/SCSI` 版本快照 |
| `snapshot_log_lines()` | `Vec<String>` | 当前缓冲日志 |
| `snapshot_managed_disks()` | `Vec<ManagedDiskSnapshot>` | 当前所有管理盘 |
| `query_backend_stats()` | `bool` | 查询统计，失败时写 `out_error_text` |
| `query_debug_snapshot()` | `bool` | 查询完整 debug 快照 |
| `find_first_free_target()` | `u32` | 找首个空闲 target |
| `create_managed_disk()` | `bool` | 建盘 |
| `try_create_managed_disk()` | `Result<u32, Box<dyn Media>>` | 建盘，失败时返还 media |
| `notify_managed_disk_data_changed()` | `bool` | 对单盘同步下发 `data_changed` 通知 |
| `remove_managed_disk()` | `bool` | 删单盘 |
| `remove_managed_disk_with_media()` | `Option<Box<dyn Media>>` | 删单盘并返还 media |
| `remove_all_managed_disks()` | `bool` | 删全部宿主持有盘 |

### 7.1 `notify_managed_disk_data_changed()`

```rust
pub fn notify_managed_disk_data_changed(
    &self,
    target_id: u32,
    out_error_text: Option<&mut String>,
) -> bool
```

固定语义：

- 这是一条宿主显式发起的 per-disk 下行通知，只表达“该盘底层内容已被别处改动”。
- `BackendRust` 只负责把目标盘解析到现有 runtime / `AK_DISK*`，再同步下发给 `AppKernel`。
- 当前版本只支持单盘 `data_changed`，不承载 `smid`、共享组、sibling 集合或批量 fanout。
- `WriteFinalCommitted` 事件线程仍只处理当前盘 staged write commit，不会隐式调用这条 API。

入口约束：

- `BackendContext` session 必须已打开。
- `target_id` 必须仍存在于当前 `BackendRust` 管理盘集合内。
- 目标盘必须已有有效 `AK_DISK*` 句柄。
- 目标盘当前生命周期必须是 `running`。

失败返回：

- 返回 `false`，并可通过 `out_error_text` 取得当前错误文本。
- 当前实现已使用的错误文本包括：
  - `session-not-open`
  - `target-not-found`
  - `disk-handle-not-ready`
  - `disk-not-running(<lifecycle>)`
  - `query-disk-state-failed(0xXXXXXXXX)`
  - `notify-data-changed-failed(0xXXXXXXXX)`

边界说明：

- 这条调用成功或失败，都不会回滚当前盘已经 committed 的写。
- 这条调用也不会修改 `BackendRust` 内部 staged write、共享组关系或额外事件队列。
- 是否需要把一次写入 fanout 到其他盘，由更上层宿主自己决定。

## 8. 推荐调用流程

1. 创建 `BackendContext`
2. 可选：`set_session_config()`
3. `open()`
4. 创建具体 `Media`
5. 组装 `DiskConfig`
6. `create_managed_disk()` 或 `try_create_managed_disk()`
7. 通过 `snapshot_managed_disks()` 读取 target/lifecycle/online
8. 如需通知某块已存在盘“底层内容已被别处改动”，调用 `notify_managed_disk_data_changed()`
9. 执行宿主侧业务
10. `remove_managed_disk()` 或 `remove_all_managed_disks()`
11. `close()`

## 9. 错误模型

`BackendError` 当前为一组稳定错误码文本。

| 枚举 | `as_code()` | 说明 |
| --- | --- | --- |
| `InvalidHeartbeatIntervalMs` | `invalid-heartbeat-interval-ms` | 心跳配置非法 |
| `InvalidInitialEventQueueCapacity` | `invalid-initial-event-queue-capacity` | 初始事件队列容量非法 |
| `InvalidTargetId` | `invalid-target-id` | target 非法 |
| `InvalidSectorSize` | `invalid-sector-size` | 扇区大小非法 |
| `InvalidDiskSizeBytes` | `invalid-disk-size-bytes` | 盘大小非法 |
| `InvalidQueueDepth` | `invalid-queue-depth` | 队列深度非法 |
| `InvalidWriteSlotBytes` | `invalid-write-slot-bytes` | 写槽大小非法 |
| `InvalidReadWorkerCount` | `invalid-read-worker-count` | 读 worker 数非法 |
| `InvalidWriteWorkerCount` | `invalid-write-worker-count` | 写 worker 数非法 |
| `InvalidAckBatchMaxRanges` | `invalid-ack-batch-max-ranges` | ack range 数非法 |
| `InvalidMediaInstance` | `invalid-media-instance` | 介质实例为空 |
| `MediaSizeMismatch` | `media-size-mismatch` | 介质大小不匹配 |
| `SessionNotOpen` | `session-not-open` | session 未打开 |
| `TargetAlreadyExists` | `target-already-exists` | target 已存在 |
| `TargetNotFound` | `target-not-found` | target 不存在 |
| `NoFreeTarget` | `no-free-target` | 无空闲 target |
| `InvalidParameter` | `invalid-parameter` | 一般参数非法 |

## 10. 常见接入坑

- `size_bytes()` 和 `DiskConfig.disk_size_bytes` 不一致会直接建盘失败。
- `target_id == 255` 只在当前 `BackendContext` 内自动找空位，不扫描系统全局。
- `read_only = true` 是对上层系统暴露的只读盘，不只是 UI 标记。
- 建盘成功不代表宿主需要等待系统设备路径；当前 SDK 不提供这类路径。
- 如果宿主直接校验内存介质，应允许 staged write 到最终 commit 之间存在短暂窗口。
- `notify_managed_disk_data_changed()` 只接单盘目标，不提供共享组或 sibling 批量语义。
