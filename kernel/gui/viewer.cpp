// See viewer.h.
#include <fs/vfs.h>
#include <gui/viewer.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <sched/sched.h>

using namespace gfx;

namespace {
const Credentials ROOT_CRED = {0, 0};
u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
} // namespace

u32* bmp_decode(const u8* d, usize size, int* width, int* height) {
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return nullptr;
    u32 offset = le32(d + 10), hdr = le32(d + 14);
    if (hdr < 40 || offset < 14 + hdr || offset > size) return nullptr;
    i32 w = (i32)le32(d + 18), h = (i32)le32(d + 22);
    u16 planes = le16(d + 26), bpp = le16(d + 28);
    u32 compression = le32(d + 30);
    bool bottom_up = h > 0;
    if (h < 0) h = -h;
    // BI_RGB only, 24 or 32 bits, sane sizes (a 32 MiB image at most).
    if (planes != 1 || (bpp != 24 && bpp != 32) || (compression != 0 && !(compression == 3 && bpp == 32))) return nullptr;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (u64)w * (u64)h * 4 > ViewerApp::MAX_BYTES) return nullptr;
    u64 stride = ((u64)w * bpp / 8 + 3) & ~3ull;
    if (offset + stride * (u64)h > size) return nullptr;
    u32* out = (u32*)kmalloc((usize)w * (usize)h * 4);
    if (!out) return nullptr;
    for (i32 y = 0; y < h; y++) {
        const u8* row = d + offset + stride * (u64)(bottom_up ? h - 1 - y : y);
        u32* o = out + (usize)y * (usize)w;
        for (i32 x = 0; x < w; x++) {
            const u8* p = row + (usize)x * (bpp / 8);
            o[x] = 0xFF000000u | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
        }
    }
    *width = w;
    *height = h;
    return out;
}

void ViewerApp::unload() {
    kfree(pixels_);
    pixels_ = nullptr;
    img_ = Surface();
}

bool ViewerApp::load(const char* path) {
    unload();
    strlcpy(path_, path, sizeof path_);
    status_[0] = 0;
    Result<Vnode*> v = vfs_resolve(nullptr, path, ROOT_CRED, LookupFlags{});
    if (!v.ok() || v.value()->type != VType::File) {
        if (v.ok()) vnode_unref(v.value());
        strlcpy(status_, "Cannot open the file.", sizeof status_);
        return false;
    }
    usize size = 0;
    Result<u8*> data = vfs_read_all(v.value(), MAX_BYTES, &size);
    vnode_unref(v.value());
    if (!data.ok()) {
        strlcpy(status_, "Cannot read the file.", sizeof status_);
        return false;
    }
    int w = 0, h = 0;
    pixels_ = bmp_decode(data.value(), size, &w, &h);
    kfree(data.value());
    if (!pixels_) {
        strlcpy(status_, "Not a BMP this viewer can show (24/32-bit, uncompressed).", sizeof status_);
        return false;
    }
    img_ = Surface(pixels_, w, h, w);
    return true;
}

bool ViewerApp::click(int x, int y, int button, const AppContext& c) {
    (void)c;
    if (button != 0 || !pixels_) return false;
    if (button_.contains(x, y)) {
        wants_wallpaper_ = true;
        return true;
    }
    return false;
}

void ViewerApp::paint(Surface& s, const AppContext& c) {
    fill_rect(s, s.bounds(), rgb(12, 13, 18));
    const int TOP = 36;
    fill_rect(s, {0, 0, s.width, TOP}, rgba(255, 255, 255, 7));
    draw_hline(s, 0, s.width - 1, TOP - 1, rgba(255, 255, 255, 14));
    char title[160];
    if (pixels_) ksnprintf(title, sizeof title, "%s  (%d x %d)", path_, img_.width, img_.height);
    else ksnprintf(title, sizeof title, "%s", path_[0] ? path_ : "No image");
    draw_text_ellipsis(s, *c.font, 10, (TOP - c.font->height) / 2, title, s.width - 160, c.text);
    button_ = {0, 0, 0, 0};
    if (!pixels_) {
        draw_text(s, *c.font, 10, TOP + 12, status_[0] ? status_ : "Open a .bmp file from Files.", c.muted);
        return;
    }
    button_ = {s.width - 140, 5, 130, 26};
    fill_rect_rounded(s, button_, 8, with_alpha(c.accent, 180));
    draw_text(s, *c.font, button_.x + (button_.w - measure_text(*c.font, "Set as wallpaper")) / 2,
              button_.y + (button_.h - c.font->height) / 2, "Set as wallpaper", rgb(15, 23, 42));
    // Fit the image, keeping its shape, centred in the rest of the window.
    int aw = s.width - 16, ah = s.height - TOP - 16;
    if (aw < 1 || ah < 1) return;
    int w = img_.width, h = img_.height;
    if (w > aw || h > ah) {
        // Scale by the tighter side.
        if ((i64)w * ah > (i64)h * aw) { h = (int)((i64)h * aw / w); w = aw; }
        else { w = (int)((i64)w * ah / h); h = ah; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    Rect dst{8 + (aw - w) / 2, TOP + 8 + (ah - h) / 2, w, h};
    if (w == img_.width && h == img_.height) blit(s, dst.x, dst.y, img_);
    else blit_scaled(s, dst, img_);
    stroke_rect(s, dst.inset(-1), rgba(255, 255, 255, 30));
}
