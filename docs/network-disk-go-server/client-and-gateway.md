# YumeDisk Client-and-Gateway Business Protocol v1

## 1. 文档定位

本文档定义 `client <-> gateway` 正式业务协议。

当前口径已经收敛为：

- `auth_id` 是显式授权对象
- `SessionOpen(auth_id)` 是进入真实会话的唯一入口
- `SessionOpen` 只负责打开会话，不返回 metadata
- `SessionDescribe(session_id)` 单独返回 metadata
- `NetworkMedia` 基于 `disk_id + session + metadata` 构造

本文档只描述 bootstrap 完成后的业务层，不重复定义 transport 帧格式。

## 2. 当前正式主链

```text
TCP connected
  -> Hello
  -> optional TLS
  -> transport ready
  -> AuthStart
  -> AuthFinish
  -> auth_id
  -> SessionOpen(auth_id)
  -> session_id
  -> SessionDescribe(session_id)
  -> metadata
  -> ReadAt / WriteAt / Close
```

补充说明：

- `Hello` 不属于本文档
- TLS 握手不属于本文档
- `auth_id` 只存在于 `client-gateway`
- metadata 不再混入 `SessionOpen`

## 3. 业务语义分层

当前正式协议拆成五段：

1. 认证层：`AuthStart / AuthFinish`
2. 授权层：`auth_id`
3. 会话层：`SessionOpen / SessionCloseNotice`
4. metadata 层：`SessionDescribe`
5. 数据面层：`ReadAt / WriteAt / Close / ConnHeartbeat`

建会话前互斥规则固定如下：

- 一个 connection 同时最多只允许一个 auth 过程
- 一个 connection 同时最多只允许一个 `SessionOpen` 过程
- auth 过程与 `SessionOpen` 过程互斥
- 已打开 session 的 `SessionDescribe / ReadAt / WriteAt / Close` 允许并发
- 活跃 session 的存在，不阻止该 connection 后续再次串行发起 auth/open

## 4. 基础约定

### 4.1 transport 引用

所有业务消息都承载在 [transport.md](transport.md) 定义的单帧 payload 中。

### 4.2 字节序

除特别说明外，所有整数使用 `big-endian`。

### 4.3 基础类型

| 类型 | 字节数 | 范围 |
| --- | --- | --- |
| `u8` | 1 | `0..255` |
| `u16` | 2 | `0..65535` |
| `u32` | 4 | `0..4294967295` |
| `u64` | 8 | `0..18446744073709551615` |

### 4.4 字符与字节字段

| 名称 | 形式 | 说明 |
| --- | --- | --- |
| `disk_id` | `ASCII[16]` | 只允许 `0-9a-zA-Z` |
| `bytes_u16` | `u16be len + bytes[len]` | opaque token / 原始字节 |

### 4.5 保留字段

- 发送方必须写 `0`
- 接收方必须校验为 `0`
- 非 `0` 视为协议错误

## 5. 通用头

所有业务 payload 以固定头开始。

### 5.1 布局

固定长度：

```text
24 bytes
```

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 1 | `protocol_version` | `u8` | 当前固定 `1` |
| `1` | 1 | `header_len` | `u8` | 当前固定 `24` |
| `2` | 1 | `op_code` | `u8` | 命令码 |
| `3` | 1 | `flags` | `u8` | 响应/notice 标志 |
| `4` | 2 | `status_code` | `u16` | 请求时固定 `0` |
| `6` | 2 | `reserved` | `u16` | 固定 `0` |
| `8` | 8 | `request_id` | `u64` | 请求配对标识 |
| `16` | 8 | `session_id` | `u64` | session-scoped 命令时使用 |

### 5.2 flags

当前定义：

| bit | 名称 | 含义 |
| --- | --- | --- |
| `0` | `FLAG_RESPONSE` | 响应包 |
| `1` | `FLAG_NOTICE` | notice 包 |

约束：

- request：两个 bit 都为 `0`
- response：只允许 `FLAG_RESPONSE = 1`
- notice：只允许 `FLAG_NOTICE = 1`
- 未知 bit 置位视为协议错误

### 5.3 request_id

- request 必须为非 `0`
- 同一 connection 上在飞 request 必须唯一
- response 必须原样回显 request 的 `request_id`
- notice 的 `request_id` 固定为 `0`

### 5.4 session_id

- 非 session-scoped 请求固定为 `0`
- `AuthStart / AuthFinish / SessionOpen / ConnHeartbeat` 固定为 `0`
- `SessionOpen` 成功响应在头部返回新分配 `session_id`
- `SessionDescribe / ReadAt / WriteAt / Close` 必须带有效非 `0` `session_id`
- `SessionCloseNotice` 必须在头部写目标 `session_id`

