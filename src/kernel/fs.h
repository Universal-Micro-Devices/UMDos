/* fs.h - UMDFS 文件系统（带子目录，支持读写和落盘） */
#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_ROOT_DIR 0xFFFFFFFFu

typedef struct {
    char name[32];
    u32  size;
    u32  first_block;
    u8   used;
    u8   type;              /* 0=文件 1=目录 */
    u8   pad[2];
    u32  parent;            /* 父目录的目录项索引，根为 FS_ROOT_DIR */
} fs_entry_t;

int  fs_mount(void *image, u32 bytes);
int  fs_mounted(void);
u32  fs_dir(void);
int  fs_set_dir(u32 dir);
int  fs_chdir(const char *path);
int  fs_up(void);
void fs_path(char *out, u32 cap);

int  fs_count(void);
const fs_entry_t *fs_get(int index);
int  fs_entry_index(const fs_entry_t *e);
const fs_entry_t *fs_at(int index);
int  fs_find(const char *name);
int  fs_is_dir(const fs_entry_t *e);
const u8 *fs_data(const fs_entry_t *e);
u32  fs_block_size(void);
u32  fs_block_count(void);
u32  fs_free_blocks(void);
const char *fs_label(void);
const u8 *fs_raw(void);

int  fs_write(const char *name, const void *data, u32 size);
int  fs_mkdir(const char *name);
int  fs_unlink(const char *name);
int  fs_rename(const char *oldname, const char *newname);
int  fs_dir_empty(u32 dir);

void fs_set_backing(u32 lba);
int  fs_backed(void);
int  fs_flush(void);

/* P9: checksum / journal / incremental write / selftest */
u32  fs_checksum(void);
int  fs_checksum_ok(void);
int  fs_readonly(void);
int  fs_write_at(const char *name, u32 off, const void *data, u32 len);
int  fs_journal_selftest(void);
int  fs_selftest(void);

#endif
