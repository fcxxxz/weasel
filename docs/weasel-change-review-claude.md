# Weasel `fix-key-event-latency` Review（Claude）

本文档是基于 `docs/weasel-change-review-reference.md` 给出的检查路线，对当前 `fix-key-event-latency` 分支（含未提交工作区改动）做的全面 review 结论。

- 工作区 diff：32 文件，+2521 / -741
- 已提交 commits：6 个 perf 提交（master..HEAD）
- review 维度：reuse / quality / efficiency / correctness / doc-criteria
- review 分工：热路径&线程、IPC 协议&UI 去重、服务生命周期&托盘、安装/打包/测试
- 已交叉对照 `docs/weasel-change-review-codex.md`，新增/修正项见末尾"与 Codex review 交叉对照"段。

## 通过标准对照

引用自 reference 文档"Review 通过标准"。

| 标准 | 状态 | 说明 |
|---|---|---|
| 按键热路径没有新的同步等待、阻塞重连或无界锁等待 | ⚠️ | 客户端侧改进到位（try_lock + TryTransact + _EnsureOnce + buffer 清理）；但服务端 `OnKeyEventWithStatus` 仍在 `g_api_mutex` 内，慢任务会卡。详见 C6。 |
| TestKey 不再推进 Rime 状态 | ✅ | `OnTestKeyDown`/`OnTestKeyUp` 只读 `_status` 和调 `ShouldEatTestKeyEvent`，不调 `process_key`，不改 composition。 |
| Backspace 回归测试存在且语义正确 | ⚠️ | 覆盖了无组合/有组合/CapsLock/Ctrl-shortcut/cache/repeat-guard/restart 命令行 7 大类，差一个 `ShouldEatKeyEvent(processed=TRUE, commit=TRUE)` 断言。 |
| 服务手动退出、自动恢复、手动重启三者语义不互相打架 | ⚠️ | 存在两处真实风险，见 H1、H2。 |
| 托盘提示覆盖部署完成/失败、退出、重启中、重启成功、重启失败 | ✅ | 五条都映射到正确文案和图标类型，server 端 1200ms 延迟保留 balloon 显示时间，三语种字符串资源齐全。 |
| 单元测试通过，TSF/Server/Setup 目标可构建 | — | 未运行验证命令；review 仅基于源码。 |
| 未出现 TSF apartment 跨线程、IPC buffer 污染、manual-exit flag 竞态 | ✅ | 这三类高风险未发现。后台恢复线程严格 `_Reconnect(false, false)` 不触 `_status` 和 `_UpdateLanguageBar`；`AddRef/Release` 用 `InterlockedIncrement/Decrement`。 |

整体判断（初版）：没有 blocker，若干高优先项建议合并前处理。

**复评（结合 Codex review 后）**：Codex 暴露的 C1（安装破坏 DLL）、C2（响应丢失）、C3（session map 污染）、C6（服务端 outer lock 仍卡按键）属于硬问题，本文末尾"与 Codex review 交叉对照"段已收编并给出建议处理顺序。建议把 C1、C6 当成 blocker 候选 ——
- C1：让用户在升级失败时丢失现网 IME，等同回滚不了的故障。
- C6：分支名是 `fix-key-event-latency`，服务端阻塞这一段不修则未达到主目标。

## 高优先（建议合并前修）

### H1 — `IsScriptLikeRestartParent` 方向反了

- 位置：`include/WeaselIPC.h:294-311`
- 严重度：high
- 维度：correctness / doc-criteria
- 现状：用黑名单枚举 `cmd.exe / powershell.exe / pwsh.exe / conhost.exe / windowsterminal.exe / wt.exe / weaselserver.exe`，其余一律算"交互父进程"，会清掉 manual-exit flag。
- 问题：reference 文档要求"旧 `/restart` 只在明确来自交互父进程时兼容成手动重启；脚本/自动恢复场景不能误清 flag"。当前实现意味着任何不在黑名单里的父进程（`notepad.exe`、第三方 launcher、调度任务、未知 service host……）都会被当成手动重启。
- 唯一兜底：`parent_path` 为空时被当 SCM 恢复，不清 flag。
- 建议：反转为白名单（`explorer.exe` / `taskmgr.exe` / `WeaselDeployer.exe` / `WeaselSetup.exe` 等已知交互入口）。在 `TestWeaselIPC` 加一个 `notepad.exe` 父进程的 case 锁住意图。

