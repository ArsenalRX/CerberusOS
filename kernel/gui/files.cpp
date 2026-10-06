// See files.h. Everything goes through the VFS as root (there are no
// users yet); a directory is read whole, at most MAX_ENTRIES entries,
// folders first, each group in name order.
#include <drivers/rtc.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <gui/files.h>
#include <sched/sched.h>
#include <lib/kprintf.h>
#include <lib/string.h>

using namespace gfx;

namespace {
constexpr int TOP = 40, ROW_H = 26, PAD = 10;
const Credentials ROOT_CRED = {0, 0};
enum { BTN_UP, BTN_NEW, BTN_RENAME, BTN_DELETE, BTN_HOME };

void size_text(u64 size, char* out, usize n) {
    if (size < 1024) ksnprintf(out, n, "%lu B", (unsigned long)size);
    else if (size < 1024 * 1024) ksnprintf(out, n, "%lu.%lu KiB", (unsigned long)(size / 1024), (unsigned long)(size % 1024 * 10 / 1024));
    else ksnprintf(out, n, "%lu.%lu MiB", (unsigned long)(size >> 20), (unsigned long)((size & 0xFFFFF) * 10 >> 20));
}
} // namespace

void FilesApp::join(char* out, usize n, const char* name) const {
    if (!strcmp(cwd_, "/")) ksnprintf(out, n, "/%s", name);
    else ksnprintf(out, n, "%s/%s", cwd_, name);
}

void FilesApp::read_dir() {
    dirty_ = false;
    count_ = 0;
    Result<Vnode*> d = vfs_resolve(nullptr, cwd_, ROOT_CRED, LookupFlags{});
    if (!d.ok()) {
        ksnprintf(status_, sizeof status_, "Cannot open %s", cwd_);
        return;
    }
    u64 cookie = 0;
    DirEntry de;
    while (count_ < MAX_ENTRIES) {
        Result<bool> more = vfs_readdir(d.value(), &cookie, &de);
        if (!more.ok() || !more.value()) break;
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        Entry& e = entries_[count_];
        strlcpy(e.name, de.name, sizeof e.name);
        e.dir = de.type == VType::Dir;
        e.size = 0;
        e.mtime = 0;
        char path[200];
        join(path, sizeof path, de.name);
        Result<Vnode*> v = vfs_resolve(nullptr, path, ROOT_CRED, LookupFlags{});
        if (v.ok()) {
            e.dir = v.value()->type == VType::Dir;
            e.size = v.value()->size;
            e.mtime = v.value()->mtime;
            vnode_unref(v.value());
        }
        count_++;
    }
    vnode_unref(d.value());
    // Insertion sort: folders first, then by name (case-insensitive).
    auto less = [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        for (int i = 0;; i++) {
            char x = a.name[i], y = b.name[i];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) return x < y;
            if (!x) return false;
        }
    };
    for (int i = 1; i < count_; i++) {
        Entry e = entries_[i];
        int j = i - 1;
        while (j >= 0 && less(e, entries_[j])) { entries_[j + 1] = entries_[j]; j--; }
        entries_[j + 1] = e;
    }
    if (selected_ >= count_) selected_ = count_ - 1;
    if (scroll_ > count_) scroll_ = count_ > 0 ? count_ - 1 : 0;
    status_[0] = 0;
}

void FilesApp::go(const char* path) {
    strlcpy(cwd_, path, sizeof cwd_);
    selected_ = -1;
    scroll_ = 0;
    delete_armed_ = -1;
    entering_ = Entering::None;
    dirty_ = true;
}

void FilesApp::up() {
    if (!strcmp(cwd_, "/")) return;
    char* slash = cwd_ + strlen(cwd_);
    while (slash > cwd_ && *slash != '/') slash--;
    if (slash == cwd_) strlcpy(cwd_, "/", sizeof cwd_);
    else *slash = 0;
    selected_ = -1;
    scroll_ = 0;
    delete_armed_ = -1;
    dirty_ = true;
}

void FilesApp::open_selected() {
    if (selected_ < 0 || selected_ >= count_) return;
    const Entry& e = entries_[selected_];
    char path[200];
    join(path, sizeof path, e.name);
    if (e.dir) {
        if (strlen(path) < sizeof cwd_) go(path);
        return;
    }
    usize nl = strlen(e.name);
    bool image = nl > 4 && (!strcmp(e.name + nl - 4, ".bmp") || !strcmp(e.name + nl - 4, ".BMP"));
    if (!image && e.size > 64 * 1024) {
        ksnprintf(status_, sizeof status_, "%s is too big for Notes (64 KiB at most)", e.name);
        return;
    }
    strlcpy(open_request_, path, sizeof open_request_);
}

