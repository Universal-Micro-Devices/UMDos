# UMDos 分阶段路线图（P1–P22）

> 配套文档：`docs/audit.md`（P1 审计报告）。本路线图把规格中的 A 部分（P1–P12）、B 部分（P13–P17、P19–P22，**P18 不做**）拆成可验收的版本组，并给出每个 P 的目标、产出物、依赖、验收标准与对应版本号。

## 1. 版本节奏（硬约束）

| 版本目录 | 覆盖任务 | 主题 | 结束动作 |
|---|---|---|---|
| `releases/v2.6.112-dev.1/` | P1 | 架构审计 + 安装引导骨架 | 交付后**暂停**，等待用户输入"开工" |
| `releases/v2.6.112-dev.2/` | P2 | 启动链职责清理 | 暂停 |
| `releases/v2.6.112-dev.3/` | P3–P4 | IDT/异常/中断 + 物理内存/分页/堆 | 暂停 |
| `releases/v2.6.112-dev.4/` | P5–P6 | 多任务/系统调用/用户态 + ELF64/libc | 暂停 |
| `releases/v2.6.112-dev.5/` | P7–P8 | VFS/持久化 FS + 设备模型 | 暂停 |
| `releases/v2.6.112-dev.6/` | P9–P10 | 窗口系统 + 桌面 Shell | 暂停 |
| `releases/v2.6.112-dev.7/` | P11–P12 | 终端/应用 + 安全多用户（**base 候选**） | 暂停；P12 验收通过后可冻结 base |
| `releases/v2.6.112-dev.8/` | P13–P14 | UEFI 并存 + ACPI/APIC/电源 | 暂停 |
| `releases/v2.6.112-dev.9/` | P15–P16 | SMP + AHCI | 暂停 |
| `releases/v2.6.112-dev.10/` | P17 + P19 | USB HID + `/proc` | 暂停 |
| `releases/v2.6.112-dev.11/` | P20–P21 | 应用包/安装器 + 音频 | 暂停 |
| `releases/v2.6.112-dev.12/` | P22 | 调试、CI、文档（**public 候选**） | 暂停；进入最终交付 |
| `releases/v2.6.11/` | A 部分 P1–P12 | base 最终版，无 `installer/` | 永久保留 |
| `releases/v2.6.112/` | A + B（P18 除外） | public 最终版，含 `installer/` | 永久保留 |

规则（来自规格第零章）：

* 中间版本号只递增 `v2.6.112-dev.N`，**不得**占用 `v2.6.11` / `v2.6.112`；`tools/bump_version.py` 只改 dev 序号。
* 每个版本独立目录、永久保留、不覆盖/不删除/不合并；目录名就是版本号；镜像名带版本号。
* 每个中间版本目录包含：完整可构建源码快照 `src/`、该版本镜像（如已构建）、README。
* README 必须写明：版本号、包含任务、构建方法、QEMU 验收命令、变更记录、已知限制、图标来源与许可、下一步待做项；中间版本 README 还要注明"本版本为开发快照，最终拆分见 v2.6.11 / v2.6.112"。
* 每完成一个版本组立即暂停，只输出交付清单与"等待开工"；未收到"开工"不得生成下一版本任何文件。

## 2. 任务总表（每个 P 一行）

