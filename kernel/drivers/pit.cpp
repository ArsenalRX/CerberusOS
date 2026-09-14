// Channel 2, mode 0 (interrupt on terminal count), polled through the
// "OUT2" status bit on port 0x61.
#include <arch/x86_64/io.h>
#include <drivers/pit.h>

namespace {
constexpr u16 PIT_CH2 = 0x42;
constexpr u16 PIT_CMD = 0x43;
constexpr u16 SPEAKER_CTRL = 0x61;
} // namespace

void pit_sleep_ms(u32 ms) {
    while (ms) {
        u32 chunk = ms > 50 ? 50 : ms;
        ms -= chunk;
        u32 count = PIT_FREQUENCY_HZ * chunk / 1000;

        // Gate channel 2 on, speaker output off.
        u8 ctrl = inb(SPEAKER_CTRL);
        outb(SPEAKER_CTRL, (ctrl & ~0x02) | 0x01);
        outb(PIT_CMD, 0xB0);                      // channel 2, lo/hi byte, mode 0
        outb(PIT_CH2, (u8)count);
        outb(PIT_CH2, (u8)(count >> 8));
        // Mode 0 starts counting on the next clock after the high byte; pulse the
        // gate so the count reloads cleanly.
        ctrl = inb(SPEAKER_CTRL);
        outb(SPEAKER_CTRL, ctrl & ~0x01);
        outb(SPEAKER_CTRL, ctrl | 0x01);
        while (!(inb(SPEAKER_CTRL) & 0x20)) asm volatile("pause");
    }
}
