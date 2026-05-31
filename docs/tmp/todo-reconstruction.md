# gateway / 网络协议重构执行清单

## 0. 当前范围

本清单只处理一件事：

- 在正式做 `ReadAt / WriteAt` 压缩之前，先把 `client <-> gateway <-> storer` 的会话与数据面职责重构干净

这里讨论的重点包括：

- gateway 职责收口
- `SessionDescribe / ReadAt / WriteAt` 代理化
- `SessionCloseNotice / SessionDataChangedNotice` 桥接化
- `whole` 本地导出与远端 `storer` 统一到同一套 route proxy 抽象

这里明确不做：

- 传输压缩本身
- transport framing 重写
- auth 算法变化
- 旧版本兼容
- TLS 或 bootstrap 扩展
- route 选择策略重写

本清单完成后，`docs/tmp/todo-network-compression.md` 才进入正式实施阶段。

## 1. 当前总目标

按最小核心闭环，把 gateway 与网络协议先收成下面这几个结论：

1. gateway 不是纯控制面，也不是纯透明代理，而是混合模型
2. 控制面职责继续保留在 gateway
3. `SessionDescribe / ReadAt / WriteAt` 改成 opaque data proxy
4. `SessionCloseNotice / SessionDataChangedNotice` 改成 lifecycle notice bridge
5. 数据面 body 演进不再要求 gateway 跟着解析和重组
6. 后续压缩能力只落在 client 与 storer，不再把 gateway 变成压缩参与方

## 2. 为什么要先重构

### 2.1 当前 gateway 仍在解释数据面

当前 gateway 在 client-facing 入口会先解析：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`

然后转成内部语义方法调用，再在 storer-facing 一侧重组新请求。

这带来的直接问题是：

- `ReadAt / WriteAt` body 只要一变，gateway 就必须同步改解析器
- `SessionDescribe` response body 只要一变，gateway 也必须同步改解析器
- 压缩字段无法只由 client / storer 自己处理

### 2.2 当前 gateway 还在翻译上游状态

当前 route 侧返回的 `status_code` 会被先映射成内部错误，再映射回 client-facing `status_code`。

这会造成：

- 上游状态语义被压扁
- 新状态码扩展需要 gateway 继续改映射表
- 数据面协议演进被 gateway 卡住

### 2.3 当前 close notice body 也没有真正透传

当前 client 发来的 `SessionCloseNotice` body 只被拿来做校验，gateway 转给 storer 时并不保留原始 `reason_code`。

这说明问题不只在 `ReadAt / WriteAt`：

- 连 notice body 语义也被 gateway 吞掉了

### 2.4 当前抽象天然鼓励 gateway 做语义层

现在 `gateway` 依赖的是一套语义接口：

- `Open`
- `Describe`
- `Read`
- `Write`
- `Close`

这套接口本身就要求 gateway 在上层先把协议 body 解释出来。

因此如果不先改抽象，后面继续做压缩或 body 扩展，gateway 迟早还会被拉回解释层。

## 3. 重构后的正式模型

### 3.1 控制面保留在 gateway

下面这些职责继续明确属于 gateway：

- `AuthStart / AuthFinish`
- `SessionOpen`
- client connection ownership 校验
- gateway session id 分配
- gateway session id 与 upstream session id 映射
- route 可用性判断
- client-facing `ConnHeartbeat`
- gateway-storer `LinkHeartbeat`
- route 断开后的本地收束
- client 断开后的本地收束

固定口径：

- 这些能力不是透传能解决的问题
- 它们就是 gateway 的正式职责

### 3.2 数据面改为 opaque proxy

下面这些操作改成 opaque data proxy：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`

“opaque” 的固定含义是：

- gateway 仍然校验通用 header 合法性
- gateway 仍然校验 session ownership
- gateway 仍然把 gateway session id 映射到 upstream session id
- gateway 仍然改写 request id 以适配 route connection

但除此之外：

- gateway 不再解析这些 op 的 body
- gateway 不再重建这些 op 的业务 body
- gateway 不再翻译这些 op 的 `status_code`
- gateway 不再决定这些 op 的 payload 形状

### 3.3 notice 作为独立桥接面

下面这些 notice 不归到纯控制或纯数据任一侧，而单独定义为 lifecycle notice bridge：

- `SessionCloseNotice`
- `SessionDataChangedNotice`

固定口径：

