# Server 结构重构执行方案

## 1. 背景

当前 `server` 的网络链路、程序边界和正式文档口径已经基本收住：

- 程序固定为：
  - `gateway`
  - `storer(role=storer | whole)`
- 正式协议主线固定为：
  - `Hello -> transport -> auth -> session -> data plane`
- `whole` 的语义已经固定为“自带 gateway 能力的 storer”
- client / server / network 正式文档已经有统一口径

当前问题不在协议模型，而在实现层次还没有完全按开发原则第 5 点摊开。

也就是说：

- 当前不是“要不要重建 server 模型”的问题
- 而是“当前 server 代码结构是否已经真正拆成多层、职责是否已经下沉到位”的问题

本任务单只讨论 `server` 实现结构重构，不改正式协议语义。

## 2. 适用原则

本方案严格按 `docs/development/development-principles.md` 第 5 点执行：

- 重构不是搬文件、不是改名字、不是多包一层壳
- 必须先摊开最小组件，再逐层向上拼装
- 默认优先形成多层结构，不允许只抽出一层就停下
- `Go` 侧依赖必须保持单向下沉，避免反向依赖和职责回流
- 同一语义前缀下的文件/模块一旦成组扩张，优先收成目录，不继续长期平铺 `xxx_yyy.go`

同时继续遵守：

- 第 2 点“激进更新原则”
- 第 3 点“单一真实来源原则”
- 第 6 点“删除优先原则”
- 第 7 点“测试覆盖原则”

## 3. 当前结构问题

当前 `server` 已经比前一版清楚很多，但从结构重构角度看，至少还有下面几类问题没有收完。

### 3.1 gateway 的 route-facing 栈还没有完全分层

当前 `gateway` 的 storer 侧链路里，下面这些职责还没有彻底拆开：

- route 真状态
- 活跃 storer 连接表
- register 闸口
- request/response round-trip
- gateway 主动 heartbeat
- route 断开后的 session 清理与 notice 传播

目前它们仍然集中缠在：

- `server/internal/gateway/storer_routes.go`
- `server/internal/gateway/storer_connection.go`
- `server/internal/gateway/runtime.go`

这说明当前 route-facing 结构还停留在“registry + connection helper + runtime 拼起来”的阶段，还没有真正摊成底层组件。

### 3.2 存在已经失去必要性的空壳层

当前 `server/internal/gateway/storer_handler.go` 与 `gateway.Runtime` 里的 `storerHandler` 已经不是必要层：

- 构造了 `StorerHandler`
- 但真正运行时并没有通过它承接 accepted storer connection
- 代码实际直接走的是 `AttachConnection -> storerConn.serve(...)`

这类结构不符合第 5 点和第 6 点：

- 不是稳定中层
- 也不是明确底层能力
- 而是留下来的过渡壳

当前倾向不是继续强化这个壳，而是删除或重建为真正有职责的中层。

### 3.3 gateway runtime 还承担了过多 route-facing 细节

`server/internal/gateway/runtime.go` 当前同时承担：

- 顶层程序运行时
- listener 管理
- storer accepted connection 生命周期
- storer heartbeat 启停
- storer register 入口调度
- client session close notice 的写回

这里的问题不是文件行数，而是运行时层还在直接碰太多下层细节。

按第 5 点，顶层 runtime 应该只做：

- 顶层装配
- listener 启停
- accepted connection 分发
- 对外统一入口

而不应继续下沉到 route-facing 链路内部流程。

### 3.4 storer 侧运行时也还偏厚

`server/internal/storer/role_runtime.go` 当前同时承担：

- `role=storer` 顶层入口
- dial gateway
- register 报文构造与发送
- watchdog 管理
- transport runtime 启停
- 本地 session connection cleanup

语义上它能工作，但层次上仍然把“顶层装配”和“单条 gateway link 运行时”揉在一起。

### 3.5 listener 层只收了一半

当前 client listener 已经抽成了共享能力：

- `ServeClientListener(...)`

但 storer listener 仍然保留在 `gateway/runtime.go` 手写 accept loop。

这不是功能错误，但说明当前 listener 层的提炼还没有完全一致。

注意：

- 这里不是说一定要做“一个万能 listener”
- 而是要保证 listener 壳这一层本身的职责边界清楚，不再混入业务处理

