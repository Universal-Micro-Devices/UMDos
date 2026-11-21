/* vfs.c - VFS 层：路径解析、文件描述符、UMDFS 适配、/dev 路由（P7/P8）
 *
 * 设计：
 *   - 每个 fd 记录路径、父目录索引、目录项索引、偏移、标志；
 *   - 路径解析走 UMDFS 的 fs_set_dir/fs_find，解析前后恢复当前目录；
 *   - "/dev/*" 路由到 dev.c 的设备；
 *   - 文件写入按“读旧内容 + 打补丁 + 整文件写回”实现（上限 4KB），
 *     真正的块级增量写等 P9–P10 的文件系统重构。
 */
#include "vfs.h"
#include "fs.h"
#include "dev.h"
#include "io.h"
#include "proc.h"
#include "spinlock.h"
#include "mem.h"

typedef vfs_fd_t fd_t;

static int write_allowed(const char *path);   /* P12：/config 写保护 */

static vfs_fdset_t g_kernel_set;
static vfs_fdset_t *g_cur = &g_kernel_set;
static int  g_open_count;

vfs_fdset_t *vfs_current_set(void) { return g_cur; }
void vfs_select_set(vfs_fdset_t *set) { g_cur = set ? set : &g_kernel_set; }
vfs_fdset_t *vfs_kernel_set(void) { return &g_kernel_set; }

static fd_t *fd_get(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FD) return 0;
    return g_cur->fd[fd].used ? &g_cur->fd[fd] : 0;
}

static int fd_alloc(void)
{
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!g_cur->fd[i].used) {
            for (u32 k = 0; k < sizeof(fd_t); k++) ((u8 *)&g_cur->fd[i])[k] = 0;
            g_cur->fd[i].used = 1;
            g_cur->fd[i].index = -1;
            g_open_count++;
            return i;
        }
    }
    return -1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

/* ---------------- 路径解析 ---------------- */
/* 返回 0：parent=父目录索引，index=目录项索引（根为 -1），isdir=是否目录 */
static int resolve(const char *path, u32 *parent, int *index, int *isdir)
{
    if (!path || path[0] != '/') return -1;
    u32 saved = fs_dir();
    u32 dir = FS_ROOT_DIR;
    int idx = -1;
    int ok = -1;
    const char *p = path;
    while (*p == '/') p++;

    if (!*p) {
        *parent = FS_ROOT_DIR;
        *index = -1;
        *isdir = 1;
        fs_set_dir(saved);
        return 0;
    }

    for (;;) {
        char comp[VFS_NAME_MAX];
        int n = 0;
        while (p[n] && p[n] != '/' && n < VFS_NAME_MAX - 1) {
            comp[n] = p[n];
            n++;
        }
        comp[n] = 0;

        fs_set_dir(dir);
        idx = fs_find(comp);
        if (idx < 0) break;
        const fs_entry_t *e = fs_at(idx);
        int is_d = e ? fs_is_dir(e) : 0;
        p += n;
        while (*p == '/') p++;
        if (!*p) {
            *parent = dir;
            *index = idx;
            *isdir = is_d;
            ok = 0;
            break;
        }
        if (!is_d) break;
        dir = (u32)idx;
    }
    fs_set_dir(saved);
    return ok;
}

static int resolve_dir(const char *path, u32 *out)
{
    if (!path) return -1;
    if (path[0] != '/') return -1;
    if (path[1] == 0) {
        *out = FS_ROOT_DIR;
        return 0;
    }
    u32 parent;
    int index, isdir;
    if (resolve(path, &parent, &index, &isdir) != 0) return -1;
    if (!isdir) return -1;
    *out = (u32)index;
    return 0;
}

static void split_parent(const char *path, char *pp, char *name)
{
    int len = (int)slen(path);
    int i = len - 1;
    while (i > 0 && path[i] != '/') i--;
    int k = 0;
    for (int j = i + 1; j < len && k < VFS_NAME_MAX - 1; j++) name[k++] = path[j];
    name[k] = 0;
    if (i <= 0) {
        pp[0] = '/';
        pp[1] = 0;
    } else {
        for (int j = 0; j < i; j++) pp[j] = path[j];
        pp[i] = 0;
    }
}

