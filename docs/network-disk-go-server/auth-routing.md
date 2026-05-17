# 认证与路由

## 1. 文档定位

本文档描述：

- 领盘码模型
- `gateway` 如何缓存认证材料与 route
- challenge 与 `auth_id` 的边界
- `auth_id` 如何进入 `SessionOpen`

当前正式口径不再采用“认证成功只隐式挂在 connection 上”的模型，而是显式返回 `auth_id`。

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

## 3. route 缓存来源

当前采用：

```text
storer -> gateway 注册
```

注册后，`gateway` 至少缓存：

- `disk_id`
- `auth_verifier`
- `route_connection`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

`gateway` 不保存原始 `claim_code`。

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
  conn_nonce,
})
```

并发约束：

- 同一条 connection 上同时最多只允许一个 challenge 驱动的 auth 过程
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
4. 根据 token 中的 `disk_id` 查 route 缓存
5. 使用 route 缓存中的 `auth_verifier` 校验 `proof`
6. 成功后分配新的 `auth_id`

返回：

```text
AuthFinishResponse {
  auth_id
}
```

### 5.3 auth_id 的定义

`auth_id` 是一个 connection-scoped 的显式授权对象。

它至少绑定：

- `auth_id`
- `connection_id`
- `disk_id`
- `issued_at`
- `expire_at`
- `state`

### 5.4 auth_id 的意义

`auth_id` 只表示：

- 当前 connection 拥有对某个 `disk_id` 申请打开 session 的资格

它不表示：

- 会话已打开
- session 已分配
- metadata 已下发

## 6. auth_id 生命周期

`auth_id` 状态建议固定为：

```text
granted
  -> consumed
  -> expired
  -> revoked
```

### 6.1 consumed

`SessionOpen(auth_id)` 成功后，该 `auth_id` 被消费。

### 6.2 expired

认证成功后长时间未消费，应自动过期。

### 6.3 revoked

以下情况应直接撤销：

- connection 死亡
- route 消失
- gateway 主动清理

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

会话建立不再直接以 `disk_id` 为输入，而以：

```text
SessionOpen(auth_id)
```

### 8.2 gateway 行为

gateway 执行：

1. 校验 `auth_id` 是否存在
2. 校验 `auth_id` 是否属于当前 connection
3. 校验 `auth_id` 是否过期
4. 从 `auth_id` 还原 `disk_id`
5. 用 `disk_id` 查找 route
6. 向目标 storer 发起 `SessionOpen`
7. 成功时分配 `gateway_session_id`

并发约束：

- 同一条 connection 上同时最多只允许一个 `SessionOpen` 过程
- `SessionOpen` 过程未结束前，不允许该 connection 并发开启新的 auth 过程

### 8.3 busy 失败

如果 `SessionOpen` 因 busy 失败：

- connection 保持存活
- `auth_id` 仍然有效
- client 可以在未过期前重试

## 9. route 唯一性

当前正式口径固定为：

- 同一时刻一个 `disk_id` 只能对应一个活跃 route

如果不同 storer connection 尝试注册相同 `disk_id`：

- 直接拒绝后到者
- 不允许隐式覆盖旧 route

## 10. route 失效后的传播

当 route connection 断线或 route heartbeat 超时后，gateway 必须：

1. 撤销该 route
2. 撤销该 route 名下全部未消费 `auth_id`
3. 关闭该 route 名下全部 session
4. 向仍在线的 client 发送 `SessionCloseNotice`

这样可避免：

- 悬空授权对象
- 悬空 session
- client 侧迟迟不知道该盘已经失效
