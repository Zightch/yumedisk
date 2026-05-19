# 当前待办总表

## 1. 当前状态

本轮正式文档收口已完成，当前下一步切到 `tauri-client` 网盘迁移草案。

## 2. 当前正式口径

- 正式网络盘主线只看：
  - `docs/network/define/`
  - `docs/network/client/README.md`
  - `docs/network/server/README.md`
- `docs/progress/` 只做进度归档，不进入正式文档同步范围。
- `docs/old_dev/` 保留为早期驱动探索归档，不参与正式文档同步。

## 3. 当前活跃待办

- `docs/tmp/todo-tauri-client.md`
  - 收口 `rust-cli -> tauri-client` 的网盘迁移边界、命名、draft 生命周期、重扫与 cleanup 闸口。

## 4. tauri-client 网盘结构重构待办

### 4.1 背景

前一个实现已经完成 `windows/tauri-client` 网盘最小闭环，当前对象模型与正式文档口径已经基本收住：

- `ClientState -> NetworkClientState -> connection_pool / opened_disk_sessions / network_create_drafts`
- `DiskCatalogState -> DiskRuntimeStore -> DiskRuntime[*]`
- 挂载态下 `DiskRuntime -> NetworkMedia -> DiskSession`

当前问题不在概念模型本身，而在实现层次还没有按开发原则第 5 点摊开：

- `src-tauri/src/backend/network_service.rs` 同时承担输入校验、网络编排、draft 生命周期、事件收束、挂载/拔出/删除、重扫、错误映射。
- `src-tauri/src/backend/disk_service.rs` 仍然知道过多 network 细节。
- `src-tauri/src/commands/*` 和 `src-tauri/src/lib.rs` 需要手动在多个入口触发 `sync_pending_events`，边界没有收成唯一闸口。
- `src/features/createNetworkDisk/CreateNetworkDiskDialog.vue` 同时承担对话框壳、交互态、API 编排、错误翻译、列表展示。

本轮不是改逻辑，不是改正式对象关系，而是把已经成立的最小闭环拆成清晰多层结构。

### 4.2 本轮硬约束

#### 4.2.1 不改变的正式概念结构

必须保持与以下文档一致：

- `docs/network/define/网盘结构模型.jpeg`
- `docs/network/define/model-topology.svg`
- `docs/network/client/tauri-client.md`

固定不改：

- `NetworkClientState` 仍然是网络真状态唯一持有者。
- `DiskRuntime` 仍然只描述本地盘卡片与持久化配置。
- `NetworkMedia` 仍然只在真正挂载到 `BackendRust` 时构造。
- live `DiskSession` 仍然归 `NetworkClientState`，不下沉进 `DiskRuntime`。
- 全局唯一键仍然是 `(server_addr, remote_disk_id)`。
- draft、rescan、mount、eject、delete、cleanup 的当前正式语义不改。

#### 4.2.2 不改变的外部接口

固定不改：

- Tauri command 名称
- 前端 invoke 参数和返回 DTO 结构
- 持久化字段口径
- 主页三态展示口径
- 创建网络盘对话框当前交互链路
- cleanup 触发点和故障后转 `invalid` 的当前策略

#### 4.2.3 本轮禁止项

本轮明确不做：

- 不引入第二份网络状态 store
- 不引入新的 UI 业务真状态容器
- 不引入 actor 模型、后台 idle sweeper、自动重连
- 不额外扩展 server 协议
- 不保留长期兼容层或旧 facade
- 不把 `network_service.rs` 简单搬家后继续维持巨型文件

### 4.3 重构目标

本轮目标固定为：

1. 把当前 network 相关实现拆成“底层最小能力 / 中层协作编排 / 上层桥接入口”三层。
2. 把 network 业务编排从 `src-tauri/src/backend/` 中收出去，避免 backend 目录继续承担宿主适配以外的职责。
3. 把事件同步、draft 生命周期、runtime 生命周期、cleanup 责任分别收进明确模块。
4. 让 `commands/*` 回到薄桥接定位，只负责 DTO 映射、加锁、调用明确 usecase。
5. 让 `CreateNetworkDiskDialog.vue` 回到 feature 组合壳定位，不再平铺所有交互逻辑。

