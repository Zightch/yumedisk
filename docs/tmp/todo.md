# 当前拆分说明

当前重构任务已经拆成两份独立文档，避免把 `server` 和 `windows/rust-cli` 的任务树堆在一起：

- [todo-server.md](./todo-server.md)
- [todo-rust-cli.md](./todo-rust-cli.md)

固定顺序：

1. 先按正式文档冻结 `server` wire 和边界。
2. 再按同一口径重构 `windows/rust-cli` 的协议消费者、会话对象和 `NetworkMedia`。
3. 最后做两侧联调和测试收口。
