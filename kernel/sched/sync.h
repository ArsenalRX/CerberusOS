// Synchronisation primitives (SPEC phase 6). All are zero-initialisable, so
// they can be plain globals or members with no constructor call.
//
//   Spinlock  - short critical sections; disables interrupts while held, so
//               it may be taken from interrupt handlers. Never sleep while
//               holding one.
//   Mutex     - sleeping lock for thread context; not recursive.
//   Semaphore - counting; up() is interrupt-safe, down() sleeps.
//   CondVar   - wait(mutex) atomically releases the mutex and sleeps.
//   RwLock    - many readers or one writer; writers are preferred so they
//               cannot starve.
//
// Mutex, CondVar::wait, Semaphore::down and RwLock may only be used from
// thread context with interrupts enabled.
#pragma once

#include <lib/types.h>
#include <sched/sched.h>

struct Spinlock {
    volatile u32 locked;
    u64 saved_flags;

    void lock();
    void unlock();
};

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
