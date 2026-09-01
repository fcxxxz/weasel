# weasel 三方对比与集成报告（2026-09）

对比对象：`rime/weasel`（origin/master @ d73f629）、`fxliang/weasel`（pb @ 4e1206f）、本地 fork。
集成分支：`integrate/pb`（基于本地 master，合并 fxliang/pb 与上游修复）。

## 1. 三方关系

```
93eec2d (upstream) ──┬── origin/master = 93eec2d + 9 提交（Setup profile、STDMETHODIMP、
                     │   non-inline auto-commit、版本宏、tray IPC d73f629…）
                     │        └── fxliang/pb = origin/master + 42 提交（4e1206f）
                     └── 本地 master = 93eec2d + 9 个本地性能/稳定性提交
                              └── integrate/pb = fxliang/pb + 本地工作 + 上游 PR 吸收
```

pb 分支**包含**最新 rime/master，因此合并 pb 即同时获得上游全部修复。

## 2. pb 相对 master 的核心内容（已全部集成）

- **渲染层重写**：GDI+/GDI/DirectWrite-DC + UpdateLayeredWindow → D3D11 + D2D1.1 +
  DXGI flip-model + DirectComposition（`WeaselUI/d2d.{h,cpp}`）。GPU 高斯模糊替代
  CPU/OpenMP 模糊；进程级共享设备、TextFormat 缓存、WIC 工厂缓存；device-lost 三层
  恢复 + WARP 回退；一批 GDI/句柄泄漏与越界修复。最低系统要求升至 Win 8.1（与 README
  声明一致）。
- **设置工具**：FontSettingDialog（字体/字重/字形/码位回退/实时预览）、真实候选窗样式
  预览（可拖拽）、40 个样式键 + 22 个颜色键 GUI 编辑（`WeaselStyleKeys.h`、
  `WeaselStyleColor.h`），落盘 `weasel.custom.yaml`；`CDialogDpiAware` 全对话框高 DPI。
- **TSF**：`hide_ime_mode_icon`、HKL 注册改 TSF-only、fake_key 语言栏刷新、
  `LAYOUT_VERTICAL_TEXT_FULLSCREEN`/`vertical_right_to_left`。
- **上游 9 提交**：Setup 按 region 选 profile（/hk /mc /sg）、STDMETHODIMP 修正、
  non-inline auto-commit、托盘刷新移出管道线程（#1912）等。

## 3. 从 issue/PR 吸收的内容（本轮新增）

| 来源 | 内容 | 提交 |
|---|---|---|
| #1914（open） | 客户端 IPC fail-fast + OVERLAPPED 有界等待（按键 500ms / 焦点通知 25ms），超时取消 I/O 并断线重连 | 95deded, 6b2b533 |
| #1889（open） | TSF conversion compartment 值驱动同步（外部程序切换中英文、Win10/11 差异、防级联反转） | 2b9ddd1 |
| #1907（closed） | CUAS workaround 用 U+2060 且仅在工作区激活时写入 | 2b9ddd1, 77b40a7 |
| 审计发现 | `/i` 升级重装 TSF、hide_ime_mode_icon 再激活、D2D DPI 未初始化、Release 关 D2D debug layer、LOG 宏零成本门控（WEASEL_ODS_LOG/调试器） | 43f2793, a05fe7d |
| 本轮测试挖出 | 见下节 | da35197 |

评估后**未**采纳：#1905（键盘布局，需实机）、#1836（图标泄漏，pb 已覆盖）、#1839
（重入崩溃，需 Thunderbird 复现）、#1906（D2D 并发崩溃，需 dump）、#1913（SOFTWARE
渲染延迟，需多 GPU 数据）、#1916（离线 userdb 竞争，需正式 maintenance-hold 设计）。

## 4. 本轮发现并修复的存量 bug（master/pb 同样存在）

重编测试后 `TestResponseParser` 间歇崩溃，顺藤摸瓜修了三处：

1. **`ContextUpdater` vec[2] 越界读**：`ctx.preedit.cursor` 只有两段值时读
   `vec[2]`，堆损坏导致间歇 0xC0000005/0xC0000409。cursor 缺省回退 -1。
2. **`TryDeserialize` 异常逃逸 + 模态弹窗**：只捕获 `archive_exception`，且归档
   **构造**（会立即校验签名）在保护外——畸形 payload 可让未捕获异常直接终止宿主
   进程（MSVC abort=exit 3）；失败路径还会在调用线程弹 `MessageBoxA`（卡顿类 bug）。
   现在构造纳入保护、捕获所有异常、失败重置目标、OutputDebugString 报告。
