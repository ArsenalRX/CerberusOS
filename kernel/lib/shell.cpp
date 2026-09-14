// Polled serial line editor plus built-in commands. Tests are dispatched to the
// registry in tests/kernel/ktest.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/lapic.h>
#include <drivers/ps2kbd.h>
#include <drivers/refclock.h>
#include <drivers/serial.h>
#include <kernel/ktest.h>
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
int cmd_panic(int, char**);
int cmd_halt(int, char**);
int cmd_reboot(int, char**);

bool g_idle_spin = false;       // shell waits with pause instead of hlt (diagnostic)

const ShellCommandEntry COMMANDS[] = {
    {"help", "list commands", cmd_help},
    {"ticks", "show the timer tick count and uptime", cmd_ticks},
    {"mem", "print the boot memory map", cmd_mem},
    {"sym", "sym <hex-address>: resolve an address to a symbol", cmd_sym},
    {"test", "test <name|all> [args]: run a kernel self-test", cmd_test},
    {"idle", "idle hlt|spin: how the shell waits for input (diagnostic)", cmd_idle},
    {"timermode", "timermode periodic|oneshot: APIC timer mode (diagnostic)", cmd_timermode},
    {"panic", "trigger a kernel panic", cmd_panic},
    {"halt", "halt the CPU", cmd_halt},
    {"reboot", "reset the machine", cmd_reboot},
};

int cmd_idle(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "spin") == 0) g_idle_spin = true;
    else if (argc > 1 && strcmp(argv[1], "hlt") == 0) g_idle_spin = false;
    kprintf("idle: %s\n", g_idle_spin ? "spin" : "hlt");
    return 0;
}

int cmd_timermode(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "oneshot") == 0) lapic_timer_set_rearm_mode(true);
    else if (argc > 1 && strcmp(argv[1], "periodic") == 0) lapic_timer_set_rearm_mode(false);
    kprintf("timermode: %s\n", lapic_timer_rearm_mode() ? "oneshot (re-armed per tick)" : "periodic");
    return 0;
}

int cmd_help(int, char**) {
    for (const auto& c : COMMANDS) kprintf("  %-8s %s\n", c.name, c.help);
    kprintf("tests:\n");
    for (usize i = 0; i < ktest_count(); i++) kprintf("  test %-8s %s\n", ktests()[i].name, ktests()[i].help);
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
            if (t.halts) continue;      // destructive tests only run by name
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
    // Pulse the keyboard controller reset line; fall back to a triple fault.
    asm volatile("cli");
    asm volatile("outb %0, %1" ::"a"((u8)0xFE), "Nd"((u16)0x64));
    struct __attribute__((packed)) { u16 limit; u64 base; } null_idt{0, 0};
    asm volatile("lidt %0; int3" ::"m"(null_idt));
    halt_forever();
}

int read_line(char* buf, usize cap) {
    usize n = 0;
    for (;;) {
        int c = serial_getc();
        if (c < 0) c = ps2kbd_getc();
        if (c < 0) {
            fbconsole_flush();
            if (g_idle_spin) cpu_relax();
            else cpu_halt();    // the timer tick or a key press wakes us to poll again
            continue;
        }
        if (c == '\r' || c == '\n') {
            kprintf("\n");
            buf[n] = 0;
            return (int)n;
        }
        if (c == 0x7F || c == 0x08) {
            if (n) {
                n--;
                kprintf("\b \b");
            }
            continue;
        }
        if (c == 0x15) {        // Ctrl-U clears the line
            while (n) { n--; kprintf("\b \b"); }
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
    kprintf("lumen kernel shell. type 'help' for commands.\n");
    for (;;) {
        kprintf("lumen> ");
        read_line(line, sizeof line);
        int argc = split_args(line, argv, ARGV_MAX);
        if (!argc) continue;
        bool found = false;
        for (const auto& c : COMMANDS) {
            if (strcmp(c.name, argv[0]) == 0) {
                found = true;
                int rc = c.fn(argc, argv);
                if (rc) kprintf("(exit %d)\n", rc);
                break;
            }
        }
        if (!found) kprintf("unknown command '%s'\n", argv[0]);
    }
}
