#!/usr/bin/env python3
"""把使用说明生成一份可打印的 Word 文档。"""
import os
from docx import Document
from docx.shared import Pt, Cm, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.oxml.ns import qn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "使用说明.docx")
SHOTS = os.path.join(ROOT, "shots")

BODY = "微软雅黑"
MONO = "Consolas"


def set_font(style, name, size=None, bold=None, color=None):
    style.font.name = name
    style.element.rPr.rFonts.set(qn("w:eastAsia"), name)
    if size:
        style.font.size = Pt(size)
    if bold is not None:
        style.font.bold = bold
    if color:
        style.font.color.rgb = color


doc = Document()

sec = doc.sections[0]
sec.page_width, sec.page_height = Cm(21), Cm(29.7)
sec.left_margin = sec.right_margin = Cm(2.2)
sec.top_margin = sec.bottom_margin = Cm(2.2)

set_font(doc.styles["Normal"], BODY, 10.5)
for lvl in (1, 2, 3):
    set_font(doc.styles[f"Heading {lvl}"], BODY, {1: 15, 2: 12.5, 3: 11}[lvl], True,
             RGBColor(0x1F, 0x3A, 0x5F))

# 代码段样式
code_style = doc.styles.add_style("CodeBlock", 1)
code_style.base_style = doc.styles["Normal"]
set_font(code_style, MONO, 9)
code_style.paragraph_format.left_indent = Cm(0.4)
code_style.paragraph_format.space_after = Pt(2)


def para(text, style=None, bold_prefix=None, size=None):
    p = doc.add_paragraph(style=style)
    if bold_prefix and text.startswith(bold_prefix):
        r = p.add_run(bold_prefix)
        r.bold = True
        p.add_run(text[len(bold_prefix):])
    else:
        p.add_run(text)
    if size:
        for r in p.runs:
            r.font.size = Pt(size)
    return p


def bullet(text):
    return doc.add_paragraph(text, style="List Bullet")


def code(lines):
    for line in lines:
        doc.add_paragraph(line, style="CodeBlock")


def table(header, rows, widths=None):
    t = doc.add_table(rows=1, cols=len(header))
    t.style = "Table Grid"
    t.alignment = WD_TABLE_ALIGNMENT.CENTER
    for i, h in enumerate(header):
        cell = t.rows[0].cells[i]
        cell.text = ""
        run = cell.paragraphs[0].add_run(h)
        run.bold = True
        run.font.size = Pt(9.5)
    for row in rows:
        cells = t.add_row().cells
        for i, v in enumerate(row):
            cells[i].text = ""
            run = cells[i].paragraphs[0].add_run(v)
            run.font.size = Pt(9)
    if widths:
        for row in t.rows:
            for i, w in enumerate(widths):
                row.cells[i].width = Cm(w)
    doc.add_paragraph()
    return t


def picture(name, caption, width=Cm(14.5)):
    path = os.path.join(SHOTS, name)
    if not os.path.exists(path):
        return
    doc.add_picture(path, width=width)
    doc.paragraphs[-1].alignment = WD_ALIGN_PARAGRAPH.CENTER
    cap = doc.add_paragraph(caption)
    cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
    for r in cap.runs:
        r.font.size = Pt(8.5)
        r.font.color.rgb = RGBColor(0x66, 0x66, 0x66)


# ---------------------------------------------------------------- 正文
doc.add_heading("UMDos 使用说明", 0)
p = doc.add_paragraph("一个 16 位实模式起步、最终进入 64 位长模式的启动链演示系统："
                      "引导扇区 → 激活校验 → 图形登录（SHA-256 验证）→ 初始化任务 → "
                      "VBE 高分辨率 → 64 位内核入口。")
p.paragraph_format.space_after = Pt(10)

doc.add_heading("一、先看这一页", 1)
table(["项目", "值"], [
    ["激活码", "UMDOS2024"],
    ["用户名 / 密码", "第一次启动时自己设定（各 ≤ 16 字符）"],
    ["运行环境", "Legacy BIOS（必须关闭 UEFI/EFI）、64 位 CPU、≥ 32MB 内存"],
    ["第一次启动要输入", "3 次：设账号 → 激活码 → 登录"],
    ["之后还问吗", "可写介质（硬盘/软盘）只问登录；光盘只读，每次都要重走一遍"],
    ["键位", "Enter 确认 · Tab 切换输入框 · Backspace 删除"],
    ["鼠标", "自动检测，有就能用（点输入框切换焦点），没有也能全键盘操作"],
], widths=[4.2, 12.0])

