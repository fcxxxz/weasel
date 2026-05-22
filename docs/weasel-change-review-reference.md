# Weasel 本地改动 Review 参照

本文档用于 review 当前本地未提交改动。它不是 PR 描述正文，而是给 reviewer 的检查路线图：说明每组改动的目标、关键设计、需要重点质疑的地方、以及建议验证方式。

## Review 范围

当前 tracked diff 涉及这些主要模块：

- TSF 按键路径：`WeaselTSF/KeyEventSink.cpp`、`WeaselTSF/WeaselTSF.cpp`、`WeaselTSF/WeaselTSF.h`、`include/KeyEvent.h`
- IPC 客户端和管道：`WeaselIPC/WeaselClientImpl.*`、`WeaselIPC/PipeChannel.cpp`、`include/PipeChannel.h`、`include/WeaselIPC.h`
- IPC 服务端：`WeaselIPCServer/WeaselServerImpl.*`
- Rime/UI 桥接：`RimeWithWeasel/RimeWithWeasel.cpp`、`include/RimeWithWeasel.h`、`include/WeaselIPCData.h`
- 服务进程和托盘：`WeaselServer/WeaselServer.cpp`、`WeaselServer/WeaselServerApp.*`、`WeaselServer/WeaselTrayIcon.*`、`WeaselServer/WeaselService.cpp`
- 部署和安装：`WeaselDeployer/Configurator.cpp`、`WeaselSetup/*`、`output/install.*`、`output/start_service.bat`
- 测试：`test/TestWeaselIPC/TestWeaselIPC.cpp`
- 文案资源：`WeaselServer/WeaselServer.rc`、`WeaselServer/resource.h`、`include/resource.h`

未跟踪的 `x64/`、`test/TestWeaselIPC/x64/` 属于构建产物，不应纳入代码 review。未跟踪的 `output/data/` 方案、lua、皮肤预览等数据是否纳入提交需要单独确认。

## 改动目标

这批改动主要解决三类问题：

1. 按键卡顿：服务端忙、pipe 不可用、恢复过程或重复 UI 刷新不应该阻塞 TSF 当前按键线程。
2. 服务状态可见性：部署完成/失败、退出算法服务、重启算法服务、重启成功/失败需要有托盘提示。
3. 服务退出/恢复语义：用户手动退出算法服务后，自动恢复不能立刻把服务重新拉起；手动重启应能清除退出标记并启动服务。

后续追加的回归修复：

- `OnTestKeyDown` 预测逻辑不能无条件吃 Backspace。无组合状态下 Backspace/Delete/Enter/方向键等编辑键应交给宿主；正在组合时再交给 Rime。
- 不保留 JetBrains/IDEA/Rider 专用分支，也不使用 120ms 这类输入延迟 hack。

## 建议 Review 顺序

1. 先读 `include/WeaselIPC.h`
   - 确认新增 IPC 命令、维护结果、服务通知、shutdown reason、命令行参数语义是否清晰。
   - 重点看 `/restart`、`/restart-manual`、`/recover`、`/startup`、`/q` 的分工。

2. 再读 IPC 热路径
   - `include/PipeChannel.h`
   - `WeaselIPC/PipeChannel.cpp`
   - `WeaselIPC/WeaselClientImpl.cpp`
   - 重点确认按键、输入位置、焦点、候选、托盘等 UI 热路径没有引入新的同步等待。

3. 再读 TSF 按键路径
   - `include/KeyEvent.h`
   - `WeaselTSF/KeyEventSink.cpp`
   - `WeaselTSF/WeaselTSF.cpp`
   - 重点确认 TestKey 无副作用、正式 KeyDown 才进 Rime、Backspace 预测正确、重复 keydown 防护不影响长按和正常快捷键。

4. 再读服务端和托盘
   - `WeaselIPCServer/WeaselServerImpl.cpp`
   - `WeaselServer/WeaselServer.cpp`
   - `WeaselServer/WeaselServerApp.cpp`
   - `WeaselServer/WeaselTrayIcon.*`
   - 重点确认退出/重启通知能发出，且不会在通知刚出现时立刻移除托盘图标。

5. 最后读 Rime/UI 刷新和部署安装
   - `RimeWithWeasel/RimeWithWeasel.cpp`
   - `include/RimeWithWeasel.h`
   - `WeaselDeployer/Configurator.cpp`
   - `WeaselSetup/*`
   - 重点确认部署结果能传回 server，UI/托盘 diff 不会漏掉可见状态变化。

