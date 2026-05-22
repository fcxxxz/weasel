# 按键卡顿优化 PR 草稿与 Review

## PR 标题建议

perf: 降低按键处理热路径阻塞和重复刷新

## 背景

用户反馈的问题不是主动切换输入法状态，而是在进程稍多、服务端忙或 IPC 不稳定时，输入法按键处理会出现明显卡顿，甚至表现为“算法掉了、切不回来，只能重启算法”。这类问题不适合靠新增日志等待复现，优先目标是直接减少按键热路径上的同步等待、重复 IPC、重复 UI/托盘刷新和不必要的全局锁竞争。

## 改动范围

1. TSF 按键热路径改为使用带状态返回的 `ProcessKeyEvent(key, bool* eaten)`，按键处理失败时不再在当前按键路径里阻塞重连，而是触发异步恢复。
2. IPC 增加 `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS`，结果同时表达“服务端是否处理过”和“按键是否被吃掉”，避免客户端把 IPC 失败误判成正常未吃键。
3. `PipeChannel` 增加非阻塞的一次性连接/请求路径，按键与输入位置更新避免反复等待管道、避免热路径 `FlushFileBuffers`。
4. 输入位置更新增加客户端侧去重，相同压缩坐标不再重复发送 IPC。
5. 托盘图标刷新增加签名缓存，只有输入法状态或图标相关配置实际变化时才刷新，减少每次按键后读取 `client_app`、创建线程或触发回调。
6. Rime 响应和 UI 更新复用同一次状态快照，减少一次按键内重复 `get_status`。
7. TSF 的 `OnTestKey*` / `OnKey*` 对同一方向、同一 `wParam/lParam` 的测试结果做缓存，包含 `eaten=false`，避免 Word 等应用重复测试同一个物理按键时反复打到 Rime。
8. 服务端 UI 更新增加 context/status diff，内容不变时跳过候选窗刷新。
9. 服务端 IPC 外层锁收窄：Rime/UI 状态相关命令仍受保护，shutdown、输入位置解码、慢托盘命令不再持有整个 API 外层锁；需要接触 Rime 状态的分支保留短锁。
10. `TestWeaselIPC` 增加单元入口，覆盖按键结果编码、托盘签名、Rime 状态快照、TSF 测试键缓存 helper、输入位置缓存 helper、UI diff 和 IPC 锁策略。

## 预期效果

按键路径不再因为服务端暂时不可用、管道繁忙、重复 test-key、重复坐标更新、重复托盘/UI 刷新而放大延迟。进程较多或服务端忙时，输入延迟应从“每次按键都可能等待服务端/管道/UI”收敛到“按键只做必要的 Rime 处理，恢复和慢刷新尽量移出热路径”。

恢复期间如果后台线程正在重建 IPC/session，候选选择、候选高亮、翻页、commit/clear、focus/tray 等 UI 入口可能短暂无响应；这是用少量操作失败换取 UI 线程不被慢 IPC 卡住。服务重启后 LanguageBar 也不会由后台线程立刻刷新，而是等下一次 UI 线程事件自然同步状态，避免非 UI 线程触碰 TSF apartment-bound 对象。

这个改动不试图改变 Shift、CapsLock 等主动切换语义，也不把用户误触组合键当成根因；它处理的是非主动切换场景下服务端/IPC/刷新路径拖慢或失联后恢复困难的问题。

## 兼容性与风险点

1. 旧的 `ProcessKeyEvent(key)` 入口保留，新增带状态入口供 TSF 热路径使用，降低调用方兼容风险。
2. 新 IPC 命令只在客户端和服务端同时更新后使用；失败时客户端返回“未处理”，并触发异步恢复。
3. 跳过 `FlushFileBuffers` 只用于按键/坐标这类热路径非阻塞请求，普通事务仍保留原同步语义。
4. UI/托盘去重依赖签名和 diff 的字段完整性，后续如果新增影响显示的字段，需要同步纳入签名或比较逻辑。
5. 服务端锁收窄需要保证所有接触 Rime API 或共享 UI 状态的路径仍被保护。
6. 异步恢复路径需要避免和按键热路径并发修改同一个客户端会话状态，也需要保住延迟重连线程里的 TSF 对象生命周期。

