# UMDos P1 架构审计报告

| 项目 | 内容 |
|---|---|
| 文档版本 | v2.6.112-dev.1（P1 交付物） |
| 审计对象 | `src/`（6 级 16 位启动链 + 64 位 C 内核）、`build.py`、`fsgen.py`、`bin/` 产物、`tools/` 测试脚本、现有 4 份中文文档 |
| 审计方式 | 只读源码与二进制；用 `grep` 定位中断/端口/哈希；解析 `bin/kernel.elf` 程序头；用 SHA-256 复算激活码常量；阅读 Unicorn 测试脚本确认覆盖范围 |
| 审计日期 | 2026-11-07 |
| 源码改动 | 无。本次只新增 `docs/audit.md` 与 `docs/roadmap.md`，未修改任何现有文件 |
| 一句话结论 | 当前实现是"6 级启动链 + 图形登录演示 + 可读写的小文件系统"；规格 P1–P22 要求的 IDT、E820、分页堆、多任务、ELF、VFS、设备模型、终端、SHA-3、UEFI、ACPI、SMP、AHCI、USB HID、`/proc`、应用包、音频、CI 均未实现（P9/P10/P22 只有雏形） |

> 诚实声明：本文只描述"代码现在做什么"。凡标"当前未实现"的条目，都是在 `src/` 中找不到对应实现，而不是"尚未验证"。
> 现有 `releases/` 目录为空；`installer/` 目录中已存在部分并行开发的骨架文件（截至本次审计时点，`tools/build_installer.py`、`releases/v2.6.112-dev.1/` 尚未生成），不属于 P1 审计范围。

## 0. 结论摘要（先看这 9 条）

1. **认证是 SHA-256，不是规格要求的 SHA-3-256**：16 位实现在 `src/inc/sha256.inc`，64 位实现在 `src/kernel/sha256.c`；无盐、单轮、无迭代。
2. **凭据有两份副本**：物理内存 `0x0700`（`[0]=0x5A` + 两个 32 字节哈希）与磁盘 `LBA6/LBA7`（`0xAA`/`0x55` + 哈希）；不存在 `/config/auth`。
3. **全工程没有 IDT**：`src/` 内没有 `lidt` 指令；16 位阶段使用实模式 IVT，且只有 `head.asm` 安装了 IRQ12 一个向量。
4. **64 位内核自始至终关中断**：`start.asm` 第一条指令是 `cli`，从 `kmain` 到桌面死循环之间没有任何 `sti`；PIC/PIT 从未编程。
5. **键盘、鼠标、ATA 全部轮询**：内核 `input.c` 轮询 8042（`0x64/0x60`），`ata.c` 轮询 `0x1F7` 状态位，没有 IRQ1/IRQ12/IRQ14 处理。
6. **ATA 只覆盖主通道主盘**：固定 `0x1F0`、`0xA0`、LBA28 PIO；无从盘、无次通道、无 LBA48、无 DMA、无 AHCI、无 ATAPI。
7. **UMDFS 是自定义 128KB 镜像**：超级块 40 字节 + 64 字节目录项（含 `parent`）+ 块位图 + 数据区；`fs.c` 与 `fsgen.py` 的字段偏移逐项一致（见第 5 节核对表）。
8. **内存管理只到"静态页表"**：`firstmain` 建 4GB 恒等映射（2MB 大页）后进长模式；没有 E820、页帧分配器、`kmalloc`，也没有用户地址空间。
9. **版本号没有注入**：没有 `VERSION`、没有 `version.h`、镜像名不带版本；界面里硬编码 `UMDos 1.0`（`ui.c:287`）与 `UMDos-DOS1 beta`（`head.asm:648`）。

## 1. 启动链职责清单

### 1.1 六级职责（LBA / 加载地址 / 大小 / 失败行为）

| 级 | 源文件 | LBA | 扇区 / 大小 | 加载地址 | 入口后的职责 | 失败行为 |
|---|---|---|---|---|---|---|
| 1 boot | `src/boot.asm` | 0 | 1 / 512 B | `0x7C00` | `cli`+段清零+`sp=0x7C00`；读 LBA6 判断首字节 `0xAA`；未激活则 `int 16h` 采集用户名/密码到 `0x0500`/`0x0520`；读 LBA1–5 到 `0x7E00` 后 `jmp 0x0000:0x7E00` | 任一读失败 → `Disk error!` + `cli;hlt` 死循环，无重试、无磁盘复位（`boot.asm:58-64`） |
| 2 loader | `src/loader.asm` | 1–5 | 5 / 2560 B | `0x7E00` | `disk_init` 探测几何；读 LBA6；已激活则把用户名哈希拷到 `0x0700` 并尝试读 LBA7 取密码哈希；未激活则读激活码、SHA-256 与内置常量比较、计算用户名/密码哈希写入 `0x0700`、`save_creds` 尽力写回 LBA6/7；读 LBA8–17 到 `0x8800` 后跳转 | 磁盘错误 → `disk_error` 死循环；激活码错误/过长 → 打印诊断后 `cli;hlt`；写回失败被忽略（只读介质可继续） |
| 3 head | `src/head.asm` | 8–17 | 10 / 5120 B | `0x8800` | 切 VGA `320x200x256`；画图形登录界面；初始化可选 PS/2 鼠标（失败降级键盘）；`int 16h` 轮询键盘；用 `0x0700` 的 SHA-256 校验用户名/密码；成功后读 LBA18–29（12 扇区）到 `0xA000` 并跳转 | 登录失败重试并清空密码框（`head.asm:196-211`）；`0x0700` 无 `0x5A` 标志提示 `No credentials`；磁盘错误 → 死循环 |
| 4 hello | `src/hello.asm` | 18–29 | 12 / 6144 B | `0xA000` | 切 VGA、画进度界面；四项任务：PXE 存在性（`int 1Ah AH=56h`）、CPUID 厂商串、CMOS RTC 时分秒、色块；`int 15h AH=86h` 延时；读 LBA30–45（16 扇区）到 `0xC000` 并跳转 | 磁盘错误 → 死循环；无 PXE → 显示 `offline mode`（不算失败）；VGA 无法检测 |
| 5 firstmain | `src/firstmain.asm` | 30–45 | 16 / 8192 B | `0xC000` | VBE 扫描并切换 32bpp 线性帧缓冲（优先 1280×720）；读 MBR 到 `0x7200` 并解析 4 个分区项；读内核 LBA50 共 128 扇区到 `0x10000`；读 FS 两段各 128 扇区到 `0x30000`/`0x40000`；填 bootinfo `0x7000`；建 4GB 恒等映射页表 `0x1000–0x6FFF`；开 PAE/LME/PG 进长模式；`jmp 0x08:long64` | 无合适 VBE 模式 → 打印 `VBE: no suitable...` 后 `cli;hlt`；CPU 无长模式 → 打印 `CPU does not support long mode!` 后停机；磁盘错误 → 死循环 |
| 6 nextmain | `src/nextmain.asm` | 46–49 | 4 / 2048 B | `0xE000` | 64 位跳板：`rdi=0x7000`、`rsp=0x90000`、`jmp 0x10000` | 无任何校验、无失败路径（`nextmain.asm:9-11`） |
| 7 内核 | `src/kernel/` | 50–177 | 128 / 64 KB | `0x10000` | `_start`：`cli`、开 SSE、清 `.bss`、`rsp=0x90000`、`kmain`；`kmain`：`gfx_init`→`input_init`→`ui_splash`（60 秒）→`ui_login`→`ui_desktop` | 无异常处理，任何异常都是三重故障重启；FS 挂载失败不报错，桌面显示空列表 |

