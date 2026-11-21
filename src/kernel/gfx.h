/* gfx.h - 帧缓冲绘图 */
#ifndef GFX_H
#define GFX_H

#include "types.h"

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

/* 常用配色（BGRX 内存序，值形如 0x00RRGGBB） */
#define C_BLACK       0x00000000u
#define C_WHITE       0x00FFFFFFu
#define C_DESKTOP_1   0x00C0C0C0u   /* classic desktop gray */
#define C_DESKTOP_2   0x00C0C0C0u
#define C_PANEL       0x00C0C0C0u   /* window / dialog face */
#define C_PANEL_DARK  0x00808080u   /* 3D shadow edge */
#define C_TITLE       0x00000080u   /* active title bar start */
#define C_TITLE_2     0x001084D0u   /* active title bar end */
#define C_ACCENT      0x000A246Au   /* selection / focus */
#define C_TEXT        0x00000000u
#define C_TEXT_DIM    0x00404040u
#define C_SELECT      0x000A246Au
#define C_SELECT_TEXT 0x00FFFFFFu
#define C_GREEN       0x00008000u
#define C_RED         0x00800000u
#define C_GOLD        0x00808000u

typedef struct {
    u32 base;
    u32 width, height, stride, bpp;
} gfx_info;

void gfx_init(const gfx_info *info);
u32  gfx_width(void);
u32  gfx_height(void);

void gfx_clear(u32 color);
void gfx_pixel(int x, int y, u32 color);
void gfx_rect(int x, int y, int w, int h, u32 color);
void gfx_frame(int x, int y, int w, int h, u32 color);
void gfx_gradient(int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_hline(int x, int y, int w, u32 color);
void gfx_round_rect(int x, int y, int w, int h, u32 fill, u32 border);

void gfx_char(int x, int y, char ch, u32 color, int scale);
void gfx_vline(int x, int y, int h, u32 color);
void gfx_bevel_up(int x, int y, int w, int h, u32 fill);
void gfx_bevel_down(int x, int y, int w, int h, u32 fill);
void gfx_text(int x, int y, const char *s, u32 color, int scale);
int  gfx_text_width(const char *s, int scale);
void gfx_text_center(int cx, int y, const char *s, u32 color, int scale);



#endif
