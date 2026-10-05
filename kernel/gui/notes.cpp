// See notes.h. One flat buffer; lines are found by scanning, which is fine
// for the 64 KiB this editor holds.
#include <drivers/refclock.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <gui/notes.h>
#include <sched/sched.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>

using namespace gfx;

namespace {
constexpr int PAD = 10, TOP = 36;         // the bar with the file name sits at the top
const Credentials ROOT_CRED = {0, 0};
} // namespace

bool NotesApp::init() {
    if (buf_) return true;
    buf_ = (char*)kmalloc(CAP);
    if (!buf_) return false;
    len_ = 0;
    return true;
}

usize NotesApp::line_start(usize at) const {
    while (at > 0 && buf_[at - 1] != '\n') at--;
    return at;
}

usize NotesApp::line_end(usize at) const {
    while (at < len_ && buf_[at] != '\n') at++;
    return at;
}

int NotesApp::line_of(usize at) const {
    int n = 0;
    for (usize i = 0; i < at && i < len_; i++) n += buf_[i] == '\n';
    return n;
}

usize NotesApp::pos_of_line(int line) const {
    usize at = 0;
    while (line > 0 && at < len_) {
        if (buf_[at++] == '\n') line--;
    }
    return at;
}

usize NotesApp::pos_at(int x, int y) const {
    int line = scroll_line_ + (y - TOP - PAD) / line_h_;
    if (line < 0) line = 0;
    usize at = pos_of_line(line);
    if (at >= len_ && line > line_of(len_)) return len_;
    int col = (x - PAD + char_w_ / 2) / char_w_;
    if (col < 0) col = 0;
    usize end = line_end(at);
    return at + (usize)col < end ? at + (usize)col : end;
}

void NotesApp::snapshot() {
    if (undo_count_ == 8) {
        kfree(undo_[0]);
        for (int i = 1; i < 8; i++) { undo_[i - 1] = undo_[i]; undo_len_[i - 1] = undo_len_[i]; }
        undo_count_ = 7;
    }
    char* copy = (char*)kmalloc(len_ ? len_ : 1);
    if (!copy) return;
    memcpy(copy, buf_, len_);
    undo_[undo_count_] = copy;
    undo_len_[undo_count_] = len_;
    undo_count_++;
}

bool NotesApp::undo() {
    if (!undo_count_) return false;
    undo_count_--;
    len_ = undo_len_[undo_count_];
    memcpy(buf_, undo_[undo_count_], len_);
    kfree(undo_[undo_count_]);
    if (cursor_ > len_) cursor_ = len_;
    anchor_ = cursor_;
    modified_ = true;
    return true;
}

void NotesApp::insert(const char* text, usize n) {
    if (has_selection()) {
        usize a, b;
        selection(&a, &b);
        erase(a, b);
    }
    if (len_ + n > CAP) n = CAP - len_;
    if (!n) return;
    memmove(buf_ + cursor_ + n, buf_ + cursor_, len_ - cursor_);
    memcpy(buf_ + cursor_, text, n);
    len_ += n;
    cursor_ += n;
    anchor_ = cursor_;
    modified_ = true;
    preferred_col_ = -1;
}

void NotesApp::erase(usize from, usize to) {
    if (from >= to || to > len_) return;
    memmove(buf_ + from, buf_ + to, len_ - to);
    len_ -= to - from;
    cursor_ = anchor_ = from;
    modified_ = true;
    preferred_col_ = -1;
}

void NotesApp::selection(usize* from, usize* to) const {
    *from = anchor_ < cursor_ ? anchor_ : cursor_;
    *to = anchor_ < cursor_ ? cursor_ : anchor_;
}

void NotesApp::keep_cursor_visible() {
    int line = line_of(cursor_);
    if (line < scroll_line_) scroll_line_ = line;
    if (line >= scroll_line_ + rows_visible_) scroll_line_ = line - rows_visible_ + 1;
    if (scroll_line_ < 0) scroll_line_ = 0;
}

bool NotesApp::load(const char* path) {
    if (!init()) return false;
    new_file();
    strlcpy(path_, path, sizeof path_);
    Result<Vnode*> v = vfs_resolve(nullptr, path, ROOT_CRED, LookupFlags{});
    if (!v.ok() || v.value()->type != VType::File) {
        if (v.ok()) vnode_unref(v.value());
        return false;
    }
    usize size = 0;
    Result<u8*> data = vfs_read_all(v.value(), CAP, &size);
    vnode_unref(v.value());
    if (!data.ok()) return false;
    // Only printable text and newlines are kept; anything else is shown as '?'.
    for (usize i = 0; i < size; i++) {
        char c = (char)data.value()[i];
        if (c == '\r') continue;
        buf_[len_++] = c == '\n' || c == '\t' || (c >= 32 && c < 127) ? c : '?';
    }
    kfree(data.value());
    modified_ = false;
    return true;
}

