# 使用模型

## 总规则

`BackendRust` 继续只把 `NetworkMedia` 当作一个普通 `Media`。

前台看到的接口不变：

- `read_locked(offset, buffer)`
- `write_locked(offset, data)`

缓存层不要求 `BackendRust` 改成块语义，也不要求 `DiskSession` 了解上层字节语义。

## `ro` 和 `rw`

当前模型固定分成两条路径：

- `ro`
  - 继续直通
  - 不创建缓存状态
  - 不创建 temp
  - 不启动 worker
- `rw`
  - 进入缓存路径
  - 使用 resident、temp、worker 和背压规则

这版设计只讨论 `rw` 缓存。

## 读路径

`rw` 读请求的模型是：

1. `BackendRust` 传入任意 offset 和长度
2. `cache` 判断触达了哪些 `32 KiB` 逻辑块
3. 对每个块优先查 resident
4. resident miss 再查 `spilled_dirty`
5. 两边都没有才向 `DiskSession` 发 aligned read
6. 读到整块后，再把需要的字节区间返回给 `BackendRust`

这里有两条固定规则：

- 对内拉块时一律按整块处理，不做远端局部读
- 对外返回时仍然保留原始字节语义

## 写路径

`rw` 写请求的模型是：

1. `BackendRust` 传入任意 offset 和长度
2. `cache` 判断触达了哪些 `32 KiB` 逻辑块
3. 命中 resident 时，直接在 resident 块上打 patch
4. miss 时，先拿到完整块视图，再应用 patch
5. patch 完成后只修改 resident 数据和脏位
6. 远端发送由后续 flush 推进

这版固定不允许“构造一个不完整块再直接远端写”。

也就是说：

- partial write 仍然可以对外暴露
- 但缓存内部必须先得到完整块，再生成规范块状态

## 与 `BackendRust` 的配合

这层仍然遵守当前 `BackendRust` 运行时的边界：

- 读路径里，`BackendRust` 先读 `Media`，再叠加它自己的 staging overlay
- 写路径里，AppKernel 的写碎片仍然先留在 `BackendRust staging`
- `cache` 实际看到的是 staging commit 之后的正式 `write_locked()`

所以 `cache` 不需要直接接半包写碎片，只需要处理已经进入 `Media` 边界的正式读写。

## 写成功的语义

引入缓存后，`write_locked()` 成功的含义会收窄为：

- 本地缓存已经接受这次写

它不再等价于：

- 远端已经确认这次写

这也是当前缓存模型和原始直通模型最大的语义区别。