doc.add_heading("二、文件清单", 1)
table(["文件", "用途"], [
    ["uMDos-hdd.img", "16MB 硬盘镜像。VirtualBox/VMware 用这个（先转成虚拟盘）"],
    ["uMDos.iso", "可启动光盘镜像。挂到虚拟机光驱即可"],
    ["uMDos.img", "1.44MB 软盘镜像。也可以写 U 盘做整盘启动"],
    ["README.md / 修复说明.md", "构建说明与修复记录"],
    ["src/、bin/", "源码与编译产物"],
    ["shots/", "运行截图"],
], widths=[4.2, 12.0])

doc.add_heading("三、启动方式", 1)

doc.add_heading("A. VirtualBox（推荐用硬盘镜像）", 2)
para("注意：VirtualBox 不能直接把 .img 当硬盘挂，必须先转成 VDI。")
bullet("在镜像所在目录执行：")
code(['"C:\\Program Files\\Oracle\\VirtualBox\\VBoxManage.exe" convertfromraw uMDos-hdd.img uMDos.vdi'])
bullet("新建虚拟机：类型 Other / Unknown (64-bit)，内存 ≥ 64MB，不创建虚拟硬盘。")
bullet("设置 → 存储 → 控制器 SATA → 添加硬盘 → 选择 uMDos.vdi。")
bullet("设置 → 系统 → 主板：取消勾选「启用 EFI」（关键）；启动顺序把硬盘放第一位。")
bullet("设置 → 显示：图形控制器选 VBoxVGA，取消「启用 3D 加速」，显存 ≥ 16MB。")
bullet("启动虚拟机。")

doc.add_heading("B. VirtualBox（用光盘镜像）", 2)
bullet("设置 → 存储 → 控制器选 IDE → 添加光驱 → 选择 uMDos.iso。")
bullet("系统 → 主板：关闭 EFI；启动顺序把光驱放第一位；显示设置同 A。")
bullet("光盘只读，所以每次启动都要重新走「设账号 → 激活码」流程。")

doc.add_heading("C. QEMU", 2)
code([
    "qemu-system-x86_64 -hda uMDos-hdd.img -boot c     # 硬盘",
    "qemu-system-x86_64 -cdrom uMDos.iso -boot d       # 光盘",
    "qemu-system-x86_64 -fda uMDos.img -boot a         # 软盘",
])

doc.add_heading("D. VMware", 2)
para("把 uMDos-hdd.img 作为「现有磁盘」导入，或把 uMDos.iso 挂到 CD/DVD；"
     "固件类型选 BIOS（不要 UEFI）。")

doc.add_heading("E. 写 U 盘", 2)
code(["dd if=uMDos-hdd.img of=/dev/sdX bs=1M conv=fsync"])
para("Windows 下可用 Rufus，选择「DD 模式」写入 uMDos-hdd.img。写完从 U 盘启动即可。")

doc.add_heading("四、第一次启动会看到什么", 1)

doc.add_heading("第 1 屏：安装（设置账号）", 2)
code(["UMDos installer", "Username: ______", "Password: ******"])
para("自己设一个用户名和密码（各最多 16 字符，密码显示为 *）。")

doc.add_heading("第 2 屏：激活", 2)
code(["Enter activation code: ______"])
para("输入 UMDOS2024，回车。成功显示 Activation OK.。"
     "输错会显示 Wrong activation code!，并在下一行打印诊断信息"
     "（len=输入长度 sha=算出的哈希前 8 字节），可据此判断是输入问题还是其它。")

doc.add_heading("第 3 屏：图形登录", 2)
picture("1-login.png", "图 1  图形登录界面（320×200×256，8×8 字库）")
para("输入刚才设的用户名，Tab 切到密码框，输入密码，回车。")
picture("2-login-ok.png", "图 2  登录成功")

