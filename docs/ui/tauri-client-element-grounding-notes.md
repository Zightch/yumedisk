# Tauri Client UI：以 Element Plus 为地基的落地说明

## 1. 文档目的

本文说明 `windows/tauri-client` 当前 UI 是如何在 **Element Plus 作为底层承载** 的前提下，完成既定目标视觉效果的。

重点不是罗列“做了什么样式”，而是把以下事情收清：

- 目标视觉是什么；
- 哪些部分直接使用 Element 默认能力；
- 哪些部分为了目标效果做了覆写或接管；
- 覆写的边界在哪里；
- 后续新增功能时，哪些做法可以延续，哪些做法应该避免。

---

## 2. 核心原则

本项目当前采用的 UI 落地原则可以概括为一句话：

> **Element Plus 负责提供稳定的布局骨架、表单语义、交互状态与基础控件能力；项目只在目标视觉与 Element 默认样式明显不一致时，做局部、受控的样式接管。**

这意味着：

- 不重写一套自有控件系统；
- 不把 Element 仅当作“安装了但没真正用”的依赖；
- 不为了追求像素效果，在全局随意覆盖 Element 所有控件；
- 不让页面组件直接依赖 Element 的内部 DOM 细节。

---

## 3. 目标视觉

当前 Tauri Client 的目标视觉，不是 Element 默认的后台管理风格，而是更轻、更干净、更桌面化的单窗口工具体验。

主要目标如下：

### 3.1 页面气质

- 整体为单窗口桌面工具，而不是传统管理后台；
- 纯色与轻渐变为主；
- 控件圆角明显；
- 大部分区域不强调描边，边框只在层次或可交互性需要时出现；
- 页面密度较高，但不压迫。

### 3.2 结构目标

- 程序启动后直接进入主页，由主页承接启动期展示；
- 顶部会话状态按钮负责表达“正在初始化 / 会话正常 / 会话失败”；
- 主页分为顶部工具区和中部磁盘列表区；
- 磁盘列表是主视觉中心；
- 设置页作为覆盖层切入，而不是新窗口；
- 创建磁盘与编辑磁盘使用对话框体系。

### 3.3 交互目标

- 连接 / 断开是盘卡片主动作；
- 编辑 / 删除是悬停后出现的次动作；
- 添加磁盘先弹轻量模式气泡，再进入具体对话框；
- 主题切换影响整体颜色族，但不改变组件语义。

---

## 4. Element 作为地基：具体承载了什么

当前项目并不是“外面套了一层 Element 皮”，而是真正把 Element 用在了基础承载层。

### 4.1 页面骨架

页面主骨架依赖 Element 布局容器：

- 主页：`windows/tauri-client/src/pages/home/HomePage.vue:120`

对应承载：

- `el-container`
- `el-header`
- `el-main`

这些组件负责页面的结构流、头部与内容区关系。

### 4.2 列表与状态组件

磁盘列表区域依赖：

- `el-card`
- `el-scrollbar`
- `el-skeleton`
- `el-alert`
- `el-empty`

位置：`windows/tauri-client/src/widgets/DiskListPanel/DiskListPanel.vue:25`

这些组件负责：

- 主列表容器；
- 列表内部滚动；
- 加载骨架；
- 错误提示；
- 空状态提示。
- 列表头刷新图标按钮。

当前实现补充：

- 空状态继续使用 `el-empty`，只是把默认大图标关闭；
- 重扫入口继续使用 `el-button + el-icon`，只是从文字按钮收成刷新图标按钮。

顶部会话状态入口依赖：

- `el-button`
- `el-icon`
- `el-dialog`

对应文件：

- `windows/tauri-client/src/widgets/AppHeader/AppHeader.vue:1`
- `windows/tauri-client/src/features/sessionStatus/SessionStatusDialog.vue:1`

这些组件负责：

- 会话三态按钮语义；
- 顶部可点击状态入口；
- 会话状态查看对话框；
- 失败态重试入口。

### 4.3 表单与对话框

创建 / 编辑磁盘对话框依赖：

- `el-dialog`
- `el-tabs`
- `el-form`
- `el-form-item`
- `el-input`
- `el-input-number`
- `el-switch`
- `el-radio-group`
- `el-radio-button`
- `el-alert`
- `el-button`

对应文件：

- `windows/tauri-client/src/features/createMemoryDisk/CreateMemoryDiskDialog.vue:83`
- `windows/tauri-client/src/features/createFileDisk/CreateFileDiskDialog.vue:183`
- `windows/tauri-client/src/features/editDisk/EditDiskDialog.vue:83`

这些组件负责：

