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

// Registers the device behind /dev/fb0.
void fbdev_init();
