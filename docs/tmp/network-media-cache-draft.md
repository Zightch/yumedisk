# NetworkMedia 缓存草案

## 1. 这份草案要解决什么

这份草案只讨论 `windows/tauri-client/src-tauri/src/backend/network_media.rs`。

目标很简单：

- 只改 `NetworkMedia`
- 把它从“按请求直通远端”的薄适配层，改成“本地 32 KiB 块缓存层”
- 其余层继续不感知缓存

明确不做：

- 不把缓存状态放到 `BackendRust`
- 不把缓存状态放到 `network-core`
- 不改 server 数据面
- 不改正式协议
- 不改前端状态层

## 2. 当前边界

当前链路里，和这个设计直接相关的事实只有 3 个：

- `NetworkMedia` 现在只是透传
  - `read_locked()` 直接调用 `session.read_at(...)`
  - `write_locked()` 直接调用 `session.write_at(...)`
- `BackendRust` 不会把半包写直接交给 `NetworkMedia`
  - AppKernel 写碎片先留在 `staging`
  - 只有 `WriteFinalCommitted` 之后才真正调用 `media.write_locked(...)`
- server 当前只是无状态转发
  - gateway 转给 storer
  - storer 再落到文件后端

所以这件事可以只收在 `NetworkMedia` 里做。

## 3. 一句话模型

`NetworkMedia` 内部维护一份 32 KiB 对齐块缓存。

这份缓存有 4 个核心部分：

- 2Q 表
  - `FIFO + LRU`
- 单块状态机
  - 读 miss、写 miss、dirty、spill 都靠它驱动
- temp 文件
  - dirty 数据离开内存前，必须先落 temp
- 背压策略
  - resident 空间和 temp 空间都有限，打满后要阻塞需要新资源的请求

## 4. 固定约束

### 4.1 块模型

- 固定逻辑块大小：`32 KiB`
- 固定全局对齐：块起点只能是 `N * 32 KiB`
- 缓存内部只允许存在规范块，不允许滑动窗口块

块映射公式固定为：

```text
block_size  = 32768
block_index = offset / 32768
block_base  = block_index * 32768
block_range = [block_base, block_base + 32768)
```

尾块规则：

- 逻辑块大小仍然按 `32 KiB` 看
- 但真实有效长度是 `block_valid_len`
- 读远端只读 `block_valid_len`
- 写远端也只写 `block_valid_len`

### 4.2 2Q 模型

缓存只用两条队列：

- `FIFO`
  - 新块第一次进入缓存时放这里
- `LRU`
  - FIFO 里的块再次命中后提升到这里

规则很直接：

```text
首次装入   -> FIFO 尾部
FIFO 再命中 -> 提升到 LRU 尾部
LRU 再命中  -> 移动到 LRU 尾部
```

### 4.3 写 miss 规则

写 miss 不能直接远端写局部数据。

固定规则：

- miss 写必须先拿完整对齐块
- 再把 patch 打到这块上

也就是说：

- 不能构造“不完整块”
- 不能把局部 patch 直接发给 server

### 4.4 单块 flush 规则

固定约束：

- 同一块同一时刻只能有一个 active flush snapshot
- 不做同块多个 active snapshot
- 不用块版本号来解决并发

后续如果这块又被写：

- 只改 resident 数据
- 只重新标脏位
- 不去改已经在发送的 temp 文件

### 4.5 temp 文件规则

temp 文件只做两件事：

- dirty block 淘汰前的权威副本
- resident dirty block flush 时的发送载体

它不是：

- WAL
- 崩溃恢复日志
- 启动后 replay 的持久化协议

所以：

- 启动时可以直接清旧 temp
- 进程重启后不做恢复

### 4.6 temp 数量上限

temp 文件按“单盘文件数”限，不按“总字节数”限。

原因很简单：

- 块大小固定是 `32 KiB`
- temp 文件初版不引入复杂文件头
- 直接按文件数限，和块级 flush 语义更对齐

建议保留一个内部常量，例如：

- `TEMP_MAX_FILES_PER_DISK`

### 4.7 背压规则

这里不做“全局冻结”，而是做“资源阻塞”。

固定规则：

- `read hit`
  - 直接放行
- `write hit` 到已 resident 块
  - 一般直接放行
- 只有那些“必须拿到新 resident 空间”或“必须拿到新 temp slot”才能继续的请求，才阻塞

典型阻塞场景：

- `read miss`
  - 需要新 resident block
  - 但当前只能通过 dirty eviction 腾位
  - 而 dirty eviction 又因为 temp 满了做不了
- `write miss`
  - 同理
