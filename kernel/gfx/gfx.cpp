// Software rasteriser. Everything goes through fill spans and per-pixel
// blends; clarity over speed until phase 14, except that fully opaque fills
// and blits use straight stores.
#include <gfx/gfx.h>

namespace gfx {

namespace {

inline Color blend(Color dst, Color src) {
    u32 a = alpha_of(src);
    if (a == 255) return src;
    if (a == 0) return dst;
    u32 ia = 255 - a;
    u32 r = (red_of(src) * a + red_of(dst) * ia) / 255;
    u32 g = (green_of(src) * a + green_of(dst) * ia) / 255;
    u32 b = (blue_of(src) * a + blue_of(dst) * ia) / 255;
    u32 da = alpha_of(dst);
    u32 oa = a + da * ia / 255;
    return ((u32)oa << 24) | (r << 16) | (g << 8) | b;
}

inline void span(Surface& s, int x0, int x1, int y, Color c) {
    if (y < s.clip.y || y >= s.clip.bottom()) return;
    if (x0 < s.clip.x) x0 = s.clip.x;
    if (x1 > s.clip.right()) x1 = s.clip.right();
    if (x0 >= x1) return;
    u32* p = s.row(y) + x0;
    if (alpha_of(c) == 255) {
        for (int x = x0; x < x1; x++) *p++ = c;
    } else {
        for (int x = x0; x < x1; x++, p++) *p = blend(*p, c);
    }
}

inline void put(Surface& s, int x, int y, Color c) {
    if (!s.clip.contains(x, y)) return;
    u32* p = s.row(y) + x;
    *p = blend(*p, c);
}

} // namespace

int corner_inset(int radius, int dy) {
    // Distance from the arc centre in y is (radius - dy - 0.5); inset = radius - sqrt(r^2 - y^2).
    int yy = radius - dy;
    int y2 = (2 * yy - 1) * (2 * yy - 1);        // (2y-1)^2 to keep half-pixel precision
    int r2 = 4 * radius * radius;
    if (y2 >= r2) return radius;
    // integer sqrt of (r2 - y2)
    int v = r2 - y2, x = 0;
    while ((x + 1) * (x + 1) <= v) x++;
    return radius - (x + 1) / 2;
}

Color mix(Color a, Color b, u8 t) {
    u32 it = 255 - t;
    u32 r = (red_of(a) * it + red_of(b) * t) / 255;
    u32 g = (green_of(a) * it + green_of(b) * t) / 255;
    u32 bl = (blue_of(a) * it + blue_of(b) * t) / 255;
    u32 al = (alpha_of(a) * it + alpha_of(b) * t) / 255;
    return (al << 24) | (r << 16) | (g << 8) | bl;
}

Rect Rect::intersect(const Rect& o) const {
    int nx = x > o.x ? x : o.x;
    int ny = y > o.y ? y : o.y;
    int nr = right() < o.right() ? right() : o.right();
    int nb = bottom() < o.bottom() ? bottom() : o.bottom();
    return {nx, ny, nr - nx, nb - ny};
}

Rect Rect::unite(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    int nx = x < o.x ? x : o.x;
    int ny = y < o.y ? y : o.y;
    int nr = right() > o.right() ? right() : o.right();
    int nb = bottom() > o.bottom() ? bottom() : o.bottom();
    return {nx, ny, nr - nx, nb - ny};
}

Surface Surface::sub(const Rect& r) const {
    Rect rr = r.intersect(bounds());
    Surface s;
    if (rr.empty()) return s;
    s.pixels = row(rr.y) + rr.x;
    s.width = rr.w;
    s.height = rr.h;
    s.stride = stride;
    s.clip = clip.intersect(rr).translated(-rr.x, -rr.y);
    return s;
}

void ClipStack::push(Surface& s, const Rect& r) {
    if (depth < 16) stack[depth++] = s.clip;
    s.clip = s.clip.intersect(r);
}

void ClipStack::pop(Surface& s) {
    if (depth > 0) s.clip = stack[--depth];
}

Font font_from_psf2(const u8* blob) {
    Font f;
    const u32* h = (const u32*)blob;
    if (h[0] != 0x864AB572) return f;
    f.glyphs = blob + h[2];
    f.glyph_count = h[4];
    f.bytes_per_glyph = h[5];
    f.height = (int)h[6];
    f.width = (int)h[7];
    return f;
}

void fill_rect(Surface& s, const Rect& r, Color c) {
    Rect rr = r.intersect(s.clip);
    if (rr.empty()) return;
    for (int y = rr.y; y < rr.bottom(); y++) span(s, rr.x, rr.right(), y, c);
}

namespace {

// Corner coverage tables, one per radius in use (windows, buttons, menus use
// a handful of radii), so the 8x8 supersampling runs once per radius.
constexpr int MAX_RADIUS = 32;
struct CornerTable {
    int radius = -1;
    u8 cov[MAX_RADIUS][MAX_RADIUS];
};
CornerTable g_corner_tables[6];
int g_corner_next = 0;

const CornerTable& corner_table(int radius) {
    for (const CornerTable& t : g_corner_tables)
        if (t.radius == radius) return t;
    CornerTable& t = g_corner_tables[g_corner_next];
    g_corner_next = (g_corner_next + 1) % (int)(sizeof g_corner_tables / sizeof g_corner_tables[0]);
    t.radius = radius;
    // Arc centre at (radius, radius); coordinates in 1/16 pixel so the 8x8
    // sample points sit at odd sixteenths (pixel centres of the sub-grid).
    int r2 = radius * radius * 256;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int inside = 0;
            for (int j = 0; j < 8; j++) {
                int sy = dy * 16 + j * 2 + 1 - radius * 16;
                for (int i = 0; i < 8; i++) {
                    int sx = dx * 16 + i * 2 + 1 - radius * 16;
                    if (sx * sx + sy * sy <= r2) inside++;
                }
            }
            t.cov[dy][dx] = inside == 64 ? 255 : (u8)(inside * 4);
        }
    }
    return t;
}

