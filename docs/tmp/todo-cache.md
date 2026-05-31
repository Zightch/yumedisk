# `cache` 组件实现执行清单

## 0. 当前范围

本清单只服务于根目录 `cache/` 这个独立 Rust 组件。

本轮总目标只有两个：

- 把 `cache` 组件本身实现完整
- 为后续接回 `NetworkMedia` 保留最薄适配面

本清单不承接虚拟测试座子、测试矩阵和 testhook 设计；这些内容已经独立收口到 [todo-test-socket.md](./todo-test-socket.md)。

## 1. 当前总目标

按最小核心闭环完成下面这条主线：

1. `cache` 对左侧只暴露 `read_locked()` / `write_locked()`
2. `cache` 对右侧只依赖 `read_at()` / `write_at()`
3. `cache` 左侧接受任意 offset 和任意长度
4. `cache` 右侧始终只发块大小对齐、固定长度的 I/O
5. resident 按 `2Q = FIFO + LRU` 管理
6. dirty block 能经过 temp、flush、spilled、rehydrate 完成完整闭环

如果这个组件本身跑通，后续接回 `NetworkMedia` 只保留一层很薄的 `mux` 和接口适配。

## 2. 固定边界

### 2.1 对外边界

`cache` 当前只保留这几类公开边界：

- `CacheConfig`
- `Cache`
- 左侧
  - `read_locked(offset, buffer)`
  - `write_locked(offset, data)`
- 右侧依赖
  - `read_at(offset, buffer)`
  - `write_at(offset, data)`

### 2.2 当前明确不做

- 不让 `cache` 自己管理 `session_id`、`disk_id`、`remote_disk_id`
- 不让 `cache` 自己决定 temp 根目录结构
- 不让 `cache` 直接依赖 `BackendRust`
- 不让 `cache` 直接依赖 `DiskSession`
- 不让 `cache` 直接依赖网络错误类型
- 不先做 `NetworkMedia` 接线之外的上层业务拼装
- 不先做真实网络联调

### 2.3 temp 目录口径

`cache` 只接受上层给好的 `temp_dir`。

目录命名、目录隔离、测试清理、实例目录准备，都由上层或测试座子负责。

`cache` 自己只负责：

- 在这个目录下创建 temp 文件
- 复用 temp 文件
- 删除 temp 文件

## 3. 实现原则

本清单按 [开发原则](../development/development-principles.md) 执行，重点如下：

- 极简核心原则
  - 先跑通一条最小闭环：`read/write -> resident -> right-side aligned io`
  - 先不引入多套模式和额外扩展点
- 单一真实来源原则
  - resident / spilled / temp / active snapshot 各自只有一份真状态
- 边界闸口原则
  - 右侧对齐和固定长度约束，只允许在 `cache` 右侧出口统一收口

## 4. 数据结构固定口径

### 4.1 resident 主表

resident block 主表固定按：

- `HashMap<block_index, BlockEntry>`

这里的 `BlockEntry` 持有：

- resident 数据本体
- 缓存块 tag 信息

### 4.2 缓存块 tag 信息

缓存块的 `tag/state` 和块数据本体要明确分开。

原因很直接：

- tag 负责描述块当前处于什么状态
- data 负责承载真实块内容
- 后续队列移动、提升、淘汰时，应尽量只动 tag，不动 data

这里必须严格按“单一真实来源”收口：

- 一些派生信息不能再额外存一份
- resident 内不保存 `Missing`
- resident 内不保存 `spilled`
- 不单独保存“是否 dirty”“是否有 snapshot”“是否有 pending patch”这类可由真实字段直接推导出的布尔态

当前建议 resident block 只保留最小核心状态：

- `queue_ref`
  - 当前块在哪个队列，以及它的节点句柄
  - 例如 `Fifo(node)` / `Lru(node)`
- `load_state`
  - 只保留 resident 内真实需要推进的状态
  - 例如 `LoadingRemote / Rehydrating / Ready`
- `dirty_ranges`
  - 这是 dirty 真状态
- `active_snapshot`
  - `Option<ActiveSnapshot>`
  - snapshot 是否存在由 `Option` 本身表达
- `pending_patches`
  - patch 是否存在由集合是否为空表达
- `valid_len`
  - 仅当当前实现没有别的唯一真来源可推导尾块长度时才保留

下面这些字段不再单独保存：

- `block_index`
  - resident 主表 key 已经是 `block_index`
