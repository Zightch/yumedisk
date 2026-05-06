# AppKernel 分层重构草案

## 1. 目标

把当前 `RWTestApp` 里已经混在一起的三类职责拆开：

1. `KMDF` 控制会话与协议交互
2. per-disk 队列线程与 slot/ACK 数据面推进
3. 实际介质与业务策略

重构后的目标口径是：

- `AppKernel` 用纯 `C` 写
- `AppKernel` 只负责和 `YumeDiskKMDF` 打交道，以及 per-disk 队列线程管理
- 实际介质不再放在 `AppKernel` 内
- 介质、LBA overlap 策略、建盘业务、CLI、压测辅助都上移到业务层

这次重构不是为了做平台化，而是因为当前 `RWTestApp/main.cpp` 已经同时承担：

- 控制面
- 数据面
- 介质
- 业务编排
- CLI
- 测试辅助

职责已经明显混杂，符合 `development-principles.md` 里的主动分层重构条件。

## 2. 重构原则

严格按当前开发原则执行：

- 不做新旧双轨长期共存
- 不做 DLL / 插件 / 可切换多实现这种超前设计
- 不把重构做成“只是搬文件”
- 必须形成清晰三层：底层最小能力、中层协作内核、上层业务编排

当前只做一种实现：

- `AppKernel` 是唯一数据面内核
- 业务层通过回调把介质能力提供给 `AppKernel`

## 3. 目标分层

### 3.1 底层最小能力层

这一层只放稳定、低语义、和业务无关的能力。

建议内容：

- `KMDF` 设备打开/关闭
- `QueryInfo / CreateDisk / RemoveDisk / RemoveAllDisks / QueryDebugState`
- heartbeat 发送
- 同步/异步 `DeviceIoControl`
- `OVERLAPPED` 生命周期
- 线程启动/停止/Join
- 基础日志出口
- 协议结构编解码

这一层不允许知道：

- medium 是内存、文件还是别的介质
- 哪个 target 对应哪个业务对象
- LBA overlap 策略
- CLI 命令

### 3.2 中层协作内核层

这一层就是 `AppKernel` 本体。

它负责：

- `session` 生命周期
- per-disk worker 组织
- `POST_READ_SLOT / POST_WRITE_SLOT / READ_ACK / WRITE_ACK_BATCH`
- per-disk read workers / write workers / write-ack flush worker
- queue depth 分片
- per-disk in-flight 状态
- 和业务层回调协作完成 read/write

它不负责：

- 持有介质 bytes
- 介质锁
- 业务元数据
- 驱动包安装/修复
- 设备实例收敛
- benchmark 命令拼接

### 3.3 上层业务编排层

这一层先仍然放在 `RWTestApp` 里。

它负责：

- 单实例
- 驱动包安装与设备实例收敛
- 介质对象创建/销毁
- read/write 回调实现
- LBA overlap 一致性策略
- `ct / rm / ls / debug / exit`
- bench/压测辅助

这层可以继续用 `C++`。

## 4. 单一真实来源

重构后状态归属必须固定：

- `KMDF session` 状态：只在 `AppKernel session` 内
- per-disk worker / queue / in-flight slot 状态：只在 `AppKernel disk runtime` 内
- 介质内容和介质锁：只在业务层 disk object 内
- 驱动与设备实例收敛状态：只在上层业务启动流程内

禁止重构后出现：

- `AppKernel` 再存一份 medium
- 业务层再镜像一份 slot runtime 状态
- 业务层和 `AppKernel` 同时维护 pending ACK 状态

## 5. AppKernel 边界

## 5.1 AppKernel 对外职责

`AppKernel` 对外只暴露三类能力：

1. 打开/关闭 `KMDF` session
2. 创建/删除一个由业务层提供介质回调的 disk runtime
3. 查询运行态和统计

## 5.2 AppKernel 不做的事情

明确不放进 `AppKernel`：

- 驱动包安装
- devnode 创建/去重
- visible path / physical drive 枚举
- medium 内存分配
- medium stripe lock
- benchmark 命令生成
- CLI 解析

## 6. 业务层和 AppKernel 的接口

建议接口形状保持极简，只保留真实需要的回调。

