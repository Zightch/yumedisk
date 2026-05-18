# Server Implementation

## 定位

本文档只描述当前 server 侧实现口径。

它承接 `docs/network/define` 没有定义的 server 侧策略，包括：

- 程序边界与运行形态
- gateway 内部真源状态
- storer 的注册、watchdog 与退出策略
- `whole` 的内嵌网关方式
- 当前 bootstrap payload 与 metadata 来源

## 程序关系

当前程序关系固定为：

```text
程序 gateway
程序 storer(role=storer | whole)
```

这里要明确区分：

- `gateway` 是独立程序
- `storer` 是独立程序
- `storer` 程序只有两个运行形态：
  - `role=storer`
  - `role=whole`

不再把 `gateway` 和 `storer role` 混写成同一层级的“角色枚举”。

## 当前三种运行形态

### gateway 程序

职责固定为：

- 对 client 提供 `Hello -> transport -> auth -> session -> data plane`
- 接收外部 storer 的 `StorerRegister`
- 持有 route / auth grant / session 三张真源表
- 负责 route 选择、认证、会话建立、metadata 查询与故障传播

### storer 程序 `role=storer`

职责固定为：

- 打开本地介质
- 主动连接 gateway
- 发送 `StorerRegister`
- 在 route connection 上承接 `SessionOpen / ReadAt / WriteAt / Close`
- 维护 gateway 主动喂狗的 `LinkHeartbeat` watchdog

它不负责：

- 对 client 监听
- 对外暴露 client-facing `Hello`
- 内嵌 gateway client-facing 能力

### storer 程序 `role=whole`

职责固定为：

- 打开本地介质
- 内嵌 gateway core
- 对 client 暴露完整 client-facing 监听口
- 固定把 route 路由到本地唯一盘

约束固定为：

- `whole` 对 client 仍完整走 `Hello -> transport -> auth -> session -> SessionDescribe -> data plane`
- `whole` 不对其他 storer 暴露 storer listener
- `whole` 不走外部 `StorerRegister` 网络注册
- `whole` 的注册表里只承载自己的本地盘

## 当前第一版非目标

当前正式主线不做以下事情：

- client 直连 storer
- 一个 storer 进程承载多盘
- locator
- 分片
- 多副本
- 自动重连
- 自动恢复
- 历史协议兼容桥

## 真源状态

当前唯一控制真源都在 gateway 侧。

### `route_registry`

表示可用 route，至少承接：

