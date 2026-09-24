# LilyChat

面向低资源 ARM64 Debian 设备的轻量非官方 Telegram 客户端。LilyChat 通过 TDLib
使用 Telegram API，属于 Telegram 生态，但与 Telegram 官方无隶属或背书关系。
项目使用 C++17、
LVGL 9.5 和 TDLib JSON 接口，目标硬件为四核 1 GHz Cortex-A53 与
512 MB LPDDR2。

应用显示名称由 `lpm.toml` 的 `project.title` 统一配置。图标采用独立的对话气泡
设计，矢量源文件为 `assets/app-icon.svg`，启动器使用 `assets/app-icon.png`
（128 × 128）。包名、应用 ID 和数据目录继续沿用原标识，以保持安装和数据兼容。

## 当前功能

已支持：

- 已有 Telegram 账号登录，包括手机号、邮件、验证码和两步验证密码流程；
- 聊天列表、日期分组消息和横竖屏加载更早历史消息；
- 在缓存会话的名称和最后消息预览中本地搜索；
- 按会话保留内存中的文本草稿，切换会话或页签后恢复；
- 打开会话后同步已读状态，播放语音时同步已收听状态；
- 纯文本消息接收和发送；
- JPEG、PNG 图片消息接收、下载与显示；
- 聊天列表、会话标题、联系人和消息发送者的圆形头像，未设置或加载失败时回退到首字符；
- Ogg Opus 语音消息接收、下载与 ALSA 播放；
- 参考 Telegram iOS 经典浅色主题的横屏和竖屏触控界面；
- `Contacts` / `Chats` / `Settings` 三个页签；
- 联系人列表、本地搜索、按姓名或在线状态排序、详情、按手机号添加和删除联系人；
- 从联系人信息按钮或会话标题、头像查看用户资料，包括头像、姓名、状态、手机号、用户名和简介；
- 查看普通群组、超级群组和频道的头像、名称、类型、人数、公开链接和简介；
- 从联系人打开私聊，以及从设置打开 Saved Messages；
- 查看个人资料，修改姓名、用户名和简介；
- 分别设置私聊、群组和频道的通知开关、消息预览与声音；
- 设置手机号、最后上线状态、头像的可见范围，并保留已有隐私例外；
- 查看登录设备并终止其他设备的会话，查看黑名单并解除屏蔽；
- 查看存储用量、清理本机媒体缓存、设置照片自动下载和消息字号；
- 连接状态、应用信息和带确认的退出登录。

暂不支持：

- 视频、动画、普通音频及文档预览；
- 录音，以及语音、图片或文件发送；
- 新账号注册；
- 在应用内完成登录所要求的 Telegram Premium 购买；
- 展示剧透内容或阅后即焚媒体；这些消息会显示安全占位提示；
- 编辑、回复、转发、远程全局搜索、消息历史搜索、通话、动态贴纸和 secret chat；
- 群成员名单浏览及群组管理；
- 对方已读回执展示。

设置仅展示已接入的功能。隐私设置可选择所有人、联系人或无人，已有用户和群组
例外会保留并显示数量；本版本尚不编辑例外名单，也不提供两步验证密码修改、
手机号更换、账号删除、代理或系统推送通知配置。

本地搜索范围是当前已缓存的最多 100 个会话，不向 Telegram 发起搜索请求。
草稿仅存在本次应用运行的内存中，不写入磁盘、不与 Telegram 同步，退出账号或
应用后不保留。己方消息后的单勾只表示没有待发送或失败标记，不代表对方已读。
联系人搜索只匹配已加载的最多 500 位联系人，范围包括姓名、用户名和手机号；
ASCII 字母匹配不区分大小写，其他 Unicode 文本保留原样匹配。

用户、群组和频道详情在进入页面时通过 TDLib 按需加载，提供加载、错误和重试
状态。可见字段取决于 Telegram 返回的数据和隐私权限，隐藏的手机号等资料无法
获取。详情只保留当前查看对象，退出账号后清空；从会话进入详情再返回时保留
消息草稿和滚动位置。

