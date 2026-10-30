# 多账号网络隔离集成验证

条目 1–8 已实现并集成。条目 9 已完成本机 Debug 构建、7/500 个模拟账号的原生界面与存储验证，以及本地 socket 隔离边界验证。真实 500 账号持续联网和双协议真实认证未完成，整体需求仍不能标记为已验收或可发布。

## 环境和方法

2026-10-30，MacBookPro18,3，32 GiB 内存、10 个逻辑 CPU，macOS 26.7.2 (25G332)，Qt 6.11.2，Apple clang 17。最终运行产品版本为 `07552bf97829188a90027185ac6b870f3e1bd8a4`。

使用 APFS 克隆的独立 Debug app 与新建空白 portable profile。没有使用日常账号、现有账号目录或真实凭据。所有合成身份明确命名为 SIMULATED，环境为 Test，通过真实 `Domain`、`Account::createSession()`、存储和原生窗口创建与恢复。每次切换前检查账号白名单、Test 环境和阻断状态。

可移除的 `-testagent` overlay 只用于测试，不提交到产品。详细日志、截图、原始失败和资源采样位于 `/tmp/tdesktopUltra-account-network-isolation/`；总记录为 `integration-validation-09.md`。各运行保存独立的 `test_log.txt`、`runner-output.json`、截图和进程采样。

## 构建与测试入口恢复

| 阶段 | 产品版本 | 结果和证据 |
| --- | --- | --- |
| 首轮无 overlay 的生产 Debug | `6e0291f4a6` | `build-09.log`：BUILD SUCCEEDED，1023 CompileC |
| 最终候选带测试 overlay 的 Debug | `07552bf978` | `build-overlay-final-09.log`：BUILD SUCCEEDED，4 CompileC；此前各集成构建也成功 |
| 恢复原 no-op 后的生产 Debug | `07552bf978` | `build-production-final-09.log`：BUILD SUCCEEDED，1 CompileC |

最后一轮构建后再次核对 `test_scenario.cpp` 与原文件完全相同，其 SHA256 为 `e06ace7302e9a47d7911493d4b80c384aef10120efd80174660cc3d4e8ca7dc4`，没有提交 overlay。未执行 clean、删除 out/build、改动 out/Debug 中的 portable 账号或停止日常客户端。仅清理本次创建且已退出的旧测试 app 副本，测试 profile 和全部证据保留。

## 最终运行结果

| 运行 | 结果 | 总耗时 | RSS 采样峰值 | 线程采样峰值 |
| --- | --- | --- | --- | --- |
| `run-seven-final-01` | 新空白 profile，7 个模拟账号，全部通过，正常退出 | 20.903 秒 | 1,122,192 KiB | 31 |
| `run-500-final-01` | 恢复原 500 个模拟账号，全部通过，正常退出 | 58.523 秒 | 5,045,744 KiB | 532 |
| `run-seven-final-restore-01` | 同版本恢复，125% 缩放，全部通过，正常退出 | 12.580 秒 | 1,145,920 KiB | 40 |
| `run-retry-final-01` | 连续重试和解绑取消，全部通过，正常退出 | 45.015 秒 | 882,640 KiB | 28 |

资源每秒采样一次，数值为采样峰值。500 场景的 532 线程为启动瞬态，稳定界面阶段约 30 线程；没有把稳定值当作峰值。16 GiB RSS 保护阈值未触发。

实际 rail 点击各采样 20 次，7 个账号 P95 为 62.0323 毫秒，7 个账号恢复且 125% 缩放为 85.2865 毫秒，500 个账号为 123.434 毫秒。计时结束条件为下一次检查时目标本地内容可见、启用且 active/primary/session 一致，含约 60 毫秒轮询粒度。点击在两个已加载目标间切换，没有远端历史下载；不将其称为最终绘制时间、全 500 个目标逐一切换或真实联网性能。

## 实际网络边界

- 全新空白 profile 中，真实 MTP `help_GetConfig` 和 HTTP 请求在未绑定时被阻断。先验证本地监听端的正向连接，再观察 1.5 秒内目的端新增连接为零。该结果只覆盖指定监听端，不代替全进程外网或 DNS 抓包。
- 最小本地 SOCKS5 端点收到真实 MTP CONNECT。fixture 明确模拟代理验证通过，不连接 Telegram 完成认证。`block` 后 `quiesceNetwork` 的回调可观察 peer EOF/ECONNRESET，回调一次；停止中的 worker 也完成关闭屏障。
- 共享执行池中，A 后建立三个空实例再建立 B，覆盖本机同一执行分片。A 销毁后 B 的 socket 保持，并可再次完成实际 SOCKS 握手。`stopSession` 后立即析构时，对端在析构返回前已关闭。
- `KeysDestroyer` 使用明确的 dummy key 与本地端点。实际 socket 在 quiesce acknowledgement 前关闭，回调一次，其他实例保持可用。
- 500 个账号恢复后，本地端点 descriptor 为 1054，实际 dispatcher 为 `QEventDispatcherUNIX`。原生 socket 正向往返验证端点有效；独立 MTP 实例在高 descriptor 下完成 SOCKS 握手与关闭屏障，500 个合成账号的业务流量始终阻断。
- 同条件下，公开 `StartProxyCheck()` 完成真实 SOCKS 握手；`ResetProxyCheckers()` 返回前 peer EOF 已可观察，随后 500 毫秒无迟到完成/失败回调。
- 通过真实账号绑定操作完成 held proxy A → B → 解绑。替换返回时 A 的 peer 已关闭，B 建立新的实际 SOCKS 连接，解绑返回时 B 的 peer 已关闭。
- 实际拒绝连接后，5 秒、再 10 秒的自动重试都产生新 socket；解绑后的 22 秒观察期新增连接为零。此测试没有模拟远端恢复 Ready。
- 500 项批量操作对全部冻结目标生效且 `allowed=0`。16.089 秒内本地观察到 8 次连接，前 4 项进入 Failed，界面显示失败而非“已被其他操作替换”。没有等待 500 项检测全部排空，其余检测通过解绑取消；不将此记录写成 500 个代理验证完成。

