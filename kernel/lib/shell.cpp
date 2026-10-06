// Polled serial line editor plus built-in commands. Tests are dispatched to the
// registry in tests/kernel/ktest.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <boot/bootinfo.h>
#include <drivers/lapic.h>
#include <drivers/refclock.h>
#include <gui/desktop.h>
#include <mm/kheap.h>
#include <arch/x86_64/power.h>
#include <drivers/driver.h>
#include <drivers/pci.h>
#include <fs/file.h>
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <kernel/kbench.h>
#include <kernel/ktest.h>
#include <lib/console.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/shell.h>
#include <lib/string.h>
#include <lib/symbols.h>
#include <mm/pmm.h>

namespace {

constexpr usize LINE_MAX = 256;
constexpr int ARGV_MAX = 16;

int cmd_help(int, char**);
int cmd_ticks(int, char**);
int cmd_mem(int, char**);
int cmd_sym(int argc, char** argv);
int cmd_test(int argc, char** argv);
int cmd_idle(int argc, char** argv);
int cmd_timermode(int argc, char** argv);
int cmd_gui(int argc, char** argv);
int cmd_resolution(int argc, char** argv);
int cmd_notify(int argc, char** argv);
int cmd_clear(int argc, char** argv);
int cmd_irqs(int argc, char** argv);
int cmd_heapstat(int, char**);
int cmd_ps(int, char**);
int cmd_bench(int, char**);
int cmd_run(int argc, char** argv);
int cmd_cd(int argc, char** argv);
int cmd_pwd(int, char**);
int cmd_mount(int argc, char** argv);
int cmd_pci(int, char**);
int cmd_drivers(int, char**);
int cmd_runas(int argc, char** argv);
int cmd_poweroff(int, char**);
int cmd_panic(int, char**);
int cmd_halt(int, char**);
int cmd_reboot(int, char**);

// The table `help` prints: in groups, each command with its arguments and
// one short line. An entry without a function is a group heading.
const ShellCommandEntry COMMANDS[] = {
    {"Files and programs", nullptr, nullptr, nullptr},
    {"run", "<path> [args]", "start a program and wait for it", cmd_run},
    {"runas", "<uid> <program> [args]", "start a program as another user", cmd_runas},
    {"cd", "[dir]", "change the working directory", cmd_cd},
    {"pwd", "", "print the working directory", cmd_pwd},
    {"mount", "", "list mounted file systems", cmd_mount},

    {"System", nullptr, nullptr, nullptr},
    {"help", "[command|tests]", "this list, or more about one entry", cmd_help},
    {"ticks", "", "uptime and the timer tick count", cmd_ticks},
    {"ps", "", "list threads and their CPU time", cmd_ps},
    {"mem", "", "the boot memory map", cmd_mem},
    {"heapstat", "", "kernel heap usage", cmd_heapstat},
    {"pci", "", "list PCI devices", cmd_pci},
    {"drivers", "", "list drivers and their devices", cmd_drivers},
    {"irqs", "", "interrupt counts", cmd_irqs},

    {"Desktop", nullptr, nullptr, nullptr},
    {"resolution", "[<width> <height>]", "show or change the screen resolution", cmd_resolution},
    {"gui", "", "compositor statistics", cmd_gui},
    {"notify", "<text...>", "show a notification on the desktop", cmd_notify},
    {"clear", "", "clear the terminal (also Ctrl+L)", cmd_clear},

    {"Power", nullptr, nullptr, nullptr},
    {"poweroff", "", "save everything and switch off", cmd_poweroff},
    {"reboot", "", "restart the machine", cmd_reboot},
    {"halt", "", "stop the CPU", cmd_halt},

    {"Testing and diagnostics", nullptr, nullptr, nullptr},
    {"test", "<name|all> [args]", "run a kernel self-test (help tests)", cmd_test},
    {"bench", "", "run the micro-benchmarks", cmd_bench},
    {"sym", "<hex-address>", "name the function at an address", cmd_sym},
    {"idle", "hlt|spin", "what idle CPUs do", cmd_idle},
    {"timermode", "periodic|oneshot", "APIC timer mode", cmd_timermode},
    {"panic", "", "trigger a kernel panic (halts)", cmd_panic},
};

int cmd_clear(int, char**) {
    if (!gui_active()) {
        kprintf("\x1b[2J\x1b[H");
        return 0;
    }
    gui_terminal_clear();
    return 0;
}

int cmd_notify(int argc, char** argv) {
    if (argc < 2) {
        kprintf("usage: notify <text...>\n");
        return 1;
    }
    // The words joined with single spaces, cut to what a card can show.
    char text[64];
    usize len = 0;
    for (int i = 1; i < argc && len < sizeof text - 1; i++) {
        if (i > 1) text[len++] = ' ';
        for (const char* p = argv[i]; *p && len < sizeof text - 1; p++) text[len++] = *p;
    }
    text[len] = 0;
    if (!gui_notify("Terminal", text)) {
        kprintf("notify: the desktop is not running\n");
        return 1;
    }
    return 0;
}

int cmd_resolution(int argc, char** argv) {
    if (!gui_active()) {
        kprintf("resolution: the desktop is not running\n");
        return 1;
    }
    u32 w = 0, h = 0;
    if (argc == 1) {
        gui_screen_size(&w, &h);
        kprintf("resolution: %ux%u\n", w, h);
        return 0;
    }
    auto number = [](const char* s, u32* out) {
        u32 v = 0;
        if (!*s) return false;
        for (; *s; s++) {
            if (*s < '0' || *s > '9' || v > 100000) return false;
            v = v * 10 + (u32)(*s - '0');
        }
        *out = v;
        return true;
    };
    if (argc != 3 || !number(argv[1], &w) || !number(argv[2], &h)) {
        kprintf("usage: resolution [<width> <height>]\n");
        return 1;
    }
    if (!gui_request_resolution(w, h)) {
        kprintf("resolution: this display cannot show %ux%u\n", w, h);
        return 1;
    }
    return 0;
}

int cmd_irqs(int, char**) {
    for (unsigned v = 0; v < 256; v++) {
        u64 n = interrupt_count((u8)v);
        if (n) kprintf("  vector %3u (%#04x): %lu\n", v, v, (unsigned long)n);
    }
    return 0;
}

int cmd_heapstat(int, char**) {
    kheap_report();
    return 0;
}

int cmd_gui(int, char**) {
    if (!gui_active()) {
        kprintf("gui: not active\n");
        return 1;
    }
    GuiStats s = gui_stats();
    kprintf("gui: %lu frames, last frame %lu us, last present %lu px (%lu written), %u windows\n",
            (unsigned long)s.frames, (unsigned long)s.last_frame_us, (unsigned long)s.last_present_pixels,
            (unsigned long)s.last_written_pixels, s.windows);
    kprintf("gui: slowest frame since the last 'gui': %lu us, %lu px written\n", (unsigned long)s.worst_frame_us,
            (unsigned long)s.worst_written_pixels);
    gui_reset_worst();
    return 0;
}

int cmd_idle(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "spin") == 0) sched_set_idle_spin(true);
    else if (argc > 1 && strcmp(argv[1], "hlt") == 0) sched_set_idle_spin(false);
    kprintf("idle: %s\n", sched_idle_spin() ? "spin" : "hlt");
    return 0;
}