- `queue_kind`
  - 由 `queue_ref` 直接表达
- `access_state`
  - 当前 2Q 里是否已提升，可由当前在 `FIFO/LRU` 直接表达
- `Missing`
  - 不在 resident 主表里就是 `Missing`
- `spill_state`
  - spilled 真状态只存在于 `spilled_dirty` 表
- `dirty bool`
  - 由 `dirty_ranges` 是否为空直接表达
- `active_snapshot_state bool`
  - 由 `active_snapshot.is_some()` 直接表达
- `pending_patch_state bool`
  - 由 `pending_patches` 是否为空直接表达

这里再固定一条实现口径：

- resident data 和 resident state 同属 `BlockEntry`
- resident 主表是块数据和块状态的唯一真来源
- 队列层只持有块索引或块句柄，不直接持有块数据副本
- 队列永远不是数据所有者，只是调度顺序视图

### 4.3 `LRU`

`LRU` 不用标准库 `LinkedList`，也不引第三方容器。

初版固定为：

- `HashMap + 自己实现的双向链表`

理由很直接：

- LRU 需要按块 O(1) 查找
- 需要按块 O(1) 移尾
- 需要按块 O(1) 删除

标准 `LinkedList` 不适合做这件事，因为它不提供适合这里的稳定节点句柄。

### 4.4 `FIFO`

语义上 `FIFO` 仍然只是普通队列：

- 新块进尾
- 淘汰从头

但实现上建议和 `LRU` 复用同一套“双向链表节点 + 索引”骨架，而不是单独再造第二套容器。

理由：

- FIFO block 命中后要提升到 LRU
- 提升时需要从 FIFO 中 O(1) 按块删除
- 复用同一套链表骨架，代码会更少，边界也更统一

### 4.5 队列移动不复制块数据

这条规则单独固定下来：

- `FIFO -> LRU` 提升时，不允许复制块数据
- `LRU` 命中移尾时，不允许复制块数据
- queue 之间的移动，本质上只能是 tag / 节点重挂接

推荐实现口径：

- resident 主表持有唯一块数据
- FIFO / LRU 只保存 `block_index` 或 `BlockEntry` 句柄
- 提升时只做：
  - 从 FIFO 摘节点
  - 挂到 LRU 尾部
  - 更新 `queue_ref`
  - 更新节点句柄

不允许做的事：

- 把整块 `Vec<u8>` 从 FIFO 复制一份到 LRU
- 因为队列提升而新分配第二份 resident 数据
- 用“两个容器各存一份块对象”的方式实现 2Q

## 5. 工作任务拆分

## 5.1 第一阶段：最小空壳和边界定型

目标：

- 把 crate 空壳和公开接口先立住

任务：

- [x] 创建根目录 `cache/` crate
- [x] 建立最小 `CacheConfig`
- [x] 建立最小 `Cache`
- [x] 建立右侧 `read_at` / `write_at` trait
- [x] 建立左侧 `read_locked` / `write_locked` stub
- [x] 跑通基础编译

当前这一步已经完成。

## 5.2 第二阶段：块计算和基础类型

目标：

- 把所有“字节请求 -> 触达块集合”的基础计算收成唯一入口

任务：

- [x] 定义 `block_size`、`block_index`、`block_base`、`valid_len` 的基础模型
- [x] 实现触达块计算
  - 输入任意 `offset + len`
  - 输出按顺序触达的逻辑块视图
- [x] 实现块内切片定位
  - 读时从整块裁出用户需要的字节
  - 写时把 patch 落到整块里的对应区间
- [x] 实现右侧对齐校验辅助
  - 统一断言右侧 `_at` I/O 必须块对齐、固定长度
- [x] 定义基础错误类型

固定实现要求：

- 同一套块计算同时服务读路径和写路径
- 不在多个地方重复做块映射逻辑

当前这一步已经完成。

## 5.3 第三阶段：2Q 基础容器

目标：

- 先把 resident 的纯内存命中、提升、淘汰骨架做好

任务：

- [x] 实现通用双向链表骨架
  - `push_back`
  - `pop_front`
  - `remove(node)`
  - `move_to_back(node)`
- [x] 实现 `FIFO`
- [x] 实现 `LRU`
- [x] 实现 resident block 表
- [x] 定义 resident 最小状态结构
- [x] 定义 `QueueRef`
  - `Fifo(node)`
  - `Lru(node)`
- [x] 实现 2Q 基础策略
  - 新块首次进入 FIFO
  - FIFO 再命中提升到 LRU
  - LRU 命中移到尾部