加载顺序与跳转：`build.py:17-24` 的 `STAGES` 表与上表逐项一致；`build_bins` 还校验每个阶段产物字节数必须等于 `扇区数×512`（`build.py:56-61`）。

### 1.2 跨级交接约定

| 交接物 | 约定 | 证据 |
|---|---|---|
| 启动盘号 | 每级 `mov [drive_num], dl`，跳转前 `mov dl,[drive_num]` 传给下一级 | `boot.asm:22,55`、`loader.asm:30,166`、`head.asm:36,193`、`hello.asm:21,60`、`firstmain.asm:31` |
| 凭据块 | `0x0700`：`[0]=0x5A`、`[1..32]` 用户名 SHA-256、`[33..64]` 密码 SHA-256 | `loader.asm:8,45-64,121`、`head.asm:12,145-180`、`ui.c:264-274` |
| bootinfo | `0x7000`，100 字节；前 32 字节复用 VBE 信息块布局，magic `"UMD1"` 在 +32 | `firstmain.asm:11-12,116-123`、`bootinfo.h:14-35` |
| 内核入口 | `RDI = 0x7000`、`RSP = 0x90000`、`jmp 0x10000` | `nextmain.asm:9-11`、`start.asm:31-32` |

### 1.3 结构性问题（不是"能跑就行"）

* **每级都信任下一级**：没有长度/签名/magic 校验。`nextmain` 直接 `jmp 0x10000`，不检查 `bootinfo.magic`，也不检查内核入口字节。
* **每级都重复初始化**：`cli`、段寄存器清零、设栈、`disk_init` 在 loader/head/hello/firstmain 各写一遍；boot 还自带一份与 `src/inc/disk16.inc` 重复的 `read`/`puts`/`gets`（`boot.asm:66-179`）。
* **栈顶压在上一级映像里**：`hello` 的 `sp=0x8000` 位于 loader 映像（`0x7E00–0x87FF`）内部；`firstmain` 的 `sp=0xC000` 距 hello 映像尾部（`0xB800`）只有 2 KB；`head` 的 `sp=0x8800` 压在 loader 尾部。见第 2 节。
* **一次 EDD 读 128 扇区**：`firstmain` 用单个 DAP 一次读 64 KB（`firstmain.asm:80-113`）。传统 BIOS 的 DAP 计数上限常见为 127，且传输不得跨 64 KB DMA 边界；当前只是"恰好从段偏移 0 开始、正好 64 KB"，换 BIOS 可能失败后落到逐扇区 CHS 回退（`disk16.inc:99-140`），速度极慢。
* **LBA 只有 16 位**：`read_sectors` 的 LBA 参数是 `AX`（`disk16.inc:57-58`），镜像超过 32 MB 后无法寻址。

## 2. 物理内存布局表

| 地址范围 | 大小 | 用途 | 证据（文件:行） | 备注 |
|---|---|---|---|---|
| `0x0500–0x051F` | 32 B | boot 阶段用户名明文 | `boot.asm:39`、`loader.asm:15` | 只在"未激活"流程使用；用后不清零 |
| `0x0520–0x053F` | 32 B | boot 阶段密码明文 | `boot.asm:45`、`loader.asm:16` | 同上；head 的密码缓冲在 `0x1020` |
| `0x0700–0x0740` | 65 B | 凭据块（跨阶段唯一来源） | `loader.asm:14`、`head.asm:12`、`firstmain.asm:120`、`ui.c:264` | `[0]=0x5A`，后两个 32 字节哈希 |
| `0x1000–0x15FF` | 1.5 KB | 16 位 SHA-256 工作区 | `sha256.inc:8-16` | 与 head 的用户名/哈希缓冲（`0x1000`/`0x1100`）地址重叠，但不同时使用 |
| `0x1000–0x1FFF` | 4 KB | PML4 | `firstmain.asm:13,232-237` | 长模式后仍被 CR3 引用，不能复用 |
| `0x2000–0x2FFF` | 4 KB | PDPT（4 个表项） | `firstmain.asm:14,239-246` | 覆盖 0–4 GB |
| `0x3000–0x6FFF` | 16 KB | 4 个 PD（2048 个 2 MB 大页） | `firstmain.asm:15,248-257` | 恒等映射，全部置 P + RW + PS |
| `0x7000–0x7063` | 100 B | bootinfo（前 32 B 是 VBE 信息） | `firstmain.asm:11-12,189-206`、`bootinfo.h:14-35` | magic `0x31444D55` 在 +32 |
| `0x7200–0x73FF` | 512 B | MBR 扇区读缓冲 | `firstmain.asm:50-57` | 签名 `0xAA55` 在 `0x73FE`，分区项在 `0x73BE` |
| `0x7C00–0x7DFF` | 512 B | boot 映像；栈顶 `0x7C00` | `boot.asm:11,19` | 栈向 `0x7Bxx` 生长 |
| `0x7E00–0x87FF` | 2.5 KB | loader 映像；栈顶 `0x7E00` | `loader.asm:12,27`、`build.py:19` | 被 hello 的 `sp=0x8000` 覆盖下半段 |
| `0x8800–0x9BFF` | 5 KB | head 映像；栈顶 `0x8800` | `head.asm:10,33`、`build.py:20` | 栈向下压 loader 尾部 |
| `0xA000–0xB7FF` | 6 KB | hello 映像；栈顶 `0x8000` | `hello.asm:10,18`、`build.py:21` | 栈顶落在 loader 映像内 |
| `0xC000–0xDFFF` | 8 KB | firstmain 映像；栈顶 `0xC000` | `firstmain.asm:9,28` | 距 hello 尾部仅 2 KB |
| `0xE000–0xE7FF` | 2 KB | nextmain 跳板 | `nextmain.asm:7`、`build.py:23` | 无栈切换（沿用 `0x90000`） |
| `0x10000–0x1FFFF` | 64 KB | C 内核装入区（128 扇区） | `build.py:29-30`、`firstmain.asm:17-18,84` | ELF 实际载荷到 `0x1D410` |
| `0x1D41C–0x23170` | ≈23 KB | 内核 `.bss`（含 `ui.c` 的 4×4 KB 编辑缓冲） | `bin/kernel.elf` 第 3 个 PT_LOAD | **越过 64 KB 装入区 12,656 字节** |
| `0x30000–0x4FFFF` | 128 KB | UMDFS 镜像（两段各 128 扇区） | `ui.c:10`、`firstmain.asm:90-113` | 落盘 LBA 来自 `bootinfo.fs_lba` |
| `0x90000` | — | 长模式与内核栈顶（向下生长） | `firstmain.asm:299`、`nextmain.asm:10`、`start.asm:30` | 无 guard；到 `0x50000` 之间为空闲 |
| `0xA0000–0xAFFFF` | 64 KB | VGA `320x200x256` 显存 | `vga8.inc:66-71` | 只在 16 位阶段使用 |
| `0xFD000000+` | ≈3.5 MB | VBE 线性帧缓冲（1280×720×4） | `firstmain.asm:190-206`、`gfx.c:11`、`emu_ui.py:37` | 真实地址由 BIOS `PhysBasePtr` 决定；模拟器/VBox 常见 `0xFD000000` |

