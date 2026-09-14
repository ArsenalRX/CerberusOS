// Scancode set 1 decode. Only what the kernel shell needs: printable ASCII,
// Enter, Backspace, Tab, Ctrl+letter. Everything else is dropped.
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/io.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <drivers/ps2kbd.h>

namespace {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS = 0x64;
constexpr u8 IRQ_KEYBOARD = 1;

constexpr usize RING_SIZE = 64;
volatile u8 g_ring[RING_SIZE];
volatile usize g_head = 0, g_tail = 0;

bool g_shift = false, g_ctrl = false, g_caps = false, g_extended = false;

// Index = scancode (set 1, make codes). 0 = no character.
const char MAP_LOWER[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
const char MAP_UPPER[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

void push(u8 c) {
    usize next = (g_head + 1) % RING_SIZE;
    if (next == g_tail) return;     // full: drop the key
    g_ring[g_head] = c;
    g_head = next;
}

void handle_scancode(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    bool released = sc & 0x80;
    u8 code = sc & 0x7F;
    bool extended = g_extended;
    g_extended = false;

    switch (code) {
    case 0x2A: case 0x36: g_shift = !released; return;      // left/right shift
    case 0x1D: g_ctrl = !released; return;                  // ctrl (also E0 1D)
    case 0x3A: if (!released) g_caps = !g_caps; return;     // caps lock
    default: break;
    }
    if (released || extended) return;

    char c = g_shift ? MAP_UPPER[code] : MAP_LOWER[code];
    if (!c) return;
    if (g_caps && !g_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (g_caps && g_shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (g_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)((c & 0x1F));
    push((u8)c);
}

void irq_handler(InterruptFrame*, void*) {
    while (inb(PORT_STATUS) & 1) handle_scancode(inb(PORT_DATA));
    lapic_eoi();
}

} // namespace

void ps2kbd_init() {
    // Drain anything the firmware left in the output buffer.
    while (inb(PORT_STATUS) & 1) inb(PORT_DATA);
    interrupt_register(vec::IRQ_BASE + IRQ_KEYBOARD, irq_handler);
    ioapic_unmask_irq(IRQ_KEYBOARD);
}

int ps2kbd_getc() {
    if (g_head == g_tail) return -1;
    u8 c = g_ring[g_tail];
    g_tail = (g_tail + 1) % RING_SIZE;
    return c;
}