- [x] 实现 victim 选择骨架

固定实现要求：

- 不接磁盘 I/O，也能独立验证 2Q 行为
- FIFO/LRU 状态不会分叉
- FIFO -> LRU 提升不发生块数据内存拷贝
- 不保存可以由唯一真状态直接推导出的派生字段

当前这一步已经完成。

## 5.4 第四阶段：最小读闭环

目标：

- 先只做 read path，把 cache 变成真正可运行的只读缓存

任务：

- [x] resident hit 直接返回
- [x] resident miss 从右侧拉整块
- [x] 整块装入 resident
- [x] 回填给左侧请求需要的字节区间
- [x] 读命中时更新 2Q
- [x] 同块并发 read miss 不重复发右侧读

当前这一步已经完成。

## 5.5 第五阶段：最小写闭环

目标：

- 在不做 flush 的前提下，先把 write path 的 resident patch 跑通

任务：

- [x] resident hit 直接 patch
- [x] resident miss 先补整块再 patch
- [x] dirty 标记先建立最小可用模型
- [x] 写命中更新 2Q
- [x] 同块 loading 期间的后续写不重复补块
- [x] 先建立 pending patch 骨架

当前这一步已经完成。

## 5.6 第六阶段：temp 文件和 dirty 基础状态

目标：

- 把 dirty 数据离开 resident 前必须先落 temp 这条规则收实

任务：

- [x] 设计 temp 文件命名规则
- [x] 实现 temp 文件创建、覆盖、删除
- [x] 建立 `active snapshot` 基础结构
- [x] 建立 `spilled dirty` 基础结构
- [x] 实现“先写 temp，再清对应 dirty bit”的流程骨架
- [x] 实现 dirty eviction 的 temp 前置规则

当前这一步已经完成。

## 5.7 第七阶段：flush worker 和周期策略

目标：

- 把 resident dirty 和 spilled dirty 的后台推进闭环做完整

任务：

- [x] 建立 worker 基础循环
- [x] 支持按周期扫描 dirty
- [x] 支持 resident dirty 生成 snapshot
- [x] 支持 active snapshot 重试
- [x] 支持 spilled dirty 重试
- [x] 固定处理优先级
  - 已有 temp
  - resident dirty 周期扫描

当前这一步已经完成。

## 5.8 第八阶段：淘汰、阻塞和排队

目标：

- 把“cache 满 + temp 满”时的真实推进逻辑补齐

任务：

- [x] clean victim 直接淘汰
- [x] dirty victim 必须先 spill
- [x] temp 满时，dirty eviction 相关 miss 在右侧拉块前阻塞
- [x] `read hit` 不被误阻塞
- [x] `write hit` 不被误阻塞
- [x] temp slot 释放后正确唤醒等待请求
- [x] terminal / stopping 时正确退出等待
- [x] 等待 dirty eviction 的前台 miss 优先于 resident dirty 周期扫描

当前这一步已经完成。

## 5.9 第九阶段：接回 `NetworkMedia`

目标：

- 当 `cache` 组件本身稳定后，再做最薄接线

任务：

- [ ] 在 `NetworkMedia` 内接入 `cache`
- [ ] `mux` 只做 `ro` 直通 / `rw` 缓存分流
- [ ] 把当前 `DiskSession` 适配到 `cache` 右侧 `_at` trait
- [ ] 把 `BackendRust Media` 调用适配到 `cache` 左侧 `_locked`
- [ ] 保持 `cache` 本体不引入 `NetworkMedia` 业务语义

## 6. 推荐落地顺序

建议严格按下面顺序推进，不要并行摊太开：

1. 第二阶段：块计算和基础类型
2. 第三阶段：2Q 基础容器
3. 第四阶段：最小读闭环
4. 第五阶段：最小写闭环
5. 第六阶段：temp 文件和 dirty 基础状态
6. 第七阶段：flush worker 和周期策略
7. 第八阶段：淘汰、阻塞和排队
8. 独立推进 [todo-test-socket.md](./todo-test-socket.md)
9. 第九阶段：接回 `NetworkMedia`

## 7. 当前建议

当前 `cache` 主线实现已经推进到第八阶段。

下一步建议是：

1. 按 [todo-test-socket.md](./todo-test-socket.md) 把虚拟测试座子和 testhook 收稳
2. 等 `cache` 自身行为测透后，再推进第九阶段接线
