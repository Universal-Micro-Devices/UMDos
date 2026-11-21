/* ui.c - 启动画面 / 登录 / 桌面 / 我的电脑（文件管理器） */
#include "ui.h"
#include "gfx.h"
#include "umdlogo.h"
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
#include "acpi.h"

#define FS_ADDR   0x80000u

/* forward declarations (UI only) */
static void redraw(void);
static void ui_lock_screen(void);
static int  win_open_ed(int dir, const fs_entry_t *e);

static void open_readme(void);
#define DBG(c)    outb(0xE9, (u8)(c))

/* ---------------- UI strings (ASCII only) ---------------- */
#define T_MYCOMPUTER  "This PC"
#define T_TERMINAL    "Terminal"
#define T_RECYCLE     "Recycle Bin"
#define T_LOGIN       "Login"
#define T_USER        "User name"
#define T_PASS        "Password"
#define T_FILES       "File Manager"
#define T_NAME        "Name"
#define T_SIZE        "Size"
#define T_CLOSE       "Close"
#define T_TOTAL       "Total"
#define T_PIECES      "items"
#define T_SYSTEM      "System"
#define T_NEW         "New"
#define T_DEL         "Delete"
#define T_EDIT        "Edit"
#define T_RENAME      "Rename"
#define T_UP          "Up"


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

static void fill_ellipse(int cx, int cy, int rx, int ry, u32 color)
{
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) * ry * ry + y * y * rx * rx <= rx * rx * ry * ry) w++;
        gfx_hline(cx - w, cy + y, 2 * w + 1, color);
    }
}

static void fill_circle(int cx, int cy, int r, u32 color)
{
    fill_ellipse(cx, cy, r, r, color);
}

/* 官方 UMDos logo（2bpp 位图，最近邻缩放到 size×size） */
static void draw_umd_logo_size(int x, int y, int size)
{
    for (int dy = 0; dy < size; dy++) {
        int gy = (dy * UMD_LOGO_H) / size;
        const u8 *row = &umd_logo[gy * UMD_LOGO_STRIDE];
        int dx = 0;
        while (dx < size) {
            int gx = (dx * UMD_LOGO_W) / size;
            int idx = (row[gx >> 2] >> ((3 - (gx & 3)) * 2)) & 3;
            int run = 1;
            while (dx + run < size) {
                int gx2 = ((dx + run) * UMD_LOGO_W) / size;
                int j = (row[gx2 >> 2] >> ((3 - (gx2 & 3)) * 2)) & 3;
                if (j != idx) break;
                run++;
            }
            u32 col = idx == 1 ? 0x0063AAFFu : (idx == 2 ? 0x00191029u : 0u);
            if (col) gfx_rect(x + dx, y + dy, run, 1, col);
            dx += run;
        }
    }
}

static void draw_umd_logo(int x, int y, int scale)
{
    draw_umd_logo_size(x, y, UMD_LOGO_W * scale);
}

/* Windows 2000 风格的启动画面：白底 + 官方 logo + 产品字 + 底部蓝条 */
static void splash_static(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_gradient(0, 0, (int)w, (int)h, 0x00F8F9FBu, 0x00E4E9F1u);

    int cx = (int)w / 2;
    int lx = cx - 210, ly = 120;
    /* 官方 logo */
    draw_umd_logo(lx, ly, 4);

    /* 产品名 */
    gfx_text(lx + 154, ly + 4, "UMDos", 0x00191029u, 7);
    gfx_text(lx + 156, ly + 104, "2.6 Professional", 0x00323A46u, 3);
    gfx_text(lx + 158, ly + 152, "Built on UMDos Technology", 0x00707880u, 1);

    /* 底部蓝条 */
    int bh = 42, by = (int)h - bh;
    gfx_gradient(0, by, (int)w, bh, 0x000E4FA0u, 0x000A3A78u);
    gfx_hline(0, by, (int)w, 0x004A8AE0u);
    gfx_text(24, by + 13, "Starting up...", 0x00FFFFFFu, 1);
    DBG(0x10);
}

