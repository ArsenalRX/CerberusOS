// The primitives are built on "interrupts off" as the one true lock, which is
// exact on a single CPU. Phase 8 puts a real spinlock inside each of them.
#include <arch/x86_64/cpu.h>
#include <lib/panic.h>
#include <sched/sync.h>

void Spinlock::lock() {
    u64 flags = interrupts_save();
    while (__atomic_exchange_n(&locked, 1u, __ATOMIC_ACQUIRE)) cpu_relax();
    saved_flags = flags;
}

void Spinlock::unlock() {
    u64 flags = saved_flags;
    __atomic_store_n(&locked, 0u, __ATOMIC_RELEASE);
    interrupts_restore(flags);
}

void Mutex::lock() {
    u64 irq = interrupts_save();
    ASSERT(owner != thread_current());      // not recursive
    while (owner) sched_block_locked(waiters);
    owner = thread_current();
    interrupts_restore(irq);
}

bool Mutex::try_lock() {
    u64 irq = interrupts_save();
    bool got = !owner;
    if (got) owner = thread_current();
    interrupts_restore(irq);
    return got;
}

void Mutex::unlock() {
    u64 irq = interrupts_save();
    ASSERT_ALWAYS(owner == thread_current());
    owner = nullptr;
    interrupts_restore(irq);
    waiters.wake_one();
}

void Semaphore::down() {
    u64 irq = interrupts_save();
    while (count <= 0) sched_block_locked(waiters);
    count--;
    interrupts_restore(irq);
}

bool Semaphore::try_down() {
    u64 irq = interrupts_save();
    bool got = count > 0;
    if (got) count--;
    interrupts_restore(irq);
    return got;
}

void Semaphore::up() {
    u64 irq = interrupts_save();
    count++;
    interrupts_restore(irq);
    waiters.wake_one();
}

void CondVar::wait(Mutex& m) {
    u64 irq = interrupts_save();
    ASSERT_ALWAYS(m.owner == thread_current());
    // Release the mutex and go to sleep with no window in between: with
    // interrupts off nothing can signal before this thread is on the queue.
    m.owner = nullptr;
    m.waiters.wake_one();
    sched_block_locked(waiters);
    interrupts_restore(irq);
    m.lock();
}

void CondVar::signal() { waiters.wake_one(); }
void CondVar::broadcast() { waiters.wake_all(); }

void RwLock::read_lock() {
    u64 irq = interrupts_save();
    // Wait behind any waiting writer so a stream of readers cannot starve it.
    while (writing || writers_waiting) sched_block_locked(read_waiters);
    readers++;
    interrupts_restore(irq);
}

void RwLock::read_unlock() {
    u64 irq = interrupts_save();
    ASSERT_ALWAYS(readers > 0);
    bool last = --readers == 0;
    interrupts_restore(irq);
    if (last) write_waiters.wake_one();
}

void RwLock::write_lock() {
    u64 irq = interrupts_save();
    writers_waiting++;
    while (writing || readers) sched_block_locked(write_waiters);
    writers_waiting--;
    writing = true;
    interrupts_restore(irq);
}

void RwLock::write_unlock() {
    u64 irq = interrupts_save();
    ASSERT_ALWAYS(writing);
    writing = false;
    bool writer_next = writers_waiting > 0;
    interrupts_restore(irq);
    if (writer_next) write_waiters.wake_one();
    else read_waiters.wake_all();
}