bool FilesApp::confirm_delete() {
    if (selected_ < 0 || selected_ >= count_) return false;
    if (delete_armed_ != selected_) {
        delete_armed_ = selected_;
        ksnprintf(status_, sizeof status_, "Delete %s? Press Delete again.", entries_[selected_].name);
        return true;
    }
    char path[200];
    join(path, sizeof path, entries_[selected_].name);
    Result<void> r = vfs_unlink(nullptr, path, ROOT_CRED, entries_[selected_].dir);
    ksnprintf(status_, sizeof status_, r.ok() ? "Deleted %s" : "Could not delete %s: %s", entries_[selected_].name,
              r.ok() ? "" : error_name(r.error()));
    delete_armed_ = -1;
    dirty_ = true;
    return true;
}

void FilesApp::finish_entry(const AppContext& c) {
    (void)c;
    if (!entry_len_) { entering_ = Entering::None; return; }
    for (int i = 0; i < entry_len_; i++)
        if (entry_[i] == '/') { strlcpy(status_, "A name cannot contain '/'", sizeof status_); return; }
    char path[200];
    join(path, sizeof path, entry_);
    Result<void> r;
    if (entering_ == Entering::NewFolder) {
        Result<Vnode*> v = vfs_create(nullptr, path, ROOT_CRED, VType::Dir, 0755, true);
        if (v.ok()) vnode_unref(v.value());
        else r = v.error();
    } else if (selected_ >= 0 && selected_ < count_) {
        char from[200];
        join(from, sizeof from, entries_[selected_].name);
        r = vfs_rename(nullptr, from, path, ROOT_CRED);
    }
    if (!r.ok()) ksnprintf(status_, sizeof status_, "%s: %s", entry_, error_name(r.error()));
    else status_[0] = 0;
    entering_ = Entering::None;
    dirty_ = true;
}

bool FilesApp::key(const KeyEvent& e, const AppContext& c) {
    if (entering_ != Entering::None) {
        if (e.key == key::ESCAPE) entering_ = Entering::None;
        else if (e.key == key::ENTER) finish_entry(c);
        else if (e.key == key::BACKSPACE) { if (entry_len_) entry_[--entry_len_] = 0; }
        else if (e.ascii >= 32 && e.ascii < 127 && entry_len_ < (int)sizeof entry_ - 1) {
            entry_[entry_len_++] = e.ascii;
            entry_[entry_len_] = 0;
        } else return false;
        return true;
    }
    switch (e.key) {
    case key::UP: if (selected_ > 0) selected_--; else selected_ = count_ ? 0 : -1; break;
    case key::DOWN: if (selected_ + 1 < count_) selected_++; break;
    case key::HOME: selected_ = count_ ? 0 : -1; break;
    case key::END: selected_ = count_ - 1; break;
    case key::ENTER: open_selected(); return true;
    case key::BACKSPACE: up(); return true;
    case key::DELETE: return confirm_delete();
    case key::F1 + 1:           // F2: rename
        if (selected_ >= 0) {
            entering_ = Entering::Rename;
            strlcpy(entry_, entries_[selected_].name, sizeof entry_);
            entry_len_ = (int)strlen(entry_);
        }
        return true;
    case key::F1 + 4: dirty_ = true; return true;      // F5: refresh
    default:
        if ((e.mods & mod::CTRL) && (e.ascii == 'n' || e.ascii == 0x0E)) {
            entering_ = Entering::NewFolder;
            entry_len_ = 0;
            entry_[0] = 0;
            return true;
        }
        return false;
    }
    if (selected_ >= 0) {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + rows_visible_) scroll_ = selected_ - rows_visible_ + 1;
    }
    delete_armed_ = -1;
    return true;
}

bool FilesApp::click(int x, int y, int button, int clicks, const AppContext& c) {
    if (button != 0) return false;
    for (int i = 0; i < button_count_; i++) {
        if (!buttons_[i].r.contains(x, y)) continue;
        switch (buttons_[i].id) {
        case BTN_UP: up(); break;
        case BTN_HOME: go(fs_data_mounted() ? "/data" : "/"); break;
        case BTN_NEW: entering_ = Entering::NewFolder; entry_len_ = 0; entry_[0] = 0; break;
        case BTN_RENAME:
            if (selected_ >= 0) {
                entering_ = Entering::Rename;
                strlcpy(entry_, entries_[selected_].name, sizeof entry_);
                entry_len_ = (int)strlen(entry_);
            }
            break;
        case BTN_DELETE: confirm_delete(); break;
        }
        (void)c;
        return true;
    }
    if (y < TOP) return false;
    int row = (y - TOP - 4) / ROW_H + scroll_;
    if (row < 0 || row >= count_) {
        selected_ = -1;
        return true;
    }
    if (selected_ != row) delete_armed_ = -1;
    selected_ = row;
    if (clicks >= 2) open_selected();
    return true;
}

bool FilesApp::wheel(int notches) {
    scroll_ -= notches * 3;
    if (scroll_ > count_ - rows_visible_) scroll_ = count_ - rows_visible_;
    if (scroll_ < 0) scroll_ = 0;
    return true;
}