- `disk_id`
- `route_connection_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `route_state`

当前正式约束：

- 一个 `storer` 进程只承载一个 `disk_id`
- 一个 route connection 只绑定一个 `disk_id`
- 同一时刻一个 `disk_id` 只允许一个活跃 route
- route metadata 在当前 route 生命周期内视为不可变

### `auth_grant_registry`

表示 `auth_id` 真源，至少承接：

- `auth_id`
- `client_connection_id`
- `disk_id`
- `expire_at`
- `grant_state`

### `session_registry`

表示 client 可见 session 真源，至少承接：

- `gateway_session_id`
- `client_connection_id`
- `route_connection_id`
- `storer_session_id`
- `disk_id`
- session metadata snapshot
- `session_state`

## metadata 当前口径

当前项目采用单一真实来源原则，metadata 路径固定为：

1. `role=storer` 在 `StorerRegister` 上送 `disk_size_bytes / read_only / max_io_bytes`
2. gateway 将其写入 `route_registry`
3. `SessionOpen` 成功时，gateway 把 route metadata 复制到 `session_registry` 作为 session snapshot
4. `SessionDescribe(session_id)` 由 gateway 从本地 session snapshot 回答

因此当前不做：

- `SessionOpen` 顺带返回 metadata
- `SessionDescribe` 再向 storer 发额外 round-trip
- route metadata 与 session metadata 双向同步

## 当前 bootstrap 口径

`docs/network/define` 只定义 `Hello` 的 payload 是 bootstrap 负载。当前 server 实现口径固定为：

- `HelloRequest` body 为空
- `HelloResponse.server_capabilities` 当前为空 payload
- 当前不在 `Hello` 后切 TLS，bootstrap 成功后直接进入 transport

这只是当前项目 payload 选择，不是 `docs/network/define` 的硬编码约束。

## 当前认证口径

gateway 当前实现策略为：

- `AuthStart` 生成 connection-bound challenge token
- challenge TTL 当前为 `30s`
- `algo_version = 1`
- 认证材料按 `HMAC-SHA512(key = auth_verifier, msg = salt)` 校验
- 认证失败时当前 gateway 采用随机 `2..5s` 延迟失败路径

当前 storer / whole 侧认证材料口径为：

- claim code 至少 `80` 个字节
- 前 `16` 字节对应 `disk_id`
- 整个 claim code 字节串计算 `SHA512` 得到 `auth_verifier`
- storer 注册时上传的是 `auth_verifier`，不是原始 claim code
- gateway 不保存原始 claim code，只保存 route 侧认证材料

当前 challenge token 的内部编码口径为：

- version
- `connection_id`
- `expire_at`
- `disk_id`
- `salt`
- `HMAC-SHA512` 完整性标签

也就是说，token 当前同时绑定 connection、目标盘、随机盐和过期时间。这个结构属于 gateway 实现细节，不在 `docs/network/define` 展开。

## `SessionOpen` 当前 server 口径

当前 server 文档对 `SessionOpen` 收口如下：

- network 协议层只定义 `auth_id -> session_id`
- gateway 成功时返回 `OK + session_id`
- 当前 gateway 成功响应不额外携带业务负载
- metadata 一律走后续 `SessionDescribe`

失败面当前收口为：

- `docs/network/define` 不定义统一 open-failure 业务语义
- 当前项目把 open 失败视为 server 自己的 reject / failure
- 若某个 storer 策略选择拒绝新的 open，gateway 当前返回实现自定义状态码 `0x1202`
- 当前 `0x1202` 的项目语义是 `open rejected`
- 当前 reject response body 为空
- open reject 不消费 `auth_id`
- 被 reject 的 `auth_id` 继续保留，直到后续 open 成功、授权过期、connection 关闭或 route 被撤销

也就是说，open 失败的具体业务含义属于 server 实现口径，不再写进 network 正式协议。

## gateway-storer route 口径

当前 gateway 程序对外 route 逻辑固定为：

- 外部 storer 主动连接 gateway
- gateway 验证 `gateway_token`
- 注册成功后，把这条连接视为 route connection
- `gateway -> storer` 的 `SessionOpen` 不再带 `disk_id` 或 `auth_id`

当前 `role=whole` 的固定路由口径为：

- 不走外部 storer 网络边
- 内嵌 gateway 通过本地 backend 固定路由到自己
- client 看到的仍是完整 client-gateway 协议

## 当前数据面转发口径

当前 gateway 的数据面职责固定为：

1. 接收 client 侧 `gateway_session_id`
2. 查本地 `session_registry`
3. 还原 `(route_connection_id, storer_session_id)`
4. 改写上游 `request_id / session_id`
5. 把结果回传给 client

当前明确不做：

- 缓存盘数据
- 改写成功/失败业务语义
- 替 client 做跨请求重组

## 心跳与故障传播

### client-gateway

当前 gateway 只接受一种 client connection 心跳：

- `client -> gateway : ConnHeartbeat`

gateway 返回普通 response，不再派生其他 client 心跳分支。

### gateway-storer

当前只有一种 route 心跳：

- `gateway -> storer : LinkHeartbeat`

当前项目策略为：

- gateway 主动发送 heartbeat
- storer 必须立即回显 `nonce`
- storer 本地 watchdog 超时未收到 heartbeat 时，直接走错误退出路径
- gateway heartbeat 超时未收到回复时，把该 route 视为死亡连接

`role=whole` 的本地 fixed route 不走外部 `LinkHeartbeat` 网络链路。

## route / connection / session 清理

当前 gateway 在 route 失效后的清理口径为：

1. 撤销该 route
2. 撤销该 route 名下未消费 `auth_id`
3. 收束该 route 名下活跃 session
4. 对仍在线的 client 发送 `SessionCloseNotice`
5. 清理本地映射

当前 storer 在 connection 结束后的口径为：

- 关闭该 connection 名下本地 session
- 结束本地 route runtime
- `role=storer` 直接退出该次 gateway 连接运行

## `whole` 的 client-facing 结果

`whole` 虽然内嵌 gateway，但对 client 暴露的仍是标准 client-gateway 协议。

因此：

- client 不需要知道对端是独立 gateway 还是 `whole`
- `whole` 内部本地盘失效时，也必须映射成标准 session / connection 失效结果
- 对 client 的故障表达仍通过已有状态码与 `SessionCloseNotice` 完成
