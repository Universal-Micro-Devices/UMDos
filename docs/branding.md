# 官方 Logo 集成（v2.6.112-dev.13）

用户提供的官方图标（浅蓝 + 深蓝折叠形状 + 白色缺口）已作为启动画面和登录页的品牌标识。

## 1. 素材与转换

| 项 | 说明 |
|---|---|
| 原始素材 | `assets/logo/umd_logo.png`（1261×802，官方 logo） |
| 转换脚本 | `tools/gen_umdlogo.py`（PIL 裁剪图标 → 28×28 最近邻缩放 → 三色量化 → 2bpp 打包） |
| 生成头文件 | `src/kernel/umdlogo.h`（28×28、196 字节） |
| 颜色 | 0=透明，1=浅蓝 `RGB(99,170,255)`，2=深蓝 `RGB(25,16,41)` |

图标 bbox 自动检测：扫描左半幅列占用，遇到首个 ≥10px 空白即判定图标结束（避免把 "UMD" 文字裁进来）。

## 2. 渲染

`src/kernel/ui.c` 新增 `draw_umd_logo(x, y, scale)`：逐行解 2bpp，按同色连续段用 `gfx_rect` 绘制（无逐像素开销）。

| 位置 | 尺寸 |
|---|---|
| 启动画面 Logo | 112×112（scale 4） |
| 登录页底部标识 | 28×28（scale 1）+ `UMDos 2.6` |

## 3. 启动画面调整

* 移除四色方块（Windows 旗），替换为官方 logo；
* `UMDos` 字标颜色改为深蓝 `RGB(25,16,41)`，与 logo 一致；
* 其余（浅色渐变、`2.6 Professional`、`Built on UMDos Technology`、底部 `Starting up...` 蓝条与进度条）保持。

## 4. 验证

```sh
python tools/test_chain.py
python tools/test_p11p12.py
python tools/test_p15p16.py
```

全部通过。

## 5. 已知限制

1. 28×28 缩放为 112×112 后边缘呈像素阶梯（与 OS 的 8×16 点阵风格一致）。
2. 素材来自用户提供的 PNG；如后续提供矢量版可换成多边形绘制。
