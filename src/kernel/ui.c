/* ui.c - 启动画面 / 登录 / 桌面 / 我的电脑（文件管理器） */
#include "ui.h"
#include "gfx.h"
#include "input.h"
#include "fs.h"
#include "ata.h"
#include "sha256.h"
#include "auth.h"
#include "term.h"
#include "vfs.h"
#include "idt.h"
#include "sha3.h"
#include "io.h"

#define FS_ADDR   0x40000u
#define DBG(c)    outb(0xE9, (u8)(c))

/* ---------------- 中文字符串 ---------------- */
static const u32 T_MYCOMPUTER[] = { 0x6211, 0x7684, 0x7535, 0x8111, 0 };          /* 我的电脑 */
static const u32 T_LOGIN[]      = { 0x767B, 0x5F55, 0 };                          /* 登录 */
static const u32 T_USER[]       = { 0x7528, 0x6237, 0x540D, 0 };                  /* 用户名 */
static const u32 T_PASS[]       = { 0x5BC6, 0x7801, 0 };                          /* 密码 */
static const u32 T_FILES[]      = { 0x6587, 0x4EF6, 0x7BA1, 0x7406, 0x5668, 0 };  /* 文件管理器 */
static const u32 T_NAME[]       = { 0x540D, 0x79F0, 0 };                          /* 名称 */
static const u32 T_SIZE[]       = { 0x5927, 0x5C0F, 0 };                          /* 大小 */
static const u32 T_CLOSE[]      = { 0x5173, 0x95ED, 0 };                          /* 关闭 */
static const u32 T_TOTAL[]      = { 0x5171, 0 };                                  /* 共 */
static const u32 T_PIECES[]     = { 0x4E2A, 0x6587, 0x4EF6, 0 };                  /* 个文件 */
static const u32 T_SYSTEM[]     = { 0x7CFB, 0x7EDF, 0 };                          /* 系统 */
static const u32 T_NEW[]        = { 0x65B0, 0x5EFA, 0 };                          /* 新建 */
static const u32 T_DEL[]        = { 0x5220, 0x9664, 0 };                          /* 删除 */
static const u32 T_EDIT[]       = { 0x7F16, 0x8F91, 0 };                          /* 编辑 */
static const u32 T_RENAME[]     = { 0x91CD, 0x547D, 0x540D, 0 };                  /* 重命名 */

/* ---------------- 状态 ---------------- */
static const bootinfo_t *g_bi;
static int ui_state;              /* 0=桌面 1=文件管理器 2=编辑器 3=新建对话框 */
static int sel_index;
static int btn_prev;
static const char *fm_msg;        /* 文件管理器工具栏右侧的提示 */

static char user_buf[24], pass_buf[24];
static int user_len, pass_len, focus;
static const char *login_msg;
static u32 flash_tick;

/* 光标 */
static int cur_x, cur_y;
static u32 cur_save[8 * 14];
static int cur_visible;

static const u8 cursor_shape[8] = { 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF };
static const u8 cursor_tail[8]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xE0 };

/* ---------------- 时钟 ---------------- */
static u8 cmos_read(u8 reg) { outb(0x70, reg); return inb(0x71); }

static u32 bcd2bin(u8 v)
{
    if (!(cmos_read(0x0B) & 0x04))
        return (u32)((v >> 4) * 10 + (v & 0x0F));
    return v;
}

static u32 rtc_seconds(void)
{
    u32 s = 0;
    for (int i = 0; i < 4; i++) {
        u8 raw = cmos_read(0x00);
        s = bcd2bin(raw);
        if (s == bcd2bin(raw)) break;
    }
    return s;
}

static void rtc_hms(u32 *h, u32 *m, u32 *s)
{
    *s = bcd2bin(cmos_read(0x00));
    *m = bcd2bin(cmos_read(0x02));
    *h = bcd2bin(cmos_read(0x04));
}

/* ---------------- 光标 ---------------- */
static void cursor_erase(void)
{
    if (!cur_visible) return;
    for (int y = 0; y < 14; y++)
        for (int x = 0; x < 8; x++)
            gfx_pixel(cur_x + x, cur_y + y, cur_save[y * 8 + x]);
    cur_visible = 0;
}

static void cursor_draw(void)
{
    if (cur_visible) return;
    for (int y = 0; y < 14; y++)
        for (int x = 0; x < 8; x++)
            cur_save[y * 8 + x] = 0;
    /* 保存背景 */
    volatile u32 *fb = (volatile u32 *)(u64)g_bi->fb_base_lo;
    for (int y = 0; y < 14; y++) {
        for (int x = 0; x < 8; x++) {
            int px = cur_x + x, py = cur_y + y;
            if (px >= 0 && py >= 0 && (u32)px < gfx_width() && (u32)py < gfx_height())
                cur_save[y * 8 + x] = fb[(u64)py * g_bi->fb_stride + (u32)px];
        }
    }
    /* 画箭头（先黑边后白色） */
    for (int y = 0; y < 14; y++) {
        u8 bits = (y < 8) ? cursor_shape[y] : cursor_tail[y - 8];
        for (int x = 0; x < 8; x++) {
            if (!(bits & (0x80u >> x))) continue;
            gfx_pixel(cur_x + x, cur_y + y, C_BLACK);
            gfx_pixel(cur_x + x + 1, cur_y + y, C_BLACK);
            gfx_pixel(cur_x + x, cur_y + y + 1, C_BLACK);
            gfx_pixel(cur_x + x + 1, cur_y + y + 1, C_WHITE);
        }
    }
    cur_visible = 1;
}

static void cursor_move(int dx, int dy)
{
    cursor_erase();
    cur_x += dx;
    cur_y -= dy;                    /* PS/2：正 Y 向上 */
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > (int)gfx_width() - 9) cur_x = (int)gfx_width() - 9;
    if (cur_y > (int)gfx_height() - 15) cur_y = (int)gfx_height() - 15;
    cursor_draw();
}

/* ---------------- 启动画面 ---------------- */
static int load_step;
static u32 load_sum;
static int load_files;

static void do_step(int step)
{
    switch (step) {
    case 0:                             /* 读分区表 */
        DBG(0x11);
        break;
    case 1: {                           /* 校验内核镜像 */
        const u8 *k = (const u8 *)0x10000u;
        u32 n = g_bi->kernel_sectors * 512u;
        load_sum = 0;
        for (u32 i = 0; i < n; i += 64)
            load_sum += k[i];
        DBG(0x12);
        break;
    }
    case 2:                             /* 文件系统（P7 起在 kmain 里挂载） */
        if (!fs_mounted()) {
            extern int storage_init(struct bootinfo *bi);
            storage_init(g_bi);
        }
        load_files = fs_count();
        DBG(0x13);
        break;
    default:
        break;
    }
}

static void splash_static(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_gradient(0, 0, (int)w, (int)h, C_DESKTOP_1, 0x00285090u);
    gfx_text_center((int)w / 2, (int)h / 4, "A bosom friend afar", C_WHITE, 4);
    gfx_text_center((int)w / 2, (int)h / 4 + 70, "brings a distant land near.", C_WHITE, 4);
    gfx_text_center((int)w / 2, (int)h / 4 + 150,
                    "Wang Bo - Sending Du Shaofu to Shuzhou", 0x0090B8E0u, 2);
    gfx_hline((int)w / 2 - 320, (int)h / 4 + 210, 640, 0x004070A0u);
    DBG(0x10);
}

/* 状态区：画在固定深色面板里，每次只重画这一块 */
static void splash_panel(u32 el, u32 total, int step)
{
    u32 w = gfx_width(), h = gfx_height();
    int pw = 760, ph = 190;
    int px = (int)w / 2 - pw / 2, py = (int)h / 2 + 40;

    gfx_rect(px, py, pw, ph, 0x00182848u);
    gfx_frame(px, py, pw, ph, 0x00385888u);

    const char *msgs[4] = {
        "Reading partition table ...",
        "Loading kernel image ...",
        "Mounting UMDFS filesystem ...",
        "Starting user session ...",
    };
    gfx_text(px + 24, py + 18, msgs[step > 3 ? 3 : step], 0x00C8DCF0u, 2);

    int bw = pw - 48, bx = px + 24, by = py + 62;
    u32 done = (u32)step * 25u;
    u32 time_part = total ? (el * 100u) / total : 100u;
    u32 pct = done + time_part / 4u;
    if (pct > 100) pct = 100;
    gfx_frame(bx, by, bw, 24, 0x00A0C0E0u);
    gfx_rect(bx + 3, by + 3, (int)((bw - 6) * pct / 100u), 18, C_ACCENT);

    char buf[24];
    itou(pct, buf, 10, 0);
    int pct_x = px + pw - 24 - (int)strlen(buf) * 16 - 16;
    gfx_text(pct_x, py + 18, buf, C_WHITE, 2);
    gfx_text(pct_x + (int)strlen(buf) * 16, py + 18, "%", C_WHITE, 2);

    u32 left = (el < total) ? total - el : 0;
    itou(left, buf, 10, 0);
    gfx_text(px + 24, py + 118, "Time remaining:", 0x0088A8C8u, 1);
    gfx_text(px + 24 + 16 * 8, py + 112, buf, 0x00E0F0FFu, 2);
    gfx_text(px + 24 + 16 * 8 + (int)strlen(buf) * 16 + 8, py + 118, "s", 0x0088A8C8u, 1);
    gfx_text(px + 24, py + 152,
             "Loading kernel and filesystem - please wait", 0x007890B0u, 1);
}

