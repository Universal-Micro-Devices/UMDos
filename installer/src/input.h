/* input.h - PS/2 键盘 + 鼠标（轮询） */
#ifndef INPUT_H
#define INPUT_H

#include "types.h"

#define EV_NONE   0
#define EV_KEY    1
#define EV_MOUSE  2

#define KEY_ENTER     0x0D
#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09
#define KEY_ESC       0x1B

/* 扩展键（0xE0 前缀） */
#define KEY_UP        0x100
#define KEY_DOWN      0x101
#define KEY_LEFT      0x102
#define KEY_RIGHT     0x103
#define KEY_HOME      0x104
#define KEY_END       0x105
#define KEY_DEL       0x106
#define KEY_PGUP      0x107
#define KEY_PGDN      0x108
#define KEY_F1        0x109
#define KEY_F2        0x10A
#define KEY_F3        0x10B

typedef struct {
    int type;
    int key;            /* EV_KEY: ASCII 或功能键 */
    int dx, dy;         /* EV_MOUSE: 位移 */
    int buttons;        /* EV_MOUSE: bit0 左键 bit1 右键 */
    int shift;          /* 按下时 Shift 状态 */
    int ctrl;           /* 按下时 Ctrl 状态 */
} input_event;

void input_init(void);
int  input_poll(input_event *ev);
int  input_shift_down(void);

#endif
