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

## 6. 剩余风险（需实机）

- #1906 类 D2D 并发绘制崩溃：需多宿主压力 + dump；线程收口方案待定。
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
