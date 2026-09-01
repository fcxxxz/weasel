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
