# 认证与路由

## 1. 领盘码模型

### 格式

- 总长度不少于 `80`
- 前 `16` 个字符为 `disk_id`
- 剩余 `64+` 个字符为领取密钥
- 字符集为 `0-9a-zA-Z`

### storer 初始化

如果与可执行文件同级目录下存在 `config.toml`，则直接读取：

- `role = whole | storer`
- `claim_code`
- 存储文件路径
- `whole.listen_addr` 或 `storer.gateway_addr + gateway_token`

如果 `config.toml` 不存在，则进入初始化流程：

1. 交互输入 `role`
2. 交互输入存储文件路径
3. 按 `role` 输入：
   - `whole.listen_addr`
   - 或 `storer.gateway_addr + storer.gateway_token`
4. 自动生成 `claim_code`
5. 写回 `config.toml`
6. 仅在初始化时向终端打印一次完整领盘码

第一阶段不要求用户手动输入领盘码，也不要求用户手动生成 `disk_id`。

### gateway 初始化

`gateway` 也使用与可执行文件同级目录下的 `config.toml`。

如果 `config.toml` 不存在，则进入初始化流程：

1. 交互输入 `client.listen_addr`
2. 交互输入 `storer.listen_addr`
3. 自动生成 `gateway_token`
4. 写回 `config.toml`
5. 仅在初始化时向终端打印一次完整 `gateway_token`

## 2. 路由缓存来源

当前草案默认采用 `storer -> gateway` 注册方式。

`storer` 启动后向 `gateway` 注册：

