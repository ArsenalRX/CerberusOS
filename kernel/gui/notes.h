// Notes (Slate's first form): a plain-text editor over one file. Typing,
// arrows, Home/End, Page Up/Down, Shift+arrows select, Ctrl+C/X/V, Ctrl+A,
// Ctrl+Z undo, Ctrl+S save, Ctrl+F find (Enter: next match, Escape: back),
// mouse click and drag, wheel. New text goes to /data/notes.txt when there
// is a data disk, else /tmp/notes.txt. See app.h.
#pragma once

#include <gui/app.h>

class NotesApp {
public:
    bool init();                                // allocates the buffers; false if out of memory
    void paint(gfx::Surface& s, const AppContext& c);
    bool key(const KeyEvent& e, const AppContext& c);
    bool click(int x, int y, int button, bool drag, const AppContext& c);
    bool wheel(int notches);
    // Loads a file (at most CAP bytes); false and an empty buffer if it cannot be read.
    bool load(const char* path);
    void new_file();
    const char* path() const { return path_; }
    bool modified() const { return modified_; }
    static constexpr int MIN_W = 360, MIN_H = 240;
    static constexpr usize CAP = 64 * 1024;

private:
    void insert(const char* text, usize n);
    void erase(usize from, usize to);
    void snapshot();
    bool undo();
    bool save(const AppContext& c);
    usize line_start(usize at) const;
    usize line_end(usize at) const;
    int line_of(usize at) const;
    usize pos_of_line(int line) const;
    usize pos_at(int x, int y) const;
    void keep_cursor_visible();
    bool has_selection() const { return anchor_ != cursor_; }
    void selection(usize* from, usize* to) const;
    void find_next();

    char* buf_ = nullptr;
    usize len_ = 0;
    usize cursor_ = 0, anchor_ = 0;
    int scroll_line_ = 0;
    int preferred_col_ = -1;
    int rows_visible_ = 1;
    int line_h_ = 16, char_w_ = 8;
    char path_[128] = {};
    bool modified_ = false;
    char* undo_[8] = {};
    usize undo_len_[8] = {};
    int undo_count_ = 0;
    bool finding_ = false;
    char find_[40] = {};
    int find_len_ = 0;
    char status_[64] = {};
    u64 status_until_ = 0;
};
