# tauri-client 网络盘迁移草案

## 1. 目标

本轮目标不是把 `rust-cli` 的网盘宿主逻辑原样搬进 `tauri-client`，而是按当前正式网络文档和开发原则，把网盘能力重建为 `tauri-client` 的正式宿主能力。

固定收口如下：

- `tauri-client` 成为网盘卡片、运行时、重扫、挂载的正式宿主。
- `rust-cli` 当前已有的纯网络 client 能力迁入新的共享核心，不在两个宿主里长期保留两份分叉实现。
- `tauri-client` 主页与创建对话框只消费唯一运行时真状态，不自己维护一套平行网络状态。
- 当前阶段不扩展 server 协议，不在 network 正式文档上追加 tauri 专属协议语义。

## 2. 开发原则约束

本草案固定遵循以下原则：

- 按“激进更新原则”收口，不为旧 `rust-cli` 宿主结构保留长期双轨。
- 按“单一真实来源原则”收口，connection、auth、disksession、磁盘运行态都必须有唯一持有者。
- 按“删除优先原则”收口，不在 `tauri-client` 里再复制一套 `CliHost` 风格的混合宿主。
- 按“文档跟随实现原则”收口，后续实现如果采用本草案，正式 client/server/network 文档也要同步改成同一套口径。

## 3. 命名与状态收口

### 3.1 名词边界

- `appsession`
  - 只表示 `BackendRust + KMDF` 的本机会话。
- `connection`
  - 只表示一条 `client-gateway` 业务连接。
- `auth_id`
  - 只表示一次成功认证得到的授权对象。
- `disksession`
  - 只表示一条已打开的网络盘会话。
- `NetworkMedia`
  - 只表示把已打开 `disksession` 接到 `BackendRust::Media` 的适配对象。

### 3.2 `local_disk_id / remote_disk_id`

`tauri-client` 后续固定改名如下：

- 本地磁盘卡片、`DiskRuntime`、持久化记录上的磁盘标识统一叫 `local_disk_id`
- 网络协议和网络盘语义里的盘标识统一叫 `remote_disk_id`

注意：

- 协议线上 `AuthStart` 请求体字段仍然是 network 文档定义的 `disk_id`
- 进入 `tauri-client` 本地模型后，必须立即投影成 `remote_disk_id`
- `tauri-client` 内部不再继续用单个 `disk_id` 同时表示本地盘和远端盘

这样收是为了避免：

- `DiskRuntime` 本地身份
- 远端真实网盘身份
- UI 卡片身份

三者混在一起，后续再靠注释区分。

### 3.3 `server_addr`

`tauri-client` 后续统一使用 `server_addr` 作为领域名。

固定要求：

- UI 中文文案统一写“服务器地址”
- 本地模型字段统一写 `server_addr`
- 不再使用 `server_ip` 作为领域字段名

当前 UI 如果先只允许输入 IP，这只是输入约束，不改变领域命名。

### 3.4 状态口径

主页网络盘卡片仍只保留三态：

- `mounted`
- `unmounted`
- `invalid`

当前最小闭环下：

- 只要当前 `local_disk_id` 找不到已打开 `disksession`，就是 `invalid`
- 找得到已打开 `disksession` 但还没挂到 `BackendRust`，就是 `unmounted`
- 已挂到 `BackendRust`，就是 `mounted`

### 3.5 `auth_material`

当前最小闭环下：

- `auth_material` 直接保存原始 `claim_code`

这属于当前实现策略，不改变 network 正式文档对认证材料来源的留白口径。

## 4. 需要迁移的模块

## 4.1 直接迁移为共享网络核心

应从 `windows/rust-cli/src/network/` 提炼出共享网络核心，固定收口到现有独立 crate：

- `transport_client.rs`
- `hello_client.rs`
- `protocol_client.rs`
- `gateway_connection.rs`
- `connection_authenticator.rs`
- `session_opener.rs`
- `session_describer.rs`
- `disk_session.rs`
- `crypto_win32.rs`
- `error.rs`

固定落点：

- `windows/network-core/`

固定原因：

- 这些文件本质上是纯网络 client 栈
- 它们不应该继续寄生在 `rust-cli` 宿主目录下
- `tauri-client` 和 `rust-cli` 不能长期各维护一份拷贝

## 4.2 不原样迁移的模块

以下内容不能直接照搬：