| P | 目标 | 产出物 | 依赖 | 验收标准 | 版本 |
|---|---|---|---|---|---|
| P1 | 审计现状、列出矛盾与必须修改项、给出路线图 | `docs/audit.md`、`docs/roadmap.md`；dev.1 另加 `installer/` 骨架与 `tools/build_installer.py` | 无 | 审计覆盖 6 级启动链/内存布局/中断/ATA-VBE-PS2/UMDFS/认证；≥10 条必须修改；路线图含 P1–P22 与版本映射；**不改现有内核源码**并附哈希清单 | dev.1 |
| P2 | 启动链职责单一化：引导扇区只读+跳转，loader 负责 A20/GDT/保护模式，head 进长模式，内核统一初始化 | 重构后的 `src/boot.asm`/`loader.asm`/`head.asm`/`firstmain.asm`、`docs/boot-map.md`、阶段串口日志 | P1 | Legacy 软盘/硬盘/ISO El Torito 三种路径均启动；A20/GDT 有显式日志；重复初始化清零；`test_all.py` 全过 | dev.2 |
| P3 | 64 位 IDT、0–31 异常、IRQ0/1/12/14、panic 屏、PIT tick、键盘环形缓冲 | `src/kernel/idt.c`/`isr.asm`/`pic.c`/`pit.c`/`panic.c`、异常截图 | P2 | 除零/页故障/GP/断点分别打印 CS:RIP/CR2/错误码并停在 panic（不重启）；键盘 IRQ1 入环形缓冲；PIT 1 kHz tick 可读；ATA 可切 IRQ14（保留轮询回退） | dev.3 |
| P4 | E820、物理页帧分配器、4 级页表管理、`kmalloc/kfree`、内核/帧缓冲映射策略 | `src/kernel/e820.c`/`pmm.c`/`vmm.c`/`heap.c`、内存统计输出 | P3 | 启动打印 E820 表；恒等映射前 1 MB+帧缓冲+内核区；`kmalloc` 对齐/可 free/压力测试通过；内核 `.bss` 不再越过装入区 | dev.3 |
| P5 | 内核线程、round-robin 调度、上下文切换、TSS、ring3、syscall 入口 | `sched.c`/`task.c`/`tss.c`/`syscall.c`、2 个内核线程 demo | P4 | 2+ 内核线程轮转；ring3 程序可 `write`/`exit`；非法指针触发页故障而不是静默；`getpid/sleep/brk` 可用 | dev.4 |
| P6 | ELF64 加载器（PT_LOAD/用户栈）与最小 libc | `elf.c`、`user/` 示例、`libc/` 静态库 | P5 | 用户态 ELF 被加载并输出；`malloc/free/open/read/write/close/exit` 可用；坏 ELF 被拒绝 | dev.4 |
| P7 | VFS（inode/dentry/file/ops、路径解析）与持久化根文件系统；认证迁移到 `/config/auth` | `vfs.c`、`umdfs2.c`（或 FAT32）、`/config` 初始化、旧镜像迁移 | P6 | `open/read/write/close/stat/mkdir/unlink/rename` 全通；路径解析支持相对/绝对/`..`；认证从 LBA6/7 一次性迁移；掉电至少不破坏超级块 | dev.5 |
| P8 | 设备注册表 + 统一 read/write/ioctl；块设备、framebuffer、事件设备 | `dev.c`、`blkdev.c`、`fbdev.c`、`evdev.c`、ATA PIO 封装（LBA28/48） | P7 | FS 只经块设备访问磁盘；GUI 只经 framebuffer 设备绘制；输入只经事件设备读取；LBA48 大盘可读写 | dev.5 |
| P9 | 图形合成与窗口系统：双缓冲、矩形/文字/位图、光标合成、窗口 z 序/拖动/焦点、事件队列、Alt+Tab | `compositor.c`、`window.c`、`event.c`、`wm.c` | P8 | 最多 4 窗口同时操作；拖动/置顶/关闭无撕裂；鼠标光标独立合成；Alt+Tab 切换；键盘/鼠标/定时器事件统一入队 | dev.6 |
| P10 | 桌面 Shell：5×8 图标网格、任务栏、开始菜单、状态栏（网络/状态/音量/设置/关机/重启）、设置 6 页、锁屏 | `shell.c`、`taskbar.c`、`startmenu.c`、`settings.c`、`lock.c` | P9 | 图标可双击运行/拖动/右键；任务栏 ≤4 显示图标+名称、>4 只显示图标、焦点高亮；状态栏 6 项可交互（网络显示离线占位）；设置含性能/声音/存储/显示/网络/系统信息/关于；关机/重启可用；**登录界面与桌面"关于"版本号一致** | dev.6 |
| P11 | 终端模拟器 + 命令行 shell + 常用命令；文件管理器增强 | `term.c`、`sh.c`、`fm` 增强（地址栏/多标签/复制粘贴/右键菜单） | P10 | `ls/cd/cat/echo/mkdir/rm/mv/cp/edit/reboot/ps/kill` 可用；管道/重定向可选；文件管理器支持地址栏、多标签、复制/粘贴 | dev.7 |
| P12 | SHA-3-256 + 随机盐认证、多用户与权限、登录限速、锁屏；`/config/auth` 格式 `sha3-256$salt$hash` | `auth.c`、`sha3.c`、`users.c`、`/config` 权限、限速策略 | P11 | `/config/auth` 无明文；错误密码触发限速；普通用户不可写 `/config`；用户态不能执行 I/O 指令、不能访问内核页；**P12 验收通过后才允许构建 base** | dev.7 |
| P13 | UEFI 与 Legacy 并存：`efi/` PE 入口、GOP 帧缓冲、输入过渡、同时产出两种启动路径 | `efi/`（`bootx64.efi`）、构建脚本改动、混合 ISO | P12 | QEMU/VirtualBox 开 EFI 可从同一 ISO 启动进内核；关 EFI 时 Legacy 仍可启动；GOP 下分辨率与鼠标正常；文档说明 VirtualBox 开/关 EFI 差异 | dev.8 |
| P14 | ACPI/APIC/电源：RSDP/XSDT/FADT/MADT 解析、LAPIC/IOAPIC（保留 PIC 回退）、关机/重启 | `acpi.c`、`apic.c`、`power.c` | P13 | 打印 ACPI 表关键字段；`acpi_shutdown()`/`acpi_reboot()` 可用；桌面电源按钮调用它们；PIC 回退路径仍可启动 | dev.8 |
| P15 | SMP：MADT 解析、INIT-SIPI-SIPI、每 CPU 栈与调度队列、全局锁 | `smp.c`、`percpu.c`、锁 | P14 | 2+ vCPU 被拉起；每 CPU 独立栈/队列；全局锁保护 `kmalloc`/VFS；BSP 跑桌面、AP 跑内核线程 | dev.9 |
| P16 | AHCI 驱动注册为块设备，ATA PIO 保留回退 | `ahci.c`、`pci.c`、块设备注册 | P15 | QEMU `-device ahci` 下识别 SATA 盘并读写；FS 只经块设备；无 AHCI 时回退 ATA PIO | dev.9 |
| P17 | USB HID：xHCI/EHCI 枚举、端口复位、控制传输、HID 报告解析 | `usb/`、`hid.c`、事件设备接入 | P16 | QEMU USB 键鼠可操作桌面；PS/2 保留回退；拔出/插入不崩溃 | dev.10 |
| P19 | `/proc` 伪文件系统：cpuinfo/meminfo/uptime/version/ps | `procfs.c`、`/proc` 挂载 | P17 | 终端 `cat` 可读 5 个文件；`/proc/version` 与 `VERSION` 一致；任务管理器可解析 `ps` | dev.10 |
| P20 | 最小应用包 `.umpkg`（清单/ELF/资源/图标/依赖）+ `pkg install/remove/list` + 安装器 | `pkg.c`、`installer/`、安装界面、`/apps` 布局 | P19 | `pkg install/remove/list` 可用；安装到 `/apps`；开始菜单动态读取；安装器完成磁盘选择/分区格式化/FAT32-EXT32/安装地址/user+administrator 密码/风险提示/agree-disagree | dev.11 |
| P21 | 音频：AC'97 或 Intel HDA 最小驱动、PCM 输出、`audio_write` | `audio.c`、WAV 播放、通知音 | P20 | 播放 WAV 提示音；无音频设备时静默回退、不阻塞启动 | dev.11 |
| P22 | 调试/CI/文档：串口日志、panic 截图、QEMU smoke test、CI 或 `make test`、`docs/test-matrix.md` | `.github/workflows/`（或本地脚本）、`docs/test-matrix.md`、回归归档 | P21 | 每次提交自动跑：Legacy 启动→激活→登录→桌面→打开文件管理器→新建文件→重启验证；日志/截图归档；测试矩阵文档完整 | dev.12 |