/* ---------------- 打开/关闭 ---------------- */
int vfs_open(const char *path, int flags)
{
    if (!path || path[0] != '/') return -1;
    if ((flags & VFS_O_CREAT) && !write_allowed(path)) return -1;

    /* /proc 虚拟文件（P10） */
    if (slen(path) > 6 && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
        path[4] == 'c' && path[5] == '/') {
        char *mb = (char *)kmalloc(512);
        if (!mb) return -1;
        u32 n = 0;
        const char *f = path + 6;
        if (streq(f, "version")) n = proc_format_version(mb, 512);
        else if (streq(f, "meminfo")) n = proc_format_meminfo(mb, 512);
        else if (streq(f, "tasks")) n = proc_format_tasks(mb, 512);
        else if (streq(f, "mounts")) n = proc_format_mounts(mb, 512);
        else {
            kfree(mb);
            return -1;
        }
        int fd = fd_alloc();
        if (fd < 0) {
            kfree(mb);
            return -1;
        }
        g_cur->fd[fd].type = VFS_PROC;
        g_cur->fd[fd].mbuf = mb;
        g_cur->fd[fd].mlen = n;
        int k = 0;
        for (const char *q = path; *q && k < 63; q++) g_cur->fd[fd].path[k++] = *q;
        g_cur->fd[fd].path[k] = 0;
        return fd;
    }

    /* 设备文件 */
    if (slen(path) > 5 && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        dev_t *d = dev_find(path + 5);
        if (!d) return -1;
        int fd = fd_alloc();
        if (fd < 0) return -1;
        g_cur->fd[fd].type = VFS_DEV;
        g_cur->fd[fd].dev = d;
        int k = 0;
        for (const char *q = path; *q && k < 63; q++) g_cur->fd[fd].path[k++] = *q;
        g_cur->fd[fd].path[k] = 0;
        g_cur->fd[fd].flags = flags;
        if (d->open) d->open(d);
        return fd;
    }

    u32 parent = FS_ROOT_DIR;
    int index = -1, isdir = 0;
    if (resolve(path, &parent, &index, &isdir) != 0) {
        if (!(flags & VFS_O_CREAT)) return -1;
        char pp[64], name[VFS_NAME_MAX];
        split_parent(path, pp, name);
        u32 pd;
        if (resolve_dir(pp, &pd) != 0) return -1;
        u32 saved = fs_dir();
        fs_set_dir(pd);
        int rc = fs_write(name, "", 0);
        fs_set_dir(saved);
        if (rc != 0) return -1;
        if (resolve(path, &parent, &index, &isdir) != 0) return -1;
    }

    int fd = fd_alloc();
    if (fd < 0) return -1;
    g_cur->fd[fd].type = isdir ? VFS_DIR : VFS_FILE;
    g_cur->fd[fd].dir = parent;
    g_cur->fd[fd].index = index;
    g_cur->fd[fd].flags = flags;
    int k = 0;
    for (const char *q = path; *q && k < 63; q++) g_cur->fd[fd].path[k++] = *q;
    g_cur->fd[fd].path[k] = 0;
    if (flags & VFS_O_TRUNC) {
        const fs_entry_t *e = fs_at(index);
        if (e && !isdir) {
            char name[VFS_NAME_MAX];
            char pp[64];
            split_parent(path, pp, name);
            u32 saved = fs_dir();
            fs_set_dir(parent);
            fs_write(name, "", 0);
            fs_set_dir(saved);
        }
    }
    return fd;
}

int vfs_close(int fd)
{
    fd_t *f = fd_get(fd);
    if (!f) return -1;
    if (f->type == VFS_PROC && f->mbuf) kfree(f->mbuf);
    for (u32 k = 0; k < sizeof(fd_t); k++) ((u8 *)f)[k] = 0;
    g_open_count--;
    return 0;
}

