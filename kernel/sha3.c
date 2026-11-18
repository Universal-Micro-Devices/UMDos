/* sha3.c - SHA3-256 (Keccak-f[1600])，用于安装时生成密码哈希 */
#include "sha3.h"
#include "types.h"

static const u64 RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static u64 rotl(u64 x, int n)
{
    if (n == 0) return x;
    return (x << n) | (x >> (64 - n));
}

static void keccakf(u64 s[25])
{
    static const int rho[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
    static const int pi[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };

    for (int round = 0; round < 24; round++) {
        u64 c[5];
        for (int x = 0; x < 5; x++)
            c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        for (int x = 0; x < 5; x++) {
            u64 t = c[(x + 4) % 5] ^ rotl(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5)
                s[y + x] ^= t;
        }
        u64 t = s[1];
        for (int i = 0; i < 24; i++) {
            int j = pi[i];
            u64 tmp = s[j];
            s[j] = rotl(t, rho[i]);
            t = tmp;
        }
        for (int y = 0; y < 25; y += 5) {
            u64 a[5];
            for (int x = 0; x < 5; x++) a[x] = s[y + x];
            for (int x = 0; x < 5; x++)
                s[y + x] = a[x] ^ ((~a[(x + 1) % 5]) & a[(x + 2) % 5]);
        }
        s[0] ^= RC[round];
    }
}

static void absorb(u64 s[25], const u8 *p, u32 len)
{
    const u32 rate = 136;
    while (len >= rate) {
        for (u32 i = 0; i < rate / 8; i++) {
            u64 v = 0;
            for (int k = 0; k < 8; k++)
                v |= (u64)p[i * 8 + k] << (8 * k);
            s[i] ^= v;
        }
        keccakf(s);
        p += rate;
        len -= rate;
    }
    u8 block[136];
    for (u32 i = 0; i < rate; i++) block[i] = 0;
    for (u32 i = 0; i < len; i++) block[i] = p[i];
    block[len] ^= 0x06;
    block[rate - 1] ^= 0x80;
    for (u32 i = 0; i < rate / 8; i++) {
        u64 v = 0;
        for (int k = 0; k < 8; k++)
            v |= (u64)block[i * 8 + k] << (8 * k);
        s[i] ^= v;
    }
    keccakf(s);
}

void sha3_256(const void *data, u32 len, u8 out[32])
{
    u64 s[25];
    for (int i = 0; i < 25; i++) s[i] = 0;
    absorb(s, (const u8 *)data, len);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            out[i * 8 + k] = (u8)(s[i] >> (8 * k));
}
