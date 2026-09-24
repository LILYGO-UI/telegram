# Telegram 运行与部署

本文说明当前 `lilygo-ui-telegram` 实现的功能边界、运行架构和 ARM64 Debian
部署要求。目标设备是四核 Cortex-A53、512 MB LPDDR2；以下限制是当前代码中的
实际限制，不代表 Telegram 或 TDLib 的完整能力。

## 功能边界

当前版本支持：

- 使用手机号、邮件地址、邮件验证码、Telegram 验证码和两步验证密码完成已有
  Telegram 账号登录；
- 加载聊天列表和当前聊天的消息历史；
- 在已加载会话和联系人中本地搜索；加载、添加、删除联系人并打开私聊；
- 查看个人资料、编辑姓名、用户名与简介，以及打开 Saved Messages；
- 设置私聊、群组和频道通知；设置手机号、最后上线和头像的基本可见范围，保留
  现有隐私例外；查看登录设备并终止其他会话，查看黑名单并解除屏蔽；
- 查看存储并清理本机媒体缓存，保存消息字号、照片自动下载和联系人排序偏好；
- 打开会话后向 TDLib 回写已读状态，开始播放语音时回写已收听状态；
- 接收和发送纯文本消息；
- 接收图片消息，下载后显示 PNG 和 baseline JPEG；构建时找到 libjpeg 时也支持
  progressive JPEG，并在工作线程缩放图片；
- 接收 Telegram 语音消息，下载 Ogg Opus 文件后通过 ALSA 播放；
- 显示未支持消息的占位提示，而不是尝试解析其内容。

当前版本不支持：

- 视频、动画、普通音频和文档的播放或内容预览；
- 录音、发送语音、发送图片或其他文件；
- 编辑、回复、转发、远程搜索、消息历史搜索、通话、贴纸动画和 secret chat；
- 编辑隐私例外名单、修改两步验证密码、更换手机号、删除账号及应用内代理配置；
- 展示剧透文本、剧透图片和阅后即焚媒体；这些内容会被安全占位提示替代；
- 新账号注册，以及在应用内完成登录所要求的 Telegram Premium 购买。相关状态只
  会提示用户改用官方客户端处理。

baseline JPEG 与 PNG 由 LVGL 内建能力显示。语音依赖设备上的 ALSA 和
libopusfile；缺少它们时文本、图片和登录仍可使用，语音按钮会报告能力错误。

## 线程与数据流

LVGL 只能在 AppKit 的 UI 主线程访问。当前数据流如下：

```text
LVGL 主线程（每 50 ms）
  TelegramView -> TelegramViewModel::pump() -> TelegramModel
          ^                 ^
          |                 | 有界事件队列（256，单次最多消费 64）
          |                 |
          |           TDLib JSON 工作线程
          |
          +--- 图片结果队列 <--- libjpeg 解码工作线程（构建时可选）
          +--- 语音结果队列 <--- Ogg Opus/ALSA 播放工作线程
```

没有 libjpeg 开发文件时，baseline JPEG 通过 LVGL 的 TJpgDec 在绘制阶段流式解码，
不建立整张 ARGB 缓存；送入解码器前仍会检查编码大小、尺寸和像素数。

TDLib 回调、图片解码回调和语音播放回调都不会直接调用 LVGL。关闭应用时先停止
媒体工作线程，再停止并等待 TDLib 工作线程，以免回调引用已销毁的界面对象。
`TelegramViewModel` 是单次生命周期对象；调用 `stop()` 后不会在同一实例上重新
启动，重新打开应用会创建新实例。

联系人和设置由不依赖 LVGL 的 `TelegramAccountModel` 保存状态，通过同一个
ViewModel 调用 TDLib 工作线程中的账号协议适配器。每项用户操作可包含多次
TDLib 请求，例如修改资料后重新读取资料；界面在完整操作结束后才显示成功。
账号请求 ID 在 ViewModel 生命周期内单调递增，认证重置后忽略过期响应。

TDLib 适配层还有一个容量为 128 的命令队列。队列满时命令会被拒绝并向界面报告
错误；它不会无限增加内存。

## 512 MB 资源边界

当前实现通过下列硬限制约束应用自身的常驻对象和媒体峰值：

