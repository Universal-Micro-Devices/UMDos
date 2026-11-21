# UI 视觉重构说明（v2.6.112-dev.9）

> 范围：只改渲染与 UI 交互层。内核、中断、内存管理、多任务、shell 核心命令逻辑未改动；
> 双缓冲/事件循环/输入路径保持原样。

## 1. 扫描结果

### 中文字符串（UI 文案）

| 文件 | 内容 | 处理 |
|---|---|---|
| `src/kernel/ui.c` | 16 个 `u32` 标签数组（T_MYCOMPUTER、T_LOGIN、T_USER、T_PASS、T_FILES、T_NAME、T_SIZE、T_CLOSE、T_TOTAL、T_PIECES、T_SYSTEM、T_NEW、T_DEL、T_EDIT、T_RENAME、T_UP），经 `gfx_cjk_*` 绘制 15 处 | 全部替换为 ASCII 字符串常量，改用 `gfx_text*` |
| `installer/src/main.c` | 87 处中文字符串（欢迎页、磁盘页、分区页、文件系统页、账户页、风险页、进度页、日志） | 全部译为英文 |
| `src/kernel/proc.c` | `/proc/version` 里的阶段标记 `P10` | 改为 `kernel: 64-bit` |
| `src/kernel/term.c` | 终端横幅含 `(P11)` | 改为 `UMDos Terminal. Type 'help' ...` |

### 中文字库

| 文件 | 处理 |
|---|---|
| `src/kernel/cjk_glyphs.h`（16 个汉字的 16×16 点阵表） | **删除**；`gfx.h/gfx.c` 中的 `gfx_cjk/gfx_cjk_text/gfx_cjk_width/gfx_cjk_center` 一并移除 |
| `installer/src/cjk_glyphs.h` + `installer/src/gfx.c` 的 `gfx_cjk*` | **删除**；`mixed_text` 简化为纯 ASCII 绘制 |
| `tools/gen_fonts.py`、`tools/gen_installer_fonts.py`（汉字字库生成器） | **删除** |

### 旧拟物化图标 / 复杂位图

| 文件/函数 | 旧实现 | 处理 |
|---|---|---|
| `src/kernel/ui.c: draw_computer_icon` | 圆角外壳 + 渐变屏幕 + 多层支架 | 重写为扁平显示器：浅灰机身、单色屏幕、两条直线底座 |
| `draw_topbar` | 深色横幅 + 荧光蓝分隔线 | 改为经典灰 30px 顶栏 + 3D 边框 + 内凹时钟 |
| `draw_taskbar` | 深色条 + 圆角胶囊按钮 | 改为经典灰任务栏 + 方角 3D 按钮 |
| `draw_file_icon` / `draw_dir_icon` | 多层描边 + 高光色 | 改为单色轮廓 + 纯色块 |
| 新增 `draw_terminal_icon` / `draw_recycle_icon` | — | 深色终端方块、线条垃圾桶（纯几何） |
| `installer/assets/icons/generated/placeholder_icons.h` | 16×16 单色 mask | 保留（规格允许的 16×16 纯色 mask 回退），不是写实位图 |

## 2. 经典主题（ReactOS / Win2000）

`src/kernel/gfx.h` 调色板：

| 名称 | 值 | 用途 |
|---|---|---|
| `C_DESKTOP_1` | RGB(192,192,192) | 桌面/窗口底色 |
| `C_PANEL` | RGB(192,192,192) | 面板与按钮填充 |
| `C_PANEL_DARK` | RGB(128,128,128) | 3D 暗边 |
| `C_WHITE` | RGB(255,255,255) | 3D 高光/输入框 |
| `C_TITLE` → `C_TITLE_2` | RGB(0,0,128) → RGB(16,132,208) | 活动标题栏渐变 |
| `C_ACCENT` / `C_SELECT` | RGB(10,36,106) | 选中/焦点 |
| `C_TEXT` | RGB(0,0,0) | 正文 |

`gfx_round_rect()` 保留原签名但内部改为**方角 3D 凸起**（白上/左、灰下/右），
因此所有窗口、按钮、面板一次性切换为经典外观；新增 `gfx_bevel_up/gfx_bevel_down/gfx_vline`。
输入框改为内凹（`gfx_bevel_down`）。

## 3. 终端（Module 3）

* 背景 RGB(12,12,12)；30px 深灰 Tab 栏 "UMDos Terminal"，右上角扁平 `— □ ×`；
* 新增 `term_print_color(str, rgb)`；Prompt = 绿色 `PS ` + 蓝色 `C:\UMDos\User` + 白色 `> `；
* 光标为**实心浅灰方块**，按 PIT tick 闪烁（50 tick 亮 / 50 tick 灭）；
* 逐字符增量绘制（不整屏重绘），串口镜像保留供自动测试断言。

## 4. 文案清理

删除/替换的“施工现场”文案：

* 终端横幅的 `(P11)`；
* `/proc/version` 的 `P10`；
* 安装引导的 `dev.1`、`实验`、`只支持…留到后续版本` 等长句；
* 桌面图标下的 `double click` 小字；
* 文件管理器工具栏/状态栏的 `T_TOTAL/T_PIECES` 拼接改为 `N items`。

不支持的项（回收站）直接显示为普通图标且不响应点击，不加长句解释。

## 5. 验证

```sh
python tools/test_chain.py      # 启动链 + 登录 + 桌面
python tools/test_p11p12.py     # 终端命令 / 锁屏 / 限速 / 权限
python tools/emu_ui.py          # 桌面 / 文件管理器 / 多窗口 / 拖动
python tools/test_persist.py    # 写回磁盘
python tools/test_installer.py  # 安装引导（英文界面 + 功能）
```

全部通过；截图见 `shots/ui-login.png`、`ui-desktop.png`、`ui-terminal.png`、`ui-fm.png`。

## 6. 已知限制

1. 源码注释仍为中文（不影响 UI 与字库；UI 字符串与字库已 100% ASCII）。
2. 桌面只有 3 个图标（This PC / Terminal / Recycle Bin），回收站无功能。
3. 终端仍无滚动缓冲（原有限制，未改变）。
4. 安装引导图标为 16×16 单色 mask（规格允许的回退形式），未改成矢量绘制。
