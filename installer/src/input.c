/* input.c - 8042 轮询：键盘（扫描码集 1）+ PS/2 鼠标 */
#include "input.h"
#include "io.h"

static int shift_down;
static int caps_lock;
static int ctrl_down;
static int extended;

static const char map_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};

static const char map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

static u8 pkt[3];
static int npkt;

void input_init(void)
{
    shift_down = caps_lock = ctrl_down = extended = 0;
    npkt = 0;
    /* 清空 8042 输出缓冲里可能残留的字节 */
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 1)) break;
        (void)inb(0x60);
    }
}

int input_shift_down(void) { return shift_down; }

int input_poll(input_event *ev)
{
    u8 status = inb(0x64);
    if (!(status & 1))
        return 0;
    u8 data = inb(0x60);

    if (status & 0x20) {                 /* 鼠标数据 */
        if (npkt == 0 && !(data & 0x08))
            return 0;                    /* 未对齐，丢弃 */
        pkt[npkt++] = data;
        if (npkt < 3)
            return 0;
        npkt = 0;
        int dx = pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
        int dy = pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
        ev->type = EV_MOUSE;
        ev->dx = dx;
        ev->dy = dy;
        ev->buttons = pkt[0] & 0x07;
        ev->key = 0;
        ev->shift = shift_down;
        ev->ctrl = ctrl_down;
        return 1;
    }

    /* 键盘 */
    if (data == 0xE0) {
        extended = 1;
        return 0;
    }
    int released = data & 0x80;
    u8 code = data & 0x7F;

    if (extended) {
        extended = 0;
        if (released)
            return 0;
        int k = 0;
        switch (code) {
        case 0x48: k = KEY_UP; break;
        case 0x50: k = KEY_DOWN; break;
        case 0x4B: k = KEY_LEFT; break;
        case 0x4D: k = KEY_RIGHT; break;
        case 0x47: k = KEY_HOME; break;
        case 0x4F: k = KEY_END; break;
        case 0x49: k = KEY_PGUP; break;
        case 0x51: k = KEY_PGDN; break;
        case 0x53: k = KEY_DEL; break;
        default: return 0;
        }
        ev->type = EV_KEY;
        ev->key = k;
        ev->dx = ev->dy = 0;
        ev->buttons = 0;
        ev->shift = shift_down;
        ev->ctrl = ctrl_down;
        return 1;
    }
    if (code == 0x2A || code == 0x36) {
        shift_down = !released;
        return 0;
    }
    if (code == 0x1D) {
        ctrl_down = !released;
        return 0;
    }
    if (code == 0x3A && !released) {
        caps_lock = !caps_lock;
        return 0;
    }
    if (released || code >= 128)
        return 0;

    if (code >= 0x3B && code <= 0x44) {
        ev->type = EV_KEY;
        ev->key = KEY_F2 + (code - 0x3C);      /* 0x3B=F1, 0x3C=F2 ... */
        ev->dx = ev->dy = 0;
        ev->buttons = 0;
        ev->shift = shift_down;
        ev->ctrl = ctrl_down;
        return 1;
    }

    char ch = shift_down ? map_shift[code] : map_normal[code];
    if (ch >= 'a' && ch <= 'z') {
        if (shift_down ^ caps_lock)
            ch = (char)(ch - 'a' + 'A');
    }
    if (!ch)
        return 0;

    ev->type = EV_KEY;
    ev->key = (int)(u8)ch;
    ev->dx = ev->dy = 0;
    ev->buttons = 0;
    ev->shift = shift_down;
    ev->ctrl = ctrl_down;
    return 1;
}