void ui_splash(const bootinfo_t *bi)
{
    g_bi = bi;
    u32 total = bi->splash_seconds ? bi->splash_seconds : 60;
    if (total > 300) total = 300;

    splash_static();
    splash_panel(0, total, 0);

    u32 prev = rtc_seconds();
    u32 acc = 0;
    int step = 0;
    u32 last_el = 0xFFFFFFFFu;

    for (;;) {
        u32 now = rtc_seconds();
        /* 秒计数 0..59 循环：用累加器算真实经过的秒数，跨分钟不会回绕（P2 修复审计缺陷） */
        acc += (now + 60u - prev) % 60u;
        prev = now;
        u32 el = acc;
        if (step < 3 && el > (u32)step) {
            do_step(step);
            step++;
        }
        if (el != last_el) {
            last_el = el;
            splash_panel(el, total, step);
        }
        if (el >= total)
            break;
    }
    DBG(0x14);
}

/* ---------------- 登录 ---------------- */
static void field(int x, int y, int w, int h, const char *text, int len, int mask,
                  int active)
{
    gfx_rect(x, y, w, h, C_WHITE);
    gfx_frame(x, y, w, h, active ? C_ACCENT : 0x00889898u);
    int tx = x + 10, ty = y + (h - 16) / 2;
    for (int i = 0; i < len; i++)
        gfx_char(tx + i * 8, ty, mask ? '*' : text[i], C_TEXT, 1);
    if (active && ((flash_tick >> 3) & 1) == 0)
        gfx_rect(tx + len * 8, ty - 2, 2, 20, C_ACCENT);
}

static int  g_login_fails;
static u32  g_lock_start;
static int  g_locked;

/* P12：连续 3 次失败锁定 10 秒（用 RTC 秒，跨分钟安全） */
static u32 lock_elapsed(void)
{
    u32 now = rtc_seconds();
    return (now + 60u - g_lock_start) % 60u;
}

static int check_login(void)
{
    if (g_locked) {
        if (lock_elapsed() < 10u) {
            serial_puts("[kernel] login: locked\n");
            return 0;
        }
        g_locked = 0;
        serial_puts("[kernel] login: unlocked after lockout\n");
    }
    /* P2 起认证数据来自 UMDFS 的 /config/auth（SHA3-256 + 盐） */
    int rc = auth_check(user_buf, pass_buf);
    if (rc != 0) {
        g_login_fails++;
        if (g_login_fails >= 3) {
            g_login_fails = 0;
            g_locked = 1;
            g_lock_start = rtc_seconds();
            serial_puts("[kernel] login: 3 failures, locked 10s\n");
        }
        serial_puts("[kernel] auth fail rc=");
        char b[8];
        itou((u64)(-rc), b, 10, 0);
        serial_puts(b);
        serial_puts("\n");
    }
    if (rc == 0) {
        g_login_fails = 0;
        g_locked = 0;
        vfs_set_role(auth_role());
        serial_puts("[kernel] login ok as ");
        serial_puts(auth_name());
        serial_puts(auth_role() ? " (administrator)\n" : " (user)\n");
    }
    return rc == 0;
}

static void draw_login(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_gradient(0, 0, (int)w, (int)h, C_DESKTOP_1, 0x00204878u);

    int pw = 560, ph = 400;
    int px = (int)w / 2 - pw / 2, py = (int)h / 2 - ph / 2;
    gfx_round_rect(px, py, pw, ph, C_PANEL, 0x00507090u);
    gfx_rect(px + 2, py + 2, pw - 4, 56, C_TITLE);
    gfx_cjk_center((int)w / 2, py + 12, T_LOGIN, C_WHITE, 2);
    gfx_text(px + pw - 120, py + 20, "UMDos 1.0", 0x00A8C0D8u, 1);

    gfx_cjk_text(px + 48, py + 78, T_USER, C_TEXT, 2);
    field(px + 48, py + 118, pw - 96, 36, user_buf, user_len, 0, focus == 0);

    gfx_cjk_text(px + 48, py + 176, T_PASS, C_TEXT, 2);
    field(px + 48, py + 216, pw - 96, 36, pass_buf, pass_len, 1, focus == 1);

    int bx = (int)w / 2 - 90, by = py + 282;
    gfx_round_rect(bx, by, 180, 44, C_ACCENT, 0x00264C78u);
    gfx_text_center((int)w / 2, by + 6, "Sign in", C_WHITE, 2);

    if (login_msg)
        gfx_text_center((int)w / 2, py + ph - 30, login_msg, C_RED, 1);
    else
        gfx_text_center((int)w / 2, py + ph - 30,
                        "Enter = sign in   Tab = switch field", C_TEXT_DIM, 1);
}

static void append_char(char *buf, int *len, char ch, int cap)
{
    if (*len >= cap - 1) return;
    buf[*len] = ch;
    (*len)++;
    buf[*len] = 0;
}

int ui_login(const bootinfo_t *bi)
{
    g_bi = bi;
    user_len = pass_len = 0;
    user_buf[0] = pass_buf[0] = 0;
    focus = 0;
    login_msg = NULL;
    cur_x = (int)gfx_width() / 2;
    cur_y = (int)gfx_height() / 2;
    draw_login();
    cursor_draw();
    DBG(0x20);

    for (;;) {
        input_event ev;
        if (!input_poll(&ev))
            continue;
        if (ev.type == EV_MOUSE) {
            cursor_move(ev.dx, ev.dy);
            int pressed = (ev.buttons & 1) && !(btn_prev & 1);
            btn_prev = ev.buttons;
            if (pressed) {
                /* 点输入框切换焦点 */
                int pw = 560, ph = 400;
                int px = (int)gfx_width() / 2 - pw / 2, py = (int)gfx_height() / 2 - ph / 2;
                if (cur_x > px + 48 && cur_x < px + pw - 48) {
                    if (cur_y > py + 118 && cur_y < py + 154) { focus = 0; }
                    else if (cur_y > py + 216 && cur_y < py + 252) { focus = 1; }
                    else if (cur_y > py + 282 && cur_y < py + 326) {
                        if (check_login()) return 1;
                        login_msg = "Wrong username or password";
                    }
                    cursor_erase();
                    draw_login();
                    cursor_draw();
                }
            }
            continue;
        }

        flash_tick++;
        if (ev.key == KEY_TAB) {
            focus ^= 1;
        } else if (ev.key == KEY_BACKSPACE) {
            if (focus == 0 && user_len) user_buf[--user_len] = 0;
            if (focus == 1 && pass_len) pass_buf[--pass_len] = 0;
        } else if (ev.key == KEY_ENTER) {
            if (check_login())
                return 1;
            login_msg = "Wrong username or password";
        } else if (ev.key >= 32 && ev.key < 127) {
            if (focus == 0) append_char(user_buf, &user_len, (char)ev.key, 20);
            else append_char(pass_buf, &pass_len, (char)ev.key, 20);
            login_msg = NULL;
        }
        cursor_erase();
        draw_login();
        cursor_draw();
    }
}

/* ================= 桌面 / 窗口系统 ================= */
#define MAX_WIN 4
#define WT_FM   1
#define WT_ED   2

static const u32 T_UP[] = { 0x4E0A, 0x7EA7, 0 };        /* 上级 */

