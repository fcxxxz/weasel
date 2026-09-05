# 候选框位置跳动问题排查分析

> 日期：2026-09-05
> 现象：输入法候选框出现的位置不稳定，很大几率会先出现在一处、随即"跳一下"到光标处。
> 结论性质：根因分析，**未做任何代码修改**。分析基于工作区当前状态（含未提交改动）。

## 结论（TL;DR）

跳动不是坐标计算错误，而是**"先显示、后定位"的时序倒挂**：

1. TSF 宿主进程内的候选窗，是在按键的同步 edit session 里、用**上一次遗留的插入点位置（`WeaselPanel::m_inputPos`）**先显示出来的；
2. 本次光标的真实位置，要等一个**排队的只读 edit session（`GetTextExt`）事后执行完**，才通过 `MoveTo` 补上；
3. 两步之间隔着一次消息循环（至少一个 DWM 合成帧，应用忙时可达几十上百毫秒）。中间只要发生一次绘制，新内容就会先画在旧位置上，随后窗口整体挪到光标处——这就是肉眼可见的"跳一下"。

是否看得见跳，取决于**面板 WM_PAINT 与排队 GetTextExt session 的竞速**，结果随应用负载浮动，因此表现为"不稳定、很大几率"。

该结构自上游 d73f629 便存在（上游 `DoEditSession` 同样先 `_UpdateCompositionWindow` 后 `_UpdateUI`），不是近期 perf 改动引入；但本仓库的 DirectComposition 渲染管线让旧位置上那一帧更容易被真实渲染出来（见 §5）。

## 1. 位置更新链路全景

TSF 应用里用户看到的候选窗，是**宿主进程内** `CCandidateList::_ui`（`WeaselTSF.dll`）画的；`WeaselServer.exe` 里还有另一个 `UI` 实例（供 IMM32 客户端和通知提示用），两者相互独立。分析症状时必须区分这两个面板。

```
按键 OnTestKeyDown/OnKeyDown (KeyEventSink)
  └─ _ProcessKeyEvent ──IPC──▶ 服务端 ProcessKeyEvent（引擎吃键，生成响应）
  └─ _UpdateComposition
       └─ WeaselTSF::DoEditSession（通常同步执行）        ← EditSession.cpp
            ├─ GetResponseData 解析 commit/preedit/status
            ├─ _StartComposition → 排队 CStartCompositionEditSession（嵌套请求，异步）
            ├─ _UpdateCompositionWindow → 排队 CGetTextExtentEditSession（嵌套请求，异步）★位置来源
            └─ _UpdateUI → CCandidateList::UpdateUI
                 ├─ _ui->Update(ctx, status)   （隐藏期：存上下文 + 置脏标记）
                 └─ Show(TRUE) → UI::Show
                      ├─ RefreshDirty() → WeaselPanel::Refresh（布局 + _Reposition，用旧 m_inputPos）★先显示
                      └─ ShowWindow(SW_SHOWNA)  ← 此刻窗口在旧位置可见
……外层 session 结束，应用泵消息……
排队 session 依次执行：
  CStartCompositionEditSession（创建组合、ZWSP、末尾再排队一次 GetTextExt）
  CGetTextExtentEditSession：GetTextExt(光标处矩形)
       └─ _SetCompositionPosition
            ├─ m_client.UpdateInputPosition(rc)  ──IPC──▶ 服务端（仅影响服务端面板，非本症状）
            └─ _cand->UpdateInputPosition(rc) → WeaselPanel::MoveTo → SetWindowPos ★后定位（跳变发生处）
```

关键事实：`_UpdateCompositionWindow` 请求的 `CGetTextExtentEditSession` 是在**已持有文档锁的 edit session 内部发起的嵌套请求**，TSF 不允许重入，只能排队异步执行。代码注释对此有明确认知（`EditSession.cpp`："Positioning is still updated by the queued read session after the new composition is created."）。

## 2. 逐键时序分解（以一次组合的首键为例）

在支持 TestKeyDown/KeyDown 的常规应用里，按键处理发生在 `OnTestKeyDown`（`WeaselTSF/KeyEventSink.cpp:230-233`），`_UpdateComposition` 请求的 `DoEditSession` 通常同步执行。内部顺序：