### H2 — `WeaselService::Stop` 无条件写 manual-exit flag

- 位置：`WeaselServer/WeaselService.cpp:90`
- 严重度：high
- 维度：correctness / doc-criteria
- 现状：所有走 SCM `Stop()` 的路径都会 `MarkServiceManualExit()`。
- 问题：reference 文档只要求"用户手动退出会写 manual-exit flag"，而 SCM `Stop` 在管理员命令、卸载、机器重启等场景下都会被调，写入 flag 后下次 `/recover` 会被压制，需要用户手动 `/restart-manual` 才能恢复。
- 建议：拆 shutdown reason，只在托盘 quit / IPC EXIT 路径里写 flag。SCM `Stop` 走一个不写 flag 的 reason（或者根本不调用这个 helper）。

### H3 — `WEASEL_IPC_NOTIFY_SERVICE` 在非 UI 线程触发 Shell_NotifyIcon

- 位置：`WeaselIPCServer/WeaselServerImpl.cpp:188-198, 275-307`
- 严重度：high
- 维度：correctness

#### 现状

- `ServerImpl` 自己是 `CWindowImpl`，HWND 在 `ServerImpl::Run` 跑的 `CMessageLoop` 线程上创建。
- pipe 收发跑在另一条 `boost::thread pipeThread` 上。
- `PipeMessageNeedsOuterApiLock` 把 `WEASEL_IPC_NOTIFY_SERVICE` 和 `WEASEL_IPC_SHUTDOWN_SERVER` 都判成不需要 outer lock。
- 这两条命令最终都会调 `m_pRequestHandler->NotifyService(notification)`，server 端注册的回调是 `[this](DWORD n){ tray_icon.ShowServiceNotification(n); }`（`WeaselServerApp.cpp:45-48`）。
- 结果：`Shell_NotifyIcon` 在 pipe worker 线程被调用，且跟拿 `g_api_mutex` 的 UI 路径可以并发。`Shell_NotifyIcon` 文档要求由建图标的 window owner 线程调，违反这一条。

#### 修法：PostMessage 收口到 server message loop 线程

`ServerImpl` 已经是 `CWindowImpl`，直接在 message map 加一条自定义消息：

```cpp
// WeaselServerImpl.h
#define WM_SERVER_NOTIFY_SERVICE (WM_APP + 1)

BEGIN_MSG_MAP(WEASEL_IPC_WINDOW)
  ...
  MESSAGE_HANDLER(WM_COMMAND, OnCommand)
  MESSAGE_HANDLER(WM_SERVER_NOTIFY_SERVICE, OnServiceNotifyMessage)
END_MSG_MAP()

LRESULT OnServiceNotifyMessage(UINT uMsg, WPARAM wParam, LPARAM lParam,
                               BOOL& bHandled);
```

```cpp
// WeaselServerImpl.cpp
DWORD ServerImpl::OnServiceNotification(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  PostMessage(WM_SERVER_NOTIFY_SERVICE, static_cast<WPARAM>(wParam), 0);
  return 0;
}

LRESULT ServerImpl::OnServiceNotifyMessage(UINT, WPARAM wParam, LPARAM,
                                           BOOL&) {
  if (m_pRequestHandler)
    m_pRequestHandler->NotifyService(static_cast<DWORD>(wParam));
  return 0;
}
```

`OnShutdownServer` 里原本同步调的 `m_pRequestHandler->NotifyService(notification)` 同样改成 `PostMessage(WM_SERVER_NOTIFY_SERVICE, ...)`。后面 detached thread `Sleep(1200) → Stop()` 期间 message loop 会把通知处理掉，足够 balloon 显示。

#### 为什么 Post 不 Send

- pipe worker 跑 `SendMessage` 会同步等 UI 线程，把延迟拉回来，跟"非阻塞"基调拧着。
- PostMessage 立刻返回；NotifyService 通知幂等，丢一条不影响正确性。
- HWND 在 `WM_DESTROY` 后 Post 失败：通知丢了用户最多少看一个 balloon，可接受；想严谨可在 `OnDestroy` 翻一个 `std::atomic_bool m_running = false`，Post 前判一下。

