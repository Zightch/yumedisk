# Network

## 定位

`docs/network` 是当前网络盘文档总入口，按层固定拆成三部分：

- [define](./define/README.md)
  - 只定义网络协议本身
- [client](./client/README.md)
  - 只定义当前 client 实现口径
- [server](./server/README.md)
  - 只定义当前 server 实现口径

## 使用规则

- `define/` 只写协议边界、字段、时序、失效事实
- `client/` 只写当前 client 的对象结构、宿主策略和实现限制
- `server/` 只写当前 server 的角色结构、真源状态和实现策略

若后续结构变化，应直接在这三层内收束，不再回到并列主线。
