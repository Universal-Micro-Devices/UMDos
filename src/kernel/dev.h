/* dev.h - 设备模型与驱动框架（P8） */
#ifndef DEV_H
#define DEV_H

#include "types.h"

#define DEV_MAX 8
#define DEV_CHAR  1
#define DEV_BLOCK 2

typedef struct dev {
    char name[12];
    int  type;
    u32  irq;
    int (*open)(struct dev *d);
    int (*read)(struct dev *d, void *buf, u32 len);
    int (*write)(struct dev *d, const void *buf, u32 len);
    u64  rx;                 /* 读字节数 */
    u64  tx;                 /* 写字节数 */
    void *priv;
} dev_t;

void   dev_init(void);
int    dev_register(dev_t *d);
dev_t *dev_find(const char *name);
dev_t *dev_at(int index);
int    dev_count(void);
u32    dev_irq_total(void);

#endif
