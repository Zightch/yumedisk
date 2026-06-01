# 三机联调测试清单

## 0. 定位

这份清单只服务于当前这一轮联调：

- 按 `docs/development/joint-debugging-environment.md` 跑一组 `gateway + storer + rust-cli + rust-cli`
- 重点观察 `cache` 介入后 `ReadAt / WriteAt` 的触发时机、块对齐、`60KiB` 拆片、spill / rehydrate / flush
- 固定只走管理员 `PhysicalDriveN` 原始盘路径
- 先不碰文件系统

## 1. 固定拓扑

- 本机
  - 常驻 `gateway`
  - 常驻 `storer`
  - 管理员 PowerShell 负责抓 `tcp.port=9736`
- `vm_win11_admin`
  - 常驻 `rust-cli`
  - 默认承担 `rw + cache` 侧
- `vm_win10_admin`
  - 常驻 `rust-cli`
  - 默认承担 `ro` 对照侧

固定约束：

- `gateway`、`storer`、`rust-cli` 全部用交互终端常驻
- 两侧客户端都只通过管理员原始盘句柄访问 `\\.\PhysicalDriveN`
- 不在目标盘上建分区或文件系统
- 若 `cc` 改了 cache 默认配置，必须重新 `auth` 挂载；已存在的 cache 实例保持旧配置不变

## 2. 默认观测口径

### 2.1 默认 cache 参数

`M0-M5`、`M7-M9` 默认使用：

```text
cc fifo=1 lru=1 temp=1 block=32768 scan=30000
```

目的：

- resident 很小，便于快速打出 victim / spill / rehydrate
- `scan=30000ms`，把后台 flush 明显拉开，方便观察 `read-before-write` 和 flush 时机

`M6` 单独切到：

```text
cc fifo=1 lru=1 temp=1 block=98304 scan=30000
```

目的：

- 强制单块右侧 I/O 跨过 `60KiB raw` 上限
- 观察单个 cache block 被拆成 `61440 + 36864`

### 2.2 抓包口径

本机默认不依赖 Wireshark，直接使用管理员 `pktmon`：

```powershell
pktmon filter remove
pktmon filter add yume9736 -t TCP -p 9736
pktmon start --capture --pkt-size 0 --file-name C:\Users\Zightch\Desktop\driver\tmp\pktmon-9736.etl --log-mode memory
```

结束后导出：

```powershell
pktmon stop
pktmon etl2pcap C:\Users\Zightch\Desktop\driver\tmp\pktmon-9736.etl --out C:\Users\Zightch\Desktop\driver\tmp\pktmon-9736.pcapng
```

固定观测点：