### 4.4 目标目录结构

#### 4.4.1 Rust 侧目标结构

建议新增目录：

- `windows/tauri-client/src-tauri/src/network/`

目标结构：

```text
src-tauri/src/
  backend/
    disk_service.rs
    network_media.rs
    persistence_service.rs
    ...
  network/
    mod.rs
    validation.rs
    gateway_ops.rs
    uniqueness.rs
    cleanup.rs
    event_reconciler.rs
    draft_flow.rs
    runtime_flow.rs
```

目录职责固定为：

- `backend/`
  - 只放宿主适配、`BackendRust` 交互、`Media` 适配、持久化适配。
- `network/`
  - 只放网盘运行时编排和 network 业务规则。
- `state/`
  - 只放运行态持有和最小状态对象。
- `commands/`
  - 只放 command DTO 与入口桥接。

#### 4.4.2 前端目标结构

建议把 `createNetworkDisk` feature 拆成：

```text
src/features/createNetworkDisk/
  CreateNetworkDiskDialog.vue
  useNetworkDraftFlow.ts
  networkDraftError.ts
  NetworkDraftForm.vue
  NetworkDraftList.vue
```

职责固定为：

- `CreateNetworkDiskDialog.vue`
  - 只负责弹窗壳、组合布局、事件透传。
- `useNetworkDraftFlow.ts`
  - 只负责编排对话框本地交互态和 invoke 调用。
- `networkDraftError.ts`
  - 只负责错误码到中文文案的映射。
- `NetworkDraftForm.vue`
  - 只负责服务器地址、磁盘名、领盘码输入与按钮区。
- `NetworkDraftList.vue`
  - 只负责 draft 列表展示与移除动作。

### 4.5 Rust 侧详细拆分任务

#### 4.5.1 第一阶段：先抽底层最小能力

目标：

- 先把可独立复用的底层逻辑从 `backend/network_service.rs` 拆出来。
- 这一阶段不改变 command 调用口，只做内部去混杂。

任务：

- 新建 `src-tauri/src/network/validation.rs`
  - 收口 `server_addr`、`disk_name`、draft 存在性、disk 存在性等入口校验。
- 新建 `src-tauri/src/network/uniqueness.rs`
  - 收口 `(server_addr, remote_disk_id)` 唯一性检查。
  - 检查范围必须继续覆盖正式 `DiskRuntime` 和 draft。
- 新建 `src-tauri/src/network/gateway_ops.rs`
  - 收口 connect / auth / open / describe / close / discard 这些最小网络操作。
  - 把 `add draft item` 与 `rescan` 里重复的 `auth -> open -> describe` 主链合并到统一能力。
- 新建 `src-tauri/src/network/cleanup.rs`
  - 收口 session close cleanup、draft session cleanup、idle connection cleanup、runtime invalidation。

阶段验收：

- `network_service.rs` 中与网络协议交互、重复校验、cleanup 直接相关的实现大幅减少。
- 没有新增任何状态镜像。
- 当前行为语义不变。

#### 4.5.2 第二阶段：抽事件收束器

目标：

- 把 `sync_pending_events` 从巨型服务文件里单独收成唯一事件收束模块。

任务：

- 新建 `src-tauri/src/network/event_reconciler.rs`
  - 承接当前 `sync_pending_events` 逻辑。
  - 只负责：
    - drain `NetworkClientEvent`
    - drain media invalidation
    - 把 `NetworkClientState`、`DiskRuntimeStore`、`BackendRust` 收束成一致状态
  - 不负责：
    - draft 创建
    - mount/eject/delete 编排
    - UI DTO 映射
- 统一事件处理入口名称和语义。
- 检查当前 watcher、disk command、network draft command 的调用位置，明确谁是正式入口，谁只是同步前置。

阶段验收：

- “事件到本地状态的收束”有唯一模块和唯一口径。
- 后续不再在多个业务流里随手补一段事件处理细节。

#### 4.5.3 第三阶段：抽 draft 生命周期流

目标：

- 把“创建网络盘对话框”的草稿生命周期单独收成一条业务链。

任务：

