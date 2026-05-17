# 传输层协议

## 1. 文档定位

本文档只定义 transport 层，不定义 bootstrap、TLS、认证、会话和数据面业务语义。

职责边界固定如下：

- `bootstrap`
  - `TCP`
  - `Hello`
  - 可选 `TLS`
- `transport`
  - framed payload
- `business protocol`
  - 认证
  - 会话
  - metadata
  - 数据面

## 2. 分层顺序

当前正式顺序固定为：

```text
TCP
  -> Hello
  -> optional TLS
  -> transport
  -> business protocol
```

因此：

- `Hello` 不属于 transport
- TLS 握手不属于 transport
- transport 只在 bootstrap 完成后才启动

## 3. transport 的本质

transport runtime 的本质是：

```text
framed duplex byte stream
```

也就是：

- 下层是一条已经完成 bootstrap 的双工字节流
- 上层得到完整 payload 的收发能力

transport 不理解任何业务语义。

## 4. client 侧位置

当前 client 侧结构应理解为：

```text
NetworkMedia
  -> DiskSession
    -> GatewayConnection
      -> transport runtime
        -> stream(TCP or TLS-over-TCP)
```

含义：

- `NetworkMedia` 不直接持有 transport
- `DiskSession` 不直接持有裸 TCP
- `GatewayConnection` 内部持有 transport runtime
- 多个 session 并发复用同一条 `GatewayConnection`

## 5. server 侧位置

### 5.1 gateway

```text
client listener
  -> accepted stream
  -> bootstrap
  -> transport runtime
  -> client-and-gateway business handler

storer listener
  -> accepted stream
  -> transport runtime
  -> gateway-and-storer handler
```

说明：

- client-facing 入口需要 bootstrap
- storer-facing 当前也可以直接走 transport；后续若要补 bootstrap，应整体一致定义

### 5.2 whole

```text
whole
  -> client listener
  -> bootstrap
  -> transport runtime
  -> client-and-gateway business handler
  -> local storer core
```

`whole` 只是部署形态不同，不改变 transport 语义。

## 6. 帧格式

transport 帧定义保持：

```text
frame = u16be payload_size_m1 + payload[payload_size_m1 + 1]
```

其中：

- 长度头 2 字节，大端
- payload 实际长度范围 `1..65536`
- 不存在空包

## 7. 字节序

transport 长度头固定使用 `big-endian`。

## 8. transport 的唯一职责

transport 只做三件事：

1. 读取 2 字节长度头
2. 读取完整 payload
3. 把完整 payload 交给上层业务层

transport 明确不做：

- 认证
- route
- 会话
- 心跳决策
- 错误码解释
- 多帧业务重组
- 压缩
- 加密

## 9. 并发承载

transport 必须支撑一条连接上的全双工并发收发。

它必须满足：

1. 上层可连续写入多个 payload
2. 上层可连续接收多个 payload
3. 响应允许乱序返回
4. 不要求锁步 request-response

transport 只承载并发，不做并发业务决策。

## 10. 包大小上限

当前单帧 payload 最大 `65536` 字节。

这个上限的直接意义：

- buffer 上界明确
- 大块 I/O 必须由业务层主动切片
- 避免单帧无限膨胀

因此业务层必须自己处理：

- `max_io_bytes`
- 大读写拆片
- 多请求合并结果

## 11. bootstrap 与 transport 的错误边界

以下情况属于 bootstrap 错误，不进入 transport：

- 未完成 `Hello` 就发送业务帧
- server 要求 TLS，但 client 未切 TLS 就开始发 transport 帧
- TLS 握手阶段混入业务字节

以下情况属于 transport 错误：

- 长度头损坏
- payload 未收完整即连接断开

这两类错误都应直接导致 connection 终止。

## 12. 对上层协议的要求

transport 只提供“完整 payload 收发”。

因此上层业务协议必须自行处理：

- 业务头
- 消息类型
- 请求响应配对
- notice 分发
- `auth_id`
- `session_id`
- metadata
- 数据面错误语义
