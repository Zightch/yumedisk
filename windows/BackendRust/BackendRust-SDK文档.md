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
  - 可见盘扫描
- 不负责：
  - UI
  - 表单解析
  - 路径对话框
  - 具体介质类型选择
  - 介质内部的缓存、压缩、预读、确认链

核心原则：

- `BackendRust` 只把 `Media` 看作“随机读写逻辑块设备”。
- 具体 `Media` 由宿主创建，再把所有权移交给 `BackendRust`。
- `Media::write_locked()` 返回成功，即表示介质实例已接受本次写入，并承担后续一致性责任。

## 2. 当前稳定公开面

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
- `DiskIdentity`

### 2.2 常量

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
- `enumerate_visible_yumedisks()`
- `make_physical_drive_path()`

以下内容不再视为宿主稳定 API：

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

### 3.1 方法说明

| 方法 | 参数 | 返回 | 说明 |
| --- | --- | --- | --- |
| `size_bytes` | 无 | `u64` | 返回介质总字节数，必须稳定等于建盘时的 `disk_size_bytes` |
| `read_locked` | `offset`, `buffer` | `Result<(), BackendError>` | 从介质读取指定范围到 `buffer` |
| `write_locked` | `offset`, `data` | `Result<(), BackendError>` | 把数据写入介质 |

### 3.2 逻辑要求

- `BackendRust` 不会替你做介质范围扩容。
- `read_locked` / `write_locked` 必须自行保证范围合法。
- 对于越界、状态异常、介质失效，返回 `Err(BackendError::InvalidParameter)` 或你认为更合适的既有错误值。
- `Media` 必须线程安全，因为 `AppKernel` 会并发触发读写。

### 3.3 常见坑

- `size_bytes()` 和 `DiskConfig.disk_size_bytes` 不一致会直接建盘失败。
- `write_locked()` 返回 `Ok(())` 的语义必须谨慎；这代表 core 认为这次提交已经成功。

## 4. `SessionConfig`

```rust
pub struct SessionConfig {
    pub heartbeat_interval_ms: u32,
    pub initial_event_queue_capacity: u32,
}
```

### 4.1 参数表

| 字段 | 类型 | 默认值 | 允许值 | 说明 |
| --- | --- | --- | --- | --- |
| `heartbeat_interval_ms` | `u32` | `1000` | `> 0` | session 心跳周期 |
| `initial_event_queue_capacity` | `u32` | `1024` | `> 0` | 初始事件队列容量 |

### 4.2 逻辑

- `BackendContext::open()` 前可以设置。
- `open()` 后再调用 `set_session_config()`，当前实现会忽略更新并保留旧值。

### 4.3 常见坑

- 设为 `0` 会被 `validate_session_config()` 拒绝。
- 事件队列过小在极端事件风暴下可能更早触发扩容。

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

### 5.1 参数表

| 字段 | 类型 | 默认值 | 允许值 | 说明 |
| --- | --- | --- | --- | --- |
| `target_id` | `u32` | `255` | `0..=254` 或 `255` | `255` 表示自动分配首个空闲 target |
| `sector_size` | `u32` | `4096` | `> 0` | 逻辑扇区大小 |
| `disk_size_bytes` | `u64` | `0` | `> 0` 且能整除 `sector_size` | 盘总大小 |
| `queue_depth` | `u32` | `32` | `> 0` | 队列深度 |
| `write_slot_bytes` | `u32` | `1048576` | `> 0` | 单写槽字节数 |
| `read_worker_count` | `u16` | `4` | `> 0` | 读 worker 数 |
| `write_worker_count` | `u16` | `2` | `> 0` | 写 worker 数 |
| `ack_batch_max_ranges` | `u32` | `32` | `> 0` | 单次 ack 最多 range 数 |
| `read_only` | `bool` | `false` | `true/false` | 是否系统级只读 |

### 5.2 关键取值说明

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `YUMEDISK_MIN_TARGET_ID` | `0` | 最小 target |
| `YUMEDISK_MAX_USABLE_TARGET_ID` | `254` | 最大可用 target |
| `YUMEDISK_MAX_TARGETS` | `255` | 作为“自动分配 target”哨兵值 |

### 5.3 逻辑

- `target_id == 255` 时，core 会自动找首个空闲 target。
- `disk_size_bytes` 必须和 `Media::size_bytes()` 完全相等。
- 当前 `BackendRust` 不替宿主推导 worker 参数，宿主需要显式提供或沿用默认值。

### 5.4 常见坑

