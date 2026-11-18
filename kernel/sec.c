/* sec.c - P12 安全自检：I/O 权限、页面 U 位、/config 写保护 */
#include "sec.h"
#include "gdt.h"
#include "mem.h"
#include "vfs.h"
#include "io.h"

int sec_selftest(void)
{
    /* 1) TSS 没有 I/O 位图（iomap_base 指向 TSS 末尾）：ring3 执行 in/out 会 #GP */
    int iomap_ok = (tss_iomap_base() >= 104);

    /* 2) 用户页必须带 U 位；内核映像页不能带 U 位。
     * 自检早于用户程序时用户窗口还没映射，这里临时映射一页做检查。 */
    int probe = 0;
    if (!vmm_is_mapped(USER_BASE)) {
        void *pa = pmm_alloc();
        if (pa && vmm_map_page(USER_BASE, (u64)pa, 0x07) == 0) probe = 1;
    }
    u64 userf = vmm_pte_flags(USER_BASE);
    u64 kernf = vmm_pte_flags(0x10000u);
    int user_ok = (userf & 4u) != 0;
    int kern_ok = (kernf & 4u) == 0;

    /* 3) 普通用户不能写 /config */
    vfs_set_role(0);
    int denied = (vfs_open("/config/SECTEST.TXT",
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC) < 0);
    vfs_set_role(1);

    if (probe) vmm_unmap_page(USER_BASE);

    int ok = iomap_ok && user_ok && kern_ok && denied;
    serial_puts("[kernel] security selftest: ");
    serial_puts(ok ? "iomap/userpage/config-perm ok" : "FAILED");
    serial_puts(" (iomap=");
    serial_putc((char)('0' + iomap_ok));
    serial_puts(" user=");
    serial_putc((char)('0' + user_ok));
    serial_puts(" kern=");
    serial_putc((char)('0' + kern_ok));
    serial_puts(" cfg=");
    serial_putc((char)('0' + denied));
    serial_puts(")\n");
    return ok ? 0 : -1;
}
