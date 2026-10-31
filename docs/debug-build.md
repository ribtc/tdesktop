# macOS 本地 Debug 构建

在仓库根目录运行：

```sh
python3 Telegram/build/debug_build.py
```

入口复用已配置的 `out/`，执行 Telegram 的 Debug 增量构建。程序仍位于
`out/Debug/Telegram.app`，不会为每次构建创建应用副本或新的账号目录。

## 空间管理

查看占用与清理预览：

```sh
python3 Telegram/build/debug_build.py report
python3 Telegram/build/debug_build.py prune --dry-run
```

只执行清理：

```sh
python3 Telegram/build/debug_build.py prune
```

构建前和成功后都会执行同样的清理。范围限定为本 checkout 的 `out/build`：

| 内容 | 保留规则 |
| --- | --- |
| Xcode 构建描述 `XCBuildData/*.xcbuilddata` | 保留最近两份具有构建清单的描述；保留 `build.db` |
| Debug 编译、静态库归档失败留下的临时文件 | 删除符合编译器命名格式的 `.o.tmp`、`.a.<随机后缀>` |
| 当前 `.o`、`.a` 和预编译头 | 全部保留，供增量构建和源码调试使用 |
| 构建日志 | 写入 `out/debug-build.log`，轮转为 `.log.1`，每份约 32 MiB |
| `out/Debug`、`out/zed-profile`、其他账号目录 | 不清理 |
| 历史测试应用副本、旧手动日志、共享 Libraries、系统缓存 | 不自动清理，需按来源单独确认 |

对象文件和静态库会占用较大空间，但正常增量构建会更新相同路径。
不要把这部分固定占用当作历史备份，也不要在每次构建前删除整个 `out/build`。
删除它会导致全量重编译；执行恢复性全量重编译时，遵循 `AGENTS.md` 的专门流程。

入口默认要求至少 8 GiB 可用空间，以给编译和链接的临时文件留出空间。
这是预检门槛，不是磁盘用量上限。可按机器情况调整并发数和门槛：

```sh
python3 Telegram/build/debug_build.py --jobs 4 --min-free-gib 12
```

入口也会检查生成的 Xcode Debug 配置中、位于构建目录外的静态库和对象文件。
若已配置的 Qt 等依赖缺失，会列出路径并在启动构建前退出。共享 Libraries
是构建输入；删除其中的静态库不能代替清理构建历史。

同一 checkout 的入口有互斥锁。检测到本机的 CMake、Xcode、Clang、libtool 或 Swift
编译进程时，脚本拒绝清理，请等现有构建结束后重试。也不要同时从 Xcode、
其他终端或原始 CMake 命令启动构建。脚本不终止这些进程。

如果当前 checkout 的 Telegram 正在运行，构建会提示先关闭程序或结束调试。
取消此入口时只终止它自己启动的构建进程组。脚本不调用 `clean`，不删除
可执行程序，不读取或改写登录数据。

## Zed

在现有 `.zed/debug.json` 的“构建并调试”项中，将 `build` 设置为：

```json
{
  "command": "/usr/bin/python3",
  "args": ["Telegram/build/debug_build.py"],
  "cwd": "$ZED_WORKTREE_ROOT"
}
```

保留现有的 `program`、调试参数和 `-workdir`。直接调试已有程序的配置无需修改。

原始 `cmake --build out --config Debug --target Telegram` 仍能使用，但不会执行
本入口的空间预检、日志轮转和残留清理。

## 回归检查

```sh
python3 Telegram/build/test_debug_build.py
```

测试使用临时目录，检查清理边界、预览、符号链接、并发锁、运行进程保护和日志轮转。
