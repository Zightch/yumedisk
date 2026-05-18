# 认证与路由

## 1. 文档定位

本文档描述：

- 领盘码模型
- `gateway` 如何持有认证材料与 route
- challenge 与 `auth_id` 的边界
- `auth_id` 如何进入 `SessionOpen`

当前正式口径固定为：

- `gateway` 是认证和路由的唯一决策点
- `auth_id` 是显式短生命周期授权对象
- `auth_id` 只存在于 `client-gateway`
- route 来自外部 `storer -> gateway` 注册或 `whole` 的本地 fixed route

## 2. 领盘码模型

### 2.1 格式

- 总长度不少于 `80`
- 前 `16` 个字符为 `disk_id`
- 剩余 `64+` 个字符为领取密钥
- 字符集：`0-9a-zA-Z`

### 2.2 本地计算

当前 `algo_version = 1`：

```text
auth_verifier = SHA512(claim_code_bytes)
proof = HMAC-SHA512(key = auth_verifier, msg = salt_bytes)
```

其中：

- `auth_verifier` 固定 `64` 字节
- `proof` 固定 `64` 字节
- 线上传输的是原始字节，不是 hex 字符串

## 3. route 真源

当前 route 来源固定为两种：

1. `role = storer`：`storer -> gateway` 注册
2. `role = whole`：内嵌 gateway 为自己的 `disk_id` 写入本地 fixed route

注册后，gateway 在 `route_registry` 至少保存：

- `disk_id`
- `route_connection_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `route_state`

当前正式约束：

- gateway 不保存原始 `claim_code`
- 一个 route 只绑定一个 `disk_id`
- 一个 `disk_id` 同时只允许一个活跃 route
- `role = whole` 的 `route_registry` 中只保留自己的 `disk_id`

## 4. challenge 模型

### 4.1 AuthStart

client 发送：

```text
AuthStartRequest {
  disk_id
}
```

gateway 返回：

```text
AuthStartResponse {
  challenge_token
  salt_bytes
  ttl_seconds
  algo_version
}
```

### 4.2 challenge_token 约束

challenge token 必须满足：

- 每次 `AuthStart` 生成新的 token
- 同一 `disk_id` 的不同 connection 拿到的 token 必须不同
- token 绑定当前 connection
- token 自带过期信息
- token 对 client 不透明

建议内容：

```text
Seal({
  disk_id,
  salt,
  issued_at,
  expire_at,
  connection_nonce,
})
```

并发约束：

- 同一条 connection 上同时最多只允许一个 auth 过程
- auth 过程结束前，不允许同 connection 再开启新的 auth 过程
- auth 过程结束前，也不允许同 connection 并发进入 `SessionOpen`

## 5. AuthFinish 与 auth_id

### 5.1 AuthFinish

client 本地计算：

```text
auth_verifier = SHA512(claim_code_bytes)
proof = HMAC-SHA512(key = auth_verifier, msg = salt_bytes)
```

然后发送：

```text
AuthFinishRequest {
  challenge_token
  proof
}
```

### 5.2 gateway 成功路径

gateway 依次执行：

1. 校验 challenge token 完整性
2. 校验 challenge 是否过期
3. 校验 challenge 是否属于当前 connection
4. 根据 token 中的 `disk_id` 查 `route_registry`
5. 使用 route 中的 `auth_verifier` 校验 `proof`
6. 成功后分配新的 `auth_id`
7. 写入 `auth_grant_registry`

返回：

```text
AuthFinishResponse {
  auth_id
}
```

### 5.3 auth_id 的定义

`auth_id` 是一个 connection-scoped、单盘绑定、可消费一次的授权对象。

它至少绑定：

- `auth_id`
- `client_connection_id`
- `disk_id`
- `issued_at`
- `expire_at`
- `grant_state`

### 5.4 auth_id 的意义

`auth_id` 只表示：

- 当前 connection 获得了对某个 `disk_id` 申请打开 session 的资格

它不表示：

- connection 进入长期 authorized 状态
- 会话已经打开
- session 已分配
- metadata 已经返回

当前正式口径里，auth 过程结束后只留下 `auth_id`，不再在 connection 上保留“已认证磁盘”这种长期业务状态。

## 6. auth_id 生命周期

`auth_id` 状态固定为：

```text
granted
  -> consumed
  -> expired
  -> revoked