int cmd_bench(int, char**) { return kbench_run(); }

// run <path> [args...]: starts a user program from the boot archive, waits
// for it, and reports how it ended.
const Credentials ROOT_CRED = {0, 0};

// Starts a program and waits. "> file" and ">> file" anywhere in the
// arguments redirect its standard output. With `verbose` (the `run`
// command) the exit status is always reported; otherwise only failures.
int run_program(const char* path, int argc, char** argv, bool verbose, const Credentials* cred = nullptr) {
    const char* args[ARGV_MAX + 1];
    int n = 0;
    File* out = nullptr;
    for (int i = 0; i < argc; i++) {
        bool append = strcmp(argv[i], ">>") == 0;
        if (append || strcmp(argv[i], ">") == 0) {
            if (i + 1 >= argc) {
                kprintf("%s: missing file name after %s\n", argv[0], argv[i]);
                if (out) file_unref(out);
                return 1;
            }
            u32 flags = abi::O_WRONLY | abi::O_CREAT | (append ? abi::O_APPEND : abi::O_TRUNC);
            Result<File*> f = file_open(process_kernel()->cwd, argv[i + 1], flags, 0644, ROOT_CRED);
            if (!f.ok()) {
                kprintf("%s: cannot write %s: %s\n", argv[0], argv[i + 1], error_name(f.error()));
                if (out) file_unref(out);
                return 1;
            }
            if (out) file_unref(out);
            out = f.value();
            i++;
            continue;
        }
        args[n++] = argv[i];
    }
    args[n] = nullptr;
    Result<Process*> p = process_spawn(path, args, false, out, cred);
    if (out) file_unref(out);
    if (!p.ok()) {
        kprintf("%s: cannot start %s: %s\n", verbose ? "run" : argv[0], path, error_name(p.error()));
        return 1;
    }
    int status = process_wait(p.value());
    if (status & 0x7F) {
        kprintf("%s: %s was killed (signal %d)\n", verbose ? "run" : argv[0], path, status & 0x7F);
        return 128 + (status & 0x7F);
    }
    if (verbose) kprintf("run: %s exited with status %d\n", path, (status >> 8) & 0xFF);
    return (status >> 8) & 0xFF;
}