### 2.1 布局结论

* 地址**来源已逐个确认**：上表每一行都能在源码找到写入/使用点，未使用推断值。
* `0x20000–0x2FFFF` 目前无人使用；但内核 `.bss` 已延伸到 `0x23170`，`start.asm:24-28` 的清零范围会写到 64 KB 装入区之外。当前没有东西放在那里，所以"能跑"，但这是**越界约定**：`KERNEL_SECTORS=128` 只覆盖文件载荷，不覆盖 `.bss`。
* `0x1000/0x1100/0x1600` 被多个 16 位阶段复用（`head` 的输入缓冲、`loader` 的哈希/扇区缓冲、`sha256` 工作区），属于阶段内约定，不跨级保存。
* 帧缓冲地址是 32 位字段：`bootinfo.fb_base_hi`（`bootinfo.h:16`）被 `kmain` 完全忽略，`gfx_info.base` 也是 `u32`（`gfx.h:26`、`gfx.c:11`），**4 GB 以上的帧缓冲必然截断**。
* `bootinfo.flags`（bit0 跳过停留）在 `ui_splash` 中从未读取（`ui.c:220-247`），是死字段。

## 3. 中断使用情况

| 阶段 | CPU 模式 | 描述符表 | 中断开关 | 键盘 | 鼠标 | 定时/其它 |
|---|---|---|---|---|---|---|
| boot | 实模式 | BIOS IVT | `sti`（`boot.asm:20`） | `int 16h AH=00h` 阻塞读 | 无 | `int 13h AH=42h/02h`、`int 10h AH=0Eh` |
| loader | 实模式 | BIOS IVT | `sti`（`loader.asm:28`） | `int 16h AH=00h` | 无 | `int 13h AH=08h/42h/43h`、`int 10h AH=0Eh` |
| head | 实模式 | IVT + 自装 `0x74` | `sti`（`head.asm:34`） | `int 16h AH=01h` 轮询 + `AH=00h` 取键（`head.asm:56-60`） | **IRQ12**：`head.asm:456-507`，1 个自己写的 ISR | 无 PIT；`cli/sti` 保护绘图（`head.asm:275,345`） |
| hello | 实模式 | BIOS IVT | `sti`（`hello.asm:19`） | 无输入 | 无 | `int 1Ah AH=56h`（PXE 检测）、`int 15h AH=86h`（延时）、CMOS `0x70/0x71` 轮询 |
| firstmain | 实模式 | BIOS IVT | `sti`（`firstmain.asm:29`），进长模式前再 `cli` | 无 | 无 | `int 10h AX=4F00h/4F01h/4F02h`、`int 13h AH=42h` |
| 内核 | 长模式 | **无 IDT** | **`cli` 后不再 `sti`**（`start.asm:15`） | 轮询 8042（`input.c:42-45`） | 轮询同一 8042（`input.c:47-63`） | 无 PIT、无 RTC IRQ；CMOS 轮询（`ui.c:51-76`） |

### 3.1 逐项核实

* **没有 IDT**：对 `src/` 全树检索 `lidt`/`sidt`/`idt` 无命中；内核没有 `isr`/`irq` 代码。`head.asm` 写的是实模式 IVT 的第 0x74 项：`mov word [es:0x74*4], mouse_isr`（`head.asm:460-461`）。
* **head 打开了 IRQ1 与 IRQ12**：8042 配置字节 `or al,0x03`（`head.asm:437`），PIC 从片开 IRQ12、主片开级联 IRQ2（`head.asm:463-468`）。其中 IRQ1 的服务程序仍是 **BIOS 自带的**，head 只是用 `int 16h AH=01h` 去取 BIOS 缓冲里的键，所以"键盘有中断、但本阶段不写 ISR"。
* **head 的 IRQ12 ISR 会直接绘图**：`mouse_isr` → `mouse_apply` → `cursor_hide/cursor_show`/`draw_fields`（`head.asm:509-566`）。在中断上下文里重入绘制、并在其中再次 `cli/sti`，是明确的竞态/重入风险。
* **内核全程关中断**：`start.asm:15` 的 `cli` 之后没有任何 `sti`；`io.h:34-36` 提供了 `sti` 但无人调用。因此键盘/鼠标/ATA 只能轮询，任何异常都没有落点。
* **PIC/PIT 未编程**：内核没有对 PIC/PIT 端口（`0x20/0x21/0xA0/0xA1/0x40/0x43`）执行过一次 `outb`；端口访问只涉及 ATA `0x1F0–0x1F7/0x3F6`、8042 `0x60/0x64`、CMOS `0x70/0x71` 和调试口 `0xE9`。
* **异常后果**：没有 IDT 时，`#DE`/`#PF`/`#GP`/`int3` 都会触发三重故障 → 机器复位。没有 panic 屏、没有 CS/RIP/CR2 信息、没有串口日志（`kmain.c:20,25` 只往 `0xE9` 写标记）。
* **内核没有时间基**：splash 的 60 秒是用 CMOS RTC 的"秒"做差（`ui.c:229-245`），既没有 PIT tick，也没有 `sleep()`。这让 P5 的调度和 P3 的 tick 都缺少基础设施。

## 4. ATA / VBE / PS/2 访问点

### 4.1 ATA（`src/kernel/ata.c`）

