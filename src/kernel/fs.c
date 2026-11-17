/* fs.c - UMDFS：超级块 + 目录项表（支持子目录）+ 块位图 + 数据区 */
#include "fs.h"
#include "io.h"
#include "ata.h"

#define ROOT_DIR 0xFFFFFFFFu

typedef struct {
    u8  magic[8];
    u32 block_size;
    u32 block_count;
    u32 file_count;
    u32 data_start;
    u32 dirent_block;
    u32 dirent_count;
    u32 bitmap_block;
    u32 magic2;            /* 'FS01' 旧格式（无校验），'FS02' 带校验和 */
    u32 checksum;          /* FNV-1a(超级块前 40 字节 + 目录项 + 块位图) */
    u32 journal_off;       /* 日志区偏移（块 0 内），0 表示没有 */
} super_t;

#define MAGIC2_V1 0x46533031u
#define MAGIC2_V2 0x46533032u

/* 事务日志：记录目录项的“新值”，挂载时若发现有效记录就重放（redo） */
typedef struct {
    u32 magic;
    u32 entry;
    u32 valid;
    u32 crc;
    u8  bytes[64];
} journal_t;

#define JOURNAL_OFF 64
#define JRNL_MAGIC  0x4C4E524Au      /* 'JRNL' */

static u8 *fs_base;
static u32 fs_bytes;
static super_t sb;
static int mounted;
static int readonly;
static u32 cwd = ROOT_DIR;

static u8 *block_ptr(u32 blk) { return fs_base + (u64)blk * sb.block_size; }
static super_t *SB(void) { return (super_t *)fs_base; }
static u8 *bitmap(void) { return block_ptr(sb.bitmap_block); }

static int bit_test(u32 b) { return (bitmap()[b >> 3] >> (b & 7)) & 1; }

static void bit_set(u32 b, int value)
{
    if (value)
        bitmap()[b >> 3] |= (u8)(1u << (b & 7));
    else
        bitmap()[b >> 3] &= (u8)~(1u << (b & 7));
}