## 待 Review 清单

1. 按键 IPC 失败时，客户端是否能明确区分“服务端未处理”和“服务端处理但没吃键”。
2. `OnTestKey*` 缓存是否只复用同一个物理事件，不跨方向、不跨不同 `lParam`。
3. 输入位置去重是否只在发送成功后更新缓存，避免失败后吞掉后续重试。
4. 托盘签名和 UI diff 是否覆盖所有会影响可见状态的字段。
5. 服务端锁收窄后，Rime API、session map、UI 状态读写是否仍在锁内。
6. 异步恢复线程是否有并发保护，避免按键失败时创建大量恢复线程、并发改写 `m_client` 或释放后继续使用 `this`。
7. 测试是否覆盖新增协议、缓存 helper、diff 和锁策略；真实 TSF 事件序列、真实 IPC 响应体和恢复线程并发仍需要人工/集成验证。

## Review 结果

本地 review 和独立子代理 review 找到 1 个 Critical、2 个 Important：

1. Critical：`_RecoverServerAsync()` 的 detached 线程会和按键热路径并发访问同一个 `m_client`；`_EnsureServerConnected()` 里达到重试阈值后还会再创建一个捕获裸 `this` 的延迟重连线程，存在数据竞争和生命周期风险。
2. Important：输入位置缓存只在发送成功后更新是正确的，但新 session、断开、维护模式切换后没有清空缓存；重连后相同坐标可能不会重新发给服务端。
3. Important：`PipeChannelBase::_EnsureOnce()` 在 `CreateFile` 成功但 `SetNamedPipeHandleState` 失败时没有关闭已打开句柄，后续可能把未设置 message read mode 的句柄当成已连接。

处理结果：

1. `ClientImpl` 增加内部递归互斥，所有 `m_client` 公共操作串行化；按键处理和输入位置更新使用 `try_lock`，恢复线程占用客户端时热路径直接失败并触发异步恢复，不阻塞等待锁。
2. `_RecoverServerAsync()` 调用 `_EnsureServerConnected(false)`，后台恢复线程自己完成启动服务和延迟重连，不再额外派生裸 `this` 的内层线程；同步路径保留异步重连时增加 `AddRef/Release`。
3. `WeaselTSF::AddRef/Release` 改成 `InterlockedIncrement/InterlockedDecrement`，并用本次原子递减返回值决定是否 `delete this`，适配恢复线程跨线程持有引用。
4. 输入位置缓存抽成 `InputPositionCache`，并在 `Disconnect`、`StartSession`、`EndSession`、维护模式切换时 reset；新增单元测试固定 reset 后同坐标必须重新发送。
5. `_EnsureOnce()` 只在管道句柄成功切换到 message read mode 后写回线程本地句柄；失败时关闭临时句柄。

## 第二轮 Review 发现（基于已完成修复后的代码）

在前一轮 Critical/Important 都已处理的前提下，又扫了一遍 committed + 未提交改动，发现以下问题：

### Critical：恢复线程上调用 TSF apartment-bound API，且 `_status` 有 data race

`WeaselTSF/WeaselTSF.cpp` 中 `_Reconnect()` 末尾会调 `_UpdateLanguageBar(_status)`，而 `_UpdateLanguageBar`（`WeaselTSF/LanguageBar.cpp:403`）会调 `_GetCompartmentDWORD` / `_SetCompartmentDWORD` 以及 `_pLangBarButton->UpdateWeaselStatus`。这些都是 STA apartment 绑定的 TSF 对象。