- dirty block 淘汰
  - 需要先落 temp
  - 但 temp 已满

阻塞结束的时机：

- 有 temp 文件 flush 成功并删除，释放 slot
- 有 clean block 被淘汰，释放 resident 空间
- session 进入 terminal invalidation，等待请求直接失败
- `Drop / eject`，等待请求统一退出

### 4.8 网络约束

当前 server 默认 `MaxIOBytes = 60 * 1024`，大于 `32 KiB`。

所以 `NetworkMedia::bind(...)` 需要额外校验：

- `metadata.max_io_bytes >= 32 * 1024`

小于这个值就直接拒绝挂载。

## 5. 核心对象

这部分不用想复杂，内部就 5 类东西。

### 5.1 `NetworkMedia`

对外实现 `Media`。

它对外只暴露：

- `size_bytes()`
- `read_locked()`
- `write_locked()`

### 5.2 `Inner`

共享状态入口，建议挂在 `Arc<Inner>` 上。

原因：

- worker 要和前台 I/O 共用同一份状态
- `Media` 是按 `&self` 调用
- `Drop` 时要统一停 worker、唤醒等待者、清 temp

### 5.3 `CacheState`

这就是缓存层真正的核心表。

建议至少有：

- `blocks`
  - resident block 表，键是 `block_index`
- `fifo`
  - FIFO 队列
- `lru`
  - LRU 队列
- `spilled_dirty`
  - 已经离开内存、但 temp 还在的块
- `next_temp_id`
  - temp 文件命名用
- `waiting_requests`
  - 资源不足时的阻塞辅助状态
- `worker_notified`
  - 减少重复唤醒

### 5.4 `BlockEntry`

一个 resident block 至少需要这些信息：

- `block_index`
- `valid_len`
- `data`
- `queue_kind`
  - 当前在 FIFO 还是 LRU
- `access_count`
  - 用来判断是否需要 FIFO -> LRU
- `load_state`
  - `Ready / LoadingRemote / RehydratingFromTemp`
- `dirty_ranges`
  - 当前还没被 snapshot 接管的脏位置
- `active_snapshot`
  - 当前唯一 active flush snapshot；没有就是空
- `pending_patches`
  - 写 miss 或 rehydrate 时挂起的 patch

### 5.5 `SpilledDirtyEntry`

一个 spilled dirty block 至少需要：

- `block_index`
- `valid_len`
- `temp_path`
- `state`
  - `PendingSend / InFlight / RetryPending`

## 6. 状态机

单块状态不用画得太细，理解下面 6 个状态就够了。

### 6.1 `Missing`

块不在 resident，也不在 spilled。

### 6.2 `LoadingRemote`

块不在本地，前台线程正在远端补整块。

这时：

- 后续同块读写不重复发读
- 只等待这次装载完成

### 6.3 `ResidentClean`

块在 resident 里，没有脏位，也没有 active snapshot。

### 6.4 `ResidentDirty`

块在 resident 里，带脏位，还没有 active snapshot。

### 6.5 `ResidentSnapshotOwned`

块还在 resident 里，但已经有一份 active snapshot 在负责发送。

此时可能有两种情况：

- resident 上已经没有旧脏位了
  - 因为它们已经被 snapshot 接管
- resident 上又出现了新脏位
  - 因为 snapshot 发送期间，这块又被写了

### 6.6 `SpilledDirty`

块已经离开 resident。

这时：

- temp 文件是唯一权威副本
- 后续读写不能直接把它当普通 miss 处理

## 7. 读策略

### 7.1 总规则

`read_locked(offset, buffer)` 的流程：

1. 先校验范围
2. 按 `32 KiB` 拆成触达块
3. 每块优先查 resident
4. resident miss 再查 `spilled_dirty`
5. 两边都没有才走远端 aligned read

### 7.2 resident hit

如果块已经在 resident 且 `load_state = Ready`：

- 直接从 `data` 里拷贝需要的部分
- 按 2Q 规则更新 FIFO/LRU

### 7.3 resident loading

如果块正在：

- `LoadingRemote`
- `RehydratingFromTemp`

则当前读线程只等待，不重复发读。

### 7.4 spilled dirty hit

如果 resident 没命中，但 `spilled_dirty` 里有：

- `PendingSend / RetryPending`
  - 先从 temp rehydrate 回 resident
  - 再读
- `InFlight`
  - 先等这次发送结束
  - 成功就按普通 miss 读远端
  - 失败就从 temp rehydrate 再读

### 7.5 普通 miss

普通 miss 的流程：

