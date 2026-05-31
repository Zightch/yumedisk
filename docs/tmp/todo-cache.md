# `NetworkMedia` 接入 `cache` 执行清单

## 0. 当前范围

本清单只服务于“把已经完成的 `cache/` 独立组件接回 `NetworkMedia`”这件事。

这里固定以 [docs/cache/network-media-cache-structure.svg](../cache/network-media-cache-structure.svg) 为正式模型，口径收成：

- `rw` 走 `cache`
- `ro` 继续由 `mux` 旁路直通 `DiskSession`
- `mux` 只做路径复用，不做对齐、拼接、裁剪
- `DiskSession` 继续只负责远端 I/O

本轮不再讨论：

- `cache` 组件内部实现
- resident / temp / flush worker 算法细节
- 网络协议改造
- `BackendRust` 公开接口改造
- `DiskSession` 协议层语义改造

`cache` 组件本体已经独立完成，本轮只做最薄接线。

## 1. 当前起点

### 1.1 `cache` 组件现状

当前根目录 `cache/` crate 已经具备完整最小闭环：

- 左侧 `read_locked(offset, buffer)`
- 左侧 `write_locked(offset, data)`
- 右侧 `AtIo::read_at(offset, buffer)`
- 右侧 `AtIo::write_at(offset, data)`
- resident `2Q = FIFO + LRU`
- dirty -> temp -> flush -> spilled -> rehydrate
- temp 限流、前台 miss 背压、后台 flush worker

因此本轮不应该再把缓存细节散回 `NetworkMedia`。

### 1.2 `NetworkMedia` 当前现状

当前 `rust-cli` 和 `tauri-client` 里的 `NetworkMedia` 仍然是：

- 直接持有 `DiskSession`
- 直接持有 `disk_size_bytes`
- 直接持有 `read_only`
- 直接持有 `max_io_bytes`
- `read_locked()` / `write_locked()` 里自己按 `max_io_bytes` 拆片
- 出现 terminal error 时由自己触发 invalidation handler

也就是说，当前主线还是：

```text
BackendRust <-> NetworkMedia <-> DiskSession
```

还没有把 `cache` 接进去。

## 2. 当前总目标

按最薄接线完成下面这条主线：

1. `NetworkMedia` 内部显式容纳 `mux + cache + session`
2. `mux` 只负责 `ro` 直通 / `rw` 缓存分流
3. `cache` 左侧继续承接 `BackendRust::Media`
4. `cache` 右侧只依赖一个 `DiskSession` 适配层
5. `DiskSession` 继续完全不知道 resident、temp、flush worker
6. `BackendRust` 继续不感知 `cache` 的内部状态机

完成后正式结构应保持为：

```text
BackendRust <-> NetworkMedia <-> DiskSession
                    |
                    +-> mux
                    +-> cache
```

## 3. 固定边界

### 3.1 `mux` 的边界

`mux` 只负责下面两件事：

- 判定当前盘走 `ro` 直通路径还是 `rw` 缓存路径
- 复用同一个 `DiskSession` 及其宿主生命周期

`mux` 明确不负责：

- 按块大小对齐
- 任意字节请求拆块
- `max_io_bytes` 拆片
- dirty 状态管理
- temp 文件管理
- flush worker
- invalidation 语义发明

### 3.2 `cache` 的边界

`cache` 接入后继续只做它自己的事情：

- 左侧接受字节语义
- 右侧产出按配置块大小对齐、固定长度的 `_at` I/O
- 管 resident / temp / snapshot / spilled / worker

`cache` 明确不做：

- 不直接依赖 `BackendRust`
- 不直接依赖 `DiskSession`
- 不知道 `disk_id / remote_disk_id / session_id`
- 不知道 `NetworkClientError`
- 不持有 invalidation handler

### 3.3 `DiskSession` 的边界

`DiskSession` 接入后仍然只负责：

- `ReadAt`
- `WriteAt`
- `Close`
- `SessionUnavailable` 等会话错误上报

它不应该因为接入 `cache` 而新增：

- resident 状态
- temp 文件语义
- flush 控制
- cache 命中行为

### 3.4 `BackendRust` 的边界

`BackendRust` 继续只看到：

- `Media::size_bytes()`
- `Media::read_locked()`
- `Media::write_locked()`

本轮不把 `CacheError`、`CacheSnapshot`、`AtIo` 这些类型直接扩散到 `BackendRust` 宿主边界。

## 4. 推荐接入模型

### 4.1 `NetworkMedia` 内部最小模型

推荐把当前直接持有的“直通读写逻辑”收成一个很薄的内部 `mux`，最小模型可以是：

- `BypassRo`
  - `ro` 直通
  - 保留当前按 `max_io_bytes` 拆片直连 `DiskSession` 的逻辑
