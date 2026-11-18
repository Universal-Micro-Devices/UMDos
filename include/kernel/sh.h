/* sh.h - 命令行 shell（P11） */
#ifndef SH_H
#define SH_H

#include "types.h"

#define SH_EXIT 1

void sh_prompt(void);
int  sh_exec_line(const char *line);     /* 返回 SH_EXIT 表示要退出终端 */
const char *sh_cwd(void);
void sh_set_cwd(const char *dir);

#endif
