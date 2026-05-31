# 缓存策略

## 块模型

缓存内部只认固定逻辑块：

- 块大小不写死在组件内部，由配置决定
- 块起点固定按当前配置块大小对齐
- 不允许滑动窗口块

一个字节请求进入缓存后，先映射成若干逻辑块，再在块级状态机里推进。

## `2Q` 策略

resident 只维护两条队列：

- `FIFO`
- `LRU`

规则固定如下：

- 新块首次装入 resident，进入 FIFO 尾部
- FIFO 中的块再次命中，提升到 LRU 尾部
- LRU 中的块再次命中，移动到 LRU 尾部

这版不再引入更复杂的多级队列或自适应参数。

## 块状态

单块在缓存里只需要几个核心状态：

- `Missing`
  - resident 和 spilled 都没有
- `Loading`
  - 正在补整块或 rehydrate
- `ResidentClean`
  - resident 中有整块，没有脏位
- `ResidentDirty`
  - resident 中有整块，并且带脏位
- `ResidentSnapshotOwned`
  - resident 中仍有块，但已有 active flush snapshot 在发送
- `SpilledDirty`
  - resident 中已经没有，temp 是唯一权威副本

这些状态的目标是把读 miss、写 miss、dirty flush 和 dirty eviction 收进同一套块模型。

## 读策略

读策略很直接：

- resident hit
  - 直接从 resident 取数，并更新 `2Q`
- resident miss + spilled hit
  - 先按 spilled 规则 rehydrate 或等待发送结果
- 两边都 miss
  - 发 aligned read，把整块装入 resident

同一块在 `Loading` 期间不重复发远端读，后续同块请求只等待已有加载完成。

## 写策略

写策略的核心只有一句话：

- 写 miss 先补整块，再打 patch

具体落地为：

- resident hit
  - 直接覆盖块内范围
  - 把对应区间标成 dirty
- resident loading
  - 不重复补块
  - 先挂起 patch，等整块回来后统一应用
- resident miss
  - 先拿完整块
  - 再按顺序应用 patch
  - 最终把块放入 resident dirty

这样可以保证 cache 右侧始终只处理规范整块。

## 淘汰策略

当前版先把淘汰口径收紧：

- 淘汰只发生在 `read miss / write miss` 需要装入新块时
- 周期扫描不会主动淘汰 resident 块

两类 victim 的处理不同：

- clean victim
  - 直接淘汰
- dirty victim
  - 必须先生成或复用 temp snapshot
  - temp 成为唯一权威副本后，才能离开 resident

如果 dirty 块还没有满足可淘汰条件，就不能把它当作可用 victim。

## flush 策略

flush 和淘汰是两件事。

flush 的目的，是把 dirty 数据逐步推向远端；它本身不负责自动淘汰 resident。

当前固定规则：

- 同一块同一时刻只允许一个 active flush snapshot
- snapshot 发送期间，这块后续新写仍然只改 resident
- temp 成功落盘之后，才能清这次 snapshot 覆盖到的脏位
- flush 成功后删 temp；失败后保留 temp 并进入重试
- worker 每轮先发送已有 temp，再决定是否为 resident dirty 新建 snapshot

这套规则的重点，是把“正在发送的数据”和“resident 上后续新写的数据”分开管理。
