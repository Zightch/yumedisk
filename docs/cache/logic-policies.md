# 逻辑策略

## temp 文件策略

temp 文件只服务于缓存内部，不对外暴露协议语义。

它只做两件事：

- resident dirty flush 时承载 snapshot
- dirty eviction 后承载 spilled dirty 的唯一副本

这版明确不做：

- WAL
- 崩溃恢复 replay
- 重启后的脏数据恢复

因此，启动时可以直接清理旧 temp；清理失败只记日志，不把挂载失败扩散出去。

## temp 上限策略

temp 上限按单盘文件数量控制，不按总字节数控制。

原因是：

- 块大小本来就是固定 `32 KiB`
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

## 周期扫描优先级

周期扫描和前台 dirty eviction 冲突时，优先级固定为：

1. 先推进已经存在的 temp
2. temp slot 一旦释放，先让等待 dirty eviction 的前台 miss 继续
3. 最后才轮到 resident dirty 的新一轮周期扫描

这条规则的目的，是避免周期扫描长期抢占 temp slot，导致前台 miss 无法推进。

进一步说：

- 如果已经有前台 miss 在等待 temp slot
  - 周期扫描不再为 resident dirty 新建 snapshot
- 但 worker 仍然应该继续发送已经存在的 temp、active snapshot 和 retry 项

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

