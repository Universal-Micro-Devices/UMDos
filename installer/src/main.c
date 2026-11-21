/* main.c - UMDos setup wizard
 *
 * 流程：欢迎 -> 目标磁盘 -> 分区/格式化 -> Filesystem -> Install path
 *      -> 账户 -> Before you continue -> 写入 -> 完成
 *
 * Scope:
 *   - Legacy BIOS only
 *   - ATA (PIO) disks only
 *   - FAT32 real format; EXT32 marker only
 *   - Writes one MBR partition entry
 */
#include "bootinfo.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "ata.h"
#include "fat32.h"
#include "sha3.h"
#include "placeholder_icons.h"
#include "version.h"

#define DBG(c) outb(0xE9, (u8)(c))

#define MAXLOG 10

typedef struct {
    int page;                /* 0..7 */
    ata_dev disks[4];
    int ndisks, disk_idx;
    int mbr_on, format_on;
    int fs;                  /* 0=FAT32 1=EXT32(marker) */
    char path[48];
    int path_len;
    char uname[24];
    int ulen;
    char upass[24];
    int uplen;
    char apass[24];
    int aplen;
    int field;               /* account page: 0=user 1=user password 2=admin password */
    int agree_sel;           /* confirm page: 0=agree 1=cancel */
    int writing, done, failed;
    char log[MAXLOG][72];
    int nlog;
} inst_t;

static inst_t S;
static const gfx_info *GI;

/* ---------------- ASCII text helpers ---------------- */
static int mixed_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * 8 * scale;
}

static void mixed_text(int x, int y, const char *s, u32 color, int scale)
{
    while (*s) {
        gfx_char(x, y, *s++, color, scale);
        x += 8 * scale;
    }
}

static void mixed_center(int cx, int y, const char *s, u32 color, int scale)
{
    mixed_text(cx - mixed_width(s, scale) / 2, y, s, color, scale);
}

/* ---------------- controls ---------------- */
static void draw_icon(int x, int y, const u16 *rows, int scale, u32 color)
{
    for (int r = 0; r < 16; r++) {
        u16 bits = rows[r];
        for (int c = 0; c < 16; c++)
            if (bits & (0x8000 >> c))
                gfx_rect(x + c * scale, y + r * scale, scale, scale, color);
    }
}

static void panel(int x, int y, int w, int h, const char *title, const u16 *icon)
{
    gfx_round_rect(x, y, w, h, 0x00F4F7FBu, 0x00405878u);
    gfx_rect(x + 2, y + 2, w - 4, 40, 0x002A4A78u);
    mixed_text(x + 14, y + 12, title, C_WHITE, 2);
    if (icon)
        draw_icon(x + w - 32, y + 12, icon, 1, C_WHITE);
}

static void button(int x, int y, int w, int h, const char *label, int hot)
{
    gfx_round_rect(x, y, w, h, hot ? 0x004B8BD6u : 0x00D8E4F0u, hot ? C_ACCENT : 0x008098B0u);
    mixed_center(x + w / 2, y + (h - 16) / 2, label, hot ? C_WHITE : C_TEXT, 2);
}

static void row_item(int x, int y, int w, const char *text, int selected)
{
    if (selected)
        gfx_rect(x, y, w, 30, 0x00C8DCF0u);
    mixed_text(x + 10, y + 7, text, C_TEXT, 2);
}

/* ---------------- header / footer ---------------- */
static void draw_header(void)
{
    gfx_rect(0, 0, (int)gfx_width(), 56, 0x00102038u);
    gfx_rect(0, 56, (int)gfx_width(), 2, C_ACCENT);
    mixed_text(20, 14, "UMDos Setup", C_WHITE, 3);
    gfx_text((int)gfx_width() - 260, 20, INSTALLER_VERSION, 0x00A8C8E8u, 2);
}

static void draw_footer(const char *hint)
{
    u32 h = gfx_height();
    gfx_rect(0, (int)h - 56, (int)gfx_width(), 56, 0x00102038u);
    mixed_text(20, (int)h - 36, hint, 0x0090B0D0u, 2);
    char step[32];
    u32 shown = (u32)(S.page + 1);
    if (shown > 6) shown = 6;
    char *p = itou(shown, step, 10, 0);
    *p++ = '/';
    itou(6, p, 10, 0);
    gfx_text((int)gfx_width() - 100, (int)h - 36, step, 0x00A8C8E8u, 2);
}