PR 前 `_Reconnect()` 只从 `_ProcessKeyEvent` / `OnSetThreadFocus` 调用，全在 TSF UI 线程上；PR 后 `_RecoverServerAsync()` 起 detached background thread → `_EnsureServerConnected(false)` → `_Reconnect()`，把 TSF API 搬到了非 UI 线程，属于 undefined behavior。

同时 `_status` 既被恢复线程上的 parser（`ResponseParser` 持 `&_status`）写入，又被 UI 线程的 `KeyEventSink.cpp:48`、`EditSession.cpp:32-35` 读取，没有同步——data race。

候选修法：

1. 恢复线程只跑 IPC 部分（`Echo` / `Disconnect` / `Connect` / `StartSession`），状态解析和 `_UpdateLanguageBar` 通过 `PostMessage` 回 TSF UI 线程做。
2. 或者恢复线程里跳过 `_UpdateLanguageBar`，让下一个用户事件自然带出最新状态（接受 1 个 key 的状态延迟）。
3. 给 `_status` 加短锁，所有 TSF 状态写都 marshal 回 UI 线程。

### Important：UI 线程在非按键路径上仍会等恢复线程

`WeaselIPC/WeaselClientImpl.cpp` 里只有 `ProcessKeyEvent(KeyEvent, bool*)` 和 `UpdateInputPosition` 用 `try_lock`，其余都用 `std::lock_guard<std::recursive_mutex>`：

- `FocusIn` / `FocusOut`
- `CommitComposition` / `ClearComposition`
- `SelectCandidateOnCurrentPage` / `HighlightCandidateOnCurrentPage` / `ChangePage`
- `StartSession` / `EndSession` / `Echo`

当恢复线程在 `_Reconnect()` 内部阻塞（`WaitNamedPipe` 500ms × N、`ShellExecute` + `sleep(500ms)`、`StartSession` 全程持锁）时，UI 线程上这些入口会同样阻塞——焦点切换、点选候选、commit 上屏都会卡。

要么扩大 try_lock 覆盖到这些方法，要么显式声明短时阻塞可接受并加一个上限（如 `try_lock_for(50ms)`）。

### Important：`_keyEventTestCache` 在 TestKey 交错时可能让 Rime 重复收到同一个 key

`include/KeyEvent.h` 的 `KeyEventTestCache` 只存一项。若发生 `OnTestKeyDown(A)` → `OnTestKeyDown(B)` 这种交错（B 覆盖 A），随后 `OnKeyDown(A)` `Matches` 不命中 → 走 `_ProcessKeyEvent(A)` 第二次。

这是把原来 `_fTestKeyDownPending`（单态布尔）的行为继承下来，并不是回归。但 review doc 第 2 项点名"OnTestKey\* 缓存是否只复用同一个物理事件"——当前结论是"不会错配，但不防止重复打 Rime"。

如果确认 TSF 不会交错 TestKeyDown，需要在 `KeyEventTestCache` 上加注释说明前提；否则扩成 `(wParam,lParam)→eaten` 的小 map。

### Minor

1. `include/RimeWithWeasel.h::RimeUiNeedsUpdate` 按值收 `Context`/`Status`，每次按键深拷贝 4 份（preedit / cinfo / labels / comments）。改成 `const&` 更合适，但前提是把 `WeaselIPCData.h` 里 `Context::operator==/!=` 和 `Status::operator==`（目前签名 `const Status status` 且非 const 成员函数）补成 const。
2. `WeaselClientImpl.cpp::StartSession` 里 `input_position_cache.Reset()` 调了两次（`_SendMessage` 前后）。`_SendMessage` 不会回调进 cache，只 reset 一次就够，多写一次会让读者怀疑漏了什么。
3. `RimeWithWeasel.cpp::_UpdateUI` 把 `Status& weasel_status = m_ui->status();` 改成 `Status weasel_status = m_ui->status();`（值拷贝），并新增了 `RimeUiNeedsUpdate` 短路：同 ctx/status 不再 `m_ui->Hide()` + `m_ui->Update()`。如果有依赖每次 Hide 重绘的路径需要手动 smoke。
4. `WeaselTSF.cpp` 的 `static std::atomic_uint retry` 跨实例共享，多 TSF 实例下失败计数会互相累加。pre-existing，不阻塞此 PR，但顺手改成成员更安全。
5. `PipeChannel.cpp::_FinalizePipe` 对 client-side handle 调 `DisconnectNamedPipe` 一定失败（仅 server 端合法）。无害——紧接着 `CloseHandle`——但本 PR 在 `_EnsureOnce` 失败路径上新增了一次这种用法，留作日后清理。

