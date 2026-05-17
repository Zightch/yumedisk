# Gateway-and-Storer 业务层协议

## 1. 文档定位

本文档定义 `gateway <-> storer` 的正式边界。

当前正式口径只拆成两部分：

1. 注册阶段
2. route connection 上的会话与数据面复用

当前不把 client 的认证流程下发给 storer。

## 2. 角色与连接方向

### 2.1 whole

- 持有本地存储
- 内嵌 gateway
- 对 client 监听
- 不走本注册协议

### 2.2 storer

- 持有本地存储
- 不对 client 监听
- 主动长连 `gateway`
- 注册成功后承接 route connection 上的会话和数据面

连接方向固定为：

```text
storer ----主动长连----> gateway
```

## 3. 注册阶段

### 3.1 注册目的

注册阶段只解决：

1. 让 gateway 知道这个 storer 是否可信
2. 让 gateway 知道这个 storer 承载哪个 `disk_id`
3. 让 gateway 预缓存认证材料与基础元数据

### 3.2 注册字段

`storer` 向 `gateway` 提交：

- `gateway_token`
- `disk_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

其中：

- `auth_verifier = SHA512(claim_code_bytes)`
- `claim_code` 原文不上送
- `gateway_token` 只用于基础设施控制面信任

### 3.3 route 缓存

注册成功后，gateway 至少缓存：

- `disk_id`
- `auth_verifier`
- `route_connection`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

### 3.4 disk_id 唯一性

同一时刻，一个 `disk_id` 只能对应一个活跃 route。

如果不同 route connection 尝试注册相同 `disk_id`：

- 直接拒绝后到者
- 不允许隐式覆盖已有 route

## 4. route connection

注册成功后，`gateway <-> storer` 的长连即成为 route connection。

该连接负责：

- `SessionOpen`
- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`
- route heartbeat

## 5. 不下发 client 认证

当前正式边界固定如下：

- `AuthStart / AuthFinish / auth_id` 只存在于 `client <-> gateway`
- `gateway` 不把 client challenge/proof 转发给 storer
- storer 不生成 `auth_id`

storer 只看到：

- 这是 gateway 发来的合法 `SessionOpen / SessionDescribe / ReadAt / WriteAt / Close`

## 6. SessionOpen 复用口径

### 6.1 client 侧输入

client 侧正式输入是：

```text
SessionOpen(auth_id)
```

### 6.2 gateway 侧还原

gateway 用 `auth_id` 还原：

- `disk_id`
- route connection

### 6.3 storer 侧输入

gateway 向 storer 发起的是 storer-facing `SessionOpen`。

storer 不需要知道 client 的 `auth_id`，只需要知道：

- 要为该 route 上的某个 `disk_id` 打开真实会话

### 6.4 成功返回

成功时 storer 返回：

- `storer_session_id`

metadata 不应再强绑定在 `SessionOpen` 响应里。

## 7. SessionDescribe 复用口径

会话打开后，client 侧通过 `SessionDescribe(session_id)` 取 metadata。

gateway 转发逻辑：

1. client 发 `gateway_session_id`
2. gateway 查表得到 `(route_connection, storer_session_id)`
3. gateway 向 storer 发 `SessionDescribe(storer_session_id)`
4. storer 返回 metadata
5. gateway 原样回给 client

## 8. ReadAt / WriteAt / Close 复用口径

`SessionDescribe` 之后，client 可进入数据面。

转发逻辑：

1. client 对 `gateway_session_id` 发请求
2. gateway 查表得到 `(route_connection, storer_session_id)`
3. gateway 改写 `request_id` 和 `session_id`
4. storer 返回结果
5. gateway 恢复 client 侧 `request_id`
6. gateway 回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改成功/失败语义
- `gateway` 只负责映射、转发、回传

## 9. route heartbeat

`gateway <-> storer` 维持 route connection 级心跳。

当前方向固定为：

- gateway 主动喂狗
- storer 超时未收到喂狗则退出

这是 route connection 级能力，不属于某个 session。

## 10. route 故障传播

当 route connection 断线或 heartbeat 超时后，gateway 必须立即：

1. 撤销该 route
2. 撤销该 route 关联的未消费 `auth_id`
3. 找出该 route 名下全部活跃 session
4. 将这些 session 收束为 closed
5. 对仍在线的 client connection 发送 `SessionCloseNotice`
6. 清理本地映射

不允许保留悬空 route、悬空授权或悬空 session。

## 11. 当前正式约束

1. storer 主动连接 gateway。
2. `gateway_token` 只用于 storer 注册信任。
3. 同一时刻同一 `disk_id` 只允许一个活跃 route。
4. client 认证不下发到 storer。
5. `auth_id` 只存在于 client-gateway 边界。
6. storer 只处理真实会话和数据面。
7. route 断线时 gateway 必须主动接管关闭相关 client session。