/* ---------------- pages ---------------- */
static const char *fs_name(int i) { return i == 0 ? "FAT32" : "EXT32 (experimental)"; }

static void draw_welcome(void)
{
    int cx = (int)gfx_width() / 2;
    gfx_gradient(0, 0, (int)gfx_width(), (int)gfx_height(), 0x00102040u, 0x00285088u);
    mixed_center(cx, 180, "Welcome to UMDos", C_WHITE, 4);
    mixed_center(cx, 250, INSTALLER_VERSION"", 0x00A8C8E8u, 2);
    mixed_center(cx, 330, "This wizard guides you through disk, partition, filesystem", 0x00C0D4E8u, 2);
    mixed_center(cx, 372, "and account setup, then verifies the result.", 0x00C0D4E8u, 2);
    mixed_center(cx, 440, "Requires an ATA disk and Legacy BIOS boot.", 0x00F0C040u, 2);
    mixed_center(cx, 482, "", 0x00F0C040u, 2);
    button(cx - 120, 560, 240, 48, "Next", 1);
}

static void draw_disk(void)
{
    panel(60, 90, (int)gfx_width() - 120, 480, "Select target disk", ICON_DISK);
    if (S.ndisks == 0) {
        mixed_text(90, 160, "No ATA disk detected.", C_RED, 2);
        mixed_text(90, 200, "Attach a disk to the virtual machine and retry.", C_TEXT_DIM, 2);
    } else {
        for (int i = 0; i < S.ndisks; i++) {
            char line[96];
            char *p = line;
            const char *prefix = "Disk ";
            while (*prefix) *p++ = *prefix++;
            p = itou((u32)i, p, 10, 0);
            const char *mid = "  ·  ";
            while (*mid) *p++ = *mid++;
            const char *ch = S.disks[i].channel ? "secondary channel" : "primary channel";
            while (*ch) *p++ = *ch++;
            const char *ms = S.disks[i].master ? " master" : " slave";
            while (*ms) *p++ = *ms++;
            const char *sp = "  ·  ";
            while (*sp) *p++ = *sp++;
            p = itou((u32)(S.disks[i].sectors / 2048), p, 10, 0);
            const char *mb = " MB";
            while (*mb) *p++ = *mb++;
            *p = 0;
            row_item(90, 160 + i * 40, (int)gfx_width() - 180, line, i == S.disk_idx);
        }
        mixed_text(90, 500, "Up/Down to choose, Enter to continue", C_TEXT_DIM, 2);
    }
    draw_footer("ATA/AHCI disks are supported");
}

static void draw_part(void)
{
    panel(60, 90, (int)gfx_width() - 120, 420, "Partition and format", ICON_PARTITION);
    char line[96];
    char *p = line;
    const char *a = "Write MBR partition table: ";
    while (*a) *p++ = *a++;
    const char *v = S.mbr_on ? "Yes" : "No";
    while (*v) *p++ = *v++;
    *p = 0;
    row_item(90, 160, 700, line, 1);

    p = line;
    const char *b = "Format partition: ";
    while (*b) *p++ = *b++;
    v = S.format_on ? "Yes" : "No";
    while (*v) *p++ = *v++;
    *p = 0;
    row_item(90, 210, 700, line, 1);

    mixed_text(90, 290, "Mode: preset disk image + marker write.", C_TEXT_DIM, 2);
    mixed_text(90, 330, "Writes one MBR entry and a FAT32 boot sector.", C_TEXT_DIM, 2);
    mixed_text(90, 400, "Left/Right to toggle, Enter to continue", C_TEXT_DIM, 2);
    draw_footer("Partitioning will modify the target disk");
}

static void draw_fs(void)
{
    panel(60, 90, (int)gfx_width() - 120, 420, "Filesystem", ICON_FORMAT);
    for (int i = 0; i < 2; i++) {
        char line[64];
        char *p = line;
        p = itou((u32)(i + 1), p, 10, 0);
        *p++ = '.'; *p++ = ' ';
        const char *n = fs_name(i);
        while (*n) *p++ = *n++;
        *p = 0;
        row_item(90, 160 + i * 46, 640, line, i == S.fs);
    }
    mixed_text(90, 290, "FAT32: real format, writes /CONFIG/AUTH.", 0x0030A050u, 2);
    mixed_text(90, 334, "EXT32: marker only (not a real ext filesystem).", 0x00C07030u, 2);
    mixed_text(90, 400, "Up/Down to choose, Enter to continue", C_TEXT_DIM, 2);
    draw_footer("See README for compatibility");
}

