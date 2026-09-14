#!/usr/bin/env bash
# Builds an x86_64-elf cross toolchain (binutils + GCC 13.x, C and C++) into
# toolchain/out. Idempotent: exits immediately if x86_64-elf-g++ already exists.
#
# Usage: ./toolchain/build-cross.sh [--force]
# Env:   PREFIX     install dir      (default: <repo>/toolchain/out)
#        WORKDIR    scratch dir      (default: $HOME/.cache/lumen-toolchain — a
#                                     Linux-native path, because building on
#                                     /mnt/<drive> under WSL is many times slower)
#        JOBS       parallelism      (default: nproc)

set -euo pipefail

BINUTILS_VER=2.42
GCC_VER=13.3.0
TARGET=x86_64-elf

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HERE/out}"
WORKDIR="${WORKDIR:-$HOME/.cache/lumen-toolchain}"
JOBS="${JOBS:-$(nproc)}"

if [[ -x "$PREFIX/bin/$TARGET-g++" && "${1:-}" != "--force" ]]; then
    echo "cross toolchain already present at $PREFIX (use --force to rebuild)"
    exit 0
fi

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1  ->  $2"; exit 1; }; }
need gcc   "sudo apt install build-essential"
need g++   "sudo apt install build-essential"
need make  "sudo apt install build-essential"
need bison "sudo apt install bison"
need flex  "sudo apt install flex"
need texi2any "sudo apt install texinfo"
need curl  "sudo apt install curl"
for hdr in gmp.h mpfr.h mpc.h; do
    echo "#include <$hdr>" | gcc -E - >/dev/null 2>&1 \
        || { echo "missing header $hdr  ->  sudo apt install libgmp-dev libmpfr-dev libmpc-dev"; exit 1; }
done

mkdir -p "$WORKDIR" "$PREFIX"
cd "$WORKDIR"

fetch() {
    local url="$1" file="$2"
    [[ -f "$file" ]] || { echo "downloading $file"; curl -fL --retry 3 -o "$file" "$url"; }
}
fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz" "binutils-$BINUTILS_VER.tar.xz"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"     "gcc-$GCC_VER.tar.xz"

[[ -d "binutils-$BINUTILS_VER" ]] || tar xf "binutils-$BINUTILS_VER.tar.xz"
[[ -d "gcc-$GCC_VER" ]]           || tar xf "gcc-$GCC_VER.tar.xz"

export PATH="$PREFIX/bin:$PATH"

echo "== binutils $BINUTILS_VER =="
# Build dirs are reused so an interrupted run resumes instead of starting over.
mkdir -p build-binutils && cd build-binutils
[[ -f Makefile ]] || "../binutils-$BINUTILS_VER/configure" --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS"
make install
cd ..

echo "== gcc $GCC_VER =="
mkdir -p build-gcc && cd build-gcc
[[ -f Makefile ]] || "../gcc-$GCC_VER/configure" --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers \
    --disable-shared --disable-threads --disable-libssp --disable-libquadmath \
    --disable-libgomp --disable-libatomic --disable-hosted-libstdcxx
# libgcc without the red zone so it is safe to link into the kernel.
# libgcc is built -fpic, which the kernel code model rejects; the large model is
# PIC-compatible and links fine into a -mcmodel=kernel image. SSE cannot be
# disabled here (libgcc's float helpers return in XMM registers); the kernel is
# built -mno-sse so it never references those helpers. The flags must be given
# to all-gcc too: that step bakes them into gcc/libgcc.mvars.
LIBGCC_CFLAGS='-g -O2 -mcmodel=large -mno-red-zone'
make -j"$JOBS" all-gcc            CFLAGS_FOR_TARGET="$LIBGCC_CFLAGS"
make -j"$JOBS" all-target-libgcc  CFLAGS_FOR_TARGET="$LIBGCC_CFLAGS"
make install-gcc
make install-target-libgcc        CFLAGS_FOR_TARGET="$LIBGCC_CFLAGS"
cd ..

echo "cross toolchain installed: $PREFIX/bin/$TARGET-g++"
"$PREFIX/bin/$TARGET-g++" --version | head -1