void NotesApp::new_file() {
    len_ = cursor_ = anchor_ = 0;
    scroll_line_ = 0;
    path_[0] = 0;
    modified_ = false;
    for (int i = 0; i < undo_count_; i++) kfree(undo_[i]);
    undo_count_ = 0;
    finding_ = false;
}

bool NotesApp::save(const AppContext& c) {
    if (!path_[0]) strlcpy(path_, fs_data_mounted() ? "/data/notes.txt" : "/tmp/notes.txt", sizeof path_);
    Result<Vnode*> v = vfs_create(nullptr, path_, ROOT_CRED, VType::File, 0644, false);
    bool ok = v.ok();
    if (ok) {
        ok = vfs_truncate(v.value(), 0).ok() && (len_ == 0 || vfs_write(v.value(), 0, buf_, len_).ok());
        vnode_unref(v.value());
    }
    ksnprintf(status_, sizeof status_, ok ? "Saved %s" : "Could not save %s", path_);
    status_until_ = refclock_now_us() + 3000000;
    if (ok) modified_ = false;
    else c.notify("Notes", status_);
    return ok;
}

void NotesApp::find_next() {
    if (!find_len_ || len_ < (usize)find_len_) return;
    usize start = cursor_ < len_ ? cursor_ : 0;
    for (usize k = 0; k < len_; k++) {
        usize i = (start + k) % len_;
        if (i + (usize)find_len_ > len_) continue;
        bool match = true;
        for (int j = 0; j < find_len_ && match; j++) {
            char a = buf_[i + (usize)j], b = find_[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            match = a == b;
        }
        if (match) {
            anchor_ = i;
            cursor_ = i + (usize)find_len_;
            keep_cursor_visible();
            return;
        }
    }
    strlcpy(status_, "Not found", sizeof status_);
    status_until_ = refclock_now_us() + 2000000;
}

bool NotesApp::key(const KeyEvent& e, const AppContext& c) {
    if (!init()) return false;
    bool ctrl = e.mods & mod::CTRL, shift = e.mods & mod::SHIFT;
    if (finding_) {
        if (e.key == key::ESCAPE) finding_ = false;
        else if (e.key == key::ENTER) find_next();
        else if (e.key == key::BACKSPACE) { if (find_len_) find_[--find_len_] = 0; }
        else if (e.ascii >= 32 && e.ascii < 127 && !ctrl && find_len_ < (int)sizeof find_ - 1) {
            find_[find_len_++] = e.ascii;
            find_[find_len_] = 0;
        } else return false;
        return true;
    }
    usize old = cursor_;
    auto moved = [&]() {
        if (!shift) anchor_ = cursor_;
        keep_cursor_visible();
    };
    switch (e.key) {
    case key::LEFT: if (cursor_) cursor_--; preferred_col_ = -1; moved(); return true;
    case key::RIGHT: if (cursor_ < len_) cursor_++; preferred_col_ = -1; moved(); return true;
    case key::HOME: cursor_ = line_start(cursor_); preferred_col_ = -1; moved(); return true;
    case key::END: cursor_ = line_end(cursor_); preferred_col_ = -1; moved(); return true;
    case key::UP:
    case key::DOWN:
    case key::PAGE_UP:
    case key::PAGE_DOWN: {
        int step = e.key == key::UP ? -1 : e.key == key::DOWN ? 1 : e.key == key::PAGE_UP ? -rows_visible_ : rows_visible_;
        usize ls = line_start(cursor_);
        int col = preferred_col_ >= 0 ? preferred_col_ : (int)(cursor_ - ls);
        preferred_col_ = col;
        int line = line_of(cursor_) + step;
        if (line < 0) line = 0;
        usize at = pos_of_line(line);
        usize end = line_end(at);
        cursor_ = at + (usize)col < end ? at + (usize)col : end;
        moved();
        return true;
    }
    case key::ENTER: insert("\n", 1); keep_cursor_visible(); return true;
    case key::BACKSPACE:
        if (has_selection()) { usize a, b; selection(&a, &b); snapshot(); erase(a, b); }
        else if (cursor_) { snapshot(); erase(cursor_ - 1, cursor_); }
        keep_cursor_visible();
        return true;
    case key::DELETE:
        if (has_selection()) { usize a, b; selection(&a, &b); snapshot(); erase(a, b); }
        else if (cursor_ < len_) { snapshot(); erase(cursor_, cursor_ + 1); }
        return true;
    case key::TAB: snapshot(); insert("    ", 4); return true;
    case key::ESCAPE: anchor_ = cursor_; return true;
    default: break;
    }
    (void)old;
    if (ctrl) {
        char k = e.ascii;
        if (k >= 1 && k <= 26) k = (char)('a' + k - 1);     // the keyboard gives control codes
        switch (k) {
        case 's': save(c); return true;
        case 'z': return undo();
        case 'a': anchor_ = 0; cursor_ = len_; return true;
        case 'f': finding_ = true; return true;
        case 'n': new_file(); return true;
        case 'c':
        case 'x': {
            if (!has_selection()) return false;
            usize a, b;
            selection(&a, &b);
            c.clipboard_set(buf_ + a, b - a);
            if (k == 'x') { snapshot(); erase(a, b); }
            return true;
        }
        case 'v':
            if (!c.clipboard_len) return false;
            snapshot();
            insert(c.clipboard, c.clipboard_len);
            keep_cursor_visible();
            return true;
        default: return false;
        }
    }
    if (e.ascii >= 32 && e.ascii < 127) {
        // One undo step per burst of typing: a snapshot when the last key was not a plain character.
        static bool typing = false;
        if (!typing) snapshot();
        typing = true;
        insert(&e.ascii, 1);
        keep_cursor_visible();
        return true;
    }
    return false;
}

bool NotesApp::click(int x, int y, int button, bool drag, const AppContext& c) {
    (void)c;
    if (button != 0 || !init()) return false;
    if (y < TOP && !drag) return false;
    usize at = pos_at(x, y);
    cursor_ = at;
    if (!drag) anchor_ = at;
    preferred_col_ = -1;
    return true;
}

bool NotesApp::wheel(int notches) {
    int lines = line_of(len_) + 1;
    scroll_line_ -= notches * 3;
    if (scroll_line_ > lines - 1) scroll_line_ = lines - 1;
    if (scroll_line_ < 0) scroll_line_ = 0;
    return true;
}

void NotesApp::paint(Surface& s, const AppContext& c) {
    fill_rect(s, s.bounds(), c.bg);
    if (!init()) {
        draw_text(s, *c.font, PAD, TOP, "Out of memory.", c.muted);
        return;
    }
    line_h_ = c.mono->height + 2;
    char_w_ = c.mono->width;
    rows_visible_ = (s.height - TOP - 2 * PAD) / line_h_;
    if (rows_visible_ < 1) rows_visible_ = 1;
    // Top bar: file name, modified mark, shortcuts on the right.
    fill_rect(s, {0, 0, s.width, TOP}, rgba(255, 255, 255, 7));
    draw_hline(s, 0, s.width - 1, TOP - 1, rgba(255, 255, 255, 14));
    char title[160];
    ksnprintf(title, sizeof title, "%s%s", path_[0] ? path_ : "New note", modified_ ? "  *" : "");
    draw_text_ellipsis(s, *c.font, PAD, (TOP - c.font->height) / 2, title, s.width / 2, c.text);
    const char* hint = finding_ ? "Enter: next   Esc: close" : "Ctrl+S save   Ctrl+F find   Ctrl+Z undo";
    if (status_[0] && refclock_now_us() < status_until_) hint = status_;
    int hw = measure_text(*c.font, hint);
    draw_text(s, *c.font, s.width - PAD - hw, (TOP - c.font->height) / 2, hint, c.muted);
    // Text.
    usize sa, sb;
    selection(&sa, &sb);
    usize at = pos_of_line(scroll_line_);
    int cursor_line = line_of(cursor_);
    for (int row = 0; row < rows_visible_ && at <= len_; row++) {
        usize end = line_end(at);
        int y = TOP + PAD + row * line_h_;
        int line = scroll_line_ + row;
        if (has_selection() && sb > at && sa < end + 1) {
            usize from = sa > at ? sa : at, to = sb < end ? sb : end;
            int x0 = PAD + (int)(from - at) * char_w_, x1 = PAD + (int)(to - at) * char_w_ + (sb > end ? char_w_ / 2 : 0);
            fill_rect(s, {x0, y, x1 - x0, line_h_}, with_alpha(c.accent, 90));
        }
        int cols = (s.width - 2 * PAD) / char_w_;
        if (end > at) draw_text_n(s, *c.mono, PAD, y + 1, buf_ + at, (usize)(end - at) < (usize)cols ? end - at : (usize)cols, c.text);
        if (line == cursor_line) {
            int cx = PAD + (int)(cursor_ - at) * char_w_;
            fill_rect(s, {cx, y, 2, line_h_}, c.accent);
        }
        if (end >= len_) break;
        at = end + 1;
    }
    if (finding_) {
        Rect f{s.width - 260, TOP + 6, 250, 28};
        fill_rect_rounded(s, f, 8, rgb(30, 33, 44));
        stroke_rect_rounded(s, f, 8, with_alpha(c.accent, 150));
        int tx = f.x + 10, ty = f.y + (f.h - c.font->height) / 2;
        if (find_len_) tx = draw_text_ellipsis(s, *c.font, tx, ty, find_, f.w - 20, c.text);
        else draw_text(s, *c.font, tx, ty, "Find", c.muted);
        fill_rect(s, {tx + 1, f.y + 6, 2, f.h - 12}, c.accent);
    }
}