static void draw_path(void)
{
    panel(60, 90, (int)gfx_width() - 120, 380, "Install path", ICON_PATH);
    char line[128];
    char *p = line;
    const char *a = "Target disk ";
    while (*a) *p++ = *a++;
    p = itou((u32)S.disk_idx, p, 10, 0);
    const char *b = " - partition 1 - path ";
    while (*b) *p++ = *b++;
    *p = 0;
    mixed_text(90, 150, line, C_TEXT, 2);

    gfx_rect(90, 200, 700, 44, C_WHITE);
    gfx_frame(90, 200, 700, 44, C_ACCENT);
    gfx_text(102, 214, S.path, C_TEXT, 2);
    gfx_rect(102 + S.path_len * 16, 210, 3, 26, C_ACCENT);

    mixed_text(90, 280, "System files go here; auth file is /config/auth.", C_TEXT_DIM, 2);
    mixed_text(90, 340, "Type to edit, Enter to continue", C_TEXT_DIM, 2);
    draw_footer("/config/auth path is fixed");
}

static void draw_accounts(void)
{
    panel(60, 90, (int)gfx_width() - 120, 470, "Accounts", ICON_USER);
    const char *labels[3] = { "User name", "User password", "Administrator password" };
    for (int i = 0; i < 3; i++) {
        int y = 150 + i * 78;
        mixed_text(90, y, labels[i], C_TEXT, 2);
        gfx_rect(330, y - 6, 420, 36, C_WHITE);
        gfx_frame(330, y - 6, 420, 36, S.field == i ? C_ACCENT : 0x00A0B0C0u);
        if (i == 0) {
            gfx_text(342, y, S.uname, C_TEXT, 2);
            if (S.field == 0) gfx_rect(342 + S.ulen * 16, y - 2, 3, 24, C_ACCENT);
        } else {
            const char *pw = S.upass;
            int len = S.uplen;
            if (i == 2) { pw = S.apass; len = S.aplen; }
            for (int k = 0; k < len; k++) gfx_char(342 + k * 16, y, '*', C_TEXT, 2);
            if (S.field == i) gfx_rect(342 + len * 16, y - 2, 3, 24, C_ACCENT);
        }
    }
    mixed_text(90, 400, "User name defaults to 'user'; administrator is fixed.", C_TEXT_DIM, 2);
    mixed_text(90, 440, "Passwords are stored as salted SHA-3 hashes only.", C_TEXT_DIM, 2);
    mixed_text(90, 490, "Tab switches field, Enter to continue", C_TEXT_DIM, 2);
    draw_footer("Password: at least 1 character");
}

static void draw_risk(void)
{
    panel(60, 90, (int)gfx_width() - 120, 470, "Before you continue", ICON_WARN);
    mixed_text(90, 150, "* Device support is limited to ATA/AHCI disks;", 0x00A03030u, 2);
    mixed_text(90, 196, "* System stability is not guaranteed;", 0x00A03030u, 2);
    mixed_text(90, 242, "* Partitioning modifies the target disk;", 0x00A03030u, 2);
    mixed_text(90, 288, "* Partitioning modifies the target disk;", 0x00A03030u, 2);
    mixed_text(90, 350, "Select Agree to start writing.", C_TEXT_DIM, 2);

    button(200, 430, 240, 52, "Agree", S.agree_sel == 0);
    button(560, 430, 240, 52, "Cancel", S.agree_sel == 1);
    mixed_text(90, 510, "Left/Right to choose, Enter to confirm", C_TEXT_DIM, 2);
    draw_footer("Cancel exits setup");
}

static void draw_log(void)
{
    panel(60, 90, (int)gfx_width() - 120, 470, S.failed ? "Setup failed" : (S.done ? "Setup complete" : "Writing"), S.failed ? ICON_CROSS : ICON_CHECK);
    for (int i = 0; i < S.nlog; i++)
        mixed_text(90, 150 + i * 34, S.log[i], C_TEXT, 2);
    if (S.done && !S.failed)
        mixed_text(90, 520, "Press Enter to restart", 0x0030A050u, 2);
    if (S.failed)
        mixed_text(90, 520, "Press Enter to try again", C_RED, 2);
    draw_footer(S.done ? "Done. You can restart now." : "Please wait");
}