- 新建 `src-tauri/src/network/draft_flow.rs`
  - 只承接：
    - `test connection`
    - `create draft`
    - `add item`
    - `remove item`
    - `submit`
    - `dispose`
  - 只编排 draft 相关状态和 session 接管。
  - 不负责编排 runtime 重扫、挂载、删除。
- 把当前 draft snapshot 映射相关结构从巨型文件中抽出，视情况放在：
  - `draft_flow.rs`
  - 或 `network/dto.rs`
- 明确 “提交时 live session 从 draft 接管到 opened sessions” 的唯一代码路径。

阶段验收：

- draft 生命周期不再散在 cleanup、runtime、event 逻辑里交叉修改。
- 提交、取消、关闭对话框三条路径职责清晰。

#### 4.5.4 第四阶段：抽正式 runtime 生命周期流

目标：

- 把正式 `DiskRuntime` 的运行态操作独立出来。

任务：

- 新建 `src-tauri/src/network/runtime_flow.rs`
  - 只承接：
    - `mount_network_disk`
    - `eject_network_disk`
    - `prepare_deleted_network_runtime`
    - `rescan_network_runtimes`
  - 只编排正式运行态和 `NetworkMedia` 绑定。
  - 不再处理 draft 生命周期。
- 把 runtime invalid / unmounted 设置和 opened session 复用逻辑收口在这里。
- 保持“挂载时才创建 `NetworkMedia`”不变。
- 保持“删除后关闭 live disksession；拔出后保留 live disksession”不变。

阶段验收：

- runtime 生命周期和 draft 生命周期完全分层。
- `disk_service.rs` 不再持有 network 生命周期细节，只做分派。

#### 4.5.5 第五阶段：收口 commands 和 lib watcher

目标：

- 让 `commands/*` 和 `lib.rs` 回到薄桥接口径。

任务：

- `src-tauri/src/commands/network_disk.rs`
  - 只保留 request/response DTO 和到 `network/*` usecase 的调用。
- `src-tauri/src/commands/disk.rs`
  - 只保留普通磁盘 command 编排与 network runtime flow 分派。
- `src-tauri/src/lib.rs`
  - watcher 只调用正式事件收束入口，不直接依赖巨型 network service。
- 删除旧 `backend/network_service.rs`。
- 从 `backend/mod.rs` 中移除旧导出，改为导出新的真正 backend 模块。

阶段验收：

- `src-tauri` 的分层重新符合 “state / network flow / backend adapter / command bridge”。
- `commands/*` 中不再夹杂大量 network 业务细节。

### 4.6 state 层细化任务

目标：

- 保留 `NetworkClientState` 作为唯一真状态持有者，但把内部结构拆成更小的最小组件。

任务：

- 视复杂度把 `src-tauri/src/state/network_client.rs` 拆为目录：

```text
src-tauri/src/state/network_client/
  mod.rs
  connection_pool.rs
  opened_sessions.rs
  drafts.rs
  pending_events.rs
```

或在不改变公开结构的前提下先拆子对象再决定是否拆目录。

固定要求：

- `NetworkClientState` 仍然对外暴露唯一真状态对象。
- 连接池、opened sessions、draft、pending event queue 只是内部组件，不允许演变成多份状态 owner。
- 不能为拆分而复制字段或增加同步桥。

### 4.7 disk_service 收口任务

目标：

- 让 `disk_service.rs` 回到“普通盘主逻辑 + network 分派”定位。

任务：

- 检查并删除 `disk_service.rs` 中直接承担的 network 生命周期细节。
- 保留：
  - memory/file disk 的创建、挂载、拔出、删除准备、重扫
  - 对 network runtime flow 的分派
- 不保留：
  - network session cleanup 细节
  - network rescan 细节
  - network mount/eject 的直接编排细节

阶段验收：

- `disk_service.rs` 不再成为“所有磁盘类型的混合实现中心”。

### 4.8 前端详细拆分任务

#### 4.8.1 第一阶段：抽出组合式流程

任务：

- 新建 `src/features/createNetworkDisk/useNetworkDraftFlow.ts`
  - 承接：
    - 本地 loading / submitting / disposing 状态
    - `draftId` / `draftServerAddr` / `draftItems`
    - `handleTestConnection`
    - `handleAddDraftItem`
    - `handleRemoveDraftItem`
    - `handleSubmit`
    - `handleCancel` / `disposeCurrentDraft`
