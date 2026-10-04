// Range checks and the C++ face of the user-copy stubs. See usercopy.h.

#include <mm/usercopy.h>
#include <mm/vmm.h>

extern "C" {
int usercopy_raw(void* dst, const void* src, usize n);
long userstr_raw(char* dst, const char* src, usize max);
int userclear_raw(void* dst, usize n);
extern const u8 usercopy_insn[], usercopy_fixup_target[];
extern const u8 userstr_insn[], userstr_fixup_target[];
extern const u8 userclear_insn[], userclear_fixup_target[];
}

namespace {

UsercopyFault g_last;

// True if [addr, addr + n) lies entirely in the user half and does not wrap.
inline bool user_range_ok(vaddr_t addr, usize n) {
    vaddr_t end = addr + n;
    return end >= addr && end <= USER_MAX;
}

} // namespace

Result<void> copy_from_user(void* dst, vaddr_t src, usize n) {
    if (!n) return {};
    if (!user_range_ok(src, n)) return Error::Fault;
    if (usercopy_raw(dst, (const void*)src, n)) return Error::Fault;
    return {};
}

Result<void> copy_to_user(vaddr_t dst, const void* src, usize n) {
    if (!n) return {};
    if (!user_range_ok(dst, n)) return Error::Fault;
    if (usercopy_raw((void*)dst, src, n)) return Error::Fault;
    return {};
}

Result<usize> strncpy_from_user(char* dst, vaddr_t src, usize max) {
    if (!max) return Error::TooBig;
    // Never read past the end of the user half, even if no NUL shows up.
    if (src >= USER_MAX) return Error::Fault;
    usize room = USER_MAX - src;
    bool clipped = room < max;
    long r = userstr_raw(dst, (const char*)src, clipped ? room : max);
    if (r == -1 || (r == -2 && clipped)) return Error::Fault;
    if (r == -2) return Error::TooBig;
    return (usize)r;
}

Result<void> clear_user(vaddr_t dst, usize n) {
    if (!n) return {};
    if (!user_range_ok(dst, n)) return Error::Fault;
    if (userclear_raw((void*)dst, n)) return Error::Fault;
    return {};
}

bool usercopy_is_access(u64 rip) {
    return rip == (u64)usercopy_insn || rip == (u64)userstr_insn ||
           rip == (u64)userclear_insn;
}

bool usercopy_fixup(InterruptFrame* f, vaddr_t addr, u64 error) {
    if (f->rip == (u64)usercopy_insn) f->rip = (u64)usercopy_fixup_target;
    else if (f->rip == (u64)userstr_insn) f->rip = (u64)userstr_fixup_target;
    else if (f->rip == (u64)userclear_insn) f->rip = (u64)userclear_fixup_target;
    else return false;
    g_last = {addr, error};
    return true;
}

UsercopyFault usercopy_last_fault() { return g_last; }