/* 进度区：底部蓝条上方的步骤文字 + 进度条（只重画这一块） */
static void splash_panel(u32 el, u32 total, int step)
{
    u32 w = gfx_width(), h = gfx_height();
    const char *msgs[4] = {
        "Reading partition table ...",
        "Loading kernel image ...",
        "Mounting UMDFS filesystem ...",
        "Starting user session ...",
    };
    int bx = 24, by = (int)h - 104, bw = (int)w - 48;
    gfx_text(bx, by, msgs[step > 3 ? 3 : step], 0x00505A66u, 1);

    u32 pct = (u32)step * 25u;
    if (total) pct += (el * 25u) / total;
    if (pct > 100) pct = 100;

    gfx_bevel_down(bx, by + 24, bw, 16, 0x00FFFFFFu);
    if (pct > 0) gfx_rect(bx + 2, by + 26, (int)((bw - 4) * pct / 100u), 12, 0x002F7CD6u);

    char buf[8];
    itou(pct, buf, 10, 0);
    int tx = (int)w - 24 - ((int)strlen(buf) + 1) * 8;
    gfx_text(tx, by, buf, 0x00505A66u, 1);
    gfx_text(tx + (int)strlen(buf) * 8, by, "%", 0x00505A66u, 1);
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
    gfx_bevel_down(x, y, w, h, C_WHITE);          /* classic sunken entry */
    if (active) gfx_frame(x + 1, y + 1, w - 2, h - 2, C_ACCENT);
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
    gfx_gradient(0, 0, (int)w, (int)h, 0x000B1730u, 0x00182C4Eu);

    int cx = (int)w / 2;
    int ax = cx - 250, ay = (int)h / 2 - 120;

    /* 头像卡片（深灰底 + 白色人像） */
    gfx_rect(ax, ay, 140, 140, 0x00383E48u);
    fill_circle(ax + 70, ay + 50, 28, 0x00F2F4F7u);
    /* 肩部：椭圆只画在卡片内的上半部分 */
    for (int y = ay + 112; y < ay + 140; y++) {
        int dy = y - (ay + 150);
        int half = 0;
        while ((half + 1) * (half + 1) * 44 * 44 + dy * dy * 52 * 52 <= 52 * 52 * 44 * 44) half++;
        gfx_hline(ax + 70 - half, y, 2 * half + 1, 0x00F2F4F7u);
    }
    gfx_frame(ax, ay, 140, 140, 0x00606A78u);

    /* 输入区 */
    int fx = cx + 20, fw = 300;
    int fy = (int)h / 2 - 70;
    gfx_text(fx, fy - 26, "User name", 0x00C8D2E0u, 1);
    gfx_rect(fx, fy, fw, 34, 0x00FFFFFFu);
    gfx_frame(fx, fy, fw, 34, focus == 0 ? 0x002F7CD6u : 0x00A8B4C4u);
    for (int i = 0; i < user_len; i++)
        gfx_char(fx + 10 + i * 8, fy + 9, user_buf[i], 0x00101820u, 1);
    if (focus == 0 && ((flash_tick >> 3) & 1) == 0)
        gfx_rect(fx + 10 + user_len * 8, fy + 7, 2, 20, 0x002F7CD6u);

    gfx_text(fx, fy + 70, "Password", 0x00C8D2E0u, 1);
    gfx_rect(fx, fy + 94, fw, 34, 0x00FFFFFFu);
    gfx_frame(fx, fy + 94, fw, 34, focus == 1 ? 0x002F7CD6u : 0x00A8B4C4u);
    for (int i = 0; i < pass_len; i++)
        gfx_char(fx + 10 + i * 8, fy + 103, '*', 0x00101820u, 1);
    if (focus == 1 && ((flash_tick >> 3) & 1) == 0)
        gfx_rect(fx + 10 + pass_len * 8, fy + 101, 2, 20, 0x002F7CD6u);

    /* 蓝色箭头登录按钮 */
    int bx = fx + fw + 6, by = fy + 94;
    gfx_rect(bx, by, 34, 34, 0x002F7CD6u);
    gfx_frame(bx, by, 34, 34, 0x001E5FA8u);
    gfx_hline(bx + 8, by + 16, 16, 0x00FFFFFFu);
    for (int i = 0; i < 7; i++) {
        gfx_hline(bx + 17 + i, by + 16 - i, 2, 0x00FFFFFFu);
        gfx_hline(bx + 17 + i, by + 16 + i, 2, 0x00FFFFFFu);
    }

    if (login_msg)
        gfx_text(fx, fy + 146, login_msg, 0x00FF8A80u, 1);
    else
        gfx_text(fx, fy + 146, "Enter = sign in   Tab = switch field", 0x008E9AACu, 1);

    /* 底部标识与电源图标 */
    int bl = (int)w / 2 - 62;
    draw_umd_logo(bl, (int)h - 46, 1);
    gfx_text(bl + 36, (int)h - 32, "UMDos 2.6", 0x00A8B4C8u, 1);
    fill_circle(36, (int)h - 42, 15, 0x00D8DEE8u);
    fill_circle(36, (int)h - 42, 11, 0x000F1F38u);
    gfx_vline(36, (int)h - 52, 12, 0x00D8DEE8u);
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
                int fx = (int)gfx_width() / 2 + 20, fy = (int)gfx_height() / 2 - 70;
                int fw = 300;
                if (cur_x >= fx && cur_x < fx + fw) {
                    if (cur_y >= fy && cur_y < fy + 34) focus = 0;
                    else if (cur_y >= fy + 94 && cur_y < fy + 128) focus = 1;
                }
                if (cur_x >= fx + fw + 6 && cur_x < fx + fw + 40 &&
                    cur_y >= fy + 94 && cur_y < fy + 128) {
                    if (check_login()) return 1;
                    login_msg = "Wrong username or password";
                }
            }
            cursor_erase();
            draw_login();
            cursor_draw();
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

