# 小狼毫输入法 · 个人优化构建

[![Release](https://img.shields.io/github/v/release/fcxxxz/weasel)](https://github.com/fcxxxz/weasel/releases/latest)
[![CI](https://github.com/fcxxxz/weasel/actions/workflows/ci.yml/badge.svg)](https://github.com/fcxxxz/weasel/actions/workflows/ci.yml)

基于 [rime/weasel](https://github.com/rime/weasel) 官方版与 [fxliang/weasel](https://github.com/fxliang/weasel) pb 分支深度整合的个人优化版，目标是高性能、低内存占用、无卡顿的 Windows Rime 前端。

## 相对官方版的主要差异

**性能与内存**

- 修复会话级线程与内存泄漏：客户端超时导致服务端自连接，此前每会话泄漏 1 线程 + 约 8 句柄（+75MB/分钟）
- IPC 命令分级超时与快速路径，按键往返 p50 约 19µs / p99 约 39µs
- 服务启动时预热引擎会话与渲染管线，开机后第一次打字不再有冷启动卡顿
- 候选串与输入位置去重传输，跳过尺寸/位置未变时的冗余窗口调用

**行为**

- 移除官方自动升级通道（WinSparkle）：不会被官方发布的新版本提示或覆盖
- librime 钉版 1.17.0，构建可复现

**界面**

- 新增字体设置对话框、带实时预览的样式编辑器（来自 pb 分支）
- 高 DPI 感知的设置对话框，修复多 DPI 下的重复缩放问题
- DirectComposition / D3D11 / D2D 渲染栈替代 GDI+，长驻宿主中不再拖慢 Chrome 等应用

完整的对比分析、基准数据与事故记录见 [docs/upstream-comparison-2026-09.md](docs/upstream-comparison-2026-09.md)。

## 下载与安装

每次推送 master 自动构建，安装包发布在本仓库 [Releases](https://github.com/fcxxxz/weasel/releases/latest)（`latest` 标签，滚动更新），包含 NSIS 安装程序与调试符号。

适用于 Windows 8.1 ~ Windows 11。安装前请先退出正在运行的旧版小狼毫（托盘图标右键退出）。

## 使用

- 通过输入法指示器选择【中】图标开始使用
- `Ctrl+`` 或 `F4` 呼出方案选单
- 用户文件夹位于 `%AppData%\Rime`，修改方案或配置后需在开始菜单执行「重新部署」
- 定制方法参考 [Rime 定制指南](https://github.com/rime/home/wiki/CustomizationGuide)

## 从源码构建

见 [INSTALL.md](INSTALL.md)。要点：Visual Studio 2022 + Boost + librime（`get-rime.ps1` 自动下载），`build.bat installer` 出安装包。

## 致谢与许可

本构建站在上游之上，感谢：

- [rime/weasel](https://github.com/rime/weasel) 及其全体贡献者（佛振、邹旭、wishstudio、Prcuvu、nameoverflow、determ1ne 等）
- [fxliang](https://github.com/fxliang) 的 pb 分支提供了 DirectComposition 渲染栈与大量设置界面改进
- 图标设计 [Patricivs](https://github.com/Patricivs)

引用的开源软件：Boost、librime、LevelDB、OpenCC、yaml-cpp、marisa-trie、curl、glog、7-Zip 等。

许可证：**GPLv3**（继承自上游项目）。