| 资源 | 当前限制 |
| --- | ---: |
| 界面聊天列表 | 100 个聊天 |
| 当前聊天消息窗口 | 80 条消息 |
| TDLib 适配层聊天缓存 | 256 项 |
| TDLib 适配层消息缓存 | 512 项 |
| TDLib 适配层用户缓存 | 512 项 |
| TDLib 到 UI 的事件队列 | 256 项 |
| 联系人 / 登录会话 / 黑名单 | 500 / 100 / 500 项 |
| ViewModel 待完成账号操作 | 16 项 |
| 账号协议适配层用户 / 聊天名称缓存 | 1024 / 512 项 |
| 账号协议适配层待处理 TDLib 请求 | 192 项，并受全局请求限制 |
| 每个 UI tick 处理的 TDLib 事件 | 64 项 |
| 编辑框消息长度 | 最多 4096 个字符，并设 16 KiB UTF-8 后端上限 |
| 同时排队或解码的图片 | 3 项 |
| libjpeg 输入与单张解码目标 | 编码最大 8 MiB；源尺寸最大 2048 x 2048、最多 4 MiPixels；输出最大 720 x 720、最大 4 MiB |
| 内建 JPEG 后备 | 编码最大 8 MiB、尺寸最大 2048 x 2048、最多 4 MiPixels |
| 已解码图片缓存 | 最多 3 张且总计不超过 6 MiB |
| LVGL 内部堆 | 8 MiB |

语音按 48 kHz、双声道、S16_LE 分块解码，不会一次把整段语音解码到内存。上述
限制不包含 TDLib 自身、共享库映射、DRM framebuffer、字体缓存和系统服务的
内存，因此它们不是整个进程的 RSS 上限。设备应使用精简 Debian 环境；zram 可
作为系统级兜底，但不能替代内存泄漏和峰值测试。

聊天文件目录目前没有应用级字节数硬上限。应用通过
`setOption("use_storage_optimizer", true)` 启用 TDLib 存储优化器，但仍需监控可用
磁盘空间，不能假设下载目录永远保持固定大小。

## TDLib ARM64 共享库

应用通过 `dlopen()` 使用 TDLib JSON ABI，不包含 TDLib 头文件，也不在链接阶段
依赖 TDLib。运行时依次查找：

```text
libtdjson.so.1
libtdjson.so
libtdjson.dylib       # 仅方便主机开发
```

也可以用 `LILYGO_UI_TDJSON_LIBRARY` 指定共享库的绝对路径。适配层要求共享库导出
以下符号：

```text
td_create_client_id
td_send
td_receive
```

设备上的库必须是 AArch64 ELF，使用与目标 Debian/glibc 兼容的 ABI。应用不会
检查 TDLib 的 C++ ABI，但 JSON 对象字段仍可能随 TDLib 版本演进；生产镜像应固定
一个经过登录、历史、文件下载和退出测试的 TDLib 版本，不要在设备启动时下载
未经验证的最新版。

建议把 `libtdjson` 制作为单独的 ARM64 Debian 包，或者安装在版本化目录后通过
`LILYGO_UI_TDJSON_LIBRARY` 选择。检查部署结果：

```sh
file /opt/tdlib/lib/libtdjson.so
ldd /opt/tdlib/lib/libtdjson.so
nm -D /opt/tdlib/lib/libtdjson.so | grep -E 'td_create_client_id|td_send|td_receive'
```

TDLib 自身通常还依赖 OpenSSL、zlib、C++ 运行库和线程库；以该 ARM64 共享库的
`ldd` 输出为准，不要用开发机的依赖列表代替设备检查。

## API ID 和 API Hash

真实 Telegram 客户端必须使用在 `my.telegram.org` 申请的应用 `api_id` 和
`api_hash`。两种配置方式的优先级为：运行时环境变量高于 CMake 默认值。

构建时配置：

```sh
cmake --preset cm0-cross \
  -DTELEGRAM_DEMO=OFF \
  -DTELEGRAM_API_ID=123456 \
  -DTELEGRAM_API_HASH=0123456789abcdef0123456789abcdef
cmake --build --preset cm0-cross --parallel
```

`lpm pack` 会继承当前 shell 的环境。CMake 中未设置或为空的 `TELEGRAM_API_ID`
和 `TELEGRAM_API_HASH` 缓存会从同名环境变量初始化，并写入程序。例如使用本机配置：

```sh
. .lpm/telegram-debug.env
lpm pack
```

已有非空 CMake 缓存优先于构建时环境变量，后续直接运行 `lpm pack` 会复用它们。
更换内置凭据时需用上述 `-D` 参数更新缓存。仅在模拟器启动时设置环境变量，
不会改变已生成的安装包。

运行时配置：

```sh
export TELEGRAM_API_ID=123456
export TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef
export TELEGRAM_DATABASE_KEY='设备专用的数据库密钥'
```