/* ---------------- 读/写/定位 ---------------- */
int vfs_read(int fd, void *buf, u32 len)
{
    fd_t *f = fd_get(fd);
    if (!f || !buf) return -1;
    if (f->type == VFS_DEV)
        return f->dev && ((dev_t *)f->dev)->read ? ((dev_t *)f->dev)->read((dev_t *)f->dev, buf, len) : -1;
    if (f->type == VFS_PROC) {
        if (f->offset >= f->mlen) return 0;
        u32 n = f->mlen - f->offset;
        if (n > len) n = len;
        for (u32 i = 0; i < n; i++) ((u8 *)buf)[i] = (u8)f->mbuf[f->offset + i];
        f->offset += n;
        return (int)n;
    }
    if (f->type != VFS_FILE) return -1;

    const fs_entry_t *e = fs_at(f->index);
    if (!e) return -1;
    const u8 *data = fs_data(e);
    if (!data) return -1;
    u32 size = e->size;
    if (f->offset >= size) return 0;
    u32 n = size - f->offset;
    if (n > len) n = len;
    for (u32 i = 0; i < n; i++) ((u8 *)buf)[i] = data[f->offset + i];
    f->offset += n;
    return (int)n;
}

int vfs_write(int fd, const void *buf, u32 len)
{
    static u8 tmp[4096];
    fd_t *f = fd_get(fd);
    if (!f || !buf) return -1;
    if (f->type == VFS_DEV)
        return f->dev && ((dev_t *)f->dev)->write ? ((dev_t *)f->dev)->write((dev_t *)f->dev, buf, len) : -1;
    if (f->type != VFS_FILE) return -1;
    if (!write_allowed(f->path)) return -1;

    const fs_entry_t *e = fs_at(f->index);
    u32 size = e ? e->size : 0;
    if (size > sizeof(tmp)) return -1;
    const u8 *old = e ? fs_data(e) : 0;
    for (u32 i = 0; old && i < size; i++) tmp[i] = old[i];

    u32 need = f->offset + len;
    if (need > sizeof(tmp)) return -1;
    for (u32 i = 0; i < len; i++) tmp[f->offset + i] = ((const u8 *)buf)[i];
    if (need > size) size = need;

    char name[VFS_NAME_MAX];
    char pp[64];
    split_parent(f->path, pp, name);
    u32 saved = fs_dir();
    fs_set_dir(f->dir);
    int rc = fs_write(name, tmp, size);
    fs_set_dir(saved);
    if (rc != 0) return -1;
    f->offset = need;
    const fs_entry_t *ne = fs_at(f->index);
    if (ne && ne->size != size) {
        /* 目录项可能被重写，索引保持不变 */
    }
    return (int)len;
}

int vfs_seek(int fd, u32 off)
{
    fd_t *f = fd_get(fd);
    if (!f) return -1;
    f->offset = off;
    return 0;
}

int vfs_size(int fd)
{
    fd_t *f = fd_get(fd);
    if (!f) return -1;
    if (f->type == VFS_FILE) {
        const fs_entry_t *e = fs_at(f->index);
        return e ? (int)e->size : -1;
    }
    return 0;
}

int vfs_type(int fd)
{
    fd_t *f = fd_get(fd);
    return f ? f->type : -1;
}

int vfs_stat(const char *path, u32 *size, int *is_dir)
{
    u32 parent;
    int index, isdir;
    if (resolve(path, &parent, &index, &isdir) != 0) return -1;
    if (size) {
        if (index < 0) *size = 0;
        else {
            const fs_entry_t *e = fs_at(index);
            *size = e ? e->size : 0;
        }
    }
    if (is_dir) *is_dir = isdir;
    return 0;
}

int vfs_readdir(const char *path, int index, char *name, u32 cap, u32 *size, int *is_dir)
{
    /* /dev 是虚拟目录 */
    if (slen(path) == 4 && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') {
        dev_t *d = dev_at(index);
        if (!d) return -1;
        u32 k = 0;
        while (d->name[k] && k < cap - 1) {
            name[k] = d->name[k];
            k++;
        }
        name[k] = 0;
        if (size) *size = 0;
        if (is_dir) *is_dir = 0;
        return 0;
    }

    if (slen(path) == 5 && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' && path[4] == 'c') {
        static const char *names[4] = { "version", "meminfo", "tasks", "mounts" };
        if (index < 0 || index > 3) return -1;
        u32 k = 0;
        while (names[index][k] && k < cap - 1) {
            name[k] = names[index][k];
            k++;
        }
        name[k] = 0;
        if (size) *size = 0;
        if (is_dir) *is_dir = 0;
        return 0;
    }

    u32 dir;
    if (resolve_dir(path, &dir) != 0) return -1;
    u32 saved = fs_dir();
    fs_set_dir(dir);
    int seen = 0;
    int rc = -1;
    /* 注意：i 必须是“全局目录项索引”，不能只用子项个数 */
    for (int i = 0; i < 64; i++) {
        const fs_entry_t *e = fs_at(i);
        if (!e || !e->used) continue;
        if (e->parent != dir) continue;
        if (seen == index) {
            u32 k = 0;
            while (e->name[k] && k < cap - 1) {
                name[k] = e->name[k];
                k++;
            }
            name[k] = 0;
            if (size) *size = e->size;
            if (is_dir) *is_dir = fs_is_dir(e);
            rc = 0;
            break;
        }
        seen++;
    }
    fs_set_dir(saved);
    return rc;
}