- `windows/rust-cli/src/network/network_media.rs`
- `windows/rust-cli/src/cli/host.rs`
- `windows/rust-cli/src/cli/shell.rs`
- `windows/rust-cli/src/bin/network-auth-open.rs`

原因：

- `NetworkMedia` 属于宿主适配层，不属于纯网络核心
- `CliHost` 把 connection 池、session 清理、宿主建盘、命令交互混在一起，不符合 `tauri-client` 的状态分层
- shell 和 bin 只是 `rust-cli` 的交互外壳，不进入 `tauri-client`

## 5. tauri-client 目标结构

`tauri-client` 侧固定拆成四层：

### 5.1 共享网络核心层

职责：

- `Hello -> transport`
- `ConnHeartbeat`
- auth/open lane 互斥
- `auth_id` / `session_id` 生命周期
- `SessionCloseNotice` / disconnect 事件

不负责：

- 主页卡片
- `DiskRuntime`
- 对话框草稿状态
- `BackendRust` 建盘

### 5.2 网络运行时层

建议新增 `NetworkClientState`，作为唯一网络真状态持有者。

它至少固定持有：

- `connection_pool`
  - key 为 `server_addr`
- `opened_disk_sessions`
  - key 为 `(server_addr, remote_disk_id)`
- `network_create_drafts`
  - key 为 `draft_id`

它的结构定位固定为：

- `NetworkClientState` 是多 connection 的全局协调层
- 它不是单个 connection 管理器
- 它不要求采用 `N 线程 = N connection` 这种固定线程模型

并发边界固定为：

- 每个 `server_addr` 必须拥有独立执行 lane
- 不同 `server_addr` 的建连、重扫、auth、open、读写、故障收束可并发推进
- 同一 `server_addr` 内，仍遵守单 connection 的 auth/open lane 互斥
- `NetworkClientState` 不允许把多服务器操作收成一个全局串行大锁

也就是说，这里固定的是“多 connection + 每 connection 独立 lane”的模型，不固定线程实现细节。

这层负责：

- 连接复用
- 已打开 `disksession` 复用
- draft 生命周期
- `SessionCloseNotice` / disconnect 到本地状态的收束

这层不负责：

- UI 文本拼装
- `BackendRust` managed disk 生命周期

### 5.3 磁盘运行时与持久化层

`DiskRuntime` 和持久化配置需要新增 `Network` 介质变体。

最小字段固定为：

- `local_disk_id`
- `disk_name`
- `server_addr`
- `remote_disk_id`
- `auth_material`
- `capacity_bytes`
- `read_only`
- `auto_mount`

其中：

- `DiskRuntime` 只描述“本地这张盘卡片应该是什么”
- 它不是 live `disksession` 的持有者
- live `disksession` 统一归 `NetworkClientState`
- `auth_material` 当前最小闭环直接保存原始 `claim_code`

### 5.4 宿主适配层

`NetworkMedia` 留在 `tauri-client` 宿主层，职责固定为：

- 显式持有 `remote_disk_id`
- 显式持有目标 `disksession`
- 显式持有 `SessionDescribe` 得到的 metadata
- 在挂载时把网络盘接成 `BackendRust::Media`

它不负责：

- 建连
- 认证
- 自动重连
- 重扫
- UI 状态管理

## 6. UI 与命令口径

### 6.1 主页卡片

网络盘卡片固定显示：

```text
+-------------------------------+
|   磁盘名称              [状态]|
|N  网络盘·容量                 |
|   服务器地址·remote_disk_id   |
+-------------------------------+
```

固定要求：

- 状态文本继续沿用 `已挂载 / 未挂载 / 无效`
- 卡片头像字母固定为 `N`
- 详情文本固定为 `server_addr · remote_disk_id`

### 6.2 添加磁盘入口

顶部添加气泡中的“网络盘”选项要放开。

启用条件固定为：

- 与文件盘、内存盘一致
- 只有 `appsession` 正常时才允许点击

### 6.3 `appsession` 命名

`tauri-client` 当前前后端里叫 `session` 的本机会话命名，需要整体改成 `appsession`，避免与网络 `disksession` 冲突。

固定要求：

- `SessionStatus` 概念改成 `AppSessionStatus`
- `sessionPhase` 改成 `appSessionPhase`
- `sessionStatusText` 改成 `appSessionStatusText`
- `open_session` / `restore_client_state` 等命令后续都要按 `appsession` 语义重命名

## 7. 创建网络盘对话框主链

## 7.1 页面结构

页面固定遵循当前草图：

