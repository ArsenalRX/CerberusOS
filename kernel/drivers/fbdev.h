// /dev/fb0, the screen as a character device (see fbdev.cpp).
#pragma once

#include <lib/types.h>

// ioctl on /dev/fb0: fills an FbInfo.
constexpr u32 FBIO_GET_INFO = 0x4600;
struct FbInfo {
    u32 width, height;
    u32 pitch;              // bytes per row
    u32 bpp;
};
// ioctl on /dev/fb0: switches the resolution (the display adapter must
// have the Bochs interface, and the kernel desktop must be stopped: EBUSY
// otherwise). The mapping made before stays valid; its shape changes.
constexpr u32 FBIO_SET_MODE = 0x4601;
struct FbMode {
    u32 width, height;
};

// Registers the device behind /dev/fb0.
void fbdev_init();
