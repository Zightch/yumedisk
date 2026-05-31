# `cache` 虚拟测试座子与 testhook 清单

## 0. 当前范围

本清单只服务于根目录 `cache/` 组件的本地虚拟测试。

这里讨论的内容包括：

- 虚拟测试座子
- testhook 框架
- 测试矩阵
- 时序竞争稳定复现
- 压力测试与故障注入

这里明确不做：

- 真实网络联调
- `NetworkMedia` 正式接线
- 把测试专用能力混进正式公开 API

## 1. 当前总目标

按最小核心闭环，把 `cache` 的测试体系收成下面几件事：

1. 不走真实网络，也能把 `cache` 核心行为独立测透
2. 不靠 CPU 速度碰运气，而是稳定摆出关键竞争时序
3. 能看见 2Q、dirty、temp、waiter 等内部现场，方便定位
4. 正式环境不保留 testhook，不扩大正式职责边界

## 2. 固定边界

### 2.1 testhook 只存在于测试构建

testhook 不进入正式公开接口，不进入正式运行时配置。

固定口径：

- 不加到 `CacheConfig`
- 不改主线公开边界
- 只放在 `#[cfg(test)]` 或 `#[cfg(any(test, feature = "test-hooks"))]`
- 正式构建默认不启用

### 2.2 testhook 只能观察和阻塞

testhook 不能修改业务状态，不能变成第二套状态机。

允许做的事：

- 导出内部快照
- 在关键阶段挂闸门
- 记录事件

不允许做的事：

- 直接改 resident 状态
- 直接改 queue 顺序
- 直接补写 dirty/temp/spilled 元数据

### 2.3 快照不新增真状态

内部状态导出只能从现有真状态现算，不为了调试再额外存一份派生字段。

比如下面这些信息，只能在导出时计算：

- 当前块在 FIFO 还是 LRU
- 当前块是否 dirty
- 当前块是否有 active snapshot
- 当前块是否有 pending patch

### 2.4 时序测试不能靠 `sleep`

关键竞争测试必须靠可控事件和闸门推进，不靠 CPU 调度速度去撞。

`sleep` 只允许用于：

- 很粗的 worker 周期等待
- 非关键路径的最终清理等待

不允许把 `sleep + assert` 当作主要竞争测试手段。

## 3. testhook 结构

### 3.1 `state dump hook`

作用：

- 导出可断言的内部快照
- 出错时打印现场
- 帮助确认 2Q、dirty、temp、waiter 的真实状态

建议导出内容：

- FIFO 顺序
- LRU 顺序
- resident 块集合
- 每块 `load_state`
- 每块 dirty 范围
- 每块 `active_snapshot` 是否存在
- 每块 `pending_patches` 数量
- `spilled_dirty` 块集合
- `active_temp_blocks`
- `foreground_dirty_eviction_waiters`
- `stop_requested`

默认不要导出整块数据。

如果后续确实需要观测数据内容，优先考虑：

- 块长度
- 校验值
- 摘要

而不是直接把整块数据全量打出来。

### 3.2 `gate hook`

作用：

- 把关键时序卡在稳定边界上
- 让测试线程精确决定何时放行
- 让竞争测试可重复、可定位

建议结构：

- 测试侧创建 gate 控制器
- `cache` 在指定阶段进入 gate
- 测试线程等待“已到达”
- 测试线程手动 `release`

### 3.3 hook 点选择原则

hook 点要少，但要卡在稳定边界上。

优先卡下面这些位置：

- 选 victim 前后
- dirty victim 准备 spill 前后
- temp 写成功但 dirty bit 尚未清之前
- dirty bit 清理之后
- worker 选中“已有 temp flush”时
- worker 尝试“新建 resident snapshot”之前
- rehydrate 抢占 temp 之前
- miss 进入 `dirty eviction temp wait` 之前
- temp slot 释放、即将唤醒 waiter 之前
- stop 即将广播唤醒之前
- 右侧 `read_at` / `write_at` 发起前后

## 4. 虚拟测试座子

## 4.1 右侧假设备

至少做两种：

- 内存假设备
- 文件假设备

两者都要满足：

- 实现右侧 `read_at()` / `write_at()`
- 记录结构化调用日志
- 支持精确故障注入
- 支持对单次 I/O 加 gate

### 4.2 `_at` 调用日志

日志至少包含：

- 自增序号
- `read` / `write`
- `offset`
- `len`
- 对应 `block_index`
- 调用结果

这份日志要能直接回答下面几类问题：

- 是否所有右侧 I/O 都块对齐、固定长度
- 某次 miss 是否在拿到 temp slot 前偷跑远端读
- flush / retry / rehydrate 的顺序是否符合设计

### 4.3 temp 目录夹具

测试座子要自己管理临时目录，不把目录职责压回 `cache`。

夹具至少负责：

- 创建独立测试目录
- 提供可控的 temp 路径
- 测试结束后清理
- 支持“删除失败”类故障注入

### 4.4 左侧请求驱动

左侧需要可复用的测试驱动，至少覆盖：

- 单线程顺序读写
- 多线程并发读写
- 指定顺序的交错请求
- 等待 / 放行 / 停止 的协同控制

### 4.5 quiesce 辅助

测试座子最好提供 quiesce 辅助，用来判断：

- 当前前台请求已经结束
- 当前后台 flush 已经空闲
- 当前 temp / spilled 状态已经稳定

没有这类辅助，最终一致性断言会很难写稳。

## 5. 故障注入

至少覆盖下面几类失败：

- 右侧 `read_at` 失败
- 右侧 `write_at` 失败
- temp 写失败
- temp 读失败
- temp 删失败