#define ICON_X        40
#define ICON_Y        14
#define ICON_W        32
#define ICON_H        32
#define ICON_W   96
#define ICON_H   96

/* ---------------- 通用绘制 ---------------- */
/* ---------------- flat desktop icons (Win10 style, no gradients/shadows) ---------------- */
static int start_open, start_sub;

/* ================= classic Windows-2000 / ReactOS look ================= */

/* 32x32 flat-classic icons (rect/lines only) */
static void icon_pc(int x, int y)
{
    gfx_rect(x + 2, y + 4, 28, 20, 0x00C0C0C0u);
    gfx_frame(x + 2, y + 4, 28, 20, 0x00000000u);
    gfx_rect(x + 5, y + 7, 22, 13, 0x00008080u);
    gfx_frame(x + 5, y + 7, 22, 13, 0x00404040u);
    gfx_rect(x + 8, y + 25, 16, 4, 0x00C0C0C0u);
    gfx_frame(x + 8, y + 25, 16, 4, 0x00000000u);
    gfx_rect(x + 5, y + 29, 22, 2, 0x00808080u);
}

static void icon_cmd(int x, int y)
{
    gfx_rect(x + 2, y + 4, 28, 24, 0x00000000u);
    gfx_frame(x + 3, y + 5, 26, 22, 0x00FFFFFFu);
    gfx_rect(x + 4, y + 6, 24, 4, 0x00C0C0C0u);
    gfx_text(x + 5, y + 12, "C:\\>", 0x00FFFFFFu, 1);
    gfx_rect(x + 5, y + 22, 6, 2, 0x00FFFFFFu);
}

static void icon_bin(int x, int y)
{
    gfx_rect(x + 8, y + 6, 16, 3, 0x00C0C0C0u);
    gfx_frame(x + 8, y + 6, 16, 3, 0x00000000u);
    gfx_rect(x + 6, y + 9, 20, 20, 0x00C0C0C0u);
    gfx_frame(x + 6, y + 9, 20, 20, 0x00000000u);
    for (int i = 0; i < 3; i++)
        gfx_rect(x + 11 + i * 5, y + 12, 1, 14, 0x00808080u);
    gfx_rect(x + 13, y + 3, 6, 2, 0x00000000u);
}

static void icon_label(int cx, int y, const char *s)
{
    int x = cx - gfx_text_width(s, 1) / 2;
    gfx_text(x + 1, y + 1, s, 0x00000000u, 1);
    gfx_text(x, y, s, 0x00FFFFFFu, 1);
}

static void draw_file_icon(int x, int y)
{
    gfx_rect(x + 1, y, 12, 16, 0x00FFFFFFu);
    gfx_frame(x + 1, y, 12, 16, 0x00000000u);
    gfx_hline(x + 3, y + 5, 7, 0x00808080u);
    gfx_hline(x + 3, y + 8, 7, 0x00808080u);
    gfx_hline(x + 3, y + 11, 5, 0x00808080u);
}

static void draw_dir_icon(int x, int y)
{
    gfx_rect(x + 1, y + 4, 15, 12, 0x00F0D080u);
    gfx_frame(x + 1, y + 4, 15, 12, 0x00000000u);
    gfx_rect(x + 1, y, 7, 4, 0x00F0D080u);
    gfx_frame(x + 1, y, 7, 4, 0x00000000u);
}

