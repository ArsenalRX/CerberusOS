// The driver model (SPEC phase 10): every driver is a Driver record in one
// registry. PCI drivers say which devices they handle; at boot each PCI
// device is offered to the matching drivers (probe, then attach). Platform
// drivers (legacy devices at fixed addresses: the IDE ports, the PS/2
// controller) are probed once.
//
// Everything a device reports is hostile input: drivers check lengths,
// counts and indices read from a device before using them (SPEC §5A).
#pragma once

#include <drivers/pci.h>
#include <lib/result.h>
#include <lib/types.h>

enum class DriverBus : u8 { Platform, Pci };

struct Driver {
    const char* name;
    const char* description;
    DriverBus bus;
    // PCI match: class, subclass, programming interface (0xFF = any).
    u8 class_code, subclass, prog_if;
    // Platform: is the device there? PCI: is this matching device really
    // ours (vendor quirks)? May be null (= yes).
    bool (*probe)(const PciDevice* dev);
    // Takes the device into use. dev is null for platform drivers.
    Result<void> (*attach)(const PciDevice* dev);
    // Lets go of it; null if the driver cannot be detached.
    void (*detach)(const PciDevice* dev);
    u32 attached;               // devices in use
    Driver* next;
};

void driver_register(Driver* d);
// Probes platform drivers, then offers every PCI device to the PCI drivers.
// Thread context (drivers may sleep).
void drivers_probe_all();
// One line per driver (the `drivers` shell command).
void drivers_print();
// For drivers brought up by hand during early boot (PS/2, timers): lists
// them in the registry as attached.
void driver_note_attached(Driver* d);