### 2.1 依赖链（后面的任务不能跳过前面的）

```text
P1 → P2 → P3 → P4 → P5 → P6 → P7 → P8 → P9 → P10 → P11 → P12
                                                   └→ P13 → P14 → P15 → P16 → P17 → P19 → P20 → P21 → P22
```

* P3/P4 是所有后续任务的地基：没有 IDT 就没有 panic，没有页帧分配器就没有用户态。
* P7/P8 必须早于 P9：窗口系统需要 VFS 的目录遍历与块设备上的应用读取。
* P13 之后 P14–P17 可按硬件可用性并行做，但 P19 依赖 P13 的启动日志与 P14 的 ACPI 信息。
* P20 的安装器复用 dev.1 的 `installer/` 骨架，但真正的分区/账号写入要等 P12 的 SHA-3 认证稳定。

## 3. dev.1 专项（P1 + 安装引导骨架）

### 3.1 交付范围

* `docs/audit.md`、`docs/roadmap.md`（本文件即其一）。
* `installer/` 独立模块 + `tools/build_installer.py`（或在 `build.py` 增加独立 installer 分支；**不得重写 `build.py` 主体、不得修改 `src/` 内核源码**）。
* installer-only 临时 ISO（如已构建）与 `releases/v2.6.112-dev.1/README.md`。
* "现有内核源码未改动"的哈希清单或 diff 清单（`docs/audit.md` 附录 A 已给出基线，可直接引用）。