`TELEGRAM_API_ID` 必须是正整数。缺少或无效的 ID/hash 时，真实后端不会连接，
界面会显示配置错误。CMake 值会写入生成头文件和二进制，且原值会出现在构建树
的 `CMakeCache.txt` 中；不要提交包含凭据的缓存或构建目录。运行时环境变量也可能
被同一用户或 root 从进程环境中读取，量产设备宜通过权限为 `0600` 的服务配置
文件注入，而不是写进公开启动脚本。

`TELEGRAM_DATABASE_KEY` 默认是空字符串。应用会按 TDLib JSON `bytes` 规则在内部
做 Base64 编码，因此变量应提供原始密钥文本，不要预先编码。首次建立数据库后应
保持密钥不变；丢失或更换密钥会导致原 TDLib 数据库无法打开。当前实现不提供
密钥恢复界面。

## Debian 运行依赖

应用生成的 Debian 包声明以下硬依赖：

```text
libc6 (>= 2.38)
libstdc++6 (>= 14.1)
libgcc-s1
libdrm2
libfreetype6
libpng16-16
zlib1g
libbz2-1.0
libbrotli1
lilygo-ui-appkit-dev (>= lpm.toml 中的最低版本)
```

包中还把以下媒体运行库列为 `Recommends`：

```text
libasound2
libopusfile0
```

包不会捆绑或声明某个发行版特定名称的 `libtdjson` 包；真实后端部署时必须另外
提供 ARM64 `libtdjson.so` 及其 `ldd` 所列依赖。TDLib 包名在不同镜像中并不统一，
因此不要假设执行 `apt install libtdjson` 一定可用。

这些最低版本来自 CM0 BSP 0.1.0 产物实际引用的 `GLIBC_2.38`、
`GLIBCXX_3.4.30` 和 `CXXABI_1.3.15`。当前 BSP 面向 Debian 13；需要支持 Debian 12
或更早系统时，应使用目标发行版的 sysroot 重新构建，而不是强制安装或忽略依赖
版本。

交叉 sysroot 没有 libjpeg 开发文件时，CMake 会启用 LVGL 的 baseline JPEG 后备，
并输出 `Telegram: using LVGL's bounded baseline JPEG decoder`。若找到 libjpeg，
则输出 `Telegram: JPEG photo decoding enabled`，生成的包会把
`libjpeg62-turbo` 列为硬依赖。语音库通过运行时动态加载，因此缺少 ALSA 或
libopusfile 不会阻止应用启动，只会使播放失败。

## 构建和启动

主机模拟器默认启用确定性的本地演示后端，不需要 TDLib 或账号：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
TELEGRAM_DEMO=1 lpm start --no-build --foreground
```

要在主机上测试真实后端：

```sh
cmake --preset host-simulator -DTELEGRAM_DEMO=OFF
cmake --build --preset host-simulator --parallel
TELEGRAM_DEMO=0 \
TELEGRAM_API_ID=123456 \
TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef \
LILYGO_UI_TDJSON_LIBRARY=/absolute/path/to/libtdjson.dylib \
lpm start --no-build --foreground
```

运行时 `TELEGRAM_DEMO=0` 可覆盖演示构建默认值，所以 `lpm start` 本身可以启动
真实后端。在已经配置本机 `.lpm/telegram-debug.env` 和缓存 TDLib 共享库的工作区，
`.lpm/start-telegram.sh` 会加载这些配置并调用 `lpm start --no-build`，复用现有
数据目录和登录状态。脚本不编译修改过的代码；启动前先完成主机构建。

交叉构建和打包：

```sh
cmake --preset cm0-cross -DTELEGRAM_DEMO=OFF
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

安装包后，可从 Launcher 启动，也可在设备终端前台运行以查看错误：

```sh
TELEGRAM_API_ID=123456 \
TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef \
TELEGRAM_DATABASE_KEY='设备专用的数据库密钥' \
LILYGO_UI_TDJSON_LIBRARY=/opt/tdlib/lib/libtdjson.so \
/usr/bin/lilygo-ui-telegram --display drm
```

AppKit 默认使用 `/dev/dri/card0` 和平台默认输入设备。实际节点不同时可追加
`--drm /dev/dri/cardN`、`--input /dev/input/eventN` 和
`--orientation portrait|landscape`。

`TELEGRAM_DEMO=1` 可在运行时强制使用演示后端；它只用于 UI 和测试，不连接
Telegram，也不能证明 TDLib 部署正确。演示资料、联系人和服务端设置修改只保留
在本次演示进程中，本地 UI 偏好仍按正常路径保存。