## 重点 Review 清单

### 1. 按键 IPC 语义

需要确认：

- `WEASEL_IPC_PROCESS_KEY_EVENT_WITH_STATUS` 的返回值同时表达“服务端处理过”和“Rime 吃键”。
- IPC 失败时客户端返回未处理，不应被误解成“服务端处理了但没吃键”。
- Rime 产生 commit 时，即使 `process_key` 返回未处理，也应吃掉当前键，避免宿主再插入原始字符。

重点文件：

- `include/WeaselIPC.h`
- `WeaselIPC/WeaselClientImpl.cpp`
- `WeaselIPCServer/WeaselServerImpl.cpp`
- `RimeWithWeasel/RimeWithWeasel.cpp`

### 2. TSF TestKey 无副作用

需要确认：

- `OnTestKeyDown` / `OnTestKeyUp` 不调用 Rime `process_key`，不更新 composition。
- TestKey 只做预测，正式 `OnKeyDown` / `OnKeyUp` 才改变 Rime 状态。
- `ShouldEatTestKeyEvent` 的规则符合输入法语义：
  - 非已知键不吃。
  - CapsLock 模拟流程可吃。
  - Shift/CapsLock/Ctrl 等模式切换键可吃。
  - Ctrl/Alt 快捷键在无组合时不吃。
  - 正在组合时编辑键交给 Rime。
  - 无组合时 Backspace/Delete/Enter/方向键不吃。
  - 非 ascii 模式下普通文本键吃。

重点文件：

- `include/KeyEvent.h`
- `WeaselTSF/KeyEventSink.cpp`
- `test/TestWeaselIPC/TestWeaselIPC.cpp`

### 3. 重复按键防护

需要确认：

- `ActiveKeyDownGuard` 只抑制同一物理 keydown 在 keyup 前重复到达的情况。
- `prevKeyState=1` 的系统 repeat 不应被误杀。
- `Release` 在 keyup 时清理状态，焦点丢失时 reset。
- 没有应用名白名单，没有 JetBrains 特判，没有固定时间延迟。

重点文件：

- `include/KeyEvent.h`
- `WeaselTSF/KeyEventSink.cpp`

### 4. IPC 非阻塞和恢复

需要确认：

- UI/按键热路径使用 `try_lock` 和 `TryTransact`，恢复线程持锁时快速失败，不等待慢恢复。
- `TryTransact` 连接失败时清空发送 buffer，避免带 body 的请求污染下一次请求。
- `Reconnect(..., wait_for_pipe=false)` 在 UI 线程上只做一次尝试；后台恢复才允许等待 pipe。
- 后台恢复线程不解析响应写 `_status`，不调用 `_UpdateLanguageBar`，不触碰 TSF apartment-bound 对象。
- `AddRef/Release` 的原子化足够支撑后台恢复线程持有 TSF 对象。

重点文件：

- `WeaselIPC/WeaselClientImpl.cpp`
- `WeaselIPC/PipeChannel.cpp`
- `include/PipeChannel.h`
- `WeaselTSF/WeaselTSF.cpp`

### 5. 输入位置和 UI/托盘去重

需要确认：

- `InputPositionCache` 只在发送成功后记录坐标。
- Disconnect、StartSession、EndSession、维护模式切换后 reset，避免新 session 漏发首个相同坐标。
- `RimeUiNeedsUpdate` 比较字段足够覆盖候选窗可见状态。
- `RimeTrayIconSignature` 包含所有影响托盘图标状态的字段；后续新增字段时需要同步更新签名。

重点文件：

- `WeaselIPC/WeaselClientImpl.h`
- `WeaselIPC/WeaselClientImpl.cpp`
- `include/RimeWithWeasel.h`
- `RimeWithWeasel/RimeWithWeasel.cpp`
- `include/WeaselIPCData.h`

### 6. 退出、重启、恢复语义

需要确认：

- 手动退出会写 manual-exit flag，并注销系统应用恢复。
- `/recover` 在 manual-exit flag 存在时不会启动服务。
- `/restart-manual` 会清除 manual-exit flag 并启动服务。
- 旧 `/restart` 只在明确来自交互父进程时兼容成手动重启；脚本/自动恢复场景不能误清 flag。
- 重启旧服务时同时等待 pipe 断开和单实例 mutex 释放。
- 退出服务和重启服务的 shutdown reason 传递明确，不再依赖默认值。

重点文件：