- `disk_size_bytes` 不能整除 `sector_size`。
- `target_id > 254` 且不等于 `255` 会直接失败。
- `write_slot_bytes`、`queue_depth`、worker 数设成 `0` 都会失败。
- `read_only = true` 是对上层系统暴露的只读盘，不只是 UI 标记。

## 6. `ManagedDiskSnapshot`

```rust
pub struct ManagedDiskSnapshot {
    pub target_id: u32,
    pub disk_size_bytes: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub visible_path: String,
    pub physical_drive_path: String,
    pub lifecycle_text: String,
    pub online: bool,
}
```

### 6.1 字段说明

| 字段 | 说明 |
| --- | --- |
| `target_id` | target 编号 |
| `disk_size_bytes` | 盘总字节数 |
| `sector_size` | 逻辑扇区大小 |
| `read_only` | 是否只读 |
| `visible_path` | SetupAPI 枚举到的设备接口路径 |
| `physical_drive_path` | `\\\\.\\PhysicalDriveN` 路径 |
| `lifecycle_text` | `init/starting/running/removing/closing/closed/broken` |
| `online` | 当前是否处于 `running` |

### 6.2 常见坑

- 刚建盘后短时间内 `visible_path` / `physical_drive_path` 可能仍为空，宿主应允许短轮询等待。

## 7. `BackendStatsSnapshot`

```rust
pub struct BackendStatsSnapshot {
    pub heartbeat_sent: u64,
    pub command_failures: u64,
    pub protocol_failures: u64,
    pub events_queued: u64,
    pub events_dropped: u64,
    pub disk_count: u64,
}
```

### 7.1 字段说明

| 字段 | 说明 |
| --- | --- |
| `heartbeat_sent` | 已发心跳数 |
| `command_failures` | 命令失败计数 |
| `protocol_failures` | 协议失败计数 |
| `events_queued` | 已入队事件数 |
| `events_dropped` | 已丢弃事件数 |
| `disk_count` | 当前 runtime 内持有的盘数 |

## 8. `DebugSnapshot`

```rust
pub struct DebugSnapshot {
    pub session_state_text: String,
    pub stats: BackendStatsSnapshot,
    pub disks: Vec<ManagedDiskSnapshot>,
}
```

### 8.1 用途

- 一次性拿到 session 文本状态、聚合统计和当前盘快照。
- 适合 UI 调试页、CLI debug 输出、日志面板。

## 9. `DiskIdentity`

```rust
pub struct DiskIdentity {
    pub path: String,
    pub vendor: String,
    pub product: String,
    pub length_bytes: u64,
    pub device_number: u32,
}
```

### 9.1 字段说明

| 字段 | 说明 |
| --- | --- |
| `path` | 设备接口路径 |
| `vendor` | Vendor 字符串 |
| `product` | Product 字符串 |
| `length_bytes` | 盘字节数 |
| `device_number` | 物理盘号 |

## 10. `BackendContext`

`BackendContext` 是宿主的主入口对象。

### 10.1 生命周期方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `new()` | `BackendContext` | 创建上下文 |
| `open()` | `bool` | 打开 session 并启动事件线程 |
| `close()` | `()` | 关闭 session、事件线程和所有盘 |

### 10.2 配置方法

| 方法 | 参数 | 返回 | 说明 |
| --- | --- | --- | --- |
| `set_session_config()` | `SessionConfig` | `Result<(), BackendError>` | 仅在 `open()` 前有效 |
| `session_config()` | 无 | `SessionConfig` | 取当前配置副本 |

### 10.3 查询方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `query_session_state_text()` | `String` | 文本化 session 状态 |
| `snapshot_log_lines()` | `Vec<String>` | 当前缓冲日志 |
| `snapshot_managed_disks()` | `Vec<ManagedDiskSnapshot>` | 当前所有管理盘 |
| `query_backend_stats()` | `bool` | 查询统计，失败时写 `out_error_text` |
| `query_debug_snapshot()` | `bool` | 查询完整 debug 快照 |
| `find_first_free_target()` | `u32` | 找首个空闲 target |

### 10.4 盘管理方法

| 方法 | 参数 | 返回 | 说明 |
| --- | --- | --- | --- |
| `create_managed_disk()` | `DiskConfig`, `Box<dyn Media>`, `Option<&mut String>` | `bool` | 建盘 |
| `remove_managed_disk()` | `target_id`, `Option<&mut String>` | `bool` | 删单盘 |
| `remove_all_managed_disks()` | 无 | `bool` | 删全部宿主持有盘 |

### 10.5 方法逻辑详解

#### `open()`

