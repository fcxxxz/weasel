# Codex Review 结果

本轮 review 基于 `docs/weasel-change-review-reference.md` 的检查路线，对当前本地未提交改动做静态审查。重点看按键热路径、IPC 恢复、服务退出/重启、托盘通知、部署安装流程。

本轮只读代码做 review，没有跑构建或单元测试。

## Findings

### Critical：按键热路径仍会同步阻塞 IPC

位置：

- `WeaselTSF/KeyEventSink.cpp:120`
- `include/PipeChannel.h:130`
- `WeaselIPC/PipeChannel.cpp:94`
- `WeaselIPC/PipeChannel.cpp:112`
- `WeaselIPCServer/WeaselServerImpl.cpp:485`

`OnKeyDown` 通过 `m_client.ProcessKeyEvent(ke, &eaten)` 进入 `TryTransact()`。当前 `TryTransact()` 只在没有 pipe handle 时用 `_EnsureOnce()` 避免等待 pipe；一旦已有连接，仍然同步执行 `WriteFile` 和 `ReadFile` 等待服务端响应。

服务端 key event 默认仍进入 `g_api_mutex`，因此 server 忙、Rime 卡、维护/初始化持锁、pipe peer 接收但迟迟不响应时，TSF 当前按键线程仍可能卡住。这和“按键热路径非阻塞”的目标不一致。

建议：

- 客户端热路径改成 overlapped I/O + 短超时 + cancel。
- 或服务端 key path 对 `g_api_mutex` 使用 try-lock，忙时立即返回未处理。
- 不应把可能阻塞 `ReadFile` 的同步事务命名和使用成 `TryTransact`。

### Critical：按键被吃掉后，commit/preedit 响应可能丢失

位置：

- `WeaselTSF/KeyEventSink.cpp:220`
- `WeaselTSF/Composition.cpp:389`
- `WeaselTSF/EditSession.cpp:14`
- `WeaselIPC/WeaselClientImpl.cpp:340`
- `WeaselTSF/WeaselTSF.cpp:362`

`OnKeyDown` 先调用 `ProcessKeyEvent` 拿到 `eaten`，再通过 `_UpdateComposition()` 请求 edit session。真正解析 commit/preedit 的地方在 `DoEditSession()` 里调用 `m_client.GetResponseData()`。

问题是 `GetResponseData()` 当前也用 `std::try_to_lock`。如果后台恢复线程在 keydown 和 edit session 之间抢到同一个 `m_client` 锁，`GetResponseData()` 会直接返回 false。此时按键可能已经对宿主返回 `eaten=true`，宿主不会再处理原始键，但 Rime 返回的 commit/preedit 没有被解析，可能表现为吞字、候选状态不更新或组合状态错乱。

建议：

- 把当前线程 response buffer 的解析从 `client_mutex` 里拆出来。
- 或保证 `ProcessKeyEvent` 与对应 `GetResponseData` 在同一 UI 线程上成对完成，中间不允许恢复线程抢锁。

### Critical：安装复制失败会先破坏现有 DLL

位置：

- `WeaselSetup/imesetup.cpp:44`
- `WeaselSetup/imesetup.cpp:51`
- `WeaselSetup/imesetup.cpp:64`

`copy_file()` 在任何 `CopyFile(src, dest, FALSE)` 失败后，都会先把当前目标文件移动到 `.old.*`，并安排重启删除，然后再重试复制。

这没有区分失败原因。如果失败是源文件缺失、权限、磁盘满、路径错误等，旧 DLL 会先被移走，新 DLL 仍复制失败，安装会被破坏。

建议：

- 先验证源文件存在且可读。
- 优先复制到同目录临时文件。
- 只有确认替换文件可用，并且失败原因确认为目标被占用时，才安排延迟替换/删除。
- 失败时保留原目标文件。

### Important：服务通知从 pipe worker 线程直接操作托盘，并绕过 handler 生命周期锁

位置：

