/* auth.c - 从 UMDFS 的 /CONFIG/AUTH 读取认证信息并校验
 *
 * 文件格式（安装引导也会写同样的格式）：
 *   version=v2.6.112-dev.N
 *   user=user
 *   user_auth=sha3-256$<盐的十六进制>$<哈希的十六进制>
 *   administrator=administrator
 *   admin_auth=sha3-256$<盐>$<哈希>
 */
#include "auth.h"
#include "fs.h"
#include "vfs.h"
#include "sha3.h"
#include "io.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *s, u8 *out, int max)
{
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (u8)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static void hex_encode(const u8 *in, int n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[n * 2] = 0;
}

/* 在文本里找 "key=value" 行的 value；返回 value 起始指针（不是 0 结尾）和长度 */
static const char *find_value(const char *text, u32 len, const char *key, u32 *vlen)
{
    u32 klen = (u32)strlen(key);
    for (u32 i = 0; i + klen + 1 <= len; i++) {
        if (i > 0 && text[i - 1] != '\n') continue;
        if (strncmp(text + i, key, klen) != 0) continue;
        if (text[i + klen] != '=') continue;
        const char *v = text + i + klen + 1;
        u32 n = 0;
        while (i + klen + 1 + n < len && v[n] != '\n' && v[n] != '\r') n++;
        *vlen = n;
        return v;
    }
    return 0;
}

static int value_eq(const char *v, u32 n, const char *s)
{
    if ((u32)strlen(s) != n) return 0;
    return strncmp(v, s, n) == 0;
}

static const char *g_src = "none";
static char g_name[32] = "";
static int  g_role;             /* 1 = administrator */

const char *auth_name(void) { return g_name; }
int auth_role(void) { return g_role; }

const char *auth_source(void) { return g_src; }

static int auth_check_impl(const char *user, const char *pass)
{
    g_src = "none";
    static char text_buf[512];
    int fd = vfs_open("/config/auth", VFS_O_RDONLY);
    if (fd < 0) return -2;
    int n = vfs_read(fd, text_buf, sizeof(text_buf) - 1);
    vfs_close(fd);
    if (n <= 0) return -2;
    text_buf[n] = 0;
    const char *text = text_buf;
    u32 len = (u32)n;
    g_src = "/config/auth";

    /* 用户名：先看 user=，再看 administrator= */
    u32 vlen = 0;
    const char *v = find_value(text, len, "user", &vlen);
    const char *authkey = "user_auth";
    int is_admin = 0;
    if (!v || !value_eq(v, vlen, user)) {
        v = find_value(text, len, "administrator", &vlen);
        authkey = "admin_auth";
        is_admin = 1;
        if (!v || !value_eq(v, vlen, user))
            return -3;
    }

    u32 hlen = 0;
    const char *h = find_value(text, len, authkey, &hlen);
    if (!h || hlen < 16) return -3;
    char buf[160];
    if (hlen > sizeof(buf) - 1) hlen = sizeof(buf) - 1;
    for (u32 i = 0; i < hlen; i++) buf[i] = h[i];
    buf[hlen] = 0;

    /* 期望格式 sha3-256$salt$hash */
    const char *p1 = buf;
    const char *p2 = 0;
    const char *p3 = 0;
    for (char *p = buf; *p; p++) {
        if (*p == '$') {
            if (!p2) { p2 = p + 1; *p = 0; }
            else { p3 = p + 1; *p = 0; break; }
        }
    }
    if (!p2 || !p3 || strcmp(p1, "sha3-256") != 0) return -3;
    /* 记住登录名（给 whoami 与权限用） */
    {
        u32 k = 0;
        while (user[k] && k < sizeof(g_name) - 1) { g_name[k] = user[k]; k++; }
        g_name[k] = 0;
        g_role = is_admin;
    }

    u8 salt[32];
    int saltlen = hex_decode(p2, salt, sizeof(salt));
    if (saltlen <= 0) return -3;

    u8 msg[96];
    for (int i = 0; i < saltlen; i++) msg[i] = salt[i];
    u32 plen = (u32)strlen(pass);
    if (saltlen + (int)plen > (int)sizeof(msg)) plen = sizeof(msg) - (u32)saltlen;
    for (u32 i = 0; i < plen; i++) msg[saltlen + i] = (u8)pass[i];

    u8 digest[32];
    sha3_256(msg, (u32)saltlen + plen, digest);
    char hex[80];
    hex_encode(digest, 32, hex);
    return strcmp(hex, p3) == 0 ? 0 : -4;
}

/* 对外的 auth_check：认证过程中会切换 fs 的当前目录，结束后必须恢复，
 * 否则桌面打开文件管理器时会停留在 /CONFIG。 */
int auth_check(const char *user, const char *pass)
{
    u32 saved = fs_dir();
    int rc = auth_check_impl(user, pass);
    fs_set_dir(saved);
    return rc;
}