inline int clamp_radius(const Rect& r, int radius) {
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;
    return radius > MAX_RADIUS ? MAX_RADIUS : radius;
}

inline void put_cov(Surface& s, int x, int y, Color c, u32 cov) {
    if (!cov) return;
    put(s, x, y, cov == 255 ? c : with_alpha(c, (u8)(alpha_of(c) * cov / 255)));
}

} // namespace

u8 corner_coverage(int radius, int dx, int dy) {
    if (radius <= 0) return 255;
    if (radius > MAX_RADIUS) radius = MAX_RADIUS;
    if (dx < 0 || dy < 0) return 0;
    if (dx >= radius || dy >= radius) return 255;
    return corner_table(radius).cov[dy][dx];
}

void fill_rect_rounded(Surface& s, const Rect& r, int radius, Color c) {
    radius = clamp_radius(r, radius);
    if (radius <= 0) {
        fill_rect(s, r, c);
        return;
    }
    const CornerTable& t = corner_table(radius);
    for (int dy = 0; dy < r.h; dy++) {
        int y = r.y + dy;
        if (y < s.clip.y || y >= s.clip.bottom()) continue;
        if (dy >= radius && dy < r.h - radius) {
            span(s, r.x, r.right(), y, c);
            continue;
        }
        int cy = dy < radius ? dy : r.h - 1 - dy;
        for (int dx = 0; dx < radius; dx++) {
            put_cov(s, r.x + dx, y, c, t.cov[cy][dx]);
            put_cov(s, r.right() - 1 - dx, y, c, t.cov[cy][dx]);
        }
        span(s, r.x + radius, r.right() - radius, y, c);
    }
}

void stroke_rect(Surface& s, const Rect& r, Color c) {
    if (r.empty()) return;
    draw_hline(s, r.x, r.right() - 1, r.y, c);
    draw_hline(s, r.x, r.right() - 1, r.bottom() - 1, c);
    draw_vline(s, r.x, r.y, r.bottom() - 1, c);
    draw_vline(s, r.right() - 1, r.y, r.bottom() - 1, c);
}

