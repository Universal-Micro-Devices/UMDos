/* term.c - terminal emulator (P11, restyled)
 *
 * Layout: flat dark window, 30px tab bar "UMDos Terminal" with flat
 * minimize/maximize/close buttons, character grid 78x22 below.
 * Colors: background RGB(12,12,12); text light gray; prompt uses
 * low-saturation green/blue/white. Cursor is a solid light-gray block
 * that blinks with the PIT tick.
 */
#include "term.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "irq.h"
#include "sh.h"

#define BG        0x000C0C0Cu
#define TAB_BG    0x001E1E1Eu
#define TAB_TXT   0x00E0E0E0u
#define BTN_TXT   0x00C8C8C8u
#define CUR_COL   0x00C8C8C8u
#define TAB_H     30
#define GRID_Y    34

static u32 pal[4] = { 0x00D0D0D0u, 0x003F9E3Fu, 0x005A9FD4u, 0x00FFFFFFu };

static char grid[TERM_ROWS][TERM_COLS + 1];
static u8   gcol[TERM_ROWS][TERM_COLS + 1];
static int  cur_row, cur_col;
static char linebuf[TERM_COLS + 1];
static int  linelen;
static int  cur_drawn = -1;

void term_init(void)
{
    term_clear();
}

static void scroll_up(void)
{
    for (int r = 1; r < TERM_ROWS; r++) {
        for (int c = 0; c <= TERM_COLS; c++) {
            grid[r - 1][c] = grid[r][c];
            gcol[r - 1][c] = gcol[r][c];
        }
    }
    for (int c = 0; c <= TERM_COLS; c++) {
        grid[TERM_ROWS - 1][c] = 0;
        gcol[TERM_ROWS - 1][c] = TC_DEFAULT;
    }
    if (cur_row > 0) cur_row--;
}

void term_clear(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c <= TERM_COLS; c++) {
            grid[r][c] = 0;
            gcol[r][c] = TC_DEFAULT;
        }
    }
    cur_row = cur_col = 0;
    linelen = 0;
    linebuf[0] = 0;
    cur_drawn = -1;
}

static void putc_c(char c, int color)
{
    serial_putc(c);
    if (c == '\n') {
        grid[cur_row][cur_col] = 0;
        cur_col = 0;
        cur_row++;
        if (cur_row >= TERM_ROWS) {
            cur_row = TERM_ROWS - 1;
            scroll_up();
        }
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (cur_col >= TERM_COLS) {
        grid[cur_row][TERM_COLS] = 0;
        cur_col = 0;
        cur_row++;
        if (cur_row >= TERM_ROWS) {
            cur_row = TERM_ROWS - 1;
            scroll_up();
        }
    }
    grid[cur_row][cur_col] = c;
    gcol[cur_row][cur_col] = (u8)color;
    cur_col++;
    grid[cur_row][cur_col] = 0;
}

void term_putc(char c) { putc_c(c, TC_DEFAULT); }

void term_puts(const char *s)
{
    while (s && *s) term_putc(*s++);
}

void term_print_color(const char *s, u32 rgb)
{
    int idx = TC_DEFAULT;
    if (rgb == 0x003F9E3Fu || rgb == 0x0000FF00u) idx = TC_GREEN;
    else if (rgb == 0x005A9FD4u || rgb == 0x000000FFu) idx = TC_BLUE;
    else if (rgb == 0x00FFFFFFu) idx = TC_WHITE;
    while (s && *s) putc_c(*s++, idx);
}

static void tab_bar(void)
{
    u32 w = gfx_width();
    gfx_rect(0, 0, (int)w, TAB_H, TAB_BG);
    gfx_text(10, 7, "UMDos Terminal", TAB_TXT, 1);

    int by = 7, bh = 16, bw = 26;
    int bx = (int)w - 3 * (bw + 4) - 8;
    for (int i = 0; i < 3; i++) {
        gfx_rect(bx + i * (bw + 4), by, bw, bh, TAB_BG);
        gfx_frame(bx + i * (bw + 4), by, bw, bh, 0x00404040u);
    }
    gfx_hline(bx + 7, by + 10, 12, BTN_TXT);                     /* minimize */
    gfx_frame(bx + (bw + 4) + 7, by + 5, 12, 9, BTN_TXT);        /* maximize */
    int cx = bx + 2 * (bw + 4) + 7, cy = by + 5;                 /* close */
    for (int i = 0; i < 10; i++) {
        gfx_pixel(cx + i, cy + i, BTN_TXT);
        gfx_pixel(cx + 9 - i, cy + i, BTN_TXT);
    }
}

static void draw_row(int r)
{
    if (!grid[r][0]) return;
    int c = 0;
    while (c < TERM_COLS && grid[r][c]) {
        int col = gcol[r][c];
        char run[TERM_COLS + 1];
        int n = 0;
        while (c < TERM_COLS && grid[r][c] && gcol[r][c] == col && n < TERM_COLS)
            run[n++] = grid[r][c++];
        run[n] = 0;
        gfx_text(4 + (c - n) * 8, GRID_Y + r * 16, run, pal[col & 3], 1);
    }
}

static void cursor_paint(int on)
{
    int x = 4 + cur_col * 8;
    int y = GRID_Y + cur_row * 16;
    if (on) gfx_rect(x, y, 7, 14, CUR_COL);
    else gfx_rect(x, y, 7, 14, BG);
    cur_drawn = on;
}

void term_draw(void)
{
    gfx_clear(BG);
    tab_bar();
    for (int r = 0; r < TERM_ROWS; r++) draw_row(r);
    cur_drawn = -1;
}

void term_run(void)
{
    term_clear();
    term_puts("UMDos Terminal. Type 'help' for a list of commands.\n");
    sh_prompt();
    term_draw();
    cursor_paint(1);

    input_event ev;
    u32 last_phase = 0xFFFFFFFFu;
    for (;;) {
        if (!input_poll(&ev)) {
            u32 phase = (pit_ticks() / 50u) & 1u;
            if (phase != last_phase) {
                last_phase = phase;
                cursor_paint(phase ? 1 : 0);
            }
            continue;
        }
        if (ev.type != EV_KEY) continue;
        int k = ev.key;
        if (k == KEY_ESC) {
            term_puts("\n");
            term_draw();
            return;
        }
        if (k == KEY_ENTER) {
            term_putc('\n');
            linebuf[linelen] = 0;
            int n = sh_exec_line(linebuf);
            linelen = 0;
            if (n == SH_EXIT) {
                term_draw();
                return;
            }
            sh_prompt();
            term_draw();
            cursor_paint(1);
            last_phase = 0xFFFFFFFFu;
            continue;
        }
        if (k == KEY_BACKSPACE) {
            if (linelen > 0) {
                linelen--;
                linebuf[linelen] = 0;
                if (cur_col > 0) {
                    cur_col--;
                    grid[cur_row][cur_col] = 0;
                    gcol[cur_row][cur_col] = TC_DEFAULT;
                    gfx_rect(4 + cur_col * 8, GRID_Y + cur_row * 16, 8, 15, BG);
                    cursor_paint(1);
                }
            }
            continue;
        }
        if (k >= 32 && k < 127) {
            if (linelen < TERM_COLS - 4) {
                int cx = 4 + cur_col * 8, cy = GRID_Y + cur_row * 16;
                char one[2];
                one[0] = (char)k;
                one[1] = 0;
                linebuf[linelen++] = (char)k;
                term_putc((char)k);
                gfx_text(cx, cy, one, pal[TC_DEFAULT], 1);
                cursor_paint(1);
            }
            continue;
        }
    }
}
