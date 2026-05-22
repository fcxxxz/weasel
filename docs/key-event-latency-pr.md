# perf: 降低按键延迟并修复服务状态通知

## 背景

这批改动针对两个用户可感知的问题：

1. 输入法在服务端忙、pipe 不稳定、进程较多时出现按键卡顿，严重时表现为“算法掉了、切不回来，只能重启算法”。
2. 部署、退出算法服务、重启算法服务等状态变化缺少可靠的托盘提示，用户无法判断操作是否完成。

这里不把 Shift、CapsLock 等主动切换行为当成根因，也不改变它们的既有语义。本轮没有保留 JetBrains/IDEA/Rider 专用分支，也没有引入 120ms/15ms 之类按键延迟 hack。

## 主要改动

### 1. 按键 IPC 语义更明确

- 新增 `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS`。
- 服务端返回值同时表达：
  - 服务端是否实际处理过该按键。
  - Rime 是否吃掉该按键。
- 吃键语义补充 composing 前后状态：如果当前键让 Rime 从 composing 变成非 composing，即使没有 commit、`handled=false`，也视为输入法处理过，避免 Backspace 清掉最后一个编码字符时被宿主误处理。
- TSF 按键热路径改用 `ProcessKeyEvent(key, bool* eaten)`，避免把 IPC 失败误判成“服务端处理了但没吃键”。
- IPC 失败时当前按键路径快速返回，并触发后台恢复，不在当前输入线程同步重连。
- 旧 `WEASEL_IPC_PROCESS_KEY_EVENT` 也补了 session 守卫，和新入口语义保持一致。

### 2. 降低按键热路径阻塞

- `PipeChannel` 增加一次性连接/请求路径：
  - `TryConnect()` 只尝试一次连接，不进入 `WaitNamedPipe` 循环。
  - `TryTransact()` 跳过热路径 `FlushFileBuffers`。
  - 一次性连接失败或发送异常时清空写缓冲，避免带 body 的请求污染下一次发送。
- `ClientImpl` 增加内部互斥保护 `session_id`、pipe channel、输入位置缓存等共享状态。
- 按键、输入位置、候选、焦点、托盘等 UI 路径尽量使用 `try_lock` 和 `TryTransact`，恢复线程持锁时快速失败，不等待恢复锁。
- 服务端 key IPC 不再走阻塞 `g_api_mutex`，改为 try-lock；锁忙时立即返回未处理，让客户端把当前键交回宿主。
- `Echo()` 改走非阻塞 `_TrySendMessage(WEASEL_IPC_ECHO)`，避免 echo 失败路径自己造成阻塞重连。

### 3. 后台恢复不触碰 TSF apartment 对象

- `_Reconnect(update_tsf_status, wait_for_pipe)` 支持区分 UI 线程和后台恢复线程。
- 后台恢复固定不解析响应、不写 `_status`、不调用 `_UpdateLanguageBar`，避免跨线程访问 TSF compartment 或 language bar 对象。
- `AddRef/Release` 改为 `InterlockedIncrement/InterlockedDecrement`，恢复线程持有 TSF 对象引用时使用原子引用计数。
- 恢复重试计数从静态全局改为 TSF 实例成员，避免多个实例互相影响。

### 4. 避免按键响应丢失和 Backspace 状态滞后

- `ClientImpl::GetResponseData()` 不再抢 `client_mutex`；response buffer 是 `PipeChannel` 的 thread-local 数据，不应被后台恢复线程阻塞。
- `_ProcessKeyEvent()` 在 `ProcessKeyEvent` 成功后立即解析一次 status-only 响应，提前刷新 `_status.composing`。
- 本地 `_AbortComposition()` 会同步把 `_status.composing` 置为 false，并清理 TestKey cache，避免 TSF 本地状态残留影响下一次按键预测。
- 这样可以缩小 TestKey/Backspace 使用陈旧 composing 状态的窗口，降低“刚进入组合后 Backspace 删到宿主文本”或“首个编码字符 Backspace 删除不掉”的风险。

### 5. TestKey 单次处理缓存，重复 keydown 防护更保守

- 某些宿主可能只稳定触发 `OnTestKeyDown` / `OnTestKeyUp`，不保证后续一定按预期触发正式 `OnKeyDown` / `OnKeyUp`。
- TestKey 回调现在可以真正处理一次 Rime 按键，并缓存结果；如果随后正式 Key 回调到来，直接复用缓存结果，不再把同一个物理按键送给 Rime 第二次。
- TestKey cache 匹配时忽略 repeat count / previous-key-state 等 lParam 差异，适配部分宿主 TestKey 与 Key 回调参数不完全一致的情况。
- TestKeyDown 会清理上一轮残留的 KeyUp cache，TestKeyUp 会清理同一物理键的 KeyDown cache；即使宿主没有发送正式 KeyUp，也不会让下一次 Shift 切换命中旧缓存而跳过 Rime。
- 手动退出算法服务后，TestKey 和正式 Key 路径都透明放行，不再因为中文模式预测吃字母而导致退出后无法输入英文字母。
- `ActiveKeyDownGuard` 改为只判断是否应 suppress，不再在判断时自动记录。
- 正式 `OnKeyDown` 只有在 `_ProcessKeyEvent()` 成功且 `pfEaten=TRUE` 后才记录 active key，避免 disabled、unknown、pass-through key 的第二次回调被误吞。