void FilesApp::paint(Surface& s, const AppContext& c) {
    if (dirty_) read_dir();
    fill_rect(s, s.bounds(), c.bg);
    // Toolbar: Up, Home, the path, then New folder / Rename / Delete.
    fill_rect(s, {0, 0, s.width, TOP}, rgba(255, 255, 255, 7));
    draw_hline(s, 0, s.width - 1, TOP - 1, rgba(255, 255, 255, 14));
    button_count_ = 0;
    int x = PAD;
    auto button = [&](const char* label, int id, bool right) {
        int w = measure_text(*c.font, label) + 20;
        Rect r{right ? x - w : x, 7, w, 26};
        fill_rect_rounded(s, r, 8, rgba(255, 255, 255, 16));
        draw_text(s, *c.font, r.x + 10, r.y + (r.h - c.font->height) / 2, label, c.text);
        buttons_[button_count_++] = Button{r, id};
        x = right ? r.x - 8 : r.right() + 8;
    };
    button("Up", BTN_UP, false);
    button("Home", BTN_HOME, false);
    int path_x = x;
    x = s.width - PAD;
    button("Delete", BTN_DELETE, true);
    button("Rename", BTN_RENAME, true);
    button("New folder", BTN_NEW, true);
    draw_text_ellipsis(s, *c.font, path_x + 6, (TOP - c.font->height) / 2, cwd_, x - path_x - 12, c.muted);

    rows_visible_ = (s.height - TOP - 30) / ROW_H;
    if (rows_visible_ < 1) rows_visible_ = 1;
    if (scroll_ > count_ - rows_visible_) scroll_ = count_ - rows_visible_;
    if (scroll_ < 0) scroll_ = 0;
    char line[64];
    for (int i = 0; i < rows_visible_ && scroll_ + i < count_; i++) {
        const Entry& e = entries_[scroll_ + i];
        int y = TOP + 4 + i * ROW_H;
        bool sel = scroll_ + i == selected_;
        if (sel) fill_rect_rounded(s, {PAD - 4, y, s.width - 2 * PAD + 8, ROW_H}, 6, with_alpha(c.accent, 70));
        // An icon: a folder tab or a page.
        int ix = PAD + 4, iy = y + 6;
        if (e.dir) {
            fill_rect_rounded(s, {ix, iy + 2, 16, 12}, 2, rgb(250, 204, 21));
            fill_rect_rounded(s, {ix, iy, 8, 5}, 2, rgb(250, 204, 21));
        } else {
            fill_rect_rounded(s, {ix + 2, iy, 12, 15}, 2, rgb(148, 163, 184));
            fill_rect(s, {ix + 5, iy + 4, 6, 1}, c.bg);
            fill_rect(s, {ix + 5, iy + 7, 6, 1}, c.bg);
            fill_rect(s, {ix + 5, iy + 10, 6, 1}, c.bg);
        }
        draw_text_ellipsis(s, *c.font, PAD + 30, y + (ROW_H - c.font->height) / 2, e.name, s.width - 240, c.text);
        if (!e.dir) {
            size_text(e.size, line, sizeof line);
            int w = measure_text(*c.font, line);
            draw_text(s, *c.font, s.width - PAD - 110 - w, y + (ROW_H - c.font->height) / 2, line, c.muted);
        }
        if (e.mtime) {
            DateTime t = unix_to_datetime(e.mtime);
            ksnprintf(line, sizeof line, "%04u-%02u-%02u", t.year, t.month, t.day);
            draw_text(s, *c.font, s.width - PAD - 90, y + (ROW_H - c.font->height) / 2, line, c.muted);
        }
    }
    if (!count_ && !status_[0]) draw_text(s, *c.font, PAD + 4, TOP + 12, "Empty", c.muted);
    // Bottom line: a count, the status, or the name being typed.
    int by = s.height - 24;
    if (entering_ != Entering::None) {
        const char* what = entering_ == Entering::NewFolder ? "New folder:" : "Rename to:";
        int w = measure_text(*c.font, what);
        draw_text(s, *c.font, PAD, by, what, c.muted);
        Rect f{PAD + w + 8, by - 5, s.width - PAD - w - 16, 26};
        fill_rect_rounded(s, f, 8, rgba(255, 255, 255, 14));
        stroke_rect_rounded(s, f, 8, with_alpha(c.accent, 150));
        int tx = draw_text_ellipsis(s, *c.font, f.x + 8, f.y + (f.h - c.font->height) / 2, entry_, f.w - 16, c.text);
        fill_rect(s, {tx + 1, f.y + 6, 2, f.h - 12}, c.accent);
    } else if (status_[0]) {
        draw_text_ellipsis(s, *c.font, PAD, by, status_, s.width - 2 * PAD, delete_armed_ >= 0 ? rgb(248, 113, 113) : c.muted);
    } else {
        ksnprintf(line, sizeof line, "%d item%s", count_, count_ == 1 ? "" : "s");
        draw_text(s, *c.font, PAD, by, line, c.muted);
    }
}