1. 在 `blocks` 里先放一个 `LoadingRemote` 占位
2. 释放锁
3. 对 `block_base` 发一次 aligned read
4. 回来后填满整块
5. 变成 `ResidentClean` 或 `ResidentDirty`
6. 新块进入 FIFO

这里读 miss 一律拿整块，不拿局部。

## 8. 写策略

### 8.1 总规则

`write_locked(offset, data)` 的流程：

1. 先校验范围
2. 只读盘直接失败
3. 按 `32 KiB` 拆块
4. 每块独立 patch
5. patch 后只改 resident 数据和脏位

### 8.2 resident ready hit

如果块已 resident 且 `Ready`：

- 直接覆盖块内对应范围
- 对应范围进入 `dirty_ranges`
- 首次从 clean 变 dirty 时，可以唤醒 worker
- 正常更新 2Q 关系

### 8.3 resident loading hit

如果块还在：

- `LoadingRemote`
- `RehydratingFromTemp`

则当前写线程：

- 不重复发读
- 只把自己的 patch 挂进 `pending_patches`
- 等完整块回来后统一应用

### 8.4 miss 写入

写 miss 固定这样走：

```text
write miss
-> install LoadingRemote
-> attach current patch into pending_patches
-> remote aligned read full block
-> apply pending_patches in order
-> mark dirty
-> insert FIFO
```

核心原则只有一句话：

- 写 miss 先补整块，再 patch

### 8.5 spilled dirty 写入

如果目标块已经在 `spilled_dirty`：

- `PendingSend / RetryPending`
  - 先 rehydrate
  - 再 patch
- `InFlight`
  - 先等这次发送结束
  - 成功就按普通 miss 处理
  - 失败就从 temp rehydrate 再 patch

## 9. 周期扫描和 flush

### 9.1 worker 做什么

worker 只负责两类对象：

- `spilled_dirty`
- resident 上的 `dirty block / active snapshot`

它不负责：

- 前台读命中
- 2Q 提升/降级

### 9.2 触发源

建议保留两个触发源：

- 固定时间扫描
  - 例如 `DIRTY_SCAN_INTERVAL = 1s`
- resident block 首次变 dirty 时唤醒一次

### 9.3 处理顺序

建议每轮都这样处理：

1. 先处理 `spilled_dirty`
2. 再处理 resident block

理由很直接：

- `spilled_dirty` 更脆弱
- 先把它们送走，能先释放 temp slot

### 9.4 resident block flush

resident block 被扫描到时，按下面规则处理：

- 如果 active snapshot 是 `InFlight`
  - 跳过
- 如果 active snapshot 是 `RetryPending`
  - 直接重发这份旧 temp
- 如果没有 active snapshot，且当前没有脏位
  - 跳过
- 如果没有 active snapshot，但 temp 已满
  - 保留脏位，等待下轮
- 如果没有 active snapshot，且有脏位，且 temp 有空位
  - 先写 temp snapshot
  - temp 落盘成功后，只清这次覆盖到的脏位
  - 不改变 resident 内容
  - 不改变 FIFO/LRU 关系
  - 不因为这次扫描自动淘汰这块
  - 然后发网络写

网络写成功后：

- 删除 temp 文件
- 清掉 active snapshot
- 如果没有剩余脏位，块转 `ResidentClean`
- 如果后来又有新脏位，块保持 dirty，等下一轮再扫

网络写失败后：

- 保留 temp 文件
- active snapshot 进入 `RetryPending`
- resident 上后来产生的新脏位保持不变

### 9.5 spilled dirty flush

`spilled_dirty` 的规则更简单：

- 直接拿现有 temp 文件发
- 成功：
  - 删除 temp
  - 移除 `spilled_dirty`
- 失败：
  - 保留 temp
  - 状态改成 `RetryPending`

### 9.6 terminal error

如果 `session.write_at(...)` 返回：

- `SessionUnavailable`
- `Transport(...)`
- `Protocol(...)`

则沿用当前 `NetworkMedia` 逻辑：

- 触发 invalidation handler
- 当前网络盘进入 invalid 状态

此时 worker：

- 停止正常 flush
- 尽量保留还没删掉的 temp 到 `Drop`
- 等待中的阻塞请求统一失败退出

## 10. 淘汰策略

### 10.1 clean block 淘汰

最简单：

- 从 resident 表删掉
- 从 FIFO/LRU 删掉

### 10.2 dirty block 淘汰

dirty block 不能直接丢。

规则固定为：

- 如果这块还没有 active snapshot
  - 必须先生成 temp snapshot
- 如果这块已经有 active snapshot，且没有新的 resident 脏位
  - 可以直接复用这份 temp