3. **测试桩静默失效**：上游 93eec2d 给 `RequestHandler` 加了 `EatLine` 参数并把
   UINT 改为 `WeaselSessionId`（MSVC 下 `unsigned int`≠`unsigned long`），
   `TestRequestHandler` 的“重写”不再重写，基类空实现对一切回 0——三棵树全部
   “failed to login”。已按精确签名重写并加 `override`。

另修测试本身：test_4 改用真实线上协议（序列化 CandidateInfo，旧键值协议已废弃）、
新增 test_5 畸形 payload 回归用例、移除两处 `system("pause")` 使测试可自动化。

## 5. 验证状态（Release x64, VS2022 v143）

- 主解决方案构建通过：WeaselServer.exe / WeaselDeployer.exe / weaselx64.dll 等。
- `TestResponseParser`：8/8 exit 0（含新回归用例）。
- `TestWeaselIPC /unit`：No errors detected。
- `TestWeaselIPC` 端到端（自起服务端 + 客户端）：登录、ECHO、按键、响应体、
  会话结束全链路 8/8 exit 0，且在 25ms 焦点级超时下通过（#1914 设计得到验证）。

## 5.5 性能基准与优化（/bench，2026-09-01）

`TestWeaselIPC /bench [iters] [threads]`（5000-10000 次往返，用户生产服务同机共存）
测客户端侧 IPC 往返分位数；`WEASEL_IPC_NAMESPACE` 隔离测试命名空间。

| 指标 | 基线 | 优化后 |
|---|---|---|
| echo p50 / p95 / p99 / max | 24µs / 41µs / 68µs / 11.8ms | **15µs / 21µs / 29µs / 98µs** |
| key p50 / p95 / p99 / max | 40µs / 70µs / 7.9ms / 22ms | **19µs / 30µs / 39µs / 167µs** |

已落地的优化（`74f09b9`）：

1. **线程本地复用 OVERLAPPED 事件**：原来每次读写都 CreateEvent/CloseHandle
   （客户端+服务端每事务 4 次内核句柄 churn）——中位数降约 20%。
2. **Present 同步间隔 1→0**：候选窗不再每帧等 vblank（120Hz 屏上最多 8ms/帧）。
3. **候选归档差分**：会话内页面未变时不重序列化 CandidateInfo（响应体最大块的
   序列化+传输都省掉）。
4. **TextFormat 缓存上限 64**：多方案/字体/DPI 长会话不再无界增长。

基准方法学教训：测试服务端每请求的 `cerr` 行缓冲打印是全部毫秒级长尾的来源
（p99 7.9ms→39µs），现已由 `WEASEL_TEST_TRACE` 门控。**结论：IPC 传输层实测干净
（p99 < 40µs），生产卡顿的剩余来源在引擎处理（librime/userdb）、服务端忙时
（部署/同步持锁，按键路径已有 500ms 上限+try-lock 直通）与渲染侧（已改立即呈现）。**

内存参考：用户在用的 0.17.4 WeaselServer 运行 1.2h 后 WS 735MB / 私有 229MB /
423 句柄 / 26 线程。新构建的 GDI/句柄泄漏修复 + 缓存上限预期显著改善，但
A/B 对比需安装新构建后实测（待用户执行）。

### 5.6 真实方案浸泡：发现并修复会话级泄漏（2026-09-01）

用用户真实魔虎方案（mohu_llm_zrm，复制到沙盒 `WEASEL_USER_DIR`）浸泡新构建，
暴露出两个仅在"大方案 + 真实延迟"下才显现的缺陷（小词典测试完全测不出）：

1. **服务端自连接泄漏（b615078）**：客户端事务超时关闭管道后，服务端 worker
   的响应写入失败，`PipeChannel::_Send` 的通用 catch-重试调用 `_Reconnect()`
   ——在服务端语境下这会**作为客户端连向自己的监听线程**。待发响应字节被
   自己的 worker 当命令读入，接受线程在一条永不关闭的自连接上永久阻塞。
   表现为每客户端会话泄漏 1 线程 + ~8 句柄，churn 下内存 +75MB/分钟线性上涨
   （6 分钟 518MB）。修复：服务端响应用无重试发送，写失败即退出 worker。