#### 为什么 `tray_icon.Refresh()` 不在这次范围里

评估过，**风险显著大于 NotifyService，不打包进 H3。**

- `NotifyService` 跨线程只传 `DWORD` —— atomic、值传递、无共享状态。
- `Refresh()` 读的是 `m_style`/`m_status` 引用，包含 `current_zhung_icon` / `current_ascii_icon` 等 `std::wstring`。写方是 pipe worker 在 `g_api_mutex` 内写。
- 只 PostMessage `Refresh()` 而不改数据流：UI 线程读 wstring 期间 worker 改写 = 真正的 UB，不是 DWORD 那种 benign torn read。
- 安全迁移路径需要先**在 PostMessage 前快照所需字段**，把快照塞进消息或 staging 结构。这要改 `RimeWithWeaselHandler` 的 update 路径和 `WeaselTrayIcon` 的状态读取接口，scope 远大于 H3。
- `SendMessage` 同步等可绕开 wstring race，但语义上把 UI 线程拉回来等 worker，违背非阻塞目标。

挂为 follow-up：`_UpdateUICallback` 路径上的 tray 操作也应该用同样的 PostMessage 模式收口到 UI 线程，但要配合数据快照一并改。

#### 副作用 / 不打架

- `_UpdateUI` 现在的跨线程行为不被此修改改变。
- `WM_SERVER_NOTIFY_SERVICE = WM_APP + 1`，`WM_WEASEL_TRAY_NOTIFY = WEASEL_IPC_LAST_COMMAND + 100`，值域不重叠。
- 改动量约 10 行；不动 `RimeWithWeasel`，不动 `WeaselTrayIcon`。

### H4 — Rime 热路径有未门控的 debug log

- 位置：`RimeWithWeasel/RimeWithWeasel.cpp:296-302`
- 严重度：high
- 维度：efficiency / doc-criteria
- 现状：`!handled && has_commit` 这条很常见的分支（标点上屏、空格选词等）每键 `WeaselDebugLog` 一行。
- 问题：reference 文档明确"`WEASEL_TRACE_KEY_EVENTS` 关闭时不输出每键 trace，避免默认日志过重"。当前实现违反该承诺。
- 建议：用 `ShouldTraceKeyEvents()` 门控（或本文件早已有的 trace 谓词），关闭时不进 log 路径。

### H5 — TSF `TraceKeyEvent` 调用点未先门控就构造字符串

- 位置：`WeaselTSF/KeyEventSink.cpp:81-89, 127-131, 199-202, 222-225, 238-241, 257-259`
- 严重度：high
- 维度：efficiency
- 现状：每个 Test/Key Down/Up 都进 `TraceKeyEvent`，函数内部检查环境变量并 early-return；但在调用点之前已经构造好 6-8 个 `std::wstring`、跑了多次 `std::to_wstring`，并且 `ShouldTraceKeyEvents()` 每次都查环境变量。
- 问题：这是按键热路径，doc 关心默认日志开销。
- 建议：把 `ShouldTraceKeyEvents()` 缓存成 `static bool g_trace_keys`（`std::call_once`），调用点改成 `if (g_trace_keys) TraceKeyEvent(...)`，关闭时不构造任何字符串。

### H6 — 旧 `OnKeyEvent` 缺 `FindSession` 守卫

- 位置：`WeaselIPCServer/WeaselServerImpl.cpp:248-259`
- 严重度：high
- 维度：correctness
- 现状：新 `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS` 先 `FindSession(lParam)` 守卫再 `ProcessKeyEvent`；旧 `WEASEL_IPC_PROCESS_KEY_EVENT` 直接走 `m_pRequestHandler->ProcessKeyEvent`，session miss 会让 `session_id == 0` 落到 Rime。
- 建议：旧分支也加同样的 `FindSession` 守卫，保持两个变体对外语义一致。

## 中优先（可同 PR 修；也可挂 follow-up）

### M1 — 重启等待循环硬编码在 `_tWinMain` 里 25 行

- 位置：`WeaselServer/WeaselServer.cpp:231-259`
- 维度：quality / reuse
- 现状：50 × 100ms 的 poll loop，`retry == 24` 时再次 nudge shutdown，超时 `NotifyService(RESTART_FAILURE)`，全部 inline。
- 建议：抽成 `weasel::WaitForExistingServiceShutdown()`，返回枚举 `{kStopped, kTimedOut}`，5s 预算、mid-wait re-shutdown、failure-bubble 都用命名常量；挂 TestWeaselIPC 单测。