int cmd_run(int argc, char** argv) {
    if (argc < 2) {
        kprintf("usage: run <path> [arguments]\n");
        return 1;
    }
    return run_program(argv[1], argc - 1, argv + 1, true);
}

// Resolves a command name to a program path: as given if it has a slash,
// else in /bin.
void program_path(const char* name, char* path, usize cap) {
    if (strchr(name, '/')) strlcpy(path, name, cap);
    else ksnprintf(path, cap, "/bin/%s", name);
}

int cmd_runas(int argc, char** argv) {
    if (argc < 3) {
        kprintf("usage: runas <uid> <program> [arguments]\n");
        return 1;
    }
    u32 uid = 0;
    for (const char* p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("runas: '%s' is not a user id\n", argv[1]);
            return 1;
        }
        uid = uid * 10 + (u32)(*p - '0');
    }
    Credentials cred{uid, uid};
    char path[PATH_MAX];
    program_path(argv[2], path, sizeof path);
    return run_program(path, argc - 2, argv + 2, false, &cred);
}

// Writes file systems out, then powers off through ACPI.
int cmd_poweroff(int, char**) {
    kprintf("powering off\n");
    vfs_sync();
    power_off();
    kprintf("poweroff: ACPI power off is not available on this machine; it is now safe to switch off\n");
    return 1;
}

int cmd_drivers(int, char**) {
    drivers_print();
    return 0;
}

int cmd_pci(int, char**) {
    pci_print();
    return 0;
}

int cmd_cd(int argc, char** argv) {
    Process* k = process_kernel();
    const char* path = argc > 1 ? argv[1] : "/";
    Result<Vnode*> v = vfs_resolve(k->cwd, path, ROOT_CRED, LookupFlags{});
    if (!v.ok()) {
        kprintf("cd: %s: %s\n", path, error_name(v.error()));
        return 1;
    }
    if (v.value()->type != VType::Dir) {
        vnode_unref(v.value());
        kprintf("cd: %s: not a directory\n", path);
        return 1;
    }
    if (k->cwd) vnode_unref(k->cwd);
    k->cwd = v.value();
    return 0;
}

int cmd_pwd(int, char**) {
    char buf[PATH_MAX];
    Process* k = process_kernel();
    Result<void> r = vfs_path_of(k->cwd ? k->cwd : vfs_root(), buf, sizeof buf);
    if (!r.ok()) {
        kprintf("pwd: %s\n", error_name(r.error()));
        return 1;
    }
    kprintf("%s\n", buf);
    return 0;
}