- `WeaselIPCServer/WeaselServerImpl.cpp:188`
- `WeaselIPCServer/WeaselServerImpl.cpp:288`
- `WeaselIPCServer/WeaselServerImpl.cpp:304`
- `WeaselIPCServer/WeaselServerImpl.cpp:106`
- `WeaselServer/WeaselServerApp.cpp:45`
- `WeaselServer/WeaselTrayIcon.cpp:99`

`WEASEL_IPC_SHUTDOWN_SERVER` 和 `WEASEL_IPC_NOTIFY_SERVICE` 被排除在 `g_api_mutex` 外，但它们仍直接访问 `m_pRequestHandler` 并触发 `NotifyService()`。同时 `OnEndSystemSession()` 会在持锁状态下 `Finalize()` 并把 `m_pRequestHandler` 置空。

这存在两个风险：

- `m_pRequestHandler` 的读取、finalize、置空之间没有统一 happens-before，可能出现数据竞争。
- `NotifyService()` 的回调最终直接调用 `tray_icon.ShowServiceNotification()`，这发生在 pipe worker 线程上，跨线程操作托盘 UI 对象。

建议：

- handler 生命周期仍用同一把锁保护，或改成 `shared_ptr/weak_ptr` 管理。
- 托盘气泡通知通过 `PostMessage` marshal 回 server 主消息线程显示。

### Important：Backspace/Delete/Enter 的 TestKey 预测可能使用陈旧 `_status.composing`

位置：

- `WeaselTSF/KeyEventSink.cpp:77`
- `WeaselTSF/EditSession.cpp:11`
- `WeaselTSF/Composition.cpp:389`

`_TestKeyEvent()` 用 `_status.composing` 判断 Backspace/Delete/Enter/方向键等编辑键是否应该交给 Rime。但 `_status` 是后续 edit session 里解析服务端响应才更新，而 `_UpdateComposition()` 使用 `TF_ES_ASYNCDONTCARE`，可能异步返回。

因此刚输入进入组合状态后，下一次 `OnTestKeyDown(Backspace)` 仍可能看到旧的 `composing=false`，把 Backspace 放给宿主，导致删除宿主文本而不是 Rime preedit。

建议：

- 正式 `OnKeyDown` 收到 IPC 响应后，同步维护一个只用于 TestKey 预测的 composing/status 快照。
- 或先解析必要 status，再调度异步 edit session。

### Important：`/q`、`/restart*` 只做一次 `TryConnect()`，pipe 瞬时不可连就当作没有服务

位置：

- `WeaselServer/WeaselServer.cpp:205`
- `WeaselServer/WeaselServer.cpp:260`

服务命令启动时只调用一次 `client.TryConnect()`。如果 pipe 正忙、server 刚启动/退出、listener 处于间隙，`TryConnect()` 返回 false 后，`/q` 会直接走 “quit requested but no server” 并返回。

这会导致退出/重启命令被静默丢掉，调用方误以为服务已经停止。该现象也和之前日志里的 `/q ... connected=0 ... no server` 一致。

建议：

- 退出/重启命令使用有界重试。
- 同时检查单实例 mutex；如果 mutex 仍存在但 pipe 暂不可连，应继续等待或明确失败。
- 不要对 stop/restart 这类控制命令只做一次非阻塞连接。

### Important：修改安装后的后台流程和 1200ms 延迟退出存在竞态

位置：

- `WeaselSetup/WeaselSetup.cpp:134`
- `WeaselSetup/WeaselSetup.cpp:137`
- `WeaselSetup/WeaselSetup.cpp:140`
- `WeaselIPCServer/WeaselServerImpl.cpp:294`

修改安装完成后，后台线程按固定顺序执行：

1. `WeaselServer.exe /q`
2. `Sleep(500)`
3. `WeaselServer.exe /restart-manual`
4. `Sleep(500)`
5. `WeaselDeployer.exe /deploy`

但服务端收到 shutdown 后会延迟 1200ms 才 `Stop()`。因此 500ms 后旧服务通常仍在退出过程中，新服务和 deploy 可能与旧服务重叠，deploy 也可能连到即将退出的旧 server，造成部署通知丢失或服务状态不稳定。

建议：

