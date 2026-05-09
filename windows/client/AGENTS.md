# windows/client 作用域规范

本文件作用域覆盖 `windows/client/` 整个目录树。

## 1. Qt / C++ 代码风格

- Qt 相关代码采用类 Java 风格：
  - 类名使用 `UpperCamelCase`
  - 方法名使用 `lowerCamelCase`
  - 成员变量使用 `lowerCamelCase`
  - 局部变量和参数使用 `lowerCamelCase`
- 这里不是额外定义一套偏离 Qt 的命名规则，而是直接与 Qt 框架既有代码风格对齐。

## 2. 目录组织规则

- 目录优先按类拆分，而不是先按文件类型拆分。
- 一个明确类优先使用一个同名子目录承接其实现文件。
- 类目录名与类名保持一致，统一使用 `UpperCamelCase`。
- 类实现文件名也与类名保持一致，统一使用 `UpperCamelCase`，包括 `ui / cpp / h`。
- 没有明显类归属、而是按功能聚合的组件目录和组件文件名，统一使用 `lowerCamelCase`。
- 当前推荐结构形态：
  - `Widget/Widget.h`
  - `Widget/Widget.cpp`
  - `Widget/Widget.ui`
  - `backend/Backend.h`
  - `backend/Backend.cpp`
- 没有明显类归属、而是按功能聚合的代码，再按功能模块或组件拆子目录，例如：
  - `utils/`
  - `media/`
  - `runtime/`
  - `config/`

## 3. 当前阶段实现边界

- 当前 `client` 目标是把 `TestApp` 的最小闭环从 `CLI` 升级为 `UI`，不额外引入多余层级。
- 当前后端继续只保留 `BackendContext` 和 `ManagedDisk` 主线，不新增 `viewmodel/store/runtime facade` 一类扩展壳。
- UI 只做操作面和展示面，不持有第二份业务真状态。
