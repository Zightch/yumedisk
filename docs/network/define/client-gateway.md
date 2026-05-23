# Client-Gateway

## 定位

本文档只定义 `client <-> gateway` 的业务协议。

它不定义：

- rust-cli 如何组织连接池和盘对象
- gateway 内部如何保存 route / grant / session
- `SessionOpen` 失败时当前项目采用什么 reject 策略

## 基础约定

- 除特别说明外，所有多字节整数使用 `big-endian`
- `disk_id` 采用 `ASCII[16]`
- 变长字节字段统一采用 `u16be len + bytes[len]`
- 保留字段发送方必须写 `0`
- 保留字段接收方必须校验为 `0`

## 通用业务头

所有 `client-gateway` payload 都以固定头开始。

### 布局

固定长度：

```text
24 bytes
```

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 1 | `protocol_version` | `u8` | 当前固定 `1` |
| `1` | 1 | `header_len` | `u8` | 当前固定 `24` |
| `2` | 1 | `op_code` | `u8` | 命令码 |
| `3` | 1 | `flags` | `u8` | response / notice 标志 |
| `4` | 2 | `status_code` | `u16` | request 时固定 `0` |
| `6` | 2 | `reserved` | `u16` | 固定 `0` |
| `8` | 8 | `request_id` | `u64` | 请求配对标识 |
| `16` | 8 | `session_id` | `u64` | session-scoped 命令使用 |

### flags

| bit | 名称 | 含义 |
| --- | --- | --- |
| `0` | `FLAG_RESPONSE` | response |
| `1` | `FLAG_NOTICE` | notice |

约束：

- request：两个 bit 都为 `0`
- response：只允许 `FLAG_RESPONSE`
- notice：只允许 `FLAG_NOTICE`
- 未知 bit 置位视为协议错误

### `request_id`

- request 必须非 `0`
- 同一条 connection 上，在飞 request 的 `request_id` 必须唯一
- response 必须原样回显 request 的 `request_id`
- notice 的 `request_id` 固定为 `0`

### `session_id`

- `AuthStart / AuthFinish / SessionOpen / ConnHeartbeat` 固定为 `0`
- `SessionDescribe / ReadAt / WriteAt` 必须带有效非 `0` `session_id`
- `SessionOpen` 成功响应在响应头写入新分配的 `session_id`
- `SessionCloseNotice` 在 notice 头中写目标 `session_id`

## 业务对象与字段边界

协议中四个常见标识的出现位置固定如下：

| 标识 | 所在位置 | 说明 |
| --- | --- | --- |
| `connection` | transport 连接本身 | 不在头里编码 |
| `request_id` | 通用头 | request-response 配对 |
| `auth_id` | `SessionOpen` body | 显式授权对象 |
| `session_id` | 通用头 | 已打开会话标识 |

`disk_id` 只出现在 `AuthStart` body 中，不在通用头中出现。

`ReadAt / WriteAt` 的请求与响应 body 细节见 [data-plane](data-plane.md)。

## 操作码

| 码值 | 名称 | 方向 |
| --- | --- | --- |
| `0x01` | `AuthStart` | req/resp |
| `0x02` | `AuthFinish` | req/resp |
| `0x03` | `SessionOpen` | req/resp |
| `0x04` | `SessionDescribe` | req/resp |
| `0x05` | `SessionCloseNotice` | notice |
| `0x10` | `ReadAt` | req/resp |
| `0x11` | `WriteAt` | req/resp |
| `0x12` | `ConnHeartbeat` | req/resp |

## 核心状态码

### 通用成功

| 码值 | 含义 |
| --- | --- |
| `0x0000` | `OK` |

### 协议错误

| 码值 | 含义 |
| --- | --- |
| `0x1001` | 协议版本错误 |
| `0x1002` | 业务头非法 |
| `0x1003` | body 非法 |
| `0x1004` | 请求与当前 connection 状态不匹配 |
| `0x1005` | 不支持的 op |

### 认证与授权

