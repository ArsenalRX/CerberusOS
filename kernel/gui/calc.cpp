// See calc.h. Numbers are i64 scaled by 10^6 (SCALE); multiplication and
// division go through 128 bits with a small shift-subtract divider, since
// the kernel links no libgcc division helper and has no floating point.
#include <gui/calc.h>
#include <lib/kprintf.h>
#include <lib/string.h>

using namespace gfx;

namespace {

constexpr i64 SCALE = 1000000;
constexpr i64 LIMIT = 9000000000000LL * SCALE;        // |values| above this are an error

// Unsigned 128 / 64 -> 64 (quotient must fit; the callers check).
u64 udiv128(unsigned __int128 n, u64 d) {
    u64 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | (u64)((n >> i) & 1);
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    return q;
}

// Signed (a * b) / c, exact in 128 bits; false when the result does not fit.
bool muldiv(i64 a, i64 b, i64 c, i64* out) {
    if (c == 0) return false;
    bool neg = (a < 0) != (b < 0);
    if (c < 0) { neg = !neg; c = -c; }
    unsigned __int128 n = (unsigned __int128)(a < 0 ? -(u64)a : (u64)a) * (unsigned __int128)(b < 0 ? -(u64)b : (u64)b);
    // The quotient must fit in 63 bits: n < c * 2^63.
    if (n >= ((unsigned __int128)(u64)c << 63)) return false;
    u64 q = udiv128(n, (u64)c);
    if (q > (u64)LIMIT) return false;
    *out = neg ? -(i64)q : (i64)q;
    return true;
}

struct Parser {
    const char* p;
    bool error = false;

    void skip() { while (*p == ' ') p++; }
    bool check(i64 v) { if (v > LIMIT || v < -LIMIT) error = true; return !error; }

    i64 number() {
        i64 whole = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (whole > LIMIT / SCALE / 10) { error = true; return 0; }
            whole = whole * 10 + (*p++ - '0');
            digits++;
        }
        i64 frac = 0, place = SCALE / 10;
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') {
                if (place) frac += (*p - '0') * place;
                place /= 10;
                p++;
                digits++;
            }
        }
        if (!digits) error = true;
        return whole * SCALE + frac;
    }

    i64 primary() {
        skip();
        if (*p == '(') {
            p++;
            i64 v = expression();
            skip();
            if (*p != ')') error = true;
            else p++;
            return v;
        }
        if (*p == '-') { p++; return -primary(); }
        if (*p == '+') { p++; return primary(); }
        return number();
    }

    i64 power() {
        i64 base = primary();
        skip();
        if (*p != '^') return base;
        p++;
        i64 e = power();            // right-associative
        if (error) return 0;
        if (e < 0 || e % SCALE != 0 || e / SCALE > 64) { error = true; return 0; }
        i64 r = SCALE;
        for (i64 i = 0; i < e / SCALE; i++) {
            if (!muldiv(r, base, SCALE, &r)) { error = true; return 0; }
        }
        return r;
    }

    i64 term() {
        i64 v = power();
        for (;;) {
            skip();
            char op = *p;
            if (op != '*' && op != '/' && op != '%') return v;
            p++;
            i64 rhs = power();
            if (error) return 0;
            if (op == '*') { if (!muldiv(v, rhs, SCALE, &v)) error = true; }
            else if (op == '/') { if (rhs == 0 || !muldiv(v, SCALE, rhs, &v)) error = true; }
            else { if (rhs == 0) error = true; else v %= rhs; }
            if (error) return 0;
        }
    }

    i64 expression() {
        i64 v = term();
        for (;;) {
            skip();
            char op = *p;
            if (op != '+' && op != '-') return v;
            p++;
            i64 rhs = term();
            if (error) return 0;
            v = op == '+' ? v + rhs : v - rhs;
            if (!check(v)) return 0;
        }
    }
};

