# Cerberus

A hybrid-kernel x86-64 operating system with a compositing desktop, and
**Spec**, the systems language written alongside it.

The full build specification is `docs/SPEC.md`. Current state is in
`docs/STATUS.md`; design rationale is in `docs/DECISIONS.md`.

## Building (WSL2 Ubuntu 24.04 or any Linux)

```sh
sudo apt install build-essential nasm xorriso qemu-system-x86 gdb python3 \
                 bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev
./toolchain/build-cross.sh     # one-off, ~15-30 min
make                           # kernel + userland + Spec compiler
make run                       # boot in QEMU (serial on stdout)
make vbox                      # boot in VirtualBox (from WSL, VM "Cerberus")
make test                      # all test suites
```