```c
typedef struct APPK_SESSION APPK_SESSION;
typedef struct APPK_DISK APPK_DISK;

typedef struct APPK_DISK_GEOMETRY {
    uint32_t target_id;
    uint32_t sector_size;
    uint64_t disk_size_bytes;
    uint32_t queue_depth;
    uint32_t write_slot_bytes;
} APPK_DISK_GEOMETRY;

typedef struct APPK_READ_OP {
    uint64_t event_id;
    uint32_t target_id;
    uint64_t lba;
    uint32_t block_count;
    uint32_t data_length;
} APPK_READ_OP;

typedef struct APPK_WRITE_OP {
    uint64_t event_id;
    uint32_t target_id;
    uint32_t seq;
    uint32_t total_seq;
    uint64_t lba;
    uint32_t byte_offset_in_write;
    uint32_t data_length;
} APPK_WRITE_OP;

typedef struct APPK_DISK_CALLBACKS {
    long (*read_blocks)(
        void* user_ctx,
        const APPK_READ_OP* op,
        void* out_buffer,
        uint32_t* out_data_length);

    long (*write_blocks)(
        void* user_ctx,
        const APPK_WRITE_OP* op,
        const void* data_buffer);

    void (*log_line)(
        void* user_ctx,
        int level,
        const char* text);
} APPK_DISK_CALLBACKS;
```

核心约束：

- `read_blocks` 由业务层负责真正读介质
- `write_blocks` 由业务层负责真正写介质
- `AppKernel` 只负责把 slot 事件和 ACK 协议跑完

## 7. 取消模型

这个拆分后，取消模型不变：

- 系统侧取消只追到 `SCSI`
- `AppKernel` 不负责构造额外的全链路取消协议
- 业务层不需要感知“系统已经取消了某个请求”
- `AppKernel` 继续按正常路径把 `READ_ACK / WRITE_ACK_BATCH` 发回去
- 是否 stale / cancelled / not found，仍由 `SCSI` 在 ACK 到达时决定

这点很重要：

- 业务层只实现介质读写
- 不把系统取消语义上浮到业务层

否则边界会重新变脏。

## 8. 线程模型

`AppKernel` 内部维护当前唯一数据面模型：

- per-disk `read workers`
- per-disk `write workers`
- per-disk `write ack flush worker`

业务层不允许再自己包一层“外部数据面线程池”。

也就是说：

- 业务层实现 read/write 回调
- `AppKernel` 决定什么时候调用这些回调

这样线程所有权才单一。

## 9. 目录建议

第一阶段不要新建独立 DLL 或独立仓库。

建议先在当前 `RWTestApp` 目录下收敛：

```text
windows/tests/RWTestApp/
  main.cpp
  appkernel/
    appkernel.h
    appk_session.c
    appk_control.c
    appk_disk.c
    appk_read_worker.c
    appk_write_worker.c
    appk_ack_flush.c
    appk_overlapped.c
    appk_protocol.c
    appk_internal.h
```

上层业务先保持在：

```text
windows/tests/RWTestApp/
  main.cpp
  business_disk.cpp
  business_control.cpp
```

等这层结构稳定后，再决定是否把 `appkernel/` 提到更通用的位置。

## 10. 落地步骤

### 第一步

先把当前 `RWTestApp/main.cpp` 里的 `KMDF` 控制协议和数据面 worker 代码抽进 `appkernel/*.c`，但仍然先让业务层继续提供“内存介质”。

目标：

- 行为不变
- 介质先不变
- 只是把职责边界切开

### 第二步

把 `ManagedDisk.medium` 和相关锁从 `AppKernel` 里删掉，上移为业务层 disk object。

目标：

- `AppKernel` 不再持有介质 bytes
- `AppKernel` 只通过回调访问介质

### 第三步

把 `main.cpp` 缩成：

- 启动收敛
- 介质对象管理
- CLI
- 调用 `AppKernel`

## 11. 本次重构后的预期结果

做完后应该得到：

- `AppKernel` 是纯 `C` 的协议/线程/运行时内核
- `RWTestApp` 只是一个业务宿主
- 以后要换内存介质、文件介质、映射介质，只动业务层
- 以后要压 `KMDF` / queue / ACK / worker，只动 `AppKernel`

最关键的是：

- 介质归业务层
- `KMDF` 交互和队列线程归 `AppKernel`
- 两边都不再重复持有对方状态

## 12. 当前不做的事情

本草案阶段明确不做：

- 不做 DLL 化
- 不做 plugin 化
- 不做多宿主 ABI 稳定承诺
- 不做“AppKernel C + AppKernel C++ 双实现”
- 不做“先抽一层 helper 再说”

当前唯一正确方向就是：

- 直接把现在的 `RWTestApp` 重构成“上层业务 + 下层 AppKernel”两层组合