| 项目 | 实际实现 | 证据 |
|---|---|---|
| 通道 | 仅主 IDE 通道 | `ata.c:5-14`（`0x1F0–0x1F7`、`0x3F6`） |
| 设备 | 仅主盘 | `ata.c:42`（`outb(ATA_DRIVE, 0xA0)`）、`ata.c:76` |
| 寻址 | LBA28 | `ata.c:76`（驱动器头写 `0xE0`，再或上 LBA 高 4 位） |
| 传输 | PIO，`inw/outw` 每次 16 位 | `ata.c:86-110` |
| 中断 | 无，纯轮询 BSY/DRQ/ERR | `ata.c:24-34` |
| 命令 | `0xEC` IDENTIFY、`0x20` READ、`0x30` WRITE、`0xE7` FLUSH | `ata.c:51,89,102,115` |
| 块计数 | `u8`，0 直接拒绝（最大 255 扇区/次） | `ata.c:88,101` |
| 容量 | IDENTIFY 的 `id[60..61]`（28 位） | `ata.c:64` |
| 落盘 | 整份 128 KB 镜像，每 64 扇区一次 `ata_write` | `fs.c:390-403` |
| 未实现 | 从盘、次通道、LBA48、DMA、ATAPI、分区解析、PIT 超时（用循环计数 `1000000/2000000`） | 全文无对应代码 |

补充：`ata_init` 只用 `status==0xFF` 判断"无控制器"（`ata.c:44-45`），没有检查 IDENTIFY 的 ERR/ABRT 位，也没有校验设备是否支持 LBA。`ata_sectors()` 目前无人调用。

### 4.2 VBE（`src/firstmain.asm:126-215`）

| 项目 | 实际实现 | 证据 |
|---|---|---|
| 入口 | `int 10h AX=4F00h`，`vbe_block` 先写 `'VBE2'` | `firstmain.asm:128-133` |
| 模式枚举 | 从 `VideoModePtr` 逐项 `AX=4F01h` 填 `mode_block`（256 B） | `firstmain.asm:135-170` |
| 筛选 | 属性 `0x0090`（图形+线性帧缓冲）且 bpp=32；优先 1280×720，否则取第一个 32bpp | `firstmain.asm:152-167` |
| 切换 | `AX=4F02h`，`BX = mode` 再或上 `0x4000`（LFB） | `firstmain.asm:182-187` |
| 信息保存 | `PhysBasePtr(0x28)`、`BytesPerScanLine(0x10)×YRes(0x14)`、XRes(0x12)、YRes(0x14)、行像素数=字节/4、bpp(0x19) → `0x7000` | `firstmain.asm:190-206` |
| 局限 | 只保留**一个**模式，不存模式列表、不支持运行时切换；无双重缓冲；无 EDID/刷新率；`fb_base_hi` 恒 0；`gfx.c` 用 `u32` 基址 | `bootinfo.h:15-16`、`gfx.c:11` |

### 4.3 PS/2（`head.asm` 与 `src/kernel/input.c`）

| 项目 | 实际实现 | 证据 |
|---|---|---|
| 端口 | `0x64` 命令/状态、`0x60` 数据 | `head.asm:367-477`、`input.c:42-45` |
| 初始化（head） | 等写/读就绪 → `0xA8` 开 aux → `0x20/0x60` 改配置（开 IRQ1+IRQ12）→ `0xF6` 默认参数 → `0xF4` 开数据上报 | `head.asm:423-477` |
| 中断 | IRQ12（IVT `0x74`），ISR 清 `0xA0/0x20` EOI | `head.asm:456-507` |
| 内核 | `input_init` 只清 8042 残留字节；**不发** `0xF4/0xF6/0xA8` | `input.c:27-36` |
| 键盘 | 扫描码集 1，含 `0xE0` 扩展、Shift/Ctrl/Caps，F1–F12 只映射到 F1–F3 | `input.c:10-22,67-128`、`input.h:26-28` |
| 鼠标 | 3 字节包，按键 bit0–2，符号位 `pkt[0]` bit4/5；无滚轮（不做 4 字节/Intellimouse） | `input.c:47-63` |
| 关键限制 | 内核鼠标能否工作**取决于 head 阶段是否已经发过 `0xF4`**；若绕过 head（例如以后换成别的启动路径），内核鼠标不会有数据 | `input.c` 全文无初始化命令 |

## 5. UMDFS 实际格式

### 5.1 超级块（块 0，512 B，实际使用 40 B）

| 偏移 | 类型 | 字段 | fs.c | fsgen.py | 一致 |
|---|---|---|---|---|---|
| +0 | `u8[8]` | magic `"UMDFS1\0\0"` | `fs.c:8,67-69` | `fsgen.py:15,117` | 是 |
| +8 | `u32` | `block_size`（恒 512） | `fs.c:9,70` | `fsgen.py:117` | 是 |
| +12 | `u32` | `block_count`（128 KB → 256） | `fs.c:10` | `fsgen.py:117` | 是 |
| +16 | `u32` | `file_count`（目录+文件总数） | `fs.c:11,318` | `fsgen.py:117` | 是 |
| +20 | `u32` | `data_start`（=8） | `fs.c:12,248` | `fsgen.py:18,117` | 是 |
| +24 | `u32` | `dirent_block`（=1） | `fs.c:13,53` | `fsgen.py:19,117` | 是 |
| +28 | `u32` | `dirent_count`（生成 32） | `fs.c:14,76` | `fsgen.py:20,117` | 是 |
| +32 | `u32` | `bitmap_block`（=5） | `fs.c:15,28` | `fsgen.py:21,117` | 是 |
| +36 | `u32` | `magic2 = 0x46533031`（`"FS01"`） | `fs.c:16,70` | `fsgen.py:16,117` | 是 |

### 5.2 目录项（64 B/项，表在块 1–4）

| 偏移 | 类型 | 字段 | fs.c / fs.h | fsgen.py | 一致 |
|---|---|---|---|---|---|
| +0 | `char[32]` | 名称，NUL 补齐，ASCII，最长 31 | `fs.h:10`、`fs.c:271-277` | `fsgen.py:123-124` | 是 |
| +32 | `u32` | `size`（字节） | `fs.h:11`、`fs.c:301` | `fsgen.py:125` | 是 |
| +36 | `u32` | `first_block` | `fs.h:12`、`fs.c:302` | `fsgen.py:125` | 是 |
| +40 | `u8` | `used` | `fs.h:13`、`fs.c:308` | `fsgen.py:126` | 是 |
| +41 | `u8` | `type`：0=文件，1=目录 | `fs.h:14`、`fs.c:304` | `fsgen.py:127` | 是 |
| +42 | `u8[2]` | 填充 | `fs.h:15` | 未写（保持 0） | 是 |
| +44 | `u32` | `parent`（父目录项索引，根=`0xFFFFFFFF`） | `fs.h:16`、`fs.c:317` | `fsgen.py:128` | 是 |

注意：`fs_entry_t` 的 `sizeof` 是 48，但**步长固定 64**（`fs.c:51-54`），每项浪费 16 B；`dirent_count` 个表项占用 `ceil(count×64/512)` 个块。

### 5.3 位图、数据区与分配

