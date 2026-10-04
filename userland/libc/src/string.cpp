// Memory and string primitives. The compiler may emit calls to memcpy,
// memset, memmove and memcmp on its own, so they must exist.
#include <cerberus.h>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    asm volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    while (n--) d[n] = s[n];            // overlapping, destination above source: copy backwards
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return nullptr;
    }
}

size_t strlcpy(char* dst, const char* src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t take = len < n - 1 ? len : n - 1;
        memcpy(dst, src, take);
        dst[take] = 0;
    }
    return len;
}

unsigned long strtoul(const char* s, char** end, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long v = 0;
    for (;; s++) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        v = v * (unsigned long)base + (unsigned long)digit;
    }
    if (end) *end = (char*)s;
    return v;
}

} // extern "C"
