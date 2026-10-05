// See bga.h. The adapter is programmed through an index/data port pair; its
// frame buffer is the first memory region (BAR 0) of the VGA-class PCI
// device. The whole region is mapped once, so a mode switch is a handful of
// port writes.
#include <arch/x86_64/io.h>
#include <boot/bootinfo.h>
#include <drivers/bga.h>
#include <drivers/driver.h>
#include <drivers/fbconsole.h>
#include <drivers/pci.h>
#include <lib/kprintf.h>
#include <mm/vmm.h>

namespace {

constexpr u16 PORT_INDEX = 0x1CE, PORT_DATA = 0x1CF;
constexpr u16 REG_ID = 0, REG_XRES = 1, REG_YRES = 2, REG_BPP = 3, REG_ENABLE = 4, REG_VIRT_WIDTH = 6,
              REG_VIRT_HEIGHT = 7, REG_X_OFFSET = 8, REG_Y_OFFSET = 9;
constexpr u16 ID_MIN = 0xB0C0, ID_MAX = 0xB0C5;
constexpr u16 ENABLED = 0x01, LINEAR_FRAMEBUFFER = 0x40;
constexpr u64 MAP_LIMIT = 64 * MIB;         // more video memory than this is not mapped
constexpr u32 MIN_W = 640, MIN_H = 480, MAX_W = 4096, MAX_H = 2160;

bool g_available = false;
u8* g_vram = nullptr;           // the whole frame-buffer region
u64 g_vram_bytes = 0;

Driver g_driver = {"bga", "Bochs/QEMU/VirtualBox display: resolution switching", DriverBus::Platform, 0, 0, 0,
                   nullptr, nullptr, nullptr, 0, nullptr};

u16 reg_read(u16 index) {
    outw(PORT_INDEX, index);
    return inw(PORT_DATA);
}

void reg_write(u16 index, u16 value) {
    outw(PORT_INDEX, index);
    outw(PORT_DATA, value);
}

void program(u32 w, u32 h) {
    reg_write(REG_ENABLE, 0);
    reg_write(REG_XRES, (u16)w);
    reg_write(REG_YRES, (u16)h);
    reg_write(REG_BPP, 32);
    reg_write(REG_VIRT_WIDTH, (u16)w);
    reg_write(REG_VIRT_HEIGHT, (u16)h);
    reg_write(REG_X_OFFSET, 0);
    reg_write(REG_Y_OFFSET, 0);
    reg_write(REG_ENABLE, ENABLED | LINEAR_FRAMEBUFFER);
}

// Size of a 32-bit memory BAR: write ones, read back which bits stick.
u64 bar_size(const PciDevice& d, u8 offset) {
    u32 original = pci_read32(d, offset);
    pci_write32(d, offset, 0xFFFFFFFFu);
    u32 mask = pci_read32(d, offset) & ~0xFu;
    pci_write32(d, offset, original);
    return mask ? (u64)(~mask) + 1 : 0;
}

} // namespace

void bga_init() {
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    if (!fb.present || fb.bpp != 32) return;
    u16 id = reg_read(REG_ID);
    if (id < ID_MIN || id > ID_MAX) return;
    const PciDevice* vga = pci_find_class(0x03, 0x00, 0xFF);
    if (!vga || (vga->bar[0] & 1)) return;                  // BAR 0 must be memory
    u64 base = pci_bar_address(*vga, 0);
    u64 size = bar_size(*vga, 0x10);
    // The screen the bootloader set up must be this adapter's frame buffer;
    // otherwise the ports belong to a card that is not showing anything.
    Result<paddr_t> shown = vmm_kernel().translate((vaddr_t)fb.address);
    if (!base || size < 4 * MIB || !shown.ok() || shown.value() < base || shown.value() >= base + size) return;
    if (reg_read(REG_XRES) != fb.width || reg_read(REG_YRES) != fb.height) return;
    if (size > MAP_LIMIT) size = MAP_LIMIT;
    Result<vaddr_t> at = vmm_kernel().mmap_device(base, size, vm::WRITE);
    if (!at.ok()) return;
    g_vram = (u8*)at.value();
    g_vram_bytes = size;
    g_available = true;
    driver_note_attached(&g_driver);
    kprintf("bga: display adapter id %#x, %lu MiB of video memory; the resolution can be changed\n", id,
            (unsigned long)(size / MIB));
}

bool bga_available() { return g_available; }

bool bga_mode_fits(u32 width, u32 height) {
    if (!g_available || width < MIN_W || height < MIN_H || width > MAX_W || height > MAX_H) return false;
    return (u64)width * height * 4 <= g_vram_bytes;
}

Result<void> bga_set_mode(u32 width, u32 height) {
    if (!g_available) return Error::NotSupported;
    if (!bga_mode_fits(width, height)) return Error::Invalid;
    FramebufferInfo& fb = g_boot_info.framebuffer;
    u32 old_w = fb.width, old_h = fb.height;
    program(width, height);
    // The adapter clamps what it cannot do; believe only what it reports.
    if (reg_read(REG_XRES) != width || reg_read(REG_YRES) != height) {
        program(old_w, old_h);
        return Error::IO;
    }
    fb.address = g_vram;
    fb.width = width;
    fb.height = height;
    fb.pitch = width * 4;
    fbconsole_retarget(fb);         // panics and exceptions print to the new shape
    return {};
}
