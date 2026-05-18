# 传输层协议

## 1. 文档定位

本文档只定义 transport 层，不定义认证、授权、会话和数据面业务语义。

当前正式结构有两条边：

- `client <-> gateway`
- `gateway <-> storer`

它们共用同一套 framed transport，但 bootstrap 规则不同。

## 2. 分层顺序

### 2.1 client-facing

`client -> gateway` 的正式顺序固定为：

```text
TCP
  -> HelloRequest
  -> HelloResponse(server_capabilities)
  -> optional TLS
  -> transport
  -> client-gateway business protocol
```

因此：

- `Hello` 不属于 transport
- TLS 握手不属于 transport
- transport 只在 client bootstrap 完成后才启动

当前第一版固定为：

- `HelloRequest` 当前负载为空
- `HelloResponse` 当前 `server_capabilities` 负载为空
- 因当前能力负载为空，`Hello` 完成后直接进入 transport
- 未来若引入 TLS 或其他 server 能力，必须从 `HelloResponse.server_capabilities` 扩展，不再新增第二套 bootstrap 消息

### 2.2 storer-facing

`gateway -> storer` 的正式顺序固定为：

```text
TCP
  -> transport
  -> gateway-storer business protocol
```

当前第一版口径直接定死为：

- storer-facing 不定义 `Hello`
- storer-facing 不定义 TLS 握手步骤
- storer-facing 的第一条业务消息就是 `StorerRegister`

如果后续需要改动这条边，应整体改写正式文档，而不是在正式文档中保留过渡叙述。

## 3. transport 的本质

transport runtime 的本质是：

```text
framed duplex byte stream
```

也就是：

- 下层是一条已经完成各自 bootstrap 的双工字节流
- 上层得到完整 payload 的收发能力

transport 不理解任何业务语义。

## 4. transport 上层

当前 transport 只承接三类上层协议：

1. `client-gateway control plane`
2. `gateway-storer route control plane`
3. `session metadata + shared data plane`

它不区分控制面和数据面，只承接完整 payload。

## 5. client 侧位置

当前 client 侧结构应理解为：

```text
client disk object
  -> opened session
    -> connection runtime
      -> transport runtime
        -> stream(TCP or TLS-over-TCP)
```

含义：

- client disk object 不直接持有 transport
- client disk object 自身只绑定 `disk_id + session + metadata`
- opened session 不直接持有裸 TCP
- connection runtime 内部持有 transport runtime
- 多个 session 并发复用同一条 connection runtime

## 6. gateway 侧位置

```text
client listener
  -> accepted stream
  -> client bootstrap
  -> transport runtime
  -> client-gateway handler

storer listener
  -> accepted stream
  -> transport runtime
  -> storer register / route runtime
  -> gateway-storer handler
```

## 7. whole 侧位置

```text
whole
  -> client listener
  -> client bootstrap
  -> transport runtime
  -> client-gateway handler
  -> embedded gateway core
  -> local fixed route(self disk_id)
  -> local storer core
```

补充约束：

- `whole` 不启动外部 storer-facing listener
- `whole` 不增加第二套 client-facing bootstrap 或 transport 语义
- client 看到的仍然是完整 `Hello -> transport -> auth -> session` 主链

## 8. 帧格式

transport 帧定义保持：

```text
frame = u16be payload_size_m1 + payload[payload_size_m1 + 1]
```

其中：

- 长度头 2 字节，大端
- payload 实际长度范围 `1..65536`
- 不存在空包

## 9. 字节序

transport 长度头固定使用 `big-endian`。

## 10. transport 的唯一职责

transport 只做三件事：

1. 读取 2 字节长度头
2. 读取完整 payload
3. 把完整 payload 交给上层业务层

transport 明确不做：

- 认证
- route
- 会话
- metadata 查询
- 请求响应配对
- notice 解释
- 心跳决策
- 多帧业务重组
- 压缩
- 加密

## 11. 并发承载

transport 必须支撑一条连接上的全双工并发收发。

它必须满足：

1. 上层可连续写入多个 payload
2. 上层可连续接收多个 payload
3. 响应允许乱序返回
4. 不要求锁步 request-response

transport 只承载并发，不做并发业务决策。

## 12. 包大小上限

当前单帧 payload 最大 `65536` 字节。

这个上限的直接意义：

- buffer 上界明确
- 大块 I/O 必须由业务层主动切片
- 避免单帧无限膨胀

因此业务层必须自己处理：

- `max_io_bytes`
- 大读写拆片
- 多请求合并结果

## 13. bootstrap 与 transport 的错误边界

### 13.1 client-facing bootstrap 错误

以下情况属于 client bootstrap 错误，不进入 transport：

- 未完成 `Hello` 就发送业务帧
- server 要求 TLS，但 client 未切 TLS 就开始发 transport 帧
- TLS 握手阶段混入业务字节

### 13.2 storer-facing bootstrap 错误

当前第一版 `role = storer` 的 storer-facing 没有单独 bootstrap 协商阶段。

因此外部 storer-facing 在 transport 之前只有 TCP 建连本身；一旦开始收发，就直接进入 transport。

`role = whole` 不存在外部 storer-facing TCP 边，因此不适用这一节。

### 13.3 transport 错误

以下情况属于 transport 错误：

- 长度头损坏
- payload 未收完整即连接断开

这些错误都应直接导致 connection 终止。

这里的边界同时固定为：

- transport 只上报 connection 终止
- transport 不直接定义上层对象的收束策略

## 14. 对上层协议的要求

transport 只提供“完整 payload 收发”。

因此上层业务协议必须自行处理：

- 业务头
- 消息类型
- 请求响应配对
- notice 分发
- `auth_id`
- `session_id`
- `SessionDescribe(session_id)` metadata 查询
- route/session 映射
- 失效事件上传给各自上层
