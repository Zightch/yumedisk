# cache SDK 文档

本文档面向 `cache/` 子项目的接入方和维护者，描述当前 crate 的公开 API、参数口径、运行时语义、测试辅助接口，以及它与 `BackendRust`、`NetworkMedia`、`DiskSession` 的边界。

本文档基于当前仓库代码状态整理，源代码入口见：

- `cache/src/lib.rs`
- `cache/src/cache.rs`
- `cache/src/config.rs`
- `cache/src/error.rs`
- `cache/src/io.rs`
- `cache/src/test_support/*`

## 1. 定位

`cache` 是一个独立 Rust crate，目标是把上层“任意字节范围 I/O”和下层“固定块 I/O”隔开。

当前仓库里的设计口径是：

- 左侧面对 `BackendRust::Media::read_locked/write_locked`
- 中间由 `cache::Cache` 负责块切分、resident、temp、flush worker、背压
- 右侧面对一个 `AtIo` 实现
- 若接入网络盘，`AtIo` 的典型后端会是 `DiskSession` 的适配层

按 `docs/cache/*.md` 和 `docs/network/client/*.md` 的当前收口，`cache` 预期挂在 `NetworkMedia` 内部的 `rw` 路径，`ro` 路径继续直通更合适。

需要明确的一点是：当前 `rust-cli` 和 `tauri-client` 主线代码里的 `NetworkMedia` 仍然直接调用 `DiskSession`，`cache` crate 目前还是独立组件，尚未接入那两条正式运行时链路。

## 2. crate 信息

| 项 | 当前值 |
| --- | --- |
| crate 名 | `cache` |
| 版本 | `0.1.0` |
| edition | `2026` |
| 默认 feature | 无 |
| 可选 feature | `test-hooks` |

`test_support` 模块只在以下条件下公开：

- 单元测试构建 `cfg(test)`
- 或启用 crate feature `test-hooks`

## 3. 目录

### 3.1 对外入口

| 路径 | 作用 |
| --- | --- |
| `cache/src/lib.rs` | crate 根导出，只公开 `Cache`、`CacheConfig`、`CacheError`、`AtIo`，以及 feature-gated 的 `test_support` |
| `cache/src/cache.rs` | 主运行时，包含前台读写、resident slot 准备、rehydrate、flush worker、Drop 停机逻辑 |
| `cache/src/config.rs` | `CacheConfig` 定义 |
| `cache/src/error.rs` | `CacheError` 定义 |
| `cache/src/io.rs` | `AtIo` trait 定义 |

### 3.2 内部模块

| 路径 | 作用 |
| --- | --- |
| `cache/src/block/mod.rs` | 字节请求到逻辑块请求的映射，负责 touched block 计算和右侧块对齐校验 |
| `cache/src/resident/mod.rs` | `2Q = FIFO + LRU` resident 管理、块状态、pending patch、active snapshot |
| `cache/src/resident/list.rs` | resident 队列表基础设施 |
| `cache/src/temp.rs` | temp 文件读写、原子替换、备份回滚 |
| `cache/src/deps.rs` | temp store 和 test hooks 依赖注入 |

### 3.3 测试/调试模块

| 路径 | 作用 |
| --- | --- |
| `cache/src/test_support/io.rs` | 内存/文件版 `AtIo`、I/O 结构化日志、时延模拟 |
| `cache/src/test_support/faults.rs` | 右侧 I/O 和 temp I/O 故障注入 |
| `cache/src/test_support/hooks.rs` | 手工闸门、状态观测 hook |
| `cache/src/test_support/snapshot.rs` | 缓存快照结构 |
| `cache/src/test_support/quiesce.rs` | 等待缓存静稳的辅助函数 |
| `cache/src/test_support/stress.rs` | 随机压测、参考模型、不变量检查 |
| `cache/src/test_support/temp_dir.rs` | 临时目录助手 |

## 4. 当前公开面

crate 根当前只公开以下默认接口：

```rust
pub use cache::Cache;
pub use config::CacheConfig;
pub use error::CacheError;
pub use io::AtIo;
```

额外还有：

```rust
#[cfg(any(test, feature = "test-hooks"))]
pub mod test_support;
```

这意味着正式接入方默认只需要理解 4 个类型：

- `Cache<R>`
- `CacheConfig`
- `CacheError`
- `AtIo`

## 5. `AtIo` trait

```rust
pub trait AtIo: Send + Sync {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError>;
    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError>;
}
```

### 5.1 职责

`AtIo` 是 `cache` 右侧唯一依赖的 I/O 抽象。

它不关心 `BackendRust`、`Media`、session、gateway，只要求实现“按 offset 的随机读写”。

### 5.2 参数语义

| 方法 | 参数 | 含义 |
| --- | --- | --- |
| `read_at` | `offset` | 右侧逻辑起始字节偏移 |
| `read_at` | `buffer` | 读目标缓冲区；成功时必须被完整填满 |
| `write_at` | `offset` | 右侧逻辑起始字节偏移 |
| `write_at` | `data` | 要写出的完整字节序列 |

### 5.3 调用约束

