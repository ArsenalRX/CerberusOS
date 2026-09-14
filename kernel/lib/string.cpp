// Straightforward byte-at-a-time implementations. Speed does not matter before
// phase 14 (docs/SPEC.md section 17); correctness and freedom from SSE do.
#include <lib/string.h>

extern "C" {

void* memcpy(void* dst, const void* src, usize n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, usize n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int c, usize n) {
    u8* d = (u8*)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

int memcmp(const void* a, const void* b, usize n) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    for (; n; n--, x++, y++) {
        if (*x != *y) return *x < *y ? -1 : 1;
    }
    return 0;
}

usize strlen(const char* s) {
    usize n = 0;
    while (s[n]) n++;
    return n;
}

usize strnlen(const char* s, usize max) {
    usize n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

int strncmp(const char* a, const char* b, usize n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

char* strncpy(char* dst, const char* src, usize n) {
    usize i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

} // extern "C"

usize strlcpy(char* dst, const char* src, usize n) {
    usize len = strlen(src);
    if (n) {
        usize copy = len < n - 1 ? len : n - 1;
        memcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return len;
}
