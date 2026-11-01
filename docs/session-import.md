# Session 账号导入

欢迎页和二维码登录页提供“session号登陆”。账号管理页从“添加账号”菜单进入同一选项，宽窄窗口均可使用。文件选择器支持多选，每批最多 100 个文件。

支持 Telethon v1–v8 SQLite `.session` 文件；v8 的临时密钥字段不会替代永久授权密钥。不读取配套 JSON，也不支持 Pyrogram、StringSession 或 tdata。源文件只读打开，每个文件最多 64 MiB。

导入沿用入口账号的直连或代理设置，使用客户端的生产 DC 配置。只有服务器确认当前用户后才创建本地账号。重复账号保留原有登录；失败不影响同批其他文件。关闭导入窗口会取消未完成项，已成功的账号保留。从登录页导入时，关闭窗口后进入首个成功账号。

导入会复用文件中的已有授权。请先停止其他客户端或脚本对同一 Session 的使用；Telegram 会因主连接并发使用同一授权而返回 `AUTH_KEY_DUPLICATED` 并撤销授权，届时需要重新登录取得新文件。客户端会阻止本进程内同一密钥的并发导入。[Telegram 授权错误说明](https://core.telegram.org/api/errors)

## 模块边界

`main/session_file.h` 的 `ReadSessionFile(path)` 是可复用的只读转换接口。返回 `SessionCredentials` 或分类错误，只依赖 Qt Core 和 SQLite，不依赖 UI、网络、账号存储或 Telegram API。凭据包含 DC 编号及 256 字节授权密钥。

`main/session_import.h` 的 `SessionImport` 负责顺序批处理、超时、连接策略、服务器身份验证和去重，以 `updates()` 发布每个文件的状态。验证连接关闭后，`Main::Domain::addAuthorized()` 将已验证账号接入现有加密存储。

`intro/session_import_box.h` 的 `ChooseSessionFiles()` 统一承接各入口的多选文件选择和结果展示。将来增加格式时，在解析层提供新的适配器，无需复制登录页面或账号持久化逻辑。

## 验证

启用 `DESKTOP_APP_TEST_APPS` 后，`test_session_file` 覆盖真实格式的合成数据库、损坏文件、授权字段、版本以及源文件保真。也可把本地 `.session` 文件路径作为额外参数，只输出通过或失败，不输出授权密钥。

真实登录验收使用独立工作目录，分别检查多选导入、账号管理中的重复导入，以及重启后的账号恢复。真实账号文件和工作目录不加入版本控制。