- 表单语义；
- 校验容器；
- 数值输入；
- 开关语义；
- 选项切换；
- 弹窗行为。

### 4.4 设置页

设置页依赖：

- `ElButton`
- `ElCard`
- `ElScrollbar`
- `ElRadioGroup`
- `ElRadioButton`

位置：`windows/tauri-client/src/widgets/SettingsPage/SettingsPage.vue:53`

设置页没有自己发明“卡片”“滚动容器”“单选组”这类基础结构，而是依赖 Element 提供的行为基础。

### 4.5 轻量交互

轻量交互仍然依赖 Element：

- 添加盘模式气泡：`el-popover`
- 无效盘说明：`el-tooltip`
- 各类反馈：`ElMessage`
- 列表头重扫按钮图标：`el-button + el-icon`

对应文件：

- `windows/tauri-client/src/widgets/AppHeader/AppHeader.vue:52`
- `windows/tauri-client/src/widgets/DiskCard/DiskCard.vue:92`
- `windows/tauri-client/src/widgets/DiskListPanel/DiskListPanel.vue:30`

---

## 5. 为目标视觉做了哪些工作

以下工作是“在 Element 为底层”的前提下完成的。

---

### 5.1 建立主题令牌层，而不是直接写死 Element 颜色

主题令牌文件：`windows/tauri-client/src/shared/styles/tokens.css:1`

这里做的事情不是直接改每个控件颜色，而是先建立项目自己的视觉语义变量，例如：

- 窗口背景；
- 面板背景；
- 选中背景；
- 软边框 / 强边框；
- 主文本 / 次文本 / 弱文本；
- accent 主色族；
- loading 语义色。

这样做的意义：

- 项目视觉先抽象成自己的语义层；
- Element 只是消费这些变量；
- 亮暗主题与颜色家族切换不需要反复改局部控件样式。

---

### 5.2 建立 Element 变量桥，而不是全局暴力重皮

桥接文件：`windows/tauri-client/src/shared/styles/element.css:1`

本轮重构后，这个文件的职责被明确收紧为两类：

#### A. Element 主题变量映射

例如：

- `--el-color-primary`
- `--el-text-color-primary`
- `--el-bg-color`
- `--el-border-color`
- `--el-box-shadow`
- `--el-border-radius-base`

也就是把项目令牌映射到 Element 自己的变量系统。

#### B. 少量全局基础面修正

例如：

- 统一卡片 / 对话框 / 弹层背景与边框；
- 统一输入框 wrapper 的背景和 focus 边界；
- 统一 tag / divider / 空状态描述色。

重构前这里还包含了更宽的全局按钮接管；本轮已收掉，避免所有 `el-button` 都被一层全局视觉强压。

---

### 5.3 把“项目特有视觉”下沉到局部样式文件

当前样式文件分工如下：

- `windows/tauri-client/src/shared/styles/layout.css`
  - 页面骨架、窗口尺寸、主页主壳布局
- `windows/tauri-client/src/shared/styles/header.css`
  - 顶部栏、状态块、添加气泡
- `windows/tauri-client/src/shared/styles/listPanel.css`
  - 磁盘列表面板、滚动区、列表内容间距
- `windows/tauri-client/src/shared/styles/diskCard.css`
  - 磁盘卡片外观、连接态、动作按钮
- `windows/tauri-client/src/shared/styles/dialogs.css`
  - 创建 / 编辑对话框视觉
- `windows/tauri-client/src/shared/styles/settingsPage.css`
  - 设置页视觉

这个分层的价值在于：

- Element 全局桥只做底层映射；
- 页面特有视觉在局部文件里表达；
- 后续修改某一块 UI，不需要去全局文件里翻找副作用。

---

### 5.4 把原生按钮收回到 Element 按钮底座

这是本轮重构的重点之一。

此前有几类按钮直接使用原生 `button`：

- 头部设置按钮；
- 头部添加按钮；
- 添加磁盘气泡中的模式按钮；
- 磁盘卡片的编辑 / 删除按钮；
- 对话框关闭按钮。

本轮统一收回到 `el-button` 底座，位置包括：

- `windows/tauri-client/src/widgets/AppHeader/AppHeader.vue:25`
- `windows/tauri-client/src/widgets/DiskCard/DiskCard.vue:117`
- `windows/tauri-client/src/features/createMemoryDisk/CreateMemoryDiskDialog.vue:92`
- `windows/tauri-client/src/features/createFileDisk/CreateFileDiskDialog.vue:192`
- `windows/tauri-client/src/features/editDisk/EditDiskDialog.vue:92`

这样做后，项目重新获得了 Element 按钮的这些基础能力：