typedef struct {
    int open, type;
    int x, y, w, h;
    int dir;                 /* 所在目录（文件管理器 = 当前目录，编辑器 = 文件所在目录） */
    int sel;                 /* 文件管理器：选中的行 */
    char name[40];           /* 编辑器：文件名 */
    int len;                 /* 编辑器：内容长度 */
    int cur;                 /* 编辑器：光标字节偏移 */
    int anchor;              /* 编辑器：选区锚点 */
    int top;                 /* 编辑器：滚动的首行 */
    int lscroll;             /* 文件管理器：列表滚动首行 */
} uwnd;

static uwnd wins[MAX_WIN];
static int zorder[MAX_WIN];
static int nz;
static int wfocus = -1;
static char ebuf[MAX_WIN][4096];
static int drag_win = -1, drag_dx, drag_dy;
static int dlg_open, dlg_owner;
static char dlg_buf[40];
static int dlg_len;
static const char *dlg_err;
static int dlg_mode;                 /* 0=新建 1=重命名 */
static char dlg_old[40];
static int key_shift, key_ctrl;

/* 剪贴板（跨窗口共享） */
static char clipboard[2048];
static int clip_len;

/* 查找 / 替换 */
static int find_open, find_mode;     /* find_mode: 0=查找 1=替换 */
static int find_field;               /* 0=查找框 1=替换框 */
static char find_buf[40];
static int find_len;
static char repl_buf[40];
static int repl_len;

#define ICON_X   20
#define ICON_Y   470
#define ICON_W   96
#define ICON_H   96

/* ---------------- 通用绘制 ---------------- */
static void draw_computer_icon(int x, int y, int hot)
{
    u32 frame_col = hot ? C_ACCENT : 0x0080A0C0u;
    gfx_round_rect(x, y, ICON_W, ICON_H - 16, 0x00E8EEF8u, frame_col);
    gfx_rect(x + 8, y + 8, ICON_W - 16, ICON_H - 40, 0x00123060u);
    for (int i = 0; i < 4; i++)
        gfx_rect(x + 16, y + 18 + i * 10, 30 + (i * 13) % 40, 3, 0x004080C0u);
    gfx_rect(x + 16, y + 58, ICON_W - 32, 4, 0x00305080u);
    gfx_rect(x + ICON_W / 2 - 10, y + ICON_H - 16, 20, 8, frame_col);
    gfx_rect(x + ICON_W / 2 - 26, y + ICON_H - 8, 52, 6, frame_col);
}

static void draw_topbar(void)
{
    u32 w = gfx_width();
    gfx_rect(0, 0, (int)w, 44, 0x00102038u);
    gfx_rect(0, 44, (int)w, 2, C_ACCENT);
    gfx_text(16, 12, "UMDos", C_WHITE, 2);
    gfx_text(120, 16, "64-bit kernel", 0x0088A8C8u, 1);

    u32 hh, mm, ss;
    rtc_hms(&hh, &mm, &ss);
    char t[16];
    char *p = itou(hh, t, 10, 2);
    *p++ = ':';
    p = itou(mm, p, 10, 2);
    *p++ = ':';
    p = itou(ss, p, 10, 2);
    *p = 0;
    gfx_text((int)w - 170, 12, t, 0x00A8C8E8u, 2);
}

static void draw_file_icon(int x, int y)
{
    gfx_rect(x, y, 14, 18, 0x00E0E8F0u);
    gfx_frame(x, y, 14, 18, 0x00708090u);
    for (int i = 0; i < 3; i++)
        gfx_hline(x + 3, y + 5 + i * 4, 8, 0x00A0B0C0u);
}

static void draw_dir_icon(int x, int y)
{
    gfx_rect(x, y + 3, 16, 13, 0x00F0D080u);
    gfx_frame(x, y + 3, 16, 13, 0x00907830u);
    gfx_rect(x + 2, y, 7, 5, 0x00F0D080u);
    gfx_frame(x + 2, y, 7, 5, 0x00907830u);
}

static void draw_preview(int x, int y, int w, int h, const fs_entry_t *e)
{
    gfx_rect(x, y, w, h, C_WHITE);
    gfx_frame(x, y, w, h, 0x00A0B0C0u);
    if (!e) {
        gfx_text(x + 10, y + 12, "Select a file", C_TEXT_DIM, 1);
        return;
    }
    if (fs_is_dir(e)) {
        gfx_text(x + 10, y + 12, "Directory", C_TEXT, 1);
        gfx_text(x + 10, y + 32, "Double click to open", C_TEXT_DIM, 1);
        return;
    }
    const u8 *data = fs_data(e);
    if (!data) {
        gfx_text(x + 10, y + 12, "(cannot read file)", C_RED, 1);
        return;
    }
    int max_cols = (w - 20) / 8;
    int max_rows = (h - 20) / 18;
    if (max_cols < 8) max_cols = 8;
    int cx = x + 10, cy = y + 10, col = 0, row = 0;
    for (u32 i = 0; i < e->size && row < max_rows; i++) {
        char ch = (char)data[i];
        if (ch == '\r') continue;
        if (ch == '\n') { row++; cx = x + 10; col = 0; cy = y + 10 + row * 18; continue; }
        if (col >= max_cols) { row++; cx = x + 10; col = 0; cy = y + 10 + row * 18; if (row >= max_rows) break; }
        gfx_char(cx, cy, ch, C_TEXT, 1);
        cx += 8; col++;
    }
}

/* ---------------- 窗口管理 ---------------- */
static int win_index(const uwnd *w) { return (int)(w - wins); }

static void raise_win(int i)
{
    int pos = -1;
    for (int k = 0; k < nz; k++)
        if (zorder[k] == i) { pos = k; break; }
    if (pos < 0) return;
    for (int k = pos; k < nz - 1; k++)
        zorder[k] = zorder[k + 1];
    zorder[nz - 1] = i;
    wfocus = i;
}

static void close_win(int i)
{
    if (!wins[i].open) return;
    wins[i].open = 0;
    int pos = -1;
    for (int k = 0; k < nz; k++)
        if (zorder[k] == i) { pos = k; break; }
    if (pos >= 0) {
        for (int k = pos; k < nz - 1; k++)
            zorder[k] = zorder[k + 1];
        nz--;
    }
    if (nz > 0) wfocus = zorder[nz - 1];
    else wfocus = -1;
}

static int win_alloc(void)
{
    for (int i = 0; i < MAX_WIN; i++)
        if (!wins[i].open) return i;
    return -1;
}

static int win_open_fm(void)
{
    int i = win_alloc();
    if (i < 0) return -1;
    uwnd *w = &wins[i];
    w->open = 1;
    w->type = WT_FM;
    w->w = 760;
    w->h = 470;
    w->x = 150 + (nz * 44) % 320;
    w->y = 60 + (nz * 34) % 180;
    w->dir = (u32)fs_dir();
    w->sel = -1;
    w->lscroll = 0;
    zorder[nz++] = i;
    wfocus = i;
    return i;
}

static int win_open_ed(int dir, const fs_entry_t *e)
{
    int i = win_alloc();
    if (i < 0) return -1;
    uwnd *w = &wins[i];
    w->open = 1;
    w->type = WT_ED;
    w->w = 780;
    w->h = 520;
    w->x = 190 + (nz * 40) % 280;
    w->y = 70 + (nz * 30) % 160;
    w->dir = dir;
    strcpy_s(w->name, sizeof(w->name), e->name);
    const u8 *d = fs_data(e);
    u32 n = e->size;
    if (n > 4095) n = 4095;
    w->len = 0;
    if (d) {
        for (u32 k = 0; k < n; k++) ebuf[i][k] = (char)d[k];
        w->len = (int)n;
    }
    ebuf[i][w->len] = 0;
    w->cur = w->len;
    w->anchor = w->len;
    w->top = 0;
    w->lscroll = 0;
    w->sel = 0;
    zorder[nz++] = i;
    wfocus = i;
    return i;
}