- 不要用固定 sleep 串联。
- 等待旧服务 pipe 断开且单实例 mutex 释放后，再启动新服务。
- 确认新服务 pipe/mutex 就绪后再 deploy。

### Important：部署完成/失败通知被 `m_disabled` 状态门控

位置：

- `WeaselDeployer/Configurator.cpp:136`
- `WeaselDeployer/Configurator.cpp:153`
- `RimeWithWeasel/RimeWithWeasel.cpp:496`

部署器只有在 `client.TryConnect()` 成功时发送 `StartMaintenance()` 和 `EndMaintenance(result)`。server 端 `EndMaintenance(result)` 只有在 `m_disabled == true` 时才会 `_SetDeployMessage(result)` 并 `NotifyService(result)`。

如果 `StartMaintenance()` 消息丢失、server 在部署后才启动、或安装流程竞态导致 `m_disabled == false`，部署结果通知会被跳过。

建议：

- 部署结果通知应独立于 `m_disabled` 状态处理。
- 只把重新初始化和 UI 更新放在 `m_disabled` 分支内。
- `StartMaintenance()` / `EndMaintenance()` 最好返回可检查的 IPC 结果，让 deployer 能知道通知是否真的送达。

### Important：LanguageBar 退出先写 manual-exit flag，再确认 IPC 是否成功

位置：

- `WeaselTSF/LanguageBar.cpp:533`
- `WeaselTSF/LanguageBar.cpp:536`
- `WeaselServer/WeaselServerApp.cpp:67`

LanguageBar 退出菜单先调用 `MarkServiceManualExit()`，然后才 `TrayCommandSync(ID_WEASELTRAY_QUIT)`。如果 IPC 失败，代码只显示退出失败提示，不会回滚 manual-exit flag。

结果是服务可能仍在运行或未成功退出，但后续自动恢复已经被 manual-exit flag 抑制。server 托盘退出路径本身也会写 manual-exit flag，因此这里存在重复且提前写入。

建议：

- IPC 确认成功后再写 manual-exit flag。
- 或失败时清除 flag。
- 最好由真正执行退出的一端统一负责写 flag。

### Important：旧 `/restart` 的父进程判断过宽，未知父进程会误清 manual-exit flag

位置：

- `include/WeaselIPC.h:294`
- `include/WeaselIPC.h:302`

`ShouldTreatLegacyRestartAsManual()` 通过 `IsScriptLikeRestartParent()` 做黑名单判断：`cmd.exe`、`powershell.exe`、`pwsh.exe`、`conhost.exe`、`windowsterminal.exe`、`wt.exe`、`weaselserver.exe` 被视为脚本/非交互，其余父进程只要 `parent_path` 不为空，就会被当成“交互父进程”，从而清掉 manual-exit flag。

这和 review 目标里的“旧 `/restart` 只在明确来自交互父进程时兼容成手动重启；脚本/自动恢复场景不能误清 flag”不一致。未知 launcher、调度任务、第三方服务外壳等都可能被误判成交互重启。

建议：

- 改成白名单，只允许明确的交互入口清 flag。
- 至少为未知父进程加测试，例如 `notepad.exe` 或第三方 launcher 不应被当成手动重启。

### Important：SCM `Stop()` 无条件写 manual-exit flag

位置：

- `WeaselServer/WeaselService.cpp:90`

`WeaselService::Stop()` 在所有 SCM stop 路径上都会 `MarkServiceManualExit()`，然后再尝试通过 IPC shutdown server。

SCM stop 不等同于用户手动退出。管理员停服务、卸载、系统关机、服务管理器操作等路径都会走这里。如果写入 manual-exit flag，下次 `/recover` 会被抑制，需要用户手动重启算法服务才能恢复。

建议：

- 只在托盘退出或明确 IPC EXIT 路径写 manual-exit flag。
- SCM stop 使用不写 manual-exit flag 的 shutdown reason，或单独的 stop helper。

### Important：Rime 热路径 debug log 没有按 `WEASEL_TRACE_KEY_EVENTS` 门控

位置：

- `RimeWithWeasel/RimeWithWeasel.cpp:296`

