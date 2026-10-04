// See virtio.h. Layouts from the virtio 1.1 specification, section 4.1 (PCI
// transport) and 2.6 (split virtqueues).
#include <drivers/virtio.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/pmm.h>

namespace virtio {

namespace {

// Common configuration offsets.
constexpr u32 C_DEVICE_FEATURE_SELECT = 0, C_DEVICE_FEATURE = 4, C_DRIVER_FEATURE_SELECT = 8, C_DRIVER_FEATURE = 12,
              C_NUM_QUEUES = 18, C_DEVICE_STATUS = 20, C_QUEUE_SELECT = 22, C_QUEUE_SIZE = 24,
              C_QUEUE_MSIX_VECTOR = 26, C_QUEUE_ENABLE = 28, C_QUEUE_NOTIFY_OFF = 30, C_QUEUE_DESC = 32,
              C_QUEUE_DRIVER = 40, C_QUEUE_DEVICE = 48;
constexpr u8 S_ACKNOWLEDGE = 1, S_DRIVER = 2, S_DRIVER_OK = 4, S_FEATURES_OK = 8, S_FAILED = 128;
constexpr u8 CAP_COMMON = 1, CAP_NOTIFY = 2, CAP_DEVICE = 4;
constexpr u16 MAX_QUEUE = 64;

inline u8 r8(volatile u8* b, u32 o) { return *(b + o); }
inline u16 r16(volatile u8* b, u32 o) { return *(volatile u16*)(b + o); }
inline u32 r32(volatile u8* b, u32 o) { return *(volatile u32*)(b + o); }
inline void w8(volatile u8* b, u32 o, u8 v) { *(b + o) = v; }
inline void w16(volatile u8* b, u32 o, u16 v) { *(volatile u16*)(b + o) = v; }
inline void w32(volatile u8* b, u32 o, u32 v) { *(volatile u32*)(b + o) = v; }
inline void w64(volatile u8* b, u32 o, u64 v) {
    w32(b, o, (u32)v);
    w32(b, o + 4, (u32)(v >> 32));
}

// Maps the structure a vendor capability describes. Null if it lives in an
// I/O BAR or is implausible.
volatile u8* map_cap(const PciDevice& pci, u8 cap, u32* length) {
    u8 bar = pci_read8(pci, (u8)(cap + 4));
    u32 offset = pci_read32(pci, (u8)(cap + 8));
    u32 len = pci_read32(pci, (u8)(cap + 12));
    if (bar > 5 || len == 0 || len > 1 * MIB) return nullptr;
    u64 base = pci_bar_address(pci, bar);
    if (!base) return nullptr;
    if (length) *length = len;
    return (volatile u8*)early_map(base + offset, len, MapCache::Uncached);
}

} // namespace

Result<void> init(Device* d, const PciDevice* pci, u64 wanted) {
    memset(d, 0, sizeof *d);
    d->pci = pci;
    for (u8 cap = pci_find_capability(*pci, 0x09); cap; cap = pci_find_capability(*pci, 0x09, cap)) {
        u8 type = pci_read8(*pci, (u8)(cap + 3));
        if (type == CAP_COMMON && !d->common) {
            u32 len = 0;
            d->common = map_cap(*pci, cap, &len);
            if (len < 56) d->common = nullptr;
        } else if (type == CAP_NOTIFY && !d->notify_base) {
            d->notify_base = map_cap(*pci, cap, nullptr);
            d->notify_multiplier = pci_read32(*pci, (u8)(cap + 16));
        } else if (type == CAP_DEVICE && !d->device_cfg) {
            d->device_cfg = map_cap(*pci, cap, &d->device_cfg_len);
        }
    }
    if (!d->common || !d->notify_base || !d->device_cfg) return Error::NoDevice;
    pci_enable_dma(*pci);

    w8(d->common, C_DEVICE_STATUS, 0);                      // reset
    for (int i = 0; i < 100000 && r8(d->common, C_DEVICE_STATUS) != 0; i++) asm volatile("pause");
    w8(d->common, C_DEVICE_STATUS, S_ACKNOWLEDGE);
    w8(d->common, C_DEVICE_STATUS, S_ACKNOWLEDGE | S_DRIVER);
    w32(d->common, C_DEVICE_FEATURE_SELECT, 0);
    u64 offered = r32(d->common, C_DEVICE_FEATURE);
    w32(d->common, C_DEVICE_FEATURE_SELECT, 1);
    offered |= (u64)r32(d->common, C_DEVICE_FEATURE) << 32;
    if (!(offered & F_VERSION_1)) return Error::NotSupported;
    d->features = offered & (wanted | F_VERSION_1);
    w32(d->common, C_DRIVER_FEATURE_SELECT, 0);
    w32(d->common, C_DRIVER_FEATURE, (u32)d->features);
    w32(d->common, C_DRIVER_FEATURE_SELECT, 1);
    w32(d->common, C_DRIVER_FEATURE, (u32)(d->features >> 32));
    w8(d->common, C_DEVICE_STATUS, S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK);
    if (!(r8(d->common, C_DEVICE_STATUS) & S_FEATURES_OK)) {
        fail(d);
        return Error::NotSupported;
    }
    return {};
}

Result<void> queue_setup(Device* d, Queue* q, u16 index) {
    memset(q, 0, sizeof *q);
    if (index >= r16(d->common, C_NUM_QUEUES)) return Error::NoDevice;
    w16(d->common, C_QUEUE_SELECT, index);
    u16 size = r16(d->common, C_QUEUE_SIZE);
    if (size == 0) return Error::NoDevice;
    if (size > MAX_QUEUE) size = MAX_QUEUE;
    while (size & (size - 1)) size &= (u16)(size - 1);      // round down to a power of two
    w16(d->common, C_QUEUE_SIZE, size);
    // One zeroed frame each: descriptors (16 bytes x 64 = 1 KiB), the driver
    // ("available") ring and the device ("used") ring all fit.
    paddr_t desc = pmm_alloc_zeroed(1), avail = pmm_alloc_zeroed(1), used = pmm_alloc_zeroed(1);
    if (desc == PMM_NO_MEMORY || avail == PMM_NO_MEMORY || used == PMM_NO_MEMORY) return Error::NoMemory;
    q->index = index;
    q->size = size;
    q->desc = (Desc*)hhdm_virt(desc);
    q->avail = (volatile u16*)hhdm_virt(avail);
    q->used = (volatile u16*)hhdm_virt(used);
    w16(d->common, C_QUEUE_MSIX_VECTOR, 0xFFFF);            // no interrupt: polled
    w64(d->common, C_QUEUE_DESC, desc);
    w64(d->common, C_QUEUE_DRIVER, avail);
    w64(d->common, C_QUEUE_DEVICE, used);
    u16 notify_off = r16(d->common, C_QUEUE_NOTIFY_OFF);
    q->notify = (volatile u16*)(d->notify_base + (u32)notify_off * d->notify_multiplier);
    q->avail[0] = 1;                                        // flags: no interrupts wanted
    w16(d->common, C_QUEUE_ENABLE, 1);
    return {};
}

void driver_ok(Device* d) {
    w8(d->common, C_DEVICE_STATUS, S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
}

void fail(Device* d) { w8(d->common, C_DEVICE_STATUS, (u8)(r8(d->common, C_DEVICE_STATUS) | S_FAILED)); }

void submit(Queue* q, u16 head) {
    u16 idx = q->avail[1];
    q->avail[2 + idx % q->size] = head;
    asm volatile("mfence" ::: "memory");
    q->avail[1] = (u16)(idx + 1);
    asm volatile("mfence" ::: "memory");
    *q->notify = q->index;
}

bool poll_used(Queue* q, u16* head) {
    u16 idx = q->used[1];
    if (idx == q->last_used) return false;
    // used ring entries start 4 bytes in: {u32 id, u32 len}.
    const volatile u32* ring = (const volatile u32*)(q->used + 2);
    u32 id = ring[(q->last_used % q->size) * 2];
    q->last_used++;
    *head = id < q->size ? (u16)id : 0xFFFF;
    return true;
}

} // namespace virtio
