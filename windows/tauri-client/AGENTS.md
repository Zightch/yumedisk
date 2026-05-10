# `windows/tauri-client` 开发约束

本文件作用域为 `windows/tauri-client` 整个目录树。

## 目标

- 本项目采用 `Tauri + Vue 3 + TypeScript`，按前端标准结构组织，不沿用 Qt/C++ 的“按类一套一套拆目录”思路。
- 优先保证最小闭环，先把“用户要完成什么事”收清，再补页面表现、动画、主题和更多宿主能力。
- 保持 UI、流程编排、宿主桥接分层清晰，不引入额外复杂度。

## 目录组织

- `src/app`
  - 应用入口、App 壳、全局 provider、主题装配、全局初始化。
- `src/pages`
  - 页面级视图，只负责页面结构与页面级编排。
- `src/widgets`
  - 页面内可复用的大块 UI 区域，例如列表区、顶部栏、设置面板。
- `src/features`
  - 面向用户动作的最小闭环功能模块，例如创建盘、删除盘、恢复盘、切换主题。
- `src/entities`
  - 领域对象、类型、映射、展示模型，不放页面逻辑。
- `src/shared`
  - 通用 UI 组件、工具、样式、常量、基础 API 封装。
- `src-tauri/src`
  - 宿主桥接层，只负责 Tauri command、权限、原生能力接入，不堆 UI 编排逻辑。
- `src-tauri/src/commands`
  - 按能力拆分 Tauri command，例如 session、disk、config。
- `src-tauri/src/state`
  - Rust 侧运行态持有、服务装配。
- `src-tauri/src/backend`
  - Rust 侧对原生 backend / DLL / FFI 的适配封装。

## 命名规范

- Vue 组件文件使用 `UpperCamelCase`
  - 例如 `DiskCard.vue`、`CreateDiskDialog.vue`
- 组件主目录可使用 `UpperCamelCase`
  - 例如 `DiskCard/`
- 组合式函数使用 `useXxx.ts`
  - 例如 `useTheme.ts`、`useCreateDisk.ts`
- 普通模块、工具、服务、适配器、映射器文件使用 `lowerCamelCase`
  - 例如 `backendClient.ts`、`diskMapper.ts`
- 非组件目录、功能目录、领域目录使用 `lowerCamelCase`
  - 例如 `features/createDisk`、`shared/styles`
- TypeScript 类型、接口、枚举、类使用 `UpperCamelCase`
- 变量、函数、参数、props、emits、store 字段使用 `lowerCamelCase`
- 常量仅在“真正跨模块共享且长期稳定”时使用 `UPPER_SNAKE_CASE`
- CSS class 名使用语义化 `kebab-case`

## 分层与依赖原则

- 不按“技术类型”堆平文件；优先按职责和最小闭环拆分。
- 页面层依赖 `widgets` / `features` / `entities` / `shared`，不要反向依赖。
- `features` 可以组合 `entities` 和 `shared`，但不要直接耦合页面实现细节。
- `entities` 只放领域模型、类型和映射，不放页面交互逻辑。
- `shared` 只放真正通用的内容，不要把业务代码塞进 `shared`。
- `src-tauri` 与 `src` 之间通过明确的桥接接口交互，不把 Rust 内部结构直接泄露到前端组件树。

## 状态与边界原则

- UI 不持有第二份业务真状态。
- 运行态真状态应集中在明确的宿主状态或前端业务状态容器中，组件只持有展示态与交互态。
- 前端组件不要直接消费底层 backend 原始结构；先做一层面向 UI 的模型映射。
- `src-tauri` 保持“薄桥接”定位：
  - 负责 command、权限、宿主能力、原生接口适配；
  - 不在 Rust 侧堆叠本应属于前端页面的流程编排。

## Vue / TypeScript 约定

- 优先使用 Composition API。
- Vue 单文件组件优先使用 `<script setup lang="ts">`。
- props、emits、返回值类型尽量显式声明。
- 优先使用明确的父子通信、组合式函数和 feature 内状态，不引入隐式事件总线。
- 先做最小闭环，不为未来假设过度抽象。
- 一个文件如果同时承担多种职责，应先考虑拆成 feature、widget、entity 或 shared 内的更小组件。

## 样式约定

- 优先全局主题变量和共享样式令牌，不在组件内散落硬编码颜色和尺寸。
- 全局样式基础设施放在 `src/shared/styles`。
- 组件局部样式只处理局部结构差异，不重复定义整套主题规则。
- 动画、主题、交互反馈先统一机制，再做局部特效。

## Tauri / Rust 侧约定

- `src-tauri` 代码优先按能力模块拆分，不写成单个巨型 `lib.rs`。
- Tauri command 名称、参数、返回结构保持稳定、明确、可文档化。
- Rust 侧优先返回清晰的 DTO，不把前端绑死在内部实现细节上。
- 只有确实涉及权限、性能、原生能力或 FFI 时，才把逻辑下沉到 Rust。

## 变更原则

- 优先重建清晰结构，不在混乱结构上继续平铺逻辑。
- 新增代码时，先判断它属于 page、widget、feature、entity 还是 shared，再落目录。
- 如果一个目录开始同时承担多种职责，应主动收口拆分。
