# Gateway-and-Storer 业务层协议草案

## 1. 当前定位

`gateway-and-storer` 当前不重新定义一整套新的数据面业务协议。

当前只拆成两部分：

1. 注册阶段
2. 数据面复用规则

其中：

- 注册阶段单独定义
- 注册成功后，`gateway <-> storer` 复用现有 `SessionOpen / ReadAt / WriteAt / Ping / Close` 业务语义

这意味着：

- `AuthStart / AuthFinish` 只存在于 `client <-> gateway`
- `gateway <-> storer` 不再有 client 认证业务层

## 2. 角色与连接方向

### `whole`

- 持有本地存储
- 启动内嵌 `gateway`
- 对 client 监听
- 不走 `gateway-and-storer` 注册协议

### `storer`

- 持有本地存储
- 不对 client 监听
- 主动长连 `gateway`
- 先注册，再进入数据面复用阶段

连接方向固定为：

```text
storer ----主动长连----> gateway
```

第一版不做：

- gateway 主动反连 storer
- 多条 storer 控制连接
- storer 多盘批量注册

## 3. 注册阶段

### 3.1 注册目的

注册阶段只解决三件事：

1. 让 gateway 知道这个 storer 是可信的
2. 让 gateway 知道这个 storer 提供哪个 `disk_id`
3. 让 gateway 预缓存 `auth_verifier` 与基础盘元数据

### 3.2 注册请求最小字段

`storer` 向 `gateway` 提交：

- `gateway_token`
- `disk_id`
- `auth_verifier = SHA512(claim_code_bytes)`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `session_ttl_seconds`

其中：

- `claim_code` 本体不上传给 `gateway`
- `gateway_token` 是基础设施控制面凭据，不复用 `claim_code`

### 3.3 注册成功后 gateway 本地缓存

`gateway` 至少缓存：

- `disk_id`
- `auth_verifier`
- `storer_connection`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `session_ttl_seconds`

要求：

- 路由表是 connection-scoped
- `storer_connection` 断开时立即撤销该盘路由
- 同时撤销该 storer 名下全部会话映射

## 4. 数据面复用规则

注册成功后，`gateway <-> storer` 不重新定义新的 `SessionOpen / ReadAt / WriteAt / Ping / Close` 业务命令。

复用规则：

- 复用同一套 `op_code`
- 复用同一套 body 布局
- 复用同一套 status 语义

当前不采用：

- 裸字节 blind forward
- client request 原样不改直接转发给 storer

## 5. Gateway 必须改写的字段

虽然业务语义复用，但 `gateway` 仍然必须本地维护映射并改写以下字段。

### 5.1 `request_id`

原因：

- 多个 client 会并发复用同一条 `gateway <-> storer` 连接
- 不同 client 的 `request_id` 可能冲突

因此：

- `gateway` 向 storer 侧发送请求时必须分配自己的 `request_id`
- 响应回来后再还原到对应 client 请求

### 5.2 `session_id`

原因：

- client 面向的是 gateway 的统一会话命名空间
- storer 内部只认识自己的 `storer_session_id`

因此：

- `gateway` 对 client 暴露 `gateway_session_id`
- `gateway` 内部维护：

```text
gateway_session_id -> (storer_connection, storer_session_id)
```

- `storer_session_id` 不直接作为 client 侧长期正式会话 ID

## 6. SessionOpen 透传口径

当 client 在 `gateway` 侧已经完成认证后：

1. client 发送 `SessionOpen(disk_id)`
2. gateway 根据 `disk_id` 找到目标 `storer_connection`
3. gateway 向 storer 发送复用语义的 `SessionOpen(disk_id)`
4. storer 决定：
   - 成功
   - busy
   - unavailable
   - read_only
5. 成功时 storer 返回：
   - `storer_session_id`
   - `disk_size_bytes`
   - `read_only`
   - `max_io_bytes`
   - `session_ttl_seconds`
6. gateway 分配 `gateway_session_id`
7. gateway 建立映射并回给 client

当前约束：

- 单盘独占策略仍由 storer 决定
- gateway 只路由和映射，不替 storer 判定独占

## 7. ReadAt / WriteAt / Ping / Close 透传口径

`SessionOpen` 成功后：

1. client 对 `gateway_session_id` 发请求
2. gateway 查表拿到 `(storer_connection, storer_session_id)`
3. gateway 用 storer 侧的 `session_id` 转发
4. storer 返回结果
5. gateway 恢复到 client 侧的 `request_id`
6. gateway 把语义一致的响应回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改变成功/失败语义
- `gateway` 只负责路由、映射、转发、回传

## 8. 会话与认证边界

当前边界固定如下：

- `AuthStart / AuthFinish`：只在 `client <-> gateway`
- 认证成功：只表示当前 client 连接获得开会话资格
- `SessionOpen`：才是真实会话创建入口
- `SessionOpen` busy 失败：client 可以保留同一连接，稍后继续重试

这意味着：

- 不向 client 暴露单独的 `auth_id`
- 不向 storer 传 client 的认证流程
- 认证资格绑定在 gateway 看到的 client connection 上

## 9. 第一版实现顺序

建议按下面顺序推进：

1. 配置角色：`whole | storer`
2. `storer` 长连 `gateway` 的注册骨架
3. `gateway` 的 storer listener 骨架
4. 盘路由表
5. `SessionOpen` 转发
6. `ReadAt / WriteAt / Ping / Close` 转发
7. 掉线清理盘路由和会话映射