## 4. 本轮重构目标

本轮目标固定为：

1. 把 `server` 的 route-facing 链路拆成真正的多层结构。
2. 把 `gateway` 顶层 runtime 收回到“装配和调度”定位。
3. 把 `storer` 顶层 runtime 收回到“装配和调度”定位。
4. 删除已经失去必要性的空壳、中转壳和过渡层。
5. 保持协议语义、程序边界和文档口径不变。

## 5. 本轮硬约束

### 5.1 固定不改的正式语义

必须保持与以下文档一致：

- `docs/network/define/`
- `docs/network/server/README.md`

本轮固定不改：

- `gateway` / `storer` 两程序关系
- `role=whole | storer` 语义
- `Hello / transport / auth / session / data plane` 主链
- `ConnHeartbeat` 与 `LinkHeartbeat` 的方向
- `SessionOpen` 的最小闭环语义
- `whole` 固定路由自身的语义
- route / auth grant / session 三类真状态的归属

### 5.2 本轮禁止项

本轮明确不做：

- 不新增协议分支
- 不做历史兼容桥
- 不重新引入 session TTL
- 不新增自动重连
- 不做多盘 storer
- 不把 `whole` 和独立 `gateway` 重新揉成同一程序角色
- 不为了复用而做“大而全 runtime 基类”
- 不做一个“万能 service/helper”把复杂度重新包起来

## 6. 目标层次

本轮收口后，`server` 结构至少要明确分成下面三层。

### 6.1 底层最小能力层

这一层只承接稳定、通用、低语义能力，不承接业务编排。

包括但不限于：

- `proto`
- `bootstrap`
- `transport`
- `auth`
- `route.Registry`
- `session.Service`
- 本地文件后端
- pending request 表
- heartbeat watchdog / heartbeat loop
- register request 编解码
- storer round-trip RPC 小能力

固定要求：

- 底层能力不感知程序运行形态
- 底层能力不感知顶层 runtime
- 底层能力不做 route 失效后的全局清理编排

### 6.2 中层协作与适配层

这一层负责把底层最小能力拼成“单条链路”或“单一职责模块”。

典型对象包括：

- client-facing `Handler`
- `authenticator`
- `sessionOpener`
- storer 单连接 link runtime
- storer register 闸口
- route data plane adapter
- local gateway backend

固定要求：

- 中层只能围绕单一职责拼装底层能力
- 不再反向依赖顶层 runtime
- 不复制 route / auth grant / session 真状态

### 6.3 上层业务编排与对外入口层

这一层只保留：

- 顶层 runtime
- role 装配
- listener 启停
- accepted connection 分发
- 统一对外调用口

包括：

- `gateway.Runtime`
- `storer.RoleRuntime`
- `storer.WholeRuntime`
- `cmd/gateway`
- `cmd/storer`

固定要求：

- 上层不直接操纵下层细粒度细节
- 上层不继续维护新的业务真状态
- 上层只通过明确中层接口完成装配

## 7. 目标结构草案

本轮不强制文件名一字不差，但职责边界至少要达到下面这个结构级别：

```text
server/internal/
  gateway/
    runtime.go                       # 顶层 gateway runtime，只做装配、listener、分发
    client/
      listener_runtime.go            # client listener 壳
      connection_runtime.go          # accepted client connection 壳
      heartbeat_watchdog.go          # client connection watchdog
      handler.go                     # client-facing 协议闸口
      authenticator.go               # 认证过程
      session_opener.go              # open/describe/read/write/close
      auth_grant_registry.go         # auth grant 真状态
      session_registry.go            # gateway session 真状态

    storer/
      listener_runtime.go            # storer listener 壳
      link_runtime.go                # 单条 accepted storer link 生命周期
      register_gate.go               # register 闸口
      rpc_client.go                  # round-trip / request_id / response 配对
      connection_registry.go         # 活跃 storer link 真状态
      data_plane.go                  # route-facing SessionDataPlane 适配
      disconnect_notifier.go         # route 失效后的清理传播

  storer/
    core.go                          # 本地盘核心宿主
    role_runtime.go                  # role 顶层装配
    whole_runtime.go                 # whole 顶层入口
    gateway/
      local_adapter.go               # whole 本地固定路由适配
      link_runtime.go                # role=storer 的单条对 gateway 链路运行时
      register_client.go             # storer -> gateway register 主动发起
      data_plane_handler.go          # storer 侧数据面协议闸口
    link_heartbeat_watchdog.go       # storer 被动喂狗 watchdog
```

