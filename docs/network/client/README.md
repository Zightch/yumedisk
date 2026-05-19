# Client

## 定位

`docs/network/client` 承接 `docs/network/define` 没有定义的 client 侧内容，只描述：

- client 对象结构
- 宿主真状态边界
- 当前最小闭环策略
- 故障与 cleanup 的当前实现口径

这里不重复定义：

- wire protocol
- header / body 字段
- auth/open/session 的协议语义

这些内容统一回到 `docs/network/define`。

## 当前拆分

- [rust-cli](./rust-cli.md)
  - 当前 `rust-cli` 已落地实现口径
- [tauri-client](./tauri-client.md)
  - 当前 `tauri-client` 正式目标口径与最小闭环收口

## 使用规则

- 协议层共性放进 `docs/network/define`
- 宿主专属策略只写进对应宿主文档
- 若后续新增新的 client 宿主，直接在本目录新增独立文档
- 本 `README.md` 只保留概览，不再混放具体宿主细节
