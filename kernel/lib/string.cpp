// memcpy/memset/memmove use the x86 string instructions: on every CPU we care
// about `rep movsb`/`rep stosb` are microcoded fast paths (ERMSB) and they
// never touch SSE, which the kernel is built without. The str* functions are
// plain loops; speed does not matter there before phase 14.
#include <lib/string.h>

extern "C" {

void* memcpy(void* dst, const void* src, usize n) {
    void* ret = dst;
    asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, usize n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        // Overlapping with dst above src: copy backwards with the direction flag set.
        d += n - 1;
        s += n - 1;
        asm volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dst;
}

void* memset(void* dst, int c, usize n) {
    void* ret = dst;
    asm volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"((u8)c) : "memory");
    return ret;
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

const char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return s;
        if (!*s) return nullptr;
    }
}
