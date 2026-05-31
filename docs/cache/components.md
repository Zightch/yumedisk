# 主要组件

## 总体结构

当前缓存层不是独立宿主，而是 `NetworkMedia` 内部的一个子系统：

```text
BackendRust
   <-> NetworkMedia
        ├─ mux
        ├─ cache
        └─ DiskSession
```

这层的重点不是再造一套新的外部接口，而是在 `Media` 边界内把“字节语义”和“块语义”分开：

- `BackendRust` 继续按字节区间读写
- `DiskSession` 继续按远端块 I/O 工作
- `cache` 负责把两边接起来

## `NetworkMedia`

`NetworkMedia` 仍然是对外的 `Media` 实现。

它的职责是：

- 持有当前网络盘的 session 和缓存状态
- 暴露 `read_locked()` 和 `write_locked()`
- 在 `ro` / `rw` 两种模式下走不同内部路径
- 统一处理 invalidation、停止和清理

它不再只是一个单纯透传层，但也不会把缓存状态扩散到 `BackendRust` 或 `network-core`。

## `mux`

`mux` 是一个很薄的复用器。

它只负责：

- 把请求分到 `ro` 直通路径或 `rw` 缓存路径
- 复用底层 `DiskSession`
- 在停止、失效、等待等场景下统一串联状态

它明确不负责：

- 按配置块大小的对齐
- 请求拼接和裁剪
- dirty 状态管理
- temp 文件管理

这些事情都属于 `cache`。

## `cache`

`cache` 是整个设计的核心。

它左侧面对 `BackendRust`，接受任意 offset 和任意长度；右侧面对 `DiskSession`，只产出按当前配置块大小对齐的固定块。

它的职责包括：

- 任意字节请求到块请求的转换
- resident block 管理
- `2Q` 队列维护
- dirty 标记和 snapshot 管理
- temp 文件申请、复用和删除
- flush worker 协同
- miss 阻塞和唤醒

这意味着缓存复杂性被收敛在一个地方，不会把块级规则散到其他层。

## resident 表和 `2Q`

resident 是当前仍在内存中的块集合。

resident 上面只保留两条队列：

- `FIFO`
  - 新块第一次进入缓存时放这里
- `LRU`
  - FIFO 再命中后提升到这里

这个模型的目标很直接：

- 用 FIFO 吸收一次性块
- 用 LRU 保留重复命中的热点块

## dirty / spilled / temp

dirty 块分两类：

- resident dirty
  - 块还在内存里，数据和脏位都还在 resident
- spilled dirty
  - 块已经被淘汰出 resident，temp 文件成了唯一权威副本

temp 文件只承担两件事：

- resident dirty flush 时的 snapshot 载体
- dirty eviction 后的临时权威副本

它不是：

- WAL
- 崩溃恢复日志
- 长期持久化层

## flush worker

worker 只做后台脏数据推进，不做前台命中路径。

它主要处理：

- resident dirty 的周期扫描和 snapshot 发送
- `spilled_dirty` 的重试发送
- temp 释放后的等待唤醒

它不负责：

- 2Q 提升/降级
- 普通读命中
- 普通写命中
- 主动淘汰 clean 块