- `CachedRw`
  - `rw` 路径
  - `BackendRust` 调 `NetworkMedia::read_locked/write_locked`
  - `NetworkMedia` 只把调用转给 `cache`

关键点只有一条：

- `rw` 路径不再让 `NetworkMedia` 自己做块对齐和缓存细节

### 4.2 `DiskSessionAtIo` 适配层

`cache` 右侧不应直接吃 `DiskSession`，而应补一层很薄的 `AtIo` 适配器。

这层适配器至少要负责：

- 把 `DiskSession` 包成 `AtIo`
- 用 `disk_size_bytes` 做真实越界校验
- 在需要时按 `max_io_bytes` 继续拆片
- 把 `NetworkClientError` 映射成 `CacheError`
- 遇到 terminal error 时，把失效上抛回 `NetworkMedia` 的 invalidation handler

固定口径：

- `cache` 左侧看不到 `NetworkClientError`
- `DiskSession` 看不到 `CacheConfig`
- invalidation 仍由 `NetworkMedia` 统一收束

### 4.3 `temp_dir` 和 `CacheConfig`

`cache` 仍然只吃上层准备好的 `CacheConfig`。

因此 `NetworkMedia` 接线时必须补齐：

- `block_size_bytes`
- `fifo_capacity_blocks`
- `lru_capacity_blocks`
- `dirty_scan_interval`
- `temp_max_files`
- `temp_dir`

这里固定一条边界：

- `cache` 不决定 temp 根目录结构
- temp 目录仍由更上层 runtime / 持久化配置 / 本地实例目录准备

### 4.4 `ro` 路径保持直通

`ro` 路径不创建 `cache`，不创建 temp，不启动 worker。

这样收的好处是：

- 保持 `ro` 简单
- 不引入无意义的 temp 生命周期
- 不把 `write_locked()` 的本地接受语义混到只读盘里

### 4.5 `rw` 路径接 `cache`

`rw` 路径进入 `cache` 后，`NetworkMedia` 只保留这些宿主职责：

- 保持 `disk_size_bytes`
- 保持 `read_only` 边界
- 保持 invalidation handler
- 保持 `DiskSession` 生命周期
- 决定当前实例是否创建 `cache`

`cache` 则负责：

- 字节请求拆块
- resident 命中
- temp / spill / rehydrate
- 右侧块对齐 I/O

## 5. 需要完成的工作

## 5.1 第一阶段：把当前直通逻辑和接入目标收口

目标：

- 先把 `NetworkMedia` 当前“直通 + 拆片”逻辑和未来“缓存 + 直通双路径”边界收清楚

任务：

- [ ] 盘点 `rust-cli` 版 `NetworkMedia`
- [ ] 盘点 `tauri-client` 版 `NetworkMedia`
- [ ] 固定 `rw cache / ro bypass` 口径
- [ ] 固定 `NetworkMedia` 仍是 `BackendRust::Media` 的唯一宿主对象

### 5.2 第二阶段：补一个很薄的 `mux`

目标：

- 只做路径分流，不做第二层缓存实现

任务：

- [ ] 在 `NetworkMedia` 内定义最薄路径模型
- [ ] 收出 `ro` 旁路逻辑
- [ ] 收出 `rw` 缓存逻辑入口
- [ ] 明确 `mux` 不持有额外缓存状态

固定实现要求：

- `mux` 不是新宿主，不是新 runtime
- `mux` 不做块计算
- `mux` 不做 `max_io_bytes` 之外的 I/O 政策发明

### 5.3 第三阶段：补 `DiskSession -> AtIo` 适配层

目标：

- 让 `cache` 的右侧只看到 `AtIo`

任务：

- [ ] 设计最小 `DiskSessionAtIo`
- [ ] 保留 `disk_size_bytes` 真实范围校验
- [ ] 保留 `max_io_bytes` 拆片能力
- [ ] 补 `NetworkClientError -> CacheError` 映射
- [ ] 补 terminal error -> invalidation 上抛

固定实现要求：

- `DiskSessionAtIo` 只做 I/O 适配，不做 resident 逻辑
- `DiskSessionAtIo` 不复制 `cache` 内部状态
- `DiskSessionAtIo` 不把 `DiskSession` 变成新的缓存管理器

### 5.4 第四阶段：把 `cache` 接入 `rw` 路径

目标：

- 在不改 `BackendRust::Media` 对外接口的前提下，把 `rw` 读写接进 `cache`

任务：

- [ ] `read_locked()` 在 `rw` 路径转给 `cache.read_locked()`
- [ ] `write_locked()` 在 `rw` 路径转给 `cache.write_locked()`
- [ ] 保持 `size_bytes()` 仍由 `NetworkMedia` 暴露
- [ ] 保持 `read_only` 拒写闸口留在宿主侧

这里要特别固定一条语义：

