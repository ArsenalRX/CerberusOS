// Limine protocol requests. They live in their own section so the linker script
// keeps them together and the bootloader can find them; the start/end markers
// bound the region. boot_info_collect() reads the responses.
#include <boot/bootinfo.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <limine.h>

#define LIMINE_REQ __attribute__((used, section(".limine_requests")))

namespace {

__attribute__((used, section(".limine_requests_start")))
volatile u64 requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
volatile u64 requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

LIMINE_REQ volatile u64 base_revision[] = LIMINE_BASE_REVISION(3);

LIMINE_REQ volatile limine_bootloader_info_request bootloader_info_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_framebuffer_request framebuffer_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_executable_address_request kaddr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = nullptr};
LIMINE_REQ volatile limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = nullptr,
    .internal_module_count = 0, .internal_modules = nullptr};
LIMINE_REQ volatile limine_mp_request mp_req = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = nullptr, .flags = 0};

MemoryType translate(u64 t) {
    switch (t) {
    case LIMINE_MEMMAP_USABLE: return MemoryType::Usable;
    case LIMINE_MEMMAP_RESERVED: return MemoryType::Reserved;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return MemoryType::AcpiReclaimable;
    case LIMINE_MEMMAP_ACPI_NVS: return MemoryType::AcpiNvs;
    case LIMINE_MEMMAP_BAD_MEMORY: return MemoryType::Bad;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return MemoryType::BootloaderReclaimable;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return MemoryType::KernelAndModules;
    case LIMINE_MEMMAP_FRAMEBUFFER: return MemoryType::Framebuffer;
    case LIMINE_MEMMAP_RESERVED_MAPPED: return MemoryType::ReservedMapped;
    default: return MemoryType::Unknown;
    }
}

} // namespace

BootInfo g_boot_info;

bool limine_base_revision_supported() { return LIMINE_BASE_REVISION_SUPPORTED(base_revision); }

const char* memory_type_name(MemoryType t) {
    switch (t) {
    case MemoryType::Usable: return "usable";
    case MemoryType::Reserved: return "reserved";
    case MemoryType::AcpiReclaimable: return "acpi-reclaimable";
    case MemoryType::AcpiNvs: return "acpi-nvs";
    case MemoryType::Bad: return "bad";
    case MemoryType::BootloaderReclaimable: return "bootloader-reclaimable";
    case MemoryType::KernelAndModules: return "kernel+modules";
    case MemoryType::Framebuffer: return "framebuffer";
    case MemoryType::ReservedMapped: return "reserved-mapped";
    default: return "unknown";
    }
}

u64 BootInfo::usable_bytes() const {
    u64 sum = 0;
    for (usize i = 0; i < region_count; i++)
        if (regions[i].type == MemoryType::Usable) sum += regions[i].length;
    return sum;
}

u64 BootInfo::total_bytes() const {
    u64 top = 0;
    for (usize i = 0; i < region_count; i++) {
        MemoryType t = regions[i].type;
        if (t == MemoryType::Usable || t == MemoryType::BootloaderReclaimable ||
            t == MemoryType::KernelAndModules || t == MemoryType::AcpiReclaimable) {
            u64 end = regions[i].base + regions[i].length;
            if (end > top) top = end;
        }
    }
    return top;
}

