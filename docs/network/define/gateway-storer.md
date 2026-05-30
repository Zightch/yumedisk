# Gateway-Storer

## 定位

本文档定义外部 `gateway <-> storer` 网络边的业务协议。

这里的 `storer` 指 route provider，不涉及某个程序如何部署 `whole`。`whole` 属于 server 实现策略，见 [Server 实现文档](../server/README.md)。

## 连接方向

固定方向：

```text
storer ----主动连接----> gateway
```

在这条边上：

- 不定义 `Hello`
- 不定义 client-facing `auth_id`
- 不定义 `disk_id` 级多路选择后的二次路由

一次成功注册后，该 route connection 绑定一次声明的 `disk_id`。

## 通用业务头

`gateway-storer` 与 `client-gateway` 共用同一套 `24` 字节业务头与大端整数规则，见 [client-gateway](client-gateway.md)。

本边额外使用的操作码只有：

| 码值 | 名称 | 方向 |
| --- | --- | --- |
| `0x20` | `StorerRegister` | req/resp |
| `0x21` | `LinkHeartbeat` | req/resp |
| `0x03` | `SessionOpen` | req/resp |
| `0x04` | `SessionDescribe` | req/resp |
| `0x05` | `SessionCloseNotice` | notice |
| `0x06` | `SessionDataChangedNotice` | notice |
| `0x10` | `ReadAt` | req/resp |
| `0x11` | `WriteAt` | req/resp |

## StorerRegister

### 作用

`StorerRegister` 只解决三件事：

1. 让 gateway 认证这条 route connection 是否可信
2. 让 gateway 知道这条 route 对应哪个 `disk_id`
3. 让 gateway 获得该 route 的认证材料

### 请求 body

| 顺序 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| `1` | `gateway_token_len` | `u16` | token 长度 |
| `2` | `gateway_token` | `bytes[N]` | route trust token |
| `3` | `disk_id` | `ASCII[16]` | route 绑定 disk |
| `4` | `auth_verifier` | `bytes[64]` | 认证校验材料 |

### 成功响应

- `status_code = OK`
- 响应 body 当前协议不要求固定内容

### 成功后的绑定事实

一旦注册成功，协议层承认以下事实：

- 这条 route connection 绑定声明的 `disk_id`
- gateway 后续可在该 route 上发 `SessionOpen / SessionDescribe / ReadAt / WriteAt`

## Route-Facing SessionOpen

在这条边上，`SessionOpen` 不再携带 `auth_id`。

### 请求

- header `session_id = 0`
- body 为空

含义为：

- 在当前 route 上为已绑定的盘打开一个真实上游 session

### 成功响应

协议层最小成功结果固定为：

- `status_code = OK`
- 响应头 `session_id = upstream_session_id`

成功响应体是否携带附加负载，由实现决定，协议层不解释。

### 失败响应

协议层只定义 open 失败这一事实，不定义统一失败业务语义。

## Route-Facing SessionDescribe

### 请求

- header `session_id` 为目标 `storer_session_id`
- body 固定为空

### 成功响应

成功响应 body 与 `client-gateway` 的 `SessionDescribe` 完全一致，见 [client-gateway](client-gateway.md)。

metadata 真源固定在 `storer`，gateway 只负责按 session 映射转发。

## 共享数据面

注册成功后，这条边承载：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `SessionDataChangedNotice`
- `SessionCloseNotice`

字段格式与 [data-plane](data-plane.md) 一致。

此处的 `session_id` 是 storer 自己的会话标识，不等于 client 看到的 `session_id`。

## SessionCloseNotice

在这条边上，协议层允许：

- `gateway -> storer`
- `storer -> gateway`

主动发出 `SessionCloseNotice`。

当前项目的最小闭环实现只要求：

- `gateway -> storer`

notice body 与 `reason_code` 定义见 [data-plane](data-plane.md)。

## SessionDataChangedNotice

### 主动发起边界

在这条边上，当前正式方向固定为：

- `storer -> gateway`

### notice body

- 固定为空

### 语义

- `SessionDataChangedNotice` 是 dedicated data notice，不复用 close
- notice 头 `session_id` 为目标 `storer_session_id`
- 它只表达目标 session 对应 backend 的数据内容已经变化
- 它不表示 session 已失效
- 它不等待回复

## LinkHeartbeat

### 方向

当前固定只有一个方向：

- `gateway -> storer`

不存在：

- `storer -> gateway` 主动心跳
- session-scoped heartbeat
- 第三条心跳分支

### body

固定长度 `8` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `nonce` | `u64` |

成功响应必须回显同一个 `nonce`。

## Route 失效事实

协议层只定义以下事实：

- route transport 断开时，该 route 下 session 全部失效
- `LinkHeartbeat` 失败时，该 route 可以被判定为失效
- route 失效后，gateway 不能再把新的 `SessionOpen / SessionDescribe / ReadAt / WriteAt` 发到该 route
- `storer` 不对 session 维护独立 TTL；session 生命周期由 `SessionCloseNotice`、route 失效和 connection 断开收束

至于：

- gateway 如何撤销 grant
- gateway 如何发送 `SessionCloseNotice`
- storer heartbeat 超时后是否退出

由 server 实现文档定义。
