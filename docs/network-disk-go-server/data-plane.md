# 数据面最小闭环

## 1. 当前范围

当前第一版只保留最小块设备能力：

- `AuthStart`
- `AuthFinish`
- `SessionOpen`
- `ReadAt`
- `WriteAt`
- `Close`
- `Ping`

其中：

- `AuthStart` / `AuthFinish` 属于认证阶段
- `SessionOpen` 是认证后打开盘会话的唯一入口
- `ReadAt / WriteAt / Close / Ping` 属于数据面最小命令

第一版不额外定义 `AuthGrant` 给 client 中转。

## 2. 协议位置

所有业务消息都包装在传输层 payload 中。

传输层只负责拆帧；业务层负责：

- 命令语义
- 请求和响应配对
- `session_id` 路由
- 偏移和长度约束
- 错误码语义

## 3. 第一版请求头职责

第一版业务层固定使用：

```text
header {
  op_code
  request_id
  session_id
}
```

约束：

- `request_id` 用于并发请求配对
- 响应允许乱序返回
- `session_id` 由 `SessionOpen` 成功后获得
- 认证前命令不带 `session_id`

## 4. 最小命令语义

### AuthStart

用途：

- 提交 `disk_id`
- 换取 challenge

### AuthFinish

用途：

- 提交基于领盘码和 salt 计算出的 `proof`
- 让 `gateway` 完成认证

### SessionOpen

用途：

- 在当前 `gateway` 连接上打开一个远端盘会话

成功返回最少这些字段：

- `session_id`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `session_ttl_seconds`

这几个字段是 `NetworkMedia` 构造所需最小元数据。

### ReadAt

用途：

- 从 `session_id` 对应的远端盘读取指定范围数据

最小语义字段：

- `offset`
- `length`

响应：

- 成功时返回完整数据段
- 失败时返回统一错误

### WriteAt

用途：

- 向 `session_id` 对应的远端盘写入指定范围数据

最小语义字段：

- `offset`
- `length`
- `payload`

响应：

- 成功时只表示本次写入已被远端接受
- 失败时返回统一错误

### Ping

用途：

- 会话保活
- 探测连接和 session 是否仍有效

### Close

用途：

- 主动关闭 `session_id`

约束：

- `Close` 只关闭目标 `DiskSession`
- 不直接关闭整条 `GatewayConnection`

## 5. 大块 I/O 约束

如果一次盘操作编码后超过单帧上限，必须由业务层主动拆分成多个完整请求。

规则：

1. 每个拆分请求都带独立 `request_id`
2. 每个拆分请求都带完整 `session_id + offset + length`
3. 对端只按单帧消息处理，不承担跨帧重组责任

因此：

- `max_io_bytes` 必须小于传输层 payload 上限
- `NetworkMedia` 发起 `read/write` 前必须先按 `max_io_bytes` 切片

## 6. Gateway 转发语义

第一版 `gateway` 同时承担数据面转发。

转发语义：

1. client 发送带 `session_id` 的业务请求到 `gateway`
2. `gateway` 根据本地 `session_id -> storer session` 映射定位目标
3. `gateway` 把请求转发给目标 `storer`
4. `storer` 返回结果
5. `gateway` 带回原始 `request_id` 回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改变 `ReadAt / WriteAt` 的成功失败语义
- `gateway` 只负责会话查找、请求转发、响应回传

## 7. 错误语义

第一版必须统一的错误类型至少包括：

- 未认证
- session 不存在
- session 过期
- 超读写越界
- 只读盘写入
- 远端盘 I/O 失败
- 请求格式非法

边界原则：

- 协议格式错误由边界入口直接拒绝
- `NetworkMedia` 只处理已经成型的业务错误
- `Media` 层不重复做网络协议级判断

## 8. NetworkMedia 第一版接入方式

`NetworkMedia` 第一版采用最小、直接的阻塞实现：

- 挂载前完成 `AuthStart -> AuthFinish -> SessionOpen`
- 构造时保存：
  - `GatewayConnection`
  - `session_id`
  - `disk_size_bytes`
  - `read_only`
  - `max_io_bytes`
- `read_locked()` 串行发送一个或多个 `ReadAt`
- `write_locked()` 串行发送一个或多个 `WriteAt`
- 不做本地写缓存
- 不做断线重连
- 不做自动重试

与当前 `BackendRust::Media` 口径对齐：

- `read_locked()` 返回 `Ok(())` 时必须已经拿到完整数据
- `write_locked()` 返回 `Ok(())` 时表示远端已经接受并承担该次写入的一致性责任

## 9. 连接失效口径

第一版收成最硬的单一路径：

- 一条 `GatewayConnection` 断开时，其下全部 `DiskSession` 一起失效
- 失效后的 `NetworkMedia` 读写直接失败
- 不自动补连
- 不自动重建会话

恢复策略属于上层宿主动作，不属于 `NetworkMedia` 自动行为。

## 10. 第一版最小验收

第一版只验收以下闭环：

1. client 使用 `claim_code` 成功完成认证
2. `SessionOpen` 成功返回会话元数据
3. 同一条 `GatewayConnection` 上可并发持有多个 `DiskSession`
4. `NetworkMedia` 可完成真实 `read/write`
5. 连接断开后，相关 `DiskSession` 全部失效
6. `tauri-client` 可完成网络盘的添加、挂载、拔出、重挂载
