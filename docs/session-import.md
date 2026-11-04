# Session 账号导入

欢迎页和二维码登录页提供“session号登陆”。账号管理页从“添加账号”菜单进入同一选项，宽窄窗口均可使用。文件选择器支持多选，每批最多 100 个文件。

支持 Telethon v1–v8 SQLite `.session` 文件；v8 的临时密钥字段不会替代永久授权密钥。读取同名 JSON 中的可选 `proxy` 字段，不使用其中的客户端指纹。暂不支持 Pyrogram、StringSession 或 tdata。源文件只读打开，每个文件最多 64 MiB。

导入使用客户端的生产 DC 配置。没有同名 JSON 或 JSON 缺少 `proxy` 时，沿用入口账号的直连或代理设置；`proxy: null` 明确使用直连。JSON 提供代理对象时，按类型、地址、端口、用户名、密码和 IPv6 偏好复用代理库中的配置，找不到则新增。代理通过协议检测后才验证 Session，成功导入的账号保存该代理绑定，重启后继续使用。只有服务器确认当前用户后才创建本地账号。重复账号保留原有登录；失败不影响同批其他文件。关闭导入窗口会取消未完成项，已成功的账号保留。从登录页导入时，关闭窗口后进入首个成功账号。

导入会复用文件中的已有授权。请先停止其他客户端或脚本对同一 Session 的使用；Telegram 会因主连接并发使用同一授权而返回 `AUTH_KEY_DUPLICATED` 并撤销授权，届时需要重新登录取得新文件。客户端会阻止本进程内同一密钥的并发导入。[Telegram 授权错误说明](https://core.telegram.org/api/errors)

## TGDirector 代理格式

将 TGDirector 导出的 `.session` 和同名 `.json` 放在同一目录，导入时只需选择 `.session`。JSON 中的代理格式为：

```json
{
  "proxy": {
    "type": "socks5",
    "host": "192.0.2.10",
    "port": 1080,
    "username": "example-user",
    "password": "example-password",
    "prefer_ipv6": false
  }
}
```

JSON 上限为 64 KiB；非法 JSON、代理字段类型错误和非法端口均拒绝导入，不回退到入口账号的连接。用户名、密码可省略，默认为空；IPv6 偏好可省略，默认为 false。原始文件保持只读。

当前代理库只允许数字 IP 地址的 SOCKS5/MTProto。TGDirector 导出的 SOCKS5 可直接复用；HTTP 或域名代理会显示不支持的代理配置，不会静默直连。已新增的代理在导入失败后仍保留在代理库，可检查后重试。导入期间修改或删除正在使用的代理会取消本批剩余导入。

## 模块边界

`main/session_file.h` 的 `ReadSessionFile(path)` 是可复用的只读转换接口。返回 `SessionCredentials` 或分类错误，只依赖 Qt Core 和 SQLite，不依赖 UI、网络、账号存储或 Telegram API。凭据包含 DC 编号、256 字节授权密钥和可选代理连接参数。

`main/session_import.h` 的 `SessionImport` 负责顺序批处理、超时、连接策略、服务器身份验证和去重，以 `updates()` 发布每个文件的状态。验证连接关闭后，`Main::Domain::addAuthorized()` 将已验证账号接入现有加密存储。

`intro/session_import_box.h` 的 `ChooseSessionFiles()` 统一承接各入口的多选文件选择和结果展示。将来增加格式时，在解析层提供新的适配器，无需复制登录页面或账号持久化逻辑。

## 验证

启用 `DESKTOP_APP_TEST_APPS` 后，`test_session_file` 覆盖真实格式的合成数据库、损坏文件、授权字段、版本以及源文件保真，以及代理认证信息、直连、旧文件兼容和异常 JSON。也可把本地 `.session` 文件路径作为额外参数，只输出通过或失败，不输出授权密钥。

真实登录验收使用独立工作目录，分别检查多选导入、账号管理中的重复导入，以及重启后的账号恢复。真实账号文件和工作目录不加入版本控制。
