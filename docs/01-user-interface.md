# LILYGO UI Telegram 界面规范

本文是当前 Telegram 应用的 UI 开发规范。AI agent 新增或
修改页面、组件、布局、样式、文案或字体时必须遵守本文。

本文中“必须”和“不得”是强制要求，“应”是默认实现方式。开始编辑前，
必须先检查当前页面、`src/components/` 中的现有能力和相关测试，不得假设
仓库中存在未实现的基类、主题或响应式辅助 API。

## Telegram iOS 参考

当前界面参考官方 [Telegram-iOS](https://github.com/TelegramMessenger/Telegram-iOS)
的经典浅色主题 `dayClassic`，源码参考固定到提交
`6ad963e5b62d354da79040f388ae2b9132fb17b8`。主要参考位置：

- [DefaultDayPresentationTheme.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramPresentationData/Sources/DefaultDayPresentationTheme.swift#L56)：
  `#0088FF` 强调色、白色会话行、灰色预览、浅绿色己方气泡和列表语义颜色；
- [ChatListItem.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/ChatListUI/Sources/Node/ChatListItem.swift#L2294)：
  圆形头像、名称与消息预览层级、右侧时间和未读徽标、从正文起点开始的分隔线；
- [ChatTitleView.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/ChatTitleView/Sources/ChatTitleView.swift#L26)：
  居中会话标题及受约束的标题宽度；
- [ChatTextInputPanelNode.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/Chat/ChatTextInputPanelNode/Sources/ChatTextInputPanelNode.swift#L74)：
  紧凑圆角输入框和独立发送操作；
- [PeerInfoHeaderNode.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/PeerInfo/PeerInfoScreen/Sources/PeerInfoHeaderNode.swift#L1656)：
  详情页的居中头像、名称和状态层级；
- [PeerInfoProfileItems.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/PeerInfo/PeerInfoScreen/Sources/PeerInfoProfileItems.swift#L158)：
  联系人的手机号、用户名及简介标签行；同文件
  [群组与频道资料](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/PeerInfo/PeerInfoScreen/Sources/PeerInfoProfileItems.swift#L619)
  参考公开链接及可换行的简介。

这是低资源设备上的 LVGL 适配：导航栏、聊天列表、联系人列表、会话消息区和
底部栏统一采用状态栏的 `#F2F2F7` 背景，使用不透明控件和纯色背景。
保留 AppKit 的 Inter、Source Han Sans CN、Font Awesome 字体链。
不移植 UIKit、iOS 字体、玻璃模糊或动态壁纸；iOS 的 pt 也不得直接当作目标
设备像素使用。`dayClassic` 的己方气泡为 `#E1FFC7`，与 `day` 的蓝色渐变气泡
应明确区分。

### 当前页面和行为

- 登录页可通过 `Use QR code` 和 `Use phone number` 在二维码与手机号方式之间
  双向切换，并保留已输入的手机号。二维码请求和认证重置期间仍可切换，按最后
  选择显示页面；后台准备完成前禁用手机号提交。二维码登录链接由 TDLib 自动更新。
- 根导航提供 `Contacts`、`Chats` 和 `Settings` 三个页签。设置页采用分组列表和
  可返回的详情页；未接入的通话、代理等功能不设置占位入口。
- 会话列表使用与页面同色的连续列表行、圆形头像、名称、最后消息预览、时间和未读徽标。
  聊天列表、会话标题和联系人显示 Telegram 的小尺寸头像；尚未下载、未设置或
  加载失败时显示彩色首字符。图片保持比例、居中裁切并由圆形容器遮罩。
- 搜索只在当前缓存的最多 100 个会话中匹配名称和最后消息预览；不会触发远程
  搜索，也不搜索完整消息历史。无匹配时显示空结果状态。
- 文本草稿按会话保存在内存中，切换会话或页签后恢复。草稿不持久化，也不与
  Telegram 草稿同步；退出账号或应用后不保留。
- 消息按日期分组，己方气泡靠右、对方气泡靠左。收到的消息在气泡左侧显示
  发送者头像，与气泡底部对齐，气泡宽度扣除头像及间距。横竖屏都在消息列表
  顶部提供加载更早消息的入口；列表可以滚动到该入口。
- 己方消息显示时间及待发送、失败或单勾状态。单勾仅表示当前消息没有待发送或
  失败标记；当前未追踪对方已读回执，不得将其描述为已读。打开会话时向 TDLib
  同步本用户已读消息是独立行为。
- 窄布局在会话列表和当前会话之间导航；宽布局并列显示侧栏与会话。断点根据
  父容器可用宽度判断；状态和输入由同一组 Model、ViewModel 和 View 管理。

### 联系人

- 联系人从 Telegram 的联系人列表加载，最多保留 500 位。列表显示圆形头像、
  姓名与在线/最近上线状态，支持刷新、按姓名或在线优先排序。
- 搜索只匹配已加载联系人的姓名、用户名和手机号。ASCII 字母不区分大小写，
  其他 Unicode 文本按原样匹配；输入限制为 128 个字符和 512 字节。无匹配、未
  加载、加载中及加载失败具有明确状态。超出列表限制时显示已加载数与总数。
- 新建联系人表单包含名字、姓氏和含国家码的手机号；手机号会交给 Telegram
  导入联系人。未注册的号码显示服务端错误，不能显示添加成功。
- 详情页提供发送消息及删除联系人。删除前必须确认；从联系人打开尚无历史的
  私聊使用临时选中会话，不能为显示详情而强行将其插入主聊天列表。其历史和
  发送操作沿用现有会话流程，后续是否进入聊天列表由 TDLib 列表更新决定。

### 用户与群组详情

- 联系人列表的信息按钮在侧栏打开详情；点击会话标题或头像，在会话区域打开
  对应用户、普通群组、超级群组或频道详情。从会话进入时，返回或发送消息操作
  回到原会话，保留已有草稿和消息滚动位置；从联系人进入时，返回联系人列表。
- 详情共用居中的 96 像素圆形头像、名称和状态头部，下方使用标签在上、资料值
  在下的信息行；长姓名、用户名和简介应在容器内换行。沿用头像缓存和首字符
  回退，加载结果不得改变头像尺寸。
- 用户详情通过 TDLib 的 `getUser` 和 `getUserFullInfo` 加载姓名、状态、手机号、
  用户名和简介。只显示服务端可见的数据，不得将隐私隐藏字段解释为客户端
  获取失败；公开用户名使用首个有效的 `active_usernames`，不展示已停用的别名。
- 群组和频道详情显示名称、头像、类型、人数、公开用户名链接及简介。当前仅
  展示成员或订阅者数量，不加载成员名单，也不提供未接入的群管理或通话入口。
  `getChat` 确定类型后调用 `getBasicGroup` / `getBasicGroupFullInfo` 或
  `getSupergroup` / `getSupergroupFullInfo`；未知人数显示为不可用，后续未知值
  不覆盖已经获取的有效人数。
- 进入详情时按需加载，具有加载、错误和重试状态。应用最多缓存一个当前详情
  对象；切换目标时丢弃旧目标请求资格，响应必须同时匹配有效请求与当前对象。
  退出账号或认证重置后清空详情及请求，过期响应不能恢复此前资料。

### 设置

| 页面 | 当前行为 |
| --- | --- |
| My Profile | 查看账号资料，分别编辑姓名、用户名和简介。 |
| Saved Messages | 打开当前账号的收藏会话。 |
| Notifications and Sounds | 分别设置私聊、群组和频道的通知、预览及声音开关。 |
| Privacy and Security | 设置手机号、最后上线、头像的基本可见范围；进入黑名单和设备列表。 |
| Devices | 查看最多 100 个登录会话，确认后终止其他设备会话；本机退出使用 Log Out。 |
| Blocked Users | 查看最多 500 个屏蔽用户或聊天，确认后解除屏蔽。 |
| Storage Usage | 查看媒体文件、文件数和数据库大小；确认后清理本机媒体缓存；设置照片自动下载。 |
| Appearance | 从 18、22、26、30 中选择消息字号，并显示对应预览。 |
| About | 显示应用信息、客户端类型和连接状态。 |
| Log Out | 确认后退出当前账号。 |

设置数据在进入相关页面时按需加载，不得在账号尚未登录时发出修改请求。请求
等待期间显示加载状态并禁用冲突修改；提交不代表操作成功。服务端返回更新后
才刷新数据，完整请求成功结束后才显示成功反馈。出错时保留表单内容，允许重试，
账号操作错误与全局登录错误分开显示。

隐私设置提供 Everybody、My Contacts、Nobody 三种基本范围。修改范围时，必须
保留已加载的用户、群组及其他非基础例外规则，不能用空例外列表覆盖它们。详情
页显示现有例外数量；本版本不编辑例外名单，未知组合显示 Custom，并禁止通过
基础范围控件改变其规则顺序。通知设置也
必须保留未在本界面编辑的 TDLib 字段，包括已有的其他声音和通知选项。

本地偏好只包含消息字号、照片自动下载和联系人排序，原子保存到应用数据目录的
`ui-preferences.json`。真实及演示模式使用同一份偏好；退出账号保留偏好并清空
账号数据和未完成请求。偏好文件不保存资料、凭据、验证码或草稿。

清理缓存仅回收本机下载媒体，保留云端消息；完成后应停止相关播放、清空已解码
图片缓存并刷新存储统计。照片自动下载关闭时可以继续显示已经缓存的图片，尚未
下载的照片不会自动请求；语音保持点击后按需下载。

头像具有独立的可见区域加载和缓存，最多保留 64 项，PNG、JPEG 解码目标最大
120 x 120，像素缓冲合计不超过 3.6 MiB，不受消息照片自动下载开关影响。
会话列表、联系人和会话标题的头像
尺寸分别固定为 60、52 和 42；消息发送者头像在窄布局为 36，宽布局为 40。
用户、群组和频道详情头像固定为 96。
加载、失败回退或收到新头像时不得改变占位尺寸；退出账号和清理缓存时移除旧头像。

## 架构边界

| 关注点 | 位置 | 规则 |
| --- | --- | --- |
| 页面状态和 UI | `src/pages/<page>/` | 页面私有的 Model、ViewModel 和 View 放在同一目录。 |
| 共享领域规则 | `src/domain/` | 仅在业务对象被多个页面实际共享时创建，不得依赖 LVGL。 |
| 通用组件和绑定 | `src/components/` | 复用现有组件和 Subject RAII 封装，明确所有权和销毁顺序。 |
| 生命周期和路由 | `src/app.cpp`、`src/app_router.*` | 应用生命周期和全局导航不得由页面 View 持有。 |

依赖方向必须是：

```text
View -> ViewModel -> Model
```

- Model 保存业务状态和领域规则，必须能在不初始化 LVGL 的情况下测试。
- ViewModel 将 Model 转换为展示状态，通过 Subject 发布变化，并提供用户命令。
- View 只负责 LVGL 对象创建、布局、事件转发和状态绑定，不得直接修改 Model。

联系人、用户与群组详情和设置共用不依赖 LVGL 的 `TelegramAccountModel`。ViewModel 为账号操作
分配单调递增 ID，最多保留 16 个未完成请求；非零 ID 的响应必须关联仍有效的
请求。退出账号或认证重置后，过期响应不能重新填入账号数据或显示成功。账号
请求通过 TDLib 工作线程执行；演示后端提供相同事件接口，但账号修改仅存在于
演示进程内存。

数据流必须保持单向：

```text
用户输入 -> View 回调 -> ViewModel 命令 -> Model 更新
         -> Subject -> View 绑定或 observer -> LVGL 渲染
```

## 视觉规范

应优先复用 `src/components/components.hpp` 中的语义颜色，不得在页面中重复定义
相同用途的颜色。

| 颜色角色 | 用途 | 色值 | 透明度 |
| --- | --- | --- | --- |
| 页面背景 | 页面根容器、导航栏、聊天列表、联系人列表和底部栏，与状态栏一致 | `#F2F2F7` | `100%` |
| 容器背景 | 设置行、输入框和对方气泡等内容控件 | `#FFFFFF` | `100%` |
| 容器标题 | 标题和小标题 | `#1A1A1A` | `100%` |
| 正文 | 正文和主要数值 | `#000000` | `100%` |
| 次要文字 | 预览、时间和辅助信息 | `#8E8E93` | `100%` |
| 边框 | 分隔线和容器边界 | `#C8C7CC` | `100%` |
| 主操作 | 发送、导航图标、选中页签和未读徽标 | `#0088FF` | `100%` |
| 主操作按下 | 主要按钮按下状态 | `#0062CC` | `100%` |
| 选中会话 | 当前会话列表行 | `#E9F0FA` | `100%` |
| 己方消息 | 己方消息气泡 | `#E1FFC7` | `100%` |
| 消息背景 | 会话消息区域和外观预览，与页面背景一致 | `#F2F2F7` | `100%` |
| 搜索背景 | 会话搜索框 | `#E9E9EB` | `100%` |
| 己方消息元信息 | 己方时间、状态和日期文字 | `#60884B` | `100%` |
| 日期背景 | 居中日期标签 | `#D3DECE` | `100%` |
| 次要徽标 | 灰色徽标和禁用发送按钮 | `#B6B6BB` | `100%` |
| 破坏性操作 | 退出登录、失败和错误文字 | `#FF3B30` | `100%` |
| 错误背景 | 会话错误提示条 | `#FFF1F0` | `100%` |
| 头像色板 | 首字符头像背景 | `#FF516A`、`#FFA85C`、`#9975DA`、`#4AC76F`、`#29B9D6` | `100%` |
| 连接就绪 | 连接状态文字 | `#2F9E44` | `100%` |
| 连接警示 | 连接中或等待网络状态 | `#D97706` | `100%` |

以上为当前 `src/components/components.hpp` 的项目 token。
`color_conversation_background` 是 `color_page_background` 的别名，页面底色统一
跟随 AppKit 状态栏的 `#F2F2F7`。页面底色、头像色板和状态色是本地适配；官方
分组背景为 `#EFEFF4`，不得把全部项目 token 描述为官方原始值。

新增颜色时必须先定义语义角色、使用场景、色值和透明度；两个页面共享的
颜色应提升到 `src/components/`。不得用渐变替代明确的颜色角色。

## 字体与文本

- 显示文本的 UI 源文件必须包含 `<cm0/typography.h>`。除下述应用专用字体场景外，
  LVGL 文本对象必须使用 `lilygo_ui_font_get(size)` 返回的 AppKit 字体。
- 必须在 `lv_init()` 完成后获取字体。`14`、`22`、`28`、`36` 和 `48` 像素是
  推荐的基础字号；产品层级或布局确有需要时，可向 `lilygo_ui_font_get(size)`
  传入其他正整数字号。
- AppKit 在首次请求某个字号时创建并缓存对应的完整字体链，后续相同字号复用该
  缓存。不得根据视口尺寸连续计算字号，也不得为相近文本层级引入大量不同字号；
  每个不同字号都会占用额外字体缓存，直至字体系统反初始化。
- 设置文本样式前必须确认字体指针非空。返回的字体对象及其 fallback 链由 AppKit
  共享和管理，应用不得修改或释放。
- 相同信息层级在同一应用中应使用一致的字号。缩小字号不得作为修复溢出的
  首选方案；应先精简文案、调整布局，或按产品要求换行和截断。
- 不得在公共 UI 中引用 `lv_font_montserrat_*`，也不得复制、改名或随应用重新打包
  AppKit 字体。

AppKit 在运行时通过 LVGL FreeType 加载共享字体文件。`lilygo_ui_font_get()` 返回
以 Inter 为主字体、Source Han Sans CN 为中文 fallback、Font Awesome 为图标字形
fallback 的字体链。中西文混排必须使用同一个字体链，不得为了切换字体拆分标签，
也不得依赖操作系统字体发现或默认字体回退。

### 应用专用字体

仅当 AppKit 运行时字体确实缺少应用所需字形时，才可在 `assets/fonts/` 提供
应用专用的 TTF、OTF 或 WOFF 字体。不得复制或重新打包 AppKit 已提供的 Inter、
Source Han Sans CN 或 Font Awesome 字体。应用专用字体必须满足：

- 通过 `lv_freetype_font_create()` 按当前 UI 字号在运行时加载；
- 将应用专用字体作为引用它的 LVGL 对象的主字体，并将其 `fallback` 设为同字号的
  `lilygo_ui_font_get(size)` 结果；
- 不得修改 `lilygo_ui_font_get()` 返回的共享字体对象或它的 fallback 链；
- 字体对象的生命周期必须不短于引用它的 LVGL 对象；移除所有引用后，必须通过
  `lv_freetype_font_delete()` 释放；
- 字体文件必须通过应用的安装和打包规则部署到稳定的运行时路径；
- 可通过可重复的子集化流程仅保留所需字形以减小安装体积，但不得手工修改生成的
  字体文件。

AppKit 的 Inter、Source Han Sans CN 和 Font Awesome 字体按 SIL Open Font License 1.1
分发。应用不得将它们描述为“无版权字体”或单独销售。包含字体的分发物必须
保留 `NOTICE.md` 和 `OFL-1.1.txt`；模板通过 AppKit 安装辅助函数包含共享字体的
许可文件。新增应用专用字体时，必须同时记录字体来源，并补充该字体的许可文件
和打包规则。

## 响应式布局

- 页面主体必须使用 LVGL Flex 或 Grid，并结合百分比尺寸、`LV_SIZE_CONTENT`、
  最小/最大尺寸、对齐、间距和滚动容器分配空间。
- 不得用大量绝对坐标组织页面主体，不得为横屏、竖屏或不同尺寸复制整套
  页面。
- 固定像素值只能用于有明确约束的尺寸，例如图标、边框、间距、控件下限和设计
  token。
- 连续尺寸变化应由 layout 自动处理。只有导航方向、栏数、内容优先级或交互
  方式发生结构变化时才设置断点。
- 断点必须依据父容器的实际可用宽高，不得依据 CPU 架构、设备型号或应用展示
  模式。可复用组件不得假设自己占满 display。
- 尺寸变化的布局更新必须可重复执行，不得把重建整个页面作为常规适配方式，
  并必须保留业务状态、焦点、滚动位置和用户输入。

响应式结果必须保证：

- 文本按产品要求换行或截断，不与相邻内容重叠；
- 主要操作始终可见、可达，并有足够的命中区域；
- 内容增多、文案变长或字体回退时，容器可扩展、重排或滚动；
- 横竖屏和尺寸切换后业务状态和用户输入不丢失。

## LVGL 对象和生命周期

- View 必须在调用者传入的 `parent` 下创建顶层对象，不得假设 parent 是当前
  screen 或整个 display。
- 页面 View 必须保存自己的顶层 LVGL 对象，并提供可重复调用的销毁流程。
- LVGL 父对象负责删除子对象。销毁父对象后必须清空所有缓存的子对象指针，
  不得重复删除。
- 事件回调使用 `user_data` 时，其指向对象的生命周期必须覆盖事件注册期间。
- 优先使用 LVGL 对象绑定的 observer。全局 observer、输入监听器、timer 和其他非对象
  绑定资源必须在销毁时显式注销。
- ViewModel 必须比绑定其 Subject 的 View 存活更久。Subject 应使用现有 RAII 封装，
  不得复制或移动底层 `lv_subject_t`。
- 延迟页面切换时，不得在当前 LVGL observer 或事件回调中同步删除触发回调的
  页面。
- 需要被渲染测试定位的关键对象必须设置稳定、有语义的 `lv_obj_set_name_static()`
  名称。

## 新增页面

新增页面前必须先检查 `src/pages/telegram/`、`src/domain/` 和现有测试。一个完整页面通常包含：

1. `src/pages/<page>/<page>_model.cpp/.hpp`：页面私有状态和领域规则；
2. `src/pages/<page>/<page>_view_model.cpp/.hpp`：命令、展示状态和 Subject；
3. `src/pages/<page>/<page>_view.cpp/.hpp`：LVGL 对象、绑定、回调和响应式布局；
4. `src/app.cpp` 中的创建和销毁，多页面应用则按需新增 `src/app_router.cpp/.hpp`；
5. Model、ViewModel 和用户可见行为的测试；
6. 修改后的预览图和用户文档。

页面私有 Model 与 ViewModel/View 放在同一目录，但仍必须编译到不同依赖层。纯 Model
代码不得因为目录内聚而链接 LVGL。

## 新增通用组件

只有组件被多个页面使用，或它明确承担通用绑定、样式或资源生命周期时，才放入
`src/components/`。否则应保持在所属页面目录。

通用组件必须：

- 在调用者传入的 parent 下构建；
- 以父容器的可用尺寸进行响应式布局；
- 复用语义颜色和 AppKit 字体；
- 声明所有权、回调 `user_data`、observer、timer 和销毁责任；
- 在最长文案、字体回退和支持视口下不发生重叠、裁切或不可达操作。

## 验收

每次 UI 修改必须至少完成：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

现有测试包括：

- `telegram-lvgl-photo-decoder`：启用内建 JPEG 后备时检查完整图片像素、缩小采样、
  边缘解码块、极窄尺寸、截断数据与资源边界；
- `telegram-json`：JSON 解析测试；
- `telegram-model`：不初始化 LVGL 的 Model 测试；
- `telegram-view-model`：ViewModel 命令、搜索、草稿、联系人排序、账号请求校验、
  过期响应隔离、临时私聊、偏好持久化及资源边界测试；
- `telegram-image-decoder-limits`：图片解码资源限制测试；
- `telegram-voice-identity`：语音播放消息身份测试；
- `telegram-qr-login-ui`：登录方式双向切换、等待状态、手机号草稿和二维码更新；
- `telegram-tdlib-auth-restart`：未完成的二维码认证重置、客户端重建、过期事件过滤
  和扫码成功与切换同时发生时的状态检查；
- `telegram-tdlib-mapping`：TDLib JSON 映射测试；
- `telegram-tdlib-account`：资料、联系人、群组和频道详情、通知、隐私例外保留、
  设备、黑名单及存储操作的 TDLib 请求和响应测试；详情覆盖过期请求、有效公开
  用户名、未知人数、实时更新、删除联系人及退出账号后的状态清理；
- `telegram-render-portrait`：`568x1232` 竖屏渲染和交互测试；
- `telegram-render-landscape`：`1232x568` 横屏渲染和交互测试。

头像渲染测试使用非正方形四色 PNG，检查聊天列表、会话标题、联系人、详情和群消息
发送者的真实图片像素、居中裁切、圆形遮罩、固定尺寸和首字符回退，并增加
`360x640` 小屏及损坏图片解码失败回退检查。测试图片只存在于测试临时目录，
不替换应用演示素材。

渲染测试直接捕获 LVGL 帧缓冲，输出到 `build/host-simulator/`。每种方向生成
`telegram-<portrait|landscape>.ppm` 会话图、在此文件名后追加 `.chats.ppm` 的
会话列表图、`.settings.ppm` 的设置图、`.contacts.ppm` 的联系人图、
`.profile.ppm` 的资料图、`.notifications.ppm` 的通知图、`.devices.ppm` 的设备图、
`.storage.ppm` 的存储图、`.latest.ppm` 的最新消息图、`.group-details.ppm` 的群组详情、
`.private-details.ppm` 的私聊资料和 `.contact-details.ppm` 的联系人详情，例如
`telegram-portrait.ppm.chats.ppm`。这些 PPM 是原生渲染产物，可转换为 PNG
便于预览；不应以网页重绘代替实际 LVGL 视觉验收。

详情交互测试检查刷新、返回和消息入口，以及草稿、消息列表位置和头像的保留。
群组、私聊详情另输出 `.compact.group-details.ppm` 和 `.compact.private-details.ppm`
两张 `360x640` 小屏预览。可查看仓库中的
[群组详情预览](../assets/previews/ios-group-info-portrait.png) 和
[联系人详情预览](../assets/previews/ios-contact-info-portrait.png)；头像为上述测试图案。

测试通过不能替代视觉检查。AI agent 在修改 UI 时还必须检查横竖屏渲染产物，
并确认：

- 布局不依赖单一固定分辨率；
- 文本无缺字、错误回退、重叠或裁切；
- 主要操作可见、可达，按钮文案和命中区域正常；
- 尺寸或方向变化后布局正确重排，业务状态、焦点、滚动位置和用户输入保留；
- 页面背景、容器、文本、边框和主操作使用正确的语义颜色；
- 关闭页面后无野指针、重复删除、未注销 observer、timer 或输入监听器。

修改 SDK、工具链、安装或打包相关 UI 资源时，还必须完成 `cm0-cross` 构建和
Debian 打包验证。