### 不属于本 PR 范围的观察

`_EnsureServerConnected` 的同步 restart_server 分支只在 `reconnect_after_launch_async=false`（恢复线程）时走。若 Critical 按"恢复线程上跳过 TSF UI 更新"修，restart_server 的同步路径需要相应避开 `_UpdateLanguageBar`。

## 第二轮 Review 处理结果

1. `_Reconnect` 增加 `update_tsf_status` 参数：后台恢复线程只做 IPC 重连和 session 重建，不再解析响应写 `_status`，也不再调用 `_UpdateLanguageBar` / TSF compartment / language bar API。
2. `_EnsureServerConnected(false)` 用于后台恢复路径；延迟启动服务后的后台 `_Reconnect` 固定走 `update_tsf_status=false`，避免非 UI 线程触碰 TSF apartment-bound 对象。
3. `ClientImpl` 中按键热路径之外的焦点、候选选择、翻页、commit/clear、旧 `ProcessKeyEvent`、`Echo` 改为 `try_lock`，恢复线程持有客户端锁时这些 UI 入口直接失败返回，不等待恢复锁。
4. `KeyEventTestCache` 从单槽改为 4 槽固定缓存，并新增 `RemoveMatched()`：交错 `OnTestKeyDown(A)` / `OnTestKeyDown(B)` 后，A/B 都可分别命中，不会错配，也减少重复打到 Rime。
5. `RimeUiNeedsUpdate` 改为按 `const&` 比较；同步把 `WeaselIPCData` 里的相关比较操作补成 `const`，避免每次按键做多份 context/status 深拷贝。
6. `StartSession` 去掉重复 `input_position_cache.Reset()`；恢复重试计数从静态全局改成 TSF 实例成员，并在连接恢复成功时清零。

## 第三轮自查与补充处理

第二轮处理后又复查了 `StartSession`、`EndSession`、`GetResponseData`、`TrayCommand` 等非按键路径，发现还有一个更深的阻塞来源：`Echo()` 虽然已经改为 `try_lock`，但拿到锁后仍走阻塞 `Transact`；同时 `_EnsureServerConnected()` 在 echo 失败后会进入 `_Reconnect()`，旧的重连序列会拆成 `Disconnect` / `Connect` / `StartSession` 三个公开调用，其中 `Disconnect()` 仍是阻塞锁，`Connect()` 也可能等待管道。

补充处理：

1. `PipeChannel` 增加 `TryConnect()`，复用 `_EnsureOnce()` 做一次性连接；`TryTransact()` 在一次性连接失败时主动清空写缓冲，避免 `StartSession` 这种带 body 的请求失败后把旧 body 留到下一次发送。
2. `ClientImpl` 增加 `Reconnect(launcher, wait_for_pipe)`，用一次 `try_lock` 包住断开、清 session、连接和重建 session；UI 线程传 `wait_for_pipe=false`，只尝试一次管道连接，后台恢复传 `true`，允许在后台等待忙碌管道。
3. `_Reconnect(update_tsf_status=true)` 改为 UI 侧非阻塞重连尝试；后台恢复仍然不解析响应、不写 `_status`、不调用 `_UpdateLanguageBar`。
4. `Echo()` 改为 `_TrySendMessage(WEASEL_IPC_ECHO)`，不再通过失败路径触发阻塞重连。
5. 旧 `ProcessKeyEvent(key)`、commit/clear、候选选择/高亮/翻页、焦点进出、托盘命令、`StartSession`、`EndSession` 改为 `TryTransact` 路径；恢复线程持锁时先快速返回，拿到锁后也尽量不走 `WaitNamedPipe` 和 `FlushFileBuffers`。
6. `StartMaintenance` / `EndMaintenance` / `ShutdownServer` 仍保留同步发送语义，因为这些不是 TSF 按键/UI 热路径，并且部署/退出流程需要明确完成。

