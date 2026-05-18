# Transport

## 定位

本文档只定义 bootstrap 与 framed transport 的协议边界，不定义认证、路由、会话和宿主策略。

两条网络边共用同一套 transport：

- `client <-> gateway`
- `gateway <-> storer`

区别只在 bootstrap。

## Client-Gateway Bootstrap

`client -> gateway` 的顺序固定为：

```text
TCP
  -> HelloRequest
  -> HelloResponse
  -> optional next layer
  -> framed transport
  -> client-gateway business protocol
```

这里的 `optional next layer` 为 bootstrap 完成后决定是否切换的下一层能力，例如 TLS。是否启用、如何启用，由 server 侧 bootstrap profile 决定。

### Hello 帧头

`Hello` 不属于 transport 帧，它自带独立头部：

```text
12 bytes
```

| 偏移 | 长度 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | 4 | `magic` | `bytes[4]` | 固定 `YDHL` |
| `4` | 1 | `version` | `u8` | 当前固定 `1` |
| `5` | 1 | `message_type` | `u8` | `1=request`, `2=response` |
| `6` | 2 | `status` | `u16` | request 固定 `0` |
| `8` | 4 | `body_len` | `u32` | body 长度，大端 |

body 最大 `65536` 字节。

### Hello 消息体边界

`Hello` 的消息体在协议层保持为 bootstrap payload：

- request body 是 client bootstrap payload
- response body 是 server bootstrap payload
- response body 也可以被视为 `server_capabilities`

`docs/network` 只定义它们是 bootstrap 负载，不解释当前项目具体填什么内容。

当前项目实际采用的 payload 内容见 [Server 实现文档](../server/README.md)。

### Hello 状态码

| 码值 | 含义 |
| --- | --- |
| `0` | `OK` |
| `1` | 协议版本不匹配 |
| `2` | bootstrap 请求非法 |

### Bootstrap 错误边界

以下错误不进入 transport：

- `Hello` 头非法
- `Hello` 版本不匹配
- 在完成 bootstrap 前就发送业务帧
- 在 bootstrap 要求切下一层时直接发送 transport 帧

## Gateway-Storer Bootstrap

`gateway -> storer` 的顺序固定为：

```text
TCP
  -> framed transport
  -> gateway-storer business protocol
```

当前协议族在这条边上不定义 `Hello`。
也不定义独立 TLS 握手步骤。

这意味着：

- 一旦 TCP 建立，第一条上层消息就进入 framed transport
- transport 之后的第一条业务消息通常是 `StorerRegister`

## Framed Transport

bootstrap 完成后进入统一 transport：

```text
frame = u16be payload_size_m1 + payload[payload_size_m1 + 1]
```

约束：

- 长度头 `2` 字节，大端
- payload 实际长度范围为 `1..65536`
- 不存在空 payload

## Transport 职责

transport 只负责：

1. 读取长度头
2. 读取完整 payload
3. 把完整 payload 交给上层
4. 把上层给出的完整 payload 写回对端

它不负责：

- 请求响应配对
- `auth_id / session_id` 解释
- 心跳决策
- metadata 语义
- 多帧业务重组

## 并发要求

transport 必须支持一条连接上的全双工并发收发：

- 可以连续发送多个 request
- response 允许乱序返回
- notice 可以与 response 交错到达

乱序与配对由业务层 header 处理，不由 transport 决定。