### 6. 输入位置和 UI/托盘刷新去重

- 新增 `InputPositionCache`，相同压缩坐标不重复发送 `WEASEL_IPC_UPDATE_INPUT_POS`。
- 只有发送成功后才更新坐标缓存；失败后允许下一次重试。
- `Disconnect`、`StartSession`、`EndSession`、维护模式切换后 reset 缓存，避免新 session 漏发首个相同坐标。
- 托盘刷新增加 `RimeTrayIconSignature`，只有影响托盘状态的字段实际变化时才刷新。
- `_Respond` 和 `_UpdateUI` 复用 Rime status/context 快照，减少一次按键内重复 `get_status` / `get_context`。
- UI 更新增加 context/status diff，内容不变时跳过候选窗 `Hide/Update`。
- 抑制 `ascii_mode` / `!ascii_mode` 的候选窗内联图标提示；Shift 中英切换不再在输入位置弹出只含图标的状态窗，部署、方案、全半角等其他通知不受影响。

### 7. 服务通知和托盘提示补齐

- 部署完成/失败通过 `EndMaintenance(result)` 回传给 server，并显示托盘气泡。
- `EndMaintenance(result)` 的部署结果通知不再被 `m_disabled` 门控；即使维护状态没有成功进入，也不会吞掉部署结果提示。
- `SyncUserData()` 失败早退前会尝试 `EndMaintenance()`，避免 server 停留在 disabled/维护状态。
- 退出算法服务、正在重启算法服务、重启成功、重启失败都映射到托盘提示。
- `WEASEL_IPC_NOTIFY_SERVICE` 和 shutdown notification 不再从 pipe worker 线程直接操作托盘，改为 `PostMessage(WM_WEASEL_SERVICE_NOTIFY)` 收口到 server 消息线程。
- `OnServiceNotifyMessage` 内部持 `g_api_mutex` 读取 handler，避免和系统结束会话时的 finalize/置空并发。

### 8. 退出、重启、恢复语义修正

- 手动退出算法服务会写 manual-exit flag，并注销系统应用恢复。
- `/recover` 在 manual-exit flag 存在时不启动服务，避免用户手动退出后被自动恢复拉起。
- `/restart-manual` 和 `/startup` 会清除 manual-exit flag，用于明确的手动恢复。
- 旧 `/restart` 只在明确来自交互入口时兼容成手动重启；父进程判断从黑名单改为白名单。
- 新增非手动 `/stop` / `/stop-service`，安装器、卸载器和 stop 脚本使用 `/stop`，避免安装/卸载流程污染 manual-exit flag。
- SCM `WeaselService::Stop()` 改用 `WEASEL_IPC_SHUTDOWN_REASON_STOP`，不再写 manual-exit flag。
- LanguageBar 退出不再先写 manual-exit flag；成功退出由 server 端 EXIT 路径统一写。
- 控制命令路径对退出/重启增加有界连接重试，pipe 瞬时不可连时不再只试一次就静默丢命令。

### 9. 安装和 IPC 稳定性修复

- `copy_file()` 改为安全替换：
  - 先验证源文件存在。
  - 先复制到 `dest.new`。
  - 即时替换失败时只安排 delayed replace。
  - 任意失败都保留原目标文件，不再先移动旧 DLL。
- `FindSession()` 改为 `map.find()` 无副作用查询，不再通过 `operator[]` 插入 stale session。
- `_UpdateUI(0)` 不再创建 session 0，也不再对 session 0 调 Rime option。
- `PipeChannel::_Send()` 重连后使用新 pipe handle 重发，不再拿旧 handle 重发。
- `PipeChannelBase::_Connect()` 在 `SetNamedPipeHandleState` 失败时关闭已打开 pipe，避免句柄泄漏。
- `PipeServer::Listen()` 创建 per-connection worker 后显式 `detach()`，避免局部 `boost::thread` 析构时留下 joinable 线程风险。

### 10. 热路径日志和清理