- 它们不是普通 request/response 数据面
- 也不只是 auth/open 这类控制流程
- 它们负责在 session 生命周期边界上传递事实

### 3.4 gateway 是混合模型

因此本轮重构后的正式表述应当是：

- gateway 保留控制面
- gateway 代理共享数据面
- gateway 桥接生命周期 notice

不再把 gateway 简化归类为单一角色。

## 4. 固定边界

### 4.1 数据面代理的最小职责

对 `SessionDescribe / ReadAt / WriteAt`，gateway 只允许做下面这些事：

1. 解析并校验通用 header
2. 校验当前 gateway session 是否属于该 client connection
3. 查表得到：
   - `route_connection_id`
   - `upstream_session_id`
4. 改写发往上游的：
   - `request_id`
   - `session_id`
5. 等待 route 响应
6. 把 route 返回的：
   - `status_code`
   - `body`
   原样带回 client-facing response
7. 把 client-facing response 的：
   - `request_id`
   - `session_id`
   改回 client 视角

### 4.2 数据面代理明确不做

对 `SessionDescribe / ReadAt / WriteAt`，gateway 明确不再做：

- `ParseSessionDescribeResponseBody`
- `ParseReadBody`
- `ParseReadWriteBody`
- 读取 `ReadAt` response body 内部结构
- 重建 `WriteAt` request body
- 重建 `ReadAt` response body
- route `status_code -> internal error -> client status_code` 双重映射
- 压缩决策
- 解压后再压

### 4.3 close notice 的双来源

`SessionCloseNotice` 固定允许两类来源：

#### A. 转发来源

例如：

- client 主动关闭 session
- storer 主动关闭 session

此时 gateway 的职责是：

- 校验 session 所属关系
- 改写 session id
- 原样转发 notice body

#### B. gateway 合成来源

例如：

- route lost
- gateway 本地协议错误
- client connection 死亡
- gateway 主动替换 client connection

此时 gateway 的职责是：

- 本地关闭映射
- 生成合适的 `reason_code`
- 向对侧补发 `SessionCloseNotice` 或向本地宿主投递 close 事实

### 4.4 `SessionDataChangedNotice` 的固定口径

`SessionDataChangedNotice` 当前固定做桥接：

- `storer -> gateway -> client`

gateway 在这条链上只做：

- upstream session id 到 gateway session id 的映射
- 目标 client connection 定位

当前不做：

- body 解释
- 聚合多个 notice
- 变成普通 response

### 4.5 本轮不做旧版兼容

固定前提：

- `client`
- `gateway`
- `storer`
- `whole`

一起升级。

因此本轮不做：

- 新旧双栈并存
- 按 peer 版本动态切换 gateway 行为
- 旧语义接口兼容壳

## 5. 新的接口模型

### 5.1 删除现有语义数据面抽象

当前这类语义接口应该退出 gateway 数据面主线：

- `Describe(...)`
- `Read(...)`
- `Write(...)`

原因：

- 它们要求 gateway 先解释协议 body
- 它们让远端 storer 和本地 whole 都被迫暴露“语义读写接口”
- 它们会持续阻碍协议 body 演进

### 5.2 新的 route proxy 抽象

gateway 数据面对上游 route 的正式抽象，建议收成：

```go
type RouteSessionProxy interface {
    Open(connectionID uint64, entry route.Entry) (uint64, error)

    RoundTrip(
        routeConnectionID uint64,
        upstreamSessionID uint64,
        opCode uint8,
        requestBody []byte,
    ) (statusCode uint16, responseBody []byte, err error)

    SendNotice(
        routeConnectionID uint64,
        upstreamSessionID uint64,
        opCode uint8,
        noticeBody []byte,
    ) error

    CloseConnection(connectionID uint64)
}
```

固定口径：

- `RoundTrip` 是 route-facing raw proxy 能力
- `requestBody / responseBody` 都是 opaque bytes
- `statusCode` 直接来自上游 route response
- gateway 不通过这个接口暴露 `Read / Write / Describe`

### 5.3 `whole` 本地导出的定位

`whole` 本地导出不是网络 peer，但也应当实现同一套 `RouteSessionProxy` 抽象。

固定口径：

- gateway 上层只看到统一的 route proxy 接口
- 本地导出可以在接口实现内部继续直连 `session.Service`
- 但这种语义解释必须留在 adapter 内部，而不是暴露给 gateway 主流程