说明：

- 这里的文件名是“建议落点”，不是最终必须照抄的名字。
- 关键不是文件名，而是职责是否真正拆开。
- 但如果一组对象已经长期共享同一前缀，例如 `storer_*`、`gateway_*`、`client_*`，本轮默认优先改成目录分组，而不是继续追加同前缀平铺文件。

## 8. 详细执行阶段

### 8.1 Phase 0：基线确认与依赖盘点

目标：

- 在动代码前把当前结构问题、依赖方向和测试基线固定下来。

任务：

- 画清当前 `gateway` route-facing 调用链：
  - storer listener
  - accepted storer connection
  - register
  - heartbeat
  - round-trip
  - route disconnect
  - session close notice
- 画清当前 `storer` 主动连 gateway 调用链：
  - dial
  - register
  - watchdog
  - transport runtime
  - local session cleanup
- 列出要删除的旧结构：
  - `StorerHandler`
  - `gateway.Runtime` 中未必要保留的 route-facing 细节
- 跑当前 `go test ./...` 作为重构起点基线。

阶段完成标准：

- 依赖关系和要删的对象已经明确
- 确认当前功能基线可回归

### 8.2 Phase 1：先拆 gateway 的 storer link 底层能力

目标：

- 不先改顶层 runtime，先把 route-facing 底层组件摊开。

本阶段要拆开的最小组件至少包括：

- pending request table
- request_id 分配
- round-trip RPC client
- register 闸口
- gateway 主动 heartbeat loop
- storer link 活跃连接表

当前重点：

- 把 `storer_connection.go` 里混在一起的职责拆开
- 把 `storer_routes.go` 里“路由表 + 活连接表 + RPC + 故障传播”拆开

固定要求：

- `route.Registry` 继续只做 route 真状态
- 活跃 storer connection 不再藏在 route 适配层里顺手维护
- RPC client 只负责收发配对，不顺便做 route 清理编排

阶段完成标准：

- `storer_connection.go` 不再是 register / heartbeat / round-trip / serve loop 的混合体
- `storer_routes.go` 不再同时承担 route state、link state 和 data plane 逻辑

### 8.3 Phase 2：重建 gateway 的 route-facing 中层

目标：

- 在底层能力拆开后，重新拼出稳定的 route-facing 中层，而不是继续让顶层 runtime 直接驱动细节。

本阶段要形成的中层至少包括：

- 单条 accepted storer link runtime
- route-facing SessionDataPlane 适配层
- route disconnect 通知层

建议收口方式：

- 单条 accepted storer connection 的完整生命周期，交给独立 `storer_link_runtime`
- `SessionDataPlane` 的 route 转发适配层只关心：
  - 查 route
  - 找 link
  - 发 RPC
  - 映射 response status
- route 断开后的 session 清理传播，不再散在 runtime 和 registry 多处

固定要求：

- 中层不反向依赖 `gateway.Runtime`
- `gateway.Runtime` 只通过中层暴露的明确入口装配

阶段完成标准：

- `serveStorerConnection(...)` 退化为很薄的装配/调用壳，或被删除
- route-facing 数据面转发有独立、清楚的承接层

### 8.4 Phase 3：收顶层 gateway runtime

目标：

- 让 `gateway.Runtime` 真正回到顶层 runtime 定位。

任务：

- 删除未使用或已失去必要性的 `StorerHandler`
- 把 `gateway.Runtime` 中 route-facing 的细节移到中层
- 让 `gateway.Runtime` 只保留：
  - listener 启停
  - connection id 分配
  - accepted connection 分发
  - client session close notice 发回
  - 顶层组件装配

可选项：

- 如确认 `storer` listener 壳也值得抽成独立层，则一起抽出
- 但禁止把 client/storer listener 强行做成“万能 listener 框架”

阶段完成标准：

- `gateway.Runtime` 不再直接操纵 storer register / heartbeat / round-trip 细节
- 已删除空壳和过渡层，而不是继续挂着不用

### 8.5 Phase 4：收 storer 侧顶层运行时

目标：

- 让 `role=storer` 的主动连 gateway 链也按同样分层摊开。