`ProcessKeyEvent()` 在 `!handled && has_commit` 时每次都会写 `WeaselDebugLog`。这类分支在标点上屏、空格选词等场景可能很常见，属于按键热路径。

文档目标里明确要求 `WEASEL_TRACE_KEY_EVENTS` 关闭时不输出每键 trace，避免默认日志过重。这里没有门控，会让默认路径仍然产生按键级日志和字符串构造开销。

建议：

- 用 `ShouldTraceKeyEvents()` 或同等全局开关门控。
- 关闭 trace 时不要进入日志字符串构造路径。

### Important：TSF `TraceKeyEvent` 调用点先构造字符串，关闭 trace 仍有热路径开销

位置：

- `WeaselTSF/KeyEventSink.cpp:21`
- `WeaselTSF/KeyEventSink.cpp:45`
- `WeaselTSF/KeyEventSink.cpp:81`
- `WeaselTSF/KeyEventSink.cpp:127`
- `WeaselTSF/KeyEventSink.cpp:199`
- `WeaselTSF/KeyEventSink.cpp:222`
- `WeaselTSF/KeyEventSink.cpp:238`
- `WeaselTSF/KeyEventSink.cpp:257`

`TraceKeyEvent()` 内部会先检查 `ShouldTraceKeyEvents()`，但多个调用点在调用前已经构造了 `std::wstring`，包含多次 `std::to_wstring` 和字符串拼接。`ShouldTraceKeyEvents()` 本身每次也查环境变量。

因此 trace 关闭时，默认按键路径仍然承担字符串构造和环境变量查询开销。

建议：

- 把 trace 开关缓存起来，例如 `std::call_once` 初始化。
- 调用点先 `if (trace_enabled)`，关闭时不构造任何 trace 字符串。

### Important：旧 `WEASEL_IPC_PROCESS_KEY_EVENT` 缺少 session 守卫

位置：

- `WeaselIPCServer/WeaselServerImpl.cpp:248`
- `WeaselIPCServer/WeaselServerImpl.cpp:261`

新增的 `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS` 会先检查 `FindSession(lParam)`，无效 session 直接返回 0。旧的 `WEASEL_IPC_PROCESS_KEY_EVENT` 只检查 `m_pRequestHandler`，然后直接调用 `ProcessKeyEvent()`。

这让两个 key event 入口的语义不一致。旧客户端或旧调用路径传入 stale session 时，可能继续把 session 0 或无效 session 送进 Rime 处理。

建议：

- 旧 `OnKeyEvent()` 也加同样的 `FindSession(lParam)` 守卫。
- 同时处理本文档前面提到的 `FindSession()` 无副作用查询问题。

### Important：同步用户数据失败会漏掉 `EndMaintenance()`

位置：

- `WeaselDeployer/Configurator.cpp:218`
- `WeaselDeployer/Configurator.cpp:226`
- `WeaselDeployer/Configurator.cpp:236`

`SyncUserData()` 成功路径会先 `StartMaintenance()`，结束后 `EndMaintenance()`。但 `rime->sync_user_data()` 失败时直接 `CloseHandle(hMutex)` 并 `return 1`，没有通知 server 退出维护模式。

如果 `StartMaintenance()` 已经送达，失败早退会让 server 保持 disabled，直到后续其他路径重新初始化或用户手动重启。

建议：

- 维护模式用 RAII guard，保证所有 return 路径都会尝试 `EndMaintenance()`。
- 失败路径可以传部署/维护失败通知，至少不能静默留在 disabled 状态。

### Minor：`FindSession()` 会污染 session map

位置：

- `WeaselIPCServer/WeaselServerImpl.cpp:264`
- `RimeWithWeasel/RimeWithWeasel.cpp:159`
- `include/RimeWithWeasel.h:174`

`OnKeyEventWithStatus()` 用 `FindSession(lParam)` 检查 session 是否存在。但 `FindSession()` 内部调用 `to_session_id(ipc_id)`，而 `to_session_id()` 使用 `m_session_status_map[ipc_id]`。对于不存在的 stale session，这会插入一个默认 `SessionStatus`，即使最终返回 0。

建议：