- `rust-cli` 挂载输出里的 `raw_limit_bytes=61440`
- 本机 `9736` 抓包里的 `ReadAt / WriteAt` 次数与长度
- `ro` 侧 `TUR + 原始读` 何时看到新字节
- `%TEMP%\yumedisk-network-media\<server_addr>\<remote_disk_id>\` 的 temp 文件变化

## 3. 执行矩阵

### M0 挂载基线

目标：

- 先确认三机联调主链稳定，`rw` / `ro` 角色正确

操作：

- 本机起 `gateway`、`storer`
- `rw` 侧按默认小缓存长周期参数执行 `cc`
- `rw` 侧 `auth` 挂载
- `ro` 侧 `auth` 挂载

观测：

- 两边都成功拿到 `PhysicalDriveN`
- `rw` 侧 `read_only=false`
- `ro` 侧 `read_only=true`
- `mounted ... raw_limit_bytes=61440`

### M1 冷读单块

目标：

- 观察首次读 miss 只补一次整块

操作：

- `rw` 侧对一个未访问过的块执行原始读
- 推荐：`offset=4096`，`length=512`
- 紧接着对同一偏移再读一次

观测：

- 第一次读触发 1 次按块对齐的远端 `ReadAt`
- 第二次同块读不再触发新的远端 `ReadAt`
- 抓包里同块只有第一次出现真正远端读

### M2 冷读跨块

目标：

- 观察跨块请求如何映射成多个块级补读

操作：

- `rw` 侧一次原始读跨越两个 cache block
- 长度保持原始盘可接受的扇区对齐

观测：

- 远端读按块边界拆成 2 次
- 两次 `ReadAt.offset` 都落在块起点
- 不出现滑动窗口式非对齐补读

### M3 冷写 `read-before-write`

目标：

- 观察写 miss 先补整块，再打 patch

操作：

- `rw` 侧对一个冷块做 `512B` 原始写
- 推荐：`offset=4096`
- 写完后立即在 `rw` 侧原始回读

观测：

- 远端先发生 1 次整块 `ReadAt`
- 本地随后接受写入
- `rw` 侧立刻能读到新字节
- `ro` 侧此时未必立刻看到更新

### M4 flush 时机

目标：

- 在长扫描周期下明确新字节对另一侧可见的最早时机

操作：

- 延续 `M3` 的脏块
- `ro` 侧在 `t+0 / +100ms / +500ms / +2s / +30s` 依次执行：
  - `TUR`
  - 同偏移原始读

观测：

- `ro` 侧第一次看到新字节的时间点
- 该时间点前后抓包里的远端 `WriteAt`
- `data changed` 与真实字节可见时间是否一致

### M5 写命中不重复补块

目标：

- 观察 dirty resident 命中后不再重复补读

操作：

- 在 `M3` 同一块上再做第二次原始写
- patch 到同块不同偏移

观测：

- 第二次写不再触发新的远端 `ReadAt`
- 只保留后续 flush / drop 时的远端 `WriteAt`

### M6 大块跨 `60KiB`

目标：

- 明确单个 cache block 右侧 I/O 如何跨 `60KiB raw` 拆片

操作：

- 先 `rm` 卸载旧盘
- `cc fifo=1 lru=1 temp=1 block=98304 scan=30000`
- 重新 `auth` 挂载
- 对一个冷块做读或写，落在同一个 `98304B` 逻辑块内

观测：

- 单个块级远端 I/O 被拆成两笔：
  - `61440`
  - `36864`
- 两笔 offset 连续且不重叠
- 证明 `block_size` 可以大于协议 raw 上限，而右侧会自行拆片

### M7 小容量触发 spill

目标：

- 观察 dirty victim 进入 temp / spilled 路径

操作：

- 回到默认小缓存参数
- 连续改写三个不同块：`A / B / C`
- `fifo=1, lru=1, temp=1` 下强制 resident 压力

观测：

- temp 目录出现 spill 痕迹
- 至少一个旧 dirty block 不再留在 resident
- 新块 miss 时会等待可用 victim / temp slot

### M8 rehydrate

目标：

- 验证 spilled dirty block 重新被访问时不会回退旧字节

操作：

- 在 `M7` 后回读最早被挤出的块 `A`

观测：

- 读回仍是最新数据
- 抓包和 temp 目录变化能够对应到 rehydrate / flush 后续动作
- 不出现“旧远端字节覆盖新脏块”的回退

### M9 drop flush

目标：

- 验证 dirty 数据在卸载时会尽量完成 quiesce / flush

操作：

- 保持至少一个 dirty 块尚未自然 flush
- `rw` 侧执行 `rm target=<id>`

观测：

- 卸载前抓包里出现最后的远端 `WriteAt`
- 卸载后 `ro` 侧原始读能看到最终新字节
- temp 目录被清理

## 4. 执行顺序

首轮固定顺序：

1. `M0`
2. `M1`
3. `M2`
4. `M3`
5. `M4`
6. `M5`
7. `M6`
8. `M7`
9. `M8`
10. `M9`

若中途要切换 `block` 或 `scan`，固定流程为：

1. `rm target=<id>`
2. 执行新的 `cc ...`
3. 重新 `auth`

## 5. 当前不纳入本轮

这轮先不做：

- 文件系统层验证
- 普通文件读写行为
- 自动重连 / 自动 reopen
- UI 层行为
- `M10` 尾块专项
- `M11` `ro` 长期旁路对照扩展项
