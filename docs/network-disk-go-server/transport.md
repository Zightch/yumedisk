# 传输层协议

## 1. 文档定位

本文档只定义 `network-disk-go-server` 的底层传输层协议。

职责范围：

- TCP 字节流拆帧
- 单帧长度编码
- 完整 payload 交付

明确不负责：

- 认证
- 路由
- 会话
- 命令语义
- 偏移和长度字段
- 错误码
- 请求响应配对

这些内容全部属于上层业务协议，不在本文档中定义。

## 2. 分层原则

所有连接统一走 `TCP`。

协议固定分两层：

- 传输层：只负责拆帧
- 业务层：完整承载在传输层 payload 中

传输层不承载任何业务语义。

## 3. 传输层对象的位置

传输层对象的本质是：

```text
transport runtime = framed TCP connection
```

也就是：

- 下层持有一个真实 `TCP` 连接对象
- 上层暴露“完整 payload 收发”能力

### 3.1 在 client 侧的位置

当前 client 侧结构应理解为：

```text
client[
  N * NetworkMedia(storer session)
    <- 并发抢 ->
  Q * GatewayConnection(
        transport object(
          TCP object
        )
      )
]
```

含义：

- `NetworkMedia` 不直接持有裸 `TCP`
- `NetworkMedia` 通过 `DiskSession` 复用某个 `GatewayConnection`
- `GatewayConnection` 内部持有传输层对象
- 一个传输层对象对应一条真实 `TCP` 连接
- 多个 `NetworkMedia` 可以并发抢同一个 `GatewayConnection`
- 并发抢的本质是：多个业务请求并发复用同一条 transport/TCP

### 3.2 在 gateway 侧的位置

当前第一版先以 `embedded gateway storer` 为准，传输层对象在服务端内部的位置应理解为：

```text
storer(embedded gateway)
  -> listener
    -> accepted client TCP
      -> transport object
      -> client-and-gateway business handler
      -> local storer runtime
```

含义：

- 传输层对象位于 client 外部连接入口之后
- 上面挂 `client-and-gateway` 业务协议处理器
- 再往内才进入本地 `storer` 数据面
- 传输层是可复用底座，不是协议边界本身

后续如果再做独立 `gateway` 与 `storer` 分离部署，可以在另一处入口再挂第二套业务协议，但这不属于当前第一版实现前提。

## 4. 帧格式

传输层帧定义：

```text
frame = u16be payload_size_m1 + payload[payload_size_m1 + 1]
```

说明：

- `payload_size_m1` 为 2 字节无符号整数，按大端编码
- payload 实际长度为 `payload_size_m1 + 1`
- 取值 `0x0000` 表示 payload 实际长度为 `1`
- 取值 `0xFFFF` 表示 payload 实际长度为 `65536`

因此传输层数据区长度恒为：

```text
1..65536 bytes
```

不存在空包。

## 5. 字节序

传输层长度头固定使用 `big-endian`。

## 6. 传输层职责边界

传输层只做三件事：

1. 从 TCP 字节流中读取 2 字节长度头
2. 计算本帧 payload 实际长度
3. 继续读取完整 payload，并把整帧交给上层业务层

传输层明确不做：

- 鉴权
- 盘路由
- 压缩
- 加密
- 校验和
- 多帧重组
- continuation
- 业务分片

## 7. 解析口径

单帧解析步骤：

1. 读取 2 字节 `payload_size_m1`
2. 计算 `payload_size = payload_size_m1 + 1`
3. 再读取 `payload_size` 字节
4. 将完整 payload 交给上层业务层

如果连接在长度头或 payload 未收完整前断开，则视为连接异常并直接终止当前连接。

## 8. 并发承载职责

传输层虽然不理解业务，但它必须承担并发承载能力。

这里的“承载并发”含义不是做业务配对，而是保证下层字节流与完整帧交付能够支撑上层并发。

传输层必须满足：

1. 一条连接支持持续全双工收发
2. 不要求请求发一个回一个的锁步模型
3. 允许上层连续写入多个完整 payload
4. 允许上层连续收到多个完整 payload
5. 不因“这帧属于哪个请求”而介入业务判断

也就是说：

- 业务层负责：
  - 请求编号
  - 响应配对
  - 会话路由
  - 并发调度策略
- 传输层负责：
  - 把并发压力承载在稳定的 framed TCP 通道上
  - 不破坏 payload 边界
  - 不把连接收发退化成串行锁步

对当前模型的直接作用：

- client 侧多个 `NetworkMedia` 抢同一条 `GatewayConnection` 时，竞争首先落在 transport/TCP 承载面
- gateway 侧多个已接入连接并发收发时，每条连接各自由对应 transport runtime 承载
- 传输层是并发承载底座，但不是并发决策层

## 9. 包粒度与缓冲区

当前上限：

- 单帧 payload 最大 `65536` 字节

这个上限的意义：

- 颗粒明确
- 缓冲区上限明确
- 便于两端做固定 buffer 复用
- 不需要额外引入更复杂的传输层分片协议

第一版工程建议：

- 每个连接至少准备可容纳 `65538` 字节的收包缓冲区
  - `2` 字节长度头
  - `65536` 字节 payload
- 收发缓冲区可通过池复用

这个上限同时服务于并发承载：

- 避免单个超大 payload 长时间占住连接
- 让同一条连接上的业务请求更容易交错推进
- 让内存占用上界保持明确

## 10. 对上层业务协议的要求

传输层只提供“单帧 payload 交付”能力。

因此上层业务协议必须自行处理：

- 业务头
- 消息类型
- 请求响应配对
- 错误语义
- 是否允许并发
- 大块 I/O 的主动拆分

当前正式业务层协议文档：

- [client-and-gateway.md](client-and-gateway.md)
- `gateway-and-storer.md` 后续独立定义
