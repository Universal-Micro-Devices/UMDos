/* libk.c - freestanding 字符串/内存函数（内核不链接 libc） */
#include "io.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

void strcpy_s(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* 无符号整数转字符串，返回 buf 末尾的 0 位置 */
char *itou(uint64_t v, char *buf, int base, int width)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[32];
    int n = 0;
    if (base < 2 || base > 16) base = 10;
    do {
        tmp[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v);
    int pad = width - n;
    char *p = buf;
    while (pad-- > 0) *p++ = '0';
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}