* 位图：块 5，512 B = 4096 位；`fsgen.py:130-137` 把块 0–7 标为占用，再标每个文件的数据块。`fs.c` 只检查 `block_count` 位（256 位），超出部分不看。
* 数据区：块 8 起（文件偏移 `0x1000`）；文件内容**连续存放**在 `first_block` 开始处（`fs.c:322-325`、`fsgen.py:139-143`）。
* 空闲块计算：`fs_free_blocks` 遍历 `0..block_count-1` 数 0 位（`fs.c:227-234`）。内存中 `free_blocks = 256 − 已用`。
* 分配：`alloc_run` 从 `data_start` 起找**第一段连续空闲块**（`fs.c:245-262`）；写放大时先分配新段再释放旧段（`fs.c:295-300`），碎片化到没有连续段时返回 `-2`（写失败）。
* 一致性副作用：`fs_write/fs_mkdir/fs_unlink` 直接改内存里的超级块 `file_count`（`fs.c:318,344,370`），`fs_flush` 再把整个 128 KB 写回（`fs.c:390-403`）。

### 5.4 与生成器的一致性核对结论

* 字段偏移、块号、块大小、目录项布局**完全一致**，`fs.img` 能被内核正确挂载（`ui.c:150`）。
* 但 `fs.c:76` 把 `dirent_count` 上限钳到 64：64 项需要 8 个块（块 1–8），会覆盖位图块 5 和数据块 8；生成器只写 32 项（4 块），所以现在是安全的，**一旦有人改生成器到 33 项以上就会踩坏位图**。
* `fs_mount` 没有校验 `data_start`/`dirent_block`/`bitmap_block` 的边界、位图大小、`first_block+size` 是否越界（只在读时检查 `fs.c:215-220`）。镜像被视为可信输入。
* 没有时间戳、权限、硬链接、稀疏文件、日志/校验和；写入非原子，掉电可能留下"位图与目录项不一致"的镜像。

## 6. 认证数据位置与算法

### 6.1 `0x0700` 凭据块（65 B）

| 偏移 | 内容 | 写入者 | 读取者 |
|---|---|---|---|
| +0 | `0x5A` 标志 | `loader.asm:121`（已激活分支 `loader.asm:45`） | `head.asm:145`、`ui.c:265` |
| +1..+32 | 用户名 SHA-256（大端 32 B） | `loader.asm:105-111` | `head.asm:158-163`、`ui.c:268-270` |
| +33..+64 | 密码 SHA-256（大端 32 B） | `loader.asm:113-119` | `head.asm:175-180`、`ui.c:271-273` |

### 6.2 磁盘 LBA6 / LBA7

| 扇区 | 偏移 | 内容 | 证据 |
|---|---|---|---|
| LBA6 | +0 | `0xAA` 已激活标志 | `loader.asm:179`、`boot.asm:30` |
| LBA6 | +1..+32 | 用户名哈希 | `loader.asm:180-183` |
| LBA6 | 其余 | 全 0（`SEC_BUF` 先整扇区清零，避免把明文写盘） | `loader.asm:174-178` |
| LBA7 | +0 | `0x55` 标志 | `loader.asm:193` |
| LBA7 | +1..+32 | 密码哈希 | `loader.asm:194-197` |

读取路径的细节：`loader.asm:33-64` 先读 LBA6，若 `[0]==0xAA` 才拷用户名哈希；随后读 LBA7，**只有 `[0]==0x55` 时才拷密码哈希**。若磁盘上有 LBA6 但没有合法的 LBA7，`0x0700+33` 保持内存原值（开机时通常是 0），图形登录会一直失败且没有诊断。

### 6.3 算法与验证路径

* 16 位：`src/inc/sha256.inc`（单块，输入 0–55 字节，工作区 `0x1200–0x15FF`）；64 位：`src/kernel/sha256.c`（多块，标准 FIPS 180-4）。
* 已复算：`SHA-256("UMDOS2024") = 72B6E915AB725A9C958BD71B002A694F9DD6FA6D1FAFF91A00220223ED7FD9F5`，与 `loader.asm:216-220` 的 `expected_activate` 逐字节一致；`SHA-256("") = E3B0C442…B855`，与 README 失败诊断中的空串哈希一致。
* 路径：boot 采集明文（`0x0500/0x0520`）→ loader 计算哈希写入 `0x0700` 并尽力写回 LBA6/7 → head 登录时对输入重新 SHA-256 并 `repe cmpsb` 比较 → 内核登录时再次 SHA-256 并与 `bootinfo.creds_addr`（=0x0700）比较。

### 6.4 安全评价

* **算法不符合规格**：规格 P12 要求 SHA-3-256 + 随机盐（`sha3-256$salt$hash`）；当前是 SHA-256、无盐、单轮。
* **可绕过**：`0x5A` 只是单字节标志，能写 `0x0700` 或能改镜像的人可以直接放任意哈希；激活码哈希硬编码在镜像中（`loader.asm:216-220`）。
* **明文驻留**：boot/loader 的用户名、密码明文留在 `0x0500/0x0520`，head 成功后不清 `0x1020`，内核登录后不清 `user_buf/pass_buf`（`ui.c:37`）。
* **无速率限制**：head 与内核登录失败都可无限重试，无延时、无锁定、无日志。
* **长度上限不一致**：boot/head 16 字符（`boot.asm:40`、`head.asm:93`），内核 19 字符（`ui.c:365-366` 的 cap=20）。

## 7. 文档矛盾与代码缺陷清单

### 7.1 必须修改项（20 条）