## 第四轮 Review 发现（在第三轮补丁基础上再扫一遍）

第二轮 + 第三轮处理已经把 Critical、所有 Important 都修了；这一轮没有发现新的 Critical 或 Important，只剩下 Minor / design / doc 级别的问题：

### Minor：恢复成功后 `_keyEventTestCache` 没清空，可能返回陈旧 `eaten`

`WeaselTSF::_Reconnect(...)` 重连成功路径上没有 `_keyEventTestCache.Clear()`。如果服务挂掉前 cache 里残留了"上一次 successful processed"的 entry（例如 OnTestKeyDown 已 Store 但 OnKeyDown 还没消费就断连），重启后 Rime 状态可能变（例如 ascii_mode 切换），下一次 `OnTestKeyDown(same wParam, lParam)` 命中残留 entry 直接 `return cache.Eaten()`，不再打到 Rime。

实际残留窗口很小（一对 TestKey/KeyDown 之间），但严格起见值得在 `_Reconnect` 成功分支末尾加一行 `_keyEventTestCache.Clear()`。

### Minor：`KeyEventTestCache::Matches` 是 const 但写 `mutable matched_entry`，有调用顺序耦合

`Matches(...) const` 写入 `mutable int matched_entry`，`Eaten()` / `RemoveMatched()` 都依赖上一次 `Matches` 留下的下标。隐含约束："`Matches` 之后必须紧接 `Eaten` / `RemoveMatched`，中间不允许 `Store` / `Clear`"。`KeyEventSink.cpp` 当前用法满足这个约束，但接口本身没强制，后续改动容易踩。

建议（择一，非阻塞）：
- `Matches` 返回 `std::optional<size_t>`，调用方持有 index 后再问 `Eaten/Remove`；
- 或者合并成 `bool TakeEaten(bool is_key_up, WPARAM, LPARAM, BOOL& out_eaten)`，一步完成查找 + 取值 + 移除；TestKey 路径单独走只查询不移除的 `PeekEaten`。

### Minor：UI 线程和恢复线程并发 `++_serverRecoveryRetry`，可能更早触发 `restart_server`

`OnSetThreadFocus` → `_EnsureServerConnected(true)`（UI 线程）和 `_RecoverServerAsync` → `_EnsureServerConnected(false)`（恢复线程）都会 `++_serverRecoveryRetry`。两条线并发 ++ 会让 retry 比预期更早到 6、提前触发 `restart_server`。`restart_server` 内有 `WeaselDeployerExclusiveMutex` 防重复 ShellExecute，所以不致命，但破坏了"连续失败 6 次才重启"的语义。

如要保留语义，可以把 retry++ 限制在恢复线程独占：UI 侧的 `_EnsureServerConnected(true)` 只做 Echo + 非阻塞 Reconnect，不计数；计数由 `_RecoverServerAsync` 持有的恢复线程独自完成。

### Minor / nit：`Connect()` 没加锁，与其他公共方法不一致

`ClientImpl::Connect(...)` 直接 `return channel.Connect();`，没 `lock_guard`。实际只在 TSF 初始化路径调，没有并发问题；但与 `Disconnect` / `Reconnect` / `ShutdownServer` 都加锁的约定不一致。一致性 nit。

### Doc：恢复期间候选窗操作也会"无响应"