- `server_addr` 输入框与“测试连接”按钮在滚动区外
- 名称、领盘码、添加按钮在滚动区外
- 已添加磁盘卡片列表在滚动区内
- 取消、提交按钮也在滚动区内
- 即使没有任何临时卡片，滚动区里也要显示取消、提交按钮

实现层优先复用当前 `tauri-client` 现有 `Element Plus` 对话框与表单骨架，不另造一套新弹层体系。

## 7.2 初始状态

刚打开页面时：

- 只允许用户输入 `server_addr`
- 其他输入框和按钮全部 disabled
- 必须先点击“测试连接”

## 7.3 测试连接

测试连接流程固定为：

1. 先按 `server_addr` 查 `connection_pool`
2. 若已有可用 connection，直接返回测试成功
3. 若没有，则执行 `Hello -> transport` 建连
4. 成功后返回测试成功，并启用后续输入区
5. 失败只提示“测试连接失败”，不展示详细错误

## 7.4 添加临时网盘

点击“添加”后固定执行：

```text
authenticate(claim_code)
  -> SessionOpen(auth_id)
  -> SessionDescribe(session_id)
  -> 全局重复检查
  -> 写入 draft item
```

重复检查固定规则：

- 网盘以 `(server_addr, remote_disk_id)` 做全局唯一检查
- 不允许重复添加同一远端盘
- 检查范围至少覆盖：
  - 已存在正式 `DiskRuntime`
  - 当前创建对话框 draft 项

若命中重复：

- 当前添加流程直接失败
- 关闭本次临时打开出来的 `disksession`
- 不写入 draft 项
- 不额外显式做 connection cleanup

成功后：

- 在当前对话框草稿列表中新增一张临时卡片
- 临时卡片至少显示 `disk_name`、`remote_disk_id`、`capacity_bytes` 和删除按钮

失败后：

- 只提示错误类型
- 当前最小闭环只区分：
  - 认证错误
  - 会话打开失败
  - metadata 获取错误

不要求展示详细错误文本。

## 7.5 删除临时卡片

删除临时卡片时固定执行：

- 关闭该临时项对应的 `disksession`
- 从当前 draft 列表移除这张临时卡片

但固定不执行 connection cleanup。

这里要额外固定一条：

- 这类关闭属于 draft 内部关闭
- 它不进入普通运行态 `disksession close -> connection cleanup` 闸口
- 对应空闲 connection 的回收统一延后到“创建网络盘对话框消失”时处理

这样收口的原因是：

- draft 页面内部可能还会继续添加当前 `server_addr` 下的其他盘
- 如果删除一张临时卡片就顺手回收 connection，很容易让 draft 状态与连接池状态反复抖动
- draft 生命周期内把 cleanup 延后到统一闸口，更符合“单一真实来源原则”

## 7.6 提交

点击提交后固定执行：

1. 把当前 draft 内保留下来的临时项写成正式 `DiskRuntime`
2. 创建正式主页卡片
3. 默认状态写成 `invalid`
4. 不自动创建 `NetworkMedia`
5. 不直接把 `DiskRuntime` 和 live `disksession` 绑死
6. 自动触发一次网络盘重扫任务

提交不再单独定义“提交时 cleanup”这条特殊分支。

统一改为：

- 提交导致对话框消失
- 对话框消失进入统一 draft 收尾闸口

提交前需要再做一次防御性重复检查：

- 若 `(server_addr, remote_disk_id)` 与正式盘或当前待提交集冲突，则提交失败
- 不能依赖前面“添加时检查一次”就认为后续不会出现冲突

## 7.7 对话框消失

当前 `tauri-client` 额外增加一个统一 connection cleanup 闸口：

- 创建网络盘对话框消失时

它覆盖：

- 点击取消
- 点击提交
- 点击右上角关闭

固定要求：

- 若对话框取消或直接关闭，则关闭该 draft 持有的全部临时 `disksession`
- 若对话框提交，则保留已被正式接管的 live `disksession`
- 无论哪种消失路径，最后都统一执行一次 draft 级 connection cleanup sweep

这个 sweep 只负责清理：

- 没有活跃 `disksession`
- 没有 open 过程
- 没有 auth 过程
- 没有未消费 `auth_id`

的 draft 相关 connection。

## 8. 重扫、挂载、拔出主链

## 8.1 网络盘重扫模型

网络盘重扫必须改成：

- 先汇总
- 再分发
- 异步执行