static u32 fnv1a(const u8 *p, u32 n, u32 h)
{
    for (u32 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static u32 calc_checksum(void)
{
    u32 h = 2166136261u;
    h = fnv1a((const u8 *)fs_base, 40, h);                    /* 超级块前 40 字节 */
    h = fnv1a(block_ptr(sb.dirent_block), sb.dirent_count * 64, h);
    h = fnv1a(block_ptr(sb.bitmap_block), sb.block_size, h);
    return h;
}

static journal_t *journal(void) { return (journal_t *)(fs_base + JOURNAL_OFF); }

static u32 journal_crc(const journal_t *j)
{
    u32 h = 2166136261u;
    h = fnv1a((const u8 *)&j->entry, 4, h);
    h = fnv1a(j->bytes, 64, h);
    return h;
}

/* 记录目录项的新值（redo 日志），随后由 fs_commit 清除并更新校验和 */
static void journal_record(int idx, const fs_entry_t *ne)
{
    journal_t *j = journal();
    j->magic = JRNL_MAGIC;
    j->entry = (u32)idx;
    j->valid = 1;
    for (int i = 0; i < 64; i++) j->bytes[i] = ((const u8 *)ne)[i];
    j->crc = journal_crc(j);
}

static void fs_commit(void)
{
    journal_t *j = journal();
    j->valid = 0;
    SB()->checksum = calc_checksum();
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (upper(*a) != upper(*b)) return 0;
        a++; b++;
    }
    return upper(*a) == upper(*b);
}

static fs_entry_t *dirent(int i)
{
    return (fs_entry_t *)(block_ptr(sb.dirent_block) + (u64)i * 64);
}

/* ---------------- 挂载 ---------------- */
int fs_mount(void *image, u32 bytes)
{
    mounted = 0;
    fs_base = (u8 *)image;
    fs_bytes = bytes;
    cwd = ROOT_DIR;
    if (!fs_base || bytes < 4096)
        return -1;

    super_t *s = SB();
    for (int i = 0; i < 8; i++)
        if (s->magic[i] != "UMDFS1\0\0"[i])
            return -2;
    if ((s->magic2 != MAGIC2_V1 && s->magic2 != MAGIC2_V2) ||
        s->block_size != 512 || s->block_count == 0)
        return -3;
    if ((u64)s->block_count * s->block_size > bytes)
        return -4;

    sb = *s;
    if (sb.dirent_count > 64) sb.dirent_count = 64;
    mounted = 1;

    /* 日志重放：上次写到一半的目录项，按记录的新值补齐 */
    journal_t *j = journal();
    if (j->magic == JRNL_MAGIC && j->valid && j->entry < sb.dirent_count &&
        j->crc == journal_crc(j)) {
        for (int i = 0; i < 64; i++) ((u8 *)dirent((int)j->entry))[i] = j->bytes[i];
        j->valid = 0;
        SB()->checksum = calc_checksum();
        serial_puts("[kernel] fs: journal replayed\n");
    }

    /* 校验和不匹配 -> 只读挂载，避免把损坏的盘写坏 */
    if (sb.magic2 == MAGIC2_V2) {
        if (calc_checksum() != sb.checksum) {
            readonly = 1;
            serial_puts("[kernel] fs: checksum mismatch, mounted read-only\n");
        }
    }
    return 0;
}

/* ---------------- 目录 ---------------- */
u32 fs_dir(void) { return cwd; }

int fs_set_dir(u32 dir)
{
    if (!mounted) return -1;
    if (dir == ROOT_DIR) { cwd = ROOT_DIR; return 0; }
    if (dir >= sb.dirent_count) return -1;
    fs_entry_t *e = dirent((int)dir);
    if (!e->used || e->type != 1) return -1;
    cwd = dir;
    return 0;
}

static int find_in(u32 dir, const char *name)
{
    for (int i = 0; i < (int)sb.dirent_count; i++) {
        fs_entry_t *e = dirent(i);
        if (e->used && e->parent == dir && name_eq(e->name, name))
            return i;
    }
    return -1;
}

int fs_chdir(const char *path)
{
    if (!path) return -1;
    u32 dir = (path[0] == '/') ? ROOT_DIR : cwd;
    while (*path == '/') path++;
    while (*path) {
        char comp[32];
        int n = 0;
        while (path[n] && path[n] != '/' && n < 31) { comp[n] = path[n]; n++; }
        comp[n] = 0;
        path += n;
        while (*path == '/') path++;
        if (!n) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (dir != ROOT_DIR) dir = dirent((int)dir)->parent;
            continue;
        }
        if (comp[0] == '.' && comp[1] == 0) continue;
        int idx = find_in(dir, comp);
        if (idx < 0) return -2;
        if (dirent(idx)->type != 1) return -3;
        dir = (u32)idx;
    }
    cwd = dir;
    return 0;
}

int fs_up(void)
{
    if (cwd == ROOT_DIR) return -1;
    cwd = dirent((int)cwd)->parent;
    return 0;
}

/* 当前路径，形如 "C:\DOCS" */
void fs_path(char *out, u32 cap)
{
    char tmp[8][32];
    int depth = 0;
    u32 d = cwd;
    while (d != ROOT_DIR && depth < 8) {
        const fs_entry_t *e = dirent((int)d);
        for (int i = 0; i < 31 && e->name[i]; i++) tmp[depth][i] = e->name[i];
        tmp[depth][31] = 0;
        int n = 0;
        while (tmp[depth][n]) n++;
        tmp[depth][n] = 0;
        depth++;
        d = e->parent;
    }
    u32 p = 0;
    const char *pre = "C:\\";
    while (*pre && p + 1 < cap) out[p++] = *pre++;
    for (int i = depth - 1; i >= 0; i--) {
        for (int k = 0; tmp[i][k] && p + 1 < cap; k++) out[p++] = tmp[i][k];
        if (i > 0 && p + 1 < cap) out[p++] = '\\';
    }
    out[p] = 0;
}

/* ---------------- 读取 ---------------- */
int fs_count(void)
{
    if (!mounted) return 0;
    int n = 0;
    for (int i = 0; i < (int)sb.dirent_count; i++) {
        fs_entry_t *e = dirent(i);
        if (e->used && e->parent == cwd) n++;
    }
    return n;
}