- 修正当前一个明显问题：
  - `disposeCurrentDraft` 失败时不能直接无条件清空本地 `draftId`
  - 需要保留可恢复的最小状态，避免 UI 丢失后端草稿引用

阶段验收：

- `CreateNetworkDiskDialog.vue` 不再承担完整流程编排。
- 对话框在 cleanup 失败时仍具备可恢复路径。

#### 4.8.2 第二阶段：抽出错误映射与子视图

任务：

- 新建 `src/features/createNetworkDisk/networkDraftError.ts`
  - 承接 network draft 错误码映射。
- 新建 `NetworkDraftForm.vue`
  - 承接输入区。
- 新建 `NetworkDraftList.vue`
  - 承接列表区。

阶段验收：

- 对话框壳文件只保留组合、布局、emit。
- form、list、error mapping 各自职责单一。

### 4.9 测试补强任务

当前 network 相关测试覆盖不足，本轮重构必须同步补测。

#### 4.9.1 Rust 侧必须补的测试

- draft 创建、添加、移除、提交、dispose 主链
- draft 提交时重复检查
- `SessionCloseNotice` 收到后的 runtime invalidation
- connection disconnect 后 draft 和 opened session 的收束
- media invalidation 触发后的统一收束
- rescan 对 live opened session 的复用
- rescan 对失效 session 的关闭与回收
- mounted / unmounted 状态下 delete 路径
- mount 时找不到 live session 的 invalid 路径

#### 4.9.2 前端侧必须补的测试

如果当前仓库已具备对应测试基础设施，则补：

- 测试连接成功/失败
- 添加 draft item 成功/重复/认证失败
- 提交成功与提交失败
- 关闭弹窗触发 dispose
- dispose 失败时 UI 不丢失草稿引用
- server_addr 改变时旧 draft 的释放链

如果当前前端还没有测试基础设施，则至少把这些场景记录进后续补测待办，不允许本轮重构后无覆盖说明。

### 4.10 文档同步任务

本轮实现完成后，需要同步更新以下文档：

- `docs/network/client/tauri-client.md`
  - 补充当前正式代码结构落点
  - 保持对象模型与策略口径不变
- `docs/progress/*.md`
  - 记录本轮重构进度与阶段结论
- 如有必要，在 `docs/tmp/` 补一份新的阶段草稿，但正式口径仍回到 `docs/network/client/`

固定要求：

- 文档只描述当前真实实现，不保留旧 `network_service.rs` 时代的历史结构幻影。

### 4.11 分阶段执行顺序

建议执行顺序固定为：

1. 抽 `validation / uniqueness / gateway_ops / cleanup`
2. 抽 `event_reconciler`
3. 抽 `draft_flow`
4. 抽 `runtime_flow`
5. 收口 `commands/*` 与 `lib.rs`
6. 收口 `disk_service.rs`
7. 删除旧 `network_service.rs`
8. 拆前端 `CreateNetworkDiskDialog.vue`
9. 补测试
10. 更新正式文档

固定原因：

- 先抽底层，再抽中层编排，最后清理入口，迁移风险最低。
- 如果一开始先改 command 或 UI，很容易在旧巨型服务仍然存在时继续叠加复杂度。

### 4.12 每阶段完成标准

每一阶段结束都要检查：

- 是否新增了第二份业务真状态
- 是否把边界约束收成唯一闸口
- 是否只是搬文件而没有真正拆职责
- 是否还残留旧入口、旧桥接、旧分支
- 是否补上了对应测试
- 是否需要同步更新正式文档

### 4.13 本轮最终验收口径

本轮完成后，应满足：

- 概念结构仍然与 `docs/network/client/tauri-client.md` 一致
- `NetworkClientState`、`DiskRuntime`、`NetworkMedia` 的职责比现在更清晰，不更模糊
- `commands/*` 明显变薄
- backend 目录不再承担 network 业务编排中心角色
- draft 生命周期、runtime 生命周期、事件收束、cleanup 各自拥有明确边界
- 前端创建网络盘 feature 拆成可读、可维护的最小组件
- 旧巨型 `network_service.rs` 被删除
- 文档、实现、测试三者重新收口到同一口径