| # | 现状（证据） | 问题 | 建议改法 | P 任务 |
|---|---|---|---|---|
| 1 | 认证用 SHA-256、无盐（`sha256.inc`、`sha256.c`、`loader.asm:105-119`） | 不符合规格 P12；彩虹表/离线爆破成本极低 | 改为 SHA-3-256 + 16 B 随机盐，格式 `sha3-256$salt$hash` | P12（P7 迁移存储） |
| 2 | 凭据在裸扇区 LBA6/7 与固定地址 `0x0700`（`loader.asm:14,170-201`） | 无文件系统语义、无权限、无迁移路径；`0x0700` 可被任意代码改写 | 存到 `/config/auth`，权限 0600；保留一次性旧镜像迁移 | P7、P12 |
| 3 | 全工程无 IDT（`src/` 无 `lidt`） | 任何异常 → 三重故障重启；无 panic 信息 | 建 64 位 IDT，0–31 异常 + IRQ0/1/12/14，打印 CS/RIP/CR2/错误码并停在 panic 屏 | P3 |
| 4 | 内核 `cli` 后不再开中断（`start.asm:15`），PIC/PIT 未编程 | 无 tick、无中断驱动 I/O；后续调度/超时都没有基础 | 重映射/初始化 PIC 或 APIC，PIT 1 kHz，开中断 | P3、P14 |
| 5 | 键盘/鼠标轮询（`input.c`），鼠标还依赖 head 遗留的 `0xF4` 状态 | CPU 100% 空转；丢字节；启动路径一变鼠标就失效 | 内核自己初始化 8042（`0xA8/0x20/0x60/0xF6/0xF4`），IRQ1/IRQ12 写环形缓冲，`input_poll` 改读缓冲 | P3、P8 |
| 6 | 无 E820、无页帧分配器、无 `kmalloc`（`src/kernel/` 无相关代码） | 所有内存都是"开机时约定好的固定地址"；加任何功能都要改链接脚本/手工地址 | 解析 E820，建位图页帧分配器，`kmalloc/kfree`，为 FB/内核/用户区建立映射策略 | P4 |
| 7 | 无用户态、无进程、无 TSS、无 syscall（`kmain.c` 单线程 ring0） | 无隔离，无法跑应用，无法做权限 | 内核线程 + RR 调度 + TSS + ring3 + `syscall` 入口（exit/write/read/open/close/getpid/sleep/brk） | P5 |
| 8 | 无 ELF64 加载器、无用户 libc | 应用只能编译进内核 | 解析 PT_LOAD，映射用户地址空间，设用户栈；提供最小 libc | P6 |
| 9 | 无 VFS；`fs.c` 直接暴露 `fs_*`，`cwd` 是全局变量（`fs.c:24`） | 上层与具体 FS 绑死；多窗口共用一个 cwd，靠每次 `fs_set_dir` 纠正（`ui.c:601,1237,1251...`） | VFS：inode/dentry/file/ops + 路径解析；每进程 cwd | P7 |
| 10 | 无设备模型；`fs.c` 直接 `#include "ata.h"`（`fs.c:4`） | 换 AHCI/USB 存储要改 FS；GUI 直接写帧缓冲，输入直接读 8042 | 设备注册表 + read/write/ioctl；块设备、framebuffer 设备、事件设备 | P8 |
| 11 | 无窗口系统抽象；`ui.c` 1662 行单体，直接写 FB，无事件队列、无双缓冲 | 无法加应用窗口、无法做重绘/合成；撕裂与重入 | 合成器（双缓冲）+ 窗口结构 + 事件队列；把文件管理器/编辑器拆成应用 | P9 |
| 12 | 桌面只有 1 个图标 + 顶栏时钟 + 底部任务栏（`ui.c:1136-1165`）；无开始菜单/状态栏/设置/锁屏/关机 | 不满足 P10 的桌面 Shell | 5×8 图标网格、开始菜单、6 项状态栏、6 页设置、锁屏/关机/重启 | P10 |
| 13 | 无终端、无 shell、无常用命令 | 不满足 P11；用户只能点鼠标 | 终端 + 命令（ls/cd/cat/echo/mkdir/rm/mv/cp/edit/reboot/ps/kill） | P11 |
| 14 | 无 UEFI 路径（仅 Legacy BIOS/El Torito） | 纯 UEFI 机器无法启动 | `efi/` + PE 入口，GOP 取帧缓冲，与 Legacy 共用内核初始化 | P13 |
| 15 | 无 ACPI/APIC/SMP | 关机/重启不可用，多核不可用 | 解析 RSDP/XSDT/FADT/MADT，LAPIC/IOAPIC，INIT-SIPI-SIPI | P14、P15 |
| 16 | ATA 只主通道主盘 LBA28 PIO（`ata.c:5-14,42,76`） | 不支持 SATA/AHCI、从盘、次通道、>128 GB | 先做块设备抽象，再加 AHCI（保留 ATA PIO 回退） | P8、P16 |
| 17 | 无 USB HID（无 PCI 枚举/USB 栈） | 无 USB 键鼠的机器不可用 | xHCI/EHCI 枚举 + 控制传输 + HID 报告 → 事件设备 | P17 |
| 18 | 无 `/proc`（无伪文件系统） | 无法暴露版本/内存/进程信息 | VFS 中加 procfs，至少 cpuinfo/meminfo/uptime/version/ps | P19 |
| 19 | 无应用包、构建脚本无 installer 分支（`build.py` 全文） | 不满足 P20 与 dev.1 的安装引导要求 | `.umpkg` 清单+安装到 `/apps`；安装器接入构建并产出 installer-only ISO | P20、dev.1 |
| 20 | 版本号未注入：无 `VERSION`/`version.h`，界面硬编码 `UMDos 1.0`（`ui.c:287`）、`UMDos-DOS1 beta`（`head.asm:648`），镜像名无版本 | 不满足"五处版本一致"验收；无法区分构建 | 单一 `VERSION` → 生成 `version.h` → 注入内核/用户态；镜像名带版本；`tools/bump_version.py` | dev.1 构建项、P22 |

### 7.2 补充缺陷（不单列"必须修改"，但会直接坑后续任务）

| # | 现状 | 影响 | 建议 |
|---|---|---|---|
| A | `ui.c:229-245` 用 `(rtc_seconds()-t0)&0xFF` 计时。秒数每分钟回绕，回绕后 `el=256-t0` 必然 ≥ 60 | 真实机器上 splash **最多只等 60−t0 秒**（可能 1 秒就结束），倒计时跳变；模拟器每读一次 CMOS 秒就自增（`emu_ui.py:118-123`），掩盖了该问题 | 用 PIT tick 或分钟+秒合成绝对时间；P3 后改用 tick |
| B | `do_step(0)` 只写调试口（`ui.c:137-139`），`do_step(1)` 的"校验内核镜像"是每 64 字节取样求和且结果 `load_sum` 从未使用（`ui.c:140-148`） | 启动画面上的"读分区表/校验内核"是**装饰性文字**，不是真校验 | 要么真做校验（SHA-256 + 清单），要么改文案 |
| C | `kmain` 忽略 `bootinfo.magic`（`kmain.c:9`）；`ui_splash` 忽略 `flags` | 传错的 bootinfo 不会被发现；调试开关无效 | 入口校验 magic/version/size，失败进 panic |
| D | 编辑器缓冲 `ebuf[4][4096]`，打开时截断到 4095 B（`ui.c:399,577-585`），保存时写回截断后的长度 | 打开并保存 >4 KB 文件会**静默丢数据** | 动态缓冲 + 明确提示；随 P7 的 VFS/`read/write` 重做 |
| E | 内核主循环 `for(;;);`（`kmain.c:28-29`），且 IF=0 | 桌面空闲时 CPU 满载、无法 `hlt` 省电 | P3 开中断后用 `hlt` 等事件 |
| F | 单次 EDD 读 128 扇区（`firstmain.asm:80-113`），超过常见 127 扇区上限 | 部分 BIOS 直接失败，退回逐扇区 CHS，启动变慢甚至超时 | 拆成 ≤64 扇区/次，失败重试 + 磁盘复位（`AH=00h`） |

### 7.3 文档矛盾（12 条）