/* ---------------- 初始化与自检 ---------------- */
int vfs_init(void)
{
    for (int i = 0; i < VFS_MAX_FD; i++) {
        for (u32 k = 0; k < sizeof(fd_t); k++) ((u8 *)&g_kernel_set.fd[i])[k] = 0;
        g_kernel_set.fd[i].index = -1;
    }
    g_open_count = 0;

    /* 标准流：0 = 键盘，1/2 = 串口控制台 */
    dev_t *kbd = dev_find("kbd");
    dev_t *ser = dev_find("serial");
    if (kbd) {
        g_kernel_set.fd[0].used = 1;
        g_kernel_set.fd[0].type = VFS_DEV;
        g_kernel_set.fd[0].dev = kbd;
        g_kernel_set.fd[0].path[0] = 0;
        g_kernel_set.fd[0].index = -1;
        g_open_count++;
    }
    for (int i = 1; i <= 2 && ser; i++) {
        g_kernel_set.fd[i].used = 1;
        g_kernel_set.fd[i].type = VFS_DEV;
        g_kernel_set.fd[i].dev = ser;
        g_kernel_set.fd[i].path[0] = 0;
        g_kernel_set.fd[i].index = -1;
        g_open_count++;
    }
    serial_puts("[kernel] vfs: mounted umdfs at /, /dev ready, fd table 32\n");
    return 0;
}

static int contains(const char *hay, const char *needle)
{
    u32 hn = slen(hay), nn = slen(needle);
    if (nn > hn) return 0;
    for (u32 i = 0; i + nn <= hn; i++) {
        u32 j = 0;
        while (j < nn && hay[i + j] == needle[j]) j++;
        if (j == nn) return 1;
    }
    return 0;
}