int cmd_mount(int argc, char** argv) {
    if (argc > 1) return run_program("/bin/mount", argc, argv, false);
    for (Mount* m = vfs_mounts(); m; m = m->next) {
        u32 f = m->flags;
        kprintf("%-14s on %-12s type %-9s (%s%s%s%s)\n", m->source, m->path, m->type, f & vfs::MNT_RDONLY ? "ro" : "rw",
                f & vfs::MNT_NOEXEC ? ",noexec" : "", f & vfs::MNT_NOSUID ? ",nosuid" : "",
                f & vfs::MNT_NODEV ? ",nodev" : "");
    }
    vfs_lock();
    PageCacheStats pc = page_cache_stats();
    vfs_unlock();
    kprintf("page cache: %lu of %lu pages, %lu dirty; %lu hits, %lu misses, %lu read ahead, %lu written back\n",
            (unsigned long)pc.pages, (unsigned long)pc.max_pages, (unsigned long)pc.dirty, (unsigned long)pc.hits,
            (unsigned long)pc.misses, (unsigned long)pc.readahead, (unsigned long)pc.writebacks);
    VfsCacheStats dc = vfs_cache_stats();
    kprintf("name cache: %u of %u entries; %lu hits, %lu misses\n", dc.entries, dc.capacity, (unsigned long)dc.hits,
            (unsigned long)dc.misses);
    return 0;
}

int cmd_ps(int, char**) {
    sched_print_threads();
    return 0;
}

int cmd_timermode(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "oneshot") == 0) lapic_timer_set_rearm_mode(true);
    else if (argc > 1 && strcmp(argv[1], "periodic") == 0) lapic_timer_set_rearm_mode(false);
    kprintf("timermode: %s\n", lapic_timer_rearm_mode() ? "oneshot (re-armed per tick)" : "periodic");
    return 0;
}

void help_line(const ShellCommandEntry& c) {
    char left[40];
    ksnprintf(left, sizeof left, "%s %s", c.name, c.args);
    kprintf("  %-30s %s\n", left, c.help);
}

int cmd_help(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "tests") == 0) {
        kprintf("Kernel self-tests (run one with: test <name>, or all with: test all)\n");
        for (usize i = 0; i < ktest_count(); i++) kprintf("  %-10s %s\n", ktests()[i].name, ktests()[i].help);
        return 0;
    }
    if (argc > 1) {
        for (const auto& c : COMMANDS)
            if (c.fn && strcmp(c.name, argv[1]) == 0) {
                help_line(c);
                return 0;
            }
        kprintf("help: no command called '%s' (a program? try: ls /bin)\n", argv[1]);
        return 1;
    }
    for (const auto& c : COMMANDS) {
        if (!c.fn) kprintf("\n%s\n", c.name);
        else help_line(c);
    }
    kprintf("\nPrograms\n  Type a program's name to run it: ls, cat, cp, mkdir, hello, ipctest ...\n"
            "  ls /bin lists them all. Add > file to save a program's output.\n");
    return 0;
}

int cmd_ticks(int, char**) {
    u64 t = lapic_timer_ticks();
    u64 us = refclock_now_us();
    kprintf("ticks=%lu (%lu.%02lu s at %u Hz); %s clock %lu.%02lu s since boot\n", (unsigned long)t,
            (unsigned long)(t / TIMER_HZ), (unsigned long)(t % TIMER_HZ * 100 / TIMER_HZ), TIMER_HZ,
            refclock_name(), (unsigned long)(us / 1000000), (unsigned long)(us % 1000000 / 10000));
    return 0;
}

