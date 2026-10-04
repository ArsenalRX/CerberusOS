// Event queues (SPEC §5A phase 11): one call that sleeps until any of
// several things is ready, so servers wait instead of polling.
//
//   event_create()                     a new, empty queue (a descriptor)
//   event_ctl(ev, op, fd, &event)      add, change or remove a watch
//   event_wait(ev, out, max, timeout)  sleep until something watched is ready
//
// What can be watched: any descriptor (ports for incoming messages and
// connections, input devices for events, ...), a one-shot timer
// (fd = EVENT_FD_TIMER), and the exit of a child process
// (fd = EVENT_FD_CHILD). Watches are level-triggered: a descriptor that is
// still ready is reported again by the next wait.
#pragma once

#include <ipc/object.h>
#include <lib/result.h>
#include <lib/types.h>

namespace event {
constexpr u32 OP_ADD = 1, OP_MOD = 2, OP_DEL = 3;
constexpr i32 FD_TIMER = -2, FD_CHILD = -3;
constexpr u32 MAX_WATCHES = 64;
constexpr u64 FOREVER = ~0ull;

// Shared with user programs (userland/libc/include/cerberus.h).
struct Event {
    u32 events;             // poll::READ | WRITE | HUP | TIMER | CHILD: wanted (ctl) or ready (wait)
    i32 fd;                 // filled in by event_wait
    u64 data;               // the caller's own value, returned with the event
    u64 timeout_ms;         // FD_TIMER: fires this long after the ctl call
};
static_assert(sizeof(Event) == 24, "event layout is ABI");
} // namespace event

Result<File*> event_create();
// Errors: Invalid (not an event queue, bad op, nothing to change), BadFd,
// Exists (already watched), NoSpace (MAX_WATCHES).
Result<void> event_ctl(File* ev, u32 op, i32 fd, const event::Event& e);
// Fills `out` with up to `max` ready events and returns how many; 0 after
// the timeout. Errors: Invalid, Interrupted.
Result<u32> event_wait(File* ev, event::Event* out, u32 max, u64 timeout_ms);