/* classic taskbar: Start + task buttons + clock tray */
static void draw_taskbar(void)
{
    u32 w = gfx_width(), h = gfx_height();
    int y = (int)h - 30;
    gfx_rect(0, y, (int)w, 30, C_PANEL);
    gfx_hline(0, y, (int)w, C_WHITE);

    int sopen = start_open;
    gfx_bevel_down(3, y + 3, 56, 24, C_PANEL);          /* Start pressed look when open */
    if (!sopen) gfx_bevel_up(3, y + 3, 56, 24, C_PANEL);
    draw_umd_logo_size(7, y + 6, 18);
    gfx_text(30, y + 9, "Start", C_TEXT, 1);

    int bx = 70;
    for (int k = 0; k < nz; k++) {
        uwnd *w2 = &wins[zorder[k]];
        int hot = (wfocus == zorder[k]);
        gfx_bevel_down(bx, y + 3, 150, 24, hot ? 0x00D0D0D0u : C_PANEL);
        const char *t = (w2->type == WT_FM) ? "File Manager" : w2->name;
        gfx_text(bx + 6, y + 9, t, C_TEXT, 1);
        bx += 156;
    }

    /* tray clock */
    gfx_bevel_down((int)w - 78, y + 3, 74, 24, C_PANEL);
    u32 hh, mm, ss;
    rtc_hms(&hh, &mm, &ss);
    char t[8];
    char *p = itou(hh, t, 10, 2);
    *p++ = ':';
    p = itou(mm, p, 10, 2);
    *p = 0;
    gfx_text((int)w - 66, y + 9, t, C_TEXT, 1);
}

