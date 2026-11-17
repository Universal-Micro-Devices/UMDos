/* user.h - 用户态（P6） */
#ifndef USER_H
#define USER_H

#include "types.h"

#define USER_STACK_PAGES 2

int  user_run(const void *elf, u32 len, u32 *entry_out, u32 *rsp_out);
void user_leave_to_kernel(int code) __attribute__((noreturn));
int  user_last_exit(void);
int  user_selftest(const void *elf, u32 len, u32 *entry_out);
void user_enter(u64 entry, u64 ustack) __attribute__((noreturn));

#endif