| 约束 | 说明 |
| --- | --- |
| `Send + Sync` | `Cache` 会从多个前台线程和一个后台 worker 线程并发调用右侧 I/O |
| 同步接口 | 当前 crate 没有 async 版本；阻塞发生在调用线程上 |
| 错误类型固定 | 右侧实现必须把自身错误转换成 `CacheError` |
| cache 驱动场景下是整块 I/O | `Cache` 发给 `AtIo` 的调用会是“块起点对齐 + 固定块大小”，不会是上层原始碎片长度 |

### 5.4 接入 `DiskSession` 时的额外要求

如果右侧真正后端是 `network-core::DiskSession`，建议不要直接把 `DiskSession` 当 `AtIo` 暴露给 `Cache`，而是包一层适配器。这个适配器通常还需要持有：

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- terminal error 到 invalidation 的上抛策略

原因有 4 个：

1. `AtIo` 只认 `CacheError`，`DiskSession` 返回的是 `NetworkClientError`
2. `cache` 本身不知道盘总大小，不能替你做越界保护
3. `DiskSession`/`NetworkMedia` 有 `max_io_bytes` 约束，块大小可能需要进一步拆片
4. `cache` 本身没有 invalidation handler，terminal session error 需要由 `NetworkMedia` 之类的外层对象收束

### 5.5 推荐实现规则

| 规则 | 说明 |
| --- | --- |
| 做范围校验 | `cache` 只校验算术溢出，不校验真实介质大小 |
| 对只读盘拒写 | `cache` 本身没有 `read_only` 字段，建议由外层在进入 `write_locked()` 前拦截 |
| 必要时内部拆片 | 若 `block_size_bytes > max_io_bytes`，适配器需要自己拆成多个 `DiskSession::read_at/write_at` |
| 避免旁路写 | 不要一边通过 `Cache` 写，一边又直接绕过缓存写 `DiskSession` 或文件后端 |

## 6. `CacheConfig`

```rust
pub struct CacheConfig {
    pub fifo_capacity_blocks: usize,
    pub lru_capacity_blocks: usize,
    pub block_size_bytes: u32,
    pub dirty_scan_interval: Duration,
    pub temp_max_files: usize,
    pub temp_dir: PathBuf,
}
```

### 6.1 字段说明与取值表

| 字段 | 类型 | 允许值 | 含义 | 备注 |
| --- | --- | --- | --- | --- |
| `fifo_capacity_blocks` | `usize` | `> 0` | FIFO resident 容量 | 新块先进入 FIFO |
| `lru_capacity_blocks` | `usize` | `> 0` | LRU resident 容量 | FIFO 再命中后提升到 LRU |
| `block_size_bytes` | `u32` | `> 0` | 逻辑块大小 | cache 右侧统一按这个块大小读写 |
| `dirty_scan_interval` | `Duration` | `>= 0` | 后台 worker 周期扫描间隔 | `0` 不会停掉 worker，而是被视为 `1ms` |
| `temp_max_files` | `usize` | `> 0` | 同时允许存在的 temp 逻辑文件数量上限 | 按文件数，不按总字节数 |
| `temp_dir` | `PathBuf` | 可写目录路径 | temp 文件根目录 | 当前不会在 `Cache::new()` 里自动创建目录 |

### 6.2 建议取值

| 场景 | 建议 |
| --- | --- |
| 默认块大小起点 | 可以先从 `32 KiB` 起步，这也是当前 `docs/cache/README.md` 里提到的建议值 |
| 接网络盘且右侧不想再拆片 | 让 `block_size_bytes <= SessionMetadata.max_io_bytes` |
| 想要更平稳的热点保留 | `lru_capacity_blocks` 通常不应小于 `fifo_capacity_blocks` |
| 不想过早触发 temp 背压 | `temp_max_files` 至少预留 `2`，更实际的值通常会更高 |
| 想压低 worker 空转 | `dirty_scan_interval` 不要设成 `0` |

### 6.3 当前不会帮你兜底的事情

`CacheConfig` 当前没有这些字段，因此相关约束必须由外层负责：

- 盘总大小 `size_bytes`
- 只读标志 `read_only`
- `max_io_bytes`
- invalidation / close / reopen 策略
- 启动时旧 temp 清理策略

## 7. `CacheError`

```rust
pub enum CacheError {
    InvalidConfig(&'static str),
    InvalidRange { offset: u64, length: usize },
    BufferTooSmall { context: &'static str, expected: usize, actual: usize },
    ArithmeticOverflow(&'static str),
    InvalidBlockDataLength { expected: usize, actual: usize },
    InvalidValidLength { valid_len: usize, block_size: usize },
    MisalignedRightIo { offset: u64, length: usize, block_size: usize },
    TempIo { operation: &'static str, path: PathBuf, kind: ErrorKind },
    ResidentBlockAlreadyExists { block_index: u64 },
    InvariantViolation(&'static str),
    Stopped,
    NotImplemented,
}
```

### 7.1 变体说明

