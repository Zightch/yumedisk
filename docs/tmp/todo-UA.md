# 网络 `rw -> ro` DataChanged / `remote_backend_id` 重建执行清单

## 0. 当前范围

本清单只服务于网络共享盘这条主线：

- `rw` 写入 committed 后，如何把 `data changed` 从 `storer` 传到 `ro` client
- `SessionDescribe` metadata 如何补 `remote_backend_id`
- client 如何基于 `remote_backend_id` 拒绝同 backend 的重复盘

以下内容已经完成并已归档到 `docs/progress/`，不再继续写在当前 `todo` 中：

- 本地 `Unit Attention / data_changed` 驱动链
- 本地共享内存 `sm / ct smid= / rm smid=` 最小闭环
- `28/00` runtime 验证

参考归档：

- `docs/progress/2026-05-28.md`
- `docs/progress/2026-05-30.md`

## 1. 当前总目标

按重建口径完成网络共享盘的最小正式闭环：

1. `docs/network` 正式定义 `SessionDataChangedNotice`
2. `docs/network` 正式定义 `SessionDescribe` metadata 增加 `backend_id`
3. `metadata` 真源从 `gateway` 完全摘除，改为由 `storer` 直接提供
4. `server` 补齐 `rw write committed -> ro notice fanout`
5. `rust-cli` 补齐 `data changed` 接收与 `remote_backend_id` 冲突检查
6. 用多机 `rust-cli` 联调验证 `rw -> ro` 变化感知
7. 用单机 `rust-cli` 验证 `rw/ro` 与多 `ro` 冲突拒绝

## 2. 固定边界

### 2.1 本轮只做的事

- `data_changed`
- `remote_backend_id`
- `SessionDescribe` metadata 真源改造
- `rw -> ro` notice 传播
- client 唯一性拒绝

### 2.2 本轮明确不做

- `capacity_changed`
- metadata changed 以外的其他 notice reason
- 通用 media change framework
- 用 `SessionCloseNotice` 复用 `data_changed`
- gateway 维护 backend 分组表
- gateway 持有 metadata snapshot
- 按历史兼容方式保留旧 metadata 路线

### 2.3 当前正式口径

- `data_changed` 不是 `SessionCloseNotice`
- `data_changed` 不等于 session 失效
- `data_changed` 不触发 network disk invalidation
- `remote_backend_id` 只通过 `SessionDescribe` 暴露
- `remote_backend_id` 不进入 `AuthStart / AuthFinish / SessionOpen`
- `remote_backend_id` 不进入 gateway 的 route/auth/session 真状态
- `gateway` 只做 session 映射与 notice 转发
- `metadata` 唯一真实来源是 `storer`

## 3. 适用原则

本清单严格按 [开发原则](../development/development-principles.md) 执行，重点收口如下：

- 极简核心原则
  - 当前只收一条最小网络主线：`rw committed -> ro data_changed notice -> client notify_managed_disk_data_changed`
- 激进更新原则
  - 直接删除 gateway metadata snapshot 口径，不保留双轨
- 单一真实来源原则
  - metadata 只由 `storer` 提供
  - `remote_backend_id` 只由 `storer` 生成并通过 `SessionDescribe` 返回
  - `rw` 写入是否 committed 只以本地 backend 的真实提交结果为准
- 边界闸口原则
  - 重复盘拒绝统一收在 client 唯一入口
  - `SessionDataChangedNotice` 的协议校验统一收在各自协议边入口
- 结构重构与层次依赖原则
  - `data changed` 链路必须按 `storer -> gateway -> client` 单向下沉
  - 不允许再让 `gateway` 同时承担 metadata 真源与 data changed 决策
- 删除优先原则
  - 删除“gateway 保存 metadata snapshot”的旧正式口径

## 4. 当前现状

### 4.1 `docs/network` 还没有正式 `data changed` notice

当前正式网络协议只有：

