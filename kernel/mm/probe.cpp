// C++ side of the memory probes: matches a faulting RIP against the probe
// instructions in probe_stubs.asm and redirects it to the matching fixup.
#include <mm/probe.h>

extern "C" {
int probe_read_raw(const void* addr, u8* out);
int probe_write_raw(void* addr, u8 value);
int probe_exec_raw(const void* addr);
extern const u8 probe_read_insn[], probe_read_fixup[];
extern const u8 probe_write_insn[], probe_write_fixup[];
extern const u8 probe_exec_fixup[];
}

namespace {
ProbeFault g_last;
vaddr_t g_exec_target = 0;
bool g_exec_armed = false;
} // namespace

bool probe_read(const void* addr, u8* out) {
    g_last = {};
    return probe_read_raw(addr, out) != 0;
}

bool probe_write(void* addr, u8 value) {
    g_last = {};
    return probe_write_raw(addr, value) != 0;
}

bool probe_exec(const void* addr) {
    g_last = {};
    g_exec_target = (vaddr_t)addr;
    g_exec_armed = true;
    bool ok = probe_exec_raw(addr) != 0;
    g_exec_armed = false;
    return ok;
}

ProbeFault probe_last_fault() { return g_last; }

bool probe_fixup(InterruptFrame* f, vaddr_t addr, u64 error) {
    if (f->rip == (u64)probe_read_insn) {
        f->rip = (u64)probe_read_fixup;
    } else if (f->rip == (u64)probe_write_insn) {
        f->rip = (u64)probe_write_fixup;
    } else if (g_exec_armed && f->rip == g_exec_target && addr == g_exec_target) {
        // The call pushed a return address before the fetch faulted; drop it
        // and resume in the fixup as if the call had never happened.
        f->rip = (u64)probe_exec_fixup;
        f->rsp += 8;
    } else {
        return false;
    }
    g_last = {addr, error, true};
    return true;
}