/* classic Start menu with Programs fly-out */
static void draw_start_menu(void)
{
    int w = gfx_width(), h = gfx_height();
    int mw = 176, mh = 8 + 7 * 22 + 6 + 2 * 22;
    int mx = 2, my = (int)h - 30 - mh;
    gfx_bevel_up(mx, my, mw, mh, C_PANEL);
    gfx_hline(mx + 2, my + 2, mw - 4, C_WHITE);

    static const char *items[7] = {
        "Programs", "Documents", "Settings", "Find", "Help", "Run...", "Shut Down..."
    };
    for (int i = 0; i < 7; i++) {
        int ry = my + 4 + i * 22;
        int dim = (i == 1 || i == 2 || i == 3);
        if (i == 0 && start_sub == 1)
            gfx_rect(mx + 2, ry, mw - 4, 20, C_SELECT);
        gfx_text(mx + 20, ry + 5, items[i], dim ? 0x00808080u : (i == 0 && start_sub == 1 ? C_SELECT_TEXT : C_TEXT), 1);
        if (i == 0) {
            int ax = mx + mw - 13, ay = ry + 7;
            for (int k = 0; k < 5; k++) gfx_vline(ax + k, ay + k, 9 - 2 * k, C_TEXT);
        }
    }
    gfx_hline(mx + 4, my + 4 + 7 * 22 + 2, mw - 8, C_PANEL_DARK);
    gfx_text(mx + 20, my + 4 + 7 * 22 + 6, "Log Off...", C_TEXT, 1);

    if (start_sub == 1) {
        int sx = mx + mw, sy = my + 4;
        int items2 = 5;
        gfx_bevel_up(sx, sy, 168, 8 + items2 * 22, C_PANEL);
        static const char *apps[5] = { "File Manager", "Command Prompt", "Text Editor", "-", "About UMDos" };
        for (int i = 0; i < items2; i++) {
            int ry = sy + 4 + i * 22;
            if (apps[i][0] == '-') {
                gfx_hline(sx + 6, ry + 10, 156, C_PANEL_DARK);
                continue;
            }
            gfx_text(sx + 10, ry + 5, apps[i], C_TEXT, 1);
        }
    }
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
    w->w = 620;
    w->h = 430;
    w->x = 90 + (nz * 30) % 120;
    w->y = 50 + (nz * 24) % 80;
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
    w->w = 640;
    w->h = 440;
    w->x = 80 + (nz * 30) % 100;
    w->y = 60 + (nz * 24) % 70;
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
static void draw_wnd_frame(int x, int y, int w, int h, const char *title, int active)
{
    gfx_bevel_up(x, y, w, h, C_PANEL);
    if (active) gfx_gradient(x + 2, y + 2, w - 4, 22, C_TITLE, C_TITLE_2);
    else gfx_rect(x + 2, y + 2, w - 4, 22, 0x00808080u);
    gfx_text(x + 6, y + 7, title, C_WHITE, 1);

    int bx = x + w - 3 * 18 - 4;
    for (int i = 0; i < 3; i++)
        gfx_bevel_up(bx + i * 18, y + 3, 16, 16, C_PANEL);
    gfx_hline(bx + 4, y + 12, 8, 0x00000000u);                  /* minimize */
    gfx_frame(bx + 18 + 4, y + 8, 8, 7, 0x00000000u);           /* maximize */
    gfx_text(bx + 36 + 4, y + 6, "X", 0x00000000u, 1);          /* close */

    gfx_text(x + 6, y + 27, "File", C_TEXT, 1);
    gfx_text(x + 44, y + 27, "Edit", C_TEXT, 1);
    gfx_text(x + 84, y + 27, "View", C_TEXT, 1);
    gfx_text(x + 124, y + 27, "Help", C_TEXT, 1);
    gfx_hline(x + 2, y + 42, w - 4, C_PANEL_DARK);
}

static void draw_fm(uwnd *w)
{
    int idx = win_index(w);
    char path[80];
    fs_set_dir((u32)w->dir);
    fs_path(path, sizeof(path));

    int tx = w->x, ty = w->y, tw = w->w, th = w->h;
    char tbuf[96];
    int ti = 0;
    const char *pre = "File Manager - ";
    for (int k = 0; pre[k] && ti < 90; k++) tbuf[ti++] = pre[k];
    for (int k = 0; path[k] && ti < 95; k++) tbuf[ti++] = path[k];
    tbuf[ti] = 0;
    draw_wnd_frame(tx, ty, tw, th, tbuf, wfocus == idx);

    /* 工具栏 */
    int bty = ty + 52;
    gfx_round_rect(tx + 10, bty, 62, 26, C_DESKTOP_1, C_PANEL_DARK);
    gfx_text(tx + 14, bty + 5, T_NEW, C_TEXT, 1);
    gfx_round_rect(tx + 78, bty, 62, 26, C_DESKTOP_1, C_PANEL_DARK);
    gfx_text(tx + 82, bty + 5, T_DEL, C_TEXT, 1);
    gfx_round_rect(tx + 146, bty, 62, 26, C_DESKTOP_1, C_PANEL_DARK);
    gfx_text(tx + 156, bty + 5, T_UP, C_TEXT, 1);

    char tmp[16];
    itou((u32)fs_count(), tmp, 10, 0);
    {
        char count[24];
        int n = 0;
        for (int k = 0; tmp[k] && n < 20; k++) count[n++] = tmp[k];
        const char *w2 = " items";
        for (int k = 0; w2[k] && n < 23; k++) count[n++] = w2[k];
        count[n] = 0;
        gfx_text(tx + 222, bty + 8, count, C_TEXT_DIM, 1);
    }
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
        gfx_text(tx + 42, ry + 6, e->name, (idx == w->sel) ? C_SELECT_TEXT : C_TEXT, 1);
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
    {
        char st[24];
        int n = 0;
        for (int k = 0; tmp[k] && n < 20; k++) st[n++] = tmp[k];
        const char *w2 = " items";
        for (int k = 0; w2[k] && n < 23; k++) st[n++] = w2[k];
        st[n] = 0;
        gfx_text(tx + 10, sy, st, C_TEXT, 1);
    }
    itou((u32)n, tmp, 10, 0);
    gfx_text(tx + 32, sy, tmp, C_TEXT, 1);

    char info[40];
    char num[16];
    {
        /* disk free space in MB / GB (no block counts) */
        u64 free_bytes = (u64)fs_free_blocks() * (u64)fs_block_size();
        u64 total_bytes = (u64)fs_block_count() * (u64)fs_block_size();
        char *q = info;
        if (total_bytes >= 1024ull * 1024ull * 1024ull) {
            u64 gb10 = (free_bytes * 10ull) / (1024ull * 1024ull * 1024ull);
            itou(gb10 / 10ull, num, 10, 0);
            for (int k = 0; num[k]; k++) *q++ = num[k];
            *q++ = '.';
            *q++ = (char)('0' + (char)(gb10 % 10ull));
            const char *un = " GB free";
            while (*un) *q++ = *un++;
        } else {
            u64 mb100 = (free_bytes * 100ull) / (1024ull * 1024ull);
            itou(mb100 / 100ull, num, 10, 0);
            for (int k = 0; num[k]; k++) *q++ = num[k];
            *q++ = '.';
            *q++ = (char)('0' + (char)((mb100 / 10ull) % 10ull));
            *q++ = (char)('0' + (char)(mb100 % 10ull));
            const char *un = " MB free";
            while (*un) *q++ = *un++;
        }
        *q = 0;
    }
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
    char tbuf[64];
    int ti = 0;
    for (int k = 0; w->name[k] && ti < 60; k++) tbuf[ti++] = w->name[k];
    tbuf[ti] = 0;
    draw_wnd_frame(tx, ty, tw, th, tbuf, wfocus == idx);
    gfx_text(tx + 12, ty + 12, T_EDIT, C_WHITE, 2);
    gfx_text(tx + 12 + 64 + 10, ty + 18, "-", C_TEXT, 1);
    gfx_text(tx + 12 + 64 + 26, ty + 18, w->name, C_TEXT, 1);
    int cbx = tx + tw - 36, cby = ty + 9;
    gfx_round_rect(cbx, cby, 26, 26, C_PANEL, C_PANEL_DARK);
    gfx_text(cbx + 8, cby + 3, "X", C_TEXT, 2);

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

static void draw_desktop_bg(void)
{
    u32 w = gfx_width(), h = gfx_height();
    gfx_gradient(0, 0, (int)w, (int)h - 30, 0x00108787u, 0x000A6464u);

    icon_pc(ICON_X, ICON_Y);
    icon_label(ICON_X + 16, ICON_Y + 36, "My");
    icon_label(ICON_X + 16, ICON_Y + 48, "Computer");
    icon_cmd(ICON_X, ICON_Y + 70);
    icon_label(ICON_X + 16, ICON_Y + 106, "Command");
    icon_label(ICON_X + 16, ICON_Y + 118, "Prompt");
    icon_bin(ICON_X, ICON_Y + 140);
    icon_label(ICON_X + 16, ICON_Y + 176, "Recycle Bin");

    draw_taskbar();
    if (start_open) draw_start_menu();
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
    gfx_round_rect(px, py, 440, 170, C_PANEL, C_PANEL_DARK);
    gfx_rect(px + 2, py + 2, 436, 38, C_TITLE);
    if (dlg_mode == 1) {
        gfx_text(px + 14, py + 8, T_RENAME, C_WHITE, 2);
        gfx_text(px + 120, py + 14, "rename to", C_TEXT, 1);
    } else {
        gfx_text(px + 14, py + 8, T_NEW, C_WHITE, 2);
        gfx_text(px + 84, py + 14, "name (file or folder)", C_TEXT, 1);
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

/* ---------------- classic menu bar popups ---------------- */
enum { MA_NONE, MA_NEW, MA_CLOSE, MA_RENAME, MA_DEL, MA_UP, MA_SAVE,
       MA_COPY, MA_CUT, MA_PASTE, MA_SELALL, MA_ABOUT };

typedef struct { const char *name; int enabled; int action; } mitem_t;

static int menu_win = -1;
static int menu_idx = -1;

static int menu_title_off(int i)
{
    return (i == 0) ? 6 : (i == 1) ? 44 : (i == 2) ? 84 : 124;
}

static int menu_items(int type, int idx, mitem_t *out)
{
    int n = 0;
    if (type == WT_FM) {
        if (idx == 0) { out[n++] = (mitem_t){"New Folder", 1, MA_NEW}; out[n++] = (mitem_t){"Close", 1, MA_CLOSE}; }
        else if (idx == 1) { out[n++] = (mitem_t){"Rename", 1, MA_RENAME}; out[n++] = (mitem_t){"Delete", 1, MA_DEL}; }
        else if (idx == 2) { out[n++] = (mitem_t){"Up", 1, MA_UP}; }
        else { out[n++] = (mitem_t){"About UMDos", 1, MA_ABOUT}; }
    } else {
        if (idx == 0) { out[n++] = (mitem_t){"Save", 1, MA_SAVE}; out[n++] = (mitem_t){"Close", 1, MA_CLOSE}; }
        else if (idx == 1) {
            out[n++] = (mitem_t){"Copy", 1, MA_COPY};
            out[n++] = (mitem_t){"Cut", 1, MA_CUT};
            out[n++] = (mitem_t){"Paste", 1, MA_PASTE};
            out[n++] = (mitem_t){"Select All", 1, MA_SELALL};
        } else if (idx == 2) {
            out[n++] = (mitem_t){"Status Bar", 0, MA_NONE};
        } else {
            out[n++] = (mitem_t){"About UMDos", 0, MA_NONE};
        }
    }
    return n;
}

static void draw_menus(void)
{
    if (menu_win < 0) return;
    uwnd *w = &wins[menu_win];
    if (!w->open) { menu_win = -1; return; }
    mitem_t items[8];
    int n = menu_items(w->type, menu_idx, items);
    int px = w->x + menu_title_off(menu_idx);
    int py = w->y + 42;
    int pw = 150;
    gfx_bevel_up(px, py, pw, 8 + n * 20, C_PANEL);
    for (int i = 0; i < n; i++) {
        int ry = py + 4 + i * 20;
        gfx_text(px + 10, ry + 5, items[i].name, items[i].enabled ? C_TEXT : 0x00808080u, 1);
    }
}

static void open_readme(void)
{
    u32 saved = fs_dir();
    fs_set_dir(FS_ROOT_DIR);
    int i = fs_find("README.TXT");
    const fs_entry_t *e = (i >= 0) ? fs_at(i) : 0;
    if (e) {
        if (win_open_ed(FS_ROOT_DIR, e) >= 0) redraw();
    }
    fs_set_dir(saved);
}

static void redraw(void)
{
    cursor_erase();
    draw_desktop_bg();
    draw_windows();
    if (dlg_open) draw_dialog();
    draw_menus();
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
    if (k == KEY_ESC && key_ctrl) { start_open = !start_open; start_sub = 0; redraw(); return; }
    if (key_ctrl && (k == 'm' || k == 'M')) {
        if (wfocus >= 0 && wins[wfocus].open) { menu_win = wfocus; menu_idx = 0; redraw(); }
        return;
    }
    if (k == KEY_ESC && start_open) { start_open = start_sub = 0; redraw(); return; }
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
    u32 h = gfx_height();
    int mh = 8 + 7 * 22 + 6 + 2 * 22;

    /* Start menu first */
    if (start_open) {
        int mx = 2, my = (int)h - 30 - mh;
        if (start_sub == 1 && cur_x >= mx + 176 && cur_x < mx + 176 + 168 &&
            cur_y >= my + 4 && cur_y < my + 4 + 5 * 22) {
            int idx = (cur_y - (my + 4)) / 22;
            start_open = start_sub = 0;
            if (idx == 0) { win_open_fm(); redraw(); return; }
            if (idx == 1) { term_run(); redraw(); return; }
            if (idx == 2 || idx == 4) { open_readme(); return; }
            redraw();
            return;
        }
        if (cur_x >= mx && cur_x < mx + 176 && cur_y >= my && cur_y < my + mh) {
            int row = (cur_y - (my + 4)) / 22;
            if (row == 0) { start_sub = (start_sub == 1) ? 0 : 1; redraw(); return; }
            if (row == 4) { start_open = start_sub = 0; open_readme(); return; }
            if (row == 5) { start_open = start_sub = 0; term_run(); redraw(); return; }
            if (row == 6) { start_open = start_sub = 0; acpi_shutdown(); return; }
            if (row >= 7) { start_open = start_sub = 0; ui_lock_screen(); redraw(); return; }
            return;
        }
        start_open = start_sub = 0;
        redraw();
    }

    /* taskbar */
    if (cur_y >= (int)h - 30) {
        if (cur_x >= 3 && cur_x < 59) {
            start_open = !start_open;
            start_sub = 0;
            redraw();
            return;
        }
        for (int k = 0; k < nz; k++) {
            if (in_rect(cur_x, cur_y, 70 + k * 156, (int)h - 27, 150, 24)) {
                raise_win(zorder[k]);
                redraw();
                return;
            }
        }
        return;
    }

    /* open menu popup item */
    if (menu_win >= 0) {
        uwnd *mw = &wins[menu_win];
        mitem_t items[8];
        int n2 = menu_items(mw->type, menu_idx, items);
        int px = mw->x + menu_title_off(menu_idx);
        int py = mw->y + 42, pw = 150;
        if (cur_x >= px && cur_x < px + pw && cur_y >= py && cur_y < py + 8 + n2 * 20) {
            int idx = (cur_y - py - 4) / 20;
            if (idx >= 0 && idx < n2 && items[idx].enabled) {
                int act = items[idx].action;
                int mwi = menu_win;
                menu_win = -1;
                uwnd *mw2 = &wins[mwi];
                switch (act) {
                case MA_NEW: dlg_open_new(mwi); break;
                case MA_CLOSE: close_win(mwi); break;
                case MA_RENAME: fm_rename_open(mw2); break;
                case MA_DEL: fm_delete_selected(mw2); break;
                case MA_UP: fm_up(mw2); break;
                case MA_SAVE: ed_save(mw2); break;
                case MA_COPY: ed_copy(mw2); break;
                case MA_CUT: ed_cut(mw2); break;
                case MA_PASTE: ed_paste(mw2); break;
                case MA_SELALL: ed_select_all(mw2); break;
                case MA_ABOUT: fm_msg = "UMDos 64-bit"; break;
                default: break;
                }
                redraw();
                return;
            }
            return;
        }
        menu_win = -1;
        redraw();
    }

    int wi = win_at(cur_x, cur_y);
    if (wi < 0) {
        if (in_rect(cur_x, cur_y, ICON_X - 16, ICON_Y - 8, 72, 66)) {
            if (win_open_fm() >= 0) { redraw(); DBG(0x31); }
        } else if (in_rect(cur_x, cur_y, ICON_X - 16, ICON_Y + 56, 72, 72)) {
            term_run();
            redraw();
            DBG(0x32);
        }
        return;
    }

    uwnd *w = &wins[wi];
    int was_focus = (wfocus == wi);
    raise_win(wi);
    if (!was_focus) { redraw(); }

    int tx = w->x, ty = w->y, tw = w->w, th = w->h;

    /* menu bar titles */
    if (cur_y >= ty + 22 && cur_y < ty + 42) {
        for (int mi = 0; mi < 4; mi++) {
            int mx = tx + menu_title_off(mi);
            if (cur_x >= mx - 2 && cur_x < mx + 34) {
                menu_win = wi;
                menu_idx = mi;
                redraw();
                return;
            }
        }
    }

    /* close button (new title-bar buttons) */
    if (in_rect(cur_x, cur_y, tx + tw - 24, ty + 3, 18, 18)) {
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
    static const char *months[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    char pw[24];
    int len = 0;
    int err = 0;
    pw[0] = 0;

    serial_puts("[kernel] lock: locked\n");
    DBG(0x50);

    for (;;) {
        u32 w = gfx_width(), h = gfx_height();
        gfx_gradient(0, 0, (int)w, (int)h, 0x000A1322u, 0x00122038u);

        u32 hh, mm, ss;
        rtc_hms(&hh, &mm, &ss);
        char tb[8];
        char *p = itou(hh, tb, 10, 2);
        *p++ = ':';
        p = itou(mm, p, 10, 2);
        *p = 0;
        gfx_text(60, (int)h - 300, tb, 0x00FFFFFFu, 7);

        u32 day = bcd2bin(cmos_read(0x07));
        u32 mon = bcd2bin(cmos_read(0x08));
        if (mon < 1 || mon > 12) mon = 11;
        if (day < 1 || day > 31) day = 1;
        char db[40];
        char *q = db;
        const char *mn = months[mon - 1];
        while (*mn) *q++ = *mn++;
        *q++ = ' ';
        q = itou(day, q, 10, 0);
        *q = 0;
        gfx_text(64, (int)h - 170, db, 0x00B8C4D8u, 2);
        gfx_text(64, (int)h - 128, "UMDos 2.6  -  Locked", 0x008E9AACu, 2);

        gfx_text(40, 40, "Press Enter to unlock", 0x00A8B4C8u, 1);

        int bw = 320, bh = 36;
        int bx = 60, by = (int)h - 66;
        gfx_rect(bx, by, bw, bh, 0x001A2A44u);
        gfx_frame(bx, by, bw, bh, 0x00C8D2E0u);
        for (int i = 0; i < len; i++)
            gfx_char(bx + 12 + i * 8, by + 10, '*', 0x00FFFFFFu, 1);
        if (err)
            gfx_text(bx, by - 26, "Wrong password", 0x00FF8A80u, 1);
        else
            gfx_text(bx, by - 26, "Password", 0x008E9AACu, 1);

        fill_circle((int)w - 36, (int)h - 42, 15, 0x00D8DEE8u);
        fill_circle((int)w - 36, (int)h - 42, 11, 0x000F1F38u);
        gfx_vline((int)w - 36, (int)h - 52, 12, 0x00D8DEE8u);

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
