// Fundamental integer and size types for the kernel. Only freestanding headers
// are used; nothing here depends on a C library.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using usize = size_t;
using isize = ptrdiff_t;
using paddr_t = u64;
using vaddr_t = u64;

constexpr u64 KIB = 1024;
constexpr u64 MIB = 1024 * KIB;
constexpr u64 GIB = 1024 * MIB;
constexpr u64 PAGE_SIZE = 4096;

template <typename T> constexpr T align_down(T v, u64 a) { return (T)((u64)v & ~(a - 1)); }
template <typename T> constexpr T align_up(T v, u64 a) { return (T)(((u64)v + a - 1) & ~(a - 1)); }
template <typename T> constexpr T min(T a, T b) { return a < b ? a : b; }
template <typename T> constexpr T max(T a, T b) { return a > b ? a : b; }
