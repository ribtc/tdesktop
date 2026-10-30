# 账号网络上下文测试

此目标从公开的 `MTP::NetworkContext` 边界验证未绑定阻断、检测结果的账号归属、配置替换后的凭据撤销、旧解析结果拒绝和旧代理模式兼容。测试使用产品的真实上下文与代理校验代码，断言日志适配器只输出失败信息。

从仓库根目录运行独立 Debug 目标；将 Qt 路径替换为本机安装目录：

```sh
cmake -S Telegram/SourceFiles/tests/network_context -B out-network-tests \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build out-network-tests
ctest --test-dir out-network-tests --output-on-failure
```

未初始化子模块的 worktree 可以额外传入 `-DTELEGRAM_TOOLKIT_ROOT=/path/to/populated/checkout`，只使用该 checkout 的工具库头文件。主工程启用 `DESKTOP_APP_TEST_APPS` 时也提供 `test_network_context` 目标。

`Main::Account::network()` 在登录前已存在。账号为普通连接及密钥销毁连接传入同一个上下文；`MTP::Instance` 不会从当前窗口推断账号。上下文的修改与订阅留在构造线程，快照可跨线程使用。替换配置即撤销所有旧快照，消费者应在连接、写入或接受结果前检查 `allowed()`。阻断态与检测态都不放行业务连接。

账号入口默认使用严格模式。`Main::Account::bindProxy()` 将稳定代理 UUID 保存到账号 Storage KV，在普通和密钥销毁实例的关闭屏障全部完成后检测新代理。旧全局代理仅导入 `Core::ProxyLibrary`，不会给账号自动绑定。`completeValidation()` 拒绝其他账号、旧版本及格式无效的代理；此函数消费真实检测结果，不执行网络探测。

本票验证配置与回调约束，不证明完整流量隔离。异步会话重启不等于旧 socket 已关闭。绑定调用者在主线程先 `block()`，再调用每个相关 `MTP::Instance::quiesceNetwork(done)`，等所有回调返回才开始新绑定检测。普通实例与密钥销毁实例都需要等待。等待期间必须保持阻断；实例销毁会取消尚未送达的回调。

屏障保留会话及请求队列，关闭当前和已排队销毁的会话工作对象、密钥握手与其连接树。回调在工作线程完成整个析构栈后返回主线程；`Ready` 更新会重建工作对象。这个生命周期路径不在上述独立上下文测试内，需要用实际账户连接补充集成验证。辅助配置请求、独立 HTTP、通话与网页不由这个会话屏障关闭。代理库只允许新建或修改为数字地址的 SOCKS5/MTProto 配置；不支持的旧记录保留但不能绑定。

代理库测试覆盖 UUID 与路由保存恢复、一次性迁移、未知格式保留、禁止域名入口以及旧编辑器不能覆盖新版本。测试不读取账号数据。登录前 Storage KV、共享代理撤销、真实代理认证及 UI 行为仍需使用隔离测试目录做集成验证；此独立目标不能证明这些运行时行为。

基础界面提供 `CreateAccountProxyBox(account)` 和 `AccountProxyStatus(account)`。代理管理界面可复用 `CreateAccountProxyEditBox(proxy, save)` 的协议、数字地址和密钥校验，再由回调调用代理库；编辑框本身不修改绑定。账号绑定使用 `bindProxy(id)`、`unbindProxy()` 和 `retryProxy()`，不读取当前窗口账号。

`MTP::HttpTransport` 固定使用一个网络快照。只有已验证且入口为数字 IP 的 SOCKS5 可发送 HTTP(S)，目标域名由代理解析。传入 `NetworkContext &` 的构造方式自动订阅撤销，构造、使用和销毁必须留在上下文所属线程。传入快照的工作线程方式由拥有者同步调用 `revoke()`；它会终止回复并关闭连接池。任何旧代次都不能重新请求或跟随重定向。业务消费者在接受成功结果前仍须检查快照，拒绝已完成但延迟送达的旧回复。

本地 SOCKS5 观察测试确认目标域名在代理握手中传递、HTTP 返回值可读、阻断会同步终止已经到达代理的请求，以及上下文销毁和旧传输复用会失败。未绑定请求也确认未到达本地直接目的端或应用全局代理。测试只监听 loopback，不访问真实账号。它不覆盖操作系统 DNS 抓包、多账号实际文件下载、特殊配置服务、MTP 网页文件/地图/广播分片及应用工作线程集成；这些仍需专用账号和代理环境验证。

`test_calls_network` 使用通话媒体策略和真实账号网络快照，验证两种代理均拒绝尚未适配的 RTC，保留已验证账号的 MTP 广播权限，并覆盖解绑、旧快照、替换绑定、无关账号及账号销毁。旧全局路由不能授权账号媒体。它只验证策略边界，不模拟通话模型，也不能证明媒体控制器零发包或直播实际播放成功；这两项需要专用账号与代理端观测补充。运行 `ctest --test-dir out-network-tests -R calls_network --output-on-failure` 可单独执行该目标。
