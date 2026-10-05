// Cell grid with per-row dirty flags; painting redraws only dirty rows so a
// blinking-free cursor and single-character echo cost one row each.
//
// Lines live in a ring of `hist_` lines. The live screen is the last `rows_`
// lines (screen row 0 = history line top_); scrolling back shows earlier
// lines from the ring, and a thin scrollbar on the right shows where the view
// is. Resizing changes only how much of the ring is shown, so text survives.
#include <gui/terminal.h>
#include <lib/string.h>

namespace {
constexpr gfx::Color TERM_BG = gfx::rgb(16, 18, 26);
constexpr gfx::Color TERM_FG = gfx::rgb(220, 224, 232);
constexpr gfx::Color TERM_CURSOR = gfx::rgb(96, 165, 250);
constexpr gfx::Color TERM_CURSOR_DIM = gfx::rgb(70, 76, 96);
constexpr gfx::Color SCROLL_TRACK = gfx::rgba(255, 255, 255, 14);
constexpr gfx::Color SCROLL_THUMB = gfx::rgba(255, 255, 255, 70);
constexpr gfx::Color SCROLL_THUMB_BACK = gfx::rgba(96, 165, 250, 170);
constexpr gfx::Color TERM_SEL = gfx::rgba(96, 165, 250, 110);
constexpr int PAD = 6;
constexpr int BAR_W = 4;
} // namespace

// ------------------------------------------------------------ selection --
Terminal::Pos Terminal::cell_at(int px, int py) const {
    int col = (px - PAD) / font_.width, row = (py - PAD) / font_.height;
    if (col < 0) col = 0;
    if (col > cols_) col = cols_;
    if (row < 0) row = 0;
    if (row >= rows_) row = rows_ - 1;
    return Pos{top_ - (u64)scroll_ + (u64)row, col};
}

void Terminal::mark_selection_rows() {
    u64 first = top_ - (u64)scroll_;
    for (int row = 0; row < rows_; row++) {
        u64 n = first + (u64)row;
        const Pos& lo = sel_a_ < sel_b_ ? sel_a_ : sel_b_;
        const Pos& hi = sel_a_ < sel_b_ ? sel_b_ : sel_a_;
        if (n >= lo.line && n <= hi.line) mark(row);
    }
}

void Terminal::select_begin(int px, int py) {
    if (sel_active_) mark_selection_rows();
    sel_active_ = true;
    sel_a_ = sel_b_ = cell_at(px, py);
}

void Terminal::select_extend(int px, int py) {
    if (!sel_active_) return;
    mark_selection_rows();
    sel_b_ = cell_at(px, py);
    mark_selection_rows();
}

void Terminal::select_clear() {
    if (!sel_active_) return;
    mark_selection_rows();
    sel_active_ = false;
}

bool Terminal::selected(u64 n, int col) const {
    if (!has_selection()) return false;
    const Pos& lo = sel_a_ < sel_b_ ? sel_a_ : sel_b_;
    const Pos& hi = sel_a_ < sel_b_ ? sel_b_ : sel_a_;
    if (n < lo.line || n > hi.line) return false;
    if (n == lo.line && col < lo.col) return false;
    if (n == hi.line && col >= hi.col) return false;
    return true;
}

usize Terminal::selection_text(char* out, usize max) const {
    usize len = 0;
    if (!has_selection() || max == 0) {
        if (max) out[0] = 0;
        return 0;
    }
    const Pos& lo = sel_a_ < sel_b_ ? sel_a_ : sel_b_;
    const Pos& hi = sel_a_ < sel_b_ ? sel_b_ : sel_a_;
    u64 kept = lines_ < (u64)hist_ ? lines_ : (u64)hist_;
    u64 oldest = lines_ - kept;
    for (u64 n = lo.line; n <= hi.line && n < lines_; n++) {
        if (n < oldest) continue;
        const u8* l = line(n);
        int from = n == lo.line ? lo.col : 0, to = n == hi.line ? hi.col : cols_;
        if (to > cols_) to = cols_;
        while (to > from && l[to - 1] == ' ') to--;        // trailing spaces are padding
        for (int c = from; c < to && len + 1 < max; c++) out[len++] = (char)l[c];
        if (n != hi.line && len + 1 < max) out[len++] = '\n';
    }
    out[len] = 0;
    return len;
}

void Terminal::init(u8* cells, int max_cols, int history_lines, const gfx::Font& font) {
    cells_ = cells;
    max_cols_ = max_cols;
    hist_ = history_lines;
    font_ = font;
    memset(cells_, ' ', (usize)max_cols_ * hist_);
    top_ = 0;
    lines_ = 1;
    cx_ = cy_ = 0;
    scroll_ = 0;
    cols_ = rows_ = 0;
    resize(max_cols, 24);
}

int Terminal::max_scroll() const {
    // Lines above the live screen that the ring still holds.
    u64 kept = lines_ < (u64)hist_ ? lines_ : (u64)hist_;
    return (int)(top_ - (lines_ - kept));     // never negative: the ring holds more lines than the screen
}