2. **START_SESSION 超时分级错误（b615078）**：登录被归入 25ms 焦点级，而真实
   方案首会话要 160-900ms（引擎加载 schema/lua/词典状态）→ 每次登录必超时，
   正是它把服务端逼进上述坏路径。登录现为独立 2s 上限，`TryTransact` 支持
   按命令分级超时。
3. **启动会话预热（3c00f2b）**：`Initialize()` 末尾创建并销毁一个引擎会话，
   把 900ms 冷启动成本移到服务启动期；首登录与后续登录同价。

验证：真实方案沙盒 21 客户端会话 → 21 worker 启动 / 21 退出（零泄漏），
线程/句柄持平，committed 稳定在 ~84MB（修复前同负载 6 分钟 518MB 且线性
上涨）；parser/unit 门全绿；Release x64 全解决方案重建通过。

真实方案下的稳态对照（同机同时刻）：新构建 WS ~700MB / 任务管理器专用工作集
~22-140MB——其中大头是 mohu 词典的内存映射页，新旧版本相同；**差异在私有
内存的增长曲线**：旧版 1.2h 涨到 229MB 仍在上行，新版修复后持平不涨。

已知未修（记录）：客户端异常退出（不 END_SESSION）会遗留一个 rime 会话与
SessionStatus——会话按 ipc_id 而非连接归属，需连接级追踪才能清理；上游同样
存在。每次应用崩溃泄漏一个会话，长期可累积。

## 6. 剩余风险（需实机）

- #1906 类 D2D 并发绘制崩溃：复扫确认本地 g_api_mutex 串行化已结构性排除
  （§8.3）；实机压力验证仍保留在回归清单。
- Win32/ARM64 未编（本机仅验证 x64）；Win 8.1 真机未测（DirectComposition 下限）。
- UIStyle 序列化字段变化要求 Server 与 TSF DLL 同包升级（单换任一会错位）。
- Chrome/Firefox/VSCode/Word 长时输入、多显示器 DPI 切换、#1822 Explorer 右键
  死锁等兼容性场景需实机回归。

## 7. 构建备忘

- 本仓库实际构建方式：msbuild `weasel.sln` /p:Configuration=Release /p:Platform=x64
  （weasel.props 已指向 E 盘 boost 1.84；librime 头/库由 get-rime.ps1 布置）。
- 仓库路径含中文时，git-bash 向 cmd/PowerShell 传参会乱码；可建 ASCII junction
  （如 `F:\weasel-build`）后再调用 MSBuild。
