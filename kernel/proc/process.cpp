// See process.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/gdt.h>
#include <fs/file.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/elf.h>
#include <proc/process.h>
#include <syscall/syscall.h>

namespace {

constexpr u64 RFLAGS_USER_START = 0x202;        // interrupts on, everything else clear
constexpr u64 ASLR_SLOTS_IMAGE = 1ull << 28;    // pages: 1 TiB of choice for the image base
constexpr u64 ASLR_SLOTS_MMAP = 1ull << 28;
constexpr u64 ASLR_SLOTS_STACK = 1ull << 22;    // pages: 16 GiB of choice for the stack top

constexpr u64 AT_NULL = 0, AT_PAGESZ = 6, AT_BASE = 7, AT_ENTRY = 9, AT_RANDOM = 25;

Process* g_init = nullptr;      // the first user process; orphans are handed to it

const char* base_name(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

// ---------------------------------------------------------- process tree --
// Interrupts off for all of these.
void link_child(Process* parent, Process* child) {
    child->parent = parent;
    child->sibling = parent->children;
    parent->children = child;
}

void unlink_child(Process* child) {
    for (Process** link = &child->parent->children; *link; link = &(*link)->sibling) {
        if (*link == child) {
            *link = child->sibling;
            break;
        }
    }
    child->sibling = nullptr;
}

// ------------------------------------------------------------ image load --
struct ExecImage {
    vaddr_t entry;
    vaddr_t sp;
    vaddr_t mmap_hint;
};

// Loads `path` into `space`, which must be empty, and builds the initial
// stack. Runs with `space` active and leaves it active on success; on
// failure the previously active space is restored (the caller disposes of
// `space`).
Result<ExecImage> exec_load(AddressSpace* space, const char* path, const ExecArgs& args) {
    const u8* file;
    usize file_size;
    u32 mode;
    Result<void> found = file_archive_lookup(path, &file, &file_size, &mode);
    if (!found.ok()) return found.error();
    if (!(mode & 0111)) return Error::Perm;             // not marked executable
    ElfImage img;
    ElfError parsed = elf_parse(file, file_size, &img);
    if (parsed != ElfError::Ok) {
        kprintf("exec: %s: %s\n", path, elf_error_name(parsed));
        return Error::NotExecutable;
    }

    AddressSpace& previous = vmm_current();
    space->activate();
    Error err = Error::None;

    vaddr_t base = PIE_BASE_MIN + csprng_below(ASLR_SLOTS_IMAGE) * PAGE_SIZE;
    for (u32 i = 0; i < img.segment_count && err == Error::None; i++) {
        const ElfSegment& s = img.segments[i];
        vaddr_t at = base + s.vaddr;
        // Map writable to fill it, then drop to the final permissions.
        err = space->mmap(at, s.mem_size, vm::WRITE, mmap_flag::FIXED).error();
        if (err == Error::None && s.file_size)
            err = copy_to_user(at + s.data_skew, file + s.file_offset, s.file_size).error();
        if (err == Error::None && !s.writable)
            err = space->mprotect(at, s.mem_size, s.executable ? vm::EXEC : 0).error();
    }

    vaddr_t stack_top = STACK_TOP_MAX - csprng_below(ASLR_SLOTS_STACK) * PAGE_SIZE;
    if (err == Error::None)
        err = space->mmap(stack_top - USER_STACK_SIZE, USER_STACK_SIZE, vm::WRITE,
                          mmap_flag::FIXED | mmap_flag::GUARD_BELOW).error();

    // Initial stack, from the top down: the strings, 16 random bytes for the
    // program's own stack protector, then the block the entry code reads:
    //   argc, argv[0..argc), NULL, envp[0..envc), NULL, auxv pairs, AT_NULL.
    vaddr_t sp = 0;
    if (err == Error::None) {
        vaddr_t strings_at = (stack_top - args.used) & ~0xFull;
        vaddr_t random_at = strings_at - 16;
        usize words = 1 + args.argc + 1 + args.envc + 1 + 2 * 5;
        sp = (random_at - words * 8) & ~0xFull;          // the ABI wants rsp 16-byte aligned at entry
        u64* block = (u64*)kzalloc(words * 8);
        u8 random[16];
        csprng_bytes(random, sizeof random);
        if (!block) {
            err = Error::NoMemory;
        } else {
            usize w = 0;
            block[w++] = args.argc;
            for (u32 i = 0; i < args.argc; i++) block[w++] = strings_at + args.offsets[i];
            block[w++] = 0;
            for (u32 i = 0; i < args.envc; i++) block[w++] = strings_at + args.offsets[EXEC_MAX_STRINGS + i];
            block[w++] = 0;
            block[w++] = AT_PAGESZ; block[w++] = PAGE_SIZE;
            block[w++] = AT_BASE;   block[w++] = base;
            block[w++] = AT_ENTRY;  block[w++] = base + img.entry;
            block[w++] = AT_RANDOM; block[w++] = random_at;
            block[w++] = AT_NULL;   block[w++] = 0;
            err = copy_to_user(strings_at, args.strings, args.used).error();
            if (err == Error::None) err = copy_to_user(random_at, random, sizeof random).error();
            if (err == Error::None) err = copy_to_user(sp, block, words * 8).error();
            kfree(block);
        }
    }

    if (err != Error::None) {
        previous.activate();
        return err == Error::Fault ? Error::NoMemory : err;     // a fault here means a frame could not be had
    }
    return ExecImage{base + img.entry, sp, MMAP_BASE_MIN + csprng_below(ASLR_SLOTS_MMAP) * PAGE_SIZE};
}

InterruptFrame user_frame(vaddr_t entry, vaddr_t sp) {
    InterruptFrame f{};
    f.rip = entry;
    f.cs = seg::UCODE;
    f.rflags = RFLAGS_USER_START;
    f.rsp = sp;
    f.ss = seg::UDATA;
    return f;
}

struct SpawnCtx {
    char path[PATH_MAX];
    ExecArgs args;
};

// First code of a process started by the kernel: load the program, become it.
void spawn_entry(void* arg) {
    SpawnCtx* ctx = (SpawnCtx*)arg;
    Process* p = thread_current()->process;
    Result<ExecImage> img = exec_load(p->space, ctx->path, ctx->args);
    Result<void> fpu = img.ok() ? thread_enable_fpu(nullptr) : Result<void>();
    if (!img.ok() || !fpu.ok()) {
        kprintf("exec: cannot start %s: %s\n", ctx->path, error_name(img.ok() ? fpu.error() : img.error()));
        exec_args_free(&ctx->args);
        kfree(ctx);
        process_exit(wait_status_exited(127));
    }
    exec_args_free(&ctx->args);
    kfree(ctx);
    p->mmap_hint = img.value().mmap_hint;
    InterruptFrame f = user_frame(img.value().entry, img.value().sp);
    enter_user(&f);
}

struct ForkCtx {
    InterruptFrame frame;
    u8 fpu[FPU_STATE_SIZE];
};

void fork_entry(void* arg) {
    ForkCtx* ctx = (ForkCtx*)arg;
    if (!thread_enable_fpu(ctx->fpu).ok()) {
        kfree(ctx);
        process_exit(wait_status_exited(127));
    }
    InterruptFrame f = ctx->frame;
    kfree(ctx);
    enter_user(&f);
}

// Interrupts off. Frees an exited child that nobody will wait for.
void reap_now(Process* child) {
    unlink_child(child);
    process_destroy(child);
}

} // namespace

// ================================================================ ExecArgs ==

Result<void> exec_args_init(ExecArgs* a) {
    memset(a, 0, sizeof *a);
    a->strings = (char*)kmalloc(EXEC_MAX_ARG_BYTES);
    if (!a->strings) return Error::NoMemory;
    return {};
}

void exec_args_free(ExecArgs* a) {
    kfree(a->strings);
    a->strings = nullptr;
}

Result<void> exec_args_add(ExecArgs* a, const char* s, bool env) {
    u32& count = env ? a->envc : a->argc;
    usize len = strlen(s) + 1;
    if (count >= EXEC_MAX_STRINGS || len > EXEC_MAX_ARG_BYTES - a->used) return Error::TooBig;
    memcpy(a->strings + a->used, s, len);
    a->offsets[(env ? EXEC_MAX_STRINGS : 0) + count++] = (u32)a->used;
    a->used += len;
    return {};
}

// ================================================================= lifetime ==

void process_init() {
    files_init();
    syscall_init();
}

Result<Process*> process_spawn(const char* path, const char* const argv[], bool auto_reap) {
    SpawnCtx* ctx = (SpawnCtx*)kzalloc(sizeof(SpawnCtx));
    if (!ctx) return Error::NoMemory;
    Error err = strlen(path) < PATH_MAX ? exec_args_init(&ctx->args).error() : Error::TooBig;
    if (err == Error::None) {
        strlcpy(ctx->path, path, sizeof ctx->path);
        for (usize i = 0; argv && argv[i] && err == Error::None; i++)
            err = exec_args_add(&ctx->args, argv[i], false).error();
        if (err == Error::None && ctx->args.argc == 0) err = exec_args_add(&ctx->args, path, false).error();
    }
    Process* p = nullptr;
    if (err == Error::None) {
        Result<Process*> made = process_create(base_name(path));
        if (made.ok()) p = made.value();
        else err = made.error();
    }
    if (err == Error::None) {
        // Standard input, output and error all start as the console.
        for (int fd = 0; fd < 3 && err == Error::None; fd++) {
            Result<File*> con = file_open_console();
            if (con.ok()) p->files[fd] = con.value();
            else err = con.error();
        }
    }
    if (err == Error::None) {
        p->auto_reap = auto_reap;
        u64 irq = interrupts_save();
        link_child(process_kernel(), p);
        if (!g_init) g_init = p;
        interrupts_restore(irq);
        Result<Thread*> t = kthread_create(spawn_entry, ctx, p->name, prio::NORMAL, p, true);
        if (t.ok()) {
            return p;
        }
        err = t.error();
        irq = interrupts_save();
        unlink_child(p);
        if (g_init == p) g_init = nullptr;
        interrupts_restore(irq);
    }
    if (p) {
        for (File*& f : p->files)
            if (f) file_unref(f);
        process_destroy(p);
    }
    if (ctx->args.strings) exec_args_free(&ctx->args);
    kfree(ctx);
    return err;
}

int process_wait(Process* child) {
    Process* self = process_kernel();
    u64 irq = interrupts_save();
    while (!child->zombie) sched_block_locked(self->child_wait);
    int status = child->exit_status;
    unlink_child(child);
    interrupts_restore(irq);
    process_destroy(child);
    return status;
}

[[noreturn]] void process_exit(int wait_status) {
    Thread* t = thread_current();
    Process* p = t->process;
    ASSERT_ALWAYS(p != process_kernel());

    for (File*& f : p->files) {
        if (f) file_unref(f);
        f = nullptr;
    }
    // Step out of the address space before tearing it down.
    vmm_kernel().activate();
    AddressSpace* space = p->space;
    p->space = nullptr;
    if (space) space->destroy();

    interrupts_disable();
    // Children go to init (or to the kernel if this is init, or init is
    // gone); ones that have already exited are freed when nobody is left to
    // collect them.
    Process* heir = (g_init && g_init != p && !g_init->zombie) ? g_init : process_kernel();
    while (Process* c = p->children) {
        p->children = c->sibling;
        c->sibling = nullptr;
        link_child(heir, c);
        if (heir == process_kernel()) {
            c->auto_reap = true;
            if (c->zombie) reap_now(c);
        }
    }
    if (heir != process_kernel()) heir->child_wait.wake_all();
    if (g_init == p) g_init = nullptr;

    p->exit_status = wait_status;
    p->zombie = true;
    // The thread outlives the process record by a moment (the reaper frees
    // it), so it moves to the kernel's thread list first.
    thread_set_process(t, process_kernel());
    if (p->auto_reap) reap_now(p);
    else p->parent->child_wait.wake_all();
    if (!t->detached) thread_detach(t);
    thread_exit(0);
}

Result<i64> process_fork(const InterruptFrame* frame) {
    Process* parent = thread_current()->process;
    ForkCtx* ctx = (ForkCtx*)kmalloc(sizeof(ForkCtx));
    if (!ctx) return Error::NoMemory;
    ctx->frame = *frame;
    ctx->frame.rax = 0;                 // what fork returns in the child
    thread_snapshot_fpu(ctx->fpu);

    Result<AddressSpace*> space = parent->space->clone();
    if (!space.ok()) {
        kfree(ctx);
        return space.error();
    }
    Result<Process*> made = process_create(parent->name, space.value());
    if (!made.ok()) {
        space.value()->destroy();
        kfree(ctx);
        return made.error();
    }
    Process* child = made.value();
    child->cred = parent->cred;
    child->mmap_hint = parent->mmap_hint;
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++)
        if (parent->files[fd]) child->files[fd] = file_ref(parent->files[fd]);
    u64 irq = interrupts_save();
    link_child(parent, child);
    interrupts_restore(irq);
    i64 pid = child->pid;