// "12.5", "-3", "0.000001": the scaled value without trailing zeros.
void format_fixed(i64 v, char* out, usize n) {
    bool neg = v < 0;
    u64 a = neg ? -(u64)v : (u64)v;
    u64 whole = a / SCALE, frac = a % SCALE;
    char f[8] = {};
    if (frac) {
        ksnprintf(f, sizeof f, "%06lu", (unsigned long)frac);
        int e = 6;
        while (e > 0 && f[e - 1] == '0') e--;
        f[e] = 0;
    }
    ksnprintf(out, n, "%s%lu%s%s", neg ? "-" : "", (unsigned long)whole, frac ? "." : "", f);
}

} // namespace

void CalcApp::push(char ch) {
    if (fresh_) {
        bool digit = (ch >= '0' && ch <= '9') || ch == '.' || ch == '(';
        if (digit) len_ = 0;            // a new number starts over; an operator continues from the result
        else {
            strlcpy(expr_, result_, sizeof expr_);
            len_ = (int)strlen(expr_);
        }
        fresh_ = false;
    }
    if (len_ < (int)sizeof expr_ - 1) {
        expr_[len_++] = ch;
        expr_[len_] = 0;
    }
}

void CalcApp::backspace() {
    if (fresh_) { clear(); return; }
    if (len_) expr_[--len_] = 0;
}

void CalcApp::clear() {
    len_ = 0;
    expr_[0] = 0;
    result_[0] = hex_[0] = bin_[0] = 0;
    error_ = false;
    fresh_ = false;
}

void CalcApp::evaluate() {
    if (!len_) return;
    Parser ps{expr_};
    i64 v = ps.expression();
    ps.skip();
    if (*ps.p) ps.error = true;
    error_ = ps.error;
    if (error_) {
        strlcpy(result_, "Error", sizeof result_);
        hex_[0] = bin_[0] = 0;
        fresh_ = true;
        return;
    }
    format_fixed(v, result_, sizeof result_);
    i64 whole = v / SCALE;
    u64 a = whole < 0 ? -(u64)whole : (u64)whole;
    ksnprintf(hex_, sizeof hex_, "%s0x%lX", whole < 0 ? "-" : "", (unsigned long)a);
    // Binary, grouped in fours, at most 32 bits shown.
    if (a >> 32) {
        strlcpy(bin_, "(too large for binary)", sizeof bin_);
    } else {
        int bits = 1;
        while (bits < 32 && (a >> bits)) bits++;
        bits = (bits + 3) & ~3;
        int o = 0;
        if (whole < 0) bin_[o++] = '-';
        for (int i = bits - 1; i >= 0 && o < (int)sizeof bin_ - 2; i--) {
            bin_[o++] = (char)('0' + ((a >> i) & 1));
            if (i && i % 4 == 0) bin_[o++] = ' ';
        }
        bin_[o] = 0;
    }
    fresh_ = true;
}

bool CalcApp::key(const KeyEvent& e, const AppContext& c) {
    char ch = e.ascii;
    if (e.key == key::ENTER || ch == '=') { evaluate(); return true; }
    if (e.key == key::BACKSPACE) { backspace(); return true; }
    if (e.key == key::ESCAPE) { clear(); return true; }
    if ((e.mods & mod::CTRL) && (ch == 0x16 || ch == 'v')) {
        for (usize i = 0; i < c.clipboard_len; i++) {
            char k = c.clipboard[i];
            if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '(' ||
                k == ')' || k == '^' || k == '%' || k == ' ')
                push(k);
        }
        return true;
    }
    if ((e.mods & mod::CTRL) && (ch == 0x03 || ch == 'c')) {
        if (result_[0] && !error_) c.clipboard_set(result_, strlen(result_));
        return false;
    }
    if (ch == 'x' || ch == 'X') ch = '*';
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '(' ||
        ch == ')' || ch == '^' || ch == '%' || ch == ' ') {
        push(ch);
        return true;
    }
    return false;
}

