# 当前总目标

按 `docs/network-disk-go-server` 当前正式设计，重建 `server` 结构，使其从“单盘 embedded gateway 一体机”收束为两个可执行文件的清晰模型：

- `storer` 可执行文件：仅支持 `whole | storer`
- `gateway` 可执行文件：独立程序，不属于 `storer` 的 `role`

本轮目标是先把结构任务树落定，不直接进入实现细节。

## 当前结构判断

当前 `server` 已具备可复用地基：

- `internal/transport`
- `internal/auth`
- `internal/session`
- `internal/storage/file`

当前 `server` 仍存在的核心问题：

1. `internal/storer/service.go` 同时承担：
   - 本地存储宿主
   - 本地会话宿主
   - embedded gateway
   - client TCP 监听入口
2. `internal/gateway` 仍按“单盘 + 本地 session”建模，不能承接路由型 gateway。
3. 缺少 `disk_id -> storer` 注册/路由层。
4. 缺少 gateway 自己的 session 映射层。

结论：

- 现状足以支撑单盘 `whole` 最小闭环。
- 现状不足以直接承接 `gateway + storer` 分体模型。
- 必须按开发原则做结构重建，而不是继续在现有 `storer/service.go` 上叠补丁。

## 重建原则

本轮 server 重建遵守以下固定口径：

1. 先保留唯一主路径，不预做分布式扩展。
2. 先拆职责，再补协议接线。
3. 优先删除旧耦合入口，不保留长期双轨。
4. `Go` 侧维持单向依赖，不引入包循环。
5. 文档、任务树、实现同步收束到当前唯一结构。

## 任务树

### A. 重建 server 角色地基

#### A2. 拆分本地存储宿主

目标：

- 从当前 `internal/storer/service.go` 中拆出“真实盘宿主”最小核心。

收口要求：

- 本地 raw 存储打开与关闭独立
- 本地 `session.Service` 独立
- 不在这一层承担 client 监听职责
- 不在这一层承担 gateway 路由职责

输出形态：

- 一个只负责“本地盘 + 本地 session 数据面”的 storer core

### B. 重建 gateway 结构

#### B1. 拆 client-facing gateway

目标：

- 把当前 `internal/gateway` 从“单盘本地 handler”重建为 client-facing gateway 边界层。

收口要求：

- 认证入口只处理 `AuthStart / AuthFinish`
- 会话入口只处理 `SessionOpen`
- 数据面入口只处理 `ReadAt / WriteAt / Ping / Close`
- client 连接上的认证资格仍为 connection-scoped

必须删除：

- `NewHandler(realDiskID, authVerifier, sessions)` 这种单盘本地直绑入口

#### B2. 新增路由/注册层

目标：

- 提供 `disk_id -> storer route` 的唯一真实来源。

收口要求：

- 保存 storer 注册信息
- 保存 `auth_verifier`
- 保存 storer 当前连接状态
- 为 client-facing gateway 提供只读路由查询入口

必须避免：

- 在多个 handler 内各自缓存 disk 路由状态

#### B3. 新增 gateway session 映射层

目标：

- 明确 gateway 和 storer 的 session 边界。

收口要求：

- `gateway_session_id -> storer_connection + storer_session_id`
- gateway 负责本地 `session_id` 分配与释放
- storer 返回的真实 `session_id` 只留在 gateway 内部

必须避免：

- 把 storer 原始 `session_id` 直接暴露给 client

#### B4. 新增 storer-facing gateway 边界

目标：

- 为 storer 注册和后续会话透传建立独立入口。

收口要求：

- 独立监听端口
- 独立注册流程
- 注册后复用现有数据面语义，不重复发明第二套数据面业务定义

### C. 重建 storer 结构

#### C1. 明确 storer 只做真实数据面

目标：

- 在 `storer` 角色下，收紧职责到“存储 + 会话 + gateway 对接”。

收口要求：

- 不直接承接 client 协议入口
- 不直接面向 client 做认证
- 只接受 gateway 注册链路与后续会话/IO 请求

#### C2. 保留 whole 的 embedded gateway 形态

目标：

- 不破坏当前单盘最小闭环。

收口要求：

- `whole` 仍可单独启动并对 client 提供完整服务
- 但实现上通过装配层完成，而不是把所有逻辑继续塞回 `storer/service.go`

### D. 协议接线与最小闭环

#### D1. 接通 gateway-and-storer 注册阶段

目标：

- 让独立 storer 能把自己挂到 gateway。

收口要求：

- storer 提交：
  - `gateway_token`
  - `disk_id`
  - `auth_verifier`
- gateway 建立注册表

#### D2. 接通 SessionOpen 透传链

目标：

- 先打通单盘最小闭环最关键的会话打开链路。

收口要求：

- client 只连 gateway
- gateway 认证成功后，向目标 storer 发起 `SessionOpen`
- storer 返回 busy / success / readonly 等结果
- gateway 完成本地 session 映射

#### D3. 接通 ReadAt / WriteAt / Ping / Close

目标：

- 完成 `gateway <-> storer` 数据面主路径。

收口要求：

- 沿用既有业务语义
- gateway 负责 `request_id` / `session_id` 映射
- storer 只理解 gateway 发来的业务包

### E. 启动入口与联调

#### E1. 新增独立启动入口

目标：

- 明确两个可执行文件的实际启动方式。

收口要求：

- `cmd/storer`：内部 `role = whole | storer`
- `cmd/gateway`：独立 gateway 程序

#### E2. 保持现有联调工具继续可用

目标：

- 不让协议联调重新绑回 KMDF/AppKernel 宿主。

收口要求：

- `windows/rust-cli/src/bin/network-auth-open.rs` 继续可直连 gateway
- 先覆盖：
  - auth
  - open
  - busy
  - close
  - reopen

### F. 文档同步

#### F1. 同步正式文档

目标：

- 文档只描述当前真实结构。

收口要求：

- `README.md`
- `overview.md`
- `gateway-and-storer.md`
- `auth-routing.md`
- `data-plane.md`

必须删除：

- 与新结构不一致的旧一体机叙述

#### F2. 同步 progress

目标：

- 每个阶段完成后收进 `docs/progress`

## 推荐实现顺序

1. A-F 已完成

## 当前下一步

本轮 `A-F` 已收完。

如果继续推进，下一步应新开任务树，而不是继续修改本轮 server 结构文档。
