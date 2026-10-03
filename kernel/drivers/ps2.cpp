// The status byte must be read before each data byte: the aux bit describes
// the byte currently in the output buffer.
#include <arch/x86_64/io.h>
#include <drivers/ps2.h>

void ps2_drain() {
    for (int i = 0; i < 64; i++) {          // bounded: a stuck controller must not hang the handler
        u8 st = inb(PS2_PORT_STATUS);
        if (!(st & PS2_STATUS_OUTPUT_FULL)) break;
        u8 b = inb(PS2_PORT_DATA);
        if (st & PS2_STATUS_FROM_AUX) ps2mouse_handle_byte(b);
        else ps2kbd_handle_byte(b);
    }
}