本阶段要拆开的对象至少包括：

- 主动 register client
- 单条 gateway link runtime
- 顶层 role runtime / storer runtime 装配边界

建议收口方式：

- `role_runtime.go` 只负责：
  - 创建 `Core`
  - 选择 `whole` 或 `storer`
  - 调用对应 runtime
- `StorerRuntime` 只负责顶层装配，不直接吞 register 报文构造、watchdog 和 transport 启停细节
- 单条对 gateway 链路的运行时，下沉到独立组件

固定要求：

- `whole` 继续复用 gateway client-facing 能力
- `storer` 继续只承担 route provider 语义
- 不为了“对称”强行把 `whole` 和 `storer` 再揉成一套大运行时

阶段完成标准：

- `role_runtime.go` 和 `StorerRuntime` 明显变薄
- 主动连 gateway 的链路有独立中层承接

### 8.6 Phase 5：删除旧结构并统一入口

目标：

- 在新层次稳定后，删除旧入口、旧桥接和失效结构。

任务：

- 删除：
  - 不再使用的 `StorerHandler`
  - 不再使用的旧 route-facing 中转壳
  - 已被新中层取代的 helper 逻辑
- 全量修改引用，统一切到新入口
- 检查是否还残留：
  - 同一真状态的双份 owner
  - 上层绕过中层直连底层细节
  - listener 壳里继续写业务逻辑

阶段完成标准：

- 没有“新结构 + 旧结构并存”的长期双轨
- 没有为了迁移暂留但已无真实职责的空文件/空对象

## 9. 测试补强要求

本轮重构改的是结构，不该改变语义；但结构重构会直接影响并发、资源释放和故障路径，所以必须补测。

### 9.1 必测主链

- `gateway + storer` 注册成功主链
- `auth -> open -> read/write -> close` 主链
- `whole` 形态下完整 client-facing 主链

### 9.2 必测故障链

- gateway -> storer heartbeat 超时
- storer watchdog 超时未收到 heartbeat
- storer TCP 直接断开
- client TCP 直接断开
- route 断开后 gateway session 清理与 `SessionCloseNotice`
- client connection 断开后 upstream session close

### 9.3 必测结构回归点

- register 前收到非法 payload
- duplicate disk register reject
- open reject 语义不变
- route disconnect 后 auth grant 清理
- whole 模式下 client 连接失效只收该连接及其 session，不结束整个进程

### 9.4 本轮统一验收命令

固定至少执行：

```powershell
cd server
go test ./...
```

如果重构过程中新增独立中层组件，应同步补该组件的单测，而不是只靠集成测试兜底。

## 10. 文档同步要求

本轮完成后，如实现结构发生稳定变化，需要同步：

- `docs/network/server/README.md`
- `docs/tmp/server-architecture.md`
- `docs/progress/YYYY-MM-DD.md`

固定要求：

- 文档只描述当前真实结构
- 不保留“旧 runtime 时代”的历史结构口径

## 11. 分阶段执行顺序

固定顺序建议为：

1. Phase 0：基线与依赖盘点
2. Phase 1：拆 gateway storer link 底层能力
3. Phase 2：重建 gateway route-facing 中层
4. Phase 3：收顶层 gateway runtime
5. Phase 4：收 storer 顶层运行时
6. Phase 5：删除旧结构、统一入口、补文档
7. 全量测试

固定原因：

- 先拆底层，再收中层，最后收顶层，最符合第 5 点
- 如果一开始直接改 runtime，很容易只是把复杂度换个地方堆着
- 如果不先删空壳和旧入口，重构最后一定会留下双轨

## 12. 本轮最终验收口径

本轮结束后，`server` 应满足：

- `gateway` 和 `storer` 仍然保持当前正式语义不变
- `Go` 依赖方向继续单向下沉，没有新增反向依赖或导入环
- route-facing 链路已经拆成底层能力、中层协作、上层 runtime 三层
- `gateway.Runtime` 和 `StorerRuntime` 回到顶层装配定位
- `StorerHandler` 这类失去必要性的空壳已被删除或重建为真实中层
- 不再存在“registry 顺手管连接、连接顺手管 route、runtime 顺手管底层细节”的混合结构
- `go test ./...` 通过
- 文档、实现、测试重新收口到同一口径