### 3.2 安装器深度（截至 dev.1 的验收口径）

| 项目 | dev.1 要求 |
|---|---|
| 图形界面 | 从 `installer/boot.asm` 进入现有 FirstMain 链路，进入 64 位后显示安装 UI；界面显示版本号 |
| 磁盘选择 | 列出 ATA 设备（至少主通道主/从盘），可选目标盘 |
| 分区/格式化 | 可写 MBR 分区表与最小格式化标记；真实完整格式化可延后 |
| 分区格式 | FAT32 可实写；EXT32 显示为**实验选项**并写标记；README 必须写明实际兼容范围 |
| 安装地址 | 目标磁盘/分区 + 安装路径 |
| 账号 | 普通用户默认 `user`（可改名）+ 管理员 `administrator`；两个密码均以 SHA-3 + 盐存储 |
| 风险提示 | 驱动有限、稳定性有限、仅 ATA/AHCI 磁盘安装、USB 磁盘驱动下版本再做 |
| 同意流程 | agree / disagree；disagree 退出 |
| 写入验证 | 预置磁盘镜像 + 标记写入验证；检查目标 `/config/auth` 为 `sha3-256$salt$hash` 且不含明文 |
| 不宣称 | **不得**宣称已完成完整物理机分区安装 |
| QEMU 命令 | `qemu-img create -f raw disk.img 64M`；`qemu-system-x86_64 -m 512M -cdrom "releases/v2.6.112-dev.1/UMDos installer v2.6.112-dev.1.iso" -hda disk.img -boot d` |

### 3.3 dev.1 工作区现状（2026-11-07）

* `docs/` 在本版本前为空；本次新增两份文档。
* `installer/` 已有并行开发的骨架文件（`boot.asm`、`src/main.c`、`src/fat32.c`、`src/sha3.c` 等），但 `tools/build_installer.py`、`releases/v2.6.112-dev.1/` 尚未生成；这些属于 dev.1 剩余工作，需在交付前补齐并验收。

## 4. 最终两个版本

| 项目 | `releases/v2.6.11/`（base） | `releases/v2.6.112/`（public） |
|---|---|---|
| 任务范围 | A 部分 P1–P12 全部完成 | A 部分 + B 部分 P13–P17、P19–P22（P18 不做） |
| 安装引导 | **无** `installer/`；直接启动进系统 | **含** `installer/` 图形安装引导 |
| 账号 | 只有 `administrator`，初始密码 `258025`（只存 SHA-3+盐，不存明文） | `user`（名称可改）+ `administrator`，密码安装时设置 |
| 认证存储 | `/config/auth`，`sha3-256$salt$hash`；盐在构建时生成并写入 README 构建记录 | 同左，安装时生成 |
| 镜像 | `UMDos base v2.6.11.iso` | `UMDos public v2.6.112.iso` |
| 启动方式 | Legacy + UEFI 混合 ISO | Legacy + UEFI 混合 ISO |
| 构建时机 | P12 认证验收通过后具备条件；最终交付阶段生成 | P13–P17、P19–P22 全部完成后生成 |
| 目录内容 | 独立 `src/` 快照 + ISO + README | 独立 `src/` 快照 + ISO + README |

