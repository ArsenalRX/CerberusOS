// Terminal widget for the desktop: a character grid the kernel shell writes
// into, painted with the 8x16 font. Handles newline, carriage return,
// backspace, tab, scrolling, and a block cursor. No escape sequences yet
// (Ember, the real terminal, arrives in phase 13).
#pragma once

#include <gfx/gfx.h>
#include <lib/types.h>

class Terminal {
public:
    // cells must hold at least max_cols * max_rows bytes.
    void init(u8* cells, int max_cols, int max_rows, const gfx::Font& font);
    // Resizes the visible grid (clamped to the cell storage); clears.
    void resize(int cols, int rows);
    void putc(char c);
    // Paints dirty rows into `s` (content surface); returns the union of the
    // repainted area, empty if nothing changed. `focused` draws the cursor solid.
    gfx::Rect paint(gfx::Surface& s, bool focused, bool force);
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cell_w() const { return font_.width; }
    int cell_h() const { return font_.height; }

private:
    void newline();
    void mark(int row) { if (row >= 0 && row < rows_) dirty_[row] = true; }

    u8* cells_ = nullptr;
    int max_cols_ = 0, max_rows_ = 0;
    int cols_ = 0, rows_ = 0;
    int cx_ = 0, cy_ = 0;
    gfx::Font font_;
    bool dirty_[256];
    bool all_dirty_ = true;
    int drawn_cursor_x_ = -1, drawn_cursor_y_ = -1;
};
