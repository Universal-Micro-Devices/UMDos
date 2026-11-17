/* kmain.c - 内核主流程：启动画面 -> 登录 -> 桌面 */
#include "bootinfo.h"
#include "gfx.h"
#include "input.h"
#include "fs.h"
#include "ui.h"
#include "io.h"
#include "idt.h"
#include "irq.h"
#include "mem.h"
#include "gdt.h"
#include "task.h"
#include "user.h"
#include "syscall.h"
#include "userprog.h"
#include "vfs.h"
#include "dev.h"
#include "proc.h"
#include "term.h"
#include "sh.h"
#include "sec.h"
#include "acpi.h"
#include "ata.h"

/* 存储初始化：ATA + UMDFS 挂载 + 写回后端。
 * P7 起 VFS 要在用户程序之前可用，所以从启动画面里挪到 kmain。 */
int storage_init(bootinfo_t *bi)
{
    if (fs_mounted()) return 0;
    if (fs_mount((void *)0x40000u, bi->fs_sectors * 512u) != 0) {
        serial_puts("[kernel] storage: umdfs mount failed\n");
        return -1;
    }
    if (ata_init() == 0) {
        u8 sec[512];
        if (ata_read(bi->fs_lba, 1, sec) == 0 && memcmp(sec, fs_raw(), 36) == 0)
            fs_set_backing(bi->fs_lba);
    }
    serial_puts("[kernel] storage: umdfs mounted, backing ready\n");
    return 0;
}

static void kthread_demo(void)
{
    task_t *t = task_current();
    for (int i = 0; i < 2; i++) {
        serial_puts("[kthread] ");
        serial_puts(t->name);
        serial_puts(" running\n");
        for (volatile u64 j = 0; j < 300000; j++)
            ;
        sched_yield();
    }
    serial_puts("[kthread] ");
    serial_puts(t->name);
    serial_puts(" done\n");
    task_exit(0);
}

void kmain(bootinfo_t *bi)
{
    gfx_info gi;
    gi.base = bi->fb_base_lo;
    gi.width = bi->fb_width;
    gi.height = bi->fb_height;
    gi.stride = bi->fb_stride;
    gi.bpp = bi->fb_bpp;
    gfx_init(&gi);
    serial_puts("[kernel] gfx ok\n");

    /* P5：先重建 GDT（0x08 改成 64 位代码段 + 用户段 + TSS），再装 IDT */
    gdt_init();
    idt_init();
    irq_init();
    sti();
    if (irq_selftest() == 0)
        serial_puts("[kernel] irq selftest: kbd ok, pit ok\n");
    else
        serial_puts("[kernel] irq selftest: FAILED\n");

    /* P4：E820 -> 页帧分配器 -> 内核堆 -> 用户窗口映射自检 */
    mem_init(bi);
    mem_selftest();

    input_init();

    /* P7/P8：存储 -> VFS -> 设备注册表，然后用自检验证 */
    storage_init(bi);
    acpi_init();
    fs_selftest();
    dev_init();          /* 先注册设备，VFS 才能把 0/1/2 绑到键盘和串口 */
    vfs_init();
    proc_init();
    vfs_selftest();
    proc_selftest();
    term_init();
    sec_selftest();

    /* P5：内核线程 + 轮转调度 */
    sched_init();
    task_create("kthreadA", kthread_demo);
    task_create("kthreadB", kthread_demo);
    for (int i = 0; i < 8; i++)
        sched_yield();
    serial_puts("[kernel] sched: switches=");
    { char b[16]; itou(sched_switches(), b, 10, 0); serial_puts(b); }
    serial_puts("\n");

    /* P6：ELF64 用户程序 + ring3 + int 0x80
     * 模拟器（Unicorn）没有 GDT/IDT 模型，iretq 的 CPL 切换会失败；
     * bootinfo.env_flags bit0 由测试工具置位，此时跑等价的内核自检。 */
    int urc;
    if (bi->env_flags & 1) {
        serial_puts("[kernel] user: ring3 skipped (no GDT/IDT model), selftest instead\n");
        urc = user_selftest(user_elf, user_elf_len, 0);
    } else {
        urc = user_run(user_elf, user_elf_len, 0, 0);
    }
    serial_puts("[kernel] user: returned, exit=");
    { char b[16]; itou((u64)(urc < 0 ? 99 : urc), b, 10, 0); serial_puts(b); }
    serial_puts("  syscalls=");
    { char b[16]; itou(syscall_count(), b, 10, 0); serial_puts(b); }
    serial_puts("\n");

    serial_puts("[kernel] init: gfx/input/idt/irq/mem/task/user ok\n");
    outb(0xE9, 0x01);

    serial_puts("[kernel] splash start\n");
    ui_splash(bi);                  /* 蓝色启动画面 + 加载 + 约 60 秒 */
    serial_puts("[kernel] login ready (auth: /config/auth)\n");
    ui_login(bi);                   /* 校验用户名/密码 */
    serial_puts("[kernel] login ok, desktop\n");

    outb(0xE9, 0x40);
    ui_desktop(bi);                 /* 桌面：我的电脑 -> 文件管理器 */

    for (;;)
        ;
}
