# `NetworkMedia` 接入 `cache` 执行清单

## 0. 当前范围

本清单只服务于一件事：

- 把已经完成的 `cache/` 独立组件接回 `NetworkMedia`

这里只讨论接线层内容：

- `cache` 左侧怎么接 `NetworkMedia`
- `cache` 右侧怎么接 `DiskSession`
- `NetworkMedia` 需要调整哪些结构和职责
- 需要补哪些模块
- 生命周期和清理链如何收住

本清单不再讨论：

- 协议字段
- 业务状态码
- 业务层 I/O 语义展开
- 压缩、拆片和 wire 细节
- `cache` 内部算法细节
- `BackendRust` 对外接口改造

固定以 [docs/cache/network-media-cache-structure.svg](../cache/network-media-cache-structure.svg) 为正式模型。

## 1. 当前起点

### 1.1 `cache` 组件现状

当前根目录 `cache/` crate 已经具备完整最小闭环：

- 左侧 `read_locked(offset, buffer)`
- 左侧 `write_locked(offset, data)`
- 右侧 `AtIo::read_at(offset, buffer)`
- 右侧 `AtIo::write_at(offset, data)`
- resident `2Q = FIFO + LRU`
- dirty -> temp -> flush -> spilled -> rehydrate
- temp 限流、前台 miss 背压、后台 flush worker

因此本轮不应该再把缓存细节散回 `NetworkMedia`。

### 1.2 `NetworkMedia` 当前现状

当前 `rust-cli` 和 `tauri-client` 里的 `NetworkMedia` 仍然是：

- 直接持有 `DiskSession`
- 直接持有 `disk_size_bytes`
- 直接持有 `read_only`
- 自己承接直通读写
- 出现 terminal error 时由自己触发 invalidation handler

当前主线还是：

```text
BackendRust <-> NetworkMedia <-> DiskSession
```

还没有把 `cache` 接进去。

## 2. 当前总目标

按最薄接线完成下面这条主线：

1. `NetworkMedia` 内部显式容纳 `mux + cache + session`
2. `mux` 只负责 `ro` 直通 / `rw` 缓存分流
3. `cache` 左侧继续承接 `BackendRust::Media`
4. `cache` 右侧只依赖一个 `DiskSession` 适配层
5. `DiskSession` 继续完全不知道 resident、temp、flush worker
6. `BackendRust` 继续不感知 `cache` 的内部状态机

完成后正式结构应保持为：

```text
BackendRust <-> NetworkMedia
                    |
                    +-> mux
                    +-> cache
                    +-> DiskSessionAtIo
                          |
                          +-> DiskSession
```

## 3. 固定边界

### 3.1 `NetworkMedia` 的边界

`NetworkMedia` 继续是唯一宿主对象，负责：

- 承接 `BackendRust::Media`
- 持有 `disk_size_bytes`
- 持有 `read_only`
- 持有 invalidation handler
- 持有 `DiskSession` 生命周期
- 决定当前实例走 `ro` 还是 `rw`
- 创建并持有 `cache`、`mux`、`DiskSessionAtIo`

`NetworkMedia` 不负责：

- 实现 resident 逻辑
- 实现 temp 管理
- 实现 flush worker
- 把 `cache` 内部状态暴露给 `BackendRust`

### 3.2 `mux` 的边界

`mux` 只负责：

- 判定当前盘走 `ro` 直通还是 `rw` 缓存路径
- 复用同一个 `DiskSession` 及其宿主生命周期
- 依据 `disk_size_bytes` 对外层请求做真实范围前置判断

`mux` 不负责：

- 块对齐
- 字节请求拆块
- 尾块补零或裁剪
- dirty 状态管理
- temp 文件管理
- flush worker

### 3.3 `cache` 的边界

`cache` 接入后继续只做它自己的事情：

- 左侧接受字节语义
- 右侧产出按块对齐的 `_at` I/O
- 管 resident / temp / snapshot / spilled / worker

`cache` 明确不做：

- 不直接依赖 `BackendRust`
- 不直接依赖 `DiskSession`
- 不知道 `disk_id / remote_disk_id / session_id`
- 不持有 invalidation handler

### 3.4 `DiskSessionAtIo` 的边界

`DiskSessionAtIo` 是 `cache` 右侧的薄适配层，负责：

- 把 `DiskSession` 包成 `AtIo`
- 承接右侧容量和块边界适配
- 处理最后短尾块的本地补齐和裁剪
- 把底层错误映射成 `cache` 可消费的错误
- 在终态错误时把失效上抛回 `NetworkMedia`

`DiskSessionAtIo` 不负责：

- resident 逻辑
- temp 生命周期
- flush 策略
- session cleanup 总控

### 3.5 `DiskSession` 的边界

`DiskSession` 接入后仍然只负责远端 I/O 与关闭动作。

它不应该因为接入 `cache` 而新增：

- resident 状态
- temp 文件语义
- flush 控制
- cache 命中行为

### 3.6 `BackendRust` 的边界

`BackendRust` 继续只看到：

- `Media::size_bytes()`
- `Media::read_locked()`
- `Media::write_locked()`

本轮不把 `CacheError`、`CacheSnapshot`、`AtIo` 这些类型直接扩散到 `BackendRust` 宿主边界。

## 4. 推荐接入模型

### 4.1 `NetworkMedia` 内部最小模型

推荐把当前直接持有的直通读写逻辑收成一个很薄的内部 `mux`，最小模型可以是：

- `BypassRo`
  - `ro` 直通
  - 继续直接走 `DiskSession`