这意味着：

- `LocalAdapter` 可以继续内部调用 `Open / Describe / Read / Write`
- 但 gateway 不能再依赖 `LocalAdapter` 暴露这些语义方法

## 6. 协议处理口径

### 6.1 `SessionOpen`

`SessionOpen` 继续属于控制面。

gateway 需要继续负责：

- `auth_id -> disk_id`
- `disk_id -> route`
- 向 route 打开 upstream session
- 建立 gateway session 映射

本轮不把 `SessionOpen` 改成透传。

### 6.2 `SessionDescribe`

`SessionDescribe` 改成 route proxy：

- gateway 不再解析 response body
- gateway 不再重建 metadata body
- 上游返回什么 body，就带什么 body 回 client

### 6.3 `ReadAt`

`ReadAt` 改成 route proxy：

- gateway 不再解析 request body
- gateway 不再解析 response body
- 上游返回的 `status_code + body` 原样传给 client

这样后续即使 `ReadAt response` 变成：

- `compress:u8 + payload`

gateway 也无需改动。

### 6.4 `WriteAt`

`WriteAt` 改成 route proxy：

- gateway 不再解析 request body
- gateway 不再重建下游 write body
- 上游返回的 `status_code` 原样传给 client

这样后续即使 `WriteAt request` 变成：

- `offset + length + compress + payload`

gateway 也无需改动。

### 6.5 `SessionCloseNotice`

固定规则如下：

- client 主动发 close：gateway 校验 ownership 后，原样保留 body，转发给 storer
- storer 主动发 close：gateway 改写 session id 后，原样保留 body，转发给 client
- route 直接断开没发 close：gateway 本地合成 close
- client connection 直接死亡：gateway 本地收束 upstream session，并按策略决定是否补发 close

### 6.6 `SessionDataChangedNotice`

固定规则如下：

- notice body 保持 opaque
- 当前 body 为空，但 gateway 不把它硬编码为“必须由语义层构造”
- gateway 只做 session id 映射和目标连接定位

## 7. 失败路径模型

### 7.1 route lost

发生 route lost 时：

- gateway 关闭该 route 下全部 gateway session 映射
- 向对应 client 发 `SessionCloseNotice(route lost)`

这仍然属于 gateway 的正式职责，不属于数据面透传。

### 7.2 client disconnect

发生 client disconnect 时：

- gateway 清理该 client connection 拥有的全部 gateway session
- 尝试向上游发 close 或直接本地收束

固定要求：

- 不把“client 已死”这类事实交给 storer 侧自己猜

### 7.3 协议错误

若某个 client-facing request：

- header 非法
- flags 非法
- session ownership 非法

则 gateway 仍可在本地拒绝，不进入 route proxy。

但一旦进入 route proxy：

- 对于上游返回的业务 `status_code`
- gateway 不再做语义翻译

### 7.4 上游 session 不可用

若 route 返回：

- `SessionUnavailable`
- `IOOutOfRange`
- `IOLarge`
- `IOReadOnly`
- `IOFailed`

则 gateway 直接把相同 `status_code` 返回给 client。

不再经过中间错误映射层。

## 8. 代码侧需要做的重构

### 8.1 `server/internal/gateway/client`

需要修改：

- `SessionDataPlane` 语义接口退出数据面主线
- `HandleDescribe / HandleRead / HandleWrite` 改成统一的 proxy helper
- `HandleSessionCloseNotice` 改成保留 notice body 的桥接逻辑

固定要求：

- `Describe / Read / Write` 不再调用语义化 `sessions.Describe / Read / Write`
- 这三类 op 的 body 在 gateway 主流程中保持 opaque bytes

### 8.2 `server/internal/gateway/storer`

需要修改：

- `DataPlane()` 改成提供 route proxy 能力，而不是语义数据面
- `roundTripData(...)` 升级成 raw `RoundTrip(...)`
- `Close(...)` 升级成 raw `SendNotice(...)`

固定要求：

- route 返回的 `status_code` 不再映射成内部错误
- route 返回的 response body 不再在 gateway-storer 层解析

### 8.3 `server/internal/storer/gateway/local_adapter.go`

需要修改：

- `LocalAdapter` 实现 route proxy 抽象
- gateway 不再直接依赖它的 `Describe / Read / Write`

固定要求：

