// /dev/fb0: the screen as a file (SPEC phase 9 devfs, phase 10 access
// rule). Reading and writing address the framebuffer's bytes, row after
// row, `pitch` bytes per row; FBIO_GET_INFO describes its shape. The node
// is mode 0600: only the window server's credential may open it, so no
// other program can read or draw on the screen.
#include <boot/bootinfo.h>
#include <drivers/bga.h>
#include <drivers/fbdev.h>
#include <fs/dev.h>
#include <gui/desktop.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>

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
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    if (request == FBIO_GET_INFO) {
        FbInfo info{fb.width, fb.height, fb.pitch, fb.bpp};
        Result<void> r = copy_to_user(arg, &info, sizeof info);
        return r.ok() ? Result<i64>(0) : Result<i64>(r.error());
    }
    if (request == FBIO_SET_MODE) {
        // The window server's way to change the resolution (phase 12). Not
        // while the kernel desktop owns the screen: it would paint into a
        // buffer of the old shape.
        FbMode mode{};
        Result<void> r = copy_from_user(&mode, arg, sizeof mode);
        if (!r.ok()) return r.error();
        if (gui_active()) return Error::Busy;
        if (mode.width < 320 || mode.height < 200 || mode.width > 8192 || mode.height > 8192) return Error::Invalid;
        Result<void> m = bga_set_mode(mode.width, mode.height);
        if (!m.ok()) return m.error();
        return 0;
    }
    return Error::NotSupported;
}

// The screen's memory for a shared mapping: write-combined, like the
// kernel's own mapping, so a compositor can stream whole frames into it.
Result<paddr_t> fb_mmap(u32, u64 offset, usize len, u32* vm_flags) {
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    u64 size = fb_size();
    if (!fb.present || offset >= size || len > size - offset) return Error::Invalid;
    *vm_flags = vm::WRITE_COMBINE;
    return (paddr_t)((u64)fb.address - g_boot_info.hhdm_offset + offset);
}

const CharDeviceOps g_ops = {fb_open, fb_read, fb_write, fb_ioctl, fb_mmap};

} // namespace

void fbdev_init() { dev_register_char(dev::FB, &g_ops); }