独立快照规则：

* 两个最终目录各自保留**完整源码**，不共享源码树；`VERSION` 单一来源，构建时生成 `version.h` 并注入内核与用户态。
* 拆分 base 时必须删除 `installer/`，并在构建前检查：**base 快照检测到 `installer/` 则构建失败**。
* 五处版本一致（最终 v2.6.112 合并验收）：镜像名、启动日志、登录界面、桌面"关于"、`/proc/version`。
  分阶段验收：dev.1 验镜像名含版本 + 安装界面显示版本 + `VERSION` 唯一；P10 后验登录界面与"关于"；P19 后验 `/proc/version`。
* base 的盐在构建时生成并写入 README 的构建记录，不写明文密码。

### 4.1 版本注入与"五处一致"落点

| 位置 | 注入方式 | 验收阶段 |
|---|---|---|
| `VERSION` 文件 | 快照根目录唯一版本源；`build.py` 读取后生成 `src/kernel/version.h` | dev.1（唯一性） |
| 镜像名 | 构建脚本用 `VERSION` 拼 `UMDos base/public vX.Y.Z.iso`，禁止手写 | dev.1（镜像名含版本） |
| 启动日志 | 内核启动早期打印 `UMDos <version>` 到串口/`0xE9` | dev.3（有日志后） |
| 登录界面 | `ui.c` 的硬编码 `UMDos 1.0` 改为 `VERSION` 宏 | P10 验收 |
| 桌面"关于" | 设置页"关于"显示 `version.h` 内容 | P10 验收 |
| `/proc/version` | procfs 读取同一宏 | P19 验收 |

`tools/bump_version.py` 只做一件事：把 `v2.6.112-dev.N` 的 N 加一；最终交付由发布脚本或手工把 `VERSION` 设为 `v2.6.11` / `v2.6.112`，不允许用 bump 脚本跨越最终版本号。

### 4.2 最终交付检查单

| # | 检查项 | 失败处理 |
|---|---|---|
| 1 | `releases/v2.6.11/` 与 `releases/v2.6.112/` 均存在且各自含完整 `src/` | 不交付 |
| 2 | base 快照中**不存在** `installer/`；构建前检查命中则失败 | 构建失败 |
| 3 | 两个 ISO 均为 Legacy + UEFI 混合镜像 | 不交付 |
| 4 | 五处版本一致（镜像名/启动日志/登录界面/关于/`/proc/version`） | 不交付 |
| 5 | base 只有 `administrator`（密码仅存 SHA-3+盐），public 有 `user`+`administrator` | 不交付 |
| 6 | `/config/auth` 无明文，格式为 `sha3-256$salt$hash` | 不交付 |
| 7 | 中间版本目录全部保留、未被覆盖；交付清单列出新旧目录 | 补齐清单 |
| 8 | 每版本 README 含版本号/任务/构建/QEMU/变更/限制/图标许可/下一步 | 补齐 README |
| 9 | 图标来源、许可与占位回退可离线复现（不依赖 git） | 使用占位图标并标注 |
| 10 | 回归全绿：启动链、持久化、编辑器、ISO、installer 流程 | 修复后重跑 |

## 5. 里程碑

| 里程碑 | 版本 | 判定标准 |
|---|---|---|
| M1 可审计、可安装演示 | dev.1 | P1 文档交付；installer-only ISO 可在 QEMU 走完安装流程到 agree；源码未改哈希清单齐全 |
| M2 稳定的内核基础 | dev.3 | 异常有 panic 不重启；E820/堆可用；三路径启动回归通过 |
| M3 能跑用户程序 | dev.4 | ring3 + syscall + ELF 示例程序运行 |
| M4 A 部分完成 / base 候选 | dev.7 | VFS/设备模型/窗口/桌面/终端/SHA-3 全部验收；base 可冻结 |
| M5 B 部分硬件扩展 | dev.10 | UEFI 混合 ISO、ACPI 电源、SMP、AHCI、USB HID、`/proc` 验收 |
| M6 最终交付 | v2.6.11 / v2.6.112 | 两个独立快照构建成功；五处版本一致；CI 回归全绿 |

