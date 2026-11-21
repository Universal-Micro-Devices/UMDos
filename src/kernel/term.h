/* term.h - terminal emulator (P11, restyled classic/flat) */
#ifndef TERM_H
#define TERM_H

#include "types.h"

#define TERM_COLS 80
#define TERM_ROWS 25

/* low-saturation palette indices */
#define TC_DEFAULT 0
#define TC_GREEN   1
#define TC_BLUE    2
#define TC_WHITE   3

void term_init(void);
void term_clear(void);
void term_putc(char c);
void term_puts(const char *s);
void term_print_color(const char *s, u32 rgb);
void term_draw(void);
void term_run(void);

#endif
