/* term.h - 全屏终端（P11） */
#ifndef TERM_H
#define TERM_H

#include "types.h"

#define TERM_COLS 78
#define TERM_ROWS 22

void term_init(void);
void term_clear(void);
void term_putc(char c);
void term_puts(const char *s);
void term_draw(void);
void term_run(void);            /* 阻塞运行，ESC/exit 返回桌面 */

#endif