/* ---------------- 文件管理器窗口 ---------------- */
static void draw_fm(uwnd *w)
{
    int idx = win_index(w);
    char path[80];
    fs_set_dir((u32)w->dir);
    fs_path(path, sizeof(path));

    int tx = w->x, ty = w->y, tw = w->w, th = w->h;
    gfx_round_rect(tx, ty, tw, th, C_PANEL, wfocus == idx ? C_ACCENT : 0x00405878u);
    gfx_rect(tx + 2, ty + 2, tw - 4, 42, wfocus == idx ? C_TITLE : 0x002A3E5Cu);

    int hx = tx + 12;
    gfx_cjk_text(hx, ty + 12, T_MYCOMPUTER, C_WHITE, 2);
    hx += gfx_cjk_width(T_MYCOMPUTER, 2) + 12;
    gfx_text(hx, ty + 18, path, 0x00A8C0E0u, 1);

    int cbx = tx + tw - 36, cby = ty + 9;
    gfx_round_rect(cbx, cby, 26, 26, 0x00C04040u, 0x00802020u);
    gfx_text(cbx + 7, cby + 3, "X", C_WHITE, 2);

    /* 工具栏 */
    int bty = ty + 52;
    gfx_round_rect(tx + 10, bty, 62, 26, 0x00D8E4F0u, 0x008098B0u);
    gfx_cjk_text(tx + 14, bty + 5, T_NEW, C_TEXT, 1);
    gfx_round_rect(tx + 78, bty, 62, 26, 0x00D8E4F0u, 0x008098B0u);
    gfx_cjk_text(tx + 82, bty + 5, T_DEL, C_TEXT, 1);
    gfx_round_rect(tx + 146, bty, 62, 26, 0x00D8E4F0u, 0x008098B0u);
    gfx_cjk_text(tx + 156, bty + 5, T_UP, C_TEXT, 1);

    char tmp[16];
    itou((u32)fs_count(), tmp, 10, 0);
    gfx_text(tx + 222, bty + 8, tmp, C_TEXT, 1);
    gfx_text(tx + 222 + (int)strlen(tmp) * 8, bty + 8, " items", C_TEXT_DIM, 1);
    if (fm_msg)
        gfx_text(tx + tw - 10 - gfx_text_width(fm_msg, 1), bty + 8, fm_msg, C_TEXT_DIM, 1);
    gfx_hline(tx + 8, ty + 84, tw - 16, 0x00B0C0D0u);

    /* 列表 */
    int list_w = tw * 58 / 100;
    int list_y = ty + 90;
    int list_h = th - 90 - 32;
    gfx_rect(tx + 8, list_y, list_w - 12, list_h, C_WHITE);
    gfx_frame(tx + 8, list_y, list_w - 12, list_h, 0x00A0B0C0u);

    int n = fs_count();
    int rows = (list_h - 4) / 30;
    if (rows < 1) rows = 1;
    if (w->sel >= 0) {
        if (w->sel < w->lscroll) w->lscroll = w->sel;
        if (w->sel >= w->lscroll + rows) w->lscroll = w->sel - rows + 1;
    }
    if (w->lscroll < 0) w->lscroll = 0;
    if (w->lscroll > n - rows) w->lscroll = n - rows;
    if (w->lscroll < 0) w->lscroll = 0;

    for (int i = 0; i < rows; i++) {
        int idx = w->lscroll + i;
        if (idx >= n) break;
        const fs_entry_t *e = fs_get(idx);
        if (!e) continue;
        int ry = list_y + 2 + i * 30;
        if (idx == w->sel)
            gfx_rect(tx + 10, ry, list_w - 20, 28, C_SELECT);
        if (fs_is_dir(e)) draw_dir_icon(tx + 18, ry + 5);
        else draw_file_icon(tx + 18, ry + 4);
        gfx_text(tx + 42, ry + 6, e->name, C_TEXT, 1);
        if (fs_is_dir(e)) {
            gfx_text(tx + 42 + (int)strlen(e->name) * 8 + 8, ry + 6, "<DIR>", C_TEXT_DIM, 1);
        } else {
            char sz[16];
            itou(e->size, sz, 10, 0);
            gfx_text(tx + list_w - 62, ry + 6, sz, C_TEXT_DIM, 1);
        }
    }

    /* 列表滚动条 */
    if (n > rows) {
        int sx = tx + list_w - 9, sy = list_y + 2, sh = list_h - 6;
        gfx_rect(sx, sy, 7, sh, 0x00E4EAF0u);
        int thumb = sh * rows / n;
        if (thumb < 16) thumb = 16;
        int span = sh - thumb;
        int off = (n > rows) ? (w->lscroll * span / (n - rows)) : 0;
        if (off < 0) off = 0;
        if (off > span) off = span;
        gfx_rect(sx + 1, sy + off, 5, thumb, 0x0090A8C0u);
    }

    /* 预览 */
    int px = tx + list_w + 6, pw = tw - list_w - 14;
    draw_preview(px, list_y, pw, list_h, fs_get(w->sel));

    /* 状态栏 */
    int sy = ty + th - 26;
    gfx_hline(tx + 8, sy - 4, tw - 16, 0x00B0C0D0u);
    gfx_cjk_text(tx + 10, sy, T_TOTAL, C_TEXT, 1);
    itou((u32)n, tmp, 10, 0);
    gfx_text(tx + 32, sy, tmp, C_TEXT, 1);
    gfx_cjk_text(tx + 32 + (int)strlen(tmp) * 8 + 8, sy, T_PIECES, C_TEXT, 1);
    char info[40];
    char *p = info;
    const char *t1 = "free ";
    while (*t1) *p++ = *t1++;
    p = itou(fs_free_blocks(), p, 10, 0);
    const char *t2 = " blocks";
    while (*t2) *p++ = *t2++;
    *p = 0;
    gfx_text(tx + tw - 10 - gfx_text_width(info, 1), sy, info, C_TEXT_DIM, 1);
}

/* ---------------- 编辑器窗口 ---------------- */
static void ed_text_box(uwnd *w, int *bx, int *by, int *bw, int *bh)
{
    *bx = w->x + 10;
    *by = w->y + 52;
    *bw = w->w - 20;
    *bh = w->h - 52 - 28;
}

static int ed_cols(uwnd *w)
{
    int bx, by, bw, bh;
    ed_text_box(w, &bx, &by, &bw, &bh);
    int c = (bw - 20) / 8;
    return c < 8 ? 8 : c;
}

static int ed_vis_rows(uwnd *w)
{
    int bx, by, bw, bh;
    ed_text_box(w, &bx, &by, &bw, &bh);
    int r = (bh - 16) / 18;
    return r < 1 ? 1 : r;
}

/* 把缓冲区按可视行切好：lay_start[i] 起始偏移，lay_len[i] 字符数 */
static int lay_start[512];
static int lay_len[512];

static int ed_layout(uwnd *w, int cols)
{
    const char *b = ebuf[win_index(w)];
    int n = 0, i = 0;
    while (n < 512) {
        int s = i, cnt = 0;
        while (i < w->len && b[i] != '\n' && cnt < cols) { i++; cnt++; }
        lay_start[n] = s;
        lay_len[n] = cnt;
        n++;
        if (i < w->len && b[i] == '\n') { i++; continue; }
        if (i >= w->len) break;
    }
    if (n == 0) { lay_start[0] = 0; lay_len[0] = 0; n = 1; }
    return n;
}

/* 光标所在的可视行号 */
static int ed_cursor_row(uwnd *w, int nrows)
{
    for (int r = 0; r < nrows; r++) {
        int end = lay_start[r] + lay_len[r];
        if (w->cur >= lay_start[r] && w->cur <= end)
            return r;
    }
    return nrows - 1;
}

static void ed_sel_range(uwnd *w, int *s, int *e);

