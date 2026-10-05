// What a small built-in desktop application looks like to the compositor
// (0.0.5h): it paints its content into a Surface and answers keys, clicks
// and the wheel in content coordinates. Each returns true when the window
// needs repainting. Instances live in desktop.cpp, one per window kind,
// until Pane makes them real programs (phase 12).
#pragma once

#include <drivers/ps2kbd.h>
#include <gfx/gfx.h>
#include <lib/types.h>

struct AppContext {
    const gfx::Font* font;
    const gfx::Font* bold;
    const gfx::Font* mono;
    gfx::Color accent;
    gfx::Color text, muted, bg;
    // The desktop clipboard.
    const char* clipboard;
    usize clipboard_len;
    void (*clipboard_set)(const char* text, usize len);
    void (*notify)(const char* title, const char* text);
};