恢复线程持锁的几百 ms 到数秒内，`SelectCandidateOnCurrentPage` / `HighlightCandidateOnCurrentPage` / `ChangePage` / `CommitComposition` 等 UI 入口都会 try_lock 失败立即返回 false。用户感知 = "字符以英文落地 + 候选窗点了没反应、翻页失败、commit 不上屏"。

这是用"切英文 + 候选无响应"换"UI 线程不卡"的预期 trade-off；建议在 PR doc 的"预期效果"里写明，避免后续被当成新 bug 反馈。

### Doc：`restart_server` 永远走 `_Reconnect(false)`，重启后 LanguageBar 不会立刻刷

`restart_server` lambda 内硬编码 `_Reconnect(false)`，无论从 UI 线程派生的子线程还是恢复线程触发都不会更新 `_status` / `_UpdateLanguageBar`。这是 Critical 修复的延伸（restart 永远在非 UI 线程跑）。结果：服务重启后 LanguageBar 状态要等下一次按键触发的 `_UpdateComposition` 才更新一次。

属于已知 trade-off；如果觉得 LanguageBar 滞后让人困惑，可以在恢复完成后用 `PostMessage` 把一个 refresh 请求 marshal 回 TSF UI 线程做。

### Nit：`Reconnect` 失败路径已 `channel.Disconnect()`，但语义没在头注释里写明

`ClientImpl::Reconnect()` 在 `channel.Disconnect()` 之后才尝试 `Connect/TryConnect`，失败返回 false 时 channel 保持 disconnected——这是预期的（下次按键 / `OnSetThreadFocus` 自然触发新一轮），但 `WeaselIPC.h::Client::Reconnect` 头注释没说"失败后客户端不可用直到下次主动触发"。加一行头注释可读性更好。

## 第四轮 Review 处理结果

1. 已处理 `_keyEventTestCache` 的陈旧结果风险，但没有在后台恢复线程直接清 cache。实现为 `KeyEventTestCacheReset` 原子 pending 标记：`_Reconnect` 成功后只 `Mark()`，下一次 `OnTestKeyDown` / `OnKeyDown` / `OnTestKeyUp` / `OnKeyUp` 进入 TSF UI 线程时再 `Apply()` 清空 cache，避免把 UI 线程状态带回后台恢复线程。
2. 新增单元测试覆盖 reset helper：标记 pending 后会清空已有 test-key cache，重复 apply 不会误报。
3. 明确保留恢复期间 UI 入口 `try_lock` 快速失败的 trade-off：恢复线程持锁期间，候选选择、候选高亮、翻页、commit/clear、focus/tray 等操作可能短暂无响应；这是用少量操作丢失换取 UI 线程不被慢 IPC 卡住。
4. 明确 `restart_server` 后 LanguageBar 不会由后台线程立刻刷新；后台恢复只重建 IPC/session，语言栏状态等下一次 UI 线程事件自然刷新，避免再次触碰 TSF apartment-bound 对象。

其余 nit（`Matches` 接口形态、retry 计数语义、`Connect` 一致性、`Reconnect` 注释）暂不处理，不阻塞当前卡顿优化 PR。

## 本轮状态

当前只准备本地 Markdown、修复 review 发现的问题并复验；不推送，不创建 PR。第二轮 review 发现的 Critical 已处理；第三轮补掉了 `Echo` / 重连序列 / 非按键 UI IPC 的剩余阻塞风险；第四轮最接近真实行为问题的 test-key cache 陈旧风险已处理。剩余主要风险是真实 TSF apartment、真实 IPC 慢响应和候选窗 smoke 仍需要人工验证。

## 验证命令

第四轮补充处理后已重新复验：

```powershell
MSBuild test\TestWeaselIPC\TestWeaselIPC.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1
.\x64\Release\TestWeaselIPC.exe /unit
MSBuild WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1
MSBuild WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir='F:\IdeaProjects\参考项目\weasel\' /m:1
git diff --check
```

其中 `InputPositionCache` 的测试先以缺少 helper 编译失败，再实现 helper 和 reset 逻辑后通过。