```

### 6.1 granted

`AuthFinish` 成功后进入 `granted`。

### 6.2 consumed

`SessionOpen(auth_id)` 成功后，该 `auth_id` 被消费。

### 6.3 expired

认证成功后长时间未消费，应自动过期。

### 6.4 revoked

以下情况应直接撤销：

- client connection 死亡
- route 消失
- gateway 主动清理

### 6.5 当前额外约束

- 一个 connection 同时最多只保留一个未消费 `auth_id`
- 如果 client 要在同一 connection 上继续打开第二个 session，必须等前一个 `auth_id` 被消费后重新走一次 auth

## 7. 认证失败口径

以下失败统一视为认证失败，不关闭 connection：

- 假盘
- 真盘但领盘码错误
- challenge 过期
- challenge 非法
- challenge 不属于当前 connection
- proof 格式非法

失败路径建议：

1. 返回统一失败语义
2. 随机延迟 `2..5s`
3. 清理本次 challenge 上下文

## 8. SessionOpen 与 route

### 8.1 输入

会话建立当前正式输入固定为：

```text
SessionOpen(auth_id)
```

### 8.2 gateway 行为

gateway 执行：

1. 校验 `auth_id` 是否存在
2. 校验 `auth_id` 是否属于当前 connection
3. 校验 `auth_id` 是否过期
4. 从 `auth_id` 还原 `disk_id`
5. 用 `disk_id` 查找活跃 route
6. 向目标 storer 发起内部 `SessionOpen`
7. 成功时分配 `gateway_session_id`
8. 把 route metadata 复制成 session 快照
9. 写入 `session_registry`
10. 仅向 client 返回 `gateway_session_id`

### 8.3 storer 侧输入

由于当前一个 route 只对应一个 `disk_id`，storer 侧 `SessionOpen` 不再需要：

- `disk_id`
- `auth_id`

storer 只需要知道：

- gateway 要在当前 route 上打开一个真实 session

### 8.4 busy 失败

如果 `SessionOpen` 因 `busy` 失败：

- connection 保持存活
- `auth_id` 仍然有效
- client 可以在未过期前重试

这里的 `busy` 定义固定为：

- 目标盘当前已有活跃 session，storer 拒绝新的 open

最终决策点在 storer，本语义不在 gateway 重新解释。

### 8.5 SessionDescribe

`SessionDescribe(session_id)` 的正式口径固定为：

- 只在 `SessionOpen` 成功之后使用
- 由 gateway 从 `session_registry` 中读取 session 快照返回
- 不影响 `auth_id`
- 不转发到 storer

## 9. route 失效后的传播

当 route connection 断线或 route heartbeat 超时后，gateway 必须：

1. 撤销该 route
2. 撤销该 route 名下全部未消费 `auth_id`
3. 关闭该 route 名下全部 session
4. 向仍在线 client 发送 `SessionCloseNotice`

`role = whole` 下若本地 fixed route 失效，gateway 也必须走完全相同的 grant/session 清理路径。

这样可避免：

- 悬空授权对象
- 悬空 session
- client 迟迟不知道该盘已经失效

这里的 client-facing 边界同时固定为：

- 网络层只上报“session 已失效”
- 网络层只调用 `NetworkMedia` 失效处理接口
- 是否立即删除盘对象，还是保留为严格假死挂起态，由 client / 宿主策略决定

## 10. 当前正式约束

1. `gateway` 是认证与路由的唯一决策点。
2. `auth_id` 是显式授权对象，不是隐式 connection 状态。
3. `auth_id` 只存在于 `client-gateway`。
4. route 来自外部 `storer` 注册或 `whole` 的本地 fixed route。
5. storer 不知道 client 的 `auth_id`。
6. `SessionOpen(auth_id)` 成功后只得到 `session_id`。
7. metadata 通过 `SessionDescribe(session_id)` 单独查询。
8. route 失效后 gateway 必须主动撤销对应 `auth_id` 与 session，但不替宿主决定 `NetworkMedia` 对象清理策略。