doc.add_heading("第 4 屏：初始化任务", 2)
picture("3-hello.png", "图 3  四项初始化任务与进度条")
table(["任务", "内容"], [
    ["1", "网络：检测是否存在 PXE（没有网卡/PXE 会显示 offline mode）"],
    ["2", "CPU：用 CPUID 读厂商字符串"],
    ["3", "时间：从 CMOS RTC 读时分秒"],
    ["4", "图形：渲染彩色方块"],
], widths=[2.0, 14.2])

doc.add_heading("第 5 屏：64 位长模式", 2)
picture("4-longmode.png", "图 4  64 位长模式与 NextMain")
para("系统切换到 VBE 线性帧缓冲（优先 1280×720×32），进入 64 位长模式，"
     "最后加载 NextMain 并停机。看到 NextMain: 64-bit OK 就说明整条链路跑通了。")

doc.add_heading("五、常见问题", 1)
table(["现象", "原因与处理"], [
    ["「未能启动虚拟电脑 / 没有可引导介质」",
     "虚拟机没找到启动设备。检查：是否把 .img 直接当硬盘挂了（VBox 不认，要先转 VDI）；"
     "光驱是否挂在 IDE 控制器上；启动顺序是否包含该设备；是否开着 EFI。"],
    ["出现 Entering graphics mode... 之后 320×200 全黑",
     "显卡问题：图形控制器改成 VBoxVGA、关闭 3D 加速、显存 ≥ 16MB。"],
    ["Wrong activation code! 但输入没错",
     "确认用最新镜像；新版会打印 len= / sha=，len=09 sha=72B6E915… 才说明输入和哈希都对。"],
    ["Disk error!", "镜像没读出来。检查镜像完整性、启动顺序、U 盘是否写坏。"],
    ["开机直接重启 / 黑屏无输出", "大概率 EFI 还开着，或 CPU 不支持 64 位（长模式那一步）。"],
    ["VBE: no suitable linear framebuffer mode!",
     "虚拟显卡不支持 32bpp 线性帧缓冲；换 VBoxVGA 或 QEMU -vga std。"],
    ["鼠标不动", "鼠标是可选功能：没有 PS/2 鼠标或初始化失败会自动降级为键盘操作，不影响登录。"],
], widths=[6.0, 10.2])

doc.add_heading("六、重新安装 / 清除已设账号", 1)
para("激活状态和账号哈希存在磁盘的 LBA 6、LBA 7 两个扇区。")
bullet("最省事：换一份新的镜像文件，重新走一遍首次启动流程。")
bullet("已有镜像想清空（会回到「第一次启动」状态）：")
code(["dd if=/dev/zero of=/dev/sdX bs=512 seek=6 count=2 conv=notrunc"])
bullet("只读介质（光盘）本来就不保存，每次启动都是全新流程。")

doc.add_heading("七、想改激活码", 1)
bullet("算出新激活码的哈希：")
code(["python tools/hashcode.py 你的新激活码"])
bullet("用它打印的 4 行 db 替换 src/loader.asm 里 expected_activate: 后面的 4 行。")
bullet("重新构建：python build.py（需要 NASM 和 pip install pycdlib），再重新生成镜像。")

doc.add_heading("八、技术参数", 1)
table(["项目", "值"], [
    ["引导方式", "Legacy BIOS / El Torito 软盘仿真 / 整盘引导"],
    ["登录与任务界面", "VGA 320×200×256，8×8 字库"],
    ["最终界面", "VBE 线性帧缓冲，优先 1280×720×32（退回任意 32bpp）"],
    ["密码保存", "SHA-256（无盐，仅演示用途）"],
    ["阶段", "boot → loader → head → hello → firstmain → nextmain"],
    ["镜像大小", "ISO 约 1.5MB；硬盘镜像 16MB；软盘镜像 1.44MB"],
], widths=[4.2, 12.0])

note = doc.add_paragraph(
    "说明：这是教学/演示系统，没有文件系统、没有用户态、没有 IDT，任何异常都会重启；"
    "密码哈希无盐，不能当作真实安全机制使用。")
note.runs[0].font.size = Pt(9)
note.runs[0].font.color.rgb = RGBColor(0x66, 0x66, 0x66)

doc.save(OUT)
print("saved", OUT, os.path.getsize(OUT), "bytes")
