// Port I/O and a few privileged instructions wrapped as inline functions.
#pragma once

#include <lib/types.h>

static inline void outb(u16 port, u8 v) { asm volatile("outb %0, %1" ::"a"(v), "Nd"(port) : "memory"); }
static inline void outw(u16 port, u16 v) { asm volatile("outw %0, %1" ::"a"(v), "Nd"(port) : "memory"); }
static inline void outl(u16 port, u32 v) { asm volatile("outl %0, %1" ::"a"(v), "Nd"(port) : "memory"); }
static inline u8 inb(u16 port) {
    u8 v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline u16 inw(u16 port) {
    u16 v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline u32 inl(u16 port) {
    u32 v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
// Roughly a microsecond of delay on real hardware; used between UART/PIC pokes.
static inline void io_wait() { outb(0x80, 0); }
// String port I/O: count 16-bit words. One instruction, so hypervisors can
// move a whole sector per exit instead of one word.
static inline void insw(u16 port, void* buf, usize count) {
    asm volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(u16 port, const void* buf, usize count) {
    asm volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}