## 6. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 一次性重写启动链/构建系统 | 启动全挂、回归不可比 | 按 P2 小步改，每步跑 `test_all.py` 三路径；构建系统只增量修改 |
| 中文化/字库依赖缺失（`C:\Windows\Fonts\msyh.ttc`） | 构建失败或中文缺字 | 已生成的头文件入库；构建脚本提供英文/占位回退并在 README 标注 |
| P4 页表/堆改动导致帧缓冲不可写 | 黑屏，难定位 | 先保留恒等映射回退路径；帧缓冲始终恒等映射；panic 打印 CR2 |
| P5/P6 用户态隔离破坏 GUI | 桌面起不来 | 分两步：先内核线程，再 ring3；GUI 仍跑内核线程直到 P9 稳定 |
| P13 UEFI 与 Legacy 双路径分叉 | 业务逻辑被复制 | 内核初始化函数共用；EFI 只负责取 GOP/加载内核/传 bootinfo |
| P15/P16/P17 无真实硬件 | 验收不可信 | 统一用 QEMU（`-smp`、`-device ahci`、`-device usb-kbd/usb-mouse`）+ 直通硬件抽查 |
| P12 认证改造迁移旧镜像失败 | 老镜像登录不了 | 保留一次性 LBA6/7 → `/config/auth` 迁移并记录日志；失败回退到旧校验并提示 |
| 版本/文档漂移 | 五处版本不一致 | 单一 `VERSION` + 构建注入；每版本 README 更新；历史报告标注"历史快照" |
| 每个版本都暂停导致节奏碎片化 | 返工 | 每个版本组结束前完成该组回归并更新 `docs/test-matrix.md` |

### 6.1 每版本组的回归增量

| 版本组 | 新增回归项 | 必须保持不变的旧行为 |
|---|---|---|
| dev.1 | installer 全流程、`/config/auth` 格式、哈希清单 | 现有 6 级启动演示全部产物的字节大小与 `bin/*.lst` 校验 |
| dev.2 | 三路径启动（软盘 CHS/硬盘 EDD/ISO）、A20/GDT 日志 | 激活码、图形登录、splash、桌面、文件管理器 |
| dev.3 | 异常向量逐个触发、panic 输出、PIT tick 计数 | 键盘/鼠标/ATA 轮询回退、三路径启动 |
| dev.4 | 内核线程轮转、syscall 表、ELF 示例、坏 ELF 拒绝 | 桌面 GUI 不回归 |
| dev.5 | VFS API、目录遍历、`/config` 迁移、断电一致性抽样 | 编辑器保存/删除/重命名落盘 |
| dev.6 | 4 窗口、拖动/置顶、Alt+Tab、设置 6 页、锁屏 | 文件管理器与编辑器全部快捷键 |
| dev.7 | 限速、权限拒绝、用户态 I/O 与内核页访问被拒、SHA-3 测试向量 | 登录/桌面/文件系统 |
| dev.8 | UEFI 开/关启动、GOP 分辨率、ACPI 关机/重启 | Legacy 路径、PIC 回退 |
| dev.9 | `-smp 2` AP 启动、全局锁压力、AHCI 读写 | ATA PIO 回退 |
| dev.10 | USB 键鼠插拔、`/proc` 5 文件内容 | PS/2 回退 |
| dev.11 | `.umpkg` 安装/卸载/列表、开始菜单动态项、WAV 播放/静默回退 | 桌面与文件系统 |
| dev.12 | CI 全链路、日志/截图归档、`docs/test-matrix.md` | 以上全部 |

## 7. 回归与验收基线（每个版本都要跑）

```sh
python build.py                 # 6 级 + 内核 + FS + img/hdd/iso
python tools/test_all.py        # 6 场景：软盘CHS/硬盘EDD/只读CD/脏ES/错激活码/错密码
python tools/test_persist.py    # 写回落盘 + 重启后仍在
python tools/test_editor.py     # 编辑器光标/行编辑/保存
python tools/test_edit2.py      # 查找替换/选区/缩进/重命名
python tools/test_mouse.py      # head 阶段 PS/2 鼠标
python tools/verify_iso.py      # ISO9660 + El Torito 结构
python tools/emu_ui.py          # 64 位启动画面→登录→桌面→文件管理器截图
```