    Result<Thread*> t = kthread_create(fork_entry, ctx, child->name, prio::NORMAL, child, true);
    if (!t.ok()) {
        irq = interrupts_save();
        unlink_child(child);
        interrupts_restore(irq);
        for (File*& f : child->files)
            if (f) file_unref(f);
        process_destroy(child);
        kfree(ctx);
        return t.error();
    }
    return pid;
}

Result<void> process_exec(InterruptFrame* frame, const char* path, const ExecArgs& args) {
    Process* p = thread_current()->process;
    Result<AddressSpace*> made = AddressSpace::create();
    if (!made.ok()) return made.error();
    AddressSpace* fresh = made.value();
    Result<ExecImage> img = exec_load(fresh, path, args);
    if (!img.ok()) {
        fresh->destroy();               // exec_load put the old space back
        return img.error();
    }
    // Point of no return: the new image is loaded and its space is active.
    AddressSpace* old = p->space;
    p->space = fresh;
    old->destroy();
    p->mmap_hint = img.value().mmap_hint;
    strlcpy(p->name, base_name(path), sizeof p->name);
    strlcpy(thread_current()->name, p->name, sizeof thread_current()->name);
    thread_reset_fpu();
    *frame = user_frame(img.value().entry, img.value().sp);
    return {};
}