- 如果这块已经有 active snapshot，但 resident 上又有新脏位
  - 这块当前不能淘汰，只能等
- 如果需要新 temp，但 temp 已满
  - 这块当前也不能淘汰，只能等

dirty block 真正淘汰成功之后：

- resident 删除
- temp 变成唯一权威副本
- 条目进入 `spilled_dirty`

### 10.3 为什么必须先落 temp

因为如果不先落 temp：

- 发送前进程崩溃会丢数据
- 发送失败没有可重试副本
- 后续读同块可能只能读到远端旧数据

## 11. temp 文件设计

### 11.1 目录

建议放在：

```text
<system temp>/yumedisk-network-media/<remote_disk_id>/<session_id>/
```

### 11.2 文件名

建议带两个字段：

- `block_index`
- `temp ticket`

例如：

```text
blk-0000000000000012-t000005.bin
```

### 11.3 文件内容

初版直接写块的有效字节内容，不做复杂封装。

元数据继续留在内存里：

- `block_index`
- `valid_len`
- `temp ticket`

### 11.4 启动清理

因为不做 crash recovery，所以：

- `bind()` 时可以直接清这个 session 目录下的旧 temp
- 清理失败只记日志，不要让挂载失败

## 12. 语义变化

### 12.1 写成功的含义变了

当前实现里：

- `write_locked()` 成功
  - 等于远端已经确认

引入缓存后：

- `write_locked()` 成功
  - 只等于本地 `NetworkMedia` 已接受这次写
  - 不等于远端已经确认

### 12.2 这会带来的后果

后果一：

- 写刚返回成功，但 session 随后失效
- 这批写可能还没真正到远端

后果二：

- 现在没有协议级 `flush / barrier / fua`
- 所以没有“强制把缓存全部推到远端再返回”的机制

后果三：

- 跨块写入的远端到达顺序不保证和本地完全一致
- 初版只保证单块内按“单 active snapshot 串行推进”

### 12.3 崩溃语义

当前 temp 的生成时机是：

- dirty eviction 时
- 周期扫描 flush 时

所以存在一个窗口：

- 写已经成功返回
- 但这块还只是 resident dirty
- 还没来得及生成 temp snapshot

如果这时进程崩溃：

- 最新写仍可能丢失

所以当前方案是：

- 尽快同步
- 但不是写返回后立刻 crash-safe

## 13. 生命周期

### 13.1 mount

`NetworkMedia::bind(...)` 建议补 3 件事：

- 校验 `max_io_bytes >= 32 KiB`
- 创建 temp 根目录
- 启动 flush worker

### 13.2 drop / eject

因为 `Media` trait 没有显式 `flush/close` 接口，所以 `NetworkMedia` 自己要在 `Drop` 里兜住：

- 标记 `stopping = true`
- 唤醒 worker 并等待退出
- 唤醒并结束所有阻塞请求
- 对剩余 dirty 做 best-effort flush
- 尽量删掉 temp

如果这时 session 已失效：

- 剩余 dirty 最终还是可能丢

这点文档里要明确接受。

## 14. 推荐实现顺序

建议按这个顺序落地：

1. 先把 `NetworkMedia` 改成 `Arc<Inner>`
2. 先把 resident cache + 2Q 跑通
3. 再补 read miss / write miss 的整块装载
4. 再补 `dirty_ranges + active_snapshot`
5. 再补 temp 文件和 dirty eviction
6. 再补 worker、周期扫描和阻塞
7. 最后补 `Drop` 和清理

## 15. 测试重点

只要把下面几类测清楚，这套设计就基本站住了：

- 基础 2Q
  - 新块进 FIFO
  - FIFO 再命中进 LRU
  - LRU 命中移尾
- 读写 miss
  - miss 会补整块
  - 同块并发 miss 不重复补块
- dirty / snapshot
  - temp 落盘成功后只清覆盖脏位
  - 同块任意时刻只允许一个 active snapshot
  - active snapshot 失败后能 retry
- spill
  - dirty eviction 前必须先有 temp
  - spilled dirty 可以 rehydrate
- 背压
  - temp 满时，需要新 temp 的请求会阻塞
  - `read hit` 不会被误阻塞
- 生命周期
  - `Drop` 不死锁
  - terminal error 后等待请求能退出

## 16. 结论

这份设计收口后，`NetworkMedia` 的模型其实很简单：

- resident 内存里是一张 `FIFO + LRU` 的 2Q 表
- 每个块按固定状态机流转
- dirty 数据离开内存前，必须先落 temp
- temp 和 resident 都有上限，打满后对需要新资源的请求做阻塞

其余层继续保持无感。