static void ed_draw_text(uwnd *w)
{
    int bx, by, bw, bh;
    ed_text_box(w, &bx, &by, &bw, &bh);
    gfx_rect(bx + 1, by + 1, bw - 2, bh - 2, C_WHITE);

    int cols = ed_cols(w);
    int rows = ed_vis_rows(w);
    int nrows = ed_layout(w, cols);
    int crow = ed_cursor_row(w, nrows);
    if (w->cur > w->len) w->cur = w->len;
    if (w->cur < 0) w->cur = 0;

    if (crow < w->top) w->top = crow;
    if (crow >= w->top + rows) w->top = crow - rows + 1;
    if (w->top < 0) w->top = 0;

    const char *b = ebuf[win_index(w)];
    int ss, se;
    ed_sel_range(w, &ss, &se);

    for (int r = w->top; r < nrows && r < w->top + rows; r++) {
        int y = by + 8 + (r - w->top) * 18;
        int rs = lay_start[r], rl = lay_len[r];
        /* 选区高亮 */
        if (se > ss) {
            int a = rs > ss ? rs : ss;
            int z = (rs + rl) < se ? (rs + rl) : se;
            if (z > a)
                gfx_rect(bx + 10 + (a - rs) * 8, y - 1,
                         (z - a) * 8, 18, 0x00B8D4F0u);
        }
        for (int c = 0; c < rl; c++)
            gfx_char(bx + 10 + c * 8, y, b[rs + c], C_TEXT, 1);
    }

    if (crow >= w->top && crow < w->top + rows) {
        int y = by + 8 + (crow - w->top) * 18;
        int cx = bx + 10 + (w->cur - lay_start[crow]) * 8;
        gfx_rect(cx, y, 2, 16, C_ACCENT);
    }

    /* 滚动条 */
    if (nrows > rows) {
        int sx = bx + bw - 9, sy = by + 4, sh = bh - 8;
        gfx_rect(sx, sy, 8, sh, 0x00E4EAF0u);
        int thumb = sh * rows / nrows;
        if (thumb < 16) thumb = 16;
        int span = sh - thumb;
        int off = (nrows > rows) ? (w->top * span / (nrows - rows)) : 0;
        if (off < 0) off = 0;
        if (off > span) off = span;
        gfx_rect(sx + 1, sy + off, 6, thumb, 0x0090A8C0u);
    }

    /* 行号 / 总行数（放在状态行左侧） */
    char info[24];
    itou((u32)(crow + 1), info, 10, 0);
    gfx_text(w->x + 350, by + bh + 4, "line ", C_TEXT_DIM, 1);
    int ix = w->x + 350 + 40;
    gfx_text(ix, by + bh + 4, info, C_TEXT_DIM, 1);
    ix += (int)strlen(info) * 8;
    gfx_text(ix, by + bh + 4, "/", C_TEXT_DIM, 1);
    itou((u32)nrows, info, 10, 0);
    gfx_text(ix + 8, by + bh + 4, info, C_TEXT_DIM, 1);
}

/* ---------------- 编辑器编辑操作 ---------------- */
static void ed_insert(uwnd *w, char c)
{
    char *b = ebuf[win_index(w)];
    if (w->len >= 4095) return;
    for (int i = w->len; i > w->cur; i--)
        b[i] = b[i - 1];
    b[w->cur] = c;
    w->len++;
    w->cur++;
    w->anchor = w->cur;          /* 插入后取消选区，否则连续打字会互相覆盖 */
    b[w->len] = 0;
}

static void ed_backspace(uwnd *w)
{
    char *b = ebuf[win_index(w)];
    if (w->cur <= 0) return;
    for (int i = w->cur - 1; i < w->len - 1; i++)
        b[i] = b[i + 1];
    w->len--;
    w->cur--;
    b[w->len] = 0;
}

static void ed_delete(uwnd *w)
{
    char *b = ebuf[win_index(w)];
    if (w->cur >= w->len) return;
    for (int i = w->cur; i < w->len - 1; i++)
        b[i] = b[i + 1];
    w->len--;
    b[w->len] = 0;
}

static void ed_move_vert(uwnd *w, int dir)
{
    int cols = ed_cols(w);
    int nrows = ed_layout(w, cols);
    int r = ed_cursor_row(w, nrows);
    int col = w->cur - lay_start[r];
    int r2 = r + dir;
    if (r2 < 0 || r2 >= nrows) return;
    int target = lay_start[r2] + (col < lay_len[r2] ? col : lay_len[r2]);
    w->cur = target;
}

static void ed_home(uwnd *w)
{
    int nrows = ed_layout(w, ed_cols(w));
    int r = ed_cursor_row(w, nrows);
    w->cur = lay_start[r];
}

static void ed_end(uwnd *w)
{
    int nrows = ed_layout(w, ed_cols(w));
    int r = ed_cursor_row(w, nrows);
    w->cur = lay_start[r] + lay_len[r];
}

/* ---------------- 选区 / 剪贴板 / 缩进 / 查找 ---------------- */
static void ed_sel_range(uwnd *w, int *s, int *e)
{
    int a = w->cur, b = w->anchor;
    if (a == b) { *s = *e = a; return; }
    if (a > b) { *s = b; *e = a; } else { *s = a; *e = b; }
    if (*s < 0) *s = 0;
    if (*e > w->len) *e = w->len;
}

static void ed_delete_range(uwnd *w, int s, int e)
{
    char *b = ebuf[win_index(w)];
    if (e <= s) return;
    for (int i = e; i < w->len; i++)
        b[i - (e - s)] = b[i];
    w->len -= (e - s);
    b[w->len] = 0;
    w->cur = s;
    w->anchor = s;
}

static void ed_insert_bytes(uwnd *w, const char *src, int n)
{
    char *b = ebuf[win_index(w)];
    if (n <= 0) return;
    if (w->len + n > 4095) n = 4095 - w->len;
    if (n <= 0) return;
    for (int i = w->len - 1; i >= w->cur; i--)
        b[i + n] = b[i];
    for (int i = 0; i < n; i++)
        b[w->cur + i] = src[i];
    w->len += n;
    w->cur += n;
    w->anchor = w->cur;
    b[w->len] = 0;
}

static void ed_copy(uwnd *w)
{
    int s, e;
    ed_sel_range(w, &s, &e);
    if (e <= s) return;
    clip_len = e - s;
    if (clip_len > (int)sizeof(clipboard)) clip_len = (int)sizeof(clipboard);
    for (int i = 0; i < clip_len; i++)
        clipboard[i] = ebuf[win_index(w)][s + i];
}

static void ed_cut(uwnd *w)
{
    int s, e;
    ed_sel_range(w, &s, &e);
    if (e <= s) return;
    ed_copy(w);
    ed_delete_range(w, s, e);
}

static void ed_paste(uwnd *w)
{
    if (clip_len <= 0) return;
    ed_insert_bytes(w, clipboard, clip_len);
}

static void ed_select_all(uwnd *w)
{
    w->anchor = 0;
    w->cur = w->len;
}

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* 从 from 开始找，返回 1 并选中；找不到就从头再来一次 */
static int ed_find_next(uwnd *w, int from)
{
    if (find_len <= 0) return 0;
    char *b = ebuf[win_index(w)];
    for (int i = from; i + find_len <= w->len; i++) {
        if (ci_eq(b + i, find_buf, find_len)) {
            w->cur = i;
            w->anchor = i + find_len;
            return 1;
        }
    }
    for (int i = 0; i + find_len <= w->len; i++) {
        if (ci_eq(b + i, find_buf, find_len)) {
            w->cur = i;
            w->anchor = i + find_len;
            return 1;
        }
    }
    return 0;
}

static void ed_replace_one(uwnd *w)
{
    int s, e;
    ed_sel_range(w, &s, &e);
    if (find_len <= 0) return;
    char *b = ebuf[win_index(w)];
    if (e - s == find_len && ci_eq(b + s, find_buf, find_len)) {
        ed_delete_range(w, s, e);
        int rl = repl_len;
        if (rl > 0) ed_insert_bytes(w, repl_buf, rl);
        ed_find_next(w, w->cur);
    } else {
        ed_find_next(w, w->cur);
    }
}

static void ed_replace_all(uwnd *w)
{
    if (find_len <= 0) return;
    char *b = ebuf[win_index(w)];
    int n = 0;
    for (int i = 0; i + find_len <= w->len; ) {
        if (ci_eq(b + i, find_buf, find_len)) {
            ed_delete_range(w, i, i + find_len);
            if (repl_len > 0) ed_insert_bytes(w, repl_buf, repl_len);
            i += repl_len;
            n++;
        } else {
            i++;
        }
    }
    (void)n;
}

/* Tab / Shift+Tab：整行或多行缩进 */
static void ed_indent(uwnd *w, int unindent)
{
    char *b = ebuf[win_index(w)];
    int s, e;
    ed_sel_range(w, &s, &e);
    if (s == e) {
        if (unindent) {
            int ls = s;
            while (ls > 0 && b[ls - 1] != '\n') ls--;
            int k = 0;
            while (ls + k < w->len && b[ls + k] == ' ' && k < 4) k++;
            if (k) ed_delete_range(w, ls, ls + k);
        } else {
            ed_insert_bytes(w, "    ", 4);
        }
        return;
    }

    int starts[64];
    int n = 0;
    int p = s;
    while (p <= e && n < 64) {
        starts[n++] = p;
        while (p < w->len && b[p] != '\n') p++;
        p++;
    }
    int last = starts[n - 1];

    if (unindent) {
        for (int i = n - 1; i >= 0; i--) {
            int st = starts[i];
            int k = 0;
            while (st + k < w->len && b[st + k] == ' ' && k < 4) k++;
            if (k) ed_delete_range(w, st, st + k);
        }
        w->anchor = starts[0];
        w->cur = last - 4 * n > w->anchor ? last - 4 * n : w->anchor;
    } else {
        for (int i = n - 1; i >= 0; i--) {
            w->cur = starts[i];
            w->anchor = starts[i];
            ed_insert_bytes(w, "    ", 4);
        }
        w->anchor = starts[0];
        w->cur = last + 4 * n;
    }
    if (w->cur > w->len) w->cur = w->len;
}