// A one-pixel anti-aliased outline: the coverage of the rounded rectangle
// minus that of the same shape inset by one pixel.
void stroke_rect_rounded(Surface& s, const Rect& r, int radius, Color c) {
    radius = clamp_radius(r, radius);
    if (radius <= 1) {
        stroke_rect(s, r, c);
        return;
    }
    const CornerTable& outer = corner_table(radius);
    const CornerTable& inner = corner_table(radius - 1);
    // Straight edges between the corners.
    span(s, r.x + radius, r.right() - radius, r.y, c);
    span(s, r.x + radius, r.right() - radius, r.bottom() - 1, c);
    for (int y = r.y + radius; y < r.bottom() - radius; y++) {
        put(s, r.x, y, c);
        put(s, r.right() - 1, y, c);
    }
    // Corners.
    for (int cy = 0; cy < radius; cy++) {
        for (int cx = 0; cx < radius; cx++) {
            int o = outer.cov[cy][cx];
            int i = (cx >= 1 && cy >= 1) ? inner.cov[cy - 1][cx - 1] : 0;
            int cov = o > i ? o - i : 0;
            if (!cov) continue;
            put_cov(s, r.x + cx, r.y + cy, c, (u32)cov);
            put_cov(s, r.right() - 1 - cx, r.y + cy, c, (u32)cov);
            put_cov(s, r.x + cx, r.bottom() - 1 - cy, c, (u32)cov);
            put_cov(s, r.right() - 1 - cx, r.bottom() - 1 - cy, c, (u32)cov);
        }
    }
}

void blit(Surface& dst, int x, int y, const Surface& src) {
    Rect target = Rect{x, y, src.clip.w, src.clip.h}.intersect(dst.clip);
    if (target.empty()) return;
    int sx0 = src.clip.x + (target.x - x);
    int sy0 = src.clip.y + (target.y - y);
    for (int row = 0; row < target.h; row++) {
        const u32* sp = src.row(sy0 + row) + sx0;
        u32* dp = dst.row(target.y + row) + target.x;
        for (int i = 0; i < target.w; i++) dp[i] = sp[i];
    }
}

void blit_alpha(Surface& dst, int x, int y, const Surface& src, u8 opacity) {
    Rect target = Rect{x, y, src.clip.w, src.clip.h}.intersect(dst.clip);
    if (target.empty()) return;
    int sx0 = src.clip.x + (target.x - x);
    int sy0 = src.clip.y + (target.y - y);
    for (int row = 0; row < target.h; row++) {
        const u32* sp = src.row(sy0 + row) + sx0;
        u32* dp = dst.row(target.y + row) + target.x;
        if (opacity == 255) {
            for (int i = 0; i < target.w; i++) dp[i] = blend(dp[i], sp[i]);
        } else {
            for (int i = 0; i < target.w; i++) {
                u32 a = alpha_of(sp[i]) * opacity / 255;
                dp[i] = blend(dp[i], with_alpha(sp[i], (u8)a));
            }
        }
    }
}

void blit_scaled(Surface& dst, const Rect& dr, const Surface& src) {
    Rect target = dr.intersect(dst.clip);
    if (target.empty() || src.width == 0 || src.height == 0) return;
    for (int row = 0; row < target.h; row++) {
        int sy = (int)((i64)(target.y + row - dr.y) * src.height / dr.h);
        const u32* sp = src.row(sy);
        u32* dp = dst.row(target.y + row) + target.x;
        for (int i = 0; i < target.w; i++) {
            int sx = (int)((i64)(target.x + i - dr.x) * src.width / dr.w);
            dp[i] = blend(dp[i], sp[sx]);
        }
    }
}

void draw_hline(Surface& s, int x0, int x1, int y, Color c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    span(s, x0, x1 + 1, y, c);
}

void draw_vline(Surface& s, int x, int y0, int y1, Color c) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) put(s, x, y, c);
}

