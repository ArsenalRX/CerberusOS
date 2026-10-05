// The Bochs/QEMU/VirtualBox display adapter ("BGA"): changing the screen
// resolution while running. Virtual machines' standard graphics cards have
// it; real graphics cards do not, and there the resolution the bootloader
// chose stays.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

// Looks for the adapter and maps its video memory. Call once, in thread
// context, after PCI has been scanned.
void bga_init();
// True if the resolution can be changed.
bool bga_available();
// Whether a 32-bit mode of this size fits the adapter's video memory.
bool bga_mode_fits(u32 width, u32 height);
// Switches to width x height at 32 bits per pixel and updates
// g_boot_info.framebuffer (address, size, pitch) to match. The screen's
// contents are undefined afterwards. Errors: NotSupported (no adapter),
// Invalid (does not fit), IO (the adapter did not take the mode; the
// previous one is restored).
Result<void> bga_set_mode(u32 width, u32 height);