- `SessionCloseNotice`
- `SessionDescribe`
- `ReadAt / WriteAt`
- `ConnHeartbeat`

当前缺失：

- dedicated `SessionDataChangedNotice`
- `data_changed` 的正式边界、方向、非目标和宿主责任

### 4.2 `SessionDescribe` 当前仍写成 gateway 本地回答

当前 server 文档口径仍是：

- storer 注册 metadata
- gateway 存 route metadata
- `SessionOpen` 时复制到 session snapshot
- `SessionDescribe` 由 gateway 本地回答

这条口径需要删除，因为它与本轮目标冲突：

- `backend_id` 不应进入 gateway 真状态
- metadata 不应在 gateway 与 storer 双份维护

### 4.3 server 当前没有网络共享盘 `data changed` 通知链

当前 server 侧已有：

- `rw` / `ro` 双导出
- route 独立 session 生命周期
- `SessionCloseNotice` 故障链

当前缺失：

- `rw` 写 committed 后，找到同 backend 下受影响 `ro session`
- `storer -> gateway` 的 `SessionDataChangedNotice`
- `gateway -> client` 的 `SessionDataChangedNotice`

### 4.4 client 当前没有 `remote_backend_id` 唯一性收口

当前 client 正式口径仍是：

- 唯一键：`(server_addr, remote_disk_id)`

当前缺失：

- `SessionDescribe` metadata 里的 `remote_backend_id`
- `(server_addr, remote_backend_id)` 冲突检查

## 5. 目标结构

### 5.1 metadata 路线

正式结构改为：

```text
client SessionDescribe(gateway_session_id)
  -> gateway 查 session 映射
  -> gateway 转发 SessionDescribe(storer_session_id)
  -> storer 直接返回 metadata
  -> gateway 原样回给 client
```

当前最小 metadata 集改为：

- `disk_size_bytes`
- `max_io_bytes`
- `read_only`
- `backend_id`

其中：

- `disk_size_bytes / max_io_bytes / read_only` 也不再由 gateway snapshot 持有
- `backend_id` 由 storer 在程序启动时为本地 backend 临时生成
- 同一个本地 backend 下的 `rw` 和 `ro` 导出必须共用同一个 `backend_id`
- `backend_id` 不要求跨重启稳定

### 5.2 `data changed` 路线

正式结构改为：

```text
rw client WriteAt
  -> gateway
  -> rw storer session
  -> backend committed
  -> storer 找到受影响 ro session
  -> storer 向 gateway 逐个发送 SessionDataChangedNotice
  -> gateway 按 session 映射转发给目标 client
  -> client 若该 session 已挂载，则 notify_managed_disk_data_changed(...)
```

固定约束：

- notice 单向发送，不等回复
- `gateway` 不按 backend 做 fanout 决策
- `gateway` 只做 `(route_connection_id, storer_session_id) -> gateway_session_id` 映射转发
- `storer` 自己负责决定哪些 `ro session` 受影响
- 当前只对 live `ro session` 发 notice

### 5.3 client 唯一性路线

冲突拒绝正式收口为：

- 冲突条件一：`(server_addr, remote_disk_id)` 相同
- 冲突条件二：`(server_addr, remote_backend_id)` 相同

直接结果：

- 同一 client 不允许同时添加同一 backend 的 `rw` 和 `ro`
- 同一 client 不允许同时添加同一 backend 的多个 `ro`
- `remote_backend_id` 只在拿到 `SessionDescribe` 后才能参与判断

## 6. 四阶段执行方案

### 第一阶段：先同步 `docs/network`

目标：

- 明确 `SessionDataChangedNotice`
- 明确 metadata 真源从 gateway 摘除
- 明确 `SessionDescribe` 增加 `backend_id`

需要修改：

- `docs/network/define/README.md`
- `docs/network/define/client-gateway.md`
- `docs/network/define/gateway-storer.md`
- `docs/network/define/data-plane.md`
- `docs/network/client/*.md`
- `docs/network/server/*.md`

