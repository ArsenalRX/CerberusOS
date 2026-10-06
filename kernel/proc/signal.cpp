// See signal.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <lib/string.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/process.h>
#include <proc/signal.h>

namespace {

// What a handler's stack holds while it runs.
struct SigFrame {
    u64 signo;
    InterruptFrame regs;
    u8 fpu[FPU_STATE_SIZE];
};

constexpr u64 RFLAGS_DF = 1 << 10;
constexpr usize RED_ZONE = 128;         // the ABI lets code use this much below rsp

// mov eax, 37 (sigreturn); syscall; ud2
const u8 TRAMPOLINE[] = {0xB8, 37, 0, 0, 0, 0x0F, 0x05, 0x0F, 0x0B};

// Rewrites the frame so the thread enters `handler(signo)` on its own stack
// with everything needed to resume saved below it. False if the stack
// cannot take it.
bool push_handler_frame(InterruptFrame* f, int signo, u64 handler) {
    Process* p = thread_current()->process;
    SigFrame sf;
    sf.signo = (u64)signo;
    sf.regs = *f;
    thread_snapshot_fpu(sf.fpu);
    vaddr_t at = (f->rsp - RED_ZONE - sizeof(SigFrame)) & ~0xFull;
    vaddr_t ret = at - 8;               // the handler's return address: the trampoline
    if (at > f->rsp || ret < PAGE_SIZE) return false;
    u64 trampoline = p->sig_trampoline;
    if (!copy_to_user(at, &sf, sizeof sf).ok() || !copy_to_user(ret, &trampoline, sizeof trampoline).ok()) return false;
    f->rip = handler;
    f->rsp = ret;
    f->rdi = (u64)signo;
    f->rflags &= ~RFLAGS_DF;
    return true;
}

} // namespace

void signal_post_locked(Process* p, int signo) {
    if (signo <= 0 || signo >= sig::MAX || p->zombie || p->exiting) return;
    u64 handler = signo == sig::KILL ? sig::DFL : p->sig_handlers[signo];
    if (handler == sig::IGN || (handler == sig::DFL && signo == sig::CHLD)) return;
    p->sig_pending |= 1ull << signo;
    // A thread that masks the signal is left alone; the signal waits in
    // sig_pending until some thread can take it.
    for (Thread* t = p->threads; t; t = t->proc_next)
        if (signo == sig::KILL || !(t->sig_mask & (1ull << signo))) thread_interrupt_locked(t);
}

// sigprocmask: how 0 blocks `set`, 1 unblocks it, 2 replaces the mask.
Result<u64> signal_set_mask(int how, u64 set, bool apply) {
    Thread* t = thread_current();
    u64 irq = sched_lock();
    u64 old = t->sig_mask;
    if (apply) {
        u64 m = old;
        if (how == 0) m |= set;
        else if (how == 1) m &= ~set;
        else if (how == 2) m = set;
        else {
            sched_unlock(irq);
            return Error::Invalid;
        }
        t->sig_mask = m & ~(1ull << sig::KILL) & ~1ull;
        // Something that was held back may be deliverable now.
        if (t->process->sig_pending & ~t->sig_mask) t->interrupt_pending = true;
    }
    sched_unlock(irq);
    return old;
}

Result<void> signal_send(u32 pid, int signo, const Credentials& sender) {
    if (signo < 0 || signo >= sig::MAX) return Error::Invalid;
    u64 irq = sched_lock();
    Process* p = process_find_locked(pid);
    Error err = Error::None;
    if (!p || p == process_kernel() || p->zombie) err = Error::NoProcess;
    else if (sender.uid != 0 && sender.uid != p->cred.uid) err = Error::Perm;
    else if (signo) signal_post_locked(p, signo);
    sched_unlock(irq);
    if (err != Error::None) return err;
    return {};
}

Result<u64> signal_set_handler(int signo, u64 handler) {
    if (signo <= 0 || signo >= sig::MAX || signo == sig::KILL) return Error::Invalid;
    if (handler > sig::IGN && handler >= USER_MAX) return Error::Invalid;
    Process* p = thread_current()->process;
    u64 irq = sched_lock();
    u64 old = p->sig_handlers[signo];
    p->sig_handlers[signo] = handler;
    sched_unlock(irq);
    return old;
}

i64 signal_return(InterruptFrame* f) {
    SigFrame sf;
    if (!copy_from_user(&sf, f->rsp, sizeof sf).ok()) process_exit(sig::SEGV);
    // The image came from user memory: clear the MXCSR bits that would make
    // loading it fault in the kernel. The segment and flag fields of the
    // frame are forced back to user values by the system-call exit path.
    u32 mxcsr;
    memcpy(&mxcsr, sf.fpu + 24, sizeof mxcsr);
    mxcsr &= 0xFFBF;
    memcpy(sf.fpu + 24, &mxcsr, sizeof mxcsr);
    thread_restore_fpu(sf.fpu);
    u64 vector = f->vector, error = f->error;
    *f = sf.regs;
    f->vector = vector;
    f->error = error;
    thread_current()->iret_return = true;       // every register comes back, rcx and r11 included
    return (i64)f->rax;
}

bool signal_deliver_fault(InterruptFrame* f, int signo) {
    Process* p = thread_current()->process;
    u64 irq = sched_lock();
    u64 handler = p->exiting ? sig::DFL : p->sig_handlers[signo];
    sched_unlock(irq);
    if (handler <= sig::IGN) return false;      // a fault cannot be ignored: it would just repeat
    return push_handler_frame(f, signo, handler);
}

Result<vaddr_t> signal_map_trampoline(AddressSpace* space, vaddr_t hint) {
    Result<vaddr_t> at = space->mmap(hint, PAGE_SIZE, vm::WRITE, 0);
    if (!at.ok()) return at.error();
    if (!copy_to_user(at.value(), TRAMPOLINE, sizeof TRAMPOLINE).ok()) return Error::NoMemory;
    Result<void> r = space->mprotect(at.value(), PAGE_SIZE, vm::EXEC);
    if (!r.ok()) return r.error();
    return at.value();
}

void user_return(InterruptFrame* f) {
    Thread* t = thread_current();
    if (!t->interrupt_pending) return;
    Process* p = t->process;
    u64 flags = interrupts_save();
    interrupts_enable();
    if (p == process_kernel()) {
        t->interrupt_pending = false;
        interrupts_restore(flags);
        return;
    }
    for (;;) {
        u64 irq = sched_lock();
        if (p->exiting) {
            sched_unlock(irq);
            thread_exit(0);                     // another thread is ending the process
        }
        int signo = 0;
        u64 takeable = p->sig_pending & ~t->sig_mask;
        if (takeable) {
            signo = __builtin_ctzll(takeable);
            p->sig_pending &= ~(1ull << signo);
        }
        if (!signo) {
            t->interrupt_pending = false;
            sched_unlock(irq);
            break;
        }
        u64 handler = signo == sig::KILL ? sig::DFL : p->sig_handlers[signo];
        sched_unlock(irq);
        if (handler == sig::IGN || (handler == sig::DFL && signo == sig::CHLD)) continue;
        if (handler == sig::DFL) process_exit(signo);
        if (!push_handler_frame(f, signo, handler)) process_exit(sig::SEGV);
        // One handler at a time; anything else pending is taken at the next
        // return to user mode (the flag stays set while something waits).
        irq = sched_lock();
        if (!p->sig_pending && !p->exiting) t->interrupt_pending = false;
        sched_unlock(irq);
        break;
    }
    interrupts_restore(flags);
}