- `FindSession()` 改为使用 `m_session_status_map.find(ipc_id)` 做无副作用查询。
- `to_session_id()` / `get_session_status()` 不应用于探测路径。

## Review 结论

这轮改动方向是对的：目标是减少按键路径阻塞、补齐服务通知、修正退出/重启语义。但当前实现仍有几个会真实影响体验和稳定性的硬问题：

- 按键路径还没有真正摆脱同步 IPC 阻塞。
- keydown 和 edit session 响应解析之间存在恢复线程抢锁导致响应丢失的窗口。
- 退出/重启/部署流程仍有一次性连接和固定 sleep 竞态。
- 托盘通知线程模型和 handler 生命周期还不稳。
- 安装复制失败路径有破坏现有安装的风险。
- Claude review 中的 H1/H2/H4/H5/H6/M6 已吸收到本文档；H3/M3/M7 与本文档已有项重合。

建议先处理 Critical，再处理服务退出/部署通知相关 Important，最后再清理 Minor。

## 修复后复核（2026-05-21）

本节记录根据 Claude review 与 Codex review 合并清单完成的一轮修复结果。已重新跑单元目标和主构建，见下方“验证”。

### 已处理

1. 按键服务端外层锁阻塞：
   - `WEASEL_IPC_PROCESS_KEY_EVENT` 和 `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS` 不再走阻塞 `g_api_mutex`。
   - 服务端 key path 改为 try-lock；锁忙时直接返回 0，让客户端按“未处理”快速返回宿主。
   - `TestWeaselIPC` 增加断言，锁定 key IPC 不走阻塞 outer lock。

2. keydown 与 edit session 响应解析之间的抢锁窗口：
   - `ClientImpl::GetResponseData()` 不再抢 `client_mutex`；response buffer 本来就是 `PipeChannel` 的 thread-local 数据，后台恢复线程不应阻塞当前 UI 线程解析响应。
   - `OnKeyDown` 在 `ProcessKeyEvent` 成功后立即解析一次 status-only 响应，提前刷新 `_status.composing`，降低 Backspace/TestKey 使用陈旧 composing 状态的窗口。

3. 安装复制安全：
   - `copy_file()` 不再在任意 `CopyFile` 失败后先移动旧 DLL。
   - 新流程先验证源文件，再复制到 `dest.new`，即时替换失败时只安排 delayed replace；失败时保留原目标文件。

4. 托盘通知跨线程：
   - `WEASEL_IPC_NOTIFY_SERVICE` 和 shutdown notification 不再从 pipe worker 线程直接调用 `tray_icon.ShowServiceNotification()`。
   - 新增 `WM_WEASEL_SERVICE_NOTIFY`，通过 `PostMessage` 收口到 server message loop 线程，再调用 handler 通知托盘。
   - `OnServiceNotifyMessage` 内部持 `g_api_mutex` 读取 handler，避免和 `OnEndSystemSession()` 的 finalize/置空并发。

5. manual-exit 语义：
   - 旧 `/restart` 父进程判断从黑名单改为白名单：只有 `explorer.exe`、`taskmgr.exe`、`WeaselDeployer.exe`、`WeaselSetup.exe` 等明确交互入口才兼容成手动重启。
   - 新增非手动 `/stop` / `/stop-service`，安装器和 stop 脚本改用 `/stop`，避免安装/卸载把用户 manual-exit flag 写脏。
   - SCM `WeaselService::Stop()` 改用 `WEASEL_IPC_SHUTDOWN_REASON_STOP`，不再写 manual-exit flag。
   - LanguageBar 退出不再先写 manual-exit flag；成功退出由 server 端 EXIT 路径统一写。
   - restart 路径去掉冗余 `ClearServiceManualExit()`，清 flag 收敛到命令语义入口和 server restart reason。

6. 部署/维护通知：
   - `EndMaintenance(result)` 的部署成功/失败通知不再被 `m_disabled` 门控；只把重新初始化和 UI 更新保留在 disabled 分支内。
   - `SyncUserData()` 失败早退前会尝试 `EndMaintenance()`，避免 server 停留在维护/disabled 状态。

