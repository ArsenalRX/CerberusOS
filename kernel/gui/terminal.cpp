// Cell grid with per-row dirty flags; painting redraws only dirty rows so a
// blinking-free cursor and single-character echo cost one row each.
#include <gui/terminal.h>
#include <lib/string.h>

namespace {
constexpr gfx::Color TERM_BG = gfx::rgb(16, 18, 26);
constexpr gfx::Color TERM_FG = gfx::rgb(220, 224, 232);
constexpr gfx::Color TERM_CURSOR = gfx::rgb(96, 165, 250);
constexpr gfx::Color TERM_CURSOR_DIM = gfx::rgb(70, 76, 96);
constexpr int PAD = 6;
} // namespace

void Terminal::init(u8* cells, int max_cols, int max_rows, const gfx::Font& font) {
    cells_ = cells;
    max_cols_ = max_cols;
    max_rows_ = max_rows > 256 ? 256 : max_rows;
    font_ = font;
    resize(max_cols, max_rows_);
}

void Terminal::resize(int cols, int rows) {
    cols_ = cols < max_cols_ ? cols : max_cols_;
    rows_ = rows < max_rows_ ? rows : max_rows_;
    if (cols_ < 1) cols_ = 1;
    if (rows_ < 1) rows_ = 1;
    memset(cells_, ' ', (usize)max_cols_ * max_rows_);
    cx_ = cy_ = 0;
    all_dirty_ = true;
    drawn_cursor_x_ = drawn_cursor_y_ = -1;
}

void Terminal::newline() {
    cx_ = 0;
    if (++cy_ >= rows_) {
        memmove(cells_, cells_ + max_cols_, (usize)(rows_ - 1) * max_cols_);
        memset(cells_ + (usize)(rows_ - 1) * max_cols_, ' ', max_cols_);
        cy_ = rows_ - 1;
        all_dirty_ = true;
    }
}

void Terminal::putc(char c) {
    switch (c) {
    case '\n': mark(cy_); newline(); break;
    case '\r': cx_ = 0; break;
    case '\t':
        cx_ = (cx_ + 8) & ~7;
        if (cx_ >= cols_) newline();
        break;
    case '\b':
        if (cx_ > 0) {
            cx_--;
            cells_[cy_ * max_cols_ + cx_] = ' ';
            mark(cy_);
        }
        break;
    default:
        if ((unsigned char)c < 0x20) break;
        cells_[cy_ * max_cols_ + cx_] = (u8)c;
        mark(cy_);
        if (++cx_ >= cols_) newline();
        break;
    }
}

gfx::Rect Terminal::paint(gfx::Surface& s, bool focused, bool force) {
    gfx::Rect damage{0, 0, 0, 0};
    bool all = all_dirty_ || force;
    if (all) gfx::fill_rect(s, s.bounds(), TERM_BG);
    // The row the cursor was drawn on must be repainted to erase it.
    if (drawn_cursor_y_ >= 0 && drawn_cursor_y_ < rows_ && (drawn_cursor_x_ != cx_ || drawn_cursor_y_ != cy_))
        dirty_[drawn_cursor_y_] = true;
    dirty_[cy_] = true;

    for (int row = 0; row < rows_; row++) {
        if (!all && !dirty_[row]) continue;
        dirty_[row] = false;
        gfx::Rect r{PAD, PAD + row * font_.height, cols_ * font_.width, font_.height};
        if (!all) gfx::fill_rect(s, {0, r.y, s.width, r.h}, TERM_BG);
        gfx::draw_text_n(s, font_, r.x, r.y, (const char*)(cells_ + row * max_cols_), (usize)cols_, TERM_FG);
        gfx::Rect full_row{0, r.y, s.width, r.h};
        damage = damage.unite(full_row);
    }
    // Cursor
    gfx::Rect cur{PAD + cx_ * font_.width, PAD + cy_ * font_.height, font_.width, font_.height};
    if (focused) gfx::fill_rect(s, cur, TERM_CURSOR);
    else gfx::stroke_rect(s, cur, TERM_CURSOR_DIM);
    // Draw the character under the cursor in inverse when solid.
    if (focused)
        gfx::draw_text_n(s, font_, cur.x, cur.y, (const char*)(cells_ + cy_ * max_cols_ + cx_), 1, TERM_BG);
    damage = damage.unite(cur);
    drawn_cursor_x_ = cx_;
    drawn_cursor_y_ = cy_;
    all_dirty_ = false;
    if (all) damage = s.bounds();
    return damage;
}