- 禁用态语义；
- 焦点语义；
- loading 语义；
- 一致的交互行为；
- 可预期的组件结构。

在此基础上，再通过局部 class 去实现目标视觉。

这里有一个实际落地细节需要明确：

- `el-button` 会对插槽内容增加自己的内部包裹层；
- 如果某个按钮的“按钮内部排版”需要特殊布局，不能只改按钮外层；
- 必须确认 Element 当前真实生效的是哪一层内容容器，再局部接管那一层。

例如顶部“添加磁盘”气泡里的模式按钮，最终就不是依赖猜测性的 `.el-button__text`，而是直接接管按钮的真实直接子内容层。

同类问题后续又在顶部会话状态按钮里再次出现：

- 绿圆 / 旋转图标 与 `会话正常` / `会话失败` 文本的间距，仅改按钮外层 `gap` 并不一定生效；
- 真正影响这组内容排布的，往往是 `el-button` 内部实际承载插槽内容的那层 `span`；
- 因此当前正式做法不是只押注 `.el-button__text`，而是优先接管局部按钮的真实直接子内容层，并保留对 `.el-button__text` 的兼容覆盖。

也就是说，当前项目在 `el-button` 这类控件上的经验已经明确为：

- 先看外层；
- 外层无效时，继续确认当前版本真正负责内容排布的内部层；
- 最终只在局部 class 范围内接管那一层，不把这类结构补偿扩散到全局按钮规则。

---

### 5.5 盘卡片主按钮保持 Element 语义，但按业务状态做局部接管

位置：`windows/tauri-client/src/shared/styles/diskCard.css:147`

盘卡片主按钮是 `el-button`，但为了满足业务视觉，做了局部收口：

- `连接` 使用绿色强调；
- `断开` 使用红色强调；
- `loading` 不使用默认全灰，而使用“当前按钮语义色灰化”；
- `invalid` 盘仍使用 Element 的 disabled 语义；
- 局部移除默认黑边 / focus 干扰。

这一块属于合理接管，因为：

- 这是项目最核心的业务动作；
- 默认 Element primary 按钮无法表达“连接/断开/连接中”的业务含义；
- 需求明确需要不同语义色。

这里还需要明确一个额外边界：

- `loading` 不能简单回退到全局主题色；
- `连接` 的 `loading` 应从绿色主动作延续；
- `断开` 的 `loading` 应从红色主动作延续。

也就是说，`loading` 视觉必须挂在**按钮语义层**，而不是挂在**全局主题层**。

---

### 5.6 对话框使用 Element 表单体系，但局部重建视觉

位置：`windows/tauri-client/src/shared/styles/dialogs.css:1`

对话框视觉和 Element 默认风格差异较大，因此进行了较深的局部接管，主要包括：

- 自定义弹窗尺寸与圆角；
- 重建 header / footer 布局；
- 重建关闭按钮；
- 收紧表单项密度；
- 重绘浏览按钮；
- 重绘 tabs 顶部切换样式；
- 重绘 radio button 为 chip 风格。

这是必要工作，因为概念设计不是后台管理弹窗，而是紧凑的桌面工具弹窗。

但这类接管也最容易失控，因此本轮还做了一件事：

- 去掉了依赖 `nth-child(2)` 的补偿写法；
- 改为直接对 `.el-tabs__item` 使用明确 padding 覆盖。

也就是从“修 Element 内部结构副作用”，收到了“控制我们自己的样式边界”。

后续又继续收了一轮创建类对话框结构，重点不是再叠加样式补丁，而是把布局责任重新划清：

- 创建内存盘对话框收为“固定标题 + 正文纵向滚动区”；
- 创建文件盘对话框收为“固定标题 + 固定 tabs 头 + 正文纵向滚动区”；
- `取消 / 创建` 按钮不再依赖 Element 默认 dialog footer，而是作为正文末尾内容参与同一滚动链；
- 创建类对话框只保留右上 / 右下圆角，左侧改为直角；
- 文件盘对话框中，`Element Plus Tabs` 只保留 tabs 头和选中语义，不再让 `el-tab-pane` 承担正文布局和滚动职责。

这样做的原因很明确：

- `Element Plus Dialog` 的默认 body / footer 结构不符合当前桌面工具式弹窗目标；
- `Element Plus Tabs` 的默认 content / pane 高度语义容易在小高度窗口下制造中部空白和滚动链断裂；
- 与其继续围绕内部 DOM 补偿，不如把“谁负责头部、谁负责滚动、谁负责正文切换”收回到项目自己的 class 边界。

这一轮之后，对话框相关边界进一步明确为：