最终 500 运行没有 `QSocketNotifier: Internal error`。客户端依然受真实网络、协议和系统资源约束，以上本地结果不能证明真实 500 账号持续在线。

## 原生界面和存储

500 个模拟账号在早期 `run-500-01` 中真实创建并保存，最终运行复用这份 profile，不补造恢复失败的账号。最终检查包括 ID、名称、Test 环境、分组与阻断状态。7 个账号同样由空白 profile 创建并真实重启恢复。

管理页实际执行名称搜索、50 页统计、下一页/上一页、全选 500 项后搜索仍保留冻结选择、创建与移动分组、分组筛选和批量绑定。原生 rail、管理页、代理列表及导入抽屉保存了宽窄窗口截图；7 个账号恢复覆盖 125% 缩放。

对照四个设计节点补齐代理列表的可识别列、导入抽屉的拖放区域与关闭入口。实际截图发现并修复长说明不换行和预览未显示；最终断言检查预览可见、尺寸非零、与 viewport 相交，说明完整换行且无密码明文。原始截图和修复后截图均保留。

## 实测发现与修复

共享 MTP 执行池将每实例独占线程改为有界分片，账号状态与连接仍独立。本次 macOS Qt Debug 构建的主线程 CF socket notifier 会拒绝大于 1024 的 descriptor；代理检查器和账号 WebSocket 迁入专用工作线程，使用适合高 descriptor 的事件循环。取消与析构仍同步关闭各自 socket，不终止其他账号连接。

批量结果从每次状态变化重排一个包含所有账号的大文本，改为只更新该账号的结果行。已有绑定重试采用 5/10/20/40/60 秒退避，正常检查代次变化不再被误报为绑定替换，账号检查统一进入四并发可取消队列。

保留了原始失败，未以新运行覆盖旧日志：

- 最早 7 账号恢复因 fixture 漏写 MTP config 被环境 gate 拒绝，后续 74 步跳过。另一批因 fixture 缺非空占位 phone，按存储规则未写 self，产生 8 个失败。修正 fixture 前提后新建 profile，未放宽恢复断言。
- 早期 500 运行出现约 1022 线程和超过 3000 的 descriptor。旧主线程观察端也受 CF 限制，故其零连接不能单独归因于产品。后续端点移入独立线程，并用原生正向控制消除这个混杂因素。
- 某些旧运行自动 PASS，但视觉检查或高 descriptor 诊断未通过验收。记录明确区分自动断言、有效观察端和人工视觉结论，不将这些旧 PASS 当作最终证据。

## 独立回归与双轴复核

账号上下文、通话、消费者、索引、管理模型和导入队列的独立测试均有通过记录。`/tmp/account-review-tests` 只构建并运行了 `proxy_import`，结果为 1/1；该目录另两个缺少可执行文件的目标不计通过。

`/tmp/account-websocket-capacity/Testing/Temporary/LastTest.log` 中 `account_consumers` 与 `websocket_capacity` 共 2/2 通过。高 descriptor WebSocket 测试保留文件描述符至 1100，经两个独立本地 SOCKS5/TLS 端点完成文本、二进制、Ping/Pong，验证撤销、迟到回调、关闭一方不影响另一方以及回调中销毁后重建。测试关闭临时自签证书验证，不覆盖 TLS 信任链。ASAN 在 main 前遇到运行时死锁，未计入通过。

Standards：原审查 3 个 P3（2 个硬性规范、1 个启发式）均修复；容量增量审查另 1 个格式 P3 已修复。

Spec：原审查 1 个 P1、2 个 P2 均修复并复核，分别为持续重试、绑定操作身份和检测并发。额外的取消重入回归通过。容量增量审查没有新增高置信规格问题。两轴统计分别保留，不混为一个优先级总数。

## 仍未完成

真实 500 个在线账号的持续负载、真实 SOCKS5/MTProto 双协议认证与出口验证、长时间故障恢复、完整外网/DNS 观察仍缺专用环境。大未读数、浅色主题及主题切换等未覆盖界面维度也保持未验。没有使用日常账号替代这些环境，没有发布或关闭父 issue。
