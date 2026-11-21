/* proc.c - 进程控制块、fork 雏形、/proc 生成（P10）
 *
 * 现状：
 *   - 每个进程有独立 PCB 与 fd 表（fd 表通过 kmalloc 分配，32 项）；
 *   - 内核本身是 0 号进程，演示线程挂在其下；
 *   - fork() 克隆 PCB 与 fd 表，子进程进入 READY（用户态抢占调度留到 P11）；
 *   - /proc 是 VFS 里的只读虚拟目录：version / meminfo / tasks / mounts。
 */
#include "proc.h"
#include "io.h"
#include "mem.h"
#include "task.h"

static proc_t g_procs[PROC_MAX];
static proc_t *g_cur;
static u32 g_next_pid = 1;

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void sset(char *dst, const char *src, u32 cap)
{
    u32 i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static char *append(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *append_num(char *p, char *end, u64 v, int base)
{
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v && n < 23) {
        u32 d = (u32)(v % (u64)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (u64)base;
    }
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

void proc_init(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        g_procs[i].used = 0;
        g_procs[i].fds = 0;
    }
    /* 0 号进程：内核 */
    proc_t *p = &g_procs[0];
    p->used = 1;
    p->pid = 0;
    p->ppid = 0;
    sset(p->name, "kernel", sizeof(p->name));
    p->state = PROC_RUNNING;
    p->fds = vfs_kernel_set();
    g_cur = p;
    g_next_pid = 1;
    serial_puts("[kernel] proc: kernel pid=0, /proc ready\n");
}

proc_t *proc_current(void) { return g_cur; }
void proc_set_current(proc_t *p) { g_cur = p; }

proc_t *proc_find(u32 pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (g_procs[i].used && g_procs[i].pid == pid) return &g_procs[i];
    return 0;
}

proc_t *proc_at(int index)
{
    if (index < 0) return 0;
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!g_procs[i].used) continue;
        if (n == index) return &g_procs[i];
        n++;
    }
    return 0;
}

int proc_count(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_procs[i].used) n++;
    return n;
}

static proc_t *proc_alloc(void)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (!g_procs[i].used) return &g_procs[i];
    return 0;
}

int proc_spawn(const char *name)
{
    proc_t *p = proc_alloc();
    if (!p) return -1;
    for (u32 k = 0; k < sizeof(*p); k++) ((u8 *)p)[k] = 0;
    p->used = 1;
    p->pid = g_next_pid++;
    p->ppid = g_cur ? g_cur->pid : 0;
    sset(p->name, name, sizeof(p->name));
    p->state = PROC_READY;
    p->fds = (vfs_fdset_t *)kmalloc(sizeof(vfs_fdset_t));
    if (!p->fds) {
        p->used = 0;
        return -1;
    }
    /* 复制父进程 fd 表 */
    if (g_cur && g_cur->fds) {
        for (u32 i = 0; i < sizeof(vfs_fdset_t); i++)
            ((u8 *)p->fds)[i] = ((const u8 *)g_cur->fds)[i];
    } else {
        for (u32 i = 0; i < sizeof(vfs_fdset_t); i++) ((u8 *)p->fds)[i] = 0;
    }
    return (int)p->pid;
}

int proc_fork(void)
{
    proc_t *parent = g_cur;
    char nm[16];
    sset(nm, parent ? parent->name : "child", sizeof(nm));
    int pid = proc_spawn(nm);
    if (pid < 0) return -1;
    serial_puts("[kernel] proc: fork -> pid=");
    char b[16];
    itou((u64)pid, b, 10, 0);
    serial_puts(b);
    serial_puts("\n");
    return pid;
}

/* ---------------- /proc ---------------- */
#define PROC_BUF 512

