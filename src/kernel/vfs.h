/* vfs.h - 虚拟文件系统层与文件描述符（P7） */
#ifndef VFS_H
#define VFS_H

#include "types.h"

#define VFS_MAX_FD   32
#define VFS_NAME_MAX 32

#define VFS_O_RDONLY 0
#define VFS_O_WRONLY 1
#define VFS_O_CREAT  0x100
#define VFS_O_TRUNC  0x200

#define VFS_FILE 1
#define VFS_DIR  2
#define VFS_DEV  3
#define VFS_PROC 4

int  vfs_init(void);
int  vfs_open(const char *path, int flags);
int  vfs_read(int fd, void *buf, u32 len);
int  vfs_write(int fd, const void *buf, u32 len);
int  vfs_close(int fd);
int  vfs_seek(int fd, u32 off);
int  vfs_size(int fd);
int  vfs_type(int fd);
int  vfs_stat(const char *path, u32 *size, int *is_dir);
int  vfs_readdir(const char *path, int index, char *name, u32 cap, u32 *size, int *is_dir);
int  vfs_selftest(void);

/* P11/P12：目录操作与权限 */
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
void vfs_set_role(int role);
int  vfs_role(void);

/* P10：按进程拆分 fd 表 */
typedef struct {
    int  used;
    int  type;
    char path[64];
    u32  dir;
    int  index;
    u32  offset;
    int  flags;
    void *dev;
    char *mbuf;          /* /proc 生成的内容 */
    u32  mlen;
} vfs_fd_t;

typedef struct {
    vfs_fd_t fd[VFS_MAX_FD];
} vfs_fdset_t;

vfs_fdset_t *vfs_current_set(void);
void         vfs_select_set(vfs_fdset_t *set);
vfs_fdset_t *vfs_kernel_set(void);

#endif