| 变体 | 字段 | 含义 | 常见触发 |
| --- | --- | --- | --- |
| `InvalidConfig` | `reason` | 配置非法 | 块大小为 0、FIFO/LRU 容量为 0、`temp_max_files` 为 0 |
| `InvalidRange` | `offset`、`length` | 请求区间非法 | 区间算术溢出，或右侧测试后端判定越界 |
| `BufferTooSmall` | `context`、`expected`、`actual` | 缓冲区长度不足 | 内部 block copy/patch 的调用上下文不匹配 |
| `ArithmeticOverflow` | `context` | 内部范围或大小计算溢出 | 超大 offset/length 组合 |
| `InvalidBlockDataLength` | `expected`、`actual` | 用来表示“这里必须是整块数据” | 传给整块装载/rehydrate 的缓冲长度不对 |
| `InvalidValidLength` | `valid_len`、`block_size` | resident 元数据里的有效长度非法 | 当前主要是内部不变量保护 |
| `MisalignedRightIo` | `offset`、`length`、`block_size` | 发往右侧的 I/O 不是块对齐整块长度 | 一般说明接入适配器绕开了 `Cache` 约束或内部逻辑被破坏 |
| `TempIo` | `operation`、`path`、`kind` | temp 文件 I/O 失败 | temp 目录不存在、不可写、删除失败、读失败 |
| `ResidentBlockAlreadyExists` | `block_index` | resident 插入重复块 | 主要是内部状态机保护 |
| `InvariantViolation` | `reason` | 缓存内部不变量被破坏 | 状态迁移或并发逻辑异常 |
| `Stopped` | 无 | cache 正在停止 | Drop 期间唤醒等待者 |
| `NotImplemented` | 无 | 占位错误 | 当前主要用于测试故障注入 |

### 7.2 接入时最常见的 5 类错误

| 错误 | 优先排查项 |
| --- | --- |
| `InvalidConfig` | `fifo/lru/temp_max_files/block_size_bytes` 是否大于 0 |
| `TempIo` | `temp_dir` 是否预先存在且可写 |
| `MisalignedRightIo` | 右侧适配器是否被外部直接调用、是否错误地把碎片 I/O 发给 `Cache::read_right_block/write_right_block` 对应路径 |
| `Stopped` | 是否在 cache Drop、runtime 清理、session 失效收束时仍有阻塞请求 |
| `InvalidRange` | 外层是否忘了用 `disk_size_bytes` 做真实越界保护 |

## 8. `Cache<R>`

`Cache<R>` 的泛型参数要求：

```rust
R: AtIo + 'static
```

之所以要求 `'static`，是因为实例内部会启动一个后台 flush worker 线程，worker 会持有 `right`。

### 8.1 构造函数

#### `Cache::new`

```rust
pub fn new(config: CacheConfig, right: R) -> Result<Self, CacheError>
```

参数表：

| 参数 | 含义 |
| --- | --- |
| `config` | 缓存配置 |
| `right` | 右侧后端 I/O 实现 |

行为：

- 校验 `temp_max_files > 0`
- 通过 `BlockLayout` 校验 `block_size_bytes > 0`
- 通过 resident 初始化校验 `fifo_capacity_blocks > 0`、`lru_capacity_blocks > 0`
- 创建内部共享状态
- 立即启动后台 flush worker

不会做的事：

- 不校验真实介质大小
- 不校验 `temp_dir` 是否存在
- 不清理旧 temp 文件
- 不检查右侧是不是只读

#### `Cache::new_for_test`

```rust
#[cfg(any(test, feature = "test-hooks"))]
pub fn new_for_test(
    config: CacheConfig,
    right: R,
    hooks: TestHooks,
) -> Result<Self, CacheError>
```

只比 `new()` 多一个 `hooks` 参数，用于插入测试闸门和状态观测。

#### `Cache::new_for_test_with_temp_failures`

```rust
#[cfg(any(test, feature = "test-hooks"))]
pub fn new_for_test_with_temp_failures(
    config: CacheConfig,
    right: R,
    hooks: TestHooks,
    temp_failures: TempFailureController,
) -> Result<Self, CacheError>
```

在 `new_for_test()` 的基础上再注入 temp 故障控制器。

### 8.2 只读访问器

#### `config`

```rust
pub fn config(&self) -> &CacheConfig
```

返回当前实例持有的配置只读引用。

#### `right`

```rust
pub fn right(&self) -> &R
```

返回右侧 `AtIo` 对象的只读引用。

这个接口主要适合：

- 测试中读取日志
- 测试中检查底层存储状态

不建议在正式路径里拿到 `right()` 后绕过 cache 直接读写，否则会破坏缓存一致性。

### 8.3 前台读接口

#### `read_locked`

```rust
pub fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError>
```

参数表：

| 参数 | 含义 |
| --- | --- |
| `offset` | 上层字节偏移，可以不是块对齐 |
| `buffer` | 输出缓冲区，长度就是本次逻辑读长度 |

返回语义：

- `Ok(())` 表示缓存层已经把目标字节填入 `buffer`
- 不表示后台 dirty 数据已经 flush
- 也不表示 temp 已清理

当前行为：

