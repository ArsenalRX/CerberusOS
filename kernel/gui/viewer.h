// Image viewer (Frame's first form): shows a BMP file (24 or 32 bits per
// pixel, uncompressed — what Print Screen writes) fitted to the window,
// with a button that makes it the wallpaper. See app.h.
#pragma once

#include <gui/app.h>

class ViewerApp {
public:
    void paint(gfx::Surface& s, const AppContext& c);
    bool click(int x, int y, int button, const AppContext& c);
    // Loads a BMP; false (and a message) if it cannot be read or decoded.
    bool load(const char* path);
    const char* path() const { return path_; }
    // The decoded image, for the wallpaper (null if none).
    const gfx::Surface* image() const { return pixels_ ? &img_ : nullptr; }
    // Set by a click on "Set as wallpaper"; the desktop consumes it.
    bool take_wallpaper_request() { bool r = wants_wallpaper_; wants_wallpaper_ = false; return r; }
    static constexpr int MIN_W = 320, MIN_H = 240;
    static constexpr usize MAX_BYTES = 32 * 1024 * 1024;

private:
    void unload();
    u32* pixels_ = nullptr;
    gfx::Surface img_;
    char path_[128] = {};
    char status_[96] = {};
    bool wants_wallpaper_ = false;
    gfx::Rect button_{0, 0, 0, 0};
};

// Decodes a BMP from memory into a new heap buffer (0xAARRGGBB, A = 255);
// null if it is not a BMP this decoder reads. Every header field is
// checked against the data's size before use.
u32* bmp_decode(const u8* data, usize size, int* width, int* height);
