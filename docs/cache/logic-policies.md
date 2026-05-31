# 逻辑策略

## temp 文件策略

temp 文件只服务于缓存内部，不对外暴露协议语义。

它只做两件事：

- resident dirty flush 时承载 snapshot
- dirty eviction 后承载 spilled dirty 的唯一副本

当前 `cache/` 独立组件里，temp 文件名固定按块索引生成：

- `block-<block_index>.temp`

这意味着：

- 同一块始终复用同一个 temp 文件
- temp slot 数量和“当前有 temp 支撑的块数量”是一一对应的

这版明确不做：

- WAL
- 崩溃恢复 replay
- 重启后的脏数据恢复

因此，启动时可以直接清理旧 temp；清理失败只记日志，不把挂载失败扩散出去。

## temp 上限策略

temp 上限按单盘文件数量控制，不按总字节数控制。

原因是：

- 每个 temp 本来就对应一个整块 snapshot
- 块大小本身由配置决定，不需要再靠 temp 总字节数做第二层对齐控制
- temp 初版不引入复杂文件头
- 按文件数限流，和“每个 temp 对应一个块 snapshot”更一致

建议始终把 temp 看成一种离散 slot 资源，而不是连续容量池。

## 背压和排队策略

这版不做全局冻结，只对“需要新资源才能继续”的请求施加背压。

固定规则如下：

- `read hit`
  - 直接放行
- `write hit`
  - 直接放行
- miss 需要新 resident，且 victim 是 clean
  - 直接淘汰并继续
- miss 需要 dirty eviction，且 temp 已满
  - 在远端拉块前阻塞

如果 cache 和 temp 都已经顶满，后续凡是还需要新 resident 或新 temp 的请求，都必须排队等待。

当前独立 `cache/` crate 的实现口径是：

- 只有真正卡在 `dirty victim + temp 满` 的 miss 会进入等待
- `read hit` / `write hit` 不会因为别的 miss 在排队而被连带阻塞
- temp slot 释放后，等待中的 miss 会被唤醒并重新判定状态
- 如果缓存正在停止，等待中的 miss 会直接退出

## 周期扫描优先级

周期扫描和前台 dirty eviction 冲突时，优先级固定为：

1. 先推进已经存在的 temp
2. 最后才轮到 resident dirty 的新一轮周期扫描

当前独立 `cache/` crate 的实现口径进一步收为：

- worker 只在周期 tick 时起一轮后台处理
- 一轮处理里，先 flush 已经存在的 temp
- 只有当前没有可发送 temp 时，才会为 resident dirty 新建 snapshot

这条规则的目的，是避免周期扫描长期抢占 temp slot，导致前台 miss 无法推进。

进一步说：

- 如果后续已经有前台 miss 在等待 temp slot
  - 周期扫描不再为 resident dirty 新建 snapshot
- 但 worker 仍然应该继续发送已经存在的 temp、active snapshot 和 retry 项

## temp 占用串行策略

同一个 temp 文件在同一时刻只允许一个使用者。

当前实现里，temp 可能被两类路径占用：

- worker flush 已有 temp
- 前台请求从 spilled dirty rehydrate

只要某个块的 temp 正在被其中一条路径使用：

- 另一条路径不能并发读取或发送同一个 temp
- 相关请求先等待当前 temp 操作完成，再重新判定状态

## active snapshot 策略

同一块同一时刻只允许一个 active flush snapshot。

这版不引入：

- 多 active snapshot
- 块版本号

原因很直接：

- 当前项目模型已经保证写端唯一
- 单块单 snapshot 足够表达当前需要的并发关系
- 再引入版本号和多快照，只会显著抬高管理复杂度

这条规则也意味着：

- snapshot 发送期间，这块后来产生的新写只保留在 resident
- 只有等下一轮 flush，才会生成下一份 snapshot

## dirty bit 清理策略

dirty bit 不能在“准备 flush”时就清掉。

固定规则是：

- 先把 snapshot 可靠写入 temp
- temp 落盘成功后，才清这次 snapshot 覆盖到的脏位

如果 temp 已满或者 temp 落盘失败：

- 脏位保持不变
- 这块继续视为 resident dirty

## 失效和生命周期策略

如果 `DiskSession` 出现 terminal error，缓存层沿用 `NetworkMedia` 的 invalidation 逻辑：

- 触发 invalidation handler
- 停止正常 flush
- 唤醒并结束等待中的阻塞请求
- 剩余 temp 留到 `Drop` 阶段 best-effort 清理

`Drop / eject` 时，这层需要自己兜住退出过程，因为当前 `Media` trait 没有单独的 `flush/close` 边界。

## 一致性语义

当前模型依赖一个前提：

- 写端唯一

在这个前提下，可以接受下面两条语义：

- `write_locked()` 成功，只表示本地缓存已接受写入
- 只要块还停留在 resident dirty、尚未形成 temp snapshot，这批写就不是 crash-safe

所以这版缓存的目标是“尽快同步”，不是“写返回即远端持久化”。
