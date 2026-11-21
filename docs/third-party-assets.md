# 第三方资源

## 图标：microsoft/fluentui-system-icons（计划使用，dev.1 未落地）

* 仓库地址：https://github.com/microsoft/fluentui-system-icons
* 计划获取方式：`https://github.com/microsoft/fluentui-system-icons/archive/refs/heads/main.zip`
  （构建脚本不依赖 git；允许资源缺失时联网获取）
* 许可：MIT（仓库根目录 LICENSE）
* 计划存放位置：`assets/icons/fluentui-system-icons/`，转换结果放 `assets/icons/generated/`

### dev.1 状态：使用内置占位图标（fallback）

dev.1 构建环境**无法联网**（尝试从 `raw.githubusercontent.com` 获取 LICENSE 时连接超时），
同时本机没有可用的 SVG 光栅化工具，无法把 SVG 转成 UMDos 可绘制的位图。
按规格允许的回退方案，安装引导使用**内置占位图标**：

| 文件 | 说明 |
|---|---|
| `assets/icons/generated/placeholder_icons.h` | 9 个 16×16 1bpp 占位图标，由 `tools/gen_placeholder_icons.py` 用 Pillow 绘制 |
| `installer/src/main.c` | `draw_icon()` 把占位图标画在安装向导各页的标题栏右侧 |

占位图标与规格要求的对应关系：

| 规格要求 | 占位图标 | 使用位置 |
|---|---|---|
| 磁盘选择 | `ICON_DISK` | 选择目标磁盘页 |
| 分区 | `ICON_PARTITION` | 分区与格式化页 |
| 格式化 | `ICON_FORMAT` | 文件系统页 |
| 安装地址 | `ICON_PATH` | 安装地址页 |
| 用户密码 | `ICON_USER` / `ICON_LOCK` | 账户设置页 |
| 风险提示 | `ICON_WARN` | 风险提示页 |
| agree / disagree | `ICON_CHECK` / `ICON_CROSS` | 写入结果页 |

### 后续计划

* P9–P10（图形合成与桌面 Shell）阶段联网获取 fluentui 图标子集，写转换脚本
  `tools/svg2umd.py`，生成 `assets/icons/generated/*.h`，并把安装引导的占位图标替换为正式图标。
* 届时同步保留仓库 LICENSE、下载日期、ZIP 文件名与提交信息到本文件。

## 其他第三方组件

| 组件 | 用途 | 许可 |
|---|---|---|
| NASM | 汇编器（构建工具，不随镜像分发） | BSD-2-Clause |
| Zig / clang | C 编译器（构建工具） | MIT / Apache-2.0 |
| pycdlib | 生成 ISO9660/El Torito 镜像（构建工具） | LGPL-2.1 |
| Pillow | 生成中文字库点阵与占位图标（构建工具） | HPND |
| Unicorn | 模拟器测试（构建工具） | GPL-2.0 |
| 微软雅黑（msyh.ttc） | 中文点阵烘焙来源，仅在本机构建时读取字形，不分发字体文件 | 系统字体，未随镜像分发 |
