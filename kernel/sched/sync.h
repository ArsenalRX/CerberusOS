// Synchronisation primitives (SPEC phases 6 and 8). All are
// zero-initialisable, so they can be plain globals or members with no
// constructor call.
//
//   Spinlock  - short critical sections; disables interrupts while held, so
//               it may be taken from interrupt handlers. Never sleep while
//               holding one. Carries a rank (lib/lock_order.h) that debug
//               builds check on every acquisition.
//   Mutex     - sleeping lock for thread context; not recursive.
//   Semaphore - counting; up() is interrupt-safe, down() sleeps.
//   CondVar   - wait(mutex) atomically releases the mutex and sleeps.
//   RwLock    - many readers or one writer; writers are preferred so they
//               cannot starve.
//
// Mutex, CondVar::wait, Semaphore::down and RwLock may only be used from
// thread context with interrupts enabled. Their internal state is protected
// by the scheduler lock (sched.h), which is also what makes "release and go
// to sleep" a single step on any number of CPUs.
#pragma once

#include <lib/lock_order.h>
#include <lib/types.h>
#include <sched/sched.h>

struct Spinlock {
    volatile u32 locked;
    u8 rank;                    // lock_rank::*; 0 = not checked
    u32 owner;                  // holder's CPU id + 1, 0 when free
    u64 saved_flags;

    // Disables interrupts and takes the lock; unlock puts them back as they
    // were. The pair must be called by the same thread with no switch in
    // between.
    void lock();
    void unlock();
    // The lock alone, for callers that manage the interrupt flag themselves.
    // Interrupts must already be off.
    void acquire();
    void release();
    // True if the calling CPU holds it. Interrupts must be off.
    bool held_by_this_cpu() const;
};

// A ranked lock as a global:  Spinlock g_lock = SPINLOCK_RANKED(lock_rank::HEAP);
#define SPINLOCK_RANKED(r) Spinlock{0, (r), 0, 0}

// Holds a Spinlock for the lifetime of the object.
class SpinGuard {
public:
    explicit SpinGuard(Spinlock& l) : lock_(l) { lock_.lock(); }
    ~SpinGuard() { lock_.unlock(); }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    Spinlock& lock_;
};

struct Mutex {
    Thread* owner;
    WaitQueue waiters;

    void lock();
    // Takes the lock if it is free; never sleeps. Returns false otherwise.
    bool try_lock();
    // Must be called by the thread that holds the lock.
    void unlock();
};

class MutexGuard {
public:
    explicit MutexGuard(Mutex& m) : mutex_(m) { mutex_.lock(); }
    ~MutexGuard() { mutex_.unlock(); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& mutex_;
};

struct Semaphore {
    i64 count;
    WaitQueue waiters;

    // Takes one unit, sleeping until one is available.
    void down();
    bool try_down();
    // Returns one unit and wakes a waiter. Interrupt-safe.
    void up();
};

struct CondVar {
    WaitQueue waiters;

    // The caller must hold `m`. Releases it, sleeps until signalled, and
    // re-acquires it before returning. Always re-check the condition in a
    // loop.
    void wait(Mutex& m);
    void signal();
    void broadcast();
};

struct RwLock {
    i32 readers;            // threads currently reading
    bool writing;
    i32 writers_waiting;
    WaitQueue read_waiters;
    WaitQueue write_waiters;

    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();
};
