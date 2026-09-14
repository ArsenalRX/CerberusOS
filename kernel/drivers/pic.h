// Legacy 8259 PIC pair. We remap it away from the exception vectors and mask
// every line; the I/O APIC delivers IRQs from then on. Kept only so a stray
// PIC interrupt cannot masquerade as a CPU exception.
#pragma once

void pic_init();
