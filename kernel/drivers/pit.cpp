// Channel 0 is run as a free-running rate generator (mode 2, full 65536
// reload) and its count is read back with the latch command. Elapsed time
// is the difference between successive readings, so nothing depends on the
// speaker/gate port (0x61), whose "timer 2 output" bit behaved erratically
// under VirtualBox. Channel 0's IRQ 0 stays masked at the I/O APIC and PIC,
// so counting has no side effects.
#include <arch/x86_64/io.h>
#include <drivers/pit.h>

namespace {
constexpr u16 PIT_CH0 = 0x40;
constexpr u16 PIT_CMD = 0x43;
constexpr u32 RELOAD = 65536;       // programmed as 0

bool g_running = false;

u16 read_count() {
    outb(PIT_CMD, 0x00);            // latch channel 0
    u8 lo = inb(PIT_CH0);
    u8 hi = inb(PIT_CH0);
    return (u16)(lo | (hi << 8));
}

void start() {
    outb(PIT_CMD, 0x34);            // channel 0, lo/hi byte, mode 2, binary
    outb(PIT_CH0, 0);
    outb(PIT_CH0, 0);
    g_running = true;
}
} // namespace

void pit_sleep_ms(u32 ms) {
    if (!g_running) start();
    u64 wanted = (u64)PIT_FREQUENCY_HZ * ms / 1000;
    u64 elapsed = 0;
    u16 prev = read_count();
    while (elapsed < wanted) {
        u16 cur = read_count();
        // The counter counts down and wraps from 0 to 65535; each poll is far
        // shorter than a full period (55 ms), so a single modulo is enough.
        elapsed += (u32)(prev - cur) % RELOAD;
        prev = cur;
        asm volatile("pause");
    }
}
