/* sh.c - 命令行 shell（P11）
 *
 * 命令：help ls cd pwd cat echo mkdir rm cp mv ps whoami free reboot exit
 * 所有输出经 term_puts 同时镜像到串口，便于自动测试。
 */
#include "sh.h"
#include "term.h"
#include "vfs.h"
#include "fs.h"
#include "proc.h"
#include "mem.h"
#include "auth.h"
#include "acpi.h"

static char g_cwd[64] = "/";

const char *sh_cwd(void) { return g_cwd; }

void sh_set_cwd(const char *dir)
{
    int i = 0;
    while (dir[i] && i < 63) {
        g_cwd[i] = dir[i];
        i++;
    }
    g_cwd[i] = 0;
    if (!g_cwd[0]) {
        g_cwd[0] = '/';
        g_cwd[1] = 0;
    }
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static int scmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

static int starts(const char *s, const char *p)
{
    while (*p) {
        if (*s != *p) return 0;
        s++;
        p++;
    }
    return 1;
}

static void num_out(u64 v)
{
    char b[24];
    int n = 0;
    if (!v) b[n++] = '0';
    while (v) {
        b[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) term_putc(b[--n]);
}

/* 相对路径 -> 绝对路径（处理 . 与 ..） */
static void abs_path(const char *arg, char *out, u32 cap)
{
    char tmp[128];
    u32 n = 0;
    if (!arg || !*arg) {
        for (u32 i = 0; g_cwd[i] && n < sizeof(tmp) - 1; i++) tmp[n++] = g_cwd[i];
    } else if (arg[0] == '/') {
        for (u32 i = 0; arg[i] && n < sizeof(tmp) - 1; i++) tmp[n++] = arg[i];
    } else {
        for (u32 i = 0; g_cwd[i] && n < sizeof(tmp) - 1; i++) tmp[n++] = g_cwd[i];
        if (n == 0 || tmp[n - 1] != '/') tmp[n++] = '/';
        for (u32 i = 0; arg[i] && n < sizeof(tmp) - 1; i++) tmp[n++] = arg[i];
    }
    tmp[n] = 0;

    /* 规范化：逐段处理 */
    char segs[16][32];
    int ns = 0;
    int i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        char seg[32];
        int k = 0;
        while (tmp[i] && tmp[i] != '/' && k < 31) seg[k++] = tmp[i++];
        seg[k] = 0;
        if (scmp(seg, ".") == 0) continue;
        if (scmp(seg, "..") == 0) {
            if (ns > 0) ns--;
            continue;
        }
        if (ns < 16) {
            int j = 0;
            while (seg[j] && j < 31) {
                segs[ns][j] = seg[j];
                j++;
            }
            segs[ns][j] = 0;
            ns++;
        }
    }
    u32 p = 0;
    if (p < cap - 1) out[p++] = '/';
    for (int s = 0; s < ns; s++) {
        for (int j = 0; segs[s][j] && p + 1 < cap; j++) out[p++] = segs[s][j];
        if (s + 1 < ns && p + 1 < cap) out[p++] = '/';
    }
    out[p] = 0;
    if (!out[0]) {
        out[0] = '/';
        out[1] = 0;
    }
}

static void cmd_ls(const char *arg)
{
    char path[64];
    abs_path(arg, path, sizeof(path));
    u32 size = 0;
    int isdir = 0;
    if (vfs_stat(path, &size, &isdir) != 0) {
        term_puts("ls: not found\n");
        return;
    }
    if (!isdir) {
        term_puts("    ");
        num_out(size);
        term_puts("  ");
        const char *nm = path;
        for (const char *q = path; *q; q++)
            if (*q == '/') nm = q + 1;
        term_puts(nm);
        term_putc('\n');
        return;
    }
    char name[32];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (vfs_readdir(path, i, name, sizeof(name), &size, &isdir) != 0) break;
        term_puts(isdir ? "DIR   " : "FILE  ");
        num_out(size);
        term_puts("\t");
        term_puts(name);
        term_putc('\n');
        count++;
    }
    if (!count) term_puts("(empty)\n");
}

static void cmd_cat(const char *arg)
{
    char path[64];
    abs_path(arg, path, sizeof(path));
    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        term_puts("cat: cannot open\n");
        return;
    }
    char buf[129];
    for (;;) {
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) term_putc(buf[i]);
    }
    vfs_close(fd);
    term_putc('\n');
}