## 6. 操作码

| `op_code` | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| `0x01` | `AuthStart` | req/resp | 获取 challenge |
| `0x02` | `AuthFinish` | req/resp | 提交 proof，成功返回 `auth_id` |
| `0x03` | `SessionOpen` | req/resp | 用 `auth_id` 打开会话 |
| `0x04` | `SessionDescribe` | req/resp | 读取 session metadata 快照 |
| `0x05` | `SessionCloseNotice` | notice | gateway 异步通知 session 已关闭 |
| `0x10` | `ReadAt` | req/resp | 读取 |
| `0x11` | `WriteAt` | req/resp | 写入 |
| `0x12` | `ConnHeartbeat` | req/resp | connection 级心跳 |
| `0x13` | `Close` | req/resp | 主动关闭 session |

## 7. 状态码

### 7.1 成功

| 码值 | 名称 |
| --- | --- |
| `0x0000` | `OK` |

### 7.2 协议错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1001` | `ERR_PROTOCOL_VERSION` | 协议版本错误 |
| `0x1002` | `ERR_BAD_HEADER` | 通用头非法 |
| `0x1003` | `ERR_BAD_BODY` | body 非法 |
| `0x1004` | `ERR_INVALID_REQUEST` | 请求与当前状态不匹配 |
| `0x1005` | `ERR_UNSUPPORTED_OP` | 不支持的 op |

### 7.3 认证与授权错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1101` | `ERR_AUTH_FAILED` | 统一认证失败 |
| `0x1102` | `ERR_AUTH_EXPIRED` | challenge 过期 |
| `0x1103` | `ERR_AUTH_CHALLENGE_INVALID` | challenge 非法或不属于当前 connection |
| `0x1104` | `ERR_AUTH_ID_INVALID` | `auth_id` 不存在、不属于当前 connection、已消费或已撤销 |
| `0x1105` | `ERR_AUTH_ID_EXPIRED` | `auth_id` 过期 |

### 7.4 会话错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1201` | `ERR_SESSION_UNAVAILABLE` | session 不存在、已关闭或 route 已消失 |
| `0x1202` | `ERR_SESSION_BUSY` | 目标盘当前已有活跃 session |

### 7.5 I/O 错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1301` | `ERR_IO_OUT_OF_RANGE` | 越界 |
| `0x1302` | `ERR_IO_TOO_LARGE` | 超过 `max_io_bytes` |
| `0x1303` | `ERR_IO_READ_ONLY` | 只读盘写入 |
| `0x1304` | `ERR_IO_FAILED` | 远端 I/O 失败 |

## 8. AuthStart

### 8.1 作用

- client 提交 `disk_id`
- gateway 返回 challenge
- challenge 只用于当前 connection 上的一次 auth 过程

### 8.2 请求 body

固定长度：

```text
16 bytes
```

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| `0` | 16 | `disk_id` |

### 8.3 成功响应 body

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 1 | `algo_version` | `u8` | 当前固定 `1` |
| `1` | 2 | `ttl_seconds` | `u16` | challenge TTL |
| `3` | 16 | `salt_bytes` | `bytes[16]` | 原始随机盐 |
| `19` | 2 | `challenge_token_len` | `u16` | token 长度 |
| `21` | `N` | `challenge_token` | `bytes[N]` | opaque token |

## 9. AuthFinish

### 9.1 作用

- client 提交 `challenge_token + proof`
- gateway 校验通过后生成显式 `auth_id`

### 9.2 请求 body

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| `0` | 2 | `challenge_token_len` | token 长度 |
| `2` | `N` | `challenge_token` | 来自 `AuthStart` |
| `2 + N` | 64 | `proof` | `HMAC-SHA512` 原始 64 字节摘要 |

### 9.3 成功响应 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `auth_id` | `u64` |

### 9.4 约束

- `auth_id` 只对当前 connection 有效
- 一个 `auth_id` 只绑定一个 `disk_id`
- 一个 `auth_id` 只能消费一次
- 一个 connection 同时最多只保留一个未消费 `auth_id`
- 认证失败不关闭 connection

## 10. SessionOpen

### 10.1 作用

- 使用 `auth_id` 打开真实远端会话
- 成功时只得到新的 `session_id`

### 10.2 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `auth_id` | `u64` |

### 10.3 成功响应

- 头部 `session_id` 为新分配的 `gateway_session_id`
- body 为空

### 10.4 约束

