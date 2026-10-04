// Terminal widget for the desktop: a character grid the kernel shell writes
// into, painted with the 8x16 font. Handles newline, carriage return,
// backspace, tab, a block cursor, and a scrollback history the user can
// scroll through. No escape sequences yet (Ember, the real terminal, arrives
// in phase 13).
#pragma once

#include <gfx/gfx.h>
#include <lib/types.h>

class Terminal {
public:
    // cells must hold at least max_cols * history_lines bytes: the history is
    // a ring of lines, each max_cols wide, and the visible screen is its end.
    void init(u8* cells, int max_cols, int history_lines, const gfx::Font& font);
    // Changes the visible grid (clamped to the storage). Text is kept.
    void resize(int cols, int rows);
    void putc(char c);
    // Scrolls the view back (lines > 0) or forward (lines < 0) through the
    // history; 0 lines back is the live screen.
    void scroll(int lines);
    void scroll_to_bottom() { scroll(-scroll_); }
    bool scrolled_back() const { return scroll_ > 0; }
    int scroll_offset() const { return scroll_; }
    int scrollable() const { return max_scroll(); }     // lines of history above the live screen
    // Paints dirty rows into `s` (content surface); returns the union of the
    // repainted area, empty if nothing changed. `focused` draws the cursor solid.
    gfx::Rect paint(gfx::Surface& s, bool focused, bool force);
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cell_w() const { return font_.width; }
    int cell_h() const { return font_.height; }

private:
    u8* line(u64 n) const { return cells_ + (usize)(n % (u64)hist_) * max_cols_; }
    void newline();
    void mark(int row) { if (row >= 0 && row < rows_) dirty_[row] = true; }
    int max_scroll() const;

    u8* cells_ = nullptr;
    int max_cols_ = 0, hist_ = 0;
    int cols_ = 0, rows_ = 0;
    u64 top_ = 0;               // history line shown in screen row 0 of the live screen
    u64 lines_ = 1;             // lines written so far (the ring keeps the last hist_)
    int cx_ = 0, cy_ = 0;       // cursor, relative to top_
    int scroll_ = 0;            // lines scrolled back from the live screen
    gfx::Font font_;
    bool dirty_[256];
    bool all_dirty_ = true;
    int drawn_cursor_x_ = -1, drawn_cursor_y_ = -1;
};