int vfs_selftest(void)
{
    int ok = 1;

    /* 1) 读取 /config/auth 并检查格式 */
    int fd = vfs_open("/config/auth", VFS_O_RDONLY);
    if (fd < 0) return -1;
    char buf[257];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = 0;
    vfs_close(fd);
    int auth_ok = (n > 0 && contains(buf, "sha3-256$"));
    ok &= auth_ok;

    /* 2) 写设备：/dev/serial */
    int sfd = vfs_open("/dev/serial", VFS_O_WRONLY);
    int dev_ok = sfd >= 0;
    if (dev_ok) {
        const char *msg = "[kernel] vfs: /dev/serial write ok\n";
        u32 m = 0;
        while (msg[m]) m++;
        dev_ok = (vfs_write(sfd, msg, m) == (int)m);
        vfs_close(sfd);
    }
    ok &= dev_ok;

    /* 3) stat 目录 */
    u32 size = 0;
    int isdir = 0;
    int stat_ok = (vfs_stat("/config", &size, &isdir) == 0 && isdir == 1);
    ok &= stat_ok;

    /* 4) 根目录 readdir */
    char name[32];
    int cnt = 0;
    for (int i = 0; i < 64; i++) {
        if (vfs_readdir("/", i, name, sizeof(name), &size, &isdir) != 0) break;
        cnt++;
    }
    int dir_ok = (cnt >= 5);
    ok &= dir_ok;

    /* 5) /dev 虚拟目录 */
    int dcnt = 0;
    for (int i = 0; i < 16; i++) {
        if (vfs_readdir("/dev", i, name, sizeof(name), &size, &isdir) != 0) break;
        dcnt++;
    }
    int vdev_ok = (dcnt >= 4);
    ok &= vdev_ok;

    /* 6) 创建 + 写 + 读 + 删除 */
    int cfd = vfs_open("/VFSTEST.TXT", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    int rw_ok = 0;
    if (cfd >= 0) {
        const char *msg = "vfs rw ok";
        u32 m = 0;
        while (msg[m]) m++;
        if (vfs_write(cfd, msg, m) == (int)m) {
            vfs_close(cfd);
            int rfd = vfs_open("/VFSTEST.TXT", VFS_O_RDONLY);
            if (rfd >= 0) {
                char rb[16];
                int rn = vfs_read(rfd, rb, sizeof(rb));
                vfs_close(rfd);
                rb[rn > 0 ? rn : 0] = 0;
                if (rn == (int)m && contains(rb, "vfs rw ok")) rw_ok = 1;
            }
        } else {
            vfs_close(cfd);
        }
        u32 saved = fs_dir();
        fs_set_dir(FS_ROOT_DIR);
        fs_unlink("VFSTEST.TXT");
        fs_set_dir(saved);
    }
    ok &= rw_ok;

    /* 7) /proc：读 version 与 tasks */
    int pfd = vfs_open("/proc/version", VFS_O_RDONLY);
    int proc_ok = 0;
    if (pfd >= 0) {
        char pb[64];
        int pn = vfs_read(pfd, pb, sizeof(pb) - 1);
        vfs_close(pfd);
        if (pn > 0) {
            pb[pn] = 0;
            proc_ok = contains(pb, "UMDos");
        }
    }
    int tfd = vfs_open("/proc/tasks", VFS_O_RDONLY);
    if (tfd >= 0) {
        char tb[128];
        int tn = vfs_read(tfd, tb, sizeof(tb) - 1);
        vfs_close(tfd);
        if (tn > 0) {
            tb[tn] = 0;
            proc_ok &= contains(tb, "kernel");
        } else {
            proc_ok = 0;
        }
    } else {
        proc_ok = 0;
    }
    ok &= proc_ok;

    serial_puts(ok ? "[kernel] vfs selftest: auth/dev/stat/readdir/rw/proc ok\n"
                   : "[kernel] vfs selftest: FAILED\n");
    return ok ? 0 : -2;
}

/* ---------------- P11/P12：目录操作与权限 ---------------- */
static spinlock_t g_vfs_lock;

u64 vfs_lock(void)
{
    u64 f = irq_save();
    spin_lock(&g_vfs_lock);
    return f;
}

void vfs_unlock(u64 f)
{
    spin_unlock(&g_vfs_lock);
    irq_restore(f);
}

static int g_role = 1;          /* 0 = 普通用户，1 = 管理员；未登录时内核上下文视为管理员 */

void vfs_set_role(int role) { g_role = role ? 1 : 0; }
int  vfs_role(void) { return g_role; }

static int under_config(const char *path)
{
    if (!path || path[0] != '/') return 0;
    const char *p = path + 1;
    const char *key = "config";
    int i = 0;
    while (key[i]) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != key[i]) return 0;
        i++;
    }
    return (p[i] == 0 || p[i] == '/');
}

static int write_allowed(const char *path)
{
    if (!under_config(path)) return 1;
    return g_role == 1;         /* 普通用户不能改 /config */
}

int vfs_mkdir(const char *path)
{
    if (!path || path[0] != '/' || !write_allowed(path)) return -1;
    char pp[64], name[VFS_NAME_MAX];
    split_parent(path, pp, name);
    u32 pd;
    if (resolve_dir(pp, &pd) != 0) return -1;
    u32 saved = fs_dir();
    fs_set_dir(pd);
    int rc = fs_mkdir(name);
    fs_set_dir(saved);
    return rc == 0 ? 0 : -1;
}

int vfs_unlink(const char *path)
{
    if (!path || path[0] != '/' || !write_allowed(path)) return -1;
    char pp[64], name[VFS_NAME_MAX];
    split_parent(path, pp, name);
    u32 pd;
    if (resolve_dir(pp, &pd) != 0) return -1;
    u32 saved = fs_dir();
    fs_set_dir(pd);
    int rc = fs_unlink(name);
    fs_set_dir(saved);
    return rc == 0 ? 0 : -1;
}
