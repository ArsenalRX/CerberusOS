// Calculator (Tally's first form): an expression typed or clicked, with
// + - * / % ^ and brackets, evaluated in fixed point with six decimals;
// the result also shown in hexadecimal and binary. See app.h.
#pragma once

#include <gui/app.h>

class CalcApp {
public:
    void paint(gfx::Surface& s, const AppContext& c);
    bool key(const KeyEvent& e, const AppContext& c);
    bool click(int x, int y, int button, const AppContext& c);
    static constexpr int MIN_W = 300, MIN_H = 420;

private:
    void push(char ch);
    void backspace();
    void clear();
    void evaluate();
    char expr_[64] = {};
    int len_ = 0;
    char result_[48] = {};
    char hex_[40] = {}, bin_[72] = {};
    bool error_ = false;
    bool fresh_ = false;        // the next digit starts a new expression
    i64 memory_ = 0;
    struct Button {
        gfx::Rect r;
        char ch;                // what it types; 0 = special, see label
        const char* label;
    };
    Button buttons_[24];
    int button_count_ = 0;
};