- 允许 `LocalAdapter` 在内部解释语义
- 但这只是 adapter 的实现细节，不再暴露给 gateway 主流程

### 8.4 session registry

当前 session registry 基本模型可保留：

- gateway session id
- client connection id
- route connection id
- upstream session id

需要继续承接的职责：

- ownership 校验
- route session 到 gateway session 的反查
- route lost / client disconnect 时批量清理

本轮不要求重写整个 registry。

### 8.5 runtime notice 发射

`gateway/runtime.go` 里的：

- `NotifySessionClosed`
- `NotifySessionDataChanged`

整体方向可保留。

需要确认的只是：

- 上游主动 notice 与 gateway 合成 notice 的边界更清楚
- close body 不再在中途被重写成固定 `normal close`

## 9. 与压缩工作的关系

本清单是 `todo-network-compression.md` 的前置任务。

顺序固定为：

1. 先完成本清单
2. 确认 `SessionDescribe / ReadAt / WriteAt` 已经 opaque proxy 化
3. 确认 `SessionCloseNotice / SessionDataChangedNotice` 已经 bridge 化
4. 再进入压缩面工作

这样做的直接收益是：

- `ReadAt response = compress:u8 + payload` 时，gateway 不需要改
- `WriteAt request = offset + length + compress + payload` 时，gateway 不需要改
- client 与 storer 可以独立决定数据面 body 形状

## 10. 分阶段执行建议

### Phase A. 文档与口径冻结

需要完成：

- 重写 `docs/tmp/todo-reconstruction.md`
- 在网络协议文档里明确 gateway 的混合模型表述
- 明确压缩工作依赖本清单先完成

完成标准：

- gateway 职责边界已经书面定案
- 不再把 gateway 误表述为纯控制面或纯透明代理

### Phase B. route proxy 抽象落地

需要完成：

- 新增 `RouteSessionProxy` 或等价抽象
- 退掉 gateway 主流程上的语义 `Describe / Read / Write` 依赖
- `whole/local adapter` 与 remote storer 都挂到这套抽象下

完成标准：

- gateway 主流程不再依赖语义数据面接口

### Phase C. client-facing 数据面代理化

需要完成：

- `HandleDescribe`
- `HandleRead`
- `HandleWrite`

三者改成统一 proxy helper。

完成标准：

- gateway 不再解析这三类 op 的 body
- gateway 不再重建这三类 op 的业务 body

### Phase D. storer-facing route proxy 化

需要完成：

- route raw request 发送
- route raw response 返回
- `status_code` 直通
- `SendNotice` 实现

完成标准：

- route 业务响应不再经过 gateway 状态映射层

### Phase E. notice bridge 收口

需要完成：

- client -> storer close notice 原样保留 body
- storer -> client close notice 原样保留 body
- route lost / client disconnect / protocol error 的合成 close 逻辑收清
- `SessionDataChangedNotice` 保持 session 映射桥接

完成标准：

- close notice 的“转发来源”和“gateway 合成来源”边界清楚

### Phase F. `whole` 兼容路径收口

需要完成：

- `LocalAdapter` 挂接 route proxy 抽象
- `whole` 运行模式在 proxy 化后仍然通过现有集成测试

完成标准：

- 本地 whole 路径与远端 storer 路径对 gateway 呈现相同的代理模型

### Phase G. 测试补齐

至少补下面这些测试：

- `SessionDescribe` response body 透传
- `ReadAt` response body 透传
- `WriteAt` request body 透传
- 上游未知或扩展 `status_code` 的直通行为
- client close reason 原样保留
- route lost 时 gateway 合成 close
- `SessionDataChangedNotice` 的 session 映射桥接
- `whole` 本地导出路径在 proxy 模型下继续成立

完成标准：

- 压缩工作需要的“gateway 不碰数据面 body”前提已被测试锁住

## 11. 当前建议结论

本清单当前建议固定为：

- gateway 正式模型定义为：
  - 控制面保留
  - 数据面代理
  - notice 桥接
- `SessionDescribe / ReadAt / WriteAt` 从 gateway 语义层退出，改为 opaque proxy
- `SessionCloseNotice / SessionDataChangedNotice` 单列为 lifecycle notice bridge
- `whole/local adapter` 与 remote storer 统一收口到 route proxy 抽象
- 压缩工作必须以后置方式依赖本清单

这样收完后，后续真正做压缩时，gateway 才不会继续成为协议演进阻塞点。