| # | 矛盾 | 证据 |
|---|---|---|
| 1 | 《使用说明》末尾说"没有文件系统"，正文却大篇幅描述 UMDFS 读写/落盘 | `使用说明.md:292` vs `使用说明.md:166-170`、`README.md:139-142` |
| 2 | 同一文件内存要求"≥32MB"与"≥64MB"并存 | `使用说明.md:14` vs `使用说明.md:48` |
| 3 | "之后只问登录"与实际每轮启动两次登录（head 图形登录 + 内核登录）不符 | `使用说明.md:16` vs `head.asm:144-194`、`ui.c:314-373`、`README.md:134-135` |
| 4 | README 称内核"实际约 42KB"；实际 ELF 载荷 54,300 B（0xD41C），加 `.bss` 到 0x23170 | `README.md:115` vs `bin/kernel.elf` 程序头 |
| 5 | README 称"各阶段互不覆盖"；实际各级栈顶压在前一级映像内（hello 0x8000、head 0x8800、firstmain 0xC000） | `README.md:118` vs `hello.asm:18`、`head.asm:33`、`firstmain.asm:28` |
| 6 | 《修复说明》回归写"5 个场景 / 5/5 passed"，README 写"6 个场景"，脚本实际 6 项（含 hostile_bios） | `修复说明.md:196-202` vs `README.md:187`、`test_all.py:86-93` |
| 7 | 《修复说明》自相矛盾：一处"6/6 通过"，一处"5/5 passed" | `修复说明.md:153` vs `修复说明.md:198-199` |
| 8 | 《代码审查报告》布局表仍是修复前版本（head 0x8000、hello 0x9000、firstmain 0xA000、LBA 58/98、40 扇区），且未标注"历史快照" | `代码审查报告.md:509-524` vs `build.py:17-24`、`head.asm:10`、`hello.asm:10`、`firstmain.asm:9` |
| 9 | 《代码审查报告》称凭据在 0x0500/0x0600、LBA7 密码哈希"从未被校验"；现状是 0x0700 且 head 与内核都校验 | `代码审查报告.md:499,503` vs `loader.asm:14`、`head.asm:176`、`ui.c:272` |
| 10 | README"内核没有分页管理"不准确：静态 4GB 页表已建、内核运行在分页开启状态 | `README.md:232` vs `firstmain.asm:227-258`、`start.asm` |
| 11 | 桌面图标上写 "double click"，代码是**单击**打开（`ui.c:1547-1549`）；《使用说明》同时出现"点一下"与"双击" | `ui.c:1163` vs `ui.c:1547`、`使用说明.md:172-173` |
| 12 | 输入长度上限不一致：boot/head 16 字符，内核 19 字符，而《使用说明》写"各最多 16 字符" | `boot.asm:40`、`head.asm:93`、`ui.c:365-366`、`使用说明.md:102` |

## 8. 与规格文档的差距摘要

规格来源：`tmp_spec.txt`（A 部分 P1–P12，B 部分 P13–P17/P19–P22，P18 不做）。

| P | 规格要求 | 当前实现 | 差距 / 状态 |
|---|---|---|---|
| P1 | 审计 + 路线图，不改内核代码 | 本文档 + `docs/roadmap.md` | 本次交付；审计结论已量化 |
| P2 | 启动链单一职责、去重复初始化、A20/GDT/保护模式归 loader | 6 级各自 `cli`/清段/设栈/`disk_init`；boot 自带重复 I/O 库；仍从实模式直接进长模式（无保护模式阶段） | 当前未实现 |
| P3 | 64 位 IDT、0–31 异常 panic、IRQ0/1/12/14 | 无 IDT；内核 IF=0；全轮询 | 当前未实现 |
| P4 | E820、物理页帧分配器、4 级页表、`kmalloc/kfree` | 仅静态 4 GB 恒等映射（2 MB 大页），无分配器 | 当前未实现（只有静态页表） |
| P5 | 内核线程、RR 调度、TSS、ring3、syscall | 单线程 ring0，`for(;;)` | 当前未实现 |
| P6 | ELF64 加载器、最小 libc | 内核是静态平铺二进制，无用户程序 | 当前未实现 |
| P7 | VFS + UMDFS2/FAT32 根 + `/config/auth` 迁移 | 仅 UMDFS 直接 API；认证在 LBA6/7 | 当前未实现 |
| P8 | 设备注册表、统一 read/write/ioctl、块设备 | `fs.c` 直接调 `ata.c`，GUI 直接写 FB | 当前未实现 |
| P9 | 双缓冲、窗口结构、事件队列、Alt+Tab | 有 4 窗口/z 序/拖动/关闭，但直接写 FB、无事件队列、无 Alt+Tab、无双重缓冲 | 部分实现（窗口有，事件/合成无） |
| P10 | 桌面图标网格、开始菜单、状态栏、设置、锁屏/关机 | 1 个桌面图标、顶栏时钟、底部任务栏；无开始菜单/状态栏/设置/电源 | 部分实现（图标/任务栏/时钟有，其余无） |
| P11 | 终端 + shell + 文件管理器增强 | 无终端；文件管理器有列表/预览/新建/删除/上级/重命名 | 部分实现（文件管理器/编辑器有，终端/命令无） |
| P12 | SHA-3-256 + 盐、多用户、权限、限速、锁屏 | SHA-256 无盐；单用户；无限速 | 当前未实现（只有单用户 SHA-256） |
| P13 | UEFI + Legacy 混合 ISO | 仅 Legacy/El Torito | 当前未实现 |
| P14 | ACPI/APIC/电源管理 | 无；关机/重启不可用 | 当前未实现 |
| P15 | SMP | 无 | 当前未实现 |
| P16 | AHCI 替代 ATA PIO | ATA PIO 主盘 LBA28 | 当前未实现 |
| P17 | USB HID 键鼠 | 无 USB 栈 | 当前未实现 |
| P18 | 网络栈 | 明确不做；hello 只检测 PXE | 按规格不做 |
| P19 | `/proc` 伪文件系统 | 无 | 当前未实现 |
| P20 | `.umpkg` 应用包 + 安装器 | 无；`installer/` 有并行骨架但未接入构建 | 当前未实现 |
| P21 | AC'97/HDA 音频 + `audio_write` | 无 | 当前未实现 |
| P22 | 串口日志、panic 截图、QEMU smoke test、CI、`docs/test-matrix.md` | 有 Unicorn 回归（`test_all.py` 6 项、`test_persist.py`、`test_editor.py`、`test_edit2.py`）与 ISO 校验；无 CI、无 `make test`、无 panic、无 test-matrix | 部分实现（有回归脚本，无 CI/panic） |

合计：P1 完成；P9/P10/P11/P22 部分；P2–P8、P12–P17、P19–P21 未实现；P18 不做。

## 9. 风险与建议