1. 把 `(offset, buffer.len())` 映射成若干 touched block
2. 对每个块优先查 resident
3. resident hit 直接拷贝目标字节范围
4. resident miss 且该块存在 `spilled_dirty` 时，先从 temp rehydrate
5. 两边都没有时，向右侧发一个“块起点对齐 + 整块长度”的读
6. 同一块如果已经在 `LoadingRemote` 或 `Rehydrating`，后续请求不会重复发右侧读，而是等待已有加载完成

补充说明：

- `buffer.len() == 0` 时直接返回 `Ok(())`
- 当前只校验区间算术是否溢出，不校验是否超过真实盘大小
- 发生 `dirty victim + temp 满` 的 miss 时，本次读可能阻塞等待 temp slot

### 8.4 前台写接口

#### `write_locked`

```rust
pub fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), CacheError>
```

参数表：

| 参数 | 含义 |
| --- | --- |
| `offset` | 上层字节偏移，可以不是块对齐 |
| `data` | 要写入的原始字节数据 |

返回语义：

- `Ok(())` 只表示本地缓存已经接受这次写
- 不表示远端已经确认
- 不表示这次写已经 crash-safe

当前行为：

1. 把 `(offset, data.len())` 映射成若干 touched block
2. resident hit 时，直接在 resident block 上 patch，并标脏
3. resident miss 时，先得到完整块视图，再应用 patch
4. 若该块正在 `LoadingRemote/Rehydrating`，新写会作为 pending patch 挂起，等完整块回来后统一应用
5. 前台写不会直接调用右侧 `write_at`
6. 真正的远端写由后台 worker 在后续 flush 时推进

补充说明：

- `data.is_empty()` 时直接返回 `Ok(())`
- 如果外层没有先拦截只读盘，这里不会自动阻止写入
- 当前 miss 也可能因为 temp 压力进入阻塞

### 8.5 调试快照

#### `debug_snapshot`

```rust
#[cfg(any(test, feature = "test-hooks"))]
pub fn debug_snapshot(&self) -> CacheSnapshot
```

作用：

- 读取 resident / spilled / active temp / waiters / stop flag 的结构化快照
- 供测试、不变量检查、手工断言使用

注意：

- 这不是默认稳定运行时 API
- 正式接入方不应依赖它做业务逻辑

### 8.6 生命周期与 `Drop`

`Cache` 当前没有显式的：

- `flush()`
- `close()`
- `shutdown()`

实例析构时只会做两件事：

1. 置 `stop_requested = true`
2. 唤醒等待线程并 `join` 后台 worker

这意味着：

- Drop 不会等待所有 dirty 数据刷完
- Drop 不会保证 temp 被清空
- Drop 不会做“写返回即落远端”的补偿

如果上层需要严格的卸载前 flush 语义，当前 crate 本身还没有提供正式 API，必须由外层另加收束策略。

## 9. 运行时语义

### 9.1 左右边界

| 边界 | 当前语义 |
| --- | --- |
| 左侧 | 任意 offset、任意长度的字节请求 |
| 右侧 | 按 `block_size_bytes` 对齐、固定长度的块请求 |

这是 `cache` 最核心的边界约束。

### 9.2 resident 策略

resident 当前是固定的 `2Q = FIFO + LRU`：

- 新块首次装入 resident 进入 FIFO
- FIFO 中块再次命中后提升到 LRU
- LRU 中块再次命中后移动到 LRU 尾部

victim 选择规则：

- FIFO 未满时优先占 FIFO 空位
- FIFO 满则优先看 FIFO 头
- FIFO 没有可淘汰 victim 时再看 LRU 头

### 9.3 状态机口径

虽然这些状态不是公开 enum，但对接入理解很关键：

| 逻辑状态 | 含义 |
| --- | --- |
| `Missing` | resident 和 spilled 都没有这块 |
| `LoadingRemote` | 正在从右侧整块拉取 |
| `Rehydrating` | 正在从 temp 恢复 spilled dirty |
| `Ready + clean` | resident 有完整块且未脏 |
| `Ready + dirty` | resident 有完整块且带脏区间 |
| `Ready + active_snapshot` | resident 上已有一份 temp snapshot 正在或等待 flush |
| `SpilledDirty` | resident 已没有这块，temp 是当前唯一权威副本 |

### 9.4 temp 文件语义

当前 temp 文件名固定是：

```text
block-<block_index>.temp
```

还会用到两个过程文件：

```text
block-<block_index>.temp.write
block-<block_index>.temp.prev
```

它们的作用：

- `.temp.write`
  - 新快照写入中的临时文件
- `.temp.prev`
  - 原 temp 替换前的备份文件

`temp_max_files` 统计口径不是“目录中文件个数”，而是：

- `spilled_dirty.len()`
- `resident.active_snapshot_count()`

也就是“当前逻辑上被 temp 支撑的块数量”。

### 9.5 flush worker 语义

worker 线程在 `Cache::new()` 时立即启动，固定名称是：

```text
cache-flush-worker
```

每轮策略：

