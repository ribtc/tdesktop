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

当前账号入口显式使用旧代理适配器，供迁移前保持现有行为。第一次 `bind()` 或 `block()` 使该上下文退出旧模式，此后全局代理变化不再覆盖它。`completeValidation()` 拒绝其他账号、旧版本及格式无效的代理；此函数消费真实检测结果，不执行网络探测。

本票验证配置与回调约束，不证明完整流量隔离。异步会话重启不等于旧 socket 已关闭；启用严格隔离的调用者还需等待关闭屏障，并接入密钥握手、辅助 HTTP、通话和网页等实际传输路径。地址字面量及首版协议限制由后续绑定策略实施。
