// 16550-compatible UART on the legacy ISA ports. Divisor 1 gives 115200 baud.
#include <arch/x86_64/io.h>
#include <drivers/serial.h>

namespace {

constexpr u16 COM1 = 0x3F8;
constexpr u16 REG_DATA = 0;        // RBR/THR, DLL when DLAB=1
constexpr u16 REG_IER = 1;         // DLM when DLAB=1
constexpr u16 REG_FCR = 2;
constexpr u16 REG_LCR = 3;
constexpr u16 REG_MCR = 4;
constexpr u16 REG_LSR = 5;
constexpr u8 LSR_DATA_READY = 1 << 0;
constexpr u8 LSR_THR_EMPTY = 1 << 5;

bool g_ready = false;

} // namespace

void serial_init() {
    outb(COM1 + REG_IER, 0x00);        // no interrupts
    outb(COM1 + REG_LCR, 0x80);        // DLAB on
    outb(COM1 + REG_DATA, 0x01);       // divisor low  = 1 -> 115200
    outb(COM1 + REG_IER, 0x00);        // divisor high = 0
    outb(COM1 + REG_LCR, 0x03);        // 8 bits, no parity, 1 stop, DLAB off
    outb(COM1 + REG_FCR, 0xC7);        // FIFO on, clear, 14-byte threshold
    outb(COM1 + REG_MCR, 0x0B);        // DTR, RTS, OUT2
    g_ready = true;
}

void serial_putc(char c) {
    if (!g_ready) return;
    if (c == '\n') serial_putc('\r');
    while (!(inb(COM1 + REG_LSR) & LSR_THR_EMPTY)) asm volatile("pause");
    outb(COM1 + REG_DATA, (u8)c);
}

void serial_write(const char* s, usize n) {
    for (usize i = 0; i < n; i++) serial_putc(s[i]);
}

int serial_getc() {
    if (!g_ready || !(inb(COM1 + REG_LSR) & LSR_DATA_READY)) return -1;
    return inb(COM1 + REG_DATA);
}