- `ShouldTraceKeyEvents()` 提到 `WeaselUtility.h` 并缓存环境变量结果。
- TSF key trace 调用改为宏门控，`WEASEL_TRACE_KEY_EVENTS` 关闭时不再构造每键 trace 字符串。
- Rime `!handled && has_commit` 的每键 debug log 改为受 `WEASEL_TRACE_KEY_EVENTS` 门控。
- 移除临时 `idea-keytrace-20260521` 调试标记。
- 删除 `output/install.bat` 里 IMM 时代遗留的 `regsvr32` 注释。
- `imesetup.cpp` 的 IMM 注释改成当前 TSF-only 行为。
- `include/resource.h` 的 `_APS_NEXT_COMMAND_VALUE` 与 `WeaselServer/resource.h` 对齐。

### 11. 测试补充

`TestWeaselIPC /unit` 增加或更新覆盖：

- 按键 IPC 结果编码和 `ShouldEatKeyEvent(TRUE, TRUE)`。
- `ShouldEatKeyEvent` 覆盖 composing 被当前键结束时仍应吃键。
- TestKey 预测、单次处理 cache、lParam 容错匹配和 cache reset。
- TestKey cache 按物理键清理上下沿残留，覆盖没有正式 Key 回调消费缓存时的下一轮 Shift。
- 本地 composition abort 后 `_status.composing` 清理。
- 重复 keydown guard 的显式 `Remember()` / suppress 行为。
- 输入位置缓存 reset。
- 托盘签名、服务通知、维护结果映射。
- Rime status 快照转换。
- UI diff 判断。
- `ascii_mode` / `!ascii_mode` 内联提示抑制，确认不会误伤部署、方案、全半角通知。
- 服务端 key IPC 不走阻塞 outer lock。
- `/restart` 父进程白名单、`/stop`、manual-exit flag 语义。
- 缺失 server 时 `TryConnect()` 快速失败。

## 预期效果

- 服务端 busy、pipe 瞬时不可用、后台恢复进行中时，TSF 当前按键线程不再被同步重连、阻塞 outer lock 或恢复锁拖住。
- TestKey 和正式 Key 回调不会重复推进 Rime 状态，减少重复回调导致的双击、吞键和组合状态错乱。
- 宿主只发 TestKey、不发正式 KeyUp 时，下一次 Shift 仍会进入 Rime 处理，不会被上一轮缓存吞掉。
- Backspace/Delete/Enter 等编辑键在组合状态下更容易拿到及时的 composing 状态；部分宿主里“输入第一个编码字符后 Backspace 删除不掉”的场景应被修复。
- 手动退出算法服务后，选中小狼毫时普通字母应交回宿主输入，不再被 TSF 预测路径吞掉。
- 部署完成/失败、退出服务、重启服务、重启成功/失败都有托盘提示。
- Shift 切换中英只改变输入状态，不再额外弹出输入位置附近的中英状态图标。
- 手动退出后不会被自动恢复误拉起；安装/卸载/SCM stop 不会污染手动退出标记。
- 安装复制失败不会先破坏现有 DLL。

## 行为取舍和残余风险

- 客户端 pipe 事务已有连接时仍是同步 `WriteFile/ReadFile` round-trip。本轮通过服务端 key path try-lock 处理最直接的 `g_api_mutex` 阻塞源；如果 Rime 在真正处理某个 key 时长时间卡住，仍可能等待该次响应。彻底解决需要 overlapped I/O + timeout/cancel 的更大 IPC 改造。
- `_UpdateUICallback` / tray refresh 的数据流还没有整体迁到 UI 线程快照模式。本轮只修了 `NotifyService(DWORD)` 这条跨线程托盘气泡路径；`Refresh()` 涉及 `UIStyle`/`Status` 中的字符串引用，不能只靠 `PostMessage` 简单搬线程。
- `WeaselSetup::CustomInstall` 仍用后台线程串联 `/stop`、`/restart-manual`、`/deploy`，保留固定 sleep。要彻底消除 sleep，需要新增“等待新 server ready 后再 deploy”的专用命令或 IPC handshake。
- 真机 TSF smoke 仍然必要：首个编码字符 Backspace 删除、Backspace 删除 preedit、IDEA/Rider 不再双击、托盘退出后手动重启、部署完成提示都需要用打包产物验证。

## 验证

已执行：

```powershell
MSBuild .\test\TestWeaselIPC\TestWeaselIPC.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
.\x64\Release\TestWeaselIPC.exe /unit

MSBuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselSetup\WeaselSetup.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselDeployer\WeaselDeployer.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal
MSBuild .\WeaselDeployer\WeaselDeployer.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1 /v:minimal

git diff --check
```

结果：

- `TestWeaselIPC.exe /unit` 输出 `No errors detected.`。
- `WeaselServer` x64/Win32、`WeaselTSF` x64/Win32、`WeaselSetup` Win32、`WeaselDeployer` x64/Win32 均构建成功。
- 构建仍有既有类型转换 warning，未出现新的 error。
- `git diff --check` 无 whitespace error，只输出当前工作区 LF/CRLF 归一化提示。