- `gateway_token`
- `disk_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `session_ttl_seconds`

`gateway` 将其放入本地路由缓存，用于后续认证和转发。

这条缓存链路的目的只有两个：

1. 快速判断某个 `disk_id` 是否存在真实目标
2. 在 `gateway` 本地完成 `proof` 校验，避免把假盘和错码压力推给 `storer`

其中：

```text
auth_verifier = SHA512(claim_code_bytes)
```

`gateway` 不保存完整 `claim_code`。

当前口径：

- `claim_code` 仍然只由存储侧持有
- `gateway_token` 是 `storer <-> gateway` 控制面凭据
- 注册成功后，`gateway <-> storer` 不重新设计第二套数据面命令，而是复用 `SessionOpen / ReadAt / WriteAt / Ping / Close`

## 3. 认证协议

当前 `client-and-gateway` 业务协议必须拆成三段语义：

1. 认证阶段
2. 会话建立阶段
3. 数据面阶段

其中认证阶段只包含：

1. `AuthStart`
2. `AuthFinish`

### AuthStart

客户端发送：

```text
AuthStartRequest {
  disk_id
}
```

`gateway` 行为：

1. 接收 `disk_id`
2. 查询本地路由缓存
3. 无论是真盘还是假盘，都生成一份 challenge 返回
4. 不在这一阶段暴露认证结果

服务端返回：

```text
AuthStartResponse {
  challenge_token
  salt_bytes
  ttl_seconds
  algo_version
}
```

约束：

- `salt` 使用固定长度随机字节，当前建议 `16B`
- `salt_bytes` 为原始 `16` 字节随机盐
- 真盘和假盘的返回字段完全一致
- 真盘和假盘的返回路径都不触发 `storer` 数据面

### challenge_token

`challenge_token` 是一次认证实例的唯一标识，不是 `disk_id` 的附属状态。

必须满足：

- 每次 `AuthStart` 生成新的 token
- 同一个 `disk_id` 的两个不同连接拿到的 token 必须不同
- token 绑定当前连接实例，不绑定源 IP
- token 自带过期信息
- token 对客户端不透明

建议内容：

```text
challenge_token = Seal({
  disk_id,
  salt,
  issued_at,
  expire_at,
  conn_nonce,
  auth_version
})
```

说明：

- `conn_nonce`：网关内部连接实例标识
- `Seal(...)`：使用网关私有密钥生成客户端不可见的 opaque token，并附带完整性校验

这允许我们做到：

- 不维护 `pending[disk_id]`
- 同一个 `disk_id` 可并发认证
- 假盘不需要分配完整的 challenge 临时表

### AuthFinish

客户端本地计算：

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

`gateway` 行为：

1. 校验 `challenge_token` 签名
2. 校验 `challenge_token` 是否过期
3. 校验 token 是否属于当前连接实例
4. 根据 token 中的 `disk_id` 查询本地路由缓存
5. 若命中真实盘，使用缓存的 `auth_verifier` 本地校验 `proof`
6. 若未命中真实盘，直接进入统一失败路径
7. `proof` 通过后，只在 `gateway` 内部标记“当前 connection 已对该 `disk_id` 完成认证”
8. 不向 client 暴露 `storer` 地址

硬约束：

- 认证成功不等于会话已建立
- 认证成功不创建 `DiskSession`
- 认证成功只表示当前 connection 获得“申请打开该盘会话”的资格

### 失败路径

所有 `AuthFinish` 失败统一走同一套失败语义：

- 真盘但领盘码错误
- 假盘
- 过期 token
- token 签名非法
- token 不属于当前连接
- `proof` 格式非法

统一处理：

1. 返回通用认证失败
2. 随机延迟 `2-5s`
3. 清理当前未认证连接上下文

这里的关键要求：

- 不允许通过微小延迟差异稳定区分真盘和假盘
- 延迟只加在 `AuthFinish` 失败路径
- `AuthStart` 保持快且统一

## 4. 假盘处理口径

假盘必须完整走外部流程，但不进入真实资源路径。

当前口径：

- `AuthStart`：假盘照样返回 challenge
- `AuthFinish`：假盘照样收 `proof`
- 校验阶段不接触 `storer`
- 失败后按统一 `2-5s` 路径清理

这样做的目的只有两个：

1. 不让外部轻易判断某个 `disk_id` 是否真实存在
2. 不让假盘请求消耗 `storer` 资源

## 5. 会话建立协议

认证通过后，client 不改连到 `storer`，而是继续留在当前 `gateway` 连接上。

下一步由 client 显式发送：

```text
SessionOpenRequest {
  disk_id
}
```

`gateway` 行为：

1. 校验当前 connection 是否已通过该 `disk_id` 认证
2. 根据路由缓存找到目标 `storer`
3. 把“打开这个盘会话”的请求交给 `storer`
4. 由 `storer` 根据打开策略决定是否允许打开
5. 若允许，则在内部建立 `gateway session -> storer session` 映射
6. 分配新的 `session_id`
7. 把会话元数据返回给 client

返回：

```text
SessionOpenResponse {
  session_id
  disk_size_bytes
  read_only
  max_io_bytes
  session_ttl_seconds
}
```

说明：

- `session_id` 是 client 后续全部数据面请求的唯一盘会话标识
- `disk_size_bytes`、`read_only`、`max_io_bytes` 是 `NetworkMedia` 构造所需最小元数据
- `gateway` 内部持有 `session_id -> storer session` 映射
- client 不持有 `storer_addr`
- 只有这一步成功后，client 才能构造 `DiskSession`
- 如果 `SessionOpen` 返回 busy，client 可以保留当前连接与认证资格，稍后继续重试 `SessionOpen`

## 6. 路由与转发边界

`gateway` 在第一版同时负责：

- 认证
- 盘路由
- client 数据面请求转发
- client 响应回传

转发边界：

- client 的 `ReadAt / WriteAt / Ping / Close` 全部先到 `gateway`
- `gateway` 按 `session_id` 找到对应 `storer session`
- `gateway` 再把请求转发给目标 `storer`
- `storer` 返回结果后，由 `gateway` 回给原 client

`gateway` 不缓存盘数据，不承担真实读写语义，只承担会话与请求路由。

## 7. 多登录口径

认证层不处理“同一 `disk_id` 是否允许多登录”的业务决策。

当前口径：

- 两个连接如果都持有正确领盘码，都可以通过认证
- 两个连接都可以继续发 `SessionOpen`
- 但能不能真的拿到会话，不由认证层决定
- 认证成功后谁有操作权、是否共享、是否排它、是否只读，由 `storer` 在 `SessionOpen` 阶段决定