int cmd_mem(int, char**) {
    const BootInfo& bi = g_boot_info;
    for (usize i = 0; i < bi.region_count; i++) {
        const MemoryRegion& r = bi.regions[i];
        kprintf("  %016lx-%016lx %10lu KiB  %s\n", (unsigned long)r.base,
                (unsigned long)(r.base + r.length - 1), (unsigned long)(r.length / KIB),
                memory_type_name(r.type));
    }
    kprintf("%lu MiB usable\n", (unsigned long)(bi.usable_bytes() / MIB));
    PmmStats s = pmm_stats();
    kprintf("pmm: %lu frames, %lu usable, %lu used (%lu MiB), %lu free (%lu MiB), largest run %lu frames\n",
            (unsigned long)s.total_frames, (unsigned long)s.usable_frames, (unsigned long)s.used_frames,
            (unsigned long)(s.used_frames * PAGE_SIZE / MIB), (unsigned long)s.free_frames,
            (unsigned long)(s.free_frames * PAGE_SIZE / MIB), (unsigned long)s.largest_free_run);
    return 0;
}

u64 parse_hex(const char* s, bool* ok) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    u64 v = 0;
    *ok = *s != 0;
    for (; *s; s++) {
        char c = *s;
        u64 d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else { *ok = false; return 0; }
        v = (v << 4) | d;
    }
    return v;
}

int cmd_sym(int argc, char** argv) {
    if (argc < 2) {
        kprintf("usage: sym <hex-address>  (%lu symbols loaded)\n", (unsigned long)symbols_count());
        return 1;
    }
    bool ok;
    u64 a = parse_hex(argv[1], &ok);
    if (!ok) {
        kprintf("bad address\n");
        return 1;
    }
    u64 off;
    const char* name = symbols_lookup(a, &off);
    if (name) kprintf("%#lx = %s+%#lx\n", (unsigned long)a, name, (unsigned long)off);
    else kprintf("%#lx: no symbol\n", (unsigned long)a);
    return 0;
}

int cmd_test(int argc, char** argv) {
    if (argc < 2) {
        kprintf("usage: test <name|all> [args]\n");
        return 1;
    }
    if (strcmp(argv[1], "all") == 0) {
        int failed = 0, ran = 0;
        for (usize i = 0; i < ktest_count(); i++) {
            const KernelTest& t = ktests()[i];
            if (t.halts || t.slow) continue;      // destructive and long tests only run by name
            kprintf("== test %s ==\n", t.name);
            int rc = t.fn(1, argv + 1);
            ran++;
            if (rc) failed++;
            kprintf("== %s: %s ==\n", t.name, rc ? "FAIL" : "PASS");
        }
        kprintf("tests: %d run, %d failed\n", ran, failed);
        return failed ? 1 : 0;
    }
    for (usize i = 0; i < ktest_count(); i++) {
        if (strcmp(ktests()[i].name, argv[1]) == 0) {
            int rc = ktests()[i].fn(argc - 1, argv + 1);
            kprintf("test %s: %s\n", argv[1], rc ? "FAIL" : "PASS");
            return rc;
        }
    }
    kprintf("unknown test '%s'\n", argv[1]);
    return 1;
}

int cmd_panic(int, char**) { PANIC("panic requested from the shell"); }

int cmd_halt(int, char**) {
    kprintf("halting\n");
    halt_forever();
}

int cmd_reboot(int, char**) {
    kprintf("rebooting\n");
    vfs_sync();
    power_reboot();
}

// The last lines entered, for Up/Down (Ctrl-P/Ctrl-N on a serial line).
constexpr int HISTORY = 16;
char g_history[HISTORY][LINE_MAX];
int g_history_count = 0, g_history_next = 0;

void history_add(const char* line) {
    if (!*line) return;
    int last = (g_history_next + HISTORY - 1) % HISTORY;
    if (g_history_count && strcmp(g_history[last], line) == 0) return;
    strlcpy(g_history[g_history_next], line, LINE_MAX);
    g_history_next = (g_history_next + 1) % HISTORY;
    if (g_history_count < HISTORY) g_history_count++;
}

