/* user.c - ELF64 加载与 ring3 进入（P6） */
#include "user.h"
#include "io.h"
#include "mem.h"
#include "gdt.h"
#include "syscall.h"
#include "vfs.h"

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed)) elf64_hdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} __attribute__((packed)) elf64_phdr;

static int g_exit;

int user_last_exit(void) { return g_exit; }

static int elf_load(const u8 *data, u32 len, u64 *entry)
{
    if (len < sizeof(elf64_hdr)) return -1;
    const elf64_hdr *h = (const elf64_hdr *)data;
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' || h->e_ident[2] != 'L' || h->e_ident[3] != 'F')
        return -1;
    if (h->e_phentsize != sizeof(elf64_phdr)) return -1;
    if (h->e_phoff + (u64)h->e_phnum * h->e_phentsize > len) return -1;

    for (u16 i = 0; i < h->e_phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(data + h->e_phoff + (u64)i * h->e_phentsize);
        if (ph->p_type != 1) continue;              /* PT_LOAD */
        if (!ph->p_memsz) continue;

        u64 start = ph->p_vaddr & ~0xFFFull;
        u64 end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFull;
        for (u64 va = start; va < end; va += 4096) {
            void *pa = pmm_alloc();
            if (!pa) return -2;
            if (vmm_map_page(va, (u64)pa, 0x07) != 0) return -3;
        }
        /* 文件内容拷进用户地址（页表已就绪，内核可直接写用户虚拟地址） */
        for (u64 k = 0; k < ph->p_filesz; k++)
            ((u8 *)(ph->p_vaddr + k))[0] = data[ph->p_offset + k];
        /* BSS 清零 */
        for (u64 k = ph->p_filesz; k < ph->p_memsz; k++)
            ((u8 *)(ph->p_vaddr + k))[0] = 0;
    }
    *entry = h->e_entry;
    return 0;
}

int user_run(const void *elf, u32 len, u32 *entry_out, u32 *rsp_out)
{
    u64 entry = 0;
    int rc = elf_load((const u8 *)elf, len, &entry);
    if (rc != 0) {
        serial_puts("[kernel] user: ELF load failed\n");
        return rc;
    }

    u64 stack_top = USER_BASE + 0x00200000ull;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        u64 va = stack_top - (u64)(i + 1) * 4096ull;
        void *pa = pmm_alloc();
        if (!pa) return -4;
        if (vmm_map_page(va, (u64)pa, 0x07) != 0) return -5;
    }
    u64 ustack = stack_top - 16;

    serial_puts("[kernel] user: entry=0x");
    char b[24];
    itou(entry, b, 16, 0);
    serial_puts(b);
    serial_puts(" rsp=0x");
    itou(ustack, b, 16, 0);
    serial_puts(b);
    serial_puts("\n");

    if (entry_out) *entry_out = (u32)entry;
    if (rsp_out) *rsp_out = (u32)ustack;

    tss_set_rsp0(0x90000);          /* 进入内核态时切到主栈 */
    g_exit = 0;
    user_enter(entry, ustack);      /* 不返回；exit 时经 user_leave_to_kernel 回来 */
    return g_exit;
}

/* 自检：Unicorn 没有 GDT/IDT 模型，iretq 的 CPL 切换做不了，
 * 这里把除“真正进入 ring3”之外的环节都跑一遍：
 * ELF 解析 + 页表映射 + 用户栈 + 用户指针校验 + 系统调用分发。 */