1. 先找已经存在的 temp
2. 优先 flush `spilled_dirty`
3. 再 flush resident 上已经有 active snapshot 的块
4. 只有在没有可发 temp 时，才尝试为 resident dirty 新建 snapshot

如果 `dirty_scan_interval == 0`，worker 实际等待时间会被收成 `1ms`，不是停掉扫描。

### 9.6 前台背压语义

当前只在一种核心场景下对 miss 施加阻塞：

- 需要新 resident slot
- victim 是 dirty
- 这个 dirty victim 还没有可复用 temp
- 且 `temp_max_files` 已满

此时：

- 这个 miss 会等待 temp slot
- resident hit 不会被连坐阻塞
- 等待者在 temp 释放后被唤醒并重新判定
- 若 cache 正在停止，则等待者返回 `CacheError::Stopped`

### 9.7 错误处理语义

| 场景 | 当前行为 |
| --- | --- |
| 右侧读失败 | 当前次 `read_locked()/write_locked()` 返回错误，placeholder 回滚 |
| temp rehydrate 读失败 | 当前次请求返回错误，placeholder 回滚，spilled 状态保留 |
| worker flush 读/写/temp 删除失败 | worker 本轮放弃该块，temp 保留，等待后续重试 |
| Drop 触发 stop | 阻塞等待者被唤醒并返回 `Stopped` |

需要注意：`cache` 当前没有“terminal right-side error -> 自动 invalidation”这层宿主策略，这一层还需要 `NetworkMedia` 或更外层 runtime 自己补。

## 10. 与 `BackendRust` / `NetworkMedia` / `DiskSession` 的边界

### 10.1 `BackendRust`

`BackendRust::Media` 当前接口是：

```rust
fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError>;
fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError>;
```

`cache` 和它的边界是：

- 接收与 `Media` 一样的字节语义
- 不参与 staging overlay
- 不知道 target/session 生命周期
- 不输出 `BackendError`，而是输出 `CacheError`

也就是说，真正把 `cache` 接进 `BackendRust` 的位置，更自然的是 `NetworkMedia` 内部，而不是直接让 `BackendRust` 知道 `CacheError`。

### 10.2 `NetworkMedia`

当前 `rust-cli` 和 `tauri-client` 里的 `NetworkMedia` 负责：

- 持有 `disk_id` / `remote_disk_id`
- 持有 `DiskSession`
- 持有 `disk_size_bytes`
- 持有 `read_only`
- 持有 `max_io_bytes`
- 持有 invalidation handler
- 把 `NetworkClientError` 映射成 `BackendError`

这些事情当前都不属于 `cache`。

更实际的接法是：

- `NetworkMedia`
  - 继续保留 metadata、只读语义、invalidation
  - 对 `rw` 实例持有一个 `Cache<DiskSessionAtIo>`
  - 对 `ro` 实例继续直通或根本不建 cache

- `DiskSessionAtIo`
  - 负责把 `DiskSession` 封装成 `AtIo`
  - 负责真实越界校验、必要的 `max_io_bytes` 拆片、错误映射

### 10.3 `DiskSession`

`network-core::DiskSession` 当前只提供：

- `read_at`
- `write_at`
- `close`
- `ensure_usable`

它本身并不理解：

- resident
- temp
- flush worker
- dirty eviction

因此 `cache` 右侧若接 `DiskSession`，`DiskSession` 只是“数据平面 I/O 执行者”，不是缓存管理者。

## 11. `test_support` 模块

`test_support` 只在 `cfg(test)` 或 feature `test-hooks` 下公开。

它适合：

- 为接入层写确定性测试
- 注入 temp 或右侧 I/O 故障
- 把并发时序卡在精确位置
- 读取缓存内部快照做断言

### 11.1 导出总表

| 分类 | 公开项 |
| --- | --- |
| 故障注入 | `FailureRuleId`、`IoFailureController`、`TempFailureController`、`TempFaultOperation` |
| Hooks | `GateHook`、`HookPoint`、`ManualGateController`、`StateDumpHook`、`TestHooks` |
| 测试 I/O | `FileBackedAtIo`、`IoLogEntry`、`IoOperation`、`IoTimings`、`MemoryAtIo`、`TestAtIo` |
| 等待辅助 | `QuiesceTimeout`、`wait_for_quiesce()`、`wait_until()` |
| 快照 | `CacheSnapshot`、`LoadStateSnapshot`、`QueueKindSnapshot`、`ResidentBlockSnapshot`、`ResidentSnapshot`、`SpilledDirtySnapshot` |
| 压测/模型 | `DeterministicRng`、`ReferenceModel`、`assert_runtime_invariants()`、`assert_quiesced_invariants()`、`assert_snapshot_invariants()`、`assert_temp_artifacts_cleared()`、`collect_temp_artifacts()` |
| 临时目录 | `TestTempDir` |

### 11.2 枚举与快照类型

#### `IoOperation`

| 值 | 含义 |
| --- | --- |
| `Read` | 右侧读调用 |
| `Write` | 右侧写调用 |

#### `TempFaultOperation`

| 值 | 含义 |
| --- | --- |
| `Write` | temp 写入 |
| `Read` | temp 读取 |
| `Delete` | temp 删除 |

