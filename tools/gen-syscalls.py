#!/usr/bin/env python3
"""Generates the system-call reference and the user library's call numbers
from kernel/syscall/table.def, so numbers, prototypes and documentation have
one source (docs/SPEC.md §7).

Usage: gen-syscalls.py <table.def> --md <SYSCALLS.md> --header <syscall_nr.h>
"""
import argparse, re, sys

ENTRY = re.compile(r'SYSCALL\(\s*(\d+)\s*,\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)', re.S)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("table")
    ap.add_argument("--md")
    ap.add_argument("--header")
    a = ap.parse_args()

    text = open(a.table, encoding="utf-8").read()
    # Drop comment lines so a SYSCALL( mentioned in prose is not an entry.
    body = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))
    calls = [(int(n), name, proto, doc) for n, name, proto, doc in ENTRY.findall(body)]
    if not calls:
        sys.exit("gen-syscalls: no SYSCALL entries found in " + a.table)
    numbers = [c[0] for c in calls]
    if len(set(numbers)) != len(numbers):
        sys.exit("gen-syscalls: duplicate system-call number")
    if numbers != sorted(numbers):
        sys.exit("gen-syscalls: entries must be in ascending number order")

    if a.md:
        out = ["# System calls", "",
               "Generated from `kernel/syscall/table.def` by `tools/gen-syscalls.py`. Do not edit by hand.", "",
               "Convention: the `syscall` instruction; number in `rax`; arguments in `rdi`, `rsi`, `rdx`, `r10`,",
               "`r8`, `r9`; result in `rax`, where a negative value is `-errno`. `rcx` and `r11` are destroyed;",
               "every other register is preserved. Numbers never change. Only implemented calls are listed.", "",
               "| No. | Prototype | Description |", "|---|---|---|"]
        for n, name, proto, doc in calls:
            out.append(f"| {n} | `{proto}` | {doc} |")
        open(a.md, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")

    if a.header:
        out = ["// Generated from kernel/syscall/table.def by tools/gen-syscalls.py. Do not edit.",
               "#pragma once", ""]
        for n, name, proto, doc in calls:
            out.append(f"#define SYS_{name} {n}")
        out.append("")
        out.append("// Every implemented call number, for tests that walk the table.")
        out.append("#define SYS_ALL_NUMBERS { " + ", ".join(str(n) for n in numbers) + " }")
        open(a.header, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")

if __name__ == "__main__":
    main()
