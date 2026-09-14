// HPET path is a direct counter read. PIT path accumulates elapsed counts
// between latched readings of channel 0 (mode 2, 65536 reload).
#include <arch/x86_64/io.h>
#include <drivers/hpet.h>
#include <drivers/pit.h>
#include <drivers/refclock.h>

namespace {

bool g_use_hpet = false;
u64 g_hpet_base = 0;

// PIT state
constexpr u16 PIT_CH0 = 0x40;
constexpr u16 PIT_CMD = 0x43;
u16 g_pit_prev = 0;
u64 g_pit_ticks = 0;       // total elapsed PIT ticks since refclock_init

u16 pit_read_count() {
    outb(PIT_CMD, 0x00);
    u8 lo = inb(PIT_CH0);
    u8 hi = inb(PIT_CH0);
    return (u16)(lo | (hi << 8));
}

} // namespace

void refclock_init() {
    g_use_hpet = hpet_available();
    if (g_use_hpet) {
        g_hpet_base = hpet_counter();
        return;
    }
    outb(PIT_CMD, 0x34);        // channel 0, lo/hi, mode 2 (rate generator), binary
    outb(PIT_CH0, 0);
    outb(PIT_CH0, 0);           // reload 65536
    g_pit_prev = pit_read_count();
    g_pit_ticks = 0;
}

const char* refclock_name() { return g_use_hpet ? "hpet" : "pit"; }

u64 refclock_now_us() {
    if (g_use_hpet) return hpet_delta_us(g_hpet_base, hpet_counter());
    u16 cur = pit_read_count();
    g_pit_ticks += (u32)(g_pit_prev - cur) % 65536;   // counts down, wraps at 0
    g_pit_prev = cur;
    return g_pit_ticks * 1000000 / PIT_FREQUENCY_HZ;
}

void refclock_sleep_us(u64 microseconds) {
    u64 start = refclock_now_us();
    while (refclock_now_us() - start < microseconds) asm volatile("pause");
}
