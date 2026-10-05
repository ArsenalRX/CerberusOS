// VirtualBox's guest device ("VMMDev", PCI 80ee:cafe). Used for one thing:
// the host's absolute mouse position, so the pointer in the desktop sits
// exactly under the host's pointer instead of drifting away from it as
// relative PS/2 motion does when the VM window is scaled or the host
// accelerates the mouse. QEMU has no such device; the desktop then stays
// on PS/2 motion.
#pragma once

#include <lib/types.h>

// Finds the device and tells the host this guest can take absolute
// positions. Requires pci_init. Harmless where there is no such device.
void vmmdev_init();
// True while the host is delivering absolute positions (mouse integration
// is on in VirtualBox).
bool vmmdev_mouse_absolute();
// The host pointer, 0..65535 across each screen axis. False if the host is
// not delivering positions. Safe from interrupt context.
bool vmmdev_mouse_position(u32* x, u32* y);
