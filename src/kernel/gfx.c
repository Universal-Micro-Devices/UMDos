/* gfx.c - 帧缓冲绘图（1280x720x32 之类的线性帧缓冲） */
#include "gfx.h"
#include "font8x16.h"


static volatile u32 *fb;
static u32 scr_w, scr_h, scr_stride;

void gfx_init(const gfx_info *info)
{
    fb = (volatile u32 *)(u64)info->base;
    scr_w = info->width;
    scr_h = info->height;
    scr_stride = info->stride;
}

u32 gfx_width(void) { return scr_w; }
u32 gfx_height(void) { return scr_h; }

void gfx_pixel(int x, int y, u32 color)
{
    if (x < 0 || y < 0 || (u32)x >= scr_w || (u32)y >= scr_h)
        return;
    fb[(u64)y * scr_stride + (u32)x] = color;
}

static void block(int x, int y, int s, u32 color)
{
    for (int dy = 0; dy < s; dy++) {
        if (y + dy < 0 || (u32)(y + dy) >= scr_h) continue;
        volatile u32 *row = fb + (u64)(y + dy) * scr_stride;
        for (int dx = 0; dx < s; dx++) {
            if (x + dx < 0 || (u32)(x + dx) >= scr_w) continue;
            row[x + dx] = color;
        }
    }
}

void gfx_rect(int x, int y, int w, int h, u32 color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)scr_w) x1 = (int)scr_w;
    if (y1 > (int)scr_h) y1 = (int)scr_h;
    for (int yy = y0; yy < y1; yy++) {
        volatile u32 *row = fb + (u64)yy * scr_stride;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = color;
    }
}

void gfx_frame(int x, int y, int w, int h, u32 color)
{
    gfx_rect(x, y, w, 1, color);
    gfx_rect(x, y + h - 1, w, 1, color);
    gfx_rect(x, y, 1, h, color);
    gfx_rect(x + w - 1, y, 1, h, color);
}

void gfx_hline(int x, int y, int w, u32 color) { gfx_rect(x, y, w, 1, color); }

void gfx_clear(u32 color)
{
    gfx_rect(0, 0, (int)scr_w, (int)scr_h, color);
}

void gfx_gradient(int x, int y, int w, int h, u32 top, u32 bottom)
{
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int i = 0; i < h; i++) {
        int r = tr + (br - tr) * i / (h > 1 ? h - 1 : 1);
        int g = tg + (bg - tg) * i / (h > 1 ? h - 1 : 1);
        int b = tb + (bb - tb) * i / (h > 1 ? h - 1 : 1);
        gfx_hline(x, y + i, w, RGB(r, g, b));
    }
}

/* 经典 3D：方角 + 1px 白/灰边框（ReactOS/Win2000 观感）。
   gfx_round_rect 保留原签名，内部改为经典凸起边框，避免逐个改调用点。 */
void gfx_vline(int x, int y, int h, u32 color)
{
    gfx_rect(x, y, 1, h, color);
}

void gfx_bevel_up(int x, int y, int w, int h, u32 fill)
{
    gfx_rect(x, y, w, h, fill);
    gfx_hline(x, y, w, C_WHITE);
    gfx_vline(x, y, h, C_WHITE);
    gfx_hline(x, y + h - 1, w, C_PANEL_DARK);
    gfx_vline(x + w - 1, y, h, C_PANEL_DARK);
}

void gfx_bevel_down(int x, int y, int w, int h, u32 fill)
{
    gfx_rect(x, y, w, h, fill);
    gfx_hline(x, y, w, C_PANEL_DARK);
    gfx_vline(x, y, h, C_PANEL_DARK);
    gfx_hline(x, y + h - 1, w, C_WHITE);
    gfx_vline(x + w - 1, y, h, C_WHITE);
}

void gfx_round_rect(int x, int y, int w, int h, u32 fill, u32 border)
{
    gfx_bevel_up(x, y, w, h, fill);
    if (border != fill && w > 4 && h > 4)
        gfx_frame(x + 1, y + 1, w - 2, h - 2, border);
}

/* ---------------- 文本 ---------------- */

void gfx_char(int x, int y, char ch, u32 color, int scale)
{
    if (scale < 1) scale = 1;
    if (ch < 32 || ch > 126) ch = '?';
    const u8 *glyph = font8x16[(int)ch - 32];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80u >> col))
                block(x + col * scale, y + row * scale, scale, color);
        }
    }
}

void gfx_text(int x, int y, const char *s, u32 color, int scale)
{
    while (*s) {
        gfx_char(x, y, *s, color, scale);
        x += 8 * scale;
        s++;
    }
}

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    while (*s++) n++;
    return n * 8 * scale;
}

void gfx_text_center(int cx, int y, const char *s, u32 color, int scale)
{
    gfx_text(cx - gfx_text_width(s, scale) / 2, y, s, color, scale);
}

/* ---------------- 中文 ---------------- */