#### `HookPoint`

| 值 | 触发点 |
| --- | --- |
| `DebugSnapshot` | 调用 `debug_snapshot()` 时 |
| `BeforeRightRead` | 右侧整块读前 |
| `AfterRightRead` | 右侧整块读后 |
| `BeforeRightWrite` | 右侧整块写前 |
| `AfterRightWrite` | 右侧整块写后 |
| `BeforeDirtyVictimSpillTempWrite` | dirty victim spill 到 temp 前 |
| `AfterDirtyVictimSpillTempWrite` | dirty victim spill 到 temp 后 |

#### `QueueKindSnapshot`

| 值 | 含义 |
| --- | --- |
| `Fifo` | 该 resident block 当前在 FIFO |
| `Lru` | 该 resident block 当前在 LRU |

#### `LoadStateSnapshot`

| 值 | 含义 |
| --- | --- |
| `LoadingRemote` | 正在从右侧读取整块 |
| `Rehydrating` | 正在从 temp 恢复 |
| `Ready` | 已可用于前台读写 |

#### `IoLogEntry`

| 字段 | 含义 |
| --- | --- |
| `sequence` | 调用顺序号，从 1 开始 |
| `operation` | `Read` 或 `Write` |
| `offset` | 调用偏移 |
| `length` | 调用长度 |
| `block_index` | 按测试后端 `block_size_bytes` 推导的块索引 |
| `result` | 本次调用结果 |

#### `IoTimings`

| 字段 | 含义 |
| --- | --- |
| `read_delay` | 每次测试读前的人为延迟 |
| `write_delay` | 每次测试写前的人为延迟 |

#### `ResidentBlockSnapshot`

| 字段 | 含义 |
| --- | --- |
| `block_index` | resident 中的逻辑块索引 |
| `queue` | 当前位于 FIFO 还是 LRU |
| `load_state` | 当前装载状态 |
| `dirty_ranges` | 已标脏区间列表 |
| `has_active_snapshot` | 是否已经有 active temp snapshot |
| `pending_patch_count` | 挂起 patch 数量 |
| `valid_len` | 当前记录的有效长度 |

#### `ResidentSnapshot`

| 字段 | 含义 |
| --- | --- |
| `fifo_order` | FIFO 队列顺序 |
| `lru_order` | LRU 队列顺序 |
| `blocks` | 所有 resident block 快照 |

#### `SpilledDirtySnapshot`

| 字段 | 含义 |
| --- | --- |
| `block_index` | spilled 脏块索引 |
| `valid_len` | 该块记录的有效长度 |

#### `CacheSnapshot`

| 字段 | 含义 |
| --- | --- |
| `resident` | resident 总快照 |
| `spilled_dirty` | 当前所有 spilled dirty |
| `active_temp_blocks` | 当前正占用 temp 的块 |
| `foreground_dirty_eviction_waiters` | 因 temp 压力阻塞的前台 miss 数 |
| `stop_requested` | 是否正在停止 |

额外公开方法：

```rust
impl CacheSnapshot {
    pub fn is_quiescent(&self) -> bool
}
```

`is_quiescent()` 只有在以下条件同时满足时才返回 `true`：

- 没有 `spilled_dirty`
- 没有 `active_temp_blocks`
- 没有前台 temp 等待者
- 没有 `stop_requested`
- resident 中所有块都 `Ready`
- resident 中没有 dirty range
- resident 中没有 active snapshot
- resident 中没有 pending patch

### 11.3 测试 I/O 接口

#### `TestAtIo`

```rust
pub trait TestAtIo: AtIo {
    fn log_snapshot(&self) -> Vec<IoLogEntry>;
    fn take_log(&self) -> Vec<IoLogEntry>;
    fn in_flight_count(&self) -> usize;
}
```

用途：

- 读取结构化 I/O 日志
- 统计正在执行中的右侧 I/O 数

#### `MemoryAtIo`

```rust
pub struct MemoryAtIo;
```

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `from_bytes(block_size_bytes, storage)` | 块大小、初始字节数组 | 构造内存版测试后端 |
| `with_timings(block_size_bytes, storage, timings)` | 同上，加时延 | 构造带固定时延的内存后端 |
| `with_failures(block_size_bytes, storage, timings, failures)` | 同上，加故障注入 | 构造带故障规则的内存后端 |
| `log_snapshot()` | 无 | 读取当前日志副本 |
| `take_log()` | 无 | 取出并清空日志 |
| `in_flight_count()` | 无 | 当前在飞 I/O 数 |
| `storage_slice(offset, length)` | 字节区间 | 读取底层内存内容 |

#### `FileBackedAtIo`