- 验证 `SessionConfig`
- 创建宿主 stop event
- 调 `AkOpen`
- 启动内部事件线程

失败时：

- 返回 `false`
- 错误会进入内部日志，可通过 `snapshot_log_lines()` 取出

#### `close()`

- 先删全部盘
- 标记 stop
- 唤醒内部 stop event
- 等待事件线程退出
- 调 `AkClose`
- 清理盘 runtime 和 stop event

当前已验证：

- 显式 `close()` 可正常完成
- `Drop` 路径也会复用同一关闭链

#### `create_managed_disk()`

- session 未打开时直接失败
- `target_id == 255` 时自动分配
- 验证 `DiskConfig + Media`
- 建立 runtime
- 调 `AkCreateDisk`
- 轮询刷新可见盘身份

失败时常见错误文本：

| 错误文本 | 含义 |
| --- | --- |
| `session-not-open` | session 未打开 |
| `no-free-target` | 没有空闲 target |
| `target-already-exists` | target 已存在 |
| `invalid-*` | 配置非法 |
| `media-size-mismatch` | 介质大小不匹配 |
| `0xXXXXXXXX` | 来自 `AppKernel` 的状态码 |

#### `remove_managed_disk()`

- session 未打开直接失败
- target 不存在直接失败
- 否则调用 `AkRemoveDisk`
- 清理 runtime、staging、media 持有

#### `remove_all_managed_disks()`

- 只面向宿主显式“一键删全部盘”场景
- 内部关闭链也会复用该能力

## 11. 校验函数

### 11.1 `validate_session_config()`

用途：

- 在宿主提交配置前先做前置检查

失败条件：

- `heartbeat_interval_ms == 0`
- `initial_event_queue_capacity == 0`

### 11.2 `validate_disk_config()`

失败条件：

- `target_id` 越界
- `sector_size == 0`
- `disk_size_bytes == 0`
- `disk_size_bytes % sector_size != 0`
- `queue_depth == 0`
- `write_slot_bytes == 0`
- `read_worker_count == 0`
- `write_worker_count == 0`
- `ack_batch_max_ranges == 0`

### 11.3 `validate_create_disk_inputs()`

额外失败条件：

- `media == None`
- `media.size_bytes() != disk_size_bytes`

## 12. 扫描函数

### 12.1 `enumerate_visible_yumedisks()`

返回：

- 当前系统中可见的 YumeDisk 设备列表

逻辑：

- 走 `SetupAPI`
- 读取 `Vendor` / `Product`
- 当前按 `Zightch` + `YumeDisk` 过滤

常见坑：

- 如果驱动未正常暴露接口，这里可能返回空列表
- 新建盘后需要给系统一点枚举时间

### 12.2 `make_physical_drive_path()`

输入：

- `device_number`

输出：

- `\\\\.\\PhysicalDriveN`

特殊值：

- `u32::MAX` 返回空字符串

## 13. 错误模型

`BackendError` 当前为一组稳定错误码文本。

### 13.1 错误码表

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

## 14. 推荐调用流程

### 14.1 最小宿主流程

1. 创建 `BackendContext`
2. 可选：`set_session_config()`
3. `open()`
4. 创建具体 `Media`
5. 组装 `DiskConfig`
6. `create_managed_disk()`
7. 通过 `snapshot_managed_disks()` 轮询等盘上线
8. 执行宿主侧业务
9. `remove_managed_disk()` 或 `remove_all_managed_disks()`
10. `close()`

### 14.2 建盘前推荐检查

1. `validate_disk_config()`
2. `validate_create_disk_inputs()`
3. 检查 target 是否冲突

## 15. 常见接入坑

### 15.1 建盘成功但马上找不到 `PhysicalDrive`

原因：

- Windows 设备枚举有延迟

建议：

- 用 `snapshot_managed_disks()` 做短轮询
- 或直接用 `enumerate_visible_yumedisks()`

### 15.2 读写通过但内存介质内容暂时不一致

原因：

- 写入先进入 staged write，最终由事件线程收到 commit 再写入介质

建议：

- 如果宿主直接校验内存介质，应允许一个短暂提交窗口

### 15.3 多次 `close()`

当前行为：

- 允许，重复关闭应落到空操作收尾

### 15.4 `remove_all_managed_disks()` 与 `close()`

当前建议：

- UI 的“删全部盘”用 `remove_all_managed_disks()`
- 宿主退出仍显式调用 `close()`

## 16. 当前未承诺内容

- `NetworkMedia` 语义细节
- 稳定的日志回调接口
- 稳定的异步 API
- 稳定的 FFI / C ABI 暴露面