### M2 — `ClearServiceManualExit()` 在 restart 路径冗余

- 位置：`WeaselServer/WeaselServer.cpp:229`
- 维度：quality
- 现状：合法清除点已有两处（`line 177-180` 的 `ShouldClearServiceManualExit` 守卫 + 服务端 `OnShutdownServer` `WeaselServerImpl.cpp:283-284`），这里再无条件清一次。
- 建议：删 `line 229`。"脚本/自动恢复路径不能误清"这条不变式由 `ShouldClearServiceManualExit` 单点保证。

### M3 — `MarkServiceManualExit` 在 TSF 和 server 各调一次

- 位置：`WeaselTSF/LanguageBar.cpp:535` + `WeaselIPCServer/WeaselServerImpl.cpp:282`
- 维度：quality
- 现状：TSF 在发 IPC 前写一次 flag，server 收到 EXIT 后又写一次。
- 建议：统一到 server 端写，TSF 只发命令。这样 `WeaselService::Stop` 等非 TSF 调用方也能复用同一段逻辑（与 H2 一起重构）。

### M4 — `OnMaintenanceResult` 等死壳

- 位置：`include/RimeWithWeasel.h:140-141`、`WeaselServer/WeaselTrayIcon.h:33-39`、`WeaselServer/WeaselTrayIcon.cpp:95-97`
- 维度：quality
- 现状：`OnMaintenanceResult`、`ShowMaintenanceResult`、`MaintenanceDeployResultStringId`、`MaintenanceDeployResultBalloonIcon` 都只是转发到对应的 `ServiceNotification*`。无生产调用方。
- 建议：删除这些 wrapper，单测改 call `ServiceNotificationStringId/...BalloonIcon`，减少双类型概念。

### M5 — `MAINTENANCE_RESULT_NONE` 与 `SERVICE_NOTIFICATION_NONE` 共享 wire 值 0

- 位置：`include/WeaselIPC.h:46-58`
- 维度：quality
- 现状：两组常量值范围（1-2 vs 3-6）手工分段，靠"我没让它碰撞"维持互斥；都通过 `NotifyService(DWORD)` 一条整数通道传。
- 建议：合并成单个 `WEASEL_IPC_NOTIFICATION` 枚举，或彻底分流到两个独立 IPC 命令。

### M6 — `Configurator` 同步用户数据失败不发 `EndMaintenance`

- 位置：`WeaselDeployer/Configurator.cpp:226-230`（早 return）、对照 `:155-158`（成功路径）
- 维度：correctness / doc-criteria
- 现状：`SyncUserData` 失败时 mutex 关掉就 return，从不调 `EndMaintenance`，server 保持 disabled 到下个 UI 事件才同步。`DictManagement` 仍传默认 `MAINTENANCE_RESULT_NONE`。
- 建议：所有维护路径走 RAII guard，`EndMaintenance(success ? DEPLOY_SUCCESS : DEPLOY_FAILURE)` 不漏发。

### M7 — `CustomInstall` 三个 `Sleep(500)`

- 位置：`WeaselSetup/WeaselSetup.cpp:134-143`
- 维度：efficiency
- 现状：`/q` → sleep → `/restart-manual` → sleep → 启动 deployer → sleep。
- 现状：server 端已经等 pipe-disconnect + single-instance mutex；客户端硬延迟是 belt-and-suspenders。
- 建议：删 sleeps 或在注释里写清楚为什么每一段都必要。

### M8 — `copy_file` 10 槽位 `.old.N` 全占时静默失败