- `CachedRw`
  - `rw` 路径
  - `NetworkMedia` 只把调用转给 `cache`

关键点只有一条：

- `rw` 路径不再让 `NetworkMedia` 自己做缓存细节

### 4.2 `DiskSessionAtIo` 适配层

`cache` 右侧不应直接吃 `DiskSession`，而应补一层很薄的 `AtIo` 适配器。

这层适配器至少要负责：

- 把 `DiskSession` 包成 `AtIo`
- 用 `disk_size_bytes` 和 `block_size_bytes` 推导右侧逻辑容量
- 命中最后一个不足整块的尾块时，只读真实前缀并在本地补 `0`
- 命中最后一个不足整块的尾块时，只写真实前缀并丢弃超出真实 EOF 的尾部
- 对右侧逻辑范围做防御性越界校验
- 把宿主侧错误映射成 `CacheError`
- 遇到 terminal error 时，把失效上抛回 `NetworkMedia` 的 invalidation handler

固定口径：

- `cache` 左侧看不到 `DiskSession`
- `DiskSession` 看不到 `CacheConfig`
- invalidation 仍由 `NetworkMedia` 统一收束
- 对外真实越界仍应由 `mux` 在进入 `cache` 前先挡掉

### 4.3 `CacheConfig` 和 `temp_dir`

`cache` 仍然只吃上层准备好的 `CacheConfig`。

因此 `NetworkMedia` 接线时必须补齐：

- `block_size_bytes`
- `fifo_capacity_blocks`
- `lru_capacity_blocks`
- `dirty_scan_interval`
- `temp_max_files`
- `temp_dir`

这里固定一条边界：

- `cache` 不决定 temp 根目录结构
- temp 目录仍由更上层 runtime / 持久化配置 / 本地实例目录准备

### 4.4 `ro` 路径保持直通

`ro` 路径不创建 `cache`，不创建 temp，不启动 worker。

这样收的好处是：

- 保持 `ro` 简单
- 不引入无意义的 temp 生命周期
- 不把缓存状态混进只读盘

### 4.5 `rw` 路径接 `cache`

`rw` 路径进入 `cache` 后，`NetworkMedia` 只保留这些宿主职责：

- 保持 `disk_size_bytes`
- 保持 `read_only` 边界
- 保持 invalidation handler
- 保持 `DiskSession` 生命周期
- 决定当前实例是否创建 `cache`

`cache` 则负责：

- 字节请求拆块
- resident 命中
- temp / spill / rehydrate
- 右侧块对齐 I/O

## 5. 需要补的模块

### 5.1 `NetworkMedia` 内部路径模型

需要补一个很薄的路径模型，用于：

- 收出 `ro` 旁路逻辑
- 收出 `rw` 缓存逻辑入口
- 固定 `NetworkMedia` 内部的双路径结构

### 5.2 `DiskSessionAtIo`

需要补一个右侧适配层，用于：

- 让 `cache` 右侧只看到 `AtIo`
- 屏蔽 `DiskSession` 细节
- 吃掉宿主侧的容量和尾块适配逻辑

### 5.3 `CacheConfig` 组装入口

需要补一个接线层配置入口，用于：

- 从宿主配置构造 `CacheConfig`
- 为每个缓存实例准备 `temp_dir`
- 固定 `cache` 初始化参数来源

### 5.4 生命周期清理入口

需要补一个统一清理入口，用于：

- 固定 session close / eject / invalidation 时的 stop 顺序
- 固定等待中的请求在 stop 时如何退出
- 固定 `cache`、`DiskSessionAtIo`、`DiskSession` 的释放关系

## 6. `NetworkMedia` 需要调整的点

`NetworkMedia` 至少需要完成下面这些调整：

- 从“直接直通读写宿主”调整成“`mux + cache + session` 宿主”
- 保留 `BackendRust::Media` 的唯一承接位置
- 把 `ro` 和 `rw` 的入口在内部显式分开
- 在 `rw` 路径把读写转给 `cache`
- 在 `ro` 路径继续保持简单直通
- 创建并持有 `DiskSessionAtIo`
- 创建并持有 `CacheConfig`
- 创建并持有缓存实例级 `temp_dir`
- 继续统一收口 invalidation handler
- 继续统一收口 `DiskSession` 生命周期

## 7. 推进顺序

建议按下面顺序推进：

1. 先收 `NetworkMedia` 内部最薄 `mux`
2. 再补 `DiskSessionAtIo`
3. 再补 `CacheConfig` 和 `temp_dir` 接线
4. 再把 `rw` 正式接进 `cache`
5. 再把 `ro` 直通路径收稳
6. 最后补生命周期和清理链

## 8. 验收口径

本轮完成后，至少要满足下面这些结果：

1. `NetworkMedia` 结构正式变成 `mux + cache + session`
2. `mux` 只做 `ro` 直通 / `rw` 缓存分流
3. `BackendRust` 仍只看到 `Media`
4. `cache` 左侧仍是字节语义，右侧仍是整块 `_at` I/O
5. `DiskSession` 仍然只是远端 I/O 通道
6. `ro` 不创建 cache、不创建 temp、不启动 worker
7. `rw` 正式通过 `cache` 承接读写
8. terminal error 的 invalidation 语义不回退
9. 不把 `cache` 内部状态扩散到 `network-core`
10. 对外真实容量判断仍留在 `NetworkMedia` / `mux`
11. 最后一个不足整块的尾块仍由 `DiskSessionAtIo` 负责适配