7. session 与 IPC 细节：
   - `FindSession()` 改为 `map.find()` 无副作用查询，不再通过 `operator[]` 插入 stale session。
   - `_UpdateUI(0)` 不再通过 `to_session_id(0)` / `get_session_status(0)` 创建 session 0，也不再对 session 0 调 Rime option。
   - 旧 `WEASEL_IPC_PROCESS_KEY_EVENT` 加上和新 status 入口一致的 `FindSession` 守卫。
   - `PipeChannel::_Send()` 重连后改用新 pipe handle 重发，不再拿旧 handle 重发。
   - `PipeChannelBase::_Connect()` 在 `SetNamedPipeHandleState` 失败时关闭已打开 pipe，避免句柄泄漏。
   - `PipeServer::Listen()` 创建 per-connection worker 后显式 `detach()`，避免局部 `boost::thread` 析构时留下 joinable 线程风险。

8. 热路径日志：
   - `ShouldTraceKeyEvents()` 提到 `WeaselUtility.h` 并缓存环境变量结果。
   - TSF key trace 调用改为宏门控，trace 关闭时不再构造每键 trace 字符串。
   - Rime `!handled && has_commit` 的每键 debug log 改为受 `WEASEL_TRACE_KEY_EVENTS` 门控。
   - 移除 `idea-keytrace-20260521` 调试标记。

9. 重复 keydown guard：
   - `ActiveKeyDownGuard` 改为只判断是否应 suppress，不再在判断时自动记录。
   - `OnKeyDown` 只有在 `_ProcessKeyEvent()` 成功且 `pfEaten=TRUE` 后才记录 active key，避免 disabled/unknown/pass-through key 的第二次回调被误吞。
   - 单元测试同步改为显式 `Remember()` 后再验证 suppress。

10. 低风险清理：
    - `output/install.bat` 删除 IMM 时代遗留的 `regsvr32` 注释。
    - `imesetup.cpp` 的 IMM 注释改成当前 TSF-only 行为。
    - `include/resource.h` 的 `_APS_NEXT_COMMAND_VALUE` 与 `WeaselServer/resource.h` 对齐。

### 仍需真实 smoke 的点

1. 客户端 pipe 事务仍是同步 `WriteFile/ReadFile`。本轮通过服务端 key path try-lock 处理了 review 中最直接的 `g_api_mutex` 阻塞源，但没有把客户端 IPC 改成 overlapped I/O + timeout。若服务端拿到 key lock 后 Rime 自身长时间卡住，仍可能等待该次 round-trip。这个需要更大的 IPC 改造和真实延迟测试。

2. `_UpdateUICallback` / tray refresh 的数据流仍没有整体迁到 UI 线程快照模式。本轮只修了 `NotifyService(DWORD)` 这条跨线程托盘气泡路径；`Refresh()` 涉及 `UIStyle`/`Status` 中的字符串引用，不能只靠 `PostMessage` 简单搬线程，否则会引入 wstring 并发读写风险。

3. `WeaselSetup::CustomInstall` 仍然用后台线程串联 `/stop`、`/restart-manual`、`/deploy`，保留固定 sleep。由于 `/restart-manual` 进程最终会成为长驻 server，不能简单等待进程退出；这块要彻底消除 sleep，需要设计一个“等待新 server ready 后再 deploy”的专用命令或 IPC handshake。

4. 真机 TSF smoke 仍是必要项：Backspace 删除 preedit、JetBrains/Rider 重复输入、托盘退出后手动重启、部署完成通知都需要打包安装后验证。

### 验证

已执行：

```powershell
MSBuild .\test\TestWeaselIPC\TestWeaselIPC.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
.\x64\Release\TestWeaselIPC.exe /unit
```

结果：构建成功；`TestWeaselIPC.exe /unit` 输出 `No errors detected.`

已执行：

```powershell
MSBuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselSetup\WeaselSetup.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
```

结果：全部构建成功；仍有既有类型转换 warning，未出现新的 error。

已执行：

```powershell
git diff --check
```

结果：无 whitespace error；只输出当前工作区 LF/CRLF 归一化提示。
