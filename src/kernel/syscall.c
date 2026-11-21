/* syscall.c - int 0x80 系统调用（P5/P6）
 *
 * 约定：rax = 调用号，rdi/rsi/rdx/r10 = 参数，返回值写回 rax。
 * 用户指针只做上界检查（落在 USER_BASE..USER_LIMIT 之内），
 * 真正的缺页处理要等 P7 之后的内存管理完善。
 */
#include "syscall.h"
#include "io.h"
#include "task.h"
#include "mem.h"
#include "user.h"
#include "vfs.h"
#include "proc.h"

#define USER_LIMIT (USER_BASE + 0x20000000ull)

static u32 g_syscalls;

u32 syscall_count(void) { return g_syscalls; }

static int user_ptr_ok(u64 p, u64 len)
{
    if (p < USER_BASE) return 0;
    if (p + len < p) return 0;
    return (p + len) <= USER_LIMIT;
}

void syscall_dispatch(u64 *regs)
{
    u64 vf = vfs_lock();
    u64 num = regs[0];        /* rax */
    u64 a1 = regs[5];         /* rdi */
    u64 a2 = regs[4];         /* rsi */
    u64 a3 = regs[3];         /* rdx */
    long ret = -1;
    g_syscalls++;

    switch (num) {
    case SYS_write:
        if (!user_ptr_ok(a2, a3 ? a3 : 1)) {
            ret = -1;
            break;
        }
        if (a3 > 4096) a3 = 4096;
        ret = vfs_write((int)a1, (const void *)(u64)a2, (u32)a3);
        break;
    case SYS_getpid:
        ret = task_current() ? (long)task_current()->id : -1;
        break;
    case SYS_sleep: {
        u64 n = a1 ? a1 : 1;
        if (n > 100) n = 100;
        for (u64 i = 0; i < n; i++) {
            for (volatile u64 j = 0; j < 200000; j++)
                ;
            sched_yield();
        }
        ret = 0;
        break;
    }
    case SYS_brk: {
        static u64 user_brk = 0x0000008000000000ull + 0x00100000ull;
        if (a1) user_brk = a1;
        ret = (long)user_brk;
        break;
    }
    case SYS_open: {
        /* 路径在用户内存里：先做上界检查，再拷进内核缓冲区 */
        char path[64];
        if (!user_ptr_ok(a1, 1)) {
            ret = -1;
            break;
        }
        const char *up = (const char *)(u64)a1;
        int k = 0;
        while (k < 63 && up[k]) {
            path[k] = up[k];
            k++;
        }
        path[k] = 0;
        if (!user_ptr_ok(a1, (u64)k + 1)) {
            ret = -1;
            break;
        }
        ret = vfs_open(path, (int)a2);
        break;
    }
    case SYS_read:
        if (!user_ptr_ok(a2, a3 ? a3 : 1)) {
            ret = -1;
            break;
        }
        if (a3 > 4096) a3 = 4096;
        ret = vfs_read((int)a1, (void *)(u64)a2, (u32)a3);
        break;
    case SYS_close:
        ret = vfs_close((int)a1);
        break;
    case SYS_fork:
        ret = proc_fork();
        break;
    case SYS_getppid:
        ret = proc_current() ? (long)proc_current()->ppid : -1;
        break;
    case SYS_exec:
        ret = -1;               /* 从 VFS 加载可执行文件留到 P11 */
        break;
    case SYS_exit:
        serial_puts("[kernel] syscall exit code=");
        {
            char b[16];
            itou(a1, b, 10, 0);
            serial_puts(b);
        }
        serial_puts("\n");
        vfs_unlock(vf);
        user_leave_to_kernel((int)a1);   /* 不会返回 */
        ret = 0;
        break;
    default:
        ret = -1;
        break;
    }
    regs[0] = (u64)ret;
    vfs_unlock(vf);
}