PNG 和常规 baseline JPEG 可由内建 LVGL 解码器显示；语音播放依赖目标系统上的
ALSA 与 libopusfile。缺少语音库不会阻止文本、图片与登录功能启动。

## 界面参考

界面参考官方 [Telegram-iOS](https://github.com/TelegramMessenger/Telegram-iOS)
提交 `6ad963e5b62d354da79040f388ae2b9132fb17b8` 的
[经典浅色主题](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramPresentationData/Sources/DefaultDayPresentationTheme.swift#L539)：
蓝色操作、圆形头像、连续会话列表、浅绿色己方气泡、日期标签和紧凑发送栏。
窄屏进入独立会话，宽屏并列显示侧栏和会话；联系人使用连续列表，设置使用分组
列表和具有返回导航的详情页。
用户、群组和频道详情参考
[PeerInfoHeaderNode.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/PeerInfo/PeerInfoScreen/Sources/PeerInfoHeaderNode.swift#L1656)
的居中头像、名称和状态层级，以及
[PeerInfoProfileItems.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/6ad963e5b62d354da79040f388ae2b9132fb17b8/submodules/TelegramUI/Components/PeerInfo/PeerInfoScreen/Sources/PeerInfoProfileItems.swift#L158)
的标签与资料值排列；详情头像固定为 96 像素，长简介在信息行中换行。

导航栏、聊天列表、联系人列表、会话消息区和底部栏的背景统一采用状态栏的
`#F2F2F7`；设置行、输入框和对方气泡等内容控件保留白色。
实现采用不透明 LVGL 控件和纯色背景，保留 AppKit 的 Inter、
Source Han Sans CN 与 Font Awesome 字体链，适配设备资源与触控尺寸。
详细源码参考、语义颜色和交互约束见 [用户界面设计](docs/01-user-interface.md)。

## 准备环境

需要 CMake 3.21 或更新版本、C/C++17 编译器、`pkg-config`、LPM，以及已初始化的
AppKit 子模块。主机模拟器还需要 SDL2，设备构建需要 AArch64 交叉编译环境。

```sh
git submodule update --init --recursive
```

## 主机演示模式

主机 preset 默认启用确定性的本地演示后端，不连接 Telegram，也不需要账号或
TDLib。演示模式中的资料、联系人和云端设置修改只影响当前演示会话：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
TELEGRAM_DEMO=1 lpm start --no-build --foreground
```

## 连接 Telegram

真实后端需要从 [my.telegram.org](https://my.telegram.org/) 申请应用的 `api_id` 和
`api_hash`，并在运行时提供兼容的 TDLib JSON 共享库：

```sh
cmake --preset host-simulator -DTELEGRAM_DEMO=OFF
cmake --build --preset host-simulator --parallel
TELEGRAM_DEMO=0 \
TELEGRAM_API_ID=123456 \
TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef \
LILYGO_UI_TDJSON_LIBRARY=/absolute/path/to/libtdjson.dylib \
lpm start --no-build --foreground
```

`lpm start` 可以直接启动真实后端，取决于运行环境和构建默认值。
`TELEGRAM_DEMO=0` 会覆盖演示构建默认值；仍需有效的 API 配置和 TDLib 共享库。
在本机已有 `.lpm/telegram-debug.env` 和 `.cache/tdlib/install/lib/libtdjson.dylib`
配置时，可以用现有助手启动已构建的模拟器并复用登录状态：

```sh
.lpm/start-telegram.sh
```

此脚本加载本机配置后调用 `lpm start --no-build`，修改代码后应先重新构建。
配置文件属于本机凭据，不应提交或在调试输出中展示。

Linux 主机上的 TDLib 路径通常以 `.so` 结尾。应用通过 `dlopen()` 加载 TDLib，
不在链接阶段依赖它。共享库必须导出以下 JSON ABI 符号：

```text
td_create_client_id
td_send
td_receive
```

## ARM64 构建与打包

打包时应同时配置 `TELEGRAM_API_ID` 和 `TELEGRAM_API_HASH`。已有本机凭据文件时：

```sh
. .lpm/telegram-debug.env
lpm pack
```

CMake 会将环境中的 API 配置填入尚未设置或为空的同名构建缓存，并编译进程序，
因此安装包可直接使用这些默认值。后续 `lpm pack` 会复用缓存；更换凭据时用
`-DTELEGRAM_API_ID=... -DTELEGRAM_API_HASH=...` 更新 CMake 配置。设备上的同名运行时
环境变量仍可覆盖内置值。凭据文件、构建缓存和生成头文件不应提交。

也可以直接使用底层构建命令：

```sh
cmake --preset cm0-cross -DTELEGRAM_DEMO=OFF
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

生成的 Debian 包不会捆绑 `libtdjson`；真实后端包声明 Debian 13 的
`libtdjson1.8.38` 依赖，通过 `apt install ./包名.deb` 安装时会自动获取。
应用支持该包的版本化库名 `libtdjson.so.1.8.38`。其他系统需要单独提供与目标
Debian、glibc 和 AArch64 ABI 兼容的 TDLib 共享库及其传递依赖，并可用
`LILYGO_UI_TDJSON_LIBRARY` 指定绝对路径。媒体运行库建议安装：

```text
libasound2
libopusfile0
```

CM0 BSP 0.1.0 生成的包要求 `libc6 >= 2.38`、`libstdc++6 >= 14.1`，适合 Debian 13
或提供等价 ABI 的系统。Debian 12 及更早版本应改用对应发行版的旧 sysroot 重新
构建，不能只绕过包管理器的版本检查。

交叉 sysroot 没有 libjpeg 开发文件时，构建会自动启用 LVGL 的有界 baseline JPEG
后备，配置输出为 `Telegram: using LVGL's bounded baseline JPEG decoder`。若 sysroot
提供 libjpeg，应用会改用后台解码与缩放路径，并输出
`Telegram: JPEG photo decoding enabled`；该路径也能处理 progressive JPEG。

内建 JPEG 后备会将解码块直接采样到最大 `720 x 720` 的完整像素缓冲区，再交给
LVGL 缩放，避免分块缩放导致图像错位。图片下载和解码按当前可见消息触发，
同时处理最多 3 张，沿用已有的图片缓存限制。

头像按可见区域下载，使用 Telegram 提供的小尺寸图片，独立于消息照片自动下载
开关。图片保持比例并居中裁切为圆形；尚未下载、未设置或无法解码时显示彩色
首字符。收到头像更新后替换对应图片，退出账号或清理缓存时移除本地头像缓存。

## 运行配置

| 变量 | 用途 |
| --- | --- |
| `TELEGRAM_DEMO` | `1` 强制演示后端，`0` 强制真实 TDLib 后端；未设置时使用构建默认值 |
| `TELEGRAM_DEMO_IMAGE` | 覆盖演示后端使用的本地图片路径 |
| `TELEGRAM_API_ID` | Telegram 应用 ID，必须是正整数 |
| `TELEGRAM_API_HASH` | Telegram 应用 hash |
| `TELEGRAM_DATABASE_KEY` | TDLib 本地数据库原始加密密钥，首次使用后应保持不变 |
| `LILYGO_UI_TDJSON_LIBRARY` | `libtdjson` 共享库的绝对路径 |
| `XDG_DATA_HOME` | 覆盖应用数据根目录 |

`TELEGRAM_API_ID` 和 `TELEGRAM_API_HASH` 也能通过同名 CMake 参数设置，但运行时
环境变量优先。不要提交包含凭据的构建缓存，也不要在日志中输出验证码、两步验证
密码或数据库密钥。

应用会按 TDLib JSON `bytes` 规则在内部 Base64 编码数据库密钥；环境变量应提供
原始密钥文本，不要预先 Base64 编码。

## 数据目录

设置 `XDG_DATA_HOME` 时，TDLib 数据存放在：

```text
$XDG_DATA_HOME/lilygo-ui-telegram/database
$XDG_DATA_HOME/lilygo-ui-telegram/files
$XDG_DATA_HOME/lilygo-ui-telegram/ui-preferences.json
```

否则使用 `$HOME/.local/share/lilygo-ui-telegram/`；两者都不可用时才退回系统临时
目录。下载的图片和语音是普通文件，应使用 Unix 目录权限和设备存储加密保护。
不要同时运行多个使用同一数据库目录的进程。

`ui-preferences.json` 只保存消息字号（18、22、26、30）、照片自动下载开关和
联系人排序方式。文件使用结构化 JSON 原子替换，读取最多 4096 字节；无效文件
使用默认值，保存失败会显示错误并保留原偏好。偏好在真实和演示模式间共享，
退出账号后仍保留，不包含账号资料、登录凭据、验证码或草稿。

清理媒体缓存会交给 TDLib 回收本机下载文件，并刷新图片缓存和存储统计；
它不会删除 Telegram 云端消息或退出登录。关闭照片自动下载后，已缓存图片仍可
显示，未下载照片不会自动下载；当前没有单张照片的手动下载入口。语音仍在点击
播放时按需下载。

## 512 MB 资源边界

应用对主要常驻数据和媒体峰值设置了硬限制：

| 资源 | 限制 |
| --- | ---: |
| 界面聊天列表 | 100 个聊天 |
| 当前聊天消息窗口 | 80 条消息 |
| 联系人列表 | 500 位联系人 |
| 用户、群组或频道详情 | 1 个当前查看对象 |
| 登录设备列表 | 100 个会话 |
| 黑名单列表 | 500 项 |
| ViewModel 中待完成的账号操作 | 16 项 |
| TDLib 到 UI 的事件队列 | 256 项 |
| TDLib 待处理请求 | 256 项 |
| 每次 UI pump 处理事件 | 64 项 |
| 编辑框消息长度 | 最多 4096 个字符，并设 16 KiB UTF-8 后端上限 |
| 同时排队或解码的图片 | 3 项 |
| libjpeg 输入与解码目标 | 编码文件最多 8 MiB；源尺寸最大 2048 x 2048、最多 4 MiPixels；输出最大 720 x 720、最多 4 MiB |
| 内建 JPEG 后备 | 编码文件最多 8 MiB、尺寸最大 2048 x 2048、最多 4 MiPixels |
| 已解码图片缓存 | 最多 3 张、合计最多 6 MiB |
| 头像缓存 | 最多 64 项；PNG、JPEG 解码目标最大 120 x 120，像素缓冲合计不超过 3.6 MiB |

联系人和黑名单超过限制时显示已加载数与服务端总数，搜索仅覆盖已加载部分。
从联系人打开的空私聊不会被强行插入主聊天列表；发送消息后按 TDLib 的列表更新
正常出现。

语音按块解码，不会把整段语音一次性展开到内存。上述限制不是进程 RSS 上限，
TDLib、共享库映射、字体缓存和系统服务仍会占用额外内存。目标机建议使用精简
Debian 用户空间，并在真实账号规模和长时间收发场景下监控 RSS 与磁盘空间。

## 测试与文档

主机测试覆盖 JSON 解析、Telegram Model、账号请求关联与校验、搜索排序、偏好
持久化、TDLib JSON 映射、隐私例外保留，以及横竖屏界面交互与渲染。渲染测试输出
原生 LVGL PPM 到 `build/host-simulator/`，包括
`telegram-portrait.ppm`、`telegram-portrait.ppm.chats.ppm`、
`telegram-portrait.ppm.settings.ppm`、`.contacts.ppm`、`.profile.ppm`、
`.notifications.ppm`、`.devices.ppm`、`.storage.ppm` 及对应的 `landscape` 产物。
更完整的运行、部署、安全和故障排查说明见：

- [Telegram 运行与部署](docs/02-telegram-runtime.md)
- [用户界面设计](docs/01-user-interface.md)
