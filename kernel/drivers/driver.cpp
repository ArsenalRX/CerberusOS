// See driver.h.
#include <drivers/driver.h>
#include <lib/kprintf.h>

namespace {

Driver* g_drivers = nullptr;

bool matches(const Driver& d, const PciDevice& dev) {
    return d.class_code == dev.class_code && d.subclass == dev.subclass &&
           (d.prog_if == 0xFF || d.prog_if == dev.prog_if);
}

} // namespace

void driver_register(Driver* d) {
    d->next = nullptr;
    Driver** tail = &g_drivers;
    while (*tail) {
        if (*tail == d) return;
        tail = &(*tail)->next;
    }
    *tail = d;
}

void driver_note_attached(Driver* d) {
    driver_register(d);
    d->attached++;
}

void drivers_probe_all() {
    for (Driver* d = g_drivers; d; d = d->next) {
        if (d->bus != DriverBus::Platform || d->attached || !d->attach) continue;
        if (d->probe && !d->probe(nullptr)) continue;
        Result<void> r = d->attach(nullptr);
        if (r.ok()) d->attached++;
        else kprintf("driver %s: attach failed: %s\n", d->name, error_name(r.error()));
    }
    for (u32 i = 0; i < pci_count(); i++) {
        const PciDevice* dev = pci_get(i);
        for (Driver* d = g_drivers; d; d = d->next) {
            if (d->bus != DriverBus::Pci || !matches(*d, *dev)) continue;
            if (d->probe && !d->probe(dev)) continue;
            Result<void> r = d->attach(dev);
            if (r.ok()) {
                d->attached++;
                break;
            }
            kprintf("driver %s: %02x:%02x.%x: attach failed: %s\n", d->name, dev->bus, dev->dev, dev->func,
                    error_name(r.error()));
        }
    }
}

void drivers_print() {
    for (Driver* d = g_drivers; d; d = d->next)
        kprintf("  %-10s %-8s %u attached  %s\n", d->name, d->bus == DriverBus::Pci ? "pci" : "platform", d->attached,
                d->description);
}