- `SessionOpen` 成功后，`auth_id` 被消费
- `SessionOpen` 失败且返回 `busy` 时，未过期 `auth_id` 仍可重试
- `SessionOpen` 不返回 metadata

## 11. SessionDescribe

### 11.1 作用

- 读取已打开 session 绑定的 metadata 快照

### 11.2 请求与响应

- 请求 body 为空
- 头部带目标 `session_id`

### 11.3 成功响应 body

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 8 | `disk_size_bytes` | `u64` | 容量 |
| `8` | 4 | `max_io_bytes` | `u32` | 单次 I/O 上限 |
| `12` | 2 | `session_flags` | `u16` | bit0 为只读 |
| `14` | 2 | `reserved` | `u16` | 固定 `0` |

### 11.4 约束

- metadata 来自 gateway `session_registry` 中的 session 快照
- `SessionDescribe` 不影响 `auth_id`
- `SessionDescribe` 不需要额外触发 storer round-trip

## 12. ReadAt

### 12.1 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |

### 12.2 成功响应 body

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| `0` | `N` | `data` |

## 13. WriteAt

### 13.1 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |
| `12` | `N` | `data` | `bytes[N]` |

### 13.2 成功响应 body

成功时 body 为空。

## 14. ConnHeartbeat

### 14.1 作用

- 维持 client-gateway connection 存活
- 检测连接是否可达

### 14.2 请求与响应

- body 为空
- 不带 `session_id`

### 14.3 约束

- 它属于 connection，不属于某个 session
- timeout 等价于 connection 死亡

## 15. Close

### 15.1 作用

- 主动关闭指定 `session_id`

### 15.2 请求与响应

- body 为空
- 头部带目标 `session_id`

### 15.3 约束

- `Close` 只关闭目标 session
- 不关闭整条 connection
- 重复关闭已不存在 session 返回 `ERR_SESSION_UNAVAILABLE`

## 16. SessionCloseNotice

### 16.1 作用

- gateway 异步通知 client：某个 session 已被动关闭

### 16.2 头部约束

- `FLAG_NOTICE = 1`
- `request_id = 0`
- `status_code = OK`
- `session_id = 目标 gateway_session_id`

### 16.3 notice body

固定长度：

```text
2 bytes
```

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 2 | `reason_code` | `u16` |

### 16.4 当前原因码

| 码值 | 名称 |
| --- | --- |
| `1` | `ROUTE_LOST` |
| `2` | `GATEWAY_SHUTDOWN` |
| `3` | `UPSTREAM_SESSION_CLOSED` |
| `4` | `CLIENT_CONNECTION_REPLACED` |
| `5` | `NORMAL_CLOSE_MIRROR` |
| `6` | `PROTOCOL_ERROR` |

当前第一版固定由 body 承载关闭原因，不再保留第二种原因承载位置。

## 17. 失效事件与宿主边界

### 17.1 connection 死亡

connection 死亡后：

- 该 connection 下 pending request 全部失败
- 该 connection 下全部未消费 `auth_id` 失效
- 该 connection 下全部 session 失效

### 17.2 session 失效

无论是收到 `SessionCloseNotice`，还是本地观察到 connection 死亡，协议层只定义：

- 目标 session 已失效
- 后续对该 session 的 `SessionDescribe / ReadAt / WriteAt / Close` 不应再返回成功

### 17.3 `NetworkMedia` 边界

网络层固定只做两件事：

1. 识别 connection / session 失效
2. 调用 `NetworkMedia` 的失效处理接口

网络层不直接决定：

- 是否立即删除盘对象
- 是否保留盘对象为严格假死挂起态

这些都由 client / 宿主策略决定。

### 17.4 假死挂起态要求

如果宿主保留对象，则该状态必须满足：

- 不主动返回成功
- 不伪装读写已完成
- 不静默自动重连

## 18. 第一版最小验收

1. `Hello -> optional TLS -> transport` 分层成立。
2. client 可通过 `AuthStart / AuthFinish` 获得 `auth_id`。
3. client 可通过 `SessionOpen(auth_id)` 获得有效 `session_id`。
4. client 可通过 `SessionDescribe(session_id)` 获得 metadata。
5. `NetworkMedia` 以 `disk_id + session + metadata` 完成构造。
6. `NetworkMedia` 可完成真实 `ReadAt / WriteAt`。
7. 一个 `GatewayConnection` 可并发承载多个 `DiskSession`。
8. route 故障时 gateway 能向 client 下发 `SessionCloseNotice`，网络层仅把失效事件上报给 `NetworkMedia` 接口。