- 位置：`WeaselSetup/imesetup.cpp:51-69`
- 维度：correctness
- 现状：循环 i=0..9，找到一个未占用的 `.old.N` 重命名旧 DLL 并 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`；若 10 个槽位都被先前安装失败留下的占满，循环走完没人成功，落到一次 `CopyFile` 重试，也会失败。原始 DLL 保留，但用户看到的是"安装失败"而不是"清掉 `.old.N` 再试"。
- 建议：循环结束没成功就 early-return + `MSG_NOT_SILENT_*` 显式提示用户清掉残留。

## 低 / nit（清理项）

- `WeaselTSF/WeaselTSF.cpp:47` 的 `marker=idea-keytrace-20260521` token 看起来像未清理的调试标记；doc 又明确反对 JetBrains/时间相关 hack。即便只是日志字符串也应该删。
- `TestWeaselIPC/TestWeaselIPC.cpp` 少一行 `BOOST_TEST(weasel::ShouldEatKeyEvent(TRUE, TRUE));` 把"process_key+commit"边界 case 锁住。
- `output/install.bat:36, 41-42` 三行 `rem regsvr32.exe ...` 是 IMM 时代残骸，删。
- `WeaselSetup/imesetup.cpp:486, 296` 的"IMM/.ime support removed; only uninstall TSF/.dll"是变更注释，应该改成描述当前行为或删掉。
- `include/WeaselUtility.h:42-44, 70-74` 的 `WeaselDebugLog` 每行调用都跑 `ExpandEnvironmentStringsW` + `fs::exists` + 可能的 `fs::create_directories`，再 `CreateFileW/Write/Close`。缓存路径一次，可选：缓存 handle。
- 三处重复 `GetModuleFileName` helper：`WeaselTSF/KeyEventSink.cpp:13-19`、`WeaselTSF/WeaselTSF.cpp:13-19`、`WeaselTSF/LanguageBar.cpp:108`。抽到 `include/WeaselUtility.h`。
- `WeaselSetup/imesetup.cpp:135-140, 323-328` 的 `register_text_service` 6 个 bool/path 参数，`register_ime` 现在等同于 do_register；rename 或打包成 struct。
- `WeaselIPC/WeaselClientImpl.cpp:30-46` 的 `_InitializeClientInfo` 手卷字符串而非 `std::filesystem`。
- `WeaselServer/WeaselTrayIcon.cpp:99-113` 的 `ShowServiceNotification` 没有 debounce，连续相同通知会连发 Shell_NotifyIcon。
- `WeaselIPC.h:93-98` + `WeaselServer/WeaselServer.cpp:286`：干净启动后 `/restart-manual` 也会显示"重启成功"（没有实际 restart 任何东西）。
- `WeaselServer/resource.h:42` (`_APS_NEXT_COMMAND_VALUE=40003`) 与 `include/resource.h:42` (`=40001`) 不一致；下次资源编辑器保存会出问题。
- `WeaselIPCServer/WeaselServerImpl.cpp:294-297` 的 `Sleep(1200)` 和 `WeaselServer/WeaselServerApp.cpp:73-76` 的 `Sleep(1200)` 应该共享同一个命名常量。
- `WeaselIPCServer/WeaselServerImpl.cpp:261-273` 的 0 返回值（"server not processed"）与 `MakeKeyEventResult(FALSE)` (= 0x01) 不冲突仅靠当前 bit 布局；加一行注释说明。
- `output/install.nsi`：卸载路径未清 HKCU manual-exit flag；下次安装可能继承上次手动退出态。
- `WeaselTrayIcon::Refresh` 里 `m_disabled = false` 那段在 `m_mode != DISABLED` 分支里，看起来不可达，需确认是死代码。
- `RimeUiNeedsUpdate` / `RimeTrayIconSignature` 当前覆盖足够；建议在 `RimeTrayIconSignature::From` 附近加注释列出"影响 tray 渲染的字段集"，让未来加字段时不漏更新。

## 复核记录

四个 review 维度各自结论：

| 维度 | 范围 | 通过标准对照结论 |
|---|---|---|
| 热路径 & 线程 | `WeaselTSF/*`、`WeaselIPC/WeaselClientImpl.*`、`WeaselIPC/PipeChannel.cpp`、`include/KeyEvent.h`、`include/PipeChannel.h` | 通过；TestKey 副作用、repeat-key、UI 单次重连、后台恢复隔离均符合。 |
| IPC 协议 & UI 去重 | `include/WeaselIPC.h`、`include/WeaselIPCData.h`、`WeaselIPCServer/*`、`RimeWithWeasel/*`、`include/RimeWithWeasel.h` | 通过；`InputPositionCache` reset 覆盖齐全，`RimeUiNeedsUpdate`/`RimeTrayIconSignature` 当前充分。 |
| 服务生命周期 & 托盘 | `WeaselServer/*`、`WeaselTSF/LanguageBar.cpp`、`WeaselDeployer/Configurator.cpp`、`output/start_service.bat` | 通过但有 H1/H2 真实风险；五种托盘通知映射齐全；重启等待同时等 pipe + mutex；shutdown reason 全部显式。 |
| 安装 / 打包 / 测试 | `WeaselSetup/*`、`output/install.*`、`output/start_service.bat`、`test/TestWeaselIPC/TestWeaselIPC.cpp`、`include/WeaselUtility.h`、`resource.h` | 通过；Run-key 用 `/startup`，脚本和 langbar 都用 `/restart-manual`；建议加一个 `(TRUE, TRUE)` 测试。 |

## 与 Codex review 交叉对照

读完 `docs/weasel-change-review-codex.md` 并逐项核对代码后，Codex 抓到了我这轮分工的 4 个 agent 漏掉的几个实质问题，也跟我的部分发现交叉。下面给出核对结果、严重度复评和处置建议。

### 新增的硬问题（应补入合并前修单）

#### C1 — `copy_file` 任何失败都先把旧 DLL 移走（升 Critical）

- 位置：`WeaselSetup/imesetup.cpp:40-74`
- 核对：`CopyFile(src, dest, FALSE)` 失败后**无条件**进入 `MoveFileEx(dest, .old.N, MOVEFILE_REPLACE_EXISTING)` + `MOVEFILE_DELAY_UNTIL_REBOOT` 流程，再做一次 `CopyFile`。不区分失败原因。
- 风险：src 缺失/不可读/权限/磁盘满任一原因都会先把现网 DLL 移走，第二次 CopyFile 仍失败 → 安装被破坏。
- 我之前的 M8 只覆盖"10 槽位全占"的子集，主问题漏。
- 修法：
  1. 先验证 `src` 可读、size 合理。
  2. 优先 `CopyFile` 到同目录 `dest + L".new"` 临时文件。
  3. 临时文件就绪后用 `MoveFileEx(temp, dest, MOVEFILE_REPLACE_EXISTING)`；只有这步失败（"目标被占用"）才走 `.old.N` + `MOVEFILE_DELAY_UNTIL_REBOOT`。
  4. 任意阶段失败一律保留原 `dest`。

#### C2 — ProcessKeyEvent 与 GetResponseData 之间存在恢复线程抢锁丢响应窗口

- 位置：`WeaselIPC/WeaselClientImpl.cpp:340-349`（GetResponseData）、`WeaselTSF/KeyEventSink.cpp:120-126`（ProcessKeyEvent → eaten → 后续 ASYNCDONTCARE edit session）
- 核对：两者都用 `client_mutex` 的 `try_to_lock`。ProcessKeyEvent 成功（eaten=true）后释放锁；TSF 调度 `_UpdateComposition`，最终 `DoEditSession` 在另一时间点调 `GetResponseData`。若此期间后台恢复线程拿到 `client_mutex` 并执行 `_Reconnect`，`GetResponseData` 的 try_lock 失败 → 直接返回 false。结果：宿主已被告知 `eaten=true`（不会再处理原始键），但 commit/preedit 没解析 → 吞字/候选不更新。
- 唯一兜底：恢复线程只在前一次 IPC 失败时被 kick 起来，正常路径上抢锁窗口偏窄。但 window 客观存在。
- 修法（两选一）：
  - 把当前线程 response buffer 的解析从 `client_mutex` 里拆出来（单独 buffer mutex）。
  - 或保证 `ProcessKeyEvent` 与对应 `GetResponseData` 在同一 UI 线程上连续完成，期间不允许恢复线程抢锁（在 ProcessKeyEvent 成功后设个 `pending_response = true` 标志，恢复线程检测到时短退）。

#### C3 — `FindSession` 用 `operator[]` 写入污染 session map

- 位置：`include/RimeWithWeasel.h:174-176`（`to_session_id`）；`RimeWithWeasel/RimeWithWeasel.cpp:159-166`（FindSession）；`WeaselIPCServer/WeaselServerImpl.cpp:264`（调用方）
- 核对：`to_session_id(ipc_id)` 实现是 `m_session_status_map[ipc_id].session_id`，`operator[]` 在 key 不存在时**插入默认 `SessionStatus`**。`FindSession` 内部调用它做探测，等于"每次探测一个不存在的 ipc_id，map 就长一条 stale 项"。
- 影响范围：`OnKeyEventWithStatus` 用 `FindSession` 做守卫；如果客户端传入的 session_id 已失效，会反复写入 stale 默认项。
- 修法：`FindSession` 改成 `auto it = m_session_status_map.find(ipc_id); if (it == end) return 0;` 后再用 `it->second.session_id` 调 `rime_api->find_session`。`to_session_id`/`get_session_status` 不能用在探测路径。

#### C4 — `EndMaintenance(result)` 被 `m_disabled` 门控，可能压制通知

- 位置：`RimeWithWeasel/RimeWithWeasel.cpp:496-508`
- 核对：`NotifyService(result)` 只在 `if (m_disabled)` 分支里调。`StartMaintenance` 通过 `Finalize()` 把 `m_disabled` 设为 true（正常路径下成立），但如果 `StartMaintenance` IPC 丢失、server 在部署后才启动、或者 server 因某些原因没进入 disabled 态，结果通知就被吃掉。
- 修法：拆开 "重新初始化 + UI 更新"（保留在 `m_disabled` 分支内）和 "通知部署结果"（无条件做）。让 deploy result balloon 独立于服务 disabled 状态。

#### C5 — `/q`、`/restart*` 只做一次 `TryConnect`，pipe 瞬时不可连即丢命令

- 位置：`WeaselServer/WeaselServer.cpp:203-263`
- 核对：进入控制命令分支前只 `client.TryConnect()` 一次。pipe 刚启动/退出/listener 间隙等瞬态不可连场景下，`/q` 走 "quit requested but no server" 直接退出；`/restart-manual` 则当作"无旧服务，直接启动"，可能与还在收尾的旧 server 撞 mutex。
- 修法：控制命令路径用**有界重试 + 兼看 single-instance mutex**，mutex 仍在但 pipe 不通时继续等或明确失败上报，不能静默丢。

#### C6 — 服务端 KEY_EVENT 仍在 `g_api_mutex` 内，慢任务会卡按键

- 位置：`WeaselIPCServer/WeaselServerImpl.cpp:188-198`（`PipeMessageNeedsOuterApiLock`）+ `OnKeyEventWithStatus`
- 核对：bypass list 只放了 `SHUTDOWN_SERVER` / `NOTIFY_SERVICE` / `UPDATE_INPUT_POS` / `TRAY_COMMAND`，KEY_EVENT 系列仍走 outer lock。Rime 卡、维护/初始化持锁、deploy 中等场景下，服务端的 KEY_EVENT 处理会被 outer lock 卡住 → 客户端 WriteFile/ReadFile 即便走 try_lock 取到了 `client_mutex`，本身的 pipe 同步 round-trip 仍要等服务端响应。
- 我之前判"非阻塞已达标"是偏乐观的。本分支客户端侧做了改进（try_lock、recovery 异步、buffer 清理），但**服务端 outer lock 这一段仍然是按键热路径上的真实阻塞源**。
- 修法（两选一）：
  - 服务端 key path 对 `g_api_mutex` 用 try-lock，忙时直接返回 `MakeKeyEventResult(FALSE)` + force-eat 走 commit 兜底。
  - 客户端走 overlapped I/O + 短超时 + cancel，超时即按"未处理"回宿主。

### 重新评估 / 修正我的发现

#### H6 → 反转：先修 `FindSession`，再讨论是否给旧 `OnKeyEvent` 加守卫

- 我此前 H6 建议给旧 `OnKeyEvent` 也加 `FindSession` 守卫"保持两个 IPC 变体一致"。但 Codex 揭示 `FindSession` 本身写副作用（C3）。
- 修订建议：**优先做 C3 的 `find()` 改造**，确认无副作用后再考虑给旧 `OnKeyEvent` 加守卫。在 C3 修好之前，给旧 `OnKeyEvent` 加守卫等于把 bug 复制一份到第二个调用点。

#### H3 补充：handler 生命周期

- 我之前 H3 只处理"NotifyService 跨线程触发 `Shell_NotifyIcon`"。Codex 指出同一段还有第二层问题：`m_pRequestHandler` 的读取、`OnEndSystemSession` 里 `Finalize()` 然后置空之间无统一 happens-before。
- 修订建议：在 H3 的 PostMessage 收口方案之上，再补 `m_pRequestHandler` 的生命周期保护——要么走同一把锁，要么 `shared_ptr` + 在 dispatcher 取 `weak_ptr.lock()`。

#### M3 补充：LanguageBar 的"先写 flag 再发 IPC"在 IPC 失败时不回滚

- 位置：`WeaselTSF/LanguageBar.cpp:533-538`
- 我之前 M3 只说"flag 在 TSF 和 server 各写一次，重复"。Codex 更精确：写在 IPC **之前**，IPC 失败时只显示退出失败提示，flag 不回滚。
- 修订建议：合并到 M3 的修法里 ——
  - 由 server（接收 EXIT IPC 一侧）单点写 flag；
  - TSF 不再预写；
  - 或者保留 TSF 预写，但 IPC 失败时必须 `ClearServiceManualExit()` 回滚。

#### M7 复核：500ms vs 1200ms 真实风险有限

- 我之前 M7 评级是"sleeps 冗余"。Codex 升级为竞态。
- 核对后维持原评级：`/restart-manual` 子进程会走 `WeaselServer.cpp:235-247` 的 50×100ms 等待循环检查 pipe + mutex 都清空，足够覆盖旧 server 的 1200ms 退出延迟。两段 500ms sleep 确实冗余、可读性差，但**不会让 deploy 真的撞上将退出的旧 server**。
- 修订建议：M7 维持"冗余可清理"评级，不升 Important。

#### Codex Critical 1 "按键热路径仍同步阻塞 IPC" 与我的 H4/H5 关系

- 我的 H4（Rime 热路径 debug log）和 H5（TraceKeyEvent 字符串无门控）讲的是"客户端 / Rime 自身在热路径上的开销"。
- Codex Critical 1 讲的是"服务端 outer lock 阻塞按键"（=本文 C6）。
- 两者**不是同一件事**，建议都修。H4/H5 减少正常路径每键开销；C6 解决慢任务并发时的按键卡顿。

### 双方都有的发现（共识）

| 主题 | 我编号 | Codex 编号 | 共识 |
|---|---|---|---|
| NotifyService 绕 lock + 跨线程触发 tray | H3 | Important 1 | 同；H3 PostMessage 方案适用，需补 handler 生命周期保护 |
| LanguageBar manual-exit flag 时序与重复写 | M3 | Important 8 | 同；以 server 为单点 |
| install 流程依赖固定 sleep | M7 | Important 6 | 同；评级我维持冗余、Codex 升竞态，按实际等待循环判断我维持原评 |
| copy_file 失败路径不安全 | M8（局部） | Critical 3 | 升 C1 |

### 我有但 Codex 没提的关键项

- H1（`IsScriptLikeRestartParent` 黑名单方向）
- H2（`WeaselService::Stop` 无条件写 flag）
- H4（Rime `ProcessKeyEvent` 每键 debug log）
- H5（`TraceKeyEvent` 调用点未先门控）
- M2（重复 `ClearServiceManualExit`）
- M4（`OnMaintenanceResult` 等死壳）
- M5（`MAINTENANCE_RESULT_NONE` 与 `SERVICE_NOTIFICATION_NONE` wire 值同 0）
- 各类 nits（idea-keytrace 标记、缺少 `ShouldEatKeyEvent(TRUE, TRUE)` 测试、`rem regsvr32` 残骸等）

这些仍按本文上面的 H/M/nit 处理。

### 合并修单建议顺序

按实际改动风险 + 用户感知顺序：

1. **先做 C1**：安装 DLL 复制安全（影响升级用户保留 IME），改动局部。
2. **再做 C6 + H4 + H5**：按键热路径阻塞与噪声，影响日常输入手感。
3. **再做 C2 + C3**：响应丢失窗口 + session map 污染，长跑稳定性。
4. **接着 H1 + H2 + M3（含 codex 失败回滚补强）**：手动退出/恢复语义。
5. **C4**：deploy 通知独立于 m_disabled。
6. **C5**：控制命令有界重试。
7. **H3（含 handler 生命周期）**：tray 跨线程收口。
8. **M2 / M4 / M5 / nit**：清理。

C1 / C2 / C3 / C6 在我的初版结论里漏了；以本节为准。