| 时刻 | 动作 | 位置依据 |
| --- | --- | --- |
| T0 | `DoEditSession`：解析响应、排队 Start/GetTextExt session、`_UpdateUI` → `Show()` | **旧 `m_inputPos`**（上一组合遗留，或初值 `{0,0,0,0}`） |
| T0 | `Show()` → `RefreshDirty()` → `Refresh()`：完整布局 + `_Reposition()` + `RedrawWindow()`，然后 `ShowWindow(SW_SHOWNA)` | 窗口在旧位置变为可见 |
| T0→T1 | 应用泵消息；WM_PAINT（队列空闲时产生）可能先到达 → `DoPaint` → `Present` | **新内容画在旧位置**（若 paint 先于 move） |
| T1 | 排队的 `CGetTextExtentEditSession` 执行：`GetTextExt` → `MoveTo`（窗口已可见，走完整重定位） | `SetWindowPos` 跳到真实光标处 |

注：若外层 `DoEditSession` 本身被应用判为异步（`_async_edit == true`），整体顺序不变——Show 仍先于排队的 GetTextExt。

## 3. 为什么"很大几率"且"不稳定"

决定跳变是否可见的竞速双方：

- **面板 WM_PAINT**：`WeaselPanel::MsgHandler`（`WeaselPanel.cpp:1223-1224`）→ `DoPaint` → `swapChain->Present`。WM_PAINT 是低优先级**生成型**消息，仅在消息队列清空时产生；
- **排队的 GetTextExt session**：由 msctf 派发，需要应用**授予读锁**。忙的应用（浏览器、IDE、Word）会拖延授权。

两种结果：

- GetTextExt 先跑完：窗口先被挪到光标处、再绘制首帧 → 看起来正常；
- WM_PAINT 先发生（应用忙、慢授锁时常见）：新候选内容画在旧位置 → 随后 `MoveTo` → 跳变可见。