```rust
pub struct FileBackedAtIo;
```

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `create(path, block_size_bytes, storage)` | 文件路径、块大小、初始内容 | 构造文件版测试后端 |
| `create_with_timings(path, block_size_bytes, storage, timings)` | 同上，加时延 | 构造带时延的文件后端 |
| `create_with_failures(path, block_size_bytes, storage, timings, failures)` | 同上，加故障注入 | 构造带故障规则的文件后端 |
| `path()` | 无 | 返回底层文件路径 |
| `log_snapshot()` | 无 | 读取当前日志副本 |
| `take_log()` | 无 | 取出并清空日志 |
| `in_flight_count()` | 无 | 当前在飞 I/O 数 |
| `storage_slice(offset, length)` | 字节区间 | 直接从底层文件读取内容 |

### 11.4 故障注入

#### `IoFailureController`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `new()` | 无 | 创建控制器 |
| `fail_once(operation, block_index, error)` | 操作、块索引过滤、错误值 | 第一次匹配即失败，然后自动清除 |
| `fail_matching_call(operation, block_index, matching_call, error)` | 第 N 次匹配失败 | `matching_call` 是 1-based |
| `fail_persistently(operation, block_index, error)` | 操作、块索引过滤、错误值 | 每次匹配都失败，直到手工清除 |
| `clear_rule(rule_id)` | 规则 ID | 删除单条规则，返回是否真的删除了 |
| `clear_all()` | 无 | 清空全部规则 |

参数说明：

| 参数 | 含义 |
| --- | --- |
| `operation` | `IoOperation::Read/Write` |
| `block_index` | `Some(x)` 只匹配某个块；`None` 匹配任意块 |
| `matching_call` | 第几次命中规则时触发，必须 `> 0` |
| `error` | 要返回的 `CacheError` |

#### `TempFailureController`

公开方法和 `IoFailureController` 同构，只是错误载体改成 `std::io::ErrorKind`，操作类型改成 `TempFaultOperation`。

#### `FailureRuleId`

`FailureRuleId` 是清理规则时使用的句柄类型，本身不暴露内部数值语义。

### 11.5 Hook 与状态观测

#### `GateHook`

```rust
pub trait GateHook: Send + Sync {
    fn reach(&self, point: HookPoint);
}
```

用于在特定 hook 点阻塞或放行执行流。

#### `StateDumpHook`

```rust
pub trait StateDumpHook: Send + Sync {
    fn observe(&self, point: HookPoint, snapshot: &CacheSnapshot);
}
```

用于在观测点接收结构化快照。

#### `ManualGateController`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `new()` | 无 | 创建手工闸门 |
| `wait_until_reached(point, expected_arrivals, timeout)` | hook 点、期望到达次数、超时 | 等待某个点至少被到达 N 次，返回是否成功 |
| `arrival_count(point)` | hook 点 | 返回当前到达次数 |
| `release_one(point)` | hook 点 | 放行一个当前已阻塞到该点的执行流 |
| `release_all_current(point)` | hook 点 | 放行当前已经到达该点的全部执行流 |
| `open(point)` | hook 点 | 永久打开该点，之后新到达的执行流不再阻塞 |

#### `TestHooks`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `disabled()` | 无 | 返回全禁用 hooks |
| `new(gate, state_dump)` | 两个 `Arc<dyn ...>` | 同时指定闸门和状态观测 |
| `with_gate(gate)` | `Arc<dyn GateHook>` | 只指定闸门，状态观测使用 noop |
| `with_state_dump(state_dump)` | `Arc<dyn StateDumpHook>` | 只指定状态观测，闸门使用 noop |

### 11.6 等待与静稳辅助

#### `QuiesceTimeout`

| 字段 | 含义 |
| --- | --- |
| `last_snapshot` | 超时前最后一次读到的缓存快照 |
| `in_flight_right_io` | 超时前最后一次观测到的在飞右侧 I/O 数 |

#### `wait_until`

```rust
pub fn wait_until<F>(timeout: Duration, predicate: F)
where
    F: FnMut() -> bool
```

语义：

- 轮询等待 `predicate()` 为真
- 超时后直接 `assert!` 失败

#### `wait_for_quiesce`

```rust
pub fn wait_for_quiesce<R>(
    cache: &Cache<R>,
    timeout: Duration,
) -> Result<CacheSnapshot, QuiesceTimeout>
where
    R: AtIo + TestAtIo + 'static
```

语义：

- 等待缓存进入“快照静稳 + 右侧无在飞 I/O”状态
- 成功返回静稳快照
- 失败返回超时信息

### 11.7 压测与不变量

#### `DeterministicRng`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `new(seed)` | 初始种子 | 构造确定性随机数源，`seed == 0` 会替换成固定非零种子 |
| `next_u64()` | 无 | 产生下一个 `u64` |
| `next_bool()` | 无 | 产生布尔值 |
| `one_in(denominator)` | 分母 | 以 `1/denominator` 概率返回 `true` |
| `next_usize(upper_exclusive)` | 上界 | 返回 `[0, upper_exclusive)` |
| `range_usize(start_inclusive, end_exclusive)` | 区间 | 返回指定半开区间内值 |
| `fill_bytes(buffer)` | 缓冲区 | 用确定性随机字节填充缓冲区 |