## 数据目录与安全

TDLib 数据位于：

```text
$XDG_DATA_HOME/lilygo-ui-telegram/database
$XDG_DATA_HOME/lilygo-ui-telegram/files
$XDG_DATA_HOME/lilygo-ui-telegram/ui-preferences.json
```

未设置 `XDG_DATA_HOME` 时使用
`$HOME/.local/share/lilygo-ui-telegram/`；两者都不可用时才退回系统临时目录。
运行用户必须对目录有读写权限。不要让多个应用进程同时使用同一数据库目录。

`ui-preferences.json` 仅保存 `message_font_size`（18、22、26、30）、
`auto_download_photos` 和 `contacts_sort_online`。使用同目录临时文件写入并原子
替换，读取上限 4096 字节；文件无效时采用默认值，保存失败时保留原偏好并反馈
错误。这三个偏好在退出账号和重启后保留，不包含登录信息或聊天草稿。

Storage Usage 的 Clear Media Cache 通过 TDLib `optimizeStorage` 回收可清理的
本机下载文件，并刷新统计。云端消息和登录状态保留；文件如仍被占用或被 TDLib
保留，清理后用量不保证归零。关闭照片自动下载不移除已缓存图片。

数据库启用了消息、文件和聊天信息存储，secret chat 被禁用。TDLib 使用传入的
`TELEGRAM_DATABASE_KEY` 保护本地数据库；下载后的图片和语音文件仍是普通文件，
应依靠 Unix 目录权限和设备存储加密保护。不要在日志中输出验证码、两步验证密码
或数据库密钥。

升级 TDLib 或更换数据库密钥前，应先停止应用并备份整个
`lilygo-ui-telegram` 数据目录。遇到数据库损坏时先把原目录移到隔离位置再重新
登录，避免直接删除唯一的诊断或恢复副本。

## 故障排查

### 显示 TDLib 不可用

- 确认 `LILYGO_UI_TDJSON_LIBRARY` 是绝对路径且运行用户可读；
- 用 `file` 确认它是 AArch64，而不是 x86-64 或 macOS 库；
- 用 `ldd` 查找 `not found`；
- 用 `nm -D` 确认三个必需 JSON ABI 符号存在；
- 前台启动应用，查看 `dlopen` 返回的完整错误。

### 显示 API ID/hash 未配置

确认没有只设置其中一个变量，并检查 `TELEGRAM_API_ID` 是否为正整数。运行环境
优先于构建值；服务管理器或 Launcher 不一定继承交互式 shell 的 `export`。

### 登录一直停在连接中

检查网络、DNS、系统时间和时区。当前 UI 没有代理配置入口。若账号进入“需要
注册”或“需要购买 Premium”状态，当前版本无法继续，应先在官方客户端完成对应
操作。

### 图片一直显示占位图

- 确认文件下载目录可写且磁盘未满；
- 确认构建日志中启用了 libjpeg 或 LVGL baseline JPEG 后备；
- libjpeg 和内建后备都会拒绝超过 8 MiB 编码大小、2048 x 2048 或 4 MiPixels 的
  JPEG；libjpeg 路径还会把输出缩小到最大 720 x 720，且输出不得超过 4 MiB；
- progressive JPEG 需要构建时找到 libjpeg；
- 当前自动解码路径只覆盖 JPEG，PNG 交给 LVGL，其他图片格式不会显示。

### 语音无法播放

确认设备存在 `libopusfile.so.0` 和 `libasound.so.2`，并检查运行用户能否打开 ALSA
的 `default` PCM。可安装 `alsa-utils` 后用 `aplay -L` 辅助检查设备枚举；当前应用
没有选择其他 ALSA 设备的设置页。语音文件必须是 TDLib 下载的有效 Ogg Opus。

### 内存或界面响应异常

用 `free -h`、`ps` 或 `smem` 同时观察应用 RSS 和系统可用内存。确认没有完整桌面
环境占用大部分 512 MB，也不要把视频或无界媒体缓存加入当前进程。事件持续积压
或出现 `command queue is full` 表示 UI/网络处理未能及时跟上，应保留前台日志和
复现账号规模进行分析。

### 退出时出现 TDLib 未及时关闭

适配层最多等待 TDLib 关闭 5 秒。若未收到关闭状态，会保留共享库映射直到进程
退出，避免后台线程执行已卸载代码；这条警告不应通过强制 `dlclose` 规避。反复
出现时应记录 TDLib 版本、网络状态和退出日志。