const fs_entry_t *fs_get(int index)
{
    if (!mounted || index < 0) return NULL;
    int n = 0;
    for (int i = 0; i < (int)sb.dirent_count; i++) {
        fs_entry_t *e = dirent(i);
        if (e->used && e->parent == cwd) {
            if (n == index) return e;
            n++;
        }
    }
    return NULL;
}

int fs_find(const char *name)
{
    if (!mounted) return -1;
    return find_in(cwd, name);
}

const fs_entry_t *fs_at(int index)
{
    if (!mounted || index < 0 || index >= (int)sb.dirent_count) return NULL;
    fs_entry_t *e = dirent(index);
    return e->used ? e : NULL;
}

int fs_entry_index(const fs_entry_t *e)
{
    if (!mounted || !e) return -1;
    const u8 *base = block_ptr(sb.dirent_block);
    const u8 *p = (const u8 *)e;
    if (p < base || p >= base + (u64)sb.dirent_count * 64)
        return -1;
    return (int)((p - base) / 64);
}

int fs_is_dir(const fs_entry_t *e) { return e && e->type == 1; }

const u8 *fs_data(const fs_entry_t *e)
{
    if (!mounted || !e || e->type == 1)
        return NULL;
    if (e->size == 0)
        return fs_base;
    if (e->first_block >= sb.block_count)
        return NULL;
    const u8 *p = block_ptr(e->first_block);
    if (p + e->size > fs_base + fs_bytes)
        return NULL;
    return p;
}

u32 fs_block_size(void) { return mounted ? sb.block_size : 0; }
u32 fs_block_count(void) { return mounted ? sb.block_count : 0; }
const u8 *fs_raw(void) { return fs_base; }

u32 fs_free_blocks(void)
{
    if (!mounted) return 0;
    u32 n = 0;
    for (u32 b = 0; b < sb.block_count; b++)
        if (!bit_test(b)) n++;
    return n;
}

const char *fs_label(void) { return mounted ? "UMDFS1" : "none"; }

/* ---------------- 写入 ---------------- */
static u32 blocks_for(u32 size)
{
    u32 n = (size + sb.block_size - 1) / sb.block_size;
    return n ? n : 1;
}