不允许把网络盘塞回文件盘当前那种逐盘同步探测结构里。

重扫时固定流程：

1. 遍历全部 `DiskRuntime`
2. 内存盘、文件盘按现有逻辑处理
3. 网络盘先把任务项加入 `network_rescan_task`
4. 遍历结束后统一提交 `network_rescan_task`

任务项最少包含：

- `local_disk_id`
- `server_addr`
- `remote_disk_id`
- `auth_material`

## 8.2 网络盘重扫执行

网络重扫任务固定流程：

1. 按 `server_addr` 汇总并去重
2. 先确保该 `server_addr` 对应的 connection 存在
3. 建连失败则该地址下所有网络盘都置为 `invalid`
4. 对每个网络盘先查 `opened_disk_sessions`
5. 若已有 `(server_addr, remote_disk_id)` 对应 `disksession`，直接标为 `unmounted`
6. 若没有，则执行 `auth -> open -> describe`
7. 成功则写入 `opened_disk_sessions` 并标为 `unmounted`
8. 失败则标为 `invalid`

## 8.3 挂载

网络盘挂载固定为：

1. `DiskRuntime` 按 `local_disk_id` 找到自己的网络配置
2. 再到 `NetworkClientState` 里按 `(server_addr, remote_disk_id)` 找 live `disksession`
3. 若找不到，则该盘视为 `invalid`
4. 若找到，则按该 `disksession + metadata` 现建 `NetworkMedia`
5. 调用 `BackendRust` 创建 managed disk

固定要求：

- `DiskRuntime` 不持有 live `NetworkMedia`
- live `NetworkMedia` 只在真正挂载时构造

## 8.4 拔出

网络盘拔出固定只做：

- 从 `BackendRust` 移除 managed disk
- 把卡片状态改回 `unmounted`

当前最小闭环下：

- 拔出不关闭 live `disksession`
- 拔出不主动回收 connection

## 8.5 编辑

网络盘编辑边界固定为：

- 只允许修改 `disk_name`
- 只允许修改 `auto_mount`

当前明确不允许通过编辑修改：

- `server_addr`
- `remote_disk_id`
- `auth_material`

如果用户要改变这些字段，固定走：

- 删除旧盘
- 重新添加新盘

不引入“编辑后原地迁移 connection / disksession”的复杂路径。

## 8.6 删除

网络盘删除固定流程为：

1. 若当前已挂载，先 eject
2. 若当前存在 live `disksession`，则关闭该 `disksession`
3. 删除对应 `DiskRuntime` 与持久化记录

固定要求：

- 删除后需要清理 live `disksession`
- 删除后不额外显式调用 connection cleanup
- 是否关闭 connection 只交给 `disksession close` 路径自动判定

## 8.7 故障路线

这里固定保留两层口径。

### 8.7.1 留白口径

网络层只负责把以下事件上报给 `DiskRuntime` 或宿主事件接口：

- `SessionCloseNotice`
- connection 死亡
- `disksession` 进入 closed

网络层不直接规定：

- client 必须立即清理
- client 必须保留挂死态
- client 必须自动重连

也就是说：

- `disksession` 关闭后，仅调用 `DiskRuntime` 的事件通知接口
- 具体是清理、挂死还是做别的处理，交给 client 当前实现策略决定

### 8.7.2 当前最小闭环实现

当前 `tauri-client` 采用的最小闭环策略固定为：

- 已挂载或未挂载的网络盘，只要收到 `SessionCloseNotice` 或 connection 死亡，就直接转为 `invalid`
- 若当前已挂载，则先 eject
- 清理对应 live `disksession`
- 清理挂载侧 `NetworkMedia`
- `DiskRuntime` 状态改为 `invalid`
- 不主动重连
- 不后台感知服务器是否重新上线
- 后续只能通过 rescan 再走 `bootstrap -> auth -> open -> describe`

也就是说，无效网盘不会自己恢复，恢复入口统一收在重扫逻辑。

## 9. connection cleanup 收口

`tauri-client` 当前 connection cleanup 闸口固定为两类：

### 9.1 `disksession` 关闭路径

这是正式长期闸口，适用于普通运行态。

只有当某个 `disksession` 明确进入 closed 后，才允许检查是否关闭它所在的 connection。

这里不包含：

- 创建网络盘对话框内删除临时卡片时产生的 draft `disksession` 关闭

### 9.2 创建网络盘对话框消失路径

这是当前 `tauri-client` 为 draft 生命周期额外增加的统一闸口。