void draw_line(Surface& s, int x0, int y0, int x1, int y1, Color c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        put(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void fill_circle(Surface& s, int cx, int cy, int radius, Color c) {
    for (int dy = -radius; dy <= radius; dy++) {
        int v = radius * radius - dy * dy, x = 0;
        while ((x + 1) * (x + 1) <= v) x++;
        span(s, cx - x, cx + x + 1, cy + dy, c);
    }
}

void fill_circle_aa(Surface& s, int cx, int cy, int radius, Color c) {
    // Coverage per pixel from the distance to the circle edge (1px ramp).
    for (int dy = -radius - 1; dy <= radius + 1; dy++) {
        for (int dx = -radius - 1; dx <= radius + 1; dx++) {
            // distance in 1/16 pixel units to avoid floats
            int d2 = dx * dx + dy * dy;
            int r_out = (radius + 1) * (radius + 1);
            int r_in = radius > 0 ? (radius - 1) * (radius - 1) : 0;
            u32 a;
            if (d2 <= r_in) a = 255;
            else if (d2 >= r_out) continue;
            else a = (u32)(255 * (r_out - d2) / (r_out - r_in));
            put(s, cx + dx, cy + dy, with_alpha(c, (u8)(a * alpha_of(c) / 255)));
        }
    }
}

void fill_gradient_vertical(Surface& s, const Rect& r, Color top, Color bottom) {
    Rect rr = r.intersect(s.clip);
    if (rr.empty()) return;
    for (int y = rr.y; y < rr.bottom(); y++) {
        u8 t = r.h > 1 ? (u8)((y - r.y) * 255 / (r.h - 1)) : 0;
        span(s, rr.x, rr.right(), y, mix(top, bottom, t));
    }
}

void fill_gradient_horizontal(Surface& s, const Rect& r, Color left, Color right) {
    Rect rr = r.intersect(s.clip);
    if (rr.empty()) return;
    for (int x = rr.x; x < rr.right(); x++) {
        u8 t = r.w > 1 ? (u8)((x - r.x) * 255 / (r.w - 1)) : 0;
        Color c = mix(left, right, t);
        for (int y = rr.y; y < rr.bottom(); y++) put(s, x, y, c);
    }
}

void fill_gradient_radial(Surface& s, const Rect& r, int cx, int cy, int radius, Color inner, Color outer) {
    Rect rr = r.intersect(s.clip);
    if (rr.empty() || radius <= 0) return;
    i64 r2 = (i64)radius * radius;
    for (int y = rr.y; y < rr.bottom(); y++) {
        u32* p = s.row(y) + rr.x;
        for (int x = rr.x; x < rr.right(); x++, p++) {
            i64 d2 = (i64)(x - cx) * (x - cx) + (i64)(y - cy) * (y - cy);
            u8 t = d2 >= r2 ? 255 : (u8)(d2 * 255 / r2);
            *p = blend(*p, mix(inner, outer, t));
        }
    }
}

void blur_box(Surface& s, int radius, u32* scratch) {
    Rect c = s.clip;
    if (c.empty() || radius <= 0) return;
    int w = c.w, h = c.h;
    // Horizontal pass into scratch (per channel running sums).
    for (int y = 0; y < h; y++) {
        const u32* src = s.row(c.y + y) + c.x;
        u32* dst = scratch + (isize)y * w;
        u32 sa = 0, sr = 0, sg = 0, sb = 0;
        int n = 0;
        auto add = [&](int x) { Color v = src[x]; sa += alpha_of(v); sr += red_of(v); sg += green_of(v); sb += blue_of(v); n++; };
        auto sub = [&](int x) { Color v = src[x]; sa -= alpha_of(v); sr -= red_of(v); sg -= green_of(v); sb -= blue_of(v); n--; };
        for (int x = 0; x < radius && x < w; x++) add(x);
        for (int x = 0; x < w; x++) {
            if (x + radius < w) add(x + radius);
            if (x - radius - 1 >= 0) sub(x - radius - 1);
            dst[x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
        }
    }
    // Vertical pass back into the surface.
    for (int x = 0; x < w; x++) {
        u32 sa = 0, sr = 0, sg = 0, sb = 0;
        int n = 0;
        auto at = [&](int y) -> Color { return scratch[(isize)y * w + x]; };
        auto add = [&](int y) { Color v = at(y); sa += alpha_of(v); sr += red_of(v); sg += green_of(v); sb += blue_of(v); n++; };
        auto sub = [&](int y) { Color v = at(y); sa -= alpha_of(v); sr -= red_of(v); sg -= green_of(v); sb -= blue_of(v); n--; };
        for (int y = 0; y < radius && y < h; y++) add(y);
        for (int y = 0; y < h; y++) {
            if (y + radius < h) add(y + radius);
            if (y - radius - 1 >= 0) sub(y - radius - 1);
            s.row(c.y + y)[c.x + x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
        }
    }
}

namespace {

struct BlobFont {               // 40 bytes, see tools/gen-fonts.py
    char name[16];
    u16 line_height, ascent, first, count, fixed_advance, pad;
    u32 glyphs_offset, coverage_offset, coverage_size;
};

const AAGlyph* aa_glyph(const Font& f, unsigned char ch) {
    if (ch < f.aa_first || ch >= f.aa_first + f.aa_count) ch = '?';
    if (ch < f.aa_first || ch >= f.aa_first + f.aa_count) return nullptr;
    return &f.aa_glyphs[ch - f.aa_first];
}

int draw_text_aa(Surface& s, const Font& f, int x, int y, const char* text, usize n, Color c) {
    int pen = x;
    u32 ca = alpha_of(c);
    for (usize i = 0; i < n && text[i]; i++) {
        const AAGlyph* g = aa_glyph(f, (unsigned char)text[i]);
        if (!g) continue;
        int gx0 = pen + g->xoff, gy0 = y + g->yoff;
        if (g->w && gx0 < s.clip.right() && gx0 + g->w > s.clip.x && gy0 < s.clip.bottom() && gy0 + g->h > s.clip.y) {
            const u8* cov = f.aa_coverage + g->offset;
            for (int row = 0; row < g->h; row++) {
                int py = gy0 + row;
                if (py < s.clip.y || py >= s.clip.bottom()) continue;
                u32* line = s.row(py);
                const u8* src = cov + row * g->w;
                for (int col = 0; col < g->w; col++) {
                    u32 a = src[col];
                    int px = gx0 + col;
                    if (!a || px < s.clip.x || px >= s.clip.right()) continue;
                    a = a * ca / 255;
                    line[px] = blend(line[px], with_alpha(c, (u8)a));
                }
            }
        }
        pen += g->advance;
    }
    return pen - x;
}

usize text_len(const char* text) {
    usize n = 0;
    while (text[n]) n++;
    return n;
}

int measure_n(const Font& f, const char* text, usize n) {
    if (!f.antialiased()) {
        usize len = 0;
        while (len < n && text[len]) len++;
        return (int)len * f.width;
    }
    int w = 0;
    for (usize i = 0; i < n && text[i]; i++) {
        const AAGlyph* g = aa_glyph(f, (unsigned char)text[i]);
        if (g) w += g->advance;
    }
    return w;
}

} // namespace

Font font_from_blob(const u8* blob, usize size, const char* name) {
    Font f;
    if (size < 8 || blob[0] != 'C' || blob[1] != 'F' || blob[2] != 'N' || blob[3] != 'T') return f;
    u32 count;
    __builtin_memcpy(&count, blob + 4, 4);
    if (count > 64 || 8 + (u64)count * sizeof(BlobFont) > size) return f;
    for (u32 i = 0; i < count; i++) {
        BlobFont h;
        __builtin_memcpy(&h, blob + 8 + i * sizeof(BlobFont), sizeof h);
        bool same = true;
        for (int k = 0; k < 16; k++) {
            if (h.name[k] != name[k]) same = false;
            if (!name[k] || !same) break;
        }
        if (!same) continue;
        // Everything the glyph table points at must lie inside the blob.
        if (h.count == 0 || h.first + (u32)h.count > 256 || h.line_height == 0 || h.line_height > 255) return f;
        if ((u64)h.glyphs_offset + (u64)h.count * sizeof(AAGlyph) > size) return f;
        if ((u64)h.coverage_offset + h.coverage_size > size) return f;
        if (h.glyphs_offset % 4) return f;
        const AAGlyph* glyphs = (const AAGlyph*)(blob + h.glyphs_offset);
        for (u32 g = 0; g < h.count; g++)
            if ((u64)glyphs[g].offset + (u64)glyphs[g].w * glyphs[g].h > h.coverage_size) return f;
        f.aa_glyphs = glyphs;
        f.aa_coverage = blob + h.coverage_offset;
        f.aa_coverage_size = h.coverage_size;
        f.aa_first = h.first;
        f.aa_count = h.count;
        f.height = h.line_height;
        f.width = h.fixed_advance;
        if (!f.width) {
            const AAGlyph* zero = aa_glyph(f, '0');
            const AAGlyph* n = aa_glyph(f, 'n');
            f.width = zero && zero->advance ? zero->advance : n ? n->advance : h.line_height / 2;
        }
        return f;
    }
    return f;
}

int draw_text_n(Surface& s, const Font& f, int x, int y, const char* text, usize n, Color c) {
    if (!f.valid()) return 0;
    if (f.antialiased()) return draw_text_aa(s, f, x, y, text, n, c);
    int stride = (f.width + 7) / 8;
    int pen = x;
    for (usize i = 0; i < n && text[i]; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= f.glyph_count) ch = '?';
        const u8* g = f.glyphs + (usize)ch * f.bytes_per_glyph;
        if (pen + f.width > s.clip.x && pen < s.clip.right()) {
            for (int gy = 0; gy < f.height; gy++) {
                int py = y + gy;
                if (py < s.clip.y || py >= s.clip.bottom()) continue;
                const u8* line = g + gy * stride;
                for (int gx = 0; gx < f.width; gx++) {
                    if (line[gx / 8] & (0x80 >> (gx % 8))) put(s, pen + gx, py, c);
                }
            }
        }
        pen += f.width;
    }
    return pen - x;
}

int draw_text(Surface& s, const Font& f, int x, int y, const char* text, Color c) {
    return draw_text_n(s, f, x, y, text, (usize)-1, c);
}

int measure_text(const Font& f, const char* text) { return measure_n(f, text, (usize)-1); }

int draw_text_ellipsis(Surface& s, const Font& f, int x, int y, const char* text, int max_width, Color c) {
    if (measure_text(f, text) <= max_width) return draw_text(s, f, x, y, text, c);
    // The longest prefix that fits together with "...".
    int dots = measure_text(f, "...");
    usize len = text_len(text), fit = 0;
    while (fit < len && measure_n(f, text, fit + 1) + dots <= max_width) fit++;
    int adv = draw_text_n(s, f, x, y, text, fit, c);
    adv += draw_text(s, f, x + adv, y, "...", c);
    return adv;
}

// A line as the set of pixels within width/2 of the segment, with a one
// pixel soft edge. Distances in 1/16 pixel, integer only.
void draw_line_aa(Surface& s, int x0, int y0, int x1, int y1, int width16, Color c) {
    int minx = (x0 < x1 ? x0 : x1) - 2, maxx = (x0 > x1 ? x0 : x1) + 2;
    int miny = (y0 < y1 ? y0 : y1) - 2, maxy = (y0 > y1 ? y0 : y1) + 2;
    i64 dx = (i64)(x1 - x0) * 16, dy = (i64)(y1 - y0) * 16;
    i64 len2 = dx * dx + dy * dy;
    i64 inner = width16 / 2, outer = inner + 16;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            i64 vx = (i64)(px - x0) * 16, vy = (i64)(py - y0) * 16;
            // Closest point on the segment.
            i64 t = len2 ? vx * dx + vy * dy : 0;
            if (t < 0) t = 0;
            if (t > len2) t = len2;
            i64 cx = len2 ? dx * t / len2 : 0, cy = len2 ? dy * t / len2 : 0;
            i64 ex = vx - cx, ey = vy - cy;
            i64 d2 = ex * ex + ey * ey;
            if (d2 >= outer * outer) continue;
            u32 cov = 255;
            if (d2 > inner * inner) {
                i64 d = inner;
                while ((d + 1) * (d + 1) <= d2) d++;
                cov = (u32)(255 * (outer - d) / 16);
            }
            put_cov(s, px, py, c, cov);
        }
    }
}

void draw_char_scaled(Surface& s, const Font& f, int x, int y, char ch, int scale, Color c) {
    if (!f.valid()) return;
    unsigned char uc = (unsigned char)ch;
    if (uc >= f.glyph_count) uc = '?';
    int stride = (f.width + 7) / 8;
    const u8* g = f.glyphs + (usize)uc * f.bytes_per_glyph;
    for (int gy = 0; gy < f.height; gy++) {
        const u8* line = g + gy * stride;
        for (int gx = 0; gx < f.width; gx++) {
            if (line[gx / 8] & (0x80 >> (gx % 8)))
                fill_rect(s, {x + gx * scale, y + gy * scale, scale, scale}, c);
        }
    }
}

} // namespace gfx
