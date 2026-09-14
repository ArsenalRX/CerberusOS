// Standard ICW1-4 initialisation sequence, then all lines masked.
#include <arch/x86_64/io.h>
#include <drivers/pic.h>

namespace {
constexpr u16 PIC1_CMD = 0x20, PIC1_DATA = 0x21;
constexpr u16 PIC2_CMD = 0xA0, PIC2_DATA = 0xA1;
constexpr u8 ICW1_INIT = 0x11;      // edge-triggered, cascade, ICW4 follows
constexpr u8 ICW4_8086 = 0x01;
} // namespace

void pic_init() {
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   // master vectors 0x20-0x27
    outb(PIC2_DATA, 0x28); io_wait();   // slave vectors 0x28-0x2F
    outb(PIC1_DATA, 0x04); io_wait();   // slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();   // slave cascade identity
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
    outb(PIC1_DATA, 0xFF);              // mask everything
    outb(PIC2_DATA, 0xFF);
}