// Tab: completes the command at the start of the line from the table; with
// several candidates, lists them.
void complete(char* buf, usize& n, usize cap) {
    if (n == 0 || n >= cap - 1) return;
    for (usize i = 0; i < n; i++)
        if (buf[i] == ' ') return;
    const char* only = nullptr;
    int matches = 0;
    for (const auto& c : COMMANDS) {
        if (!c.fn || strncmp(c.name, buf, n) != 0) continue;
        matches++;
        only = c.name;
    }
    if (matches == 1) {
        usize len = strlen(only);
        for (usize i = n; i < len && i + 1 < cap; i++) kprintf("%c", (buf[i] = only[i]));
        n = len < cap - 1 ? len : cap - 1;
        if (n + 1 < cap) {
            buf[n++] = ' ';
            kprintf(" ");
        }
        return;
    }
    if (matches < 2) return;
    kprintf("\n");
    for (const auto& c : COMMANDS)
        if (c.fn && strncmp(c.name, buf, n) == 0) kprintf("  %s", c.name);
    buf[n] = 0;
    kprintf("\ncerberus> %s", buf);
}

int read_line(char* buf, usize cap) {
    usize n = 0;
    int browse = g_history_count;       // where Up/Down are in the history
    for (;;) {
        int c = console_getc();
        if (c < 0) {
            console_idle();     // sleeps until the next tick
            continue;
        }
        if (c == '\r' || c == '\n') {
            kprintf("\n");
            buf[n] = 0;
            history_add(buf);
            return (int)n;
        }
        if (c == 0x7F || c == 0x08) {
            if (n) {
                n--;
                kprintf("\b \b");
            }
            continue;
        }
        if (c == 0x0C) {        // Ctrl-L: a clean screen with the line typed so far
            gui_terminal_clear();
            kprintf("cerberus> ");
            buf[n] = 0;
            kprintf("%s", buf);
            continue;
        }
        if (c == 0x15) {        // Ctrl-U clears the line
            while (n) { n--; kprintf("\b \b"); }
            continue;
        }
        if (c == 0x10 || c == 0x0E) {
            // Up and Down: replace the line with an older or newer one.
            int next = browse + (c == 0x10 ? -1 : 1);
            if (next < 0 || next > g_history_count) continue;
            browse = next;
            while (n) { n--; kprintf("\b \b"); }
            if (browse < g_history_count) {
                int slot = (g_history_next + HISTORY - g_history_count + browse) % HISTORY;
                strlcpy(buf, g_history[slot], cap);
                n = strlen(buf);
                kprintf("%s", buf);
            }
            continue;
        }
        if (c == '\t') {
            complete(buf, n, cap);
            continue;
        }
        if (c < 0x20 || c >= 0x7F) continue;
        if (n + 1 < cap) {
            buf[n++] = (char)c;
            kprintf("%c", c);
        }
    }
}

int split_args(char* line, char** argv, int max) {
    int argc = 0;
    char* p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

} // namespace

[[noreturn]] void shell_run() {
    char line[LINE_MAX];
    char* argv[ARGV_MAX];
    kprintf("cerberus kernel shell. type 'help' for commands.\n");
    for (;;) {
        kprintf("cerberus> ");
        read_line(line, sizeof line);
        int argc = split_args(line, argv, ARGV_MAX);
        if (!argc) continue;
        bool found = false;
        for (const auto& c : COMMANDS) {
            if (c.fn && strcmp(c.name, argv[0]) == 0) {
                found = true;
                int rc = c.fn(argc, argv);
                if (rc) kprintf("(exit %d)\n", rc);
                break;
            }
        }
        if (!found) {
            // Not built in: a program in /bin (or a path) of that name.
            char path[PATH_MAX];
            program_path(argv[0], path, sizeof path);
            Result<Vnode*> v = vfs_resolve(process_kernel()->cwd, path, ROOT_CRED, LookupFlags{});
            if (!v.ok()) {
                kprintf("unknown command '%s'\n", argv[0]);
                continue;
            }
            vnode_unref(v.value());
            int rc = run_program(path, argc, argv, false);
            if (rc) kprintf("(exit %d)\n", rc);
        }
    }
}