Result<i64> process_waitpid(i64 pid, int* status, bool nohang) {
    if (pid != -1 && pid <= 0) return Error::Invalid;
    Process* self = thread_current()->process;
    u64 irq = interrupts_save();
    for (;;) {
        bool any = false;
        for (Process* c = self->children; c; c = c->sibling) {
            if (pid != -1 && c->pid != (u32)pid) continue;
            any = true;
            if (!c->zombie) continue;
            i64 id = c->pid;
            *status = c->exit_status;
            unlink_child(c);
            interrupts_restore(irq);
            process_destroy(c);
            return id;
        }
        if (!any) {
            interrupts_restore(irq);
            return Error::NoChild;
        }
        if (nohang) {
            interrupts_restore(irq);
            return (i64)0;
        }
        sched_block_locked(self->child_wait);
    }
}

[[noreturn]] void user_exception(InterruptFrame* f) {
    Process* p = thread_current()->process;
    int sig = f->vector == 6 ? WAIT_SIGILL : (f->vector == 0 || f->vector == 16 || f->vector == 19) ? WAIT_SIGFPE
                                                                                                    : WAIT_SIGSEGV;
    if (f->vector == 14)
        kprintf("process %s (pid %u) killed: %s at %#lx, address %#lx, error %#lx\n", p->name, p->pid,
                exception_name((u8)f->vector), (unsigned long)f->rip, (unsigned long)read_cr2(),
                (unsigned long)f->error);
    else
        kprintf("process %s (pid %u) killed: %s at %#lx\n", p->name, p->pid, exception_name((u8)f->vector),
                (unsigned long)f->rip);
    interrupts_enable();
    process_exit(sig);
}
