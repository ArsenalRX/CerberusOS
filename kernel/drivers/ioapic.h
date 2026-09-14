// I/O APIC: routes global system interrupts to CPU vectors. Legacy ISA IRQ n
// is delivered on vector vec::IRQ_BASE + n, honouring the MADT interrupt
// source overrides (e.g. IRQ 0 on GSI 2, level/low for SCI). All entries
// start masked; a driver unmasks its IRQ after registering a handler.
#pragma once

#include <lib/types.h>

// Maps every I/O APIC from the MADT and programs the 16 ISA routes, masked.
// Requires acpi_init and lapic_init. Not interrupt-safe.
void ioapic_init();
void ioapic_unmask_irq(u8 irq);
void ioapic_mask_irq(u8 irq);
// Translates an ISA IRQ through the overrides; flags receives MPS INTI bits.
u32 ioapic_irq_to_gsi(u8 irq, u16* flags);
// Programs an arbitrary GSI. dest is a local APIC id.
void ioapic_route_gsi(u32 gsi, u8 vector, u32 dest, bool active_low, bool level, bool masked);
