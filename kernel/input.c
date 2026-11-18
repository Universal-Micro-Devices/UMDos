/* input.c - PS/2 键盘与鼠标
 *
 * P3 之后有两条路径：
 *   1. IRQ 路径（真实硬件）：IRQ1/IRQ12 把字节推进环形缓冲，input_poll 消费；
 *   2. 轮询回退（没有 IRQ 的环境，例如当前 Unicorn 模拟器没有 PIC/PIT 模型）：
 *      只要还没有任何 IRQ 到来，就直接读 8042。
 * 两条路径共用同一套扫描码翻译与鼠标组包逻辑。
 */
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

/* ---------------- 环形缓冲 ---------------- */
#define RING 64
static volatile u8 kb_ring[RING];
static volatile u32 kb_h, kb_t;
static volatile u8 ms_ring[RING];
static volatile u32 ms_h, ms_t;
static volatile u32 irq_seen;
static u8 ms_pkt[3];
static int ms_n;

static void kb_push(u8 b)
{
    u32 n = (kb_h + 1) % RING;
    if (n != kb_t) {
        kb_ring[kb_h] = b;
        kb_h = n;
    }
    irq_seen = 1;
}

static void ms_push(u8 b)
{
    u32 n = (ms_h + 1) % RING;
    if (n != ms_t) {
        ms_ring[ms_h] = b;
        ms_h = n;
    }
    irq_seen = 1;
}

static int kb_pop(u8 *b)
{
    if (kb_h == kb_t) return 0;
    *b = kb_ring[kb_t];
    kb_t = (kb_t + 1) % RING;
    return 1;
}

static int ms_pop(u8 *b)
{
    if (ms_h == ms_t) return 0;
    *b = ms_ring[ms_t];
    ms_t = (ms_t + 1) % RING;
    return 1;
}

/* IRQ 路径入口 */
void kbd_irq_push(u8 scancode) { kb_push(scancode); }
void mouse_irq_push(u8 byte) { ms_push(byte); }
int  input_irq_seen(void) { return (int)irq_seen; }

void input_selftest_expected(void)
{
    kb_t = kb_h;
    ms_t = ms_h;
}

/* 自检结束后恢复“还没有真实 IRQ 到来”的状态，轮询回退才能继续工作 */
void input_selftest_reset(void)
{
    kb_h = kb_t = 0;
    ms_h = ms_t = 0;
    irq_seen = 0;
    ms_n = 0;
}

/* ---------------- 扫描码翻译 ---------------- */
static int translate(u8 data, input_event *ev)
{
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
    if (code == 0x2A || code == 0x36) { shift_down = !released; return 0; }
    if (code == 0x1D) { ctrl_down = !released; return 0; }
    if (code == 0x3A && !released) { caps_lock = !caps_lock; return 0; }
    if (released || code >= 128)
        return 0;

    if (code >= 0x3B && code <= 0x44) {
        ev->type = EV_KEY;
        ev->key = KEY_F2 + (code - 0x3C);
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

/* ---------------- 初始化 ---------------- */
void input_init(void)
{
    shift_down = caps_lock = ctrl_down = extended = 0;
    kb_h = kb_t = 0;
    ms_h = ms_t = 0;
    /* 清空 8042 输出缓冲里可能残留的字节 */
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 1)) break;
        (void)inb(0x60);
    }
}

int input_shift_down(void) { return shift_down; }

/* ---------------- 事件轮询 ---------------- */
int input_poll(input_event *ev)
{
    /* 1) 键盘环形缓冲 */
    for (;;) {
        u8 sc;
        if (!kb_pop(&sc)) break;
        if (translate(sc, ev)) return 1;
    }

    /* 2) 鼠标环形缓冲：凑满 3 字节组成一个包 */
    while (ms_n < 3) {
        u8 b;
        if (!ms_pop(&b)) break;
        if (ms_n == 0 && !(b & 0x08)) continue;   /* 丢同步 */
        ms_pkt[ms_n++] = b;
    }
    if (ms_n == 3) {
        ms_n = 0;
        int dx = ms_pkt[1] - ((ms_pkt[0] & 0x10) ? 256 : 0);
        int dy = ms_pkt[2] - ((ms_pkt[0] & 0x20) ? 256 : 0);
        ev->type = EV_MOUSE;
        ev->dx = dx;
        ev->dy = dy;
        ev->buttons = ms_pkt[0] & 0x07;
        ev->key = 0;
        ev->shift = shift_down;
        ev->ctrl = ctrl_down;
        return 1;
    }

    /* 3) 轮询回退：没有任何 IRQ 时直接读 8042 */
    if (!irq_seen) {
        u8 status = inb(0x64);
        if (status & 1) {
            u8 data = inb(0x60);
            if (status & 0x20)
                ms_push(data);
            else
                return translate(data, ev);
        }
    }
    return 0;
}