- Element 继续负责弹层行为、表单语义、按钮/输入/切换组件语义；
- 项目自己负责创建类对话框的正文布局链和滚动链；
- 只接管真正需要接管的结构层，不再把 Element 默认 footer / tab content 当作必须复用的布局骨架。

---

### 5.7 设置页使用 Element 卡片和单选组，但把主题选项收成项目风格

位置：`windows/tauri-client/src/shared/styles/settingsPage.css:1`

设置页不是完全复写一套独立 UI，而是：

- `ElCard` 继续做区块容器；
- `ElScrollbar` 继续做滚动容器；
- `ElRadioGroup` / `ElRadioButton` 继续做选项切换；
- 只在视觉上把单选组收成 chip 风格。

这符合当前项目的真实需求：

- 视觉需要统一；
- 但不需要另起一套设置页控件系统。

---

### 5.8 列表滚动问题从布局层修，而不是临时补丁

位置：`windows/tauri-client/src/shared/styles/listPanel.css:89`

此前存在滚动条与卡片边缘重叠的问题。

这里最终不是通过给卡片单独乱加 margin 修，而是从列表容器层收口：

- 列表面板作为固定高度容器；
- `el-scrollbar` 承担内部滚动；
- 内容区右侧保留滚动条安全 padding。

这类问题必须在布局层修，不应该在单个卡片上打补丁。

---

### 5.9 会话状态入口继续站在 Element 语义上

顶部会话状态块当前不是普通文本，而是基于 `el-button` 的可点击状态入口；状态详情继续使用 `el-dialog` 承接。

这里做的工作是：

- 继续使用 `el-button` 提供按钮语义、图标承载和交互状态；
- 通过局部样式把它收成顶部状态 pill；
- 使用 `el-dialog` 作为状态详情和失败重试的唯一弹层承载；
- 失败态重试继续走 Element 按钮语义，不额外自造一套浮层动作体系。

这说明当前项目并不是“只要样式特殊就绕开 Element”，而是：

- 先保留 Element 的行为底座；
- 再把视觉和信息层级调整到目标效果。

---

### 5.10 启动期磁盘覆盖停留在展示映射层

主页当前会把启动期和会话失败期的磁盘统一覆盖成 `invalid` 展示，但这层逻辑并没有下沉为磁盘真实状态。

实际落地方式是：

- 宿主继续提供真实 `runtimeDisks`；
- 主页通过单一映射口生成 `displayDisks`；
- 磁盘卡片只消费最终展示 DTO；
- 卡片本身不额外判断“正在初始化”或“会话失败”。

这样做的价值是：

- 启动链和展示覆盖职责不散到多个组件；
- 磁盘卡片保持简单，只负责展示结果；
- 后续如果继续调整启动链，不需要反向清理一批卡片级补丁判断。

---

## 6. 工作边界：哪些事情没有做

为了保持“Element 为底层”的策略清晰，本项目当前没有做以下事情：

### 6.1 没有重写自有基础控件库

没有自己封装：

- 自定义 Button；
- 自定义 Input；
- 自定义 Dialog；
- 自定义 RadioGroup；
- 自定义 Scrollbar。

原因很直接：没必要，也会抬高维护成本。

### 6.2 没有全局重写所有 Element 组件

目前只桥接和收口了项目真正用到的部分。

没有去碰：

- Table
- Drawer
- Menu
- Select 下拉面板复杂态
- DatePicker
- Cascader
- Upload

因为当前项目没有用到，提前重皮只会制造噪音。

### 6.3 没有让页面组件直接依赖 Element 内部 DOM 结构

本轮重构明确收掉了一部分脆弱补偿。

后续如果不得不触碰 Element 内部结构，也必须满足两个条件：

- 默认能力确实达不到目标视觉；
- 没有更干净的 class / variable / slot 解法。

---

## 7. 当前可以接受的接管点

以下接管点是当前阶段可以接受、且应继续保留的：

### 7.1 对话框 Tabs

原因：

- 概念设计要求片状切换；
- Element 默认 tabs 风格差异过大；
- 接管范围局限在 `app-dialog-tabs`；
- 文件盘创建对话框中，tabs 只保留 header 语义，不再复用默认内容区骨架。

### 7.2 Chip 风格单选

原因：

- 设置页和文件格式选择都需要轻量 chip 风格；
- 默认 radio button 不符合视觉目标；
- 接管范围局限在局部组 class。

### 7.3 盘卡片主动作按钮

原因：

- 连接 / 断开 / loading / invalid 都有明确业务语义；
- 默认 primary / danger 不足以表达。

---