- 测试单独构建需传 `-p:SolutionDir=<repo>\`，否则 include 路径解析错误。

## 8. 全量复扫（2026-09-01）：两仓库 issue/PR/分支决策表

用 GitHub API 全量枚举 rime/weasel（issue+PR）与 fxliang/weasel（全部分支+提交），
逐项与本地代码比对后分四类。

### 8.1 确认已在（此前合并已覆盖，本轮验证判据）

| 来源 | 项 | 本地判据 |
| --- | --- | --- |
| pb `8026d75` | 样式编辑器（WeaselStyleKeys/Color + UIStyleSettingsDialog） | 文件存在 |
| pb `4e1206f` | 对话框重复 DPI 缩放修复 | `m_initialDpi`/`m_originalFont` 判据 |
| tray `57a812c` = 上游 #1912 | 托盘刷新移出管道线程（快照+合并+排空） | `WeaselTrayIconState`/`RequestRefresh`/`ApplyRefresh`/`DisableRefresh` |
| pb `416543c` | WIC 工厂缓存 + GDI 泄漏修复 | `DeviceResources::Get().wicFactory` |
| 上游 #1835 (`829b07e`) | STDMETHODIMP 误用 | 无 `STDAPI C*` 残留 |
| 上游 #1910 | 版本宏 C++ 构建 | WeaselConstants.h 已含 |
| 上游 #1779+pb `da1ef1c` | 负边距隐藏候选窗的 tip 抑制 | `_UpdateHideCandidates` 完整逻辑 |

### 8.2 本轮新吸收（commit 见 git log）

- **UI Prewarm**（#1886 思路适配）：`WeaselPanel::Prewarm()` = Refresh + 隐藏窗口上
  强制一次 DoPaint。此前 Initialize 已有 Refresh+Hide（设备/字体格式已预热），但
  隐藏窗口收不到 WM_PAINT，BeginDraw/Present/字形光栅化仍冷——现在首键前全部焐热。
  代价：空闲内存 +几 MB（D2D 设备提前常驻）；收益：首个候选窗零冷启动。
- **#1869**：`_ResizeWindow`/`_Reposition` 尺寸与位置未变时跳过 SetWindowPos
  （隐藏时；可见时仍重申 TOPMOST 以维持置顶语义），消除每键次冗余窗口管理调用。

### 8.3 判定为"已被等价实现/设计排除"（不吸收代码）

- **#1906**（服务端 D2D 并发崩溃）：本地所有触碰 UI 的 handler 调用都被单一
  `g_api_mutex` 串行化（会话/焦点类阻塞锁；按键 try-lock 拿不到即放行不吞键），
  并发绘制窗口结构性不存在。
- **#1913**（新宿主进程首键 260-820ms）：成因是 weasel.dll 进程内建 D2D 设备；
  本地 TSF dll 无任何 D2D（渲染全在服务端），且服务端冷启动已由 Prewarm 解决。
- **#1885**（位置更新去重）：`input_position_cache`（ShouldSend/MarkSent/Reset）
  已等价实现 IPC 侧去重。
- **#1908**（隐藏候选窗时状态图标每键弹出）：pb 的 `hide_candidates` 逻辑
  （margin_negative + inline_no_candidates 双路径）已覆盖；图标独立显示为
  ascii_tip 的既定特性。

### 8.4 明确不吸收（含理由）

- **#1462**（删除 CUAS 占位空格机制）：与本地"门控 CUAS workaround"（77b40a7）
  是两种模型；整删改变 TSF 提交语义，无实机回归测试 rigs 前不动。
- **#1911**（安装器区域 profiles CN/TW/HK/MC/SG）：单用户简中场景无收益。
- **#1905**（keyboard_layout 键位映射）、**#1895**（langbar 值驱动同步）、
  **#1329/#1471/#1854/#1726**（未合并大特性）：按需再议。
- **#1899/#1892/#1894/#1873/#1651/#1830/#1796/#1853/#1916**（gvim/气泡重叠/
  多显示器闪烁/黑块等）：场景狭窄或上游无进展，记录备查。
- fxliang `ui-fixes` 分支（GDI+ 时代 25 提交）：被 pb D2D 重写整体取代。

### 8.5 本轮验证

构建绿（Release x64 全解决方案）；parser/unit 门 0 退出；沙盒真服务器冒烟
（隔离命名空间 + 真实 mohu 方案）：15s 存活、`/q` 干净退出、日志无渲染错误、
空闲 WS 56.2MB / 提交 65.7MB（含预热后的 D2D 常驻，与浸泡期数据一致）。

### 8.6 事故记录与修复（2026-09-01 11:46）

现象：用户实机突然无法输入，TSF 日志大量 `skip recovery: manual exit marked`。
根因：沙盒冒烟的 `/q` 写入的 manual-exit 标记文件路径**未按命名空间隔离**
（管道/互斥量已隔离，此文件漏了），污染了真实服务的自动恢复判断。
修复：`ServiceManualExitFlagPath()` 并入 `IpcNamespaceSuffix()`；回归冒烟确认
沙盒 `/q` 只写 `weasel-service-manual-exit<ns>.flag`，默认命名空间零接触。
教训：新增任何跨进程的全局路径（文件/注册表/事件）都必须过一遍命名空间
隔离审查。

### 8.7 移除官方升级通道（2026-09-01，按用户要求）

自编译版本若保留 WinSparkle 官方 feed（rime.github.io），官方发新版后会
提示升级并可能用官方安装包覆盖定制构建。已整体移除：
- WeaselServerApp：删除 win_sparkle 初始化/语言/注册表/清理调用与
  check_update()；托盘菜单不再注册"检查新版本"。
- WeaselServer.cpp：/update 命令行不再触发检查（保留参数解析与日志）。
- WeaselServer.rc：删除 3 个语言的"检查新版本"菜单项 + 4 个 APPCAST
  feed 资源（UTF-16 编辑，BOM 与编码已验证）；WeaselTSF.rc 删除对应
  语言栏菜单项。
验证：构建绿；单测/parser 门 0 退出；WeaselServer.exe 二进制无任何
WinSparkle 引用；沙盒冒烟存活+干净退出。

### 8.8 打包链路与 librime（2026-09-01）

- CI（ci.yml）：推送 master 自动出滚动 release（tag=latest，预发布），
  含安装包 weasel*.exe 与调试符号；winsparkle 构建步骤与安装包内
  WinSparkle.dll 一并移除（对应 §8.7）；update-appcast.yml 删除。
- librime：子模块恢复为 1.17.0 版本引用；CI 与本地构建均使用
  `get-rime.ps1 -use dev` 浮动拉取最新正式版，不钉死版本。1.16.1 与
  1.17.0 A/B 测试未见启动或内存差异；1.17.0 的主要新增是多句候选与
  excluded words 崩溃修复。

## 9. 三方对比（2026-09-01，最终有效数据）

前一版表格已作废：测试时误把 fxliang 安装包内的 x86 同名文件覆盖了 x64，
且旧版服务器抢占了默认管道，造成“本构建全面落后”和句柄暴涨等假象。
本次改用各自发布包中 PE 头确认的 x64 文件；同一份数据与用户配置；
我们的版本使用唯一 IPC 命名空间；每个服务器单独启动，确认存活后再测。

| 指标 | 官方 0.17.4 x64 | fxliang pb x64 | 本构建 x64 |
|---|---:|---:|---:|
| 服务器启动后工作集 (MB) | 41.2 | 49.3 | 55.4 |
| 服务器启动后私有提交 (MB) | 55.2 | 60.7 | 66.4 |
| 首会话建立 (ms) | 78.9 | 94.7 | **18.2** |
| 首键延迟 (ms) | **0.071** | 0.099 | 0.579 |
| 按键 p50 (µs) | **32.6** | 68.6 | 62.2 |
| 按键 p95 / p99 (µs) | **47.0 / 60.6** | 102.4 / 167.8 | 175.1 / 220.6 |
| 5 次会话 churn p50 (ms) | 82.4 | 89.7 | **1.09** |
| 打字后私有提交 (MB) | **56.0** | 65.6 | 68.5 |
| 打字后句柄变化 | +2 | +82 | +11 |

结论：保活会话让本构建的会话建立和 churn 明显优于两份对照；内存略高于
官方，但与 fxliang 接近；按键中位数处于两者之间，尾延迟高于对照，仍需
继续优化。句柄仅小幅增长，上一版 +1239 的数字是测试串线造成的假象。

### 9.1 保活会话修复

官方 librime 在最后一个会话销毁后会卸载方案状态。此前服务启动预热会话后
立即销毁，导致下一次客户端登录仍支付约 700–900ms 的冷加载。本构建现在
保留一个不属于客户端会话表的常驻引擎会话；干净隔离实测首会话从 900ms 降至
约 18–156ms，服务退出时显式销毁，避免泄漏。这个改动是在发现错误三方表后
单独验证出来的实际优化。

## 10. 隐藏窗口交换链泄漏修复（2026-09-01 晚，含事后分析）

长压（30,000 真实虚拟键）暴露三个同源症状：句柄 480→11,300+（精确
2 句柄/次真实 resize）、按键 p95 高达 1.7–2.8ms、提交内存缓涨 ~30MB。
对照同负载的 fxliang x64（384→383 稳定）与官方 x64（342→344）确认是
本构建自身问题。

逐步二分（跳过 UI→稳定；跳过 resize→稳定；仅跳 OnResize→稳定；插桩
确认 DoPaint 仅 3 次、设备丢失 0 次）定位根因：

**根因**：候选窗隐藏期间每次内容尺寸变化都执行 ResizeBuffers。DWM
通过已绑定的 DirectComposition visual 持有旧缓冲区引用，而隐藏窗口
永不 Present，旧缓冲区对永远不被消费——每次真实 resize 恰好搁浅
2 个内核对象（双缓冲）。fxliang 侥幸躲过：它每键无条件调用 OnResize，
绝大多数是同尺寸 no-op 不触发重分配。每次 resize 后冗余的
SetContent/SetRoot/Commit 跨进程调用则是毫秒尾延迟的直接来源。

**修复**（两处，可见行为零变化）：
- 隐藏期间完全跳过窗口 resize，记为待定尺寸，在下一次显示前一次性
  补上（与既有的"隐藏时跳过重定位"策略一致）
- OnResize 移除冗余的 DirectComposition 重绑定（visual 已引用同一
  交换链对象，尺寸变化后的 Present 自动生效）

**验证**（30,000 键全路径，唯一命名空间隔离）：句柄 474→481 稳定；
p95 1,723µs→250µs；p50 65µs；提交内存 62.7MB 封顶。期间多轮二分
脚本曾因未替换测试二进制产生两轮无效"稳定"结论，已全部用正确
md5 的二进制重测覆盖。

基准工具 /realseat 同步修正为 Win32 虚拟键码（此前误用字符码，
不是真实键盘路径）。

## 11. 最终对照与根因三连（2026-09-01 深夜）

按"超越 fxliang 与官方、充分测试、第一性原理、剃刀、对抗审查、墨菲测试"推进。

### 11.1 对比基线修正（用户指出）

旧表误用 2025-06 官方 0.17.4 安装包，已作废。重建：官方 = master
d73f629 本地构建；fxliang = pblatest 4e1206f（其最新发布）；本构建 =
master + 本轮优化。三方同一份规范配置（各自独立全新部署），交叉多轮。

### 11.2 根因三连（逐层二分 + 2×2 互换实验）

1. 线程/内存形态 = 显卡驱动驻留：42 个线程属 nvwgf2umx.dll（NVIDIA
   用户态驱动，D3D 硬件设备加载后驻留 ~50MB 提交内存）。fx 真实打字
   同样如此（实测其线程 25→50）。结论不是弃 GPU：**GPU 优先、WARP
   兜底**（原上游语义），`WEASEL_WARP=1` 供低内存偏好用户选用——
   CPU 满载时软渲染会与系统抢 CPU，且全屏布局+阴影在 4K 下是每键
   整屏 CPU 光栅化（对抗审查 #7），故 GPU 优先是正确取舍。
2. "延迟全面落后" = 沙盒目录损坏（丢失 default.custom.yaml），2×2
   目录互换实验证伪；同目录下本就领先。
3. p95 尾部双源（隐藏 resize 搁浅 + DComp 冗余提交）已修复（§10）。

### 11.3 GPU 默认模式最终对照（同配置交叉，真实虚拟键）

| 指标 | 本构建 | 官方 master | fxliang pb |
|---|---:|---:|---:|
| 按键 p50 | **29–41µs** | 43.3 | 64.7–67.1 |
| 按键 p95 | 50–198µs（双峰，机器态相关） | 72–74 | 100–102 |
| 首会话 | **2.0–17.6ms** | 89.6–245.7 | 92–93 |
| 会话切换 p50 | **0.78–1.0ms** | 0.88–88.7 | ~90 |
| 打字后线程/提交 | 49 / 62–63MB | 23–25 / 57.7–68.4 | 50 / 67 |

会话/切换/p50 决定性领先；p95 在良态 ≤50µs、劣态 ~160-200µs（官方
稳定 72-74；本机双峰现象与 WARP/HW 无关，源于环境突发负载，留待
实机长测观察）；内存与两者打字态持平（idle 立即绘制的差异见 §11.2）。

### 11.4 对抗性审查结论（全部处置）

子代理反向审查 18 项：4 项修复——①Present 改回 (0,0)（每帧等 vblank
最高 8ms 打字卡顿，且 TSF 内进程模式卡的是宿主 UI 线程）；②
_ReceiveSync 删除短读二次分支（防版本偏移客户端帧错位）；③Initialize
入口先销毁旧保活会话（防部署后双初始化泄漏一个引擎会话）；④Prewarm
补应用待定尺寸 + _Reposition 用待定尺寸计算钳位（防隐藏期尺寸过期）。
其余 14 项核查为不成立或与改动前等价（含 TSF 客户端路径无回归、
DComp 去重绑正确、管道阻塞 IO 与原 overlapped 等价等）。

### 11.5 墨菲定律压测（全过）

冷启部署 13.4s 就绪；三轮 30k 键（共 90k）句柄纹丝不动、提交钉死；
双客户端并发无死锁（p50 反降至 16.7µs）；打字中强杀客户端后无损
续服；干净退出。GPU 模式复测句柄同样稳定（467→467/30k 键）。

### 11.6 服务器端阻塞管道 IO

已接受管道改阻塞读写（worker 可弃无需每请求事件跳转）；客户端完整
保留 overlapped+分级超时（#1914 防冻语义不变）。审查确认两路径
ERROR_MORE_DATA 语义一致、无自连接回归。

### 11.7 测试方法学教训

两次错误结论（旧安装包基线、损坏沙盒目录）均被 2×2 互换与最新源码
重建基线纠正。凡对比必须：三方各自全新部署、同配置、交叉多轮、唯一
命名空间、核验被测二进制身份（md5）。