static void ed_draw_findbar(uwnd *w)
{
    int bx, by, bw, bh;
    ed_text_box(w, &bx, &by, &bw, &bh);
    int y = by + bh - 32;
    gfx_rect(bx + 1, y, bw - 2, 30, 0x00E8EEF6u);
    gfx_hline(bx + 1, y, bw - 2, 0x00A0B0C0u);

    gfx_text(bx + 8, y + 8, "Find:", C_TEXT, 1);
    int fx = bx + 52;
    gfx_rect(fx, y + 5, 140, 20, C_WHITE);
    gfx_frame(fx, y + 5, 140, 20, find_field == 0 ? C_ACCENT : 0x00A0B0C0u);
    gfx_text(fx + 4, y + 8, find_buf, C_TEXT, 1);

    if (find_mode) {
        gfx_text(fx + 150, y + 8, "Replace:", C_TEXT, 1);
        int rx = fx + 220;
        gfx_rect(rx, y + 5, 140, 20, C_WHITE);
        gfx_frame(rx, y + 5, 140, 20, find_field == 1 ? C_ACCENT : 0x00A0B0C0u);
        gfx_text(rx + 4, y + 8, repl_buf, C_TEXT, 1);
    }
    gfx_text(bx + bw - 250, y + 8, "Enter=go Tab=field Esc=close", C_TEXT_DIM, 1);
}

static void draw_ed(uwnd *w)
{
    int idx = win_index(w);
    int tx = w->x, ty = w->y, tw = w->w, th = w->h;
    gfx_round_rect(tx, ty, tw, th, C_PANEL, wfocus == idx ? C_ACCENT : 0x00405878u);
    gfx_rect(tx + 2, ty + 2, tw - 4, 42, wfocus == idx ? C_TITLE : 0x002A3E5Cu);
    gfx_cjk_text(tx + 12, ty + 12, T_EDIT, C_WHITE, 2);
    gfx_text(tx + 12 + 64 + 10, ty + 18, "-", 0x00A8C0E0u, 1);
    gfx_text(tx + 12 + 64 + 26, ty + 18, w->name, 0x00A8C0E0u, 1);
    int cbx = tx + tw - 36, cby = ty + 9;
    gfx_round_rect(cbx, cby, 26, 26, 0x00C04040u, 0x00802020u);
    gfx_text(cbx + 7, cby + 3, "X", C_WHITE, 2);

    int bx, by, bw, bh;
    ed_text_box(w, &bx, &by, &bw, &bh);
    gfx_rect(bx, by, bw, bh, C_WHITE);
    gfx_frame(bx, by, bw, bh, 0x00A0B0C0u);
    ed_draw_text(w);
    if (find_open) ed_draw_findbar(w);

    gfx_text(tx + 10, ty + th - 22, "Esc=save ^C ^X ^V ^F ^H Tab=indent", C_TEXT_DIM, 1);
    char meta[24];
    char *p = meta;
    p = itou((u32)w->len, p, 10, 0);
    const char *t = " bytes";
    while (*t) *p++ = *t++;
    *p = 0;
    gfx_text(tx + tw - 10 - gfx_text_width(meta, 1), ty + th - 22, meta, C_TEXT_DIM, 1);
}

/* ---------------- 桌面 / 任务栏 ---------------- */
static void draw_taskbar(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_rect(0, (int)h - 40, (int)w, 40, 0x00102038u);
    gfx_text(12, (int)h - 30, "Wins:", 0x0080A0C0u, 1);
    for (int k = 0; k < nz; k++) {
        uwnd *w2 = &wins[zorder[k]];
        int bxx = 70 + k * 176, byy = (int)h - 34;
        int hot = (wfocus == zorder[k]);
        gfx_round_rect(bxx, byy, 168, 28, hot ? 0x003A5E8Cu : 0x00203050u,
                       hot ? C_ACCENT : 0x00405878u);
        if (w2->type == WT_FM) {
            gfx_cjk_text(bxx + 8, byy + 6, T_MYCOMPUTER, C_WHITE, 1);
        } else {
            gfx_text(bxx + 8, byy + 7, "ED:", 0x00A8C0E0u, 1);
            gfx_text(bxx + 32, byy + 7, w2->name, C_WHITE, 1);
        }
    }
}

static void draw_desktop_bg(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_gradient(0, 0, (int)w, (int)h, C_DESKTOP_1, 0x00285088u);
    draw_topbar();
    draw_computer_icon(ICON_X, ICON_Y, 0);
    gfx_cjk_center(ICON_X + ICON_W / 2, ICON_Y + ICON_H + 6, T_MYCOMPUTER, C_WHITE, 2);
    gfx_text(ICON_X - 6, ICON_Y + ICON_H + 42, "double click", 0x0080A0C8u, 1);
    draw_taskbar();
}

static void draw_windows(void)
{
    for (int k = 0; k < nz; k++) {
        uwnd *w = &wins[zorder[k]];
        if (!w->open) continue;
        if (w->type == WT_FM) draw_fm(w);
        else draw_ed(w);
    }
}

/* ---------------- 新建对话框 ---------------- */
static void draw_dialog(void)
{
    uwnd *owner = &wins[dlg_owner];
    int px = owner->x + owner->w / 2 - 220;
    int py = owner->y + owner->h / 2 - 80;
    gfx_round_rect(px, py, 440, 170, C_PANEL, 0x00405878u);
    gfx_rect(px + 2, py + 2, 436, 38, C_TITLE);
    if (dlg_mode == 1) {
        gfx_cjk_text(px + 14, py + 8, T_RENAME, C_WHITE, 2);
        gfx_text(px + 120, py + 14, "rename to", 0x00A8C0E0u, 1);
    } else {
        gfx_cjk_text(px + 14, py + 8, T_NEW, C_WHITE, 2);
        gfx_text(px + 84, py + 14, "name (file or folder)", 0x00A8C0E0u, 1);
    }

    gfx_rect(px + 20, py + 56, 400, 32, C_WHITE);
    gfx_frame(px + 20, py + 56, 400, 32, C_ACCENT);
    gfx_text(px + 28, py + 64, dlg_buf, C_TEXT, 1);
    gfx_rect(px + 28 + dlg_len * 8, py + 62, 2, 20, C_ACCENT);

    if (dlg_err)
        gfx_text(px + 20, py + 98, dlg_err, C_RED, 1);
    else
        gfx_text(px + 20, py + 98, "Enter = OK   Esc = Cancel   (end with \\ for folder)",
                 C_TEXT_DIM, 1);
    gfx_text(px + 20, py + 126, "e.g. TEST.TXT  or  NEWDIR\\", C_TEXT_DIM, 1);
}

/* ---------------- 重绘 ---------------- */
static void redraw(void)
{
    cursor_erase();
    draw_desktop_bg();
    draw_windows();
    if (dlg_open) draw_dialog();
    cursor_draw();
}

static void redraw_ed_fast(uwnd *w)
{
    cursor_erase();
    draw_ed(w);
    cursor_draw();
}

/* ---------------- 操作 ---------------- */
static void fm_enter_dir(uwnd *w, const fs_entry_t *e)
{
    int idx = fs_entry_index(e);
    if (idx < 0) return;
    w->dir = (u32)idx;
    w->sel = -1;
    fm_msg = NULL;
    redraw();
    DBG(0x3A);
}

static void fm_open_selected(uwnd *w)
{
    fs_set_dir((u32)w->dir);
    const fs_entry_t *e = fs_get(w->sel);
    if (!e) return;
    if (fs_is_dir(e)) {
        fm_enter_dir(w, e);
        return;
    }
    win_open_ed((int)w->dir, e);
    redraw();
    DBG(0x34);
}

static void fm_delete_selected(uwnd *w)
{
    fs_set_dir((u32)w->dir);
    const fs_entry_t *e = fs_get(w->sel);
    if (!e) return;
    char nm[40];
    strcpy_s(nm, sizeof(nm), e->name);
    int rc = fs_unlink(nm);
    if (rc == 0) {
        w->sel = -1;
        fm_msg = (fs_flush() == 0) ? "Deleted from disk" : "Deleted (RAM only)";
    } else if (rc == -4) {
        fm_msg = "Directory not empty";
    } else {
        fm_msg = "Delete failed";
    }
    redraw();
    DBG(0x38);
}

