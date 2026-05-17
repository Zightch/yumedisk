# YumeDisk Client-and-Gateway Business Protocol v1

## 1. 文档定位

本文档定义 `client <-> gateway` 正式业务协议。

本文档只描述 bootstrap 完成后的业务层，不重复定义 transport 帧格式。  
`Hello` 与可选 TLS 属于 bootstrap，见 [transport.md](transport.md) 的分层说明。

本文档高于：

- `overview.md`
- `auth-routing.md`
- `data-plane.md`

若其他文档与本文档冲突，以本文档为准，并应同步修正其他文档。

## 2. 当前正式主链

当前正式主链固定为：

```text
TCP connected
  -> Hello
  -> optional TLS
  -> transport ready
  -> connection established
  -> AuthStart
  -> AuthFinish
  -> auth_id
  -> SessionOpen(auth_id)
  -> session_id
  -> SessionDescribe(session_id)
  -> ReadAt / WriteAt / Close
```

关键约束：

- `Hello` 不属于本文档
- TLS 握手不属于本文档
- 认证成功后必须产生显式 `auth_id`
- `SessionOpen` 只负责打开会话
- metadata 必须通过 `SessionDescribe` 获取

## 3. 业务语义分层

当前正式协议必须拆成四段：

1. 认证层
2. 授权层
3. 会话层
4. metadata + 数据面层

其中：

- 认证层：`AuthStart / AuthFinish`
- 授权层：`auth_id`
- 会话层：`SessionOpen / SessionCloseNotice`
- metadata + 数据面层：`SessionDescribe / ReadAt / WriteAt / Close`

并发与互斥规则固定如下：

- 一个 connection 同时最多只允许一个 auth 过程
- 一个 connection 同时最多只允许一个 `SessionOpen` 过程
- auth 过程与 `SessionOpen` 过程互斥，不能同时存在
- 已打开 session 上的数据面请求允许并发

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
| `3` | 1 | `flags` | `u8` | 响应/notice 等标志 |
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
- `SessionOpen` 请求固定为 `0`
- `SessionOpen` 成功响应在头部返回新分配 `session_id`
- session-scoped 请求必须带有效非 `0` `session_id`
- notice 若与某个 session 绑定，应在头部写该 `session_id`

## 6. 操作码

| `op_code` | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| `0x01` | `AuthStart` | req/resp | 获取 challenge |
| `0x02` | `AuthFinish` | req/resp | 提交 proof，成功返回 `auth_id` |
| `0x03` | `SessionOpen` | req/resp | 用 `auth_id` 打开会话 |
| `0x04` | `SessionDescribe` | req/resp | 查询 session 元数据 |
| `0x05` | `SessionCloseNotice` | notice | gateway 异步通知 session 已关闭 |
| `0x10` | `ReadAt` | req/resp | 读取 |
| `0x11` | `WriteAt` | req/resp | 写入 |
| `0x12` | `ConnHeartbeat` | req/resp | 连接级心跳 |
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
| `0x1004` | `ERR_INVALID_REQUEST_ID` | `request_id` 非法 |
| `0x1005` | `ERR_UNSUPPORTED_OP` | 不支持的 op |

### 7.3 认证与授权错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1101` | `ERR_AUTH_FAILED` | 统一认证失败 |
| `0x1102` | `ERR_AUTH_EXPIRED` | challenge 过期 |
| `0x1103` | `ERR_AUTH_CHALLENGE_INVALID` | challenge 非法或不属于当前 connection |
| `0x1104` | `ERR_AUTH_ID_INVALID` | `auth_id` 不存在、不属于当前 connection 或已消费 |
| `0x1105` | `ERR_AUTH_ID_EXPIRED` | `auth_id` 过期 |

### 7.4 会话错误

| 码值 | 名称 | 说明 |
| --- | --- | --- |
| `0x1201` | `ERR_SESSION_UNAVAILABLE` | session 不存在、已关闭或已回收 |
| `0x1202` | `ERR_SESSION_BUSY` | 目标盘当前不允许新的会话打开 |

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
- gateway 返回统一 challenge
- 真盘与假盘都走同样外部流程

