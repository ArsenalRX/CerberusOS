// See crc32c.h. Reflected polynomial 0x82F63B78; the table is built on first
// use (a race between two first callers writes identical values).
#include <lib/crc32c.h>

namespace {

u32 g_table[256];
bool g_ready = false;

void build() {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        g_table[i] = c;
    }
    g_ready = true;
}

} // namespace

u32 crc32c(u32 crc, const void* data, usize n) {
    if (!g_ready) build();
    const u8* p = (const u8*)data;
    crc = ~crc;
    while (n--) crc = g_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