/* ---------------- page flow ---------------- */
static void draw_page(void)
{
    gfx_gradient(0, 0, (int)gfx_width(), (int)gfx_height(), 0x00102040u, 0x00285088u);
    draw_header();
    switch (S.page) {
    case 0: draw_welcome(); return;
    case 1: draw_disk(); return;
    case 2: draw_part(); return;
    case 3: draw_fs(); return;
    case 4: draw_path(); return;
    case 5: draw_accounts(); return;
    case 6: draw_risk(); return;
    default: draw_log(); return;
    }
}

static void add_log(const char *s)
{
    if (S.nlog >= MAXLOG) return;
    strcpy_s(S.log[S.nlog], sizeof(S.log[0]), s);
    S.nlog++;
    DBG((u8)(0x70 + S.nlog));
}

/* ---------------- write ---------------- */
static void hex_encode(const u8 *in, int n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[n * 2] = 0;
}

static u32 rng_state;
static u32 rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void install_failed(const char *why)
{
    S.failed = 1;
    add_log(why);
    draw_page();
    DBG(0x51);
}

static void do_install(void)
{
    ata_dev *d = &S.disks[S.disk_idx];
    u32 start = 2048;
    u32 sectors = 131072;
    if (d->sectors <= start + 4096) { S.failed = 1; add_log("Target disk is too small"); return; }
    if (sectors > d->sectors - start) sectors = d->sectors - start;

    S.nlog = 0;
    S.writing = 1;
    S.done = 0;
    S.failed = 0;
    add_log("Writing to target disk");
    draw_page();

    /* 1. MBR 分区表 */
    if (S.mbr_on) {
        u8 mbr[512];
        if (ata_read_dev(d, 0, 1, mbr) != 0) { S.failed = 1; add_log("Read MBR failed"); return; }
        for (int i = 446; i < 462; i++) mbr[i] = 0;
        mbr[446] = 0x80;
        mbr[447] = 0x00; mbr[448] = 0x01; mbr[449] = 0x01;
        mbr[450] = 0x0C;
        mbr[451] = 0xFE; mbr[452] = 0xFF; mbr[453] = 0xFF;
        mbr[454] = (u8)(start & 0xFF);
        mbr[455] = (u8)((start >> 8) & 0xFF);
        mbr[456] = (u8)((start >> 16) & 0xFF);
        mbr[457] = (u8)((start >> 24) & 0xFF);
        mbr[458] = (u8)(sectors & 0xFF);
        mbr[459] = (u8)((sectors >> 8) & 0xFF);
        mbr[460] = (u8)((sectors >> 16) & 0xFF);
        mbr[461] = (u8)((sectors >> 24) & 0xFF);
        mbr[510] = 0x55; mbr[511] = 0xAA;
        if (ata_write_dev(d, 0, 1, mbr) != 0) { S.failed = 1; add_log("Write MBR failed"); return; }
        add_log("MBR partition table written");
        draw_page();
    }

    /* 2. 认证内容 */
    char line1[96], line2[96], line3[96], line4[96];
    char auth[512];
    u8 salt1[8], salt2[8], h1[32], h2[32];
    for (int i = 0; i < 8; i++) salt1[i] = (u8)(rnd() >> 8);
    for (int i = 0; i < 8; i++) salt2[i] = (u8)(rnd() >> 16);
    {   /* 哈希 = SHA3-256(salt || password) */
        u8 buf[64];
        for (int i = 0; i < 8; i++) buf[i] = salt1[i];
        for (int i = 0; i < S.uplen && i < 32; i++) buf[8 + i] = (u8)S.upass[i];
        sha3_256(buf, (u32)(8 + S.uplen), h1);
        for (int i = 0; i < 8; i++) buf[i] = salt2[i];
        for (int i = 0; i < S.aplen && i < 32; i++) buf[8 + i] = (u8)S.apass[i];
        sha3_256(buf, (u32)(8 + S.aplen), h2);
    }
    char s1[24], s2[24], x1[72], x2[72];
    hex_encode(salt1, 8, s1);
    hex_encode(salt2, 8, s2);
    hex_encode(h1, 32, x1);
    hex_encode(h2, 32, x2);

    {
        char *p = auth;
        const char *hdr = "# UMDos /config/auth\nversion=" INSTALLER_VERSION "\nuser=";
        while (*hdr) *p++ = *hdr++;
        for (int i = 0; i < S.ulen; i++) *p++ = S.uname[i];
        const char *t1 = "\nuser_auth=sha3-256$";
        while (*t1) *p++ = *t1++;
        for (const char *q = s1; *q; q++) *p++ = *q;
        *p++ = '$';
        for (const char *q = x1; *q; q++) *p++ = *q;
        const char *t2 = "\nadministrator=administrator\nadmin_auth=sha3-256$";
        while (*t2) *p++ = *t2++;
        for (const char *q = s2; *q; q++) *p++ = *q;
        *p++ = '$';
        for (const char *q = x2; *q; q++) *p++ = *q;
        *p++ = '\n';
        *p = 0;
        (void)line1; (void)line2; (void)line3; (void)line4;
    }
    u32 auth_len = (u32)strlen(auth);
    add_log("SHA-3 salted hash generated");
    draw_page();

    /* 3. Filesystem */
    char err[64];
    if (S.fs == 0) {
        if (fat32_format(d, start, sectors, err) != 0) {
            S.failed = 1;
            add_log("FAT32 format failed");
            return;
        }
        add_log("FAT32 formatted");
        draw_page();

        if (fat32_create_dir(d, start, FAT32_ROOT_CLUSTER, "CONFIG     ", 3, err) < 0) {
            S.failed = 1; add_log("Create /CONFIG failed"); return;
        }
        add_log("/CONFIG created");
        draw_page();

        if (fat32_write_file(d, start, 3, "AUTH       ", auth, auth_len, err) != 0) {
            S.failed = 1; add_log("Write /config/auth failed"); return;
        }
        add_log("/config/auth written");
        draw_page();
    } else {
        u8 mark[1024];
        for (int i = 0; i < 1024; i++) mark[i] = 0;
        const char *sig = "UMDOS-EXT32-EXPERIMENTAL";
        for (int i = 0; sig[i]; i++) mark[i] = (u8)sig[i];
        for (u32 i = 0; i < auth_len && 512 + i < 1024; i++) mark[512 + i] = (u8)auth[i];
        if (ata_write_dev(d, start, 2, mark) != 0) { S.failed = 1; add_log("Write EXT32 marker failed"); return; }
        add_log("EXT32 marker written");
        draw_page();
    }

    /* 4. 回读校验 */
    if (S.fs == 0) {
        char rb[512];
        u32 got = 0;
        if (fat32_read_file(d, start, "AUTH       ", rb, sizeof(rb) - 1, &got) != 0) {
            S.failed = 1; add_log("Read back /config/auth failed"); return;
        }
        rb[got] = 0;
        if (!strstr(rb, "sha3-256$") || strstr(rb, S.upass)) {
            S.failed = 1; add_log("Verification failed"); return;
        }
        add_log("Verified: sha3-256, no plaintext");
    } else {
        u8 rb[1024];
        if (ata_read_dev(d, start, 2, rb) != 0) { S.failed = 1; add_log("Read back marker failed"); return; }
        int ok = 1;
        const char *sig = "UMDOS-EXT32-EXPERIMENTAL";
        for (int i = 0; sig[i]; i++) if (rb[i] != (u8)sig[i]) ok = 0;
        if (!ok) { S.failed = 1; add_log("EXT32 marker check failed"); return; }
        add_log("EXT32 marker verified");
    }

    ata_flush_dev(d);
    S.done = 1;
    add_log("Setup complete");
    draw_page();
    DBG(0x50);
    return;
}

