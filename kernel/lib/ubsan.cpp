// Runtime for the undefined-behaviour sanitizer (debug builds, SPEC §5A
// phase 4). The compiler inserts a call to one of these handlers wherever a
// checked operation goes wrong; each one panics with the source location, so
// undefined behaviour stops the kernel at the line that caused it instead of
// silently corrupting state. This file itself is built without the
// sanitizer. Every handler is safe in any context (it never returns).
#include <lib/panic.h>
#include <lib/types.h>

namespace {

struct SourceLocation {
    const char* file;
    u32 line;
    u32 column;
};

struct TypeMismatchData {
    SourceLocation loc;
    const void* type;
    u8 log_alignment;
    u8 kind;
};

[[noreturn]] void report(const SourceLocation* loc, const char* what) {
    panic_impl(loc && loc->file ? loc->file : "<unknown>", loc ? (int)loc->line : 0,
               "undefined behaviour: %s (column %u)", what, loc ? loc->column : 0);
}

} // namespace

extern "C" {

void __ubsan_handle_add_overflow(SourceLocation* loc, u64, u64) { report(loc, "signed overflow in addition"); }
void __ubsan_handle_sub_overflow(SourceLocation* loc, u64, u64) { report(loc, "signed overflow in subtraction"); }
void __ubsan_handle_mul_overflow(SourceLocation* loc, u64, u64) { report(loc, "signed overflow in multiplication"); }
void __ubsan_handle_negate_overflow(SourceLocation* loc, u64) { report(loc, "signed overflow in negation"); }
void __ubsan_handle_divrem_overflow(SourceLocation* loc, u64, u64) {
    report(loc, "division by zero or overflow in division");
}
void __ubsan_handle_shift_out_of_bounds(SourceLocation* loc, u64, u64) {
    report(loc, "shift by a negative amount or by the type's width or more");
}
void __ubsan_handle_out_of_bounds(SourceLocation* loc, u64) { report(loc, "array index out of bounds"); }
void __ubsan_handle_load_invalid_value(SourceLocation* loc, u64) {
    report(loc, "load of a value that is invalid for its type (bool or enum)");
}
void __ubsan_handle_pointer_overflow(SourceLocation* loc, u64, u64) { report(loc, "pointer arithmetic overflow"); }
void __ubsan_handle_nonnull_arg(SourceLocation* loc) { report(loc, "null passed to a parameter declared non-null"); }
void __ubsan_handle_nonnull_return_v1(void*, SourceLocation* loc) {
    report(loc, "null returned from a function declared to return non-null");
}
void __ubsan_handle_vla_bound_not_positive(SourceLocation* loc, u64) {
    report(loc, "variable-length array with a non-positive bound");
}
void __ubsan_handle_builtin_unreachable(SourceLocation* loc) { report(loc, "reached code marked unreachable"); }
void __ubsan_handle_missing_return(SourceLocation* loc) {
    report(loc, "reached the end of a value-returning function without a return");
}
void __ubsan_handle_invalid_builtin(SourceLocation* loc) { report(loc, "invalid argument to a compiler builtin"); }

void __ubsan_handle_type_mismatch_v1(TypeMismatchData* d, u64 ptr) {
    if (!ptr) report(&d->loc, "null pointer dereference");
    if (d->log_alignment && (ptr & ((1ull << d->log_alignment) - 1))) report(&d->loc, "misaligned pointer access");
    report(&d->loc, "access to an object too small for its type");
}

} // extern "C"