static void fm_up(uwnd *w)
{
    fs_set_dir((u32)w->dir);
    if (fs_up() == 0) {
        w->dir = (u32)fs_dir();
        w->sel = -1;
        fm_msg = NULL;
        redraw();
        DBG(0x3B);
    }
}

static void ed_save(uwnd *w)
{
    fs_set_dir((u32)w->dir);
    fs_write(w->name, ebuf[win_index(w)], (u32)w->len);
    fm_msg = (fs_flush() == 0) ? "Saved to disk" : "Saved (RAM only)";
    close_win(win_index(w));
    redraw();
    DBG(0x35);
}

static void fm_rename_open(uwnd *w)
{
    fs_set_dir((u32)w->dir);
    const fs_entry_t *e = fs_get(w->sel);
    if (!e) return;
    const char *n = e->name;
    int k = 0;
    while (n[k] && k < 39) { dlg_buf[k] = n[k]; k++; }
    dlg_buf[k] = 0;
    dlg_len = k;
    strcpy_s(dlg_old, sizeof(dlg_old), e->name);
    dlg_mode = 1;
    dlg_err = NULL;
    dlg_open = 1;
    dlg_owner = wfocus;
    redraw();
    DBG(0x40);
}

static void dlg_open_new(int owner)
{
    dlg_mode = 0;
    const char *def = "NEW.TXT";
    int k = 0;
    while (def[k]) { dlg_buf[k] = def[k]; k++; }
    dlg_buf[k] = 0;
    dlg_len = k;
    dlg_err = NULL;
    dlg_open = 1;
    dlg_owner = owner;
    redraw();
    DBG(0x37);
}

static void dlg_commit(void)
{
    uwnd *w = &wins[dlg_owner];
    fs_set_dir((u32)w->dir);

    if (dlg_mode == 1) {                    /* 重命名 */
        if (dlg_len == 0) { dlg_err = "Name cannot be empty"; redraw(); return; }
        if (strcmp(dlg_old, dlg_buf) != 0) {
            if (fs_rename(dlg_old, dlg_buf) != 0) {
                dlg_err = "Rename failed (exists?)";
                redraw();
                return;
            }
            fm_msg = (fs_flush() == 0) ? "Renamed on disk" : "Renamed (RAM only)";
        }
        for (int i = 0; i < fs_count(); i++) {
            const fs_entry_t *e = fs_get(i);
            if (e && strcmp(e->name, dlg_buf) == 0) { w->sel = i; break; }
        }
        dlg_mode = 0;
        dlg_open = 0;
        redraw();
        DBG(0x41);
        return;
    }

    if (dlg_len == 0) {
        dlg_err = "Name cannot be empty";
        redraw();
        return;
    }
    int is_dir = 0;
    if (dlg_buf[dlg_len - 1] == '\\') { is_dir = 1; dlg_buf[--dlg_len] = 0; }
    if (dlg_len == 0) { dlg_err = "Name cannot be empty"; redraw(); return; }
    if (fs_find(dlg_buf) >= 0) { dlg_err = "Already exists"; redraw(); return; }

    if (is_dir) {
        if (fs_mkdir(dlg_buf) != 0) { dlg_err = "Cannot create folder"; redraw(); return; }
        fm_msg = (fs_flush() == 0) ? "Folder created" : "Folder (RAM only)";
    } else {
        if (fs_write(dlg_buf, NULL, 0) != 0) { dlg_err = "Cannot create file"; redraw(); return; }
        fm_msg = (fs_flush() == 0) ? "Created on disk" : "Created (RAM only)";
    }
    for (int i = 0; i < fs_count(); i++) {
        const fs_entry_t *e = fs_get(i);
        if (e && strcmp(e->name, dlg_buf) == 0) { w->sel = i; break; }
    }
    dlg_open = 0;
    redraw();
    DBG(0x36);
}

