// See vmmdev.h. Requests are small structures in guest memory; the physical
// address of one is written to the device's request port and the device
// fills in the answer before the write returns. The request block lives in
// the kernel image, which the bootloader places below 4 GiB, as the 32-bit
// port needs.
#include <arch/x86_64/io.h>
#include <drivers/driver.h>
#include <drivers/pci.h>
#include <drivers/vmmdev.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/vmm.h>
#include <lib/lock_order.h>
#include <sched/sync.h>

namespace {

constexpr u16 VENDOR = 0x80EE, DEVICE = 0xCAFE;
constexpr u32 HEADER_VERSION = 0x10001;
constexpr u32 GUEST_VERSION = 0x00010004;       // the additions interface we speak
constexpr u32 OS_UNKNOWN_64 = 0x00100;
enum : u32 { REQ_GET_MOUSE_STATUS = 1, REQ_SET_MOUSE_STATUS = 2, REQ_REPORT_GUEST_INFO = 50 };
constexpr u32 MOUSE_GUEST_CAN_ABSOLUTE = 1 << 0;
constexpr u32 MOUSE_HOST_WANTS_ABSOLUTE = 1 << 1;
constexpr u32 MOUSE_NEW_PROTOCOL = 1 << 4;      // we draw our own pointer; the host hides its own

struct __attribute__((packed)) Header {
    u32 size;
    u32 version;
    u32 type;
    i32 rc;
    u32 reserved1, reserved2;
};
struct __attribute__((packed)) GuestInfo {
    Header h;
    u32 interface_version;
    u32 os_type;
};
struct __attribute__((packed)) MouseStatus {
    Header h;
    u32 features;
    i32 x, y;
};

union Request {
    Header h;
    GuestInfo info;
    MouseStatus mouse;
};
alignas(16) Request g_request;
u32 g_request_phys = 0;
u16 g_port = 0;
bool g_present = false;
bool g_absolute = false;
Spinlock g_lock = SPINLOCK_RANKED(lock_rank::UNRANKED);   // a leaf: nothing is taken while it is held
// Listed as attached by hand (like bga): it is brought up before the
// registry probes, and a PCI entry without attach would be called as one.
Driver g_driver = {"vmmdev", "VirtualBox guest device: absolute mouse position", DriverBus::Platform, 0, 0, 0,
                   nullptr, nullptr, nullptr, 0, nullptr};

// Hands the request to the device; `size` includes the header. The lock is
// held: the mouse interrupt and the shell could ask at the same time.
bool submit(u32 type, u32 size) {
    g_request.h.size = size;
    g_request.h.version = HEADER_VERSION;
    g_request.h.type = type;
    g_request.h.rc = -1;
    g_request.h.reserved1 = g_request.h.reserved2 = 0;
    outl(g_port, g_request_phys);
    return g_request.h.rc >= 0;
}

} // namespace

void vmmdev_init() {
    const PciDevice* dev = nullptr;
    for (u32 i = 0; i < pci_count() && !dev; i++)
        if (pci_get(i)->vendor == VENDOR && pci_get(i)->device == DEVICE) dev = pci_get(i);
    if (!dev || !(dev->bar[0] & 1)) return;                 // BAR 0 is the port block
    Result<paddr_t> phys = vmm_kernel().translate((vaddr_t)&g_request);
    if (!phys.ok() || phys.value() + sizeof g_request > 0xFFFFFFFFu) {
        kprintf("vmmdev: request block not below 4 GiB; not used\n");
        return;
    }
    g_request_phys = (u32)phys.value();
    g_port = (u16)(dev->bar[0] & ~3u);
    SpinGuard guard(g_lock);
    g_request.info.interface_version = GUEST_VERSION;
    g_request.info.os_type = OS_UNKNOWN_64;
    if (!submit(REQ_REPORT_GUEST_INFO, sizeof(GuestInfo))) {
        kprintf("vmmdev: device at port %#x refused the guest report (rc %d)\n", g_port, g_request.h.rc);
        return;
    }
    g_request.mouse.features = MOUSE_GUEST_CAN_ABSOLUTE | MOUSE_NEW_PROTOCOL;
    g_request.mouse.x = g_request.mouse.y = 0;
    if (!submit(REQ_SET_MOUSE_STATUS, sizeof(MouseStatus))) {
        kprintf("vmmdev: could not ask for absolute mouse positions (rc %d)\n", g_request.h.rc);
        return;
    }
    g_present = true;
    g_request.mouse.features = 0;
    if (submit(REQ_GET_MOUSE_STATUS, sizeof(MouseStatus))) g_absolute = g_request.mouse.features & MOUSE_HOST_WANTS_ABSOLUTE;
    driver_note_attached(&g_driver);
    kprintf("vmmdev: VirtualBox guest device at port %#x; the host %s absolute mouse positions\n", g_port,
            g_absolute ? "sends" : "does not send");
}

bool vmmdev_mouse_absolute() { return g_absolute; }

bool vmmdev_mouse_position(u32* x, u32* y) {
    if (!g_present) return false;
    SpinGuard guard(g_lock);
    g_request.mouse.features = 0;
    g_request.mouse.x = g_request.mouse.y = 0;
    if (!submit(REQ_GET_MOUSE_STATUS, sizeof(MouseStatus))) return false;
    // The host turns this on and off as the user toggles mouse integration.
    g_absolute = g_request.mouse.features & MOUSE_HOST_WANTS_ABSOLUTE;
    if (!g_absolute) return false;
    i32 px = g_request.mouse.x, py = g_request.mouse.y;
    *x = (u32)(px < 0 ? 0 : px > 0xFFFF ? 0xFFFF : px);
    *y = (u32)(py < 0 ? 0 : py > 0xFFFF ? 0xFFFF : py);
    return true;
}
