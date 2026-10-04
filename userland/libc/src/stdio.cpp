// Formatted output. printf formats into a buffer on the stack and writes it
// to standard output in one call, so a line is not split between processes.
#include <lumen.h>

namespace {

struct Out {
    char* buf;
    size_t cap;
    size_t pos;         // characters produced, including those that did not fit
};

void put(Out& o, char c) {
    if (o.pos + 1 < o.cap) o.buf[o.pos] = c;
    o.pos++;
}

void put_padded(Out& o, const char* s, size_t len, int width, bool left, char pad) {
    size_t w = width > 0 ? (size_t)width : 0;
    if (!left)
        for (size_t i = len; i < w; i++) put(o, pad);
    for (size_t i = 0; i < len; i++) put(o, s[i]);
    if (left)
        for (size_t i = len; i < w; i++) put(o, ' ');
}

void put_number(Out& o, unsigned long long v, unsigned base, bool upper, bool negative, int width, bool left,
                bool zero) {
    char digits[24];
    size_t n = 0;
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do digits[n++] = set[v % base];
    while (v /= base);
    char text[26];
    size_t len = 0;
    if (negative) text[len++] = '-';
    if (zero && !left) {                // zero padding goes between the sign and the digits
        size_t w = width > 0 ? (size_t)width : 0;
        for (size_t i = len + n; i < w && len < sizeof text - n; i++) text[len++] = '0';
    }
    while (n) text[len++] = digits[--n];
    put_padded(o, text, len, width, left, ' ');
}

} // namespace

extern "C" {

int vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap) {
    Out o{buf, cap, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(o, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'z') {
            longs++;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = longs ? va_arg(ap, long) : va_arg(ap, int);
            bool neg = v < 0;
            put_number(o, neg ? 0ull - (unsigned long long)v : (unsigned long long)v, 10, false, neg, width, left, zero);
            break;
        }
        case 'u':
            put_number(o, longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, false, false, width, left, zero);
            break;
        case 'x':
        case 'X':
            if (alt) {
                put(o, '0');
                put(o, 'x');
            }
            put_number(o, longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, *fmt == 'X', false, width, left,
                       zero);
            break;
        case 'p':
            put(o, '0');
            put(o, 'x');
            put_number(o, (unsigned long)va_arg(ap, void*), 16, false, false, 0, false, false);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(o, &c, 1, width, left, ' ');
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            put_padded(o, s, strlen(s), width, left, ' ');
            break;
        }
        case '%':
            put(o, '%');
            break;
        case 0:
            fmt--;              // a lone '%' at the end of the format
            break;
        default:                // unknown conversion: print it as written
            put(o, '%');
            put(o, *fmt);
            break;
        }
    }
    if (cap) buf[o.pos < cap ? o.pos : cap - 1] = 0;
    return (int)o.pos;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    write(1, buf, len);
    return n;
}

int puts(const char* s) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\n", s);
    write(1, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    return 0;
}

} // extern "C"