版本级追加项：

* dev.1：installer QEMU 流程 + `/config/auth` 格式检查 + 哈希清单。
* dev.3 起：异常/panic 用例、`/proc` 后续接入、中断统计。
* dev.7 起：`/config` 权限、限速、用户态隔离用例。
* dev.8 起：UEFI 开/关两种启动、关机/重启。
* dev.10 起：SMP `-smp 2`、AHCI、USB 键鼠。
* dev.12：CI 或 `make test` 全量跑通并归档日志/截图。

## 8. 明确不做 / 不宣称

* **P18 网络栈不做**：状态栏"网络"只显示占位/离线；不实现 TCP/IP、不联网。
* dev.1 不宣称完成物理机分区安装；EXT32 只作为实验选项并写标记。
* base 不含 `installer/`；public 才含安装引导。
* 不提前产出最终 base（安装引导版本只验证安装流程）。
* 没有通过验收的功能不得在 README 中写"已完成"；未实现项统一写"当前未实现"。

## 附录 A：审计缺陷 → P 任务对照

| `docs/audit.md` 条目 | 缺陷摘要 | 处理任务 |
|---|---|---|
| §7.1 #1 | 认证用 SHA-256、无盐 | P12（存储迁移在 P7） |
| §7.1 #2 | 凭据在 LBA6/7 与 `0x0700` | P7、P12 |
| §7.1 #3 | 无 IDT、无异常处理 | P3 |
| §7.1 #4 | 内核 IF=0、PIC/PIT 未初始化 | P3、P14 |
| §7.1 #5 | 键盘/鼠标轮询、依赖 head 遗留状态 | P3、P8 |
| §7.1 #6 | 无 E820/页帧分配/`kmalloc` | P4 |
| §7.1 #7 | 无用户态/进程/syscall | P5 |
| §7.1 #8 | 无 ELF64/用户 libc | P6 |
| §7.1 #9 | 无 VFS、全局 `cwd` | P7 |
| §7.1 #10 | 无设备模型、`fs.c` 直连 `ata.c` | P8 |
| §7.1 #11 | 无窗口系统/事件队列/双缓冲 | P9 |
| §7.1 #12 | 桌面 Shell 缺开始菜单/状态栏/设置 | P10 |
| §7.1 #13 | 无终端/Shell/命令 | P11 |
| §7.1 #14 | 无 UEFI | P13 |
| §7.1 #15 | 无 ACPI/APIC/SMP | P14、P15 |
| §7.1 #16 | ATA 仅主通道主盘 LBA28 PIO | P8、P16 |
| §7.1 #17 | 无 USB HID | P17 |
| §7.1 #18 | 无 `/proc` | P19 |
| §7.1 #19 | 无应用包/安装器接入构建 | P20、dev.1 |
| §7.1 #20 | 版本号未注入 | dev.1（`VERSION`/`version.h`）、P22 |
| §7.2 A | splash 用 RTC 秒回绕计时 | P3（tick）、P10 |
| §7.2 B | "校验内核镜像"是无效取样和 | P1 文案、P10 |
| §7.2 C | 内核忽略 `bootinfo.magic/flags` | P2、P4 |
| §7.2 D | 编辑器 4 KB 静默截断 | P7、P11 |
| §7.2 E | 桌面空闲忙等（`for(;;)`） | P3、P9 |
| §7.2 F | 单次 EDD 读 128 扇区 | P2 |
| §7.3 #1–#12 | 现有文档 12 条矛盾 | 每个版本更新 README/使用说明；历史报告标注"历史快照" |

## 附录 B：与现有文档的关系

* `README.md`、`使用说明.md` 继续作为用户/构建入口；每个版本组结束前同步更新（尤其是"已知限制"一节）。
* `代码审查报告.md`、`修复说明.md` 是**历史快照**，建议在各文件顶部加"历史版本，当前实现见 docs/audit.md"的提示，避免与现状混淆。
* 本路线图与 `docs/audit.md` 是后续所有版本的验收依据；若实现与文档冲突，以"代码 + 回归结果"为准，并先修文档再继续。

