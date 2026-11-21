/* smp.h - SMP：INIT-SIPI-SIPI 与 AP 检查（P15） */
#ifndef SMP_H
#define SMP_H

#include "types.h"

int  smp_init(void);
u32  smp_started_aps(void);
int  smp_selftest(void);

#endif
