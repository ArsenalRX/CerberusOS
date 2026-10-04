// Powering the machine off and restarting it through ACPI (SPEC §5A phase
// 10: "ACPI shutdown and reboot, not the QEMU debug port").
//
// Power off: the sleep type for state S5 is read from the \_S5_ package in
// the DSDT and written with SLP_EN to the PM1 control register(s) named in
// the FADT. Only that one object is located (by its name) and decoded; the
// kernel has no AML interpreter.
// Restart: the FADT's reset register when the firmware offers one, then the
// keyboard controller's reset line, then a triple fault.
//
// The callers write file systems out first (vfs_sync); these functions do
// not.
#pragma once

// Reads the FADT and DSDT. Requires acpi_init. Prints one line.
void power_init();
// True if power_off can work (the tables had what it needs).
bool power_can_power_off();
// Powers the machine off. Returns only if that failed.
void power_off();
// Restarts the machine. Does not return.
[[noreturn]] void power_reboot();
