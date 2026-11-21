# UMDos 启动映射（P2 / v2.6.112-dev.2）

## 1. 四级链

| 级 | 源文件 | LBA | 装入地址 | 大小 | 职责 |
|---|---|---|---|---|---|
| boot | `src/boot.asm` | 0 | `0x7C00` | 512 B | 只做两件事：从 LBA 1 读 loader 到 `0x7E00`，然后跳过去。不再有输入、分区表、内核镜像加载。 |
| loader | `src/loader.asm` | 1–8 | `0x7E00` | 4 KB | A20（快端口 + 自检）、VBE 选模式、读 MBR 分区项（只读）、把内核/文件系统/head 读进内存、建 GDT、进 32 位保护模式。 |
| head | `src/head.asm` | 9–12 | `0x9000` | 2 KB | 建 4GB 恒等映射页表（4 级页表 + 2MB 大页），开 PAE/LME/PG 进 64 位长模式，跳到内核入口 `0x10000`。 |
| kernel | `src/kernel/*` | 50–241 | `0x10000` | 192 扇区（96 KB 窗口） | 统一初始化：framebuffer、输入、UMDFS、ATA、认证、图形界面。 |

内核镜像装到 `0x10000`，装入窗口 96 KB（`0x10000`–`0x27FFF`），覆盖当前 `.bss` 末尾
`0x23170`；UMDFS 镜像装到 `0x40000`（256 扇区 = 128 KB，`0x40000`–`0x5FFFF`）。

## 2. 物理内存布局

| 范围 | 用途 | 写入者 |
|---|---|---|
| `0x0500` / `0x0510` | A20 自检用的两个字节（用完恢复） | loader |
| `0x1000`–`0x6FFF` | 页表：PML4 / PDPT / 4 个 PD | head |
| `0x7000`–`0x705F` | bootinfo（framebuffer、镜像 LBA、分区表、版本等） | loader |
| `0x7200`–`0x73FF` | MBR 扇区缓冲（读 LBA 0） | loader |
| `0x7C00`–`0x7DFF` | boot 扇区 | boot |
| `0x7E00`–`0x8DFF` | loader | boot |
| `0x9000`–`0x97FF` | head（32 位 + 64 位跳板） | loader |
| `0x10000`–`0x27FFF` | 内核镜像 + .bss 窗口 | loader |
| `0x40000`–`0x5FFFF` | UMDFS 镜像（128 KB） | loader |
| `0x90000` 起向下 | 栈（16/32/64 位共用栈顶） | 各级 |

## 3. 阶段串口日志

所有阶段都向 COM1（`0x3F8`，115200 8N1）输出一行，格式固定，便于定位挂起位置：

```
[boot] stage1 ok, jump loader
[loader] vbe ok, A20 ok
[loader] images loaded, enter protected mode
[head] paging ok, enter long mode          （由 head 在长模式下补齐，见下方说明）
long mode ok, jump kernel
[kernel] init: gfx/input ok
[kernel] splash start
[kernel] login ready (auth: /config/auth)
[kernel] login ok, desktop
```

`[head] ` 前缀由 32 位代码先输出，随后 64 位代码输出 `long mode ok, jump kernel`。
`tools/test_chain.py` 会把串口内容全部比对一遍。

## 4. 启动介质

| 介质 | 读盘方式 | 状态 |
|---|---|---|
| 1.44MB 软盘镜像 | CHS（BIOS 不支持 EDD 时自动回退） | ✅ 通过 `tools/test_chain.py` |
| 16MB 硬盘镜像（MBR 分区） | EDD（`INT 13h AH=42h`） | ✅ |
| El Torito 光盘（软盘仿真） | EDD，只读 | ✅ |

EDD 失败时统一回退 CHS；几何参数由每个读盘阶段自己用 `AH=08h` 探测一次
（BIOS 不提供跨阶段保留，属于必要的一次探测）。

## 5. P2 删除了什么

| 位置 | 旧职责 | 现在 |
|---|---|---|
| boot | 安装引导提示、用户名/密码收集、激活码、读 LBA6/7、加载 loader/head | 只读 loader 并跳转 |
| loader | 激活码校验、SHA-256 计算、写凭据块 `0x0700` | A20 / VBE / 读镜像 / GDT / 保护模式 |
| head | VGA 320×200 图形登录、键盘鼠标、加载 hello | 页表 + 长模式 + 跳内核 |
| hello | 进度条 + 4 个演示任务 | 删除（任务并入内核启动画面） |
| firstmain | VBE、MBR、加载内核/FS、页表、长模式 | VBE/加载移到 loader，页表/长模式移到 head |
| nextmain | 64 位跳板 | 删除（head 直接跳 `0x10000`） |

旧文件保留在 `src/legacy/`（boot/loader/head/hello），`src/firstmain.asm` 与
`src/nextmain.asm` 原样保留——dev.1 的安装引导 `tools/build_installer.py` 仍然复用它们，
因此安装引导的镜像布局不受本版本影响。

## 6. 认证变化

* 旧：loader 把用户名/密码的 SHA-256 写进物理地址 `0x0700`，head 和内核都读这个块。
* 新：**内核启动后从 UMDFS 的 `/CONFIG/AUTH` 读取认证信息**，格式
  `sha3-256$<盐>$<哈希>`（与安装引导写入的格式一致），内存里不再有 16 位凭据块。
* 默认账号（dev.2 开发镜像）：普通用户 `user` / 密码 `umdos`；管理员 `administrator` / 密码 `admin`。
  盐在构建时随机生成，写入 `fs.img`；镜像里没有明文密码。

## 7. 验证命令

```sh
python build.py                 # 生成 uMDos.img / uMDos-hdd.img / uMDos.iso
python tools/test_chain.py      # 三种介质启动 + 串口日志 + /config/auth 登录 + 错误密码
python tools/emu_ui.py          # 桌面/文件管理器/多窗口流程
python tools/test_persist.py    # 写回磁盘
python tools/test_editor.py     # 编辑器光标与行编辑
python tools/test_edit2.py      # 查找替换/复制粘贴/缩进/F2 重命名
python tools/test_installer.py  # dev.1 安装引导（仍可构建、仍通过）
```