void boot_info_collect() {
    BootInfo& bi = g_boot_info;
    memset(&bi, 0, sizeof(bi));

    if (bootloader_info_req.response) {
        char tmp[48];
        ksnprintf(tmp, sizeof tmp, "%s %s", bootloader_info_req.response->name,
                  bootloader_info_req.response->version);
        strlcpy(bi.bootloader, tmp, sizeof bi.bootloader);
    } else {
        strlcpy(bi.bootloader, "unknown", sizeof bi.bootloader);
    }

    if (!hhdm_req.response) PANIC("Limine: no HHDM response");
    bi.hhdm_offset = hhdm_req.response->offset;

    if (!kaddr_req.response) PANIC("Limine: no executable address response");
    bi.kernel_phys_base = kaddr_req.response->physical_base;
    bi.kernel_virt_base = kaddr_req.response->virtual_base;

    if (rsdp_req.response && rsdp_req.response->address) {
        u64 a = (u64)rsdp_req.response->address;
        // Base revision 3 hands back a physical address; older revisions an HHDM one.
        bi.rsdp = a >= bi.hhdm_offset ? a - bi.hhdm_offset : a;
    }

    if (!memmap_req.response) PANIC("Limine: no memory map response");
    auto* mm = memmap_req.response;
    if (mm->entry_count > BootInfo::MAX_REGIONS)
        PANIC("Limine: %lu memory map entries exceeds MAX_REGIONS", (unsigned long)mm->entry_count);
    bi.region_count = mm->entry_count;
    for (usize i = 0; i < bi.region_count; i++) {
        bi.regions[i].base = mm->entries[i]->base;
        bi.regions[i].length = mm->entries[i]->length;
        bi.regions[i].type = translate(mm->entries[i]->type);
    }

    if (framebuffer_req.response && framebuffer_req.response->framebuffer_count > 0) {
        auto* fb = framebuffer_req.response->framebuffers[0];
        bi.framebuffer.present = true;
        bi.framebuffer.address = fb->address;
        bi.framebuffer.width = (u32)fb->width;
        bi.framebuffer.height = (u32)fb->height;
        bi.framebuffer.pitch = (u32)fb->pitch;
        bi.framebuffer.bpp = fb->bpp;
        bi.framebuffer.red_size = fb->red_mask_size;
        bi.framebuffer.red_shift = fb->red_mask_shift;
        bi.framebuffer.green_size = fb->green_mask_size;
        bi.framebuffer.green_shift = fb->green_mask_shift;
        bi.framebuffer.blue_size = fb->blue_mask_size;
        bi.framebuffer.blue_shift = fb->blue_mask_shift;
    }

    if (module_req.response) {
        usize n = module_req.response->module_count;
        if (n > BootInfo::MAX_MODULES) n = BootInfo::MAX_MODULES;
        bi.module_count = n;
        for (usize i = 0; i < n; i++) {
            auto* f = module_req.response->modules[i];
            bi.modules[i].address = (u64)f->address;
            bi.modules[i].size = f->size;
            strlcpy(bi.modules[i].path, f->path, sizeof bi.modules[i].path);
        }
    }

    if (mp_req.response) {
        bi.bsp_lapic_id = mp_req.response->bsp_lapic_id;
        usize n = mp_req.response->cpu_count;
        if (n > BootInfo::MAX_CPUS) n = BootInfo::MAX_CPUS;
        bi.cpu_count = n;
        for (usize i = 0; i < n; i++) {
            bi.cpus[i].processor_id = mp_req.response->cpus[i]->processor_id;
            bi.cpus[i].lapic_id = mp_req.response->cpus[i]->lapic_id;
        }
    } else {
        bi.cpu_count = 1;
    }
}

namespace {

volatile u32 g_aps_parked = 0;

// Where every application processor waits until SMP bring-up (phase 8).
// Runs on the bootloader-provided stack and touches nothing but this loop
// and the counter. It halts with interrupts off rather than spinning: a
// spinning virtual CPU costs the host a whole core, and on VirtualBox's
// Hyper-V backend three of them starved the device timers until the
// keyboard stopped delivering keys. Nothing sends these CPUs an interrupt;
// phase 8 restarts them with INIT/SIPI.
[[noreturn]] void ap_park(limine_mp_info*) {
    __atomic_fetch_add(&g_aps_parked, 1, __ATOMIC_SEQ_CST);
    for (;;) asm volatile("cli; hlt");
}

} // namespace

usize boot_park_aps() {
    if (!mp_req.response) return 0;
    limine_mp_response* resp = mp_req.response;
    u32 expected = 0;
    for (u64 i = 0; i < resp->cpu_count; i++) {
        limine_mp_info* info = resp->cpus[i];
        if (info->lapic_id == resp->bsp_lapic_id) continue;
        __atomic_store_n(&info->goto_address, (limine_goto_address)ap_park, __ATOMIC_SEQ_CST);
        expected++;
    }
    // No clock is up this early; bound the wait by the time-stamp counter
    // (tens of seconds on any CPU this kernel runs on) and fail loudly.
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 start = ((u64)hi << 32) | lo;
    while (__atomic_load_n(&g_aps_parked, __ATOMIC_SEQ_CST) < expected) {
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        if ((((u64)hi << 32) | lo) - start > 60000000000ull)
            PANIC("boot: %u of %u application processors did not leave the bootloader",
                  expected - g_aps_parked, expected);
        asm volatile("pause");
    }
    return expected;
}