| 优先级 | 风险 | 影响 | 建议 |
|---|---|---|---|
| P0 | 无 IDT/异常处理 | 任何空指针/除零/坏页表都直接重启，调试成本极高；后续 P4/P5 无法开发 | 先做 P3（IDT + panic），再做内存/用户态 |
| P0 | 内核 IF=0 且无时间基 | 只能忙等/轮询；P5 调度、P3 tick、P11 终端都没有基础 | P3 里把 PIC/PIT/IRQ 框架一次做对 |
| P0 | 内核 `.bss` 超出 64 KB 装入区（到 `0x23170`） | 以后把任何东西放到 `0x20000` 都会被清零；链接脚本与 `KERNEL_SECTORS` 脱节 | P4 统一管理内核镜像/堆区，构建时校验 `bss_end ≤ 装入区` 或显式预留 |
| P0 | 认证弱（SHA-256、无盐、单字节标志、明文驻留） | 不满足安全验收，且演示系统也容易被绕过 | P12 前先按 P7 迁移到 `/config/auth`，P12 换 SHA-3+盐+限速 |
| P1 | 无物理内存管理 | 每加一个功能都要手工分配地址，冲突只会在运行时爆炸 | P4 的 E820 + 页帧分配器优先于其他 B 部分功能 |
| P1 | 一次 EDD 读 64 KB | 换 BIOS 可能启动失败；CHS 回退极慢 | P2 拆小传输 + 重试 + `AH=00h` 复位 |
| P1 | RTC 秒回绕导致 splash 最多 1–60 秒 | 真实机器与模拟器行为不一致，验收不可复现 | P3 换 PIT tick；短期至少合成"分+秒"绝对时间 |
| P1 | 编辑器 4 KB 静默截断 | 保存即丢数据，演示时会误导 | P7/P11 改动态缓冲并提示 |
| P1 | 多窗口共享全局 `cwd` | 以后加多进程/多标签会出现"窗口串目录" | P7 VFS 时把 cwd 放进进程/窗口上下文 |
| P2 | 无双重缓冲、直接写 FB | 拖动/重绘撕裂；加动画后更明显 | P9 合成器 |
| P2 | 文档与代码脱节（12 条矛盾） | 验收/交接时互相矛盾，浪费排查时间 | 每完成一个 P 同步更新 README/使用说明，历史报告标注"历史快照" |
| P2 | 版本未注入、无 CI | 无法做"五处版本一致"验收；回归靠手工 | dev.1 落 `VERSION`/`version.h`/镜像名；P22 落 CI |

建议的开工顺序（与 roadmap 一致）：P2（启动链清理 + 传输拆分）→ P3（IDT/中断/tick）→ P4（E820/分页/堆）→ P5/P6（用户态）→ P7/P8（VFS/设备模型）→ P9/P10/P11/P12（桌面与安全）→ B 部分。**不要**在 P3/P4 之前直接做 GUI 功能，否则每次异常都只能重启排查。

## 附录 A：审计基线（源文件 SHA-256，供 dev.1 证明"内核源码未改动"）

```
build.py                 d2d635cf463f4ac7512aaace7458b263e4b36f3fa3361810e2798b2a7c67ada5
fsgen.py                 c1c07e16b04a8708e71b85dc2a863b7f75733ec4ed821e5c2ccf44730a6e4f5d
src/boot.asm             0ddb526f739357f301ee9ae0a3fd565bf3b7a2a620806c6467c057391cd4fcc3
src/loader.asm           d11340b116e485f9e56727915e65dbbf0f4b813a703659a2344027bf0b3111f9
src/head.asm             3333bfafaaea015f5ef146c298c4b5815503be9b7e87f5d1b7148960afe8a477
src/hello.asm            f8a2e5760001b400da41fc66d781bbd8839d3c5c308816094bc338b8a55162f1
src/firstmain.asm        5c6ac0d04924ea4889ad2f3146cb2793a1f2302dd1f306d7e282fda2aac5bdd0
src/nextmain.asm         e7bde76dc5cfd926b00ba17627a917ec1f0d2d22fbe221092d90fb0ab7ce32bc
src/inc/disk16.inc       aa5aabdb5401ae5f840e237b3dea8b0979f8e5a40936d93afb7ac316f8d740e0
src/inc/io16.inc         33f2960ad2332f779df21a503f1823c54710f64c8bb1a705dad3a7bef4deaee4
src/inc/sha256.inc       6067e1cb49ea5b0a6682d2034902020ba37680a172b92bea6d9d1c22330720c4
src/inc/vga8.inc         f7fcf007918ded2ecbc2d898e01a694fbd4718b95ebc3cd2d42891fb9251ae81
src/kernel/start.asm     2f4de002deb746c24728dc18956ece822645c42fde25d161d9c46966889ce011
src/kernel/link.ld       0e064fa25e07a465d733909b7e8eb5cd4ad7f08ead9a4f3839a065f83e39d35f
src/kernel/kmain.c       4e4533663d6f803d8f975a6a8ac7b13a3ce8079d3a032278fb7d68df3f09ab87
src/kernel/gfx.c         6f8863f8cb15439d4a369d1fd945e57cfaf3c355034d4d94d75c47ca6b705aa1
src/kernel/input.c       dc306dc6e4e2dfdd849c02602d093e4f1277508f0b9d7233a80dc397eab06a86
src/kernel/sha256.c      53fe13da4698efc5df538526265e65235abbf93ecfe6ac5481d96fbeea70badd
src/kernel/fs.c          220ee121148f874f8755c982db237ffaa68fff438f043aa0e6c7e3bddefd532d
src/kernel/ata.c         a44b9fdae39bc030d6e38aa1e557cdd7436cf8f4b86d93e43c160c8dca80c19a
src/kernel/ui.c          3d9a279497c3ef74235a6d5e8c9ab586eec134ca5b785aaebc64e5ddec37b20f
```

（本表只列关键文件；完整列表可按附录 B 的方法对 `src/**`、`build.py`、`fsgen.py` 逐个 `SHA-256` 生成。dev.1 交付时用同一份清单比对即可证明"内核源码未改动"。）

## 附录 B：审计方法

* `grep -E "lidt|sidt|IDT"`、`grep "int 0x"`：确认中断设施与 BIOS 调用点；结果见第 3 节。
* `grep "outb|inb"`：确认端口访问面（ATA、8042、CMOS、VBE/VGA、`0xE9`）。
* `struct.unpack` 解析 `bin/kernel.elf` 程序头：得到载荷 `0x10000–0x1D410`、`.bss` 到 `0x23170`。
* 用 `System.Security.Cryptography.SHA256` 复算 `UMDOS2024` 与空串：与 `loader.asm:216-220` 及 README 诊断值一致。
* 阅读 `tools/emu.py`、`tools/emu_ui.py`、`tools/test_all.py`：确认模拟器只模拟 `INT 10h/13h/15h/16h/1Ah`、8042、CMOS、ATA、VBE（1280×720/1024×768/800×600），因此"测试通过"不等于真实硬件/规格通过。
