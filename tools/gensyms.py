#!/usr/bin/env python3
"""Generate the embedded kernel symbol table (.ksymtab blob) from `nm -nC` output.

Usage: gensyms.py <out.bin> [nm-output-file]
With no input file an empty (header-only) table is written; that is what the
first link pass uses. Only text/weak symbols are kept, sorted by address.

Format (little-endian): "LSYM", u32 count, u32 strtab_size,
count x {u64 addr, u32 name_off, u32 pad}, string table (NUL-terminated).
"""
import struct, sys

def main():
    out = sys.argv[1]
    syms = []
    if len(sys.argv) > 2:
        seen = set()
        for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) < 3:
                continue
            addr, kind, name = parts
            if kind not in ("t", "T", "w", "W"):
                continue
            if name.startswith((".L", "isr_")) and name != "isr_common":
                continue
            a = int(addr, 16)
            if (a, name) in seen:
                continue
            seen.add((a, name))
            syms.append((a, name))
    syms.sort()

    strtab = bytearray()
    entries = bytearray()
    for a, name in syms:
        entries += struct.pack("<QII", a, len(strtab), 0)
        strtab += name.encode("utf-8") + b"\0"
    header = b"LSYM" + struct.pack("<II", len(syms), len(strtab))
    with open(out, "wb") as f:
        f.write(header + entries + strtab)
    print(f"gensyms: {len(syms)} symbols, {len(header) + len(entries) + len(strtab)} bytes -> {out}")

if __name__ == "__main__":
    main()
