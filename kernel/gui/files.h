// Files (Crate's first form): browses the file tree. Double-click (or
// Enter) opens a folder or hands a text file to Notes; Backspace or "Up"
// goes up; New folder, Rename and Delete (Delete asks twice); the wheel and
// Up/Down scroll. Starts in /data when there is a data disk. See app.h.
#pragma once

#include <gui/app.h>

class FilesApp {
public:
    void paint(gfx::Surface& s, const AppContext& c);
    bool key(const KeyEvent& e, const AppContext& c);
    // `clicks` is 1 or 2 (a double-click).
    bool click(int x, int y, int button, int clicks, const AppContext& c);
    bool wheel(int notches);
    void go(const char* path);
    void refresh() { dirty_ = true; }
    // A file the user asked to open, consumed by the caller (empty = none).
    const char* take_open_request() { return open_request_[0] ? open_request_ : nullptr; }
    void clear_open_request() { open_request_[0] = 0; }
    const char* path() const { return cwd_; }
    static constexpr int MIN_W = 420, MIN_H = 260;

private:
    struct Entry {
        char name[64];
        bool dir;
        u64 size;
        u64 mtime;
    };
    static constexpr int MAX_ENTRIES = 256;
    void read_dir();
    void open_selected();
    void up();
    void join(char* out, usize n, const char* name) const;
    bool confirm_delete();
    void finish_entry(const AppContext& c);

    char cwd_[128] = "/";
    Entry entries_[MAX_ENTRIES];
    int count_ = 0;
    int selected_ = -1;
    int scroll_ = 0;
    int rows_visible_ = 1;
    bool dirty_ = true;
    char open_request_[160] = {};
    // Typing a name: for a new folder or a rename.
    enum class Entering { None, NewFolder, Rename } entering_ = Entering::None;
    char entry_[64] = {};
    int entry_len_ = 0;
    int delete_armed_ = -1;         // the entry whose Delete was pressed once
    char status_[96] = {};
    struct Button {
        gfx::Rect r;
        int id;
    };
    Button buttons_[6];
    int button_count_ = 0;
};
