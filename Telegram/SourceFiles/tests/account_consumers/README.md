# 账号 WebSocket 回归

此测试只用临时本地监听端，不读取账号目录。通过公开 `NetworkContext` 和 `WebSocketClient` 检查拒绝、代理域名 CONNECT 和服务端断开。`completeValidation` 是夹具建立路由的步骤，不代表真实代理检测成功。

用带子模块的 checkout 编译：

```sh
cmake -S Telegram/SourceFiles/tests/account_consumers -B /tmp/account-consumers -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build /tmp/account-consumers
ctest --test-dir /tmp/account-consumers --output-on-failure
```

子模块不在当前工作树时，为 CMake 添加 `-DTELEGRAM_TOOLKIT_ROOT=/path/to/initialized/checkout`。测试仅证明本机传输行为；真实账号、DNS 抓包、网页进程和平台运行结果应独立报告。

Unix 还运行 `websocket_capacity`，需要 Python 3、`openssl` 命令和至少 2048 个可用文件描述符的进程限额。macOS 使用 Qt Gui 的主线程事件分发器，其余 Unix 使用 Qt Core。测试保留描述符至 1100，再通过两个独立的本机 SOCKS5/TLS 端点完成 WebSocket 文本、二进制和 Ping/Pong 往返。

Ping/Pong 确认 worker 已处理响应后，测试撤销账号或显式关闭连接，检查旧回调不再交付。另检查关闭一个账号不影响另一个账号，以及回调中销毁客户端后共享 worker 可以重新启动。端点在独立 Python 进程使用系统 socket；临时自签证书仅供此测试进程使用，测试关闭证书验证，因此不覆盖 TLS 信任验证。测试不读取真实账号数据，也不代表 500 个账号的持续联网验收。
