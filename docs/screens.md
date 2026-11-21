# 启动画面与登录/锁屏重做（v2.6.112-dev.12）

参考：Windows 2000 Professional 启动画面 + Windows Server 2012 登录/锁屏。
只改渲染层；登录/锁定/认证逻辑（含限速、角色、auth_check）未变。

## 1. 启动画面（splash）

| 元素 | 实现 |
|---|---|
| 背景 | 浅灰白垂直渐变 `#F8F9FB → #E4E9F1`（Win2000 白底风格） |
| Logo | 白卡片 + 四色方块（红/绿/蓝/黄） |
| 产品字 | `UMDos`（scale 7）+ `2.6 Professional`（scale 3）+ `Built on UMDos Technology` |
| 进度 | 底部步骤文字（4 段）+ 全宽 3D 进度条 + 百分比 |
| 底条 | 蓝色渐变 42px，`Starting up...` 白字（Win2000 启动条） |

## 2. 登录页（Windows Server 2012 风格）

| 元素 | 实现 |
|---|---|
| 背景 | 深海军蓝渐变 `#0B1730 → #182C4E` |
| 头像 | 深灰卡片 + 白色人像（头/肩，圆与椭圆用整数扫描线绘制） |
| 账号 | `User name` 标签 + 白色输入框（聚焦蓝色边 + 光标） |
| 密码 | `Password` 标签 + 白色输入框（掩码 `*`） |
| 登录 | 蓝色方块箭头按钮（像素线绘制箭头）；键盘 Enter 仍可登录 |
| 其他 | 底部 `UMDos 2.6`、左下角电源图标、错误提示红字 |

## 3. 锁屏（Windows Server 2012 风格）

| 元素 | 实现 |
|---|---|
| 背景 | 深海军蓝渐变 |
| 时钟 | `HH:MM`（scale 7，白色） |
| 日期 | 月份名 + 日（CMOS，异常值兜底） |
| 状态 | `UMDos 2.6 - Locked` |
| 密码框 | 深色输入框 + 白色掩码；错误显示 `Wrong password` |
| 其他 | 顶部 `Press Enter to unlock`、右下角电源图标 |

## 4. 改动文件

| 文件 | 变化 |
|---|---|
| `src/kernel/ui.c` | 重写 `splash_static`/`splash_panel`、`draw_login`、`ui_lock_screen`；新增 `fill_ellipse/fill_circle`；登录鼠标命中区更新 |
| `docs/screens.md` | 本说明 |

未改动：桌面/窗口/终端/文件系统/认证逻辑，内核其余部分。

## 5. 验证

```sh
python tools/test_chain.py     # 启动链/登录/桌面
python tools/test_p11p12.py    # 登录失败限速/锁屏
```

全部通过。

## 6. 已知限制

1. 头像为几何近似人像（非位图素材）。
2. 日期在模拟器 CMOS 缺省时兜底为 `November 1`，真实机器/QEMU 显示实际日期。
3. 字体仍为 8×16 点阵，大字号（scale 7）呈块状像素风。