## 8. 当前仍需注意的风险点

虽然这轮已经比之前收得更干净，但仍有几个风险点需要后续保持警惕。

### 8.1 radio / tabs 仍属于“深度视觉接管”

对应文件：

- `windows/tauri-client/src/shared/styles/dialogs.css:186`
- `windows/tauri-client/src/shared/styles/settingsPage.css:112`

风险不在于现在不能这样做，而在于：

- 如果未来扩展更多页签 / 更多单选组；
- 又把不同视觉都叠进去；
- 就会重新走向“局部样式膨胀”。

### 8.2 局部视觉文件之间可能出现重复规则

例如：

- `dialogs.css` 和 `settingsPage.css` 都有 chip 风格；
- 它们现在是独立的，但长期看可能需要抽共性。

不过当前阶段不建议过早抽象，先保持稳定更重要。

### 8.3 主题切换只应改变 token，不应让局部组件自发分裂

如果以后某个组件为了某个主题单独写大量例外规则，就说明 token 设计不够好，应该回去收 token，而不是继续堆局部 if。

### 8.4 少量 `!important` 仍然存在，但必须保持在受控范围

当前还存在少量 `!important`，主要集中在：

- 对话框尺寸与定位强制覆盖；
- tabs item padding 强制覆盖；
- chip radio 的边框 / focus ring 压制。

这些点当前是可以接受的，原因是：

- Element 默认样式优先级较高；
- 目标视觉明确要求不同效果；
- 接管范围已经被局部 class 包住。

但后续必须遵守两个约束：

1. **`!important` 只能作为局部接管的最后手段，不能进入全局桥接层泛化使用；**
2. **如果某个区域的 `!important` 越来越多，说明应该回退到结构、token 或共享样式模式层重新收口。**

### 8.5 `layout.css` 中的强制覆盖不应继续扩散

当前 `layout.css` 中还有少量为了压 Element 容器默认 `padding` / `height` 的强制覆盖。

这类覆盖当前风险可控，因为：

- 只用于页面骨架层；
- 作用对象清晰；
- 还没有扩散到业务组件层。

但后续如果页面数量增加，必须避免出现“每个页面都各自再压一遍容器默认样式”的情况。更合适的做法是：

- 保持统一页面壳；
- 让 page 进入壳内后只表达结构，不重复修正容器默认行为。

### 8.6 `settings` 与 `dialogs` 的 chip 视觉存在重复实现

当前：

- `windows/tauri-client/src/shared/styles/dialogs.css`
- `windows/tauri-client/src/shared/styles/settingsPage.css`

都维护了一份 chip 风格单选组。

现在保留重复是可以接受的，因为：

- 两处密度与场景不同；
- 项目仍在收口阶段；
- 提前抽象容易把局部差异重新混回一起。

但这是一个明确的潜在维护点。后续如果新增第三处类似 chip 单选，优先动作不应是继续复制，而应判断是否可以抽出共享视觉模式。

---

## 9. 本轮重构结论

本轮重构之后，项目的 UI 落地策略已经更接近目标：

- Element 继续作为底层结构和交互语义承载；
- 全局样式桥从“较宽的视觉接管”收回到“主题变量映射 + 少量基础面修正”；
- 原生按钮被统一拉回到 Element 底座；
- 局部接管集中在真正有目标视觉差异的区域；
- 脆弱的内部结构补偿被减少。

换句话说，当前项目已经不再是“Element + 大量自造按钮壳”，而是更明确的：

> **Element 打底，局部定制，边界清楚。**

---

## 10. 后续扩展建议

如果后续继续加功能，建议按以下顺序思考：

1. 先判断 Element 默认组件能否直接满足；
2. 如果不能，先尝试 token / class / slot 层面的轻覆写；
3. 如果还不够，再做局部接管；
4. 如果某类接管跨多个页面反复出现，再考虑提炼共享视觉模式；
5. 不要再回到“先写原生按钮，再用 CSS 模仿控件”的路线。

### 10.1 当前阶段的明确收口建议

如果后续继续收紧当前 UI 架构，优先级建议如下：

1. **先保持 `element.css` 克制，不再把局部视觉倒灌回全局；**
2. **如果 chip / tabs 视觉再次复用，优先抽共享模式，而不是复制局部规则；**
3. **如果新增页面也需要容器强制覆盖，先回到统一页面壳检查，而不是继续堆 `!important`；**
4. **主题差异优先回 token，不在 feature 里堆主题例外分支。**

这份文档对应的是“本项目如何落地”。

下一份 `UI 规范文档` 会进一步把这里的经验抽象成后续新增页面与功能时应遵循的统一规则。
