// See input.h. One ring of events per device; when it is full the oldest
// event is dropped, so a reader that stops reading cannot hold up the
// drivers. Opening a device discards what was queued before.
#include <arch/x86_64/cpu.h>
#include <drivers/input.h>
#include <drivers/refclock.h>
#include <fs/dev.h>
#include <ipc/object.h>
#include <lib/string.h>
#include <sched/sync.h>

namespace {

constexpr u32 RING = 128;

struct Device {
    input::Event ring[RING];
    u32 head, count;            // under lock
    Spinlock lock;              // unranked leaf: taken from interrupt handlers
    Semaphore available;        // one unit per queued event (may run ahead after drops)
};
Device g_devices[2];

Result<void> in_open(u32 minor) {
    if (minor > 1) return Error::NoDevice;
    Device& d = g_devices[minor];
    d.lock.lock();
    d.head = d.count = 0;
    d.lock.unlock();
    return {};
}

// Returns whole events only; blocks until at least one is available.
Result<usize> in_read(u32 minor, u64, void* buf, usize n) {
    if (minor > 1) return Error::NoDevice;
    if (n < sizeof(input::Event)) return Error::Invalid;
    Device& d = g_devices[minor];
    usize max = n / sizeof(input::Event), done = 0;
    input::Event* out = (input::Event*)buf;
    for (;;) {
        d.lock.lock();
        while (d.count && done < max) {
            out[done++] = d.ring[d.head];
            d.head = (d.head + 1) % RING;
            d.count--;
        }
        d.lock.unlock();
        if (done) return done * sizeof(input::Event);
        if (!d.available.down_interruptible()) return Error::Interrupted;
    }
}

const CharDeviceOps g_ops = {in_open, in_read, nullptr, nullptr};

} // namespace

bool input_pending(u32 dev) { return dev <= 1 && __atomic_load_n(&g_devices[dev].count, __ATOMIC_RELAXED) != 0; }

void input_init() { dev_register_char(dev::INPUT, &g_ops); }

void input_report(u32 dev, u16 type, u16 code, i32 value, u32 unicode, u16 mods) {
    if (dev > 1) return;
    Device& d = g_devices[dev];
    input::Event e{refclock_now_us(), type, code, value, unicode, mods, 0};
    d.lock.lock();
    if (d.count == RING) {      // full: the oldest goes
        d.head = (d.head + 1) % RING;
        d.count--;
    }
    d.ring[(d.head + d.count) % RING] = e;
    d.count++;
    d.lock.unlock();
    // The count may run ahead of the ring after drops; it need not run away.
    if (__atomic_load_n(&d.available.count, __ATOMIC_RELAXED) < (i64)RING) d.available.up();
    poll_wake();
}