u32 proc_format_tasks(char *buf, u32 cap)
{
    char *p = buf, *end = buf + cap - 1;
    p = append(p, end, "PID  PPID STATE   NAME\n");
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *pr = &g_procs[i];
        if (!pr->used) continue;
        p = append_num(p, end, pr->pid, 10);
        p = append(p, end, "    ");
        p = append_num(p, end, pr->ppid, 10);
        p = append(p, end, "    ");
        const char *st = pr->state == PROC_RUNNING ? "RUN    " :
                         pr->state == PROC_READY ? "READY  " :
                         pr->state == PROC_EXITED ? "EXITED " : "FREE   ";
        p = append(p, end, st);
        p = append(p, end, pr->name);
        p = append(p, end, "\n");
    }
    *p = 0;
    return (u32)(p - buf);
}

u32 proc_format_meminfo(char *buf, u32 cap)
{
    char *p = buf, *end = buf + cap - 1;
    p = append(p, end, "frames_total ");
    p = append_num(p, end, mem_frames_total(), 10);
    p = append(p, end, "\nframes_free ");
    p = append_num(p, end, mem_frames_free(), 10);
    p = append(p, end, "\nheap_kb ");
    p = append_num(p, end, mem_heap_size() / 1024, 10);
    p = append(p, end, "\n");
    *p = 0;
    return (u32)(p - buf);
}

u32 proc_format_version(char *buf, u32 cap)
{
    char *p = buf, *end = buf + cap - 1;
    p = append(p, end, "UMDos 64-bit\n");
    p = append(p, end, "kernel: 64-bit\n");
    *p = 0;
    return (u32)(p - buf);
}

u32 proc_format_mounts(char *buf, u32 cap)
{
    char *p = buf, *end = buf + cap - 1;
    p = append(p, end, "umdfs  /       v2 fs02\n");
    p = append(p, end, "proc   /proc   virtual\n");
    p = append(p, end, "devfs  /dev    virtual\n");
    *p = 0;
    return (u32)(p - buf);
}

/* ---------------- 自检 ---------------- */
int proc_selftest(void)
{
    int ok = 1;

    u32 before = (u32)proc_count();
    int cpid = proc_fork();
    int fork_ok = (cpid > 0) && (proc_count() == (int)before + 1);
    ok &= fork_ok;

    char buf[PROC_BUF];
    u32 n = proc_format_tasks(buf, sizeof(buf));
    int tasks_ok = (n > 0);
    /* 文本里必须能找到父进程和子进程的 pid */
    int found_child = 0;
    for (u32 i = 0; i + 1 < n; i++) {
        if (buf[i] == ' ' || !(buf[i] >= '0' && buf[i] <= '9')) continue;
        u32 v = 0;
        u32 j = i;
        while (j < n && buf[j] >= '0' && buf[j] <= '9') {
            v = v * 10 + (u32)(buf[j] - '0');
            j++;
        }
        if (v == (u32)cpid) found_child = 1;
    }
    ok &= (tasks_ok && found_child);

    /* fd 表克隆：子进程的 fd 0 也必须是打开状态 */
    proc_t *child = proc_find((u32)cpid);
    int fds_ok = (child && child->fds && child->fds->fd[0].used);   ok &= fds_ok;

    /* /proc 各文件都能生成内容 */
    u32 m = proc_format_meminfo(buf, sizeof(buf));
    u32 v = proc_format_version(buf, sizeof(buf));
    u32 mt = proc_format_mounts(buf, sizeof(buf));
    int gen_ok = (m > 0 && v > 0 && mt > 0);
    ok &= gen_ok;

    serial_puts("[kernel] proc selftest: ");
    serial_puts(ok ? "fork/fd/tasks/proc ok" : "FAILED");
    serial_puts(" (fork_ok=");
    serial_putc((char)('0' + fork_ok));
    serial_puts(" tasks=");
    serial_putc((char)('0' + (tasks_ok && found_child)));
    serial_puts(" fds=");
    serial_putc((char)('0' + fds_ok));
    serial_puts(" proc=");
    serial_putc((char)('0' + gen_ok));
    serial_puts(")\n");
    return ok ? 0 : -1;
}
