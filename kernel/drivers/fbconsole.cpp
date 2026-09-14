// Text console with a shadow buffer of character cells and deferred drawing.
//
// All drawing goes forward into the framebuffer; nothing is ever read back
// from it, because framebuffer memory is uncached or write-combined and
// reading it is orders of magnitude slower than writing.
//
// Output is batched: characters update the cell buffer and mark rows dirty;
// fbconsole_flush() paints the dirty rows. A newline flushes at most every
// FLUSH_INTERVAL_TICKS so a burst of lines (a scrolling listing) costs one
// full redraw per interval instead of one per line. Callers that are about
// to block (the shell) or halt (panic) flush explicitly.
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/lapic.h>
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

// Enough cells for a 4K display at 8x16 (480x135); the extra keeps it round.
constexpr u32 MAX_COLS = 512;
constexpr u32 MAX_ROWS = 256;
constexpr u64 FLUSH_INTERVAL_TICKS = 2;     // 20 ms at 100 Hz, ~50 fps for bursts

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
    u32 cursor_col = 0, cursor_row = 0;    // where the cursor is currently painted
    bool all_dirty = false;
    u64 last_flush_tick = 0;
} g;

u8 g_cells[MAX_ROWS][MAX_COLS];
bool g_dirty[MAX_ROWS];

inline u32* pixel(u32 x, u32 y) { return (u32*)(g.fb + (u64)y * g.pitch + (u64)x * 4); }

void render_cell(u32 col, u32 row) {
    unsigned char c = g_cells[row][col];
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
            out[x] = on ? g.fg : g.bg;
        }
    }
}

void render_row(u32 row) {
    for (u32 col = 0; col < g.cols; col++) render_cell(col, row);
}

void paint_cursor(u32 col, u32 row) {
    u32 px = col * g.font->width;
    u32 py = row * g.font->height;
    // The cursor is the bottom two pixel rows of the cell in the foreground colour.
    for (u32 y = g.font->height - 2; y < g.font->height; y++) {
        u32* out = pixel(px, py + y);
        for (u32 x = 0; x < g.font->width; x++) out[x] = g.fg;
    }
}

void scroll() {
    memmove(g_cells[0], g_cells[1], (usize)(g.rows - 1) * MAX_COLS);
    memset(g_cells[g.rows - 1], ' ', MAX_COLS);
    g.all_dirty = true;
}

void maybe_flush() {
    u64 now = lapic_timer_ticks();
    // Before the timer runs (ticks stay 0) every line is painted immediately.
    if (now == 0 || now - g.last_flush_tick >= FLUSH_INTERVAL_TICKS) fbconsole_flush();
}

void newline() {
    g.cx = 0;
    if (++g.cy >= g.rows) {
        scroll();
        g.cy = g.rows - 1;
    }
    maybe_flush();
}

void put_cell(u32 col, u32 row, char c) {
    g_cells[row][col] = (u8)c;
    g_dirty[row] = true;
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
    g.cols = min(g.width / font->width, MAX_COLS);
    g.rows = min(g.height / font->height, MAX_ROWS);
    g.ready = true;
    fbconsole_clear();
    return true;
}

bool fbconsole_ready() { return g.ready; }

void fbconsole_disable() { g.ready = false; }

void fbconsole_enable() {
    if (!g.fb) return;
    g.ready = true;
    g.last_flush_tick = 0;
    fbconsole_clear();
}

void fbconsole_set_colour(u32 fg, u32 bg) {
    g.fg = fg;
    g.bg = bg;
    g.all_dirty = true;
}

void fbconsole_clear() {
    if (!g.ready) return;
    memset(g_cells, ' ', sizeof g_cells);
    for (u32 y = 0; y < g.height; y++) {
        u32* out = pixel(0, y);
        for (u32 x = 0; x < g.width; x++) out[x] = g.bg;
    }
    g.cx = g.cy = 0;
    g.cursor_drawn = false;
    g.all_dirty = false;
    memset(g_dirty, 0, sizeof g_dirty);
    fbconsole_flush();
}

void fbconsole_flush() {
    if (!g.ready) return;
    // Erase the old cursor unless its row is being repainted anyway.
    if (g.cursor_drawn && !g.all_dirty && !g_dirty[g.cursor_row]) render_cell(g.cursor_col, g.cursor_row);
    if (g.all_dirty) {
        for (u32 row = 0; row < g.rows; row++) render_row(row);
        memset(g_dirty, 0, sizeof g_dirty);
        g.all_dirty = false;
    } else {
        for (u32 row = 0; row < g.rows; row++) {
            if (g_dirty[row]) {
                render_row(row);
                g_dirty[row] = false;
            }
        }
    }
    paint_cursor(g.cx, g.cy);
    g.cursor_drawn = true;
    g.cursor_col = g.cx;
    g.cursor_row = g.cy;
    g.last_flush_tick = lapic_timer_ticks();
}

void fbconsole_putc(char c) {
    if (!g.ready) return;
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
            put_cell(g.cx, g.cy, ' ');
        }
        break;
    default:
        put_cell(g.cx, g.cy, c);
        if (++g.cx >= g.cols) newline();
        break;
    }
}

u32 fbconsole_columns() { return g.cols; }
u32 fbconsole_rows() { return g.rows; }