需要收住的点：

- `SessionDataChangedNotice` 是 dedicated notice，不复用 close
- 当前方向只定义：
  - `storer -> gateway`
  - `gateway -> client`
- body 当前固定为空
- 目标 session 由 notice header `session_id` 指向
- `SessionDescribe` metadata 真源明确为 `storer`
- 删除所有“gateway session snapshot metadata”的正式口径

验收：

- `docs/network` 内不再出现 gateway metadata snapshot 正式描述
- `data changed` 与 `SessionCloseNotice` 边界清晰分离

### 第二阶段：补 server 的 `backend_id` 面

目标：

- `storer` 为本地 backend 临时生成 `backend_id`
- `SessionDescribe` 改为真正由 storer 提供

server 需要完成：

- `role=storer`
  - 启动时为本地 backend 生成一个临时 `backend_id`
  - `rw` / `ro` 导出共用该 `backend_id`
- `role=whole`
  - 内嵌双导出同样共用一个 `backend_id`
- gateway
  - 删除 metadata snapshot 依赖
  - `SessionDescribe` 改为转发式实现

固定约束：

- `backend_id` 不进入 route registry
- `backend_id` 不进入 auth grant
- `backend_id` 不进入 gateway session registry
- metadata 不在 gateway 再存一份

验收：

- `SessionDescribe` 返回中包含 `backend_id`
- gateway 侧不再保存 metadata snapshot

### 第三阶段：补网络 `data changed` 消息链

目标：

- 打通 `rw committed -> ro client notify_managed_disk_data_changed(...)`

server 需要完成：

- 协议与编解码新增 `SessionDataChangedNotice`
- `storer`
  - `rw` 会话写入真正 committed 后
  - 找到同 backend 下所有 live `ro session`
  - 对每个目标 `ro session` 向 gateway 发 notice
- `gateway`
  - 收到 `storer_session_id` notice 后
  - 查到对应 `gateway_session_id`
  - 若目标 client 仍在线，则转发给 client
  - 若目标 session 已失效，则幂等忽略

client 需要完成：

- `network-core`
  - 支持新的 notice 编解码与回调
- `rust-cli`
  - 收到 notice 时
  - 若该 session 当前已挂载到本地目标盘，则调用 `notify_managed_disk_data_changed(...)`
  - 若 session 仅打开但未挂载，当前最小闭环先忽略

联调验证环境：

- 本机启动 server
- `vm_win10` 跑一套 `rust-cli`
- `vm_win11` 跑一套 `rust-cli`

联调验收：

1. Win10 侧打开 `rw` 盘并写入
2. Win11 侧打开同 backend 的 `ro` 盘
3. 写入 committed 后，Win11 收到 `SessionDataChangedNotice`
4. Win11 本地调用 `notify_managed_disk_data_changed(...)`
5. `ro` 盘保持在线，不进入 invalid

### 第四阶段：补 `remote_backend_id` 到 client 与冲突检查

目标：

- client 正式基于 `(server_addr, remote_backend_id)` 拒绝重复盘

client 需要完成：

- `network-core`
  - `SessionDescribeResponse / SessionMetadata` 增加 `backend_id`
- `rust-cli`
  - mount/open 结果保存 `remote_backend_id`
  - 唯一性检查扩为双条件

正式冲突规则：

- `(server_addr, remote_disk_id)` 相同拒绝
- `(server_addr, remote_backend_id)` 相同拒绝

验证只做 `rust-cli`：

- 单机 `rw + ro` 冲突测试
- 单机 `ro + ro` 冲突测试
- 确认被拒绝的是“同 backend 重复盘”，不是 session 本身异常

## 7. 当前唯一下一步

先完成第一阶段：重写 `docs/network`，删除 gateway metadata snapshot 正式口径，并补 `SessionDataChangedNotice + SessionDescribe.backend_id`。
