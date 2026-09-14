// Binary search over the (address, name) table produced by tools/gensyms.py.
// Format: "LSYM", u32 count, u32 strtab_size, count x {u64 addr, u32 name_off,
// u32 pad}, then the string table. Entries are sorted by address.
#include <lib/string.h>
#include <lib/symbols.h>

extern "C" const u8 __ksymtab_start[];
extern "C" const u8 __ksymtab_end[];

namespace {

struct __attribute__((packed)) Header {
    char magic[4];
    u32 count;
    u32 strtab_size;
};

struct __attribute__((packed)) Entry {
    u64 addr;
    u32 name_off;
    u32 pad;
};

const Entry* g_entries = nullptr;
const char* g_strtab = nullptr;
usize g_count = 0;

} // namespace

void symbols_init() {
    usize size = (usize)(__ksymtab_end - __ksymtab_start);
    if (size < sizeof(Header)) return;
    const Header* h = (const Header*)__ksymtab_start;
    if (memcmp(h->magic, "LSYM", 4) != 0) return;
    if (sizeof(Header) + (usize)h->count * sizeof(Entry) + h->strtab_size > size) return;
    g_entries = (const Entry*)(__ksymtab_start + sizeof(Header));
    g_strtab = (const char*)(g_entries + h->count);
    g_count = h->count;
}

usize symbols_count() { return g_count; }

const char* symbols_lookup(u64 addr, u64* offset) {
    if (!g_count || addr < g_entries[0].addr) return nullptr;
    usize lo = 0, hi = g_count;
    while (hi - lo > 1) {
        usize mid = (lo + hi) / 2;
        if (g_entries[mid].addr <= addr) lo = mid;
        else hi = mid;
    }
    if (offset) *offset = addr - g_entries[lo].addr;
    return g_strtab + g_entries[lo].name_off;
}
