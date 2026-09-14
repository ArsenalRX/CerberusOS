// Renders 8-wide PSF2 glyphs straight into the linear framebuffer. Scrolling is a
// memmove of the pixel rows; fine for QEMU and for a boot console.
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/serial.h>
#include <lib/string.h>

extern "C" const u8 _binary_font_8x16_start[];
extern "C" const u8 _binary_font_8x16_end[];

namespace {

struct Psf2Header {
    u32 magic;
    u32 version;
    u32 header_size;
    u32 flags;
    u32 glyph_count;
    u32 bytes_per_glyph;
    u32 height;
    u32 width;
};
constexpr u32 PSF2_MAGIC = 0x864AB572;

struct State {
    bool ready = false;
    u8* fb = nullptr;
    u32 width = 0, height = 0, pitch = 0;
    const Psf2Header* font = nullptr;
    const u8* glyphs = nullptr;
    u32 cols = 0, rows = 0;
    u32 cx = 0, cy = 0;
    u32 fg = 0xFFD0D0D0, bg = 0xFF101418;
    bool cursor_drawn = false;
} g;

inline u32* pixel(u32 x, u32 y) { return (u32*)(g.fb + (u64)y * g.pitch + (u64)x * 4); }

void draw_glyph(u32 col, u32 row, unsigned char c, u32 fg, u32 bg) {
    if (c >= g.font->glyph_count) c = '?';
    const u8* glyph = g.glyphs + (u64)c * g.font->bytes_per_glyph;
    u32 stride = (g.font->width + 7) / 8;
    u32 px = col * g.font->width;
    u32 py = row * g.font->height;
    for (u32 y = 0; y < g.font->height; y++) {
        const u8* line = glyph + y * stride;
        u32* out = pixel(px, py + y);
        for (u32 x = 0; x < g.font->width; x++) {
            bool on = line[x / 8] & (0x80 >> (x % 8));
            out[x] = on ? fg : bg;
        }
    }
}

void draw_cursor(bool on) {
    if (on == g.cursor_drawn) return;
    g.cursor_drawn = on;
    u32 px = g.cx * g.font->width;
    u32 py = g.cy * g.font->height;
    // The cursor is the bottom two pixel rows of the cell in the foreground colour.
    for (u32 y = g.font->height - 2; y < g.font->height; y++) {
        u32* out = pixel(px, py + y);
        for (u32 x = 0; x < g.font->width; x++) out[x] = on ? g.fg : g.bg;
    }
}

void scroll() {
    u64 row_bytes = (u64)g.font->height * g.pitch;
    u64 visible = (u64)g.rows * row_bytes;
    memmove(g.fb, g.fb + row_bytes, visible - row_bytes);
    for (u32 y = (g.rows - 1) * g.font->height; y < g.rows * g.font->height; y++) {
        u32* out = pixel(0, y);
        for (u32 x = 0; x < g.width; x++) out[x] = g.bg;
    }
}

void newline() {
    g.cx = 0;
    if (++g.cy >= g.rows) {
        scroll();
        g.cy = g.rows - 1;
    }
}

} // namespace

bool fbconsole_init(const FramebufferInfo& fb) {
    if (!fb.present) {
        serial_write("fbconsole: no framebuffer\n", 26);
        return false;
    }
    if (fb.bpp != 32) {
        serial_write("fbconsole: framebuffer is not 32bpp, refusing\n", 47);
        return false;
    }
    const Psf2Header* font = (const Psf2Header*)_binary_font_8x16_start;
    if (font->magic != PSF2_MAGIC || font->width != 8) {
        serial_write("fbconsole: embedded font is not PSF2/8-wide\n", 44);
        return false;
    }
    g.fb = (u8*)fb.address;
    g.width = fb.width;
    g.height = fb.height;
    g.pitch = fb.pitch;
    g.font = font;
    g.glyphs = _binary_font_8x16_start + font->header_size;
    g.cols = g.width / font->width;
    g.rows = g.height / font->height;
    g.cx = g.cy = 0;
    g.cursor_drawn = false;
    g.ready = true;
    fbconsole_clear();
    return true;
}

bool fbconsole_ready() { return g.ready; }

void fbconsole_set_colour(u32 fg, u32 bg) {
    g.fg = fg;
    g.bg = bg;
}

void fbconsole_clear() {
    if (!g.ready) return;
    for (u32 y = 0; y < g.height; y++) {
        u32* out = pixel(0, y);
        for (u32 x = 0; x < g.width; x++) out[x] = g.bg;
    }
    g.cx = g.cy = 0;
    g.cursor_drawn = false;
    draw_cursor(true);
}

void fbconsole_putc(char c) {
    if (!g.ready) return;
    draw_cursor(false);
    switch (c) {
    case '\n': newline(); break;
    case '\r': g.cx = 0; break;
    case '\t':
        g.cx = (g.cx + 8) & ~7u;
        if (g.cx >= g.cols) newline();
        break;
    case '\b':
        if (g.cx) {
            g.cx--;
            draw_glyph(g.cx, g.cy, ' ', g.fg, g.bg);
        }
        break;
    default:
        draw_glyph(g.cx, g.cy, (unsigned char)c, g.fg, g.bg);
        if (++g.cx >= g.cols) newline();
        break;
    }
    draw_cursor(true);
}

u32 fbconsole_columns() { return g.cols; }
u32 fbconsole_rows() { return g.rows; }
