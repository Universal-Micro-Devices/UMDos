# 经典主题重构说明（v2.6.112-dev.10）

> 目标：贴合参考图（ReactOS / Windows 2000 经典桌面）。只改渲染与 UI 交互层；
> 内核、中断、内存、多任务、shell 命令逻辑未动。

## 0. 分辨率

VBE 优先选 800×600×32（模式 0x11A）；窗口尺寸与终端网格按 800×600 适配，
终端为经典 80×25（8×16 字符格）。磁盘剩余空间显示为 MB/GB（不再用块）。

## 1. 桌面

| 项目 | 实现 |
|---|---|
| 背景 | 青绿渐变 `#108787 → #0A6464`（经典 Win2000 teal） |
| 图标 | 32×32 经典图标，全部用矩形/线条绘制：My Computer（米灰显示器 + 青绿屏）、Command Prompt（黑窗口 + `C:\>` + 光标块）、Recycle Bin（灰色桶 + 条纹） |
| 标签 | 白色文字 + 1px 黑色阴影；长名字分两行（My / Computer、Command / Prompt） |
| 布局 | 左上角竖排三个图标 |

## 2. 任务栏与开始菜单

* 底部 30px 经典灰任务栏，顶边 1px 白色高光；
* **Start** 按钮：3D 凸起，左侧四色小方块（红/绿/蓝/黄）；
* 任务按钮：每个打开的窗口一个 3D 按钮（File Manager / 文件名）；
* 托盘：内凹时钟（HH:MM）；
* **Start 菜单**（点击 Start 或 **Ctrl+Esc**）：Programs、Documents(灰)、Settings(灰)、Find(灰)、Help、Run...、Shut Down...、Log Off...；
* **Programs 子菜单**：File Manager、Command Prompt、Text Editor、About UMDos（▸ 三角用像素线绘制）；
* 动作接线：File Manager → 打开文件管理器；Command Prompt → 终端；Text Editor/About → 打开 README 编辑器；Log Off → 锁屏；Shut Down → ACPI S5。

## 3. 窗口外框（经典）

* 22px 标题栏：活动为 `#000080 → #1084D0` 渐变，非活动纯灰；
* 标题文字白色左对齐；
* 右上三个 16×16 3D 小方按钮：`—`（最小化）、`□`（最大化）、`X`（关闭，用像素线画）；
* 标题栏下 20px 菜单栏：`File  Edit  View  Help`；
* 内容区经典灰 + 1px 白/灰 3D 边框；输入框内凹；
* 状态栏：底部内凹分格（`N items` / `free X blocks`）。

## 4. 菜单栏下拉

| 窗口 | 菜单项 |
|---|---|
| File Manager | File: New Folder / Close；Edit: Rename / Delete；View: Up；Help: About UMDos |
| Text Editor | File: Save / Close；Edit: Copy / Cut / Paste / Select All；View: Status Bar（灰）；Help: About（灰） |

点击菜单标题打开，点击菜单项执行；**Ctrl+M** 快捷键打开当前窗口的 File 菜单。

## 5. 终端（保持上一版深色扁平风）

背景 `#0C0C0C`、30px Tab 栏 "UMDos Terminal"、右上 `— □ ×` 扁平按钮、
绿 `PS ` / 蓝 `C:\UMDos\User` / 白 `> ` Prompt、实心浅灰方块光标 + PIT 闪烁。

## 5. 与 dev.9 的差别

| 项目 | dev.9 | dev.10 |
|---|---|---|
| 桌面背景 | 纯灰 | 青绿渐变（参考图同款） |
| 桌面图标 | Win10 极简（细线条） | 经典 32×32 图标（米灰显示器/黑窗口/灰桶） |
| 顶部栏 | 灰色 30px 自定义栏 | 移除，时钟移入任务栏托盘 |
| 任务栏 | 简单 "Tasks:" | Start + 任务按钮 + 时钟托盘 |
| 开始菜单 | 无 | 完整经典菜单 + Programs 子菜单 |
| 窗口标题 | 22px 标题 + 无菜单栏 | 22px 标题 + 菜单栏 + 3 个方按钮 |

## 6. 操作

| 操作 | 效果 |
|---|---|
| 点击 Start / **Ctrl+Esc** | 开关开始菜单 |
| 点击 My Computer | 打开文件管理器 |
| 点击 Command Prompt / **F10** | 打开终端 |
| 桌面 **Enter** | 打开文件管理器 |
| **Esc** | 关闭开始菜单 / 退出终端 |
| **Ctrl+L** | 锁屏 |

## 7. 验证

```sh
python tools/test_chain.py
python tools/emu_ui.py
```
全部通过；截图：`shots/classic-desktop.png`、`classic-start.png`、`classic-fm.png`、`classic-term.png`。

## 8. 已知限制

1. 菜单栏（File/Edit/View/Help）为经典外观，尚未接入下拉菜单交互。
2. Documents/Settings/Find 置灰不可用（按规格隐藏功能，不加说明文字）。
3. 回收站无功能，仅图标。
4. 分辨率 800×600；不支持 800×600×32 的固件会回退其它 32bpp 模式。
