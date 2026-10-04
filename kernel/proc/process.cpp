// See process.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/gdt.h>
#include <fs/file.h>
#include <fs/vfs.h>
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
// The parent/child links, the zombie flags and g_init are protected by the
// scheduler lock (sched.h), which is also what a parent sleeps under while
// it waits for a child. Scheduler lock held for both of these.
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
// The program file: resolved from the process's working directory, checked
// for execute permission (and a noexec mount), read whole into memory.
Result<u8*> read_program(Process* p, const char* path, usize* size) {
    Result<Vnode*> found = vfs_resolve(p->cwd, path, p->cred, LookupFlags{});
    if (!found.ok()) return found.error();
    Vnode* v = found.value();
    Result<void> ok = v->type == VType::Dir ? Result<void>(Error::IsDir)
                      : v->type != VType::File ? Result<void>(Error::Access)
                                               : vfs_access(v, p->cred, vfs::X_OK);
    Result<u8*> data = ok.ok() ? vfs_read_all(v, EXEC_MAX_FILE, size) : Result<u8*>(ok.error());
    vnode_unref(v);
    return data;
}

Result<ExecImage> exec_load(AddressSpace* space, const char* path, const ExecArgs& args) {
    Process* p = thread_current()->process;
    usize file_size = 0;
    Result<u8*> read = read_program(p, path, &file_size);
    if (!read.ok()) return read.error();
    const u8* file = read.value();
    ElfImage img;
    ElfError parsed = elf_parse(file, file_size, &img);
    if (parsed != ElfError::Ok) {
        kprintf("exec: %s: %s\n", path, elf_error_name(parsed));
        kfree(read.value());
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
    kfree(read.value());            // the segments have been copied out of it

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

void process_init() { syscall_init(); }

Result<Process*> process_spawn(const char* path, const char* const argv[], bool auto_reap, File* out,
                               const Credentials* cred) {
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
        // Standard input, output and error start as the console, unless
        // output is redirected.
        for (int fd = 0; fd < 3 && err == Error::None; fd++) {
            if (fd == 1 && out) {
                p->files[fd] = file_ref(out);
                continue;
            }
            Result<File*> con = file_open_console();
            if (con.ok()) p->files[fd] = con.value();
            else err = con.error();
        }
        Process* k = process_kernel();
        if (err == Error::None && k->cwd) p->cwd = vnode_ref(k->cwd);
        // The standard descriptors were opened as root above; the program
        // itself runs as whoever was asked for.
        if (cred) p->cred = *cred;
    }
    if (err == Error::None) {
        p->auto_reap = auto_reap;
        u64 irq = sched_lock();
        link_child(process_kernel(), p);
        if (!g_init) g_init = p;
        sched_unlock(irq);
        Result<Thread*> t = kthread_create(spawn_entry, ctx, p->name, prio::NORMAL, p, true);
        if (t.ok()) {
            return p;
        }
        err = t.error();
        irq = sched_lock();
        unlink_child(p);
        if (g_init == p) g_init = nullptr;
        sched_unlock(irq);
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
    u64 irq = sched_lock();
    while (!child->zombie) sched_block_locked(self->child_wait);
    int status = child->exit_status;
    unlink_child(child);
    sched_unlock(irq);
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
    if (p->cwd) {
        vnode_unref(p->cwd);
        p->cwd = nullptr;
    }
    // Step out of the address space before tearing it down.
    vmm_kernel().activate();
    AddressSpace* space = p->space;
    p->space = nullptr;
    if (space) space->destroy();

    // The thread outlives the process record by a moment (the reaper frees
    // it), so it moves to the kernel's thread list first.
    thread_set_process(t, process_kernel());

    // Children go to init (or to the kernel if this is init, or init is
    // gone). Ones that have already exited and now have nobody to collect
    // them are gathered here and freed once the lock is released.
    Process* dead = nullptr;
    u64 irq = sched_lock();
    Process* heir = (g_init && g_init != p && !g_init->zombie) ? g_init : process_kernel();
    while (Process* c = p->children) {
        p->children = c->sibling;
        c->sibling = nullptr;
        if (heir == process_kernel()) {
            c->auto_reap = true;
            if (c->zombie) {
                c->sibling = dead;
                dead = c;
                continue;
            }
        }
        link_child(heir, c);
    }
    if (heir != process_kernel()) sched_wake_all_locked(heir->child_wait);
    if (g_init == p) g_init = nullptr;

    p->exit_status = wait_status;
    p->zombie = true;
    bool self_reap = p->auto_reap;
    if (self_reap) unlink_child(p);
    else sched_wake_all_locked(p->parent->child_wait);
    sched_unlock(irq);
    // From here on a waiting parent may free `p` at any moment, unless
    // nobody waits and it is this thread's to free.

    while (dead) {
        Process* c = dead;
        dead = c->sibling;
        c->sibling = nullptr;
        process_destroy(c);
    }
    if (self_reap) process_destroy(p);
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
    child->umask = parent->umask;
    child->fd_cloexec = parent->fd_cloexec;
    if (parent->cwd) child->cwd = vnode_ref(parent->cwd);
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++)
        if (parent->files[fd]) child->files[fd] = file_ref(parent->files[fd]);
    u64 irq = sched_lock();
    link_child(parent, child);
    sched_unlock(irq);
    i64 pid = child->pid;

    Result<Thread*> t = kthread_create(fork_entry, ctx, child->name, prio::NORMAL, child, true);
    if (!t.ok()) {
        irq = sched_lock();
        unlink_child(child);
        sched_unlock(irq);
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
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++)
        if ((p->fd_cloexec >> fd) & 1 && p->files[fd]) {
            file_unref(p->files[fd]);
            p->files[fd] = nullptr;
        }
    p->fd_cloexec = 0;
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
    u64 irq = sched_lock();
    for (;;) {
        bool any = false;
        for (Process* c = self->children; c; c = c->sibling) {
            if (pid != -1 && c->pid != (u32)pid) continue;
            any = true;
            if (!c->zombie) continue;
            i64 id = c->pid;
            *status = c->exit_status;
            unlink_child(c);
            sched_unlock(irq);
            process_destroy(c);
            return id;
        }
        if (!any) {
            sched_unlock(irq);
            return Error::NoChild;
        }
        if (nohang) {
            sched_unlock(irq);
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