bool CalcApp::click(int x, int y, int button, const AppContext& c) {
    if (button != 0) return false;
    for (int i = 0; i < button_count_; i++) {
        const Button& b = buttons_[i];
        if (!b.r.contains(x, y)) continue;
        if (b.ch) { push(b.ch); return true; }
        if (!strcmp(b.label, "=")) evaluate();
        else if (!strcmp(b.label, "C")) clear();
        else if (!strcmp(b.label, "<")) backspace();
        else if (!strcmp(b.label, "M+")) { if (result_[0] && !error_) { Parser ps{result_}; memory_ = ps.expression(); } }
        else if (!strcmp(b.label, "MR")) {
            char t[32];
            format_fixed(memory_, t, sizeof t);
            for (const char* p = t; *p; p++) push(*p);
        } else if (!strcmp(b.label, "copy")) {
            if (result_[0] && !error_) c.clipboard_set(result_, strlen(result_));
            return false;
        }
        return true;
    }
    return false;
}

void CalcApp::paint(Surface& s, const AppContext& ctx) {
    fill_rect(s, s.bounds(), ctx.bg);
    const int W = s.width;
    // Display: the expression small, the result large, hex and binary under.
    Rect disp{14, 12, W - 28, 112};
    fill_rect_rounded(s, disp, 12, rgba(255, 255, 255, 7));
    stroke_rect_rounded(s, disp, 12, rgba(255, 255, 255, 14));
    int tw = measure_text(*ctx.mono, expr_);
    if (len_) draw_text(s, *ctx.mono, disp.right() - 14 - (tw < disp.w - 28 ? tw : disp.w - 28), disp.y + 12, expr_,
                        ctx.muted);
    const char* big = result_[0] ? result_ : (len_ ? "" : "0");
    tw = measure_text(*ctx.bold, big);
    draw_text(s, *ctx.bold, disp.right() - 14 - tw, disp.y + 40, big, error_ ? rgb(248, 113, 113) : ctx.text);
    char line[96];
    if (hex_[0]) {
        ksnprintf(line, sizeof line, "%s   %s", hex_, bin_);
        draw_text_ellipsis(s, *ctx.mono, disp.x + 14, disp.y + 84, line, disp.w - 28, ctx.muted);
    }
    // Keys.
    static const char* const ROWS[5][5] = {
        {"C", "(", ")", "<", "/"}, {"7", "8", "9", "*", "^"}, {"4", "5", "6", "-", "%"}, {"1", "2", "3", "+", "M+"}, {"0", ".", "=", "copy", "MR"},
    };
    button_count_ = 0;
    int top = disp.bottom() + 14, gap = 8;
    int bw = (W - 28 - 4 * gap) / 5, bh = (s.height - top - 14 - 4 * gap) / 5;
    if (bh < 30) bh = 30;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 5; c++) {
            const char* label = ROWS[r][c];
            Rect b{14 + c * (bw + gap), top + r * (bh + gap), bw, bh};
            bool op = c >= 3 || !strcmp(label, "=") || !strcmp(label, "C");
            Color bg = !strcmp(label, "=") ? ctx.accent : op ? rgba(255, 255, 255, 18) : rgba(255, 255, 255, 10);
            fill_rect_rounded(s, b, 10, bg);
            const char* shown = !strcmp(label, "*") ? "x" : !strcmp(label, "/") ? "/" : !strcmp(label, "<") ? "Back" : label;
            int lw = measure_text(*ctx.font, shown);
            draw_text(s, *ctx.font, b.x + (b.w - lw) / 2, b.y + (b.h - ctx.font->height) / 2, shown,
                      !strcmp(label, "=") ? rgb(15, 23, 42) : ctx.text);
            char ch = 0;
            if (strlen(label) == 1 && label[0] != 'C' && label[0] != '=' && label[0] != '<') ch = label[0];
            if (button_count_ < (int)(sizeof buttons_ / sizeof buttons_[0])) buttons_[button_count_++] = Button{b, ch, label};
        }
    }
}
