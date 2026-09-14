// libgfx: the software renderer shared by the compositor and the toolkit
// (SPEC §8.2). Freestanding: no kernel headers beyond lib/types.h, so it can
// move to userland unchanged. Pixels are 32-bit 0xAARRGGBB, straight alpha;
// blending is source-over.
#pragma once

#include <lib/types.h>

namespace gfx {

using Color = u32;

constexpr Color rgb(u8 r, u8 g, u8 b) { return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | b; }
constexpr Color rgba(u8 r, u8 g, u8 b, u8 a) { return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b; }
constexpr u8 alpha_of(Color c) { return (u8)(c >> 24); }
constexpr u8 red_of(Color c) { return (u8)(c >> 16); }
constexpr u8 green_of(Color c) { return (u8)(c >> 8); }
constexpr u8 blue_of(Color c) { return (u8)c; }
constexpr Color with_alpha(Color c, u8 a) { return (c & 0x00FFFFFFu) | ((u32)a << 24); }
// Linear interpolation between two colours, t in 0..255.
Color mix(Color a, Color b, u8 t);

struct Point {
    int x, y;
};

struct Rect {
    int x, y, w, h;
    constexpr int right() const { return x + w; }     // exclusive
    constexpr int bottom() const { return y + h; }    // exclusive
    constexpr bool empty() const { return w <= 0 || h <= 0; }
    constexpr bool contains(int px, int py) const { return px >= x && py >= y && px < right() && py < bottom(); }
    Rect intersect(const Rect& o) const;
    Rect unite(const Rect& o) const;                 // bounding box
    bool overlaps(const Rect& o) const { return !intersect(o).empty(); }
    constexpr Rect translated(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
    constexpr Rect inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

// A pixel buffer with a clip rectangle. Never owns its memory.
struct Surface {
    u32* pixels = nullptr;
    int width = 0, height = 0;
    int stride = 0;          // in pixels
    Rect clip{0, 0, 0, 0};

    Surface() = default;
    Surface(u32* px, int w, int h, int stride_px) : pixels(px), width(w), height(h), stride(stride_px), clip{0, 0, w, h} {}
    u32* row(int y) const { return pixels + (isize)y * stride; }
    Rect bounds() const { return {0, 0, width, height}; }
    // A view onto a sub-rectangle (clip is intersected with the parent's).
    Surface sub(const Rect& r) const;
};

// Clip stack: push intersects with the current clip; pop restores.
struct ClipStack {
    Rect stack[16];
    int depth = 0;
    void push(Surface& s, const Rect& r);
    void pop(Surface& s);
};

// Bitmap font from a PSF2 blob (fixed advance, no kerning).
struct Font {
    const u8* glyphs = nullptr;
    u32 glyph_count = 0;
    u32 bytes_per_glyph = 0;
    int width = 0, height = 0;
    bool valid() const { return glyphs != nullptr; }
};
// Parses a PSF2 blob; returns an invalid Font on a bad magic.
Font font_from_psf2(const u8* blob);

// Horizontal inset of a rounded corner of the given radius at row dy
// (0 = the outermost row). Lets callers mask rectangles to rounded shapes.
int corner_inset(int radius, int dy);

// ---- primitives (all clipped to surface.clip) ----
void fill_rect(Surface& s, const Rect& r, Color c);
void fill_rect_rounded(Surface& s, const Rect& r, int radius, Color c);
void stroke_rect(Surface& s, const Rect& r, Color c);
void stroke_rect_rounded(Surface& s, const Rect& r, int radius, Color c);
// Copies src (its whole clip) to (x, y) on dst, no blending.
void blit(Surface& dst, int x, int y, const Surface& src);
// Source-over blend of src onto dst at (x, y), optionally scaled by opacity.
void blit_alpha(Surface& dst, int x, int y, const Surface& src, u8 opacity = 255);
// Nearest-neighbour scaled copy of src into dst_rect.
void blit_scaled(Surface& dst, const Rect& dst_rect, const Surface& src);
void draw_line(Surface& s, int x0, int y0, int x1, int y1, Color c);
void draw_hline(Surface& s, int x0, int x1, int y, Color c);
void draw_vline(Surface& s, int x, int y0, int y1, Color c);
void fill_circle(Surface& s, int cx, int cy, int radius, Color c);
void fill_circle_aa(Surface& s, int cx, int cy, int radius, Color c);
void fill_gradient_vertical(Surface& s, const Rect& r, Color top, Color bottom);
void fill_gradient_horizontal(Surface& s, const Rect& r, Color left, Color right);
void fill_gradient_radial(Surface& s, const Rect& r, int cx, int cy, int radius, Color inner, Color outer);
// Separable box blur of the surface's clip region, in place. Needs a
// scratch buffer of at least clip.w * clip.h pixels.
void blur_box(Surface& s, int radius, u32* scratch);

// Text (single line). Returns the advance in pixels.
int draw_text(Surface& s, const Font& f, int x, int y, const char* text, Color c);
int draw_text_n(Surface& s, const Font& f, int x, int y, const char* text, usize n, Color c);
int measure_text(const Font& f, const char* text);
// Draws text truncated with "..." to fit max_width.
int draw_text_ellipsis(Surface& s, const Font& f, int x, int y, const char* text, int max_width, Color c);
// Draws a single glyph at 2x (nearest neighbour) for large titles.
void draw_char_scaled(Surface& s, const Font& f, int x, int y, char ch, int scale, Color c);

} // namespace gfx