- 接入 `cache` 后，`rw` 的 `write_locked()` 成功，只表示本地缓存接受成功
- 不再等价于“远端已经确认写成功”

### 5.5 第五阶段：保留 `ro` 路径直通

目标：

- `ro` 继续沿用当前简单路径，不把 `cache` 拉进来

任务：

- [ ] `ro` 实例不创建 `cache`
- [ ] `ro` 实例保留当前 `read_locked()` 直通拆片逻辑
- [ ] `ro` 实例继续拒绝 `write_locked()`
- [ ] `ro` 不创建 temp、不启动 worker

### 5.6 第六阶段：生命周期、失效和清理

目标：

- 把 `cache` 接入后最容易出问题的宿主清理链补齐

任务：

- [ ] 固定 `NetworkMedia` 仍是 invalidation handler 的唯一宿主
- [ ] 固定 `cache` Drop 只负责 stop，不负责强制 flush 成功
- [ ] 固定 session close / eject / invalidation 时的 cache 停止顺序
- [ ] 固定等待中的请求在 stop 时返回 `Stopped`
- [ ] 固定 temp 生命周期仍由 `cache` 内部管理

固定实现要求：

- 不把 invalidation 逻辑下沉到 `cache`
- 不把 session cleanup 逻辑下沉到 `DiskSessionAtIo`

### 5.7 第七阶段：配置和目录准备

目标：

- 把 `cache` 所需的实例级配置补齐

任务：

- [ ] 为 `NetworkMedia` 准备 `CacheConfig`
- [ ] 为每个缓存实例准备 `temp_dir`
- [ ] 固定 `block_size_bytes` 与 `max_io_bytes` 的配合约束
- [ ] 固定 temp 根目录由上层准备，不由 `cache` 创建目录结构

### 5.8 第八阶段：测试和验收

目标：

- 接线后不回退当前两类 client 的既有语义

任务：

- [ ] `ro` 仍然保持直通语义
- [ ] `rw` 读 miss 进入 `cache`
- [ ] `rw` 写命中/写 miss 不直接旁路到 `DiskSession`
- [ ] terminal session error 仍能触发 invalidation
- [ ] temp 压力下 `rw` miss 的阻塞语义仍成立
- [ ] eject / remove / invalidation 不留悬挂 worker

## 6. 当前最容易踩坑的点

### 6.1 不要让 `mux` 变厚

一旦把下面这些事情塞给 `mux`，结构就会反向膨胀：

- 块对齐
- request 拼接
- dirty 跟踪
- temp 路径管理

这些都应该继续留在 `cache`。

### 6.2 不要在 `NetworkMedia` 和 `cache` 各存一份写语义

当前直通 `NetworkMedia` 的写语义更接近“发出并收到远端响应”。

接入 `cache` 后，`rw` 写语义会收窄为“本地缓存已接受”。

这条差异必须在接线文档和调用方预期里明确，不然会出现错误的 durability 心智模型。

### 6.3 不要让 `cache` 直接知道 `DiskSession`

如果把 `DiskSession` 直接塞进 `cache`，很容易带来后续耦合：

- `NetworkClientError`
- invalidation
- session lifecycle
- `max_io_bytes`

这些都应该先被适配层吃掉。

### 6.4 不要忘了 `block_size_bytes` 和 `max_io_bytes` 的关系

如果 `block_size_bytes > max_io_bytes`，则：

- 不能指望当前直通逻辑自然可复用
- 必须在 `DiskSessionAtIo` 里继续做拆片

### 6.5 不要把 `ro` 也强行接入 `cache`

按当前正式模型：

- `ro` 继续由 `mux` 旁路
- `rw` 才进入 `cache`

先把这条边界守住，复杂度最低。

## 7. 验收口径

本轮完成后，至少要满足下面这些结果：

1. `NetworkMedia` 结构正式变成 `mux + cache + session`
2. `mux` 只做 `ro` 直通 / `rw` 缓存分流
3. `BackendRust` 仍只看到 `Media`
4. `cache` 左侧仍是字节语义，右侧仍是整块 `_at` I/O
5. `DiskSession` 仍然只是远端 I/O 通道
6. `ro` 不创建 cache、不创建 temp、不启动 worker
7. `rw` 正式通过 `cache` 承接读写
8. terminal error 的 invalidation 语义不回退
9. 不把 `cache` 业务语义扩散到 `network-core`

## 8. 推荐推进顺序

建议严格按下面顺序推进：

1. 先收 `NetworkMedia` 内部最薄 `mux`
2. 再补 `DiskSessionAtIo`
3. 再把 `rw` 接进 `cache`
4. 再把 `ro` 直通路径收稳
5. 最后补清理、配置和验收测试

这样推进，和 `docs/cache/network-media-cache-structure.svg` 的正式模型最一致，也最不容易把 `cache` 的内部复杂性重新散回上层。