void Terminal::resize(int cols, int rows) {
    int max_rows = hist_ < 256 ? hist_ : 256;
    cols = cols < max_cols_ ? cols : max_cols_;
    rows = rows < max_rows ? rows : max_rows;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    // Keep the cursor on screen: a shorter screen starts further down.
    if (cy_ >= rows) {
        top_ += (u64)(cy_ - rows + 1);
        cy_ = rows - 1;
    }
    // A taller screen pulls lines back down from the history, so the text
    // stays at the bottom instead of leaving empty rows under it.
    u64 kept = lines_ < (u64)hist_ ? lines_ : (u64)hist_;
    u64 oldest = lines_ - kept;
    u64 want = lines_ > (u64)rows ? lines_ - (u64)rows : 0;
    if (want < oldest) want = oldest;
    if (want < top_) {
        cy_ += (int)(top_ - want);
        top_ = want;
    }
    if (cx_ >= cols) cx_ = cols - 1;
    cols_ = cols;
    rows_ = rows;
    if (scroll_ > max_scroll()) scroll_ = max_scroll();
    all_dirty_ = true;
    drawn_cursor_x_ = drawn_cursor_y_ = -1;
}

void Terminal::newline() {
    cx_ = 0;
    if (cy_ + 1 < rows_) {
        cy_++;
        u64 n = top_ + (u64)cy_;
        if (n >= lines_) {
            lines_ = n + 1;
            memset(line(n), ' ', max_cols_);
        }
        return;
    }
    // At the bottom: the screen moves down one line in the history.
    top_++;
    u64 n = top_ + (u64)cy_;
    lines_ = n + 1;
    memset(line(n), ' ', max_cols_);
    // A view scrolled back stays on the same text while output continues.
    if (scroll_ > 0 && scroll_ < max_scroll()) scroll_++;
    all_dirty_ = true;
}

void Terminal::putc(char c) {
    u8* cur = line(top_ + (u64)cy_);
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
            cur[cx_] = ' ';
            mark(cy_);
        }
        break;
    default:
        if ((unsigned char)c < 0x20) break;
        cur[cx_] = (u8)c;
        mark(cy_);
        if (++cx_ >= cols_) newline();
        break;
    }
}

void Terminal::scroll(int lines) {
    int s = scroll_ + lines;
    int max = max_scroll();
    if (s > max) s = max;
    if (s < 0) s = 0;
    if (s == scroll_) return;
    scroll_ = s;
    all_dirty_ = true;
}

gfx::Rect Terminal::paint(gfx::Surface& s, bool focused, bool force) {
    gfx::Rect damage{0, 0, 0, 0};
    bool all = all_dirty_ || force;
    if (all) gfx::fill_rect(s, s.bounds(), TERM_BG);
    // The row the cursor was drawn on must be repainted to erase it.
    if (drawn_cursor_y_ >= 0 && drawn_cursor_y_ < rows_ && (drawn_cursor_x_ != cx_ || drawn_cursor_y_ != cy_))
        dirty_[drawn_cursor_y_] = true;
    dirty_[cy_] = true;

    u64 first = top_ - (u64)scroll_;
    int text_w = s.width - BAR_W - 4;       // keep text clear of the scrollbar
    for (int row = 0; row < rows_; row++) {
        if (!all && !dirty_[row]) continue;
        dirty_[row] = false;
        gfx::Rect r{PAD, PAD + row * font_.height, cols_ * font_.width, font_.height};
        if (!all) gfx::fill_rect(s, {0, r.y, text_w, r.h}, TERM_BG);
        u64 n = first + (u64)row;
        if (n < lines_) {
            // The selected cells get a tinted background under the text.
            if (has_selection()) {
                int from = -1;
                for (int c = 0; c <= cols_; c++) {
                    bool in = c < cols_ && selected(n, c);
                    if (in && from < 0) from = c;
                    if (!in && from >= 0) {
                        gfx::fill_rect(s, {r.x + from * font_.width, r.y, (c - from) * font_.width, r.h}, TERM_SEL);
                        from = -1;
                    }
                }
            }
            gfx::draw_text_n(s, font_, r.x, r.y, (const char*)line(n), (usize)cols_, TERM_FG);
        }
        damage = damage.unite({0, r.y, text_w, r.h});
    }
    // Cursor: only on the live screen.
    if (scroll_ == 0) {
        gfx::Rect cur{PAD + cx_ * font_.width, PAD + cy_ * font_.height, font_.width, font_.height};
        if (focused) {
            gfx::fill_rect(s, cur, TERM_CURSOR);
            gfx::draw_text_n(s, font_, cur.x, cur.y, (const char*)(line(top_ + (u64)cy_) + cx_), 1, TERM_BG);
        } else {
            gfx::stroke_rect(s, cur, TERM_CURSOR_DIM);
        }
        damage = damage.unite(cur);
        drawn_cursor_x_ = cx_;
        drawn_cursor_y_ = cy_;
    } else {
        drawn_cursor_x_ = drawn_cursor_y_ = -1;
    }
    // Scrollbar, once there is anything to scroll to.
    int max = max_scroll();
    if (all && max > 0) {
        gfx::Rect track{s.width - BAR_W - 3, PAD, BAR_W, s.height - 2 * PAD};
        gfx::fill_rect_rounded(s, track, BAR_W / 2, SCROLL_TRACK);
        int total = max + rows_;
        int th = track.h * rows_ / total;
        if (th < 16) th = 16;
        int ty = track.y + (track.h - th) * (max - scroll_) / max;
        gfx::fill_rect_rounded(s, {track.x, ty, BAR_W, th}, BAR_W / 2, scroll_ ? SCROLL_THUMB_BACK : SCROLL_THUMB);
    }
    all_dirty_ = false;
    if (all) damage = s.bounds();
    return damage;
}