故障注入要支持：

- 指定第 N 次调用失败
- 指定某个块失败
- 失败一次后恢复
- 持续失败直到测试主动解除

## 6. 测试矩阵

## 6.1 `P0` 契约层

这是正式语义层，默认前台调用已串行，配一个后台 worker。

重点覆盖：

- 基础正确性
  - 单块读写
  - 跨块读写
  - 尾块读写
  - partial write 后回读
  - quiesce 后远端最终一致
- 配置口径
  - 不同 `block_size`
  - 不同 `fifo/lru` 容量
  - 不同 `temp_max_files`
- 2Q 策略
  - 首次进入 FIFO
  - FIFO 再命中提升到 LRU
  - LRU 命中移尾
  - victim 选择正确
  - resident 数量不越界
- dirty / temp / flush
  - dirty eviction 前必须先有 temp
  - temp 写成功后才能清 dirty bit
  - spilled dirty 可以 rehydrate
  - active snapshot 成功后删除 temp
  - active snapshot 失败后保留 temp 并重试
- 背压排队
  - `read hit` 直通
  - `write hit` 直通
  - clean victim miss 直通
  - dirty victim + temp 满时在远端读前阻塞
  - temp 释放后等待 miss 被唤醒
  - `stop` 时等待 miss 退出
- 优先级
  - 已有 temp flush 优先于 resident dirty 新建 snapshot
  - 前台 dirty eviction waiter 优先于 worker 新建 resident snapshot

## 6.2 `P1` 鲁棒层

这是防误用和防回归层，不作为正式边界契约，但建议补齐。

重点覆盖：

- 多前台同块读并发
- 多前台同块写并发
- 多前台跨块并发
- flush 中同块再写
- rehydrate 与 worker flush 抢同一个 temp
- 多个等待者同时等待 temp slot
- `stop` 与前台请求、后台 worker 交错

这层重点不是定义新语义，而是确认：

- 不死锁
- 不状态分叉
- 不出现明显资源泄漏

## 6.3 `P2` 压力与随机层

这层主要找长时间运行下的边角问题。

重点覆盖：

- 小容量高冲突配置下长时间随机读写
- 随机 worker tick 与前台请求交错
- 随机右侧失败和 temp 失败
- 高频阻塞与唤醒
- 长时间运行后的 temp 文件清理

最终检查至少包括：

- quiesce 后数据与参考模型一致
- resident 块数不越界
- temp 文件数不越界
- 没有遗留等待者
- 没有明显线程卡死

## 7. 全局不变量

每类测试都应尽量复用下面这些统一断言：

- 所有右侧 `_at` I/O 都必须块对齐、固定长度
- resident 块数永远不超过 `fifo + lru`
- temp 文件数永远不超过 `temp_max_files`
- 同一块同一时刻最多一个 active snapshot
- 同一 temp 文件同一时刻最多一个使用者
- 没有 miss 会在未拿到必要 temp slot 前偷跑远端读
- `stop` 之后不会留下卡死等待者

## 8. 推荐落地顺序

建议分三轮推进，不要一开始就上全量混战：

1. 先搭 testhook 骨架
2. 再搭内存假设备、日志、故障注入、quiesce 辅助
3. 先补 `P0` 契约层
4. 再补 `P1` 鲁棒层
5. 最后补 `P2` 压力与随机层

## 9. 当前任务拆分

### 9.1 第一阶段：testhook 最小骨架

任务：

- [x] 建立 test-only hook 模块
- [x] 建立 `state dump hook`
- [x] 建立 `gate hook`
- [ ] 建立最小测试侧控制器
- [x] 明确 hook 点枚举

当前已完成：

- `cache` crate 已增加 `test-hooks` feature，默认关闭
- 已建立 `test_support/` 模块与最小 hooks/snapshot 类型
- 已建立 `CacheDeps` 内部 seam，不改正式 `Cache::new(...)` 边界
- 已增加 `Cache::new_for_test(...)` 与 `debug_snapshot()`
- 已把第一批 hook 点接到：
  - `debug_snapshot`
  - 右侧 `read_at / write_at` 前后
  - dirty victim spill temp 写前后

当前未完成：

- 还没有真正可控的 gate 测试控制器
- 还没有把更多时序卡点接入主流程

### 9.2 第二阶段：虚拟右侧与日志

任务：

- [ ] 做右侧内存假设备
- [ ] 做右侧文件假设备
- [ ] 做 `_at` 结构化调用日志
- [ ] 做 temp 目录夹具
- [ ] 做 quiesce 辅助

### 9.3 第三阶段：故障注入

任务：

- [ ] 做右侧读失败注入
- [ ] 做右侧写失败注入
- [ ] 做 temp 写失败注入
- [ ] 做 temp 读失败注入
- [ ] 做 temp 删失败注入

### 9.4 第四阶段：`P0` 契约层补齐

任务：

- [ ] 收齐基础正确性
- [ ] 收齐配置口径
- [ ] 收齐 2Q 策略
- [ ] 收齐 dirty / temp / flush
- [ ] 收齐背压排队
- [ ] 收齐优先级时序

### 9.5 第五阶段：`P1` 鲁棒层补齐

任务：

- [ ] 收齐多前台并发交错
- [ ] 收齐 rehydrate / flush temp 争用
- [ ] 收齐 stop 交错退出

### 9.6 第六阶段：`P2` 压力与随机层

任务：

- [ ] 建立随机操作驱动
- [ ] 建立参考字节模型
- [ ] 长时间压力回归
- [ ] 统一全局不变量检查