- `include/WeaselIPC.h`
- `WeaselServer/WeaselServer.cpp`
- `WeaselIPCServer/WeaselServerImpl.cpp`
- `WeaselServer/WeaselService.cpp`
- `WeaselTSF/LanguageBar.cpp`
- `output/start_service.bat`

### 7. 托盘提示

需要确认：

- 部署完成/失败通过 `EndMaintenance(result)` 传到 server，再转成托盘气泡。
- 退出算法服务、正在重启算法服务、重启成功、重启失败都映射到正确文案和图标类型。
- 服务即将退出时延迟停止，给托盘气泡留出显示时间。
- 无活跃输入会话时仍能看到部署完成/失败提示。

重点文件：

- `WeaselDeployer/Configurator.cpp`
- `RimeWithWeasel/RimeWithWeasel.cpp`
- `WeaselServer/WeaselTrayIcon.*`
- `WeaselServer/WeaselServerApp.cpp`
- `WeaselServer/WeaselServer.rc`

### 8. 安装和打包脚本

需要确认：

- Run key 使用 `WeaselServer.exe /startup`。
- `start_service.bat` 使用 `/restart-manual`，用于手动重启。
- 安装器复制 TSF DLL 失败时，重命名旧文件和安排重启删除不会破坏正常升级。
- `WeaselSetup` 里手动重启服务参数和新语义一致。

重点文件：

- `WeaselSetup/WeaselSetup.cpp`
- `WeaselSetup/imesetup.cpp`
- `output/install.bat`
- `output/install.nsi`
- `output/start_service.bat`

## 已知取舍

- 恢复线程持锁期间，候选选择、候选高亮、翻页、commit/clear、focus/tray 等 UI 入口可能短暂失败返回。这是用少量操作失败换取 UI/按键线程不被慢 IPC 卡住。
- 后台恢复成功后不会立刻刷新 LanguageBar；状态会等下一次 UI 线程事件自然同步，避免后台线程触碰 TSF apartment-bound 对象。
- `WEASEL_TRACE_KEY_EVENTS` 关闭时不会输出每个按键的详细 trace，避免默认日志过重；需要排查真实宿主按键序列时再开启。
- 当前单元测试覆盖 helper、协议和策略，不替代真实 TSF 宿主 smoke。

## 建议 Smoke 验证

建议在打包安装后按下面顺序验证：

1. 普通编辑器输入中文、英文、数字、标点。
2. 无组合状态按 Backspace，应删除宿主文本。
3. 有组合状态按 Backspace，应删除 Rime preedit。
4. Ctrl/Alt 快捷键在无组合状态不应被输入法吞掉。
5. Shift、CapsLock 仍保持原有切换语义。
6. JetBrains IDEA/Rider 中输入不应双击成两个字符。
7. 托盘菜单“退出算法服务”：服务退出，不应自动恢复。
8. 退出后语言栏/脚本“重启算法服务”：服务能重新启动，并显示重启成功提示。
9. 重新部署：先看到维护中，结束后看到部署成功或失败提示。
10. 服务端进程被手动杀掉后，输入法应尝试后台恢复；恢复期间 UI 不应长时间卡死。

## 建议自动验证命令

```powershell
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$solutionDir='F:\IdeaProjects\参考项目\weasel\'

& $msbuild .\test\TestWeaselIPC\TestWeaselIPC.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=$solutionDir /m /v:minimal
.\x64\Release\TestWeaselIPC.exe /unit

& $msbuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=$solutionDir /m /v:minimal
& $msbuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=$solutionDir /m /v:minimal
& $msbuild .\WeaselServer\WeaselServer.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=$solutionDir /m /v:minimal
& $msbuild .\WeaselTSF\WeaselTSF.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=$solutionDir /m /v:minimal
& $msbuild .\WeaselSetup\WeaselSetup.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=$solutionDir /m /v:minimal
```

## Review 通过标准

这批改动可以认为通过 review 的最低标准：

- 按键热路径没有新的同步等待、阻塞重连或无界锁等待。
- TestKey 不再推进 Rime 状态。
- Backspace 回归测试存在且语义正确。
- 服务手动退出、自动恢复、手动重启三者语义不互相打架。
- 托盘提示覆盖部署完成/失败、退出、重启中、重启成功、重启失败。
- 单元测试通过，TSF/Server/Setup 目标可构建。
- reviewer 没有发现 TSF apartment 跨线程访问、IPC buffer 污染、manual-exit flag 竞态这三类高风险问题。