| 码值 | 含义 |
| --- | --- |
| `0x1101` | 认证失败 |
| `0x1102` | challenge 过期 |
| `0x1103` | challenge 非法或不属于当前 connection |
| `0x1104` | `auth_id` 非法、已消费或已撤销 |
| `0x1105` | `auth_id` 过期 |

### 会话与 I/O

| 码值 | 含义 |
| --- | --- |
| `0x1201` | session 不存在、已关闭或 route 已失效 |
| `0x1301` | 读写越界 |
| `0x1302` | 超过 `max_io_bytes` |
| `0x1303` | 只读盘写入 |
| `0x1304` | 远端 I/O 失败 |

补充口径：

- `docs/network` 不为 `SessionOpen` 定义统一失败业务语义
- 若某实现使用额外 session/open reject 状态码，它的业务含义由实现文档定义

## 建会话前互斥规则

固定规则：

- 一条 connection 同时最多一个 auth 过程在飞
- 一条 connection 同时最多一个 `SessionOpen` 过程在飞
- auth 过程与 `SessionOpen` 过程互斥
- 已签发 `auth_id` 可以并存
- 已打开 session 可以并存
- 已打开 session 上的 `SessionDescribe / ReadAt / WriteAt` 不受 auth/open lane 互斥影响

## AuthStart

### 请求 body

固定长度 `16` 字节：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| `0` | 16 | `disk_id` |

### 成功响应 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 1 | `algo_version` | `u8` |
| `1` | 2 | `ttl_seconds` | `u16` |
| `3` | 16 | `salt` | `bytes[16]` |
| `19` | 2 | `challenge_token_len` | `u16` |
| `21` | `N` | `challenge_token` | `bytes[N]` |

约束：

- `challenge_token` 对 client 透明
- token 绑定当前 connection
- token 只能服务一次 auth 流程

## AuthFinish

### 请求 body

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| `0` | 2 | `challenge_token_len` |
| `2` | `N` | `challenge_token` |
| `2 + N` | 64 | `proof` |

### 成功响应 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `auth_id` | `u64` |

## SessionOpen

### 请求 body

固定长度 `8` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `auth_id` | `u64` |

### 成功响应

协议层最小成功结果固定为：

- `status_code = OK`
- 响应头 `session_id = new_session_id`

成功响应体允许携带附加负载，但 `docs/network` 不定义其业务语义。

### 失败响应

协议层只定义“open 失败”这一事实，不定义：

- 失败类型分类
- 失败 body 结构
- 某个失败码是否表示可重试

这些都属于具体实现策略。

## SessionDescribe

### 请求

- header `session_id` 为目标 session
- body 固定为空

### 成功响应 body

固定长度 `16` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `disk_size_bytes` | `u64` |
| `8` | 4 | `max_io_bytes` | `u32` |
| `12` | 2 | `flags` | `u16` |
| `14` | 2 | `reserved` | `u16` |

当前定义的 flag：

- bit0：只读

## ConnHeartbeat

边界固定如下：

- 方向：`client -> gateway`
- `session_id = 0`
- request body 为空
- success response body 为空

它只表示 connection 级保活，不表示某个 session 保活。

若实现把 `ConnHeartbeat` 超时判定为 connection 死亡，这属于允许实现策略。

## SessionCloseNotice

### 主动发起边界

在这条边上，协议层允许：

- `client -> gateway`
- `gateway -> client`

主动发出 `SessionCloseNotice`。

当前项目的最小闭环实现只要求这两个方向，不要求其他附加 close 分支。

### notice body

固定长度 `2` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 2 | `reason_code` | `u16` |

### reason code

| 码值 | 含义 |
| --- | --- |
| `1` | route lost |
| `2` | gateway shutdown |
| `3` | upstream session closed |
| `4` | client connection replaced |
| `5` | normal close |
| `6` | protocol error |

### 语义

- `SessionCloseNotice` 是这条边上的唯一正式 close 语义
- notice 一旦发出，发送方不等待回复
- notice 一旦送达，就表示目标 session 已经失效
- 若目标 session 已不存在，接收方按幂等忽略
