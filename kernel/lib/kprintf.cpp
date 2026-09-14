// printf-style formatting without a heap, floating point, or locale.
#include <lib/console.h>
#include <lib/kprintf.h>
#include <lib/string.h>

namespace {

struct Spec {
    bool left = false;
    bool zero = false;
    bool alt = false;
    bool plus = false;
    bool space = false;
    int width = -1;
    int precision = -1;
};

struct Out {
    kprintf_sink sink;
    void* ctx;
    int count = 0;
    void put(char c) {
        sink(c, ctx);
        count++;
    }
    void pad(char c, int n) {
        while (n-- > 0) put(c);
    }
};

void emit_str(Out& o, const Spec& s, const char* str, usize len) {
    int padding = s.width > (int)len ? s.width - (int)len : 0;
    if (!s.left) o.pad(' ', padding);
    for (usize i = 0; i < len; i++) o.put(str[i]);
    if (s.left) o.pad(' ', padding);
}

// Formats an unsigned magnitude with an optional sign/prefix, honouring width,
// zero padding, and left alignment.
void emit_num(Out& o, const Spec& s, u64 v, unsigned base, bool upper, const char* prefix,
              bool negative) {
    char digits[65];
    int n = 0;
    const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = alphabet[v % base];
        v /= base;
    } while (v);

    char sign = 0;
    if (negative) sign = '-';
    else if (s.plus) sign = '+';
    else if (s.space) sign = ' ';

    int prefix_len = prefix ? (int)strlen(prefix) : 0;
    int body = n + (sign ? 1 : 0) + prefix_len;
    int padding = s.width > body ? s.width - body : 0;

    if (!s.left && !s.zero) o.pad(' ', padding);
    if (sign) o.put(sign);
    for (int i = 0; i < prefix_len; i++) o.put(prefix[i]);
    if (!s.left && s.zero) o.pad('0', padding);
    while (n) o.put(digits[--n]);
    if (s.left) o.pad(' ', padding);
}

} // namespace

int kvformat(kprintf_sink sink, void* ctx, const char* fmt, va_list ap) {
    Out o{sink, ctx};
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            o.put(*p);
            continue;
        }
        p++;
        Spec s;
        for (;; p++) {
            if (*p == '-') s.left = true;
            else if (*p == '0') s.zero = true;
            else if (*p == '#') s.alt = true;
            else if (*p == '+') s.plus = true;
            else if (*p == ' ') s.space = true;
            else break;
        }
        if (*p == '*') {
            s.width = va_arg(ap, int);
            if (s.width < 0) {
                s.left = true;
                s.width = -s.width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                s.width = (s.width < 0 ? 0 : s.width) * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == '.') {
            p++;
            s.precision = 0;
            if (*p == '*') {
                s.precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    s.precision = s.precision * 10 + (*p - '0');
                    p++;
                }
            }
        }
        // 0 = int, 1 = long, 2 = long long, 3 = size_t, -1 = short, -2 = char
        int len = 0;
        if (*p == 'l') {
            len = 1;
            p++;
            if (*p == 'l') {
                len = 2;
                p++;
            }
        } else if (*p == 'z') {
            len = 3;
            p++;
        } else if (*p == 'h') {
            len = -1;
            p++;
            if (*p == 'h') {
                len = -2;
                p++;
            }
        }

        auto read_unsigned = [&]() -> u64 {
            switch (len) {
            case 1: return va_arg(ap, unsigned long);
            case 2: return va_arg(ap, unsigned long long);
            case 3: return va_arg(ap, usize);
            case -1: return (unsigned short)va_arg(ap, unsigned);
            case -2: return (unsigned char)va_arg(ap, unsigned);
            default: return va_arg(ap, unsigned);
            }
        };
        auto read_signed = [&]() -> i64 {
            switch (len) {
            case 1: return va_arg(ap, long);
            case 2: return va_arg(ap, long long);
            case 3: return va_arg(ap, isize);
            case -1: return (short)va_arg(ap, int);
            case -2: return (signed char)va_arg(ap, int);
            default: return va_arg(ap, int);
            }
        };

        switch (*p) {
        case 'd':
        case 'i': {
            i64 v = read_signed();
            bool neg = v < 0;
            u64 mag = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
            emit_num(o, s, mag, 10, false, nullptr, neg);
            break;
        }
        case 'u': emit_num(o, s, read_unsigned(), 10, false, nullptr, false); break;
        case 'x': emit_num(o, s, read_unsigned(), 16, false, s.alt ? "0x" : nullptr, false); break;
        case 'X': emit_num(o, s, read_unsigned(), 16, true, s.alt ? "0X" : nullptr, false); break;
        case 'o': emit_num(o, s, read_unsigned(), 8, false, s.alt ? "0" : nullptr, false); break;
        case 'b': emit_num(o, s, read_unsigned(), 2, false, s.alt ? "0b" : nullptr, false); break;
        case 'p': {
            Spec ps = s;
            ps.zero = true;
            if (ps.width < 0) ps.width = 18;
            emit_num(o, ps, (u64)(uintptr_t)va_arg(ap, void*), 16, false, "0x", false);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_str(o, s, &c, 1);
            break;
        }
        case 's': {
            const char* str = va_arg(ap, const char*);
            if (!str) str = "(null)";
            usize n = s.precision >= 0 ? strnlen(str, (usize)s.precision) : strlen(str);
            emit_str(o, s, str, n);
            break;
        }
        case '%': o.put('%'); break;
        case 0: return o.count;
        default:
            o.put('%');
            o.put(*p);
            break;
        }
    }
    return o.count;
}

namespace {
void console_sink(char c, void*) { console_putc(c); }

struct BufCtx {
    char* buf;
    usize cap;
    usize pos;
};
void buf_sink(char c, void* ctx) {
    BufCtx* b = (BufCtx*)ctx;
    if (b->pos + 1 < b->cap) b->buf[b->pos] = c;
    b->pos++;
}
} // namespace

int kvprintf(const char* fmt, va_list ap) { return kvformat(console_sink, nullptr, fmt, ap); }

int kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

int kvsnprintf(char* buf, usize n, const char* fmt, va_list ap) {
    BufCtx ctx{buf, n, 0};
    int count = kvformat(buf_sink, &ctx, fmt, ap);
    if (n) buf[ctx.pos < n ? ctx.pos : n - 1] = 0;
    return count;
}

int ksnprintf(char* buf, usize n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = kvsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return count;
}