/* ---------------- 命中测试 ---------------- */
static int win_at(int x, int y)
{
    for (int k = nz - 1; k >= 0; k--) {
        uwnd *w = &wins[zorder[k]];
        if (w->open && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
            return zorder[k];
    }
    return -1;
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---------------- 事件处理 ---------------- */
static void panic_demo(void)
{
    serial_puts("[kernel] panic demo: F9 -> int3\n");
    __asm__ volatile("int3");
}

static void handle_key(int k)
{
    if (k == KEY_F9) { panic_demo(); return; }
    if (k == KEY_F10) { term_run(); redraw(); return; }
    if (dlg_open) {
        if (k == KEY_ESC) { dlg_open = 0; dlg_err = NULL; redraw(); }
        else if (k == KEY_BACKSPACE) {
            if (dlg_len) dlg_buf[--dlg_len] = 0;
            redraw();
        } else if (k == KEY_ENTER) {
            dlg_commit();
        } else if (k >= 32 && k < 127) {
            if (dlg_len < (int)sizeof(dlg_buf) - 1) {
                dlg_buf[dlg_len++] = (char)k;
                dlg_buf[dlg_len] = 0;
            }
            dlg_err = NULL;
            redraw();
        }
        return;
    }

    if (wfocus < 0 || !wins[wfocus].open) {
        if (k == KEY_ENTER) {
            if (win_open_fm() >= 0) { redraw(); DBG(0x31); }
        }
        return;
    }

    uwnd *w = &wins[wfocus];
    if (w->type == WT_FM) {
        if (k == KEY_ESC) { wfocus = -1; redraw(); DBG(0x32); }
        else if (k == KEY_ENTER) fm_open_selected(w);
        else if (k == KEY_BACKSPACE) fm_up(w);
        else if (k == KEY_DOWN) {
            int n = fs_count();
            if (w->sel < n - 1) { w->sel++; redraw(); }
        } else if (k == KEY_UP) {
            if (w->sel > 0) { w->sel--; redraw(); }
        }
        else if (k == 'n' || k == 'N') dlg_open_new(wfocus);
        else if (k == 'd' || k == 'D') fm_delete_selected(w);
        else if (k == KEY_F2) fm_rename_open(w);
    } else {
        int moved = 0;
        int is_move = 0;

        /* 查找/替换栏优先处理按键 */
        if (find_open) {
            if (k == KEY_ESC) { find_open = 0; }
            else if (k == KEY_TAB && find_mode) { find_field ^= 1; }
            else if (k == KEY_ENTER) {
                if (find_field == 0) {
                    if (find_len) ed_find_next(w, w->cur);
                } else {
                    if (key_ctrl) ed_replace_all(w);
                    else ed_replace_one(w);
                }
            } else if (k == KEY_BACKSPACE) {
                char *buf = (find_field == 0) ? find_buf : repl_buf;
                int *len = (find_field == 0) ? &find_len : &repl_len;
                if (*len) buf[--(*len)] = 0;
            } else if (k >= 32 && k < 127) {
                char *buf = (find_field == 0) ? find_buf : repl_buf;
                int *len = (find_field == 0) ? &find_len : &repl_len;
                if (*len < 39) { buf[(*len)++] = (char)k; buf[*len] = 0; }
            }
            redraw_ed_fast(w);
            return;
        }

        if (k == KEY_ESC) { ed_save(w); return; }

        if (key_ctrl) {
            if (k == 'c' || k == 'C') ed_copy(w);
            else if (k == 'x' || k == 'X') ed_cut(w);
            else if (k == 'v' || k == 'V') ed_paste(w);
            else if (k == 'a' || k == 'A') ed_select_all(w);
            else if (k == 'f' || k == 'F') {
                find_open = 1; find_mode = 0; find_field = 0;
                find_len = 0; find_buf[0] = 0;
            } else if (k == 'h' || k == 'H') {
                find_open = 1; find_mode = 1; find_field = 0;
                find_len = 0; find_buf[0] = 0;
                repl_len = 0; repl_buf[0] = 0;
            } else if (k == KEY_HOME) {
                w->cur = 0; w->anchor = 0;
            } else if (k == KEY_END) {
                w->cur = w->len; w->anchor = w->len;
            }
            redraw_ed_fast(w);
            return;
        }

        if (k == KEY_LEFT) { if (w->cur > 0) w->cur--; moved = 1; is_move = 1; }
        else if (k == KEY_RIGHT) { if (w->cur < w->len) w->cur++; moved = 1; is_move = 1; }
        else if (k == KEY_UP) { ed_move_vert(w, -1); moved = 1; is_move = 1; }
        else if (k == KEY_DOWN) { ed_move_vert(w, 1); moved = 1; is_move = 1; }
        else if (k == KEY_HOME) { ed_home(w); moved = 1; is_move = 1; }
        else if (k == KEY_END) { ed_end(w); moved = 1; is_move = 1; }
        else if (k == KEY_PGUP) { for (int i = 0; i < ed_vis_rows(w); i++) ed_move_vert(w, -1); moved = 1; is_move = 1; }
        else if (k == KEY_PGDN) { for (int i = 0; i < ed_vis_rows(w); i++) ed_move_vert(w, 1); moved = 1; is_move = 1; }
        else if (k == KEY_BACKSPACE) {
            int s, e;
            ed_sel_range(w, &s, &e);
            if (e > s) ed_delete_range(w, s, e);
            else ed_backspace(w);
            moved = 1;
        }
        else if (k == KEY_DEL) {
            int s, e;
            ed_sel_range(w, &s, &e);
            if (e > s) ed_delete_range(w, s, e);
            else ed_delete(w);
            moved = 1;
        }
        else if (k == KEY_ENTER) {
            int s, e;
            ed_sel_range(w, &s, &e);
            if (e > s) ed_delete_range(w, s, e);
            ed_insert(w, '\n');
            moved = 1;
        }
        else if (k == KEY_TAB) { ed_indent(w, key_shift); moved = 1; }
        else if (k == KEY_F3) { if (find_len) ed_find_next(w, w->cur); moved = 1; }
        else if (k >= 32 && k < 127) {
            int s, e;
            ed_sel_range(w, &s, &e);
            if (e > s) ed_delete_range(w, s, e);
            ed_insert(w, (char)k);
            moved = 1;
        }
        if (is_move && !key_shift) w->anchor = w->cur;
        if (moved) redraw_ed_fast(w);
    }
}

static void handle_click(void)
{
    /* 任务栏 */
    u32 h = gfx_height();
    if (cur_y >= (int)h - 40) {
        for (int k = 0; k < nz; k++) {
            if (in_rect(cur_x, cur_y, 70 + k * 176, (int)h - 34, 168, 28)) {
                raise_win(zorder[k]);
                redraw();
                return;
            }
        }
        return;
    }

    int wi = win_at(cur_x, cur_y);
    if (wi < 0) {
        /* 桌面图标 */
        if (in_rect(cur_x, cur_y, ICON_X - 8, ICON_Y - 8, ICON_W + 16, ICON_H + 54)) {
            if (win_open_fm() >= 0) { redraw(); DBG(0x31); }
        }
        return;
    }

    uwnd *w = &wins[wi];
    int was_focus = (wfocus == wi);
    raise_win(wi);
    if (!was_focus) { redraw(); }

    int tx = w->x, ty = w->y, tw = w->w, th = w->h;

    /* 关闭按钮 */
    if (in_rect(cur_x, cur_y, tx + tw - 36, ty + 9, 26, 26)) {
        if (w->type == WT_ED) { close_win(wi); redraw(); }
        else { close_win(wi); redraw(); }
        DBG(0x39);
        return;
    }

    /* 标题栏拖动 */
    if (in_rect(cur_x, cur_y, tx, ty, tw - 40, 42)) {
        drag_win = wi;
        DBG(0x3F);
        drag_dx = cur_x - w->x;
        drag_dy = cur_y - w->y;
        return;
    }

    if (w->type == WT_FM) {
        int bty = ty + 52;
        if (in_rect(cur_x, cur_y, tx + 10, bty, 62, 26)) { dlg_open_new(wi); return; }
        if (in_rect(cur_x, cur_y, tx + 78, bty, 62, 26)) { fm_delete_selected(w); return; }
        if (in_rect(cur_x, cur_y, tx + 146, bty, 62, 26)) { fm_up(w); return; }

        int list_y = ty + 90;
        int list_w = tw * 58 / 100;
        fs_set_dir((u32)w->dir);
        int n = fs_count();
        int rows = (th - 90 - 32 - 4) / 30;
        if (rows < 1) rows = 1;
        for (int i = 0; i < rows; i++) {
            int idx = w->lscroll + i;
            if (idx >= n) break;
            int ry = list_y + 2 + i * 30;
            if (in_rect(cur_x, cur_y, tx + 10, ry, list_w - 20, 28)) {
                if (w->sel == idx) {
                    fm_open_selected(w);
                } else {
                    w->sel = idx;
                    fm_msg = NULL;
                    redraw();
                    DBG(0x33);
                }
                return;
            }
        }
        return;
    }

    /* 编辑器：点文本框之外什么也不做 */
}

/* P12：锁屏（Ctrl+L 进入，输入当前用户密码解锁） */
static void ui_lock_screen(void)
{
    char pw[24];
    int len = 0;
    int err = 0;
    pw[0] = 0;

    serial_puts("[kernel] lock: locked\n");
    DBG(0x50);

    for (;;) {
        u32 w = gfx_width(), h = gfx_height();
        gfx_gradient(0, 0, (int)w, (int)h, 0x000A1428u, 0x00183050u);
        gfx_text_center((int)w / 2, (int)h / 2 - 120, "UMDos locked", C_WHITE, 4);
        gfx_text_center((int)w / 2, (int)h / 2 - 50, "password to unlock", 0x00B0C8E0u, 1);
        int bw = 420, bh = 46;
        int bx = (int)w / 2 - bw / 2, by = (int)h / 2;
        gfx_round_rect(bx, by, bw, bh, 0x00102030u, 0x00507090u);
        char mask[25];
        for (int i = 0; i < len; i++) mask[i] = '*';
        mask[len] = 0;
        gfx_text(bx + 14, by + 14, mask, C_WHITE, 2);
        if (err)
            gfx_text_center((int)w / 2, by + 68, "wrong password", 0x00FF9090u, 2);
        gfx_text_center((int)w / 2, by + 120, "Enter = unlock", 0x0090A8B8u, 1);

        input_event ev;
        while (!input_poll(&ev))
            ;
        if (ev.type != EV_KEY) continue;
        if (ev.key == KEY_BACKSPACE) {
            if (len) pw[--len] = 0;
        } else if (ev.key == KEY_ENTER) {
            if (auth_check(auth_name(), pw) == 0) {
                serial_puts("[kernel] lock: unlocked\n");
                vfs_set_role(auth_role());
                DBG(0x51);
                return;
            }
            err = 1;
            len = 0;
            pw[0] = 0;
            serial_puts("[kernel] lock: wrong password\n");
        } else if (ev.key >= 32 && ev.key < 127 && len < 23) {
            pw[len++] = (char)ev.key;
            pw[len] = 0;
            err = 0;
        }
    }
}

void ui_desktop(const bootinfo_t *bi)
{
    g_bi = bi;
    nz = 0;
    wfocus = -1;
    dlg_open = 0;
    btn_prev = 0;
    fm_msg = NULL;
    for (int i = 0; i < MAX_WIN; i++) wins[i].open = 0;
    cur_x = (int)gfx_width() / 2;
    cur_y = (int)gfx_height() / 2;

    draw_desktop_bg();
    cursor_draw();
    DBG(0x30);

    for (;;) {
        input_event ev;
        if (!input_poll(&ev))
            continue;

        if (ev.type == EV_KEY) {
            key_shift = ev.shift;
            key_ctrl = ev.ctrl;
            if (ev.ctrl && (ev.key == 'l' || ev.key == 'L')) {
                ui_lock_screen();
                redraw();
                cursor_draw();
                continue;
            }
            handle_key(ev.key);
            continue;
        }

        cursor_move(ev.dx, ev.dy);
        int pressed = (ev.buttons & 1) && !(btn_prev & 1);
        int released = !(ev.buttons & 1) && (btn_prev & 1);
        btn_prev = ev.buttons;

        if ((ev.buttons & 1) && drag_win >= 0 && !pressed) {
            uwnd *w = &wins[drag_win];
            DBG(0x3D);
            cursor_erase();
            w->x = cur_x - drag_dx;
            w->y = cur_y - drag_dy;
            if (w->x < -w->w + 80) w->x = -w->w + 80;
            if (w->y < 46) w->y = 46;
            if (w->x > (int)gfx_width() - 80) w->x = (int)gfx_width() - 80;
            if (w->y > (int)gfx_height() - 100) w->y = (int)gfx_height() - 100;
            redraw();
            continue;
        }
        if (released) { drag_win = -1; DBG(0x3C); continue; }
        if (!pressed) continue;

        handle_click();
    }
}