当前 `AuthStart -> AuthFinish` 整体视为一个 auth 过程。  
该过程从 `AuthStart` 被接受开始，到 `AuthFinish` 成功、失败或超时结束为止。

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
- gateway 完成本地校验
- 成功时返回显式 `auth_id`

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
- `auth_id` 绑定单个 `disk_id`
- `auth_id` 有过期时间
- 认证失败不关闭 connection
- auth 过程进行中，不允许该 connection 再发第二个 `AuthStart`
- auth 过程进行中，不允许该 connection 同时发 `SessionOpen`

## 10. SessionOpen

### 10.1 作用

- 使用 `auth_id` 打开真实远端会话
- 是进入真实会话的唯一入口

### 10.2 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `auth_id` | `u64` |

### 10.3 成功响应

- 头部 `session_id` 为新分配会话 ID
- body 为空

### 10.4 约束

- `SessionOpen` 成功后，`auth_id` 被消费
- 若因 `busy` 失败，`auth_id` 仍可在未过期前继续重试
- `SessionOpen` 不返回 metadata
- 一个 connection 在前一个 `SessionOpen` 完成前，不允许并发发第二个 `SessionOpen`
- `SessionOpen` 过程进行中，不允许该 connection 同时进入 auth 过程

## 11. SessionDescribe

### 11.1 作用

- 在 session 已建立后查询盘元数据

### 11.2 请求

- 头部 `session_id` 必须为有效非 `0`
- body 为空

### 11.3 成功响应 body

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 8 | `disk_size_bytes` | `u64` | 容量 |
| `8` | 4 | `max_io_bytes` | `u32` | 单次 I/O 上限 |
| `12` | 2 | `session_flags` | `u16` | bit0 为只读 |
| `14` | 2 | `reserved` | `u16` | 固定 `0` |

### 11.4 约束

- `SessionDescribe` 是 session-scoped，不是 disk-scoped
- 未开 session 不能单独查 metadata

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

- `ConnHeartbeat` 属于 connection，不属于某个 session
- connection heartbeat 超时等价于 connection 死亡

## 15. Close

### 15.1 作用

- 主动关闭指定 `session_id`

### 15.2 请求与响应

- body 为空
- 头部带目标 `session_id`

### 15.3 约束

- `Close` 只关闭目标 session
- 不直接关闭整条 connection
- 重复关闭已不存在 session 返回 `ERR_SESSION_UNAVAILABLE`

## 16. SessionCloseNotice

### 16.1 作用

- gateway 异步通知 client：某个 session 已被动关闭

### 16.2 触发场景

- storer route 断线
- storer 主动结束 session
- gateway 接管故障传播并关闭 session

### 16.3 notice body

当前 body 为空。  
关闭原因由头部 `status_code` 表示，或后续独立扩展。

## 17. 严格状态机

### 17.1 bootstrap 前的非法业务字节

直接断连接。

### 17.2 业务对象前置条件不满足

返回业务错误，不直接断连接。

例如：

- 无效 `auth_id`
- 过期 `auth_id`
- 无效 `session_id`
- auth 过程与 `SessionOpen` 过程互斥冲突
- 同 connection 上出现第二个并发 auth 过程
- 同 connection 上出现第二个并发 `SessionOpen` 过程

### 17.3 协议结构错误

直接按协议错误处理，并终止 connection。

例如：

- header 非法
- 非 notice 包错误使用 `FLAG_NOTICE`
- response 冒充 request

## 18. 连接失效口径

connection 死亡后：

- 该 connection 下 pending request 全部失败
- 该 connection 下全部 `auth_id` 失效
- 该 connection 下全部 session 失效

文档层当前不强制规定 `NetworkMedia` 必须立即卸载，但它必须把这些 session 视为失效对象。

## 19. 第一版最小验收

当前正式验收口径固定为：

1. `Hello -> optional TLS -> transport` 分层成立
2. client 可通过 `AuthStart / AuthFinish` 获得 `auth_id`
3. client 可通过 `SessionOpen(auth_id)` 获得有效 `session_id`
4. client 可通过 `SessionDescribe(session_id)` 获得 metadata
5. `NetworkMedia` 可基于 `session_id` 完成真实 `ReadAt / WriteAt`
6. 一个 `GatewayConnection` 可并发承载多个 `DiskSession`
7. route 故障时 gateway 能向 client 下发 `SessionCloseNotice`
8. 单 connection 上 auth 过程和 `SessionOpen` 过程满足“各自唯一且互斥”的约束
