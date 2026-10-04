// /dev/fb0: the screen as a file (SPEC phase 9 devfs, phase 10 access
// rule). Reading and writing address the framebuffer's bytes, row after
// row, `pitch` bytes per row; FBIO_GET_INFO describes its shape. The node
// is mode 0600: only the window server's credential may open it, so no
// other program can read or draw on the screen.
#include <boot/bootinfo.h>
#include <drivers/fbdev.h>
#include <fs/dev.h>
#include <mm/usercopy.h>

namespace {

u64 fb_size() {
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    return fb.present ? (u64)fb.pitch * fb.height : 0;
}

Result<void> fb_open(u32 minor) {
    return minor == 0 && g_boot_info.framebuffer.present ? Result<void>() : Result<void>(Error::NoDevice);
}

// Video memory is copied with plain byte moves through a volatile pointer:
// string instructions on it are pathologically slow under some hypervisors.
Result<usize> fb_read(u32, u64 off, void* buf, usize n) {
    u64 size = fb_size();
    if (off >= size) return (usize)0;
    if (n > size - off) n = (usize)(size - off);
    const volatile u8* src = (const volatile u8*)g_boot_info.framebuffer.address + off;
    u8* dst = (u8*)buf;
    for (usize i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

Result<usize> fb_write(u32, u64 off, const void* buf, usize n) {
    u64 size = fb_size();
    if (off >= size) return Error::NoSpace;
    if (n > size - off) n = (usize)(size - off);
    volatile u8* dst = (volatile u8*)g_boot_info.framebuffer.address + off;
    const u8* src = (const u8*)buf;
    for (usize i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

Result<i64> fb_ioctl(u32, u32 request, u64 arg) {
    if (request != FBIO_GET_INFO) return Error::NotSupported;
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    FbInfo info{fb.width, fb.height, fb.pitch, fb.bpp};
    Result<void> r = copy_to_user(arg, &info, sizeof info);
    return r.ok() ? Result<i64>(0) : Result<i64>(r.error());
}

const CharDeviceOps g_ops = {fb_open, fb_read, fb_write, fb_ioctl};

} // namespace

void fbdev_init() { dev_register_char(dev::FB, &g_ops); }