int user_selftest(const void *elf, u32 len, u32 *entry_out)
{
    u64 entry = 0;
    int rc = elf_load((const u8 *)elf, len, &entry);
    if (rc != 0) return rc;
    if (entry_out) *entry_out = (u32)entry;

    u64 stack_top = USER_BASE + 0x00200000ull;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        u64 va = stack_top - (u64)(i + 1) * 4096ull;
        void *pa = pmm_alloc();
        if (!pa) return -4;
        if (vmm_map_page(va, (u64)pa, 0x07) != 0) return -5;
    }

    /* 入口处应该是 ELF 的代码（证明映射与内容拷贝正确） */
    /* 入口页必须已映射（页表项存在） */
    int code_ok = vmm_is_mapped(entry);
    serial_puts("[kernel] selftest: entry page mapped=");
    serial_putc((char)('0' + code_ok));
    serial_puts("\n");

    /* 走一遍 write：把消息拷进用户页，再按用户指针提交 */
    const char *msg = "ring3(self-test): hello from user mode\n";
    u64 n = 0;
    while (msg[n]) n++;
    u64 upage = USER_BASE + 0x001F0000ull;
    void *pa = pmm_alloc();
    if (!pa || vmm_map_page(upage, (u64)pa, 0x07) != 0) return -6;
    u8 *ubuf = (u8 *)upage;
    for (u64 i = 0; i < n; i++) ubuf[i] = (u8)msg[i];

    u64 regs[16];
    for (int i = 0; i < 16; i++) regs[i] = 0;
    regs[0] = SYS_write;
    regs[5] = 1;                    /* rdi = fd（标准输出） */
    regs[4] = upage;                /* rsi = buf */
    regs[3] = n;                    /* rdx = len */
    syscall_dispatch(regs);
    int write_ok = (regs[0] == n);

    regs[0] = SYS_getpid;
    syscall_dispatch(regs);
    int pid_ok = (regs[0] < 16);

    regs[0] = SYS_write;
    regs[5] = 1;
    regs[4] = 0x1000;               /* 内核地址：必须被拒绝 */
    regs[3] = 4;
    syscall_dispatch(regs);
    int guard_ok = ((long)regs[0] == -1);

    /* 通过系统调用打开并读取 /config/auth（P7 的 VFS） */
    char *upath = (char *)(USER_BASE + 0x001F1000ull);
    void *ppa = pmm_alloc();
    int vfs_ok = 0;
    if (ppa && vmm_map_page(USER_BASE + 0x001F1000ull, (u64)ppa, 0x07) == 0) {
        const char *path = "/config/auth";
        int k = 0;
        while (path[k]) { upath[k] = path[k]; k++; }
        upath[k] = 0;
        u64 r2[16];
        for (int i = 0; i < 16; i++) r2[i] = 0;
        r2[0] = SYS_open;
        r2[5] = (u64)(u64)upath;        /* rdi = path */
        r2[4] = VFS_O_RDONLY;           /* rsi = flags */
        syscall_dispatch(r2);
        long fd = (long)r2[0];
        if (fd >= 0) {
            u64 rfd = (u64)fd;
            for (int i = 0; i < 16; i++) r2[i] = 0;
            r2[0] = SYS_read;
            r2[5] = rfd;                /* rdi = fd */
            r2[4] = (u64)(u64)ubuf;     /* rsi = buf */
            r2[3] = 64;                 /* rdx = len */
            syscall_dispatch(r2);
            vfs_ok = (r2[0] > 0);
            for (int i = 0; i < 16; i++) r2[i] = 0;
            r2[0] = SYS_close;
            r2[5] = rfd;
            syscall_dispatch(r2);
        }
    }

    int ok = code_ok && write_ok && pid_ok && guard_ok && vfs_ok;
    serial_puts("[kernel] selftest flags: code/write/pid/guard/vfs=");
    serial_putc((char)('0' + code_ok)); serial_putc((char)('0' + write_ok));
    serial_putc((char)('0' + pid_ok)); serial_putc((char)('0' + guard_ok));
    serial_putc((char)('0' + vfs_ok));
    serial_puts("\n");
    serial_puts(ok ? "[kernel] user selftest: elf/map/syscall ok\n"
                   : "[kernel] user selftest: FAILED\n");
    return ok ? 0 : -7;
}
