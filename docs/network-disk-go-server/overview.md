# 总览

## 1. 当前阶段目标

当前阶段只收敛到一条最短、唯一、可实现的主路径：

- `client` 只连接 `gateway`
- `gateway` 负责认证、路由、转发
- `storer` 持有真实存储介质并执行块读写

第一版不引入真正的分布式存储能力，也不保留 `client -> storer` 直连分支。

唯一主路径：

```text
client <-> gateway <-> storer
```

## 2. 当前模型图

### 2.1 部署拓扑

![Deployment topology](overview-deployment.svg)

### 2.2 Client 对象层级

```text
UI
  -> ClientState
    -> DiskCatalogState
      -> DiskRuntimeStore
        -> DiskRuntime
          -> NetworkMedia(Media adapter)
    -> GatewayConnection(server endpoint)
      -> DiskSession(storer session)

NetworkMedia
  -> bind -> DiskSession
```

对象关系：

- `ClientState` 当前真实持有 `BackendContext` 与 `DiskCatalogState`
- `DiskCatalogState` 当前真实持有 `DiskRuntimeStore`
- `DiskRuntime` 当前真实持有 `media: Option<Box<dyn Media>>`
- 网络盘接入后，`DiskRuntime` 继续持有 `NetworkMedia`
- 一个 `GatewayConnection` 对应一个 server endpoint
- 一个 `GatewayConnection` 下并发承载多个 `DiskSession`
- `GatewayConnection` 只管理连接、收发循环、pending request 和 `DiskSession` 注册表
- `NetworkMedia` 绑定一个 `DiskSession`

当前命名口径：

- `GatewayConnection`：client 到某个 server gateway 的单 TCP 连接管理对象
- `DiskSession`：某个远端盘在 client 侧的已认证会话
- `NetworkMedia`：基于 `DiskSession` 实现的 `Media` 适配层
- `DiskRuntime`：tauri-client 当前真实持有 `Media` 的盘运行时对象

当前模型约束：

- 一个 `GatewayConnection` 对应一个 server endpoint
- 一个 `GatewayConnection` 下可以并发承载多个 `DiskSession`
- 多个 `NetworkMedia` 并发抢同一条 `GatewayConnection` 是预期行为
- `DiskRuntime` 持有 `NetworkMedia`
- `NetworkMedia` 绑定一个 `DiskSession`
- client 永远只连接 `gateway`，不直接连接 `storer`

## 3. Server 语言与目录组织

`server` 使用单 Go module。

```text
server/
├── go.mod
├── cmd/
│   ├── gateway/
│   └── storer/
└── internal/
    ├── auth/
    ├── config/
    ├── gateway/
    ├── proto/
    ├── route/
    ├── session/
    ├── storage/
    │   └── file/
    └── storer/
```

说明：

- `cmd/gateway`：独立网关进程入口
- `cmd/storer`：独立存储器进程入口
- `internal/auth`：领盘码、challenge、proof
- `internal/route`：盘路由与认证缓存
- `internal/session`：client session 与 gateway 到 storer 的转发会话
- `internal/storage/file`：第一阶段唯一存储后端

第一阶段只实现 `file` backend，不提前做数据库 backend。

## 4. 职责边界

### gateway

只负责：

- 接收 `disk_id`
- 返回统一形态的 challenge
- 在本地使用缓存的认证信息校验 `proof`
- 为认证成功的 client 创建 `DiskSession`
- 维护 `client session -> storer session` 映射
- 转发 `ReadAt / WriteAt / Ping / Close`

不负责：

- 真实数据存储
- 块数据落盘
- 多登录后的权限仲裁

### storer

只负责：

- 持有真实存储介质
- 管理真实数据面会话
- 执行块读写
- 决定同一 `disk_id` 多登录后的操作权和共享策略

### embedded gateway 模式

`storer` 在单盘部署时内建 `gateway` 能力。

要求：

- 使用与独立 `gateway` 完全相同的认证协议
- 使用同一套业务层请求头和数据面命令
- 只是部署形态不同，不派生第二套认证或数据面逻辑

## 5. 单一真实来源

### storer 本地配置

`storer` 本地配置是以下事实的唯一真实来源：

- 完整领盘码 `claim_code`
- 存储后端配置
- `storer` 对外监听地址
- 绑定的盘实例

### gateway 路由缓存

`gateway` 本地内存缓存只保存认证和路由所需最小信息：

- `disk_id`
- `auth_verifier`
- `storer_id`
- `route_target`
- `auth_version`
- 路由存活信息

其中：

```text
auth_verifier = SHA512(claim_code_bytes)
```

`gateway` 不保存原始 `claim_code`。

### client 本地配置

client 本地配置只保存网络盘重建所需最小信息：

- `server_endpoint`
- `claim_code`
- `disk_name`
- `auto_mount`

以下信息不在 client 本地持久化：

- `auth_verifier`
- `session_id`
- `disk_size_bytes`
- `read_only`

其中 `disk_id` 可直接由 `claim_code` 前 `16` 个字符解析得到，不单独持久化。

### 多登录策略

认证层不处理“同一 `disk_id` 是否允许多登录”的业务决策。

当前口径：

- 两个连接如果都持有正确领盘码，都可以通过认证
- 认证成功后谁有操作权、是否共享、是否排它，由 `storer` 决定

## 6. 第一版最小会话模型

认证成功后不向 client 暴露 `storer_addr`，而是在 `gateway` 内部建立转发链。

会话模型：

```text
DiskSession {
  session_id
  disk_id
  disk_size_bytes
  read_only
  max_io_bytes
  expires_at
}
```

约束：

- `session_id` 由 `gateway` 分配
- `gateway` 持有 `session_id -> storer session` 映射
- `client` 后续所有数据面请求只带 `session_id`
- `NetworkMedia` 构造时必须拿到 `disk_size_bytes`、`read_only`、`max_io_bytes`

## 7. 第一版实现顺序

建议按下面顺序推进：

1. `storer` 本地配置与领盘码生成
2. embedded gateway 模式下的 `AuthStart/AuthFinish`
3. `SessionOpen` 与最小数据面命令
4. `NetworkMedia` 最小接入
5. 独立 `gateway`
6. `storer -> gateway` 注册与路由缓存

## 8. 第一版验收口径

第一版只验收以下最小闭环：

1. 假盘与错码在外部观察上都走同样认证流程
2. 一个 `GatewayConnection` 上能同时承载多个 `DiskSession`
3. `NetworkMedia` 能完成真实 `read/write`
4. `tauri-client` 能完成网络盘的添加、挂载、拔出、重挂载
