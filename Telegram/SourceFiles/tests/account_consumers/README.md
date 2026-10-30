# 账号 WebSocket 回归

此测试只用临时本地监听端，不读取账号目录。通过公开 `NetworkContext` 和 `WebSocketClient` 检查拒绝、代理域名 CONNECT 和服务端断开。`completeValidation` 是夹具建立路由的步骤，不代表真实代理检测成功。

用带子模块的 checkout 编译：

```sh
cmake -S Telegram/SourceFiles/tests/account_consumers -B /tmp/account-consumers -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build /tmp/account-consumers
ctest --test-dir /tmp/account-consumers --output-on-failure
```

子模块不在当前工作树时，为 CMake 添加 `-DTELEGRAM_TOOLKIT_ROOT=/path/to/initialized/checkout`。测试仅证明本机传输行为；真实账号、DNS 抓包、网页进程和平台运行结果应独立报告。
