/* spinlock.h - 单核安全的自旋锁（P15）
 *
 * 单 CPU 上真正的互斥来自关中断；锁本身用于多核场景（AP 起来后）。
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

typedef struct {
    volatile u32 locked;
} spinlock_t;

static inline void spin_init(spinlock_t *s) { s->locked = 0; }

static inline u64 irq_save(void)
{
    u64 f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f));
    return f;
}

static inline void irq_restore(u64 f)
{
    __asm__ volatile("push %0; popfq" : : "r"(f));
}

static inline void spin_lock(spinlock_t *s)
{
    while (__sync_lock_test_and_set(&s->locked, 1)) {
        while (s->locked)
            __asm__ volatile("pause");
    }
}

static inline void spin_unlock(spinlock_t *s)
{
    __sync_lock_release(&s->locked);
}

#endif