static int alloc_run(u32 n, u32 *out)
{
    u32 run = 0, start = 0;
    for (u32 b = sb.data_start; b < sb.block_count; b++) {
        if (!bit_test(b)) {
            if (!run) start = b;
            run++;
            if (run == n) {
                for (u32 i = 0; i < n; i++) bit_set(start + i, 1);
                *out = start;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

static void free_run(u32 start, u32 n)
{
    for (u32 i = 0; i < n; i++) bit_set(start + i, 0);
}

static u32 used_blocks(const fs_entry_t *e) { return blocks_for(e->size); }

static void set_name(fs_entry_t *e, const char *name)
{
    for (int k = 0; k < 32; k++) e->name[k] = 0;
    int k = 0;
    while (name[k] && k < 31) { e->name[k] = name[k]; k++; }
    e->name[k] = 0;
}

int fs_write(const char *name, const void *data, u32 size)
{
    if (!mounted || readonly || !name || !*name || size > 64 * 1024)
        return -1;

    int idx = find_in(cwd, name);
    u32 need = blocks_for(size), start = 0;

    if (idx >= 0) {
        fs_entry_t *e = dirent(idx);
        if (e->type == 1) return -4;             /* 不能写目录 */
        u32 had = used_blocks(e);
        u32 old_start = e->first_block;
        if (need <= had) {
            start = old_start;
            if (need < had) free_run(old_start + need, had - need);
        } else {
            u32 ns = 0;
            if (alloc_run(need, &ns) != 0) return -2;
            free_run(old_start, had);
            start = ns;
        }
        journal_record(idx, e);
        e->size = size;
        e->first_block = start;
        e->used = 1;
        e->type = 0;
    } else {
        int slot = -1;
        for (int i = 0; i < (int)sb.dirent_count; i++)
            if (!dirent(i)->used) { slot = i; break; }
        if (slot < 0) return -3;
        if (alloc_run(need, &start) != 0) return -2;
        fs_entry_t *e = dirent(slot);
        set_name(e, name);
        e->size = size;
        e->first_block = start;
        e->used = 1;
        e->type = 0;
        e->parent = cwd;
        SB()->file_count = SB()->file_count + 1;
        journal_record(slot, e);
    }

    if (size && data) {
        u8 *dst = block_ptr(start);
        const u8 *src = (const u8 *)data;
        for (u32 i = 0; i < size; i++) dst[i] = src[i];
    }
    fs_commit();
    return 0;
}

int fs_mkdir(const char *name)
{
    if (!mounted || !name || !*name) return -1;
    if (find_in(cwd, name) >= 0) return -3;
    int slot = -1;
    for (int i = 0; i < (int)sb.dirent_count; i++)
        if (!dirent(i)->used) { slot = i; break; }
    if (slot < 0) return -3;
    fs_entry_t *e = dirent(slot);
    set_name(e, name);
    e->size = 0;
    e->first_block = 0;
    e->used = 1;
    e->type = 1;
    e->parent = cwd;
    SB()->file_count = SB()->file_count + 1;
    return 0;
}

int fs_dir_empty(u32 dir)
{
    for (int i = 0; i < (int)sb.dirent_count; i++) {
        fs_entry_t *e = dirent(i);
        if (e->used && e->parent == dir) return 0;
    }
    return 1;
}

int fs_unlink(const char *name)
{
    if (readonly) return -5;
    int idx = find_in(cwd, name);
    if (idx < 0) return -1;
    fs_entry_t *e = dirent(idx);
    if (e->type == 1) {
        if (!fs_dir_empty((u32)idx)) return -4;
    } else {
        free_run(e->first_block, used_blocks(e));
    }
    e->used = 0;
    e->size = 0;
    e->first_block = 0;
    if (SB()->file_count) SB()->file_count = SB()->file_count - 1;
    journal_record(idx, e);
    fs_commit();
    return 0;
}

int fs_rename(const char *oldname, const char *newname)
{
    int idx = find_in(cwd, oldname);
    if (idx < 0 || !newname || !*newname || find_in(cwd, newname) >= 0)
        return -1;
    set_name(dirent(idx), newname);
    return 0;
}

/* ---------------- 落盘 ---------------- */
static int backing;
static u32 backing_lba;

void fs_set_backing(u32 lba) { backing_lba = lba; backing = 1; }
int fs_backed(void) { return backing && ata_present(); }
int fs_mounted(void) { return mounted; }

int fs_flush(void)
{
    if (!mounted || !backing || !ata_present())
        return -1;
    u32 total = sb.block_count;
    u32 done = 0;
    while (done < total) {
        u32 chunk = total - done;
        if (chunk > 64) chunk = 64;
        if (ata_write(backing_lba + done, (u8)chunk, fs_base + (u64)done * 512) != 0)
            return -2;
        done += chunk;
    }
    return ata_flush() == 0 ? 0 : -3;
}

/* ---------------- P9：增量写、校验状态、自检 ---------------- */
u32 fs_checksum(void)
{
    return mounted ? calc_checksum() : 0;
}

int fs_checksum_ok(void)
{
    if (!mounted) return 0;
    if (sb.magic2 != MAGIC2_V2) return 1;      /* 旧格式没有校验和 */
    return calc_checksum() == sb.checksum;
}

int fs_readonly(void) { return readonly; }

/* 在 offset 处写入 len 字节：容量够就原地写（真正的增量）；
 * 不够时优先原地向后扩展，失败才重新分配并搬运旧数据。 */
int fs_write_at(const char *name, u32 off, const void *data, u32 len)
{
    if (!mounted || readonly || !name || !*name || len == 0)
        return -1;
    if (off + len > 64 * 1024)
        return -1;

    int idx = find_in(cwd, name);
    u32 need_end = off + len;
    u32 start = 0, cap = 0;
    fs_entry_t *e = 0;

    if (idx >= 0) {
        e = dirent(idx);
        if (e->type == 1) return -4;
        start = e->first_block;
        cap = blocks_for(e->size) * sb.block_size;
    } else {
        int slot = -1;
        for (int i = 0; i < (int)sb.dirent_count; i++)
            if (!dirent(i)->used) { slot = i; break; }
        if (slot < 0) return -3;
        e = dirent(slot);
        set_name(e, name);
        e->size = 0;
        e->first_block = 0;
        e->used = 1;
        e->type = 0;
        e->parent = cwd;
        if (SB()->file_count) SB()->file_count = SB()->file_count + 1;
        idx = slot;
        cap = 0;
    }

    if (need_end > cap) {
        u32 need_blocks = blocks_for(need_end);
        u32 have_blocks = blocks_for(cap ? cap : 0);
        int extended = 0;
        if (cap && start) {
            u32 add = need_blocks - have_blocks;
            u32 ok = 1;
            for (u32 k = 0; k < add; k++)
                if (start + have_blocks + k >= sb.block_count ||
                    bit_test(start + have_blocks + k)) { ok = 0; break; }
            if (ok) {
                for (u32 k = 0; k < add; k++) bit_set(start + have_blocks + k, 1);
                extended = 1;
            }
        }
        if (!extended) {
            u32 ns = 0;
            if (alloc_run(need_blocks, &ns) != 0) return -2;
            if (cap && start) {
                u8 *dst = block_ptr(ns);
                const u8 *src = block_ptr(start);
                u32 old_size = e->size;
                for (u32 i = 0; i < old_size && i < need_end; i++) dst[i] = src[i];
                free_run(start, have_blocks);
            }
            start = ns;
        }
        cap = need_blocks * sb.block_size;
    }

    u8 *dstp = block_ptr(start) + off;
    const u8 *srcp = (const u8 *)data;
    for (u32 i = 0; i < len; i++) dstp[i] = srcp[i];

    journal_record(idx, e);
    if (need_end > e->size) e->size = need_end;
    e->first_block = start;
    fs_commit();
    return 0;
}

/* 日志自检：写入一条 redo 记录 -> 模拟重放 -> 确认生效 -> 恢复 */
int fs_journal_selftest(void)
{
    if (!mounted) return -1;
    int idx = -1;
    for (int i = 0; i < (int)sb.dirent_count; i++)
        if (dirent(i)->used && dirent(i)->type == 0) { idx = i; break; }
    if (idx < 0) return -2;

    fs_entry_t saved = *dirent(idx);
    fs_entry_t modified = saved;
    modified.size = (modified.size + 1) & 0xFFFF;
    journal_record(idx, &modified);
    journal_t *j = journal();
    if (j->magic != JRNL_MAGIC || !j->valid) return -3;
    for (int i = 0; i < 64; i++) ((u8 *)dirent(idx))[i] = j->bytes[i];   /* 模拟重放 */
    int replayed = (dirent(idx)->size == modified.size);
    *dirent(idx) = saved;
    fs_commit();
    return replayed ? 0 : -4;
}

int fs_selftest(void)
{
    if (!mounted) return -1;
    int ok = 1;

    int ck = fs_checksum_ok();
    ok &= ck;

    u32 saved_dir = cwd;
    cwd = ROOT_DIR;
    int rc1 = fs_write("P9TEST.TXT", "hello", 5);
    int rc2 = fs_write_at("P9TEST.TXT", 5, " vfs", 4);
    int rc3 = fs_journal_selftest();
    int data_ok = 0;
    if (rc1 == 0 && rc2 == 0) {
        int idx = find_in(ROOT_DIR, "P9TEST.TXT");
        if (idx >= 0) {
            const fs_entry_t *e = dirent(idx);
            const u8 *d = fs_data(e);
            if (e->size == 9 && d) {
                const char *want = "hello vfs";
                data_ok = 1;
                for (int i = 0; i < 9; i++) if (d[i] != (u8)want[i]) data_ok = 0;
            }
        }
    }
    fs_unlink("P9TEST.TXT");
    cwd = saved_dir;

    ok &= (rc1 == 0 && rc2 == 0 && data_ok);
    ok &= (rc3 == 0);

    serial_puts("[kernel] fs selftest flags: ck=");
    serial_putc((char)('0' + ck));
    serial_puts(" rw=");
    serial_putc((char)('0' + (rc1 == 0)));
    serial_putc((char)('0' + (rc2 == 0)));
    serial_puts(" jrnl=");
    serial_putc((char)('0' + (rc3 == 0)));
    serial_puts(" data=");
    serial_putc((char)('0' + data_ok));
    serial_puts(" ");
    serial_puts(ok ? "checksum/journal/write_at ok\n" : "FAILED\n");
    return ok ? 0 : -5;
}