/* ---------------- restart ---------------- */
static void reboot_now(void)
{
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 0x02)) break;
    }
    outb(0x64, 0xFE);
    for (;;) hlt();
}

/* ---------------- keys ---------------- */
static void next_page(void)
{
    if (S.page == 6 && S.agree_sel == 1) {       /* cancel */
        mixed_text(60, 600, "Cancelled. Exiting setup...", C_RED, 2);
        for (volatile int i = 0; i < 20000000; i++) { }
        reboot_now();
    }
    if (S.page == 6 && S.agree_sel == 0) {
        S.page = 7;
        draw_page();
        do_install();
        return;
    }
    if (S.page < 6) {
        S.page++;
        draw_page();
        DBG((u8)(0x60 + S.page));
    }
}

static void prev_page(void)
{
    if (S.page > 0 && S.page < 7) {
        S.page--;
        draw_page();
        DBG((u8)(0x60 + S.page));
    }
}

static void handle_key(int k)
{
    if (S.page == 0) {
        if (k == KEY_ENTER || k == ' ') next_page();
        return;
    }
    if (S.page == 7) {
        if (k == KEY_ENTER) {
            if (S.done && !S.failed) reboot_now();
            else { S.page = 6; draw_page(); }
        }
        return;
    }
    if (k == KEY_ENTER) { next_page(); return; }
    if (k == KEY_ESC || k == KEY_BACKSPACE) { prev_page(); return; }

    switch (S.page) {
    case 1:
        if (k == KEY_UP && S.disk_idx > 0) { S.disk_idx--; draw_page(); }
        else if (k == KEY_DOWN && S.disk_idx < S.ndisks - 1) { S.disk_idx++; draw_page(); }
        break;
    case 2:
        if (k == KEY_LEFT || k == KEY_RIGHT || k == ' ') {
            if (k == KEY_LEFT || k == ' ') S.mbr_on = !S.mbr_on;
            else S.format_on = !S.format_on;
            draw_page();
        } else if (k == 'm' || k == 'M') { S.mbr_on = !S.mbr_on; draw_page(); }
        else if (k == 'f' || k == 'F') { S.format_on = !S.format_on; draw_page(); }
        break;
    case 3:
        if (k == KEY_UP || k == KEY_DOWN) { S.fs = !S.fs; draw_page(); }
        else if (k == '1') { S.fs = 0; draw_page(); }
        else if (k == '2') { S.fs = 1; draw_page(); }
        break;
    case 4:
        if (k == KEY_BACKSPACE) {
            if (S.path_len) S.path[--S.path_len] = 0;
            draw_page();
        } else if (k >= 32 && k < 127) {
            if (S.path_len < (int)sizeof(S.path) - 1) {
                S.path[S.path_len++] = (char)k;
                S.path[S.path_len] = 0;
            }
            draw_page();
        }
        break;
    case 5:
        if (k == KEY_TAB || k == KEY_UP || k == KEY_DOWN) {
            S.field = (S.field + 1) % 3;
            draw_page();
        } else if (k == KEY_BACKSPACE) {
            if (S.field == 0) { if (S.ulen) S.uname[--S.ulen] = 0; }
            else if (S.field == 1) { if (S.uplen) S.upass[--S.uplen] = 0; }
            else { if (S.aplen) S.apass[--S.aplen] = 0; }
            draw_page();
        } else if (k >= 32 && k < 127) {
            if (S.field == 0 && S.ulen < 20) { S.uname[S.ulen++] = (char)k; S.uname[S.ulen] = 0; }
            else if (S.field == 1 && S.uplen < 20) { S.upass[S.uplen++] = (char)k; S.upass[S.uplen] = 0; }
            else if (S.field == 2 && S.aplen < 20) { S.apass[S.aplen++] = (char)k; S.apass[S.aplen] = 0; }
            draw_page();
        }
        break;
    case 6:
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_TAB) {
            S.agree_sel = !S.agree_sel;
            draw_page();
        }
        break;
    default: break;
    }
}

/* ---------------- entry ---------------- */
void kmain(bootinfo_t *bi)
{
    gfx_info gi;
    gi.base = bi->fb_base_lo;
    gi.width = bi->fb_width;
    gi.height = bi->fb_height;
    gi.stride = bi->fb_stride;
    gi.bpp = bi->fb_bpp;
    gfx_init(&gi);
    GI = &gi;
    input_init();

    rng_state = (u32)rdtsc() | 1;

    memset(&S, 0, sizeof(S));
    S.page = 0;
    S.mbr_on = 1;
    S.format_on = 1;
    S.fs = 0;
    strcpy_s(S.path, sizeof(S.path), "/UMDos");
    S.path_len = (int)strlen(S.path);
    strcpy_s(S.uname, sizeof(S.uname), "user");
    S.ulen = 4;
    S.ndisks = ata_scan(S.disks);
    S.disk_idx = 0;
    S.agree_sel = 0;

    draw_page();
    DBG(0x40);

    for (;;) {
        input_event ev;
        if (!input_poll(&ev))
            continue;
        if (ev.type == EV_KEY)
            handle_key(ev.key);
    }
}
