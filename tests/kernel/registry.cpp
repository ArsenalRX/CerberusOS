// The single list of kernel self-tests. Add a declaration and a row here when
// a new test file appears; there are no static constructors in the kernel.
#include <kernel/ktest.h>

int ktest_exceptions(int argc, char** argv);
int ktest_kprintf(int argc, char** argv);
int ktest_timer(int argc, char** argv);
int ktest_idle(int argc, char** argv);
int ktest_pmm(int argc, char** argv);
int ktest_vmm(int argc, char** argv);

namespace {
const KernelTest TESTS[] = {
    {"kprintf", "format engine: widths, padding, lengths", ktest_kprintf, false},
    {"pmm", "physical frame allocator: random alloc/free, exhaustion", ktest_pmm, false},
    {"vmm", "virtual memory: mapping, W^X, demand paging, copy-on-write, guard pages", ktest_vmm, false},
    {"timer", "APIC timer advances at 100 Hz", ktest_timer, false},
    {"idle", "timer ticks keep arriving while the CPU is halted", ktest_idle, false},
    {"exceptions", "exceptions <de|ud|pf|pfw|gp|bp|so|ub>: trigger a fatal error (halts)", ktest_exceptions,
     true},
};
} // namespace

const KernelTest* ktests() { return TESTS; }
usize ktest_count() { return sizeof TESTS / sizeof TESTS[0]; }