#### `ReferenceModel`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `from_bytes(bytes)` | 初始完整内容 | 创建参考模型 |
| `len()` | 无 | 返回总长度 |
| `read(offset, length)` | 偏移、长度 | 读取参考内容 |
| `write(offset, data)` | 偏移、写入数据 | 更新参考内容 |
| `as_slice()` | 无 | 返回整体切片 |

#### 不变量检查与 temp 清理检查

| 方法 | 作用 |
| --- | --- |
| `assert_runtime_invariants(cache, config)` | 检查运行中不变量，返回当前快照 |
| `assert_quiesced_invariants(cache, config, timeout)` | 先等静稳，再检查不变量 |
| `assert_snapshot_invariants(snapshot, config)` | 对一个快照直接做结构检查 |
| `collect_temp_artifacts(root)` | 列出 temp 目录中残留文件 |
| `assert_temp_artifacts_cleared(root)` | 断言 temp 目录已经清空 |

### 11.8 `TestTempDir`

公开方法：

| 方法 | 参数 | 作用 |
| --- | --- | --- |
| `new()` | 无 | 创建默认前缀临时目录 |
| `with_prefix(prefix)` | 目录前缀 | 创建带指定前缀的临时目录 |
| `path()` | 无 | 返回目录路径 |
| `child(name)` | 相对子路径 | 生成子路径 |
| `create_file(name, data)` | 文件名、内容 | 在该目录下创建文件 |

额外行为：

- 实现了 `Default`
- `Drop` 时会 best-effort 删除整个目录

## 12. 常见坑

### 12.1 `temp_dir` 不会自动创建

`Cache::new()` 只保存 `temp_dir`，不会 `create_dir_all()`。

影响：

- 构造时可能不报错
- 第一次真的需要 spill/snapshot 时才在 temp 写入处报 `TempIo`

建议：

- 外层在建 cache 前先创建并检查目录

### 12.2 `write_locked()` 成功不等于远端已落盘

当前语义只是“本地缓存接受成功”，不是“远端确认成功”。

如果沿用 `NetworkMedia` 当前“写成功更接近直通确认”的心智模型，会误判一致性。

### 12.3 Drop 不会自动 flush 脏数据

当前 Drop 只是停 worker，不是“优雅刷盘并关闭”。

这对接入 `DiskSession` 尤其重要：

- session 关闭前若想尽量把脏数据推完，外层必须另建显式收束步骤

### 12.4 cache 不知道真实盘大小

`Cache` API 没有 `size_bytes()`。

所以：

- 它能拦的只有算术溢出
- 拦不住“落在真实盘尾之后但没溢出”的请求

建议：

- 在 `NetworkMedia` 或 `AtIo` 适配器层使用 `disk_size_bytes` 做真实范围校验

### 12.5 cache 不处理只读盘语义

`CacheConfig` 和 `Cache` 都没有 `read_only`。

若对 `tauri-client` 的 `configured_read_only || metadata.read_only` 盘仍然把写请求放进 cache，就会得到错误的写接受语义。

建议：

- 只在 `rw` 路径实例化 cache
- 或者在外层 `write_locked()` 进入 cache 前先拒写

### 12.6 `block_size_bytes` 和 `max_io_bytes` 可能冲突

当前 `rust-cli`/`tauri-client` 的 `NetworkMedia` 会按 `max_io_bytes` 拆片，`cache` 则要求右侧看到的是整块 I/O。

若你把 `DiskSession` 直接包成一个“不再拆片”的 `AtIo`，就必须保证：

- `block_size_bytes <= max_io_bytes`

否则要在 `AtIo` 适配器内部自己继续拆片。

### 12.7 不要旁路访问 `right()`

`right()` 只是暴露底层后端引用，不是让正式路径绕过 cache 的 escape hatch。

旁路直接写 `right()` 会导致：

- resident 脏数据和底层真实数据失去一致性
- debug 快照不再可信

### 12.8 `dirty_scan_interval = 0` 不是关闭后台 flush

当前实现会把 `0` 收成 `1ms`。

结果通常是：

- worker 高频醒来
- CPU 空转上升

### 12.9 当前没有 invalidation handler

`NetworkMedia` 目前能在 terminal session error 时触发 invalidation handler；`cache` 自身没有这层钩子。

因此：

- 右侧 `AtIo` 适配器必须自己决定如何把 terminal error 抛给宿主
- 否则你只能拿到一个 `CacheError`，却没有统一的 runtime 收束动作

## 13. 最小接入建议

如果要把 `cache` 接入当前网络盘主线，建议顺序是：

1. 在 `NetworkMedia` 内部引入一个 `DiskSessionAtIo`
2. 让 `DiskSessionAtIo` 负责：
   - `disk_size_bytes` 越界保护
   - `read_only` 拒写
   - `max_io_bytes` 拆片
   - `NetworkClientError -> CacheError` 映射
   - terminal error 上抛给 invalidation handler
3. 只在 `rw` 实例创建 `Cache<DiskSessionAtIo>`
4. `ro` 继续保留当前直通模型
5. 在 session close / runtime eject 之前，再补一个外层的“等待静稳或超时放弃”收束步骤

这会比把 `Cache` 直接暴露给 `BackendRust` 更符合当前项目的职责边界。
