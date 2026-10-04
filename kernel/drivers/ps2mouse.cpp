// Controller programming is done with interrupts still masked for IRQ 12 and
// polled ACKs; afterwards packets arrive in the IRQ handler and are
// assembled into 3- or 4-byte frames, resynchronising on the always-set
// bit 3 of the first byte.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/io.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <drivers/ps2.h>
#include <drivers/ps2mouse.h>
#include <lib/kprintf.h>

namespace {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS = 0x64;
constexpr u16 PORT_CMD = 0x64;
constexpr u8 IRQ_MOUSE = 12;
constexpr u8 STATUS_OUTPUT_FULL = 1 << 0;
constexpr u8 STATUS_INPUT_FULL = 1 << 1;
constexpr u8 STATUS_FROM_AUX = 1 << 5;

constexpr usize RING_SIZE = 128;
MouseEvent g_ring[RING_SIZE];
volatile usize g_head = 0, g_tail = 0;

u8 g_packet[4];
u8 g_packet_len = 0;
u8 g_packet_size = 3;
bool g_has_wheel = false;
bool g_present = false;

bool wait_input_clear() {
    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(PORT_STATUS) & STATUS_INPUT_FULL)) return true;
        asm volatile("pause");
    }
    return false;
}

bool wait_output_full() {
    for (u32 i = 0; i < 100000; i++) {
        if (inb(PORT_STATUS) & STATUS_OUTPUT_FULL) return true;
        asm volatile("pause");
    }
    return false;
}

void controller_cmd(u8 cmd) {
    wait_input_clear();
    outb(PORT_CMD, cmd);
}

// Sends a byte to the mouse and waits for its ACK (0xFA). Returns false on timeout.
bool mouse_cmd(u8 b) {
    controller_cmd(0xD4);
    wait_input_clear();
    outb(PORT_DATA, b);
    for (int tries = 0; tries < 8; tries++) {
        if (!wait_output_full()) return false;
        u8 r = inb(PORT_DATA);
        if (r == 0xFA) return true;
        if (r == 0xFE) return false;    // resend: give up, keep it simple
    }
    return false;
}

void push(const MouseEvent& e) {
    usize next = (g_head + 1) % RING_SIZE;
    if (next == g_tail) return;         // full: drop
    g_ring[g_head] = e;
    // Publish the slot only after it is filled: the reader may be on another CPU.
    __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
}

void handle_byte(u8 b) {
    if (g_packet_len == 0 && !(b & 0x08)) return;   // out of sync: wait for a header byte
    g_packet[g_packet_len++] = b;
    if (g_packet_len < g_packet_size) return;
    g_packet_len = 0;

    u8 flags = g_packet[0];
    if (flags & 0xC0) return;                        // overflow: discard
    MouseEvent e;
    int dx = g_packet[1], dy = g_packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;
    e.dx = (i16)dx;
    e.dy = (i16)-dy;                                 // PS/2 y grows upwards
    e.dz = 0;
    if (g_packet_size == 4) {
        i8 z = (i8)(g_packet[3] & 0x0F);
        if (z & 0x08) z = (i8)(z - 16);
        e.dz = (i8)-z;
    }
    e.buttons = flags & 0x07;
    push(e);
}

bool init_locked();

void irq_handler(InterruptFrame*, void*) {
    ps2_drain();
    lapic_eoi();
}

} // namespace

void ps2mouse_handle_byte(u8 b) {
    if (g_present) handle_byte(b);
}

bool ps2mouse_init() {
    // The keyboard IRQ handler would otherwise consume the controller's
    // replies as scancodes, so the whole handshake runs with interrupts off.
    u64 flags = interrupts_save();
    bool ok = init_locked();
    interrupts_restore(flags);
    return ok;
}

namespace {
bool init_locked() {
    // Enable the auxiliary port and IRQ 12 in the controller command byte.
    controller_cmd(0xA8);
    controller_cmd(0x20);
    if (!wait_output_full()) {
        kprintf("ps2mouse: controller did not answer\n");
        return false;
    }
    u8 cfg = inb(PORT_DATA);
    cfg |= 0x02;            // aux interrupt
    cfg &= ~0x20;           // aux clock enabled
    controller_cmd(0x60);
    wait_input_clear();
    outb(PORT_DATA, cfg);

    if (!mouse_cmd(0xFF)) {         // reset
        kprintf("ps2mouse: no mouse detected\n");
        return false;
    }
    // Reset replies with 0xAA (self-test ok) then device id 0x00.
    if (wait_output_full()) inb(PORT_DATA);
    if (wait_output_full()) inb(PORT_DATA);

    mouse_cmd(0xF6);                // defaults
    // IntelliMouse wheel enable: sample rates 200, 100, 80 then read the id.
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2);
    if (wait_output_full()) {
        u8 id = inb(PORT_DATA);
        if (id == 3 || id == 4) {
            g_has_wheel = true;
            g_packet_size = 4;
        }
    }
    mouse_cmd(0xF3); mouse_cmd(100);   // 100 samples/s
    mouse_cmd(0xE8); mouse_cmd(2);     // resolution 4 counts/mm
    mouse_cmd(0xF4);                   // enable streaming

    while (inb(PORT_STATUS) & STATUS_OUTPUT_FULL) inb(PORT_DATA);
    interrupt_register(vec::IRQ_BASE + IRQ_MOUSE, irq_handler);
    ioapic_unmask_irq(IRQ_MOUSE);
    g_present = true;
    kprintf("ps2mouse: streaming, %s\n", g_has_wheel ? "wheel present (4-byte packets)" : "3 buttons");
    return true;
}
} // namespace

bool ps2mouse_poll(MouseEvent* out) {
    if (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) == g_tail) return false;
    *out = g_ring[g_tail];
    __atomic_store_n(&g_tail, (g_tail + 1) % RING_SIZE, __ATOMIC_RELEASE);
    return true;
}

bool ps2mouse_has_wheel() { return g_has_wheel; }