它只服务于：

- draft 页面里被删除过的临时项
- draft 页面里未提交的临时项
- draft 页面关闭后的尾部空闲 connection 清理

也就是说：

- draft 临时项即使已经先关闭 `disksession`
- 也仍然等到对话框消失时再统一做 connection cleanup

### 9.3 删除路径补充

正式网络盘删除路径固定为：

- 删除时关闭 live `disksession`
- 关闭后由 `disksession close` 路径自动判定是否释放 connection
- 删除逻辑自己不额外显式调用 connection cleanup

### 9.4 明确禁止

当前阶段明确不做：

- 删除临时卡片时立即做 connection cleanup
- 任意时刻只要观察到 connection 暂时空闲就立刻关闭
- 独立后台 idle sweeper
- 一边在 UI 维护 draft 状态，一边在别处悄悄回收 connection

## 10. 分阶段实施

## Phase T1: 提炼共享网络核心

需要完成：

- 从 `rust-cli` 抽出纯网络 client 核心并统一收口到 `windows/network-core/`
- `tauri-client` 改为依赖该共享核心
- `rust-cli` 后续只作为调试壳或逐步删除其网盘宿主职责

## Phase T2: `appsession` 与命名收口

需要完成：

- `session` 全量收成 `appsession`
- `disk_id` 本地命名全量收成 `local_disk_id`
- 网络盘本地投影命名全量收成 `remote_disk_id`

## Phase T3: `NetworkClientState`

需要完成：

- 新增 `connection_pool`
- 新增 `opened_disk_sessions`
- 新增 `network_create_drafts`
- 明确 notice / disconnect 收束入口
- 明确多 connection + 每 connection 独立 lane 的并发模型

## Phase T4: `DiskRuntime` 与持久化扩展

需要完成：

- 新增 `DiskMediaConfig::Network`
- 配置文件新增网络盘持久化结构
- 首页 DTO 支持网络盘卡片展示

## Phase T5: 创建网络盘对话框后端闭环

需要完成：

- `test_connection`
- `add_network_disk_draft_item`
- `remove_network_disk_draft_item`
- `submit_network_disk_draft`
- `dispose_network_disk_draft`

并把 connection cleanup 时机固定为本草案定义的两个闸口。

## Phase T6: 重扫与挂载闭环

需要完成：

- 网络盘汇总式异步重扫
- 网络盘按 `opened_disk_sessions` 进入 `unmounted`
- 网络盘挂载时现建 `NetworkMedia`
- 网络盘拔出后保留 `disksession`
- 网络盘删除后关闭 `disksession` 并走 session-close cleanup 判定
- `SessionCloseNotice` / connection 死亡后把盘收为 `invalid`

## Phase T7: UI 接入

需要完成：

- 主页放开“网络盘”创建入口
- 新增创建网络盘对话框
- 主页卡片切到网络盘展示格式
- 错误提示收成当前最小错误分类
- 编辑对话框只开放名称和自动挂载

## 11. 测试与验收

至少补齐：

- 同一 `server_addr` 下复用同一条 connection 添加多个不同 `remote_disk_id`
- 不同 `server_addr` 下的建连、auth、open、重扫可以并发推进
- 一个 connection 下多个已打开 `disksession` 并存
- 已打开网络盘 A 时，仍可继续对网络盘 B 做 auth/open
- 重复添加 `(server_addr, remote_disk_id)` 会被拒绝
- 删除临时卡片后不触发 connection cleanup
- 对话框消失时会统一回收 draft 产生的空闲 connection
- 提交后不会自动挂载，但重扫后可转为 `unmounted`
- 找不到 `disksession` 的网络盘会显示 `invalid`
- 网络盘拔出后保留 live `disksession`，状态回到 `unmounted`
- 网络盘删除后会关闭 live `disksession`，并仅通过 session-close 路径判定 connection cleanup
- `SessionCloseNotice` 或 disconnect 到来后，对应网络盘会转为 `invalid`
- `SessionCloseNotice` 或 disconnect 到来后，若盘当前已挂载，会先 eject，再清理 live `disksession` 和挂载侧 `NetworkMedia`
- 网络盘编辑只允许修改名称和自动挂载
- `auth_material` 持久化保存原始 `claim_code`
- `appsession` 未就绪时，网络盘创建入口不可点击

最终验收：

- `cargo test` 通过
- `tauri-client` 主页与创建流程对 network/client/server 正式文档的口径保持一致