static void cmd_cp(const char *src, const char *dst)
{
    char sp[64], dp[64];
    abs_path(src, sp, sizeof(sp));
    abs_path(dst, dp, sizeof(dp));
    int sfd = vfs_open(sp, VFS_O_RDONLY);
    if (sfd < 0) {
        term_puts("cp: cannot open source\n");
        return;
    }
    static u8 data[4096];
    int total = 0;
    for (;;) {
        int n = vfs_read(sfd, data + total, 256);
        if (n <= 0) break;
        total += n;
        if (total > (int)sizeof(data) - 256) break;
    }
    vfs_close(sfd);
    int dfd = vfs_open(dp, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (dfd < 0) {
        term_puts("cp: cannot create dest\n");
        return;
    }
    if (total > 0) vfs_write(dfd, data, (u32)total);
    vfs_close(dfd);
    term_puts("cp: ok (");
    num_out((u64)total);
    term_puts(" bytes)\n");
}

int sh_exec_line(const char *line)
{
    while (*line == ' ') line++;
    if (!*line) return 0;

    char cmd[32];
    int n = 0;
    while (line[n] && line[n] != ' ' && n < 31) {
        cmd[n] = line[n];
        n++;
    }
    cmd[n] = 0;
    const char *rest = line + n;
    while (*rest == ' ') rest++;

    /* 第二个参数（用于 cp/mv） */
    char a2[64];
    int k = 0;
    const char *r2 = rest;
    while (*r2 && *r2 != ' ' && k < 63) a2[k++] = *r2++;
    a2[k] = 0;
    while (*r2 == ' ') r2++;

    if (scmp(cmd, "help") == 0) {
        term_puts("commands: help ls cd pwd cat echo mkdir rm cp mv ps whoami free reboot exit\n");
    } else if (scmp(cmd, "ls") == 0) {
        cmd_ls(rest);
    } else if (scmp(cmd, "cd") == 0) {
        char path[64];
        abs_path(rest, path, sizeof(path));
        u32 size;
        int isdir = 0;
        if (vfs_stat(path, &size, &isdir) != 0 || !isdir) {
            term_puts("cd: no such directory\n");
        } else {
            sh_set_cwd(path);
        }
    } else if (scmp(cmd, "pwd") == 0) {
        term_puts(g_cwd);
        term_putc('\n');
    } else if (scmp(cmd, "cat") == 0) {
        cmd_cat(rest);
    } else if (scmp(cmd, "echo") == 0) {
        term_puts(rest);
        term_putc('\n');
    } else if (scmp(cmd, "mkdir") == 0) {
        char path[64];
        abs_path(rest, path, sizeof(path));
        if (vfs_mkdir(path) == 0) term_puts("mkdir: ok\n");
        else term_puts("mkdir: failed\n");
    } else if (scmp(cmd, "rm") == 0) {
        char path[64];
        abs_path(rest, path, sizeof(path));
        if (vfs_unlink(path) == 0) term_puts("rm: ok\n");
        else term_puts("rm: failed (missing or permission)\n");
    } else if (scmp(cmd, "cp") == 0) {
        cmd_cp(rest, r2);
    } else if (scmp(cmd, "mv") == 0) {
        cmd_cp(rest, r2);
        char path[64];
        abs_path(rest, path, sizeof(path));
        if (vfs_unlink(path) == 0) term_puts("mv: ok\n");
    } else if (scmp(cmd, "ps") == 0) {
        char buf[384];
        proc_format_tasks(buf, sizeof(buf));
        term_puts(buf);
    } else if (scmp(cmd, "whoami") == 0) {
        term_puts(auth_name());
        term_puts(vfs_role() == 1 ? " (administrator)\n" : " (user)\n");
    } else if (scmp(cmd, "free") == 0) {
        char buf[192];
        proc_format_meminfo(buf, sizeof(buf));
        term_puts(buf);
    } else if (scmp(cmd, "reboot") == 0) {
        term_puts("reboot: ACPI reset\n");
        acpi_reboot();
    } else if (scmp(cmd, "shutdown") == 0) {
        term_puts("shutdown: ACPI S5\n");
        acpi_shutdown();
    } else if (scmp(cmd, "exit") == 0) {
        return SH_EXIT;
    } else {
        term_puts("unknown command: ");
        term_puts(cmd);
        term_puts("  (type 'help')\n");
    }
    return 0;
}

/* VFS path -> classic display path: "/" => "C:\\UMDos\\User" */
static void disp_path(const char *p, char *out, u32 cap)
{
    char tmp[96];
    u32 n = 0;
    const char *pre = "C:\\UMDos\\User";
    while (*pre && n + 1 < sizeof(tmp)) tmp[n++] = *pre++;
    if (p && p[0] == '/') {
        for (u32 i = 1; p[i] && n + 1 < sizeof(tmp); i++)
            tmp[n++] = (p[i] == '/') ? '\\' : p[i];
    }
    tmp[n] = 0;
    u32 k = 0;
    while (tmp[k] && k + 1 < cap) { out[k] = tmp[k]; k++; }
    out[k] = 0;
}

void sh_prompt(void)
{
    char disp[96];
    disp_path(g_cwd, disp, sizeof(disp));
    term_print_color("PS ", 0x003F9E3Fu);       /* low-saturation green */
    term_print_color(disp, 0x005A9FD4u);          /* low-saturation blue */
    term_print_color("> ", 0x00FFFFFFu);        /* white */
}