窗口创建为 `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition swapchain（`WeaselPanel::Create`，`WeaselPanel.cpp:405-412`；`D2D.cpp:218` `CreateSwapChainForComposition`）。只要 Present 过一次，`ShowWindow` 之后 DWM **不依赖 WM_PAINT 就立即合成已有内容**，进一步提高了旧位置那一帧被看见的概率。竞速耗时随应用负载浮动 → "不稳定、很大几率"。

## 4. 旧位置的三个来源（放大因素）

### 4.1 `m_inputPos` 跨组合存活

组合正常结束：`_EndComposition(..., true)` → `EndUI` → `_DisposeUIWindow` → `_ui->Destroy()`，默认 `full=false`（`include/WeaselUI.h:32`）只销毁窗口、保留 `pimpl_` 和 `WeaselPanel` 对象，`m_inputPos` 成员原样保留（初值 `{0,0,0,0}`，`WeaselUI/WeaselPanel.h:83`）。

后果：下一句输入的首帧显示在**上一句输入的位置**；换行/换输入点后两点距离大，跳得特别明显；进程内第一次输入甚至会先被钳制到工作区左上角（`_Reposition` 的 `rcWorkArea` 钳位），出现"候选框在屏幕左上角闪一下"的经典症状。

### 4.2 GetTextExt 结果被整次丢弃

`CGetTextExtentEditSession::DoEditSession`（`WeaselTSF/Composition.cpp:193-194`）：`GetTextExt` 返回 `(0,0)` 时整次位置更新作废。部分应用（尤其 CUAS 场景）前几次调用返回无效矩形，定位被进一步推迟。

### 4.3 CUAS 探测的首调丢弃

`_SetCompositionPosition`（`WeaselTSF/Composition.cpp:243-249`）：进程内第一次定位调用若碰到零高度矩形（`rc.top == rc.bottom`），置 `_fCUASWorkaroundEnabled = TRUE` 后直接 `return`，本次不更新位置。位置更新再推迟一键，加剧"先错后跳"。

### 4.4 inline_preedit 连续输入的微跳动

启用 inline_preedit 时每键光标前进一个字符，每键都重复"T0 旧位置刷新显示、T1 再 MoveTo"。虽然单步距离小，但构成持续的轻微抖动。

## 5. 与本仓库近期改动的关系

| 改动 | 与本症状的关系 |
| --- | --- |
| 上游 d73f629 基线 | 结构性时序倒挂自上游继承（上游同样先 `_UpdateCompositionWindow` 后 `_UpdateUI`，且 `CStartCompositionEditSession` 末尾同样补一次定位）。**非本仓库引入。** |
| 998df00（隐藏期跳过布局、`MoveTo` 隐藏只记录） | 只省掉不可见时的工作，未改变"显示先于定位"的顺序。**非成因。** |
| DirectComposition / GPU 优先渲染（03d41ae 等） | 中性偏放大：DComp 内容不依赖 WM_PAINT，ShowWindow 后旧帧立即被 DWM 合成，竞速输了时那一帧内容更完整、更可见。 |
| 预热（Prewarm / StartUI warm-up） | 让隐藏窗口更早具备可呈现内容，同上属于放大因素，非根因。 |
| 未提交的服务端 `FlushPendingUI` 延迟化 | 只作用于 WeaselServer 进程的面板；TSF 应用里的候选窗在宿主进程内绘制，**与本症状无直接因果关系**。但二者同构：UI 应用被推迟到"应答之后"。 |
| 未提交的按键 IPC 重试（`Sleep(15)` 重发） | 只影响按键整体延迟，不改变显示/定位相对顺序。 |

## 6. 修复方向（仅供参考，未实施）

消除跳动的本质是让**第一次可见显示发生在拿到新插入点之后**，可选思路：

1. **首帧延迟显示**：窗口跨组合首次 Show 时，若 `m_inputPos` 是遗留旧值（可用"本次组合尚未收到过 MoveTo"标记判断），先只做布局不 `ShowWindow`，等 `MoveTo` 落地后再显示；
2. **位置先行**：把最近一次 `GetTextExt` 结果缓存进显示路径，`Show()` 前强制以缓存位置 `_Reposition`，保证显示坐标与引擎侧一致（仍受限于 GetTextExt session 排队时机）；
3. **外提定位请求**：在进入外层 edit session **之前**（key sink 回调里，无锁竞争时）先发起一次只读 `GetTextExt`，使位置更新有机会先于/同步于 Show；
4. 组合 1+3：首键场景收益最大，且不影响 998df00 已有的隐藏期省电语义。

注意任何方案都要保留 `MoveTo` 的反抖动逻辑（`WeaselPanel.cpp:219-221` 的 <6px 跳过分支）和 sticky 翻转语义，避免引入新抖动。

## 7. 关键代码索引

| 位置 | 作用 |
| --- | --- |
| `WeaselTSF/EditSession.cpp:69-75` | `DoEditSession`：先排队定位、后 `_UpdateUI` 显示（时序倒挂点） |
| `WeaselTSF/Composition.cpp:223-238` | `_UpdateCompositionWindow`：嵌套只读 session 请求（排队异步） |
| `WeaselTSF/Composition.cpp:169-220` | `CGetTextExtentEditSession`：`GetTextExt` + `(0,0)` 丢弃 + enhanced_position 修正 |
| `WeaselTSF/Composition.cpp:240-255` | `_SetCompositionPosition`：CUAS 首调丢弃；分发到客户端 IPC 与 `_cand` |
| `WeaselTSF/CandidateList.cpp:201-219` | `UpdateUI`：`_ui->Update` + `Show`（同步路径，旧位置显示点） |
| `WeaselUI/WeaselUI.cpp:48-53` | `UIImpl::Show`：`RefreshDirty` → `Refresh`（旧位置布局）→ `ShowWindow` |
| `WeaselUI/WeaselPanel.cpp:189-235` | `MoveTo`：可见窗口重定位（跳变执行点；隐藏期仅记录） |
| `WeaselUI/WeaselPanel.cpp:314-344` | `Refresh`：布局 + `_Reposition` + `RedrawWindow` |
| `WeaselUI/WeaselPanel.cpp:858-936` | `_Reposition`：工作区钳位、sticky 翻转、`m_inputPos.bottom` 回写 |
| `WeaselUI/WeaselPanel.cpp:1223-1224` | WM_PAINT → `DoPaint` → `Present`（竞速一方） |
| `WeaselUI/WeaselPanel.h:83` | `m_inputPos` 声明与初值 |
| `include/WeaselUI.h:32` | `Destroy(bool full=false)`：跨组合保留 `pimpl_`/`m_inputPos` 的根源 |
| `WeaselTSF/TextEditSink.cpp:68-70` | `OnLayoutChange` 触发的额外排队定位（应用重排版后） |
