# Auth And Routing

## 定位

本文档只定义认证和路由的协议语义，不定义：

- claim code 的 UI 表现
- challenge token 的内部编码
- gateway 内部 registry 实现
- `whole` 的 fixed route 实现

## 协议边界

认证只发生在 `client-gateway` 边界。

固定事实：

- `AuthStart / AuthFinish / auth_id` 只存在于 `client-gateway`
- `gateway-storer` 不接收 `auth_id`
- route 选择以 `disk_id` 为入口
- `SessionOpen` 以 `auth_id` 为入口

## `disk_id`

协议层只要求：

- 长度 `16` 字节
- 字符集为 `0-9a-zA-Z`

client 通过 `AuthStart(disk_id)` 声明目标盘。

## AuthStart / AuthFinish

详细 wire 见 [client-gateway](client-gateway.md)。

认证流程固定为：

```text
AuthStart(disk_id)
  -> challenge_token + salt + ttl + algo_version
  -> AuthFinish(challenge_token, proof)
  -> auth_id
```

### challenge_token

协议层只要求：

- token 对 client 不透明
- token 绑定当前 connection
- token 带过期约束

token 的内部编码和签发方式由实现定义。

### `algo_version = 1`

当前协议定义的 `algo_version = 1` 校验公式为：

```text
proof = HMAC-SHA512(key = auth_verifier, msg = salt)
```

其中：

- `salt` 来自 `AuthStart`
- `auth_verifier` 是 route 绑定的认证材料

client 如何从本地输入恢复 `auth_verifier`，由具体实现定义。

## `auth_id`

`auth_id` 的协议语义固定为：

- connection-scoped
- 单盘绑定
- 可消费一次
- 只用于 `SessionOpen(auth_id)`

它不表示：

- connection 永久进入 authenticated 状态
- session 已经存在
- metadata 已经返回

### 生命周期

协议层承认以下状态流转：

```text
granted
  -> consumed
  -> expired
  -> revoked
```

其中：

- `granted`：`AuthFinish` 成功
- `consumed`：`SessionOpen(auth_id)` 成功
- `expired`：授权超时未消费
- `revoked`：connection 或 route 失效后被撤销

同一条 connection 上可以并存多个 `granted` 状态 `auth_id`。

## 路由选择语义

协议层只定义网关是路由选择点：

1. `AuthStart` 请求带 `disk_id`
2. `AuthFinish` 成功得到绑定该盘的 `auth_id`
3. `SessionOpen(auth_id)` 时，gateway 用 `auth_id` 还原目标盘并选择 route
4. gateway 在选中的 route 上发起 route-facing `SessionOpen`

至于 route 从哪里来、如何保存、如何和 metadata 绑定，都属于 server 实现。

## 认证与 route 失效

协议层只定义以下事实：

- challenge 只属于当前 connection
- connection 失效时，该 connection 下未消费 `auth_id` 一起失效
- route 失效时，该 route 对应盘的未消费 `auth_id` 可以一起失效

认证失败是否随机延迟、失败后如何清 challenge、route 失效后清理顺序，都属于实现策略。
