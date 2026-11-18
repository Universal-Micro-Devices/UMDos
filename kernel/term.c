/* term.c - 终端模拟器（P11）
 *
 * 字符网格 78x22，scale 2 绘制（78*16=1248 < 1280，22*32=704 < 720）。
 * 每个字符同时镜像到 COM1，方便自动化测试断言终端内容。
 */
#include "term.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "sh.h"

static char grid[TERM_ROWS][TERM_COLS + 1];
static int cur_row, cur_col;
static char linebuf[TERM_COLS + 1];
static int linelen;

#define TERM_FG     0x0060FF80u
#define TERM_PROMPT 0x00C8FFC8u

void term_init(void)
{
    term_clear();
}

static void scroll_up(void)
{
    for (int r = 1; r < TERM_ROWS; r++)
        for (int c = 0; c <= TERM_COLS; c++) grid[r - 1][c] = grid[r][c];
    for (int c = 0; c <= TERM_COLS; c++) grid[TERM_ROWS - 1][c] = 0;
    if (cur_row > 0) cur_row--;
}

void term_clear(void)
{
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c <= TERM_COLS; c++) grid[r][c] = 0;
    cur_row = 0;
    cur_col = 0;
    linelen = 0;
    linebuf[0] = 0;
}

void term_putc(char c)
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
    grid[cur_row][cur_col++] = c;
    grid[cur_row][cur_col] = 0;
}

void term_puts(const char *s)
{
    while (s && *s) term_putc(*s++);
}

void term_draw(void)
{
    u32 w = gfx_width();
    gfx_clear(0x00080808u);
    gfx_rect(0, 0, (int)w, 28, 0x00102030u);
    gfx_text(12, 6, "UMDos Terminal  (Esc = back to desktop)", TERM_FG, 1);

    int y0 = 38;
    for (int r = 0; r < TERM_ROWS; r++) {
        if (!grid[r][0]) continue;
        gfx_text(8, y0 + r * 31, grid[r], (r == cur_row) ? TERM_PROMPT : TERM_FG, 2);
    }
    int cx = 8 + cur_col * 16;
    int cy = y0 + cur_row * 31;
    gfx_rect(cx, cy + 26, 12, 3, TERM_FG);
}

void term_run(void)
{
    term_clear();
    term_puts("UMDos shell (P11). type 'help' for commands.\n");
    sh_prompt();
    term_draw();

    input_event ev;
    for (;;) {
        if (!input_poll(&ev)) {
            continue;           /* 空闲不重绘，避免整屏刷新的开销 */
        }
        if (ev.type != EV_KEY) continue;
        int k = ev.key;
        if (k == KEY_ESC) {
            term_puts("\n[back to desktop]\n");
            term_draw();
            return;
        }
        if (k == KEY_ENTER) {
            term_putc('\n');
            linebuf[linelen] = 0;
            int n = sh_exec_line(linebuf);
            linelen = 0;
            if (n == SH_EXIT) {
                term_puts("[shell exit]\n");
                term_draw();
                return;
            }
            sh_prompt();
            term_draw();
            continue;
        }
        if (k == KEY_BACKSPACE) {
            if (linelen > 0) {
                linelen--;
                linebuf[linelen] = 0;
                if (cur_col > 0) {
                    cur_col--;
                    grid[cur_row][cur_col] = 0;
                    int bx = 8 + cur_col * 16, by = 38 + cur_row * 31;
                    gfx_rect(bx, by, 16, 30, 0x00080808u);
                }
            }
            continue;
        }
        if (k >= 32 && k < 127) {
            if (linelen < TERM_COLS - 4) {
                int cx = 8 + cur_col * 16, cy = 38 + cur_row * 31;
                char one[2];
                one[0] = (char)k;
                one[1] = 0;
                linebuf[linelen++] = (char)k;
                term_putc((char)k);              /* 先更新网格与光标 */
                gfx_text(cx, cy, one, TERM_PROMPT, 2);
                gfx_rect(8 + cur_col * 16, cy + 26, 12, 3, TERM_FG);
            }
            continue;
        }
    }
}
