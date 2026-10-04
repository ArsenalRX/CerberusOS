# Top-level build for Lumen OS, the Spec compiler, and the userland.
# Run from WSL2/Linux. Targets: all kernel iso run debug gdb test clean vbox check-tools.

MAKEFLAGS += --no-builtin-rules --no-print-directory
.SUFFIXES:

# ---------------------------------------------------------------------------
# Paths and tools
# ---------------------------------------------------------------------------
ROOT      := $(abspath .)
BUILD     := $(ROOT)/build
TOOLCHAIN ?= $(ROOT)/toolchain/out
LIMINE    := $(ROOT)/toolchain/limine

CROSS     := $(TOOLCHAIN)/bin/x86_64-elf-
CXX       := $(CROSS)g++
AS        := $(CROSS)as
LD        := $(CROSS)ld
OBJCOPY   := $(CROSS)objcopy
NM        := $(CROSS)nm
NASM      ?= nasm
XORRISO   ?= xorriso
QEMU      ?= qemu-system-x86_64
HOSTCXX   ?= g++
GDB       ?= gdb
PYTHON    ?= python3

KERNEL_ELF := $(BUILD)/kernel/lumen.elf
KERNEL_SYM := $(BUILD)/kernel/kernel.sym
ISO        := $(BUILD)/lumen.iso
LIMINE_BIN := $(BUILD)/limine-host/limine

# Version (docs/SPEC.md §23): the number lives in ./VERSION. `make RELEASE=1`
# builds a release (plain number); every other build is labelled as a
# development build with the commit it was built from.
VERSION_BASE := $(strip $(shell cat $(ROOT)/VERSION))
GIT_HASH     := $(shell git -c safe.directory='*' -C $(ROOT) rev-parse --short HEAD 2>/dev/null)
ifeq ($(RELEASE),1)
VERSION    := $(VERSION_BASE)
else
VERSION    := $(VERSION_BASE)-dev$(if $(GIT_HASH),+$(GIT_HASH))
endif
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

# ---------------------------------------------------------------------------
# Kernel flags (see docs/SPEC.md §1 "Non-negotiables")
# ---------------------------------------------------------------------------
# -ftrivial-auto-var-init=zero: no stack variable is ever uninitialised, so a
# forgotten initialiser cannot leak old stack contents (SPEC §5A phase 4).
# -fstack-protector-strong with a global guard: see kernel/lib/stack_protector.cpp.
KCXXFLAGS := -std=c++20 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
             -fno-stack-check \
             -ftrivial-auto-var-init=zero \
             -fno-omit-frame-pointer -fno-optimize-sibling-calls \
             -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel \
             -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
             -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
             -nostdlib -nostdinc++ -fno-builtin \
             -Wall -Wextra -Werror=return-type -Wno-unused-parameter \
             -O2 -g -MMD -MP \
             -I$(ROOT)/kernel -I$(ROOT)/tests -I$(LIMINE)
DEBUG ?= 1
ifeq ($(DEBUG),1)
KCXXFLAGS += -DLUMEN_DEBUG
# Undefined-behaviour sanitizer (SPEC §5A phase 4); handlers in lib/ubsan.cpp
# panic with the source location. Left out: vptr (needs RTTI), the float
# checks (no FPU in the kernel), and the per-dereference checks alignment,
# null and object-size, which tripled the size of kernel text; x86 permits
# unaligned access (firmware tables are unaligned by design) and a null
# dereference already faults cleanly because the low half is unmapped.
KSANFLAGS := -fsanitize=undefined \
             -fno-sanitize=vptr,float-cast-overflow,float-divide-by-zero,alignment,null,object-size
endif
# The sanitizer runtime must not be instrumented itself. The pixel loops in
# libgfx are left uninstrumented too, as a precaution: they are the hottest
# code in the system. (First-frame timings were too noisy, 21-29 ms either
# way, to measure the cost; revisit with `make bench` in phase 6.)
$(BUILD)/kernel/lib/ubsan.o: KSANFLAGS :=
$(BUILD)/kernel/gfx/%.o: KSANFLAGS :=
KASFLAGS  := -g -Wa,-I$(ROOT)
KNASMFLAGS:= -f elf64 -g -F dwarf
KLDFLAGS  := -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T $(ROOT)/kernel/linker.ld

SYMS_S      := $(ROOT)/kernel/lib/symbols.S
KERNEL_CPP  := $(shell find $(ROOT)/kernel $(ROOT)/tests/kernel -name '*.cpp' | sort)
KERNEL_S    := $(filter-out $(SYMS_S),$(shell find $(ROOT)/kernel -name '*.S' | sort))
KERNEL_ASM  := $(shell find $(ROOT)/kernel -name '*.asm' | sort)
KERNEL_OBJS := $(patsubst $(ROOT)/%.cpp,$(BUILD)/%.o,$(KERNEL_CPP)) \
               $(patsubst $(ROOT)/%.S,$(BUILD)/%.o,$(KERNEL_S)) \
               $(patsubst $(ROOT)/%.asm,$(BUILD)/%.o,$(KERNEL_ASM))
KERNEL_DEPS := $(KERNEL_OBJS:.o=.d)

# version.o is the only object that sees the version and build date. It is
# rebuilt when the version string changes (the stamp) or any other object
# does, so the banner always describes the image it is linked into.
VERSION_OBJ   := $(BUILD)/kernel/lib/version.o
VERSION_STAMP := $(BUILD)/version.stamp
$(VERSION_OBJ): KCXXFLAGS += -DLUMEN_VERSION=\"$(VERSION)\" -DLUMEN_BUILD_DATE=\"$(BUILD_DATE)\"
$(VERSION_OBJ): $(VERSION_STAMP) $(filter-out $(VERSION_OBJ),$(KERNEL_OBJS))

.PHONY: FORCE
$(VERSION_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

# ---------------------------------------------------------------------------
# QEMU invocation (docs/SPEC.md §3, exact)
# ---------------------------------------------------------------------------
# With KVM available (nested virtualisation inside WSL2, or a Linux host) the
# guest runs at native speed with accurate timers; otherwise the spec's TCG
# line is used unchanged. Override with QEMU_ACCEL=tcg to force emulation.
QEMU_ACCEL ?= $(if $(wildcard /dev/kvm),kvm,tcg)
ifeq ($(QEMU_ACCEL),kvm)
QEMU_CPU := -accel kvm -cpu host
else
QEMU_CPU := -cpu qemu64,+pdpe1gb
endif
QEMU_FLAGS := -machine q35 $(QEMU_CPU) -smp 4 -m 512M \
              -cdrom $(ISO) -boot d \
              -serial stdio \
              -d guest_errors -no-reboot -no-shutdown
QEMU_DISPLAY ?= -display gtk,zoom-to-fit=on

# ---------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------
.PHONY: all kernel iso run run-headless run-uefi debug gdb test bench fuzz clean check-tools vbox vbox-log dist help

all: check-tools kernel $(INITRD) $(SYSCALL_MD)

kernel: $(KERNEL_ELF) $(KERNEL_SYM)

iso: $(ISO)

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISPLAY)

run-headless: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -display none

# UEFI boot through OVMF (the spec's primary boot path); the default `run` uses SeaBIOS.
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
run-uefi: $(ISO)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISPLAY) -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)

debug: $(ISO)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISPLAY) -s -S

gdb: $(KERNEL_ELF)
	$(GDB) -q $(KERNEL_ELF) \
	    -ex "set confirm off" \
	    -ex "directory $(ROOT)/kernel" \
	    -ex "target remote localhost:1234"

# Integration tests: each tests/integration/<name>.expect boots the ISO headless
# and checks the serial log (see tools/qemu-probe.py). Exit nonzero on any failure.
INTEGRATION_TESTS := $(wildcard $(ROOT)/tests/integration/*.expect)

test: $(ISO) tools
	@fail=0; \
	for t in $(INTEGRATION_TESTS); do \
	    name=$$(basename $$t .expect); \
	    if $(PYTHON) $(ROOT)/tools/qemu-probe.py $(ISO) --wait 6 --quiet --expect $$t > $(BUILD)/test-$$name.log 2>&1; then \
	        echo "PASS integration/$$name"; \
	    else \
	        echo "FAIL integration/$$name (see build/test-$$name.log)"; fail=1; \
	    fi; \
	done; \
	exit $$fail

# Micro-benchmarks for the budgets in docs/SPEC.md §20.1: boots headless, runs
# the kernel's `bench` command and prints one line per metric. Record the
# numbers in docs/BENCH.md at the end of each phase.
bench: $(ISO)
	@$(PYTHON) $(ROOT)/tools/qemu-probe.py $(ISO) --wait 6 --expect $(ROOT)/tests/bench/bench.expect > $(BUILD)/bench.log 2>&1; rc=$$?; \
	 grep -a '^bench: ' $(BUILD)/bench.log | grep -v 'bench: done'; \
	 [ $$rc -eq 0 ] || echo "bench: FAILED (see build/bench.log)"; exit $$rc

clean:
	rm -rf $(BUILD)

help:
	@echo "targets: all kernel iso run run-headless run-uefi debug gdb test bench fuzz clean vbox vbox-log dist check-tools"

# ---------------------------------------------------------------------------
# Kernel build rules
# ---------------------------------------------------------------------------
# Objects also depend on the Makefile so a flag change rebuilds everything.
$(BUILD)/%.o: $(ROOT)/%.cpp $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(KCXXFLAGS) $(KSANFLAGS) -c $< -o $@

$(BUILD)/%.o: $(ROOT)/%.S $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(KCXXFLAGS) $(KASFLAGS) -c $< -o $@

$(BUILD)/%.o: $(ROOT)/%.asm $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(NASM) $(KNASMFLAGS) $< -o $@

# Two-pass link. Pass 1 links with an empty symbol table; its `nm` output is
# turned into the .ksymtab blob (tools/gensyms.py) and pass 2 links that in.
# The .ksymtab section is last in the image, so no other address can move; a
# check after pass 2 enforces that.
SYMS_EMPTY   := $(BUILD)/syms-empty.bin
SYMS_FULL    := $(BUILD)/syms.bin
KERNEL_PASS1 := $(BUILD)/kernel/lumen-pass1.elf

$(SYMS_EMPTY): $(ROOT)/tools/gensyms.py
	@mkdir -p $(dir $@)
	$(PYTHON) $< $@

$(BUILD)/symbols-empty.o: $(SYMS_S) $(SYMS_EMPTY)
	$(CXX) $(KCXXFLAGS) $(KASFLAGS) -DSYMS_FILE=\"$(SYMS_EMPTY)\" -c $< -o $@

$(KERNEL_PASS1): $(KERNEL_OBJS) $(BUILD)/symbols-empty.o $(ROOT)/kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(KLDFLAGS) $(KERNEL_OBJS) $(BUILD)/symbols-empty.o -o $@

$(SYMS_FULL): $(KERNEL_PASS1) $(ROOT)/tools/gensyms.py
	$(NM) -nC $< > $(BUILD)/pass1.sym
	$(PYTHON) $(ROOT)/tools/gensyms.py $@ $(BUILD)/pass1.sym

$(BUILD)/symbols-full.o: $(SYMS_S) $(SYMS_FULL)
	$(CXX) $(KCXXFLAGS) $(KASFLAGS) -DSYMS_FILE=\"$(SYMS_FULL)\" -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(BUILD)/symbols-full.o $(ROOT)/kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(KLDFLAGS) $(KERNEL_OBJS) $(BUILD)/symbols-full.o -o $@
	@$(NM) -n $(KERNEL_PASS1) | grep -vE 'ksymtab|__kernel_end' > $(BUILD)/pass1.chk; \
	 $(NM) -n $@ | grep -vE 'ksymtab|__kernel_end' > $(BUILD)/pass2.chk; \
	 cmp -s $(BUILD)/pass1.chk $(BUILD)/pass2.chk || \
	     { echo "error: symbol addresses moved between link passes"; exit 1; }

# Plain-text symbol list for GDB users and the debugging playbook.
$(KERNEL_SYM): $(KERNEL_ELF)
	$(NM) -n --defined-only $< | grep -E ' [tTwW] ' > $@ || true

-include $(KERNEL_DEPS)

# ---------------------------------------------------------------------------
# System-call table: one definition file generates the reference document and
# the user library's call numbers (docs/SPEC.md §7).
# ---------------------------------------------------------------------------
GEN         := $(BUILD)/gen
SYSCALL_DEF := $(ROOT)/kernel/syscall/table.def
SYSCALL_HDR := $(GEN)/lumen/syscall_nr.h
SYSCALL_MD  := $(ROOT)/docs/SYSCALLS.md

$(SYSCALL_HDR) $(SYSCALL_MD) &: $(SYSCALL_DEF) $(ROOT)/tools/gen-syscalls.py
	@mkdir -p $(dir $(SYSCALL_HDR))
	$(PYTHON) $(ROOT)/tools/gen-syscalls.py $(SYSCALL_DEF) --md $(SYSCALL_MD) --header $(SYSCALL_HDR)

# ---------------------------------------------------------------------------
# Userland: the C library and the programs in userland/bin, each linked as a
# static position-independent executable, packed into the boot archive that
# the bootloader hands to the kernel as a module.
# ---------------------------------------------------------------------------
USER_DIR  := $(ROOT)/userland
UBUILD    := $(BUILD)/user
# SSE is on here (the kernel saves it per thread). The stack protector uses a
# global guard that the start-up code seeds from the kernel's AT_RANDOM bytes.
UCXXFLAGS := -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
             -fno-use-cxa-atexit -fno-builtin -nostdinc++ \
             -fPIE -fvisibility=hidden -fstack-protector-strong -mstack-protector-guard=global \
             -fstack-clash-protection \
             -Wall -Wextra -Werror=return-type -O2 -g -MMD -MP \
             -I$(USER_DIR)/libc/include -I$(GEN)
# -z text: refuse to link if code would need to be patched at load time.
ULDFLAGS  := -nostdlib -pie --no-dynamic-linker -z text -z noexecstack -z max-page-size=0x1000 -S \
             -T $(USER_DIR)/libc/user.ld
LIBGCC    := $(shell $(CXX) -print-libgcc-file-name 2>/dev/null)
# User programs are linked with the host linker: the bare-metal cross linker
# (x86_64-elf) has no support for position-independent executables and
# silently produces a fixed-address one, which the kernel refuses to load.
# The object files are ordinary x86-64 ELF, so either linker accepts them.
USER_LD   ?= ld

LIBC_SRCS := $(sort $(wildcard $(USER_DIR)/libc/src/*.cpp))
LIBC_OBJS := $(patsubst $(USER_DIR)/%.cpp,$(UBUILD)/%.o,$(LIBC_SRCS))
CRT0      := $(UBUILD)/libc/src/crt0.o
USER_PROGS := $(sort $(basename $(notdir $(wildcard $(USER_DIR)/bin/*.cpp))))
USER_BINS  := $(addprefix $(UBUILD)/bin/,$(USER_PROGS))
USER_DEPS  := $(LIBC_OBJS:.o=.d) $(addsuffix .d,$(USER_BINS))
INITRD     := $(BUILD)/initrd.tar

$(UBUILD)/%.o: $(USER_DIR)/%.cpp $(SYSCALL_HDR) $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(UCXXFLAGS) -c $< -o $@

$(CRT0): $(USER_DIR)/libc/src/crt0.S $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@

.PRECIOUS: $(UBUILD)/bin/%.o
$(UBUILD)/bin/%: $(UBUILD)/bin/%.o $(CRT0) $(LIBC_OBJS) $(USER_DIR)/libc/user.ld $(ROOT)/Makefile
	$(USER_LD) $(ULDFLAGS) $(CRT0) $< $(LIBC_OBJS) $(LIBGCC) -o $@

# A fixed owner, date and order make the archive identical for identical input.
# One program (userland/bin/fileutils.cpp) answers to all these names.
FILEUTILS_NAMES := rmdir mv cp touch stat ln sync mount umount pwd write

$(INITRD): $(USER_BINS) $(wildcard $(USER_DIR)/etc/*)
	rm -rf $(BUILD)/initrd_root
	mkdir -p $(BUILD)/initrd_root/bin $(BUILD)/initrd_root/etc
	cp $(USER_BINS) $(BUILD)/initrd_root/bin/
	for t in $(FILEUTILS_NAMES); do cp $(UBUILD)/bin/fileutils $(BUILD)/initrd_root/bin/$$t; done
	rm -f $(BUILD)/initrd_root/bin/fileutils
	cp $(USER_DIR)/etc/* $(BUILD)/initrd_root/etc/
	# Modes are set here, not with chmod: the build tree may live on a Windows
	# drive where every file reads back as 0777.
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' \
	    --mode=0755 -C $(BUILD)/initrd_root -cf $@ bin
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' \
	    --mode=0755 --no-recursion -C $(BUILD)/initrd_root -rf $@ etc
	cd $(BUILD)/initrd_root && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
	    --mtime='2026-01-01 00:00:00' --mode=0644 -rf $@ $$(ls etc/* | sort)

-include $(USER_DEPS)

# ---------------------------------------------------------------------------
# Host tools: mkfs.lumfs formats and checks disk images on the build machine.
# It shares the on-disk format code with the kernel.
# ---------------------------------------------------------------------------
MKFS_LUMFS := $(BUILD)/tools/mkfs.lumfs
$(MKFS_LUMFS): $(ROOT)/tools/mkfs-lumfs.cpp $(ROOT)/kernel/lib/crc32c.cpp $(ROOT)/kernel/fs/lumfs_format.h \
               $(ROOT)/kernel/fs/lumfs_mkfs.h
	@mkdir -p $(dir $@)
	$(HOSTCXX) -std=c++20 -O2 -g -Wall -Wextra -I$(ROOT)/kernel -o $@ $(ROOT)/tools/mkfs-lumfs.cpp \
	    $(ROOT)/kernel/lib/crc32c.cpp

tools: $(MKFS_LUMFS)
.PHONY: tools

# ---------------------------------------------------------------------------
# Fuzzing (docs/SPEC.md §19.11). The parsers of untrusted input are built for
# the host with AddressSanitizer and fed mutated files for FUZZ_SECONDS each
# (tests/fuzz/driver.cpp); files under tests/fuzz/corpus/<name>/ are replayed
# first as regression tests. System-call arguments are fuzzed inside the
# running system by /bin/sysfuzz.
# ---------------------------------------------------------------------------
FUZZ_SECONDS ?= 60
HOST_CXX     ?= g++
FUZZ_DIR     := $(BUILD)/fuzz
FUZZ_FLAGS   := -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
                -fno-sanitize-recover=all -Wall -Wextra -I$(ROOT)/kernel

$(FUZZ_DIR)/fuzz-elf: $(ROOT)/tests/fuzz/driver.cpp $(ROOT)/tests/fuzz/fuzz_elf.cpp \
                      $(ROOT)/kernel/proc/elf.cpp $(ROOT)/kernel/proc/elf.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(FUZZ_FLAGS) $(filter %.cpp,$^) -o $@

$(FUZZ_DIR)/fuzz-ustar: $(ROOT)/tests/fuzz/driver.cpp $(ROOT)/tests/fuzz/fuzz_ustar.cpp \
                        $(ROOT)/kernel/fs/ustar.cpp $(ROOT)/kernel/fs/ustar.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(FUZZ_FLAGS) $(filter %.cpp,$^) -o $@

fuzz: $(FUZZ_DIR)/fuzz-elf $(FUZZ_DIR)/fuzz-ustar $(ISO)
	@cd $(FUZZ_DIR) && ./fuzz-elf $(FUZZ_SECONDS) $(USER_BINS) $(wildcard $(ROOT)/tests/fuzz/corpus/elf/*)
	@cd $(FUZZ_DIR) && ./fuzz-ustar $(FUZZ_SECONDS) $(INITRD) $(wildcard $(ROOT)/tests/fuzz/corpus/ustar/*)
	@if $(PYTHON) $(ROOT)/tools/qemu-probe.py $(ISO) --wait 6 --expect $(ROOT)/tests/fuzz/sysfuzz.expect > $(BUILD)/fuzz-syscall.log 2>&1; then \
	    grep -a 'sysfuzz: PASS' $(BUILD)/fuzz-syscall.log | head -1; \
	else \
	    echo "fuzz-syscall: FAILED (see build/fuzz-syscall.log)"; exit 1; \
	fi

# ---------------------------------------------------------------------------
# ISO: Limine (UEFI + BIOS hybrid) + kernel + initramfs
# ---------------------------------------------------------------------------
$(LIMINE_BIN): $(LIMINE)/limine.c
	@mkdir -p $(dir $@)
	cc -g -O2 -pipe -std=c99 $< -o $@

$(ISO): $(KERNEL_ELF) $(INITRD) $(LIMINE_BIN) $(ROOT)/limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL_ELF) $(BUILD)/iso_root/boot/lumen.elf
	cp $(INITRD) $(BUILD)/iso_root/boot/initrd.tar
	cp $(ROOT)/limine.conf $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label \
	    $(BUILD)/iso_root -o $@ 2>&1 | grep -vE '^xorriso : (NOTE|UPDATE)' || true
	$(LIMINE_BIN) bios-install $@
	@echo "ISO ready: $@"

# ---------------------------------------------------------------------------
# VirtualBox: create (once) and boot a "Lumen" VM from the ISO.
# Works from WSL by calling the Windows VBoxManage.exe.
# ---------------------------------------------------------------------------
VBOXMANAGE ?= /mnt/c/Program\ Files/Oracle/VirtualBox/VBoxManage.exe
VBOX_VM    ?= Lumen
LOGS       := $(ROOT)/logs
VBOX_LOG   := $(LOGS)/vbox-serial.log

# The VM's COM1 is written to logs/vbox-serial.log on every run (also when the
# VM is started from the VirtualBox GUI). `make vbox` archives the previous
# run's log with a timestamp first, so nothing is lost.
vbox: $(ISO)
	@mkdir -p $(LOGS); \
	ISO_WIN=$$(wslpath -w $(ISO)); LOG_WIN=$$(wslpath -w $(LOGS))\\vbox-serial.log; \
	if ! $(VBOXMANAGE) showvminfo "$(VBOX_VM)" >/dev/null 2>&1; then \
	    echo "creating VirtualBox VM '$(VBOX_VM)'"; \
	    $(VBOXMANAGE) createvm --name "$(VBOX_VM)" --ostype Other_64 --register; \
	    $(VBOXMANAGE) modifyvm "$(VBOX_VM)" --memory 2048 --cpus 4 --firmware bios --x86-long-mode on \
	        --graphicscontroller vmsvga --vram 64 --uart1 0x3F8 4 --hpet on \
	        --boot1 dvd --boot2 none --boot3 none --boot4 none --mouse ps2 --keyboard ps2; \
	    $(VBOXMANAGE) storagectl "$(VBOX_VM)" --name IDE --add ide; \
	    $(VBOXMANAGE) storageattach "$(VBOX_VM)" --storagectl IDE --port 0 --device 0 --type dvddrive --medium emptydrive; \
	fi; \
	if $(VBOXMANAGE) showvminfo "$(VBOX_VM)" --machinereadable | grep -q '^VMState="running"'; then \
	    echo "VM '$(VBOX_VM)' is already running; quit it first"; exit 1; \
	fi; \
	if [ -s $(VBOX_LOG) ]; then mv $(VBOX_LOG) $(LOGS)/vbox-serial-$$(date +%Y%m%d-%H%M%S).log; fi; \
	$(VBOXMANAGE) modifyvm "$(VBOX_VM)" --uartmode1 file "$$LOG_WIN"; \
	$(VBOXMANAGE) storageattach "$(VBOX_VM)" --storagectl IDE --port 0 --device 0 --type dvddrive --medium "$$ISO_WIN"; \
	$(VBOXMANAGE) startvm "$(VBOX_VM)"

# Show the serial log of the most recent VirtualBox run, ANSI codes stripped.
vbox-log:
	@$(PYTHON) -c "import re,sys; t=open('$(VBOX_LOG)',errors='replace').read(); print(re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',t))"

# The ISO to try in a VM (dist/ is not cleaned by `make clean`). dist/ holds
# exactly one file with a fixed name, dist/lumen.iso, and each release
# overwrites it, so there is only ever one to choose (owner's rule,
# 2026-10-03). The version is shown in the boot banner and the About window,
# and written to dist/VERSION.txt. Every powered-off VirtualBox VM that boots
# an ISO from dist/ is pointed at the file and, if it was created 32-bit,
# switched to 64-bit (tools/vbox-attach.sh).
dist: $(ISO)
	@mkdir -p $(ROOT)/dist
	@rm -f $(ROOT)/dist/lumen*.iso
	cp $(ISO) $(ROOT)/dist/lumen.iso
	@echo "Lumen $(VERSION), built $(BUILD_DATE)" > $(ROOT)/dist/VERSION.txt
	@echo "snapshot: $(ROOT)/dist/lumen.iso (Lumen $(VERSION))"
	@bash $(ROOT)/tools/vbox-attach.sh $(VBOXMANAGE) $(ROOT)/dist/lumen.iso

# ---------------------------------------------------------------------------
# Host tool check with install hints (docs/SPEC.md §3)
# ---------------------------------------------------------------------------
define CHECK_TOOL
	@command -v $(1) >/dev/null 2>&1 || { echo "missing tool: $(1)  ->  $(2)"; exit 1; }
endef

check-tools:
	@test -x $(CXX) || { echo "missing cross compiler: $(CXX)"; \
	    echo "  ->  run: ./toolchain/build-cross.sh"; exit 1; }
	$(call CHECK_TOOL,$(NASM),sudo apt install nasm)
	$(call CHECK_TOOL,$(XORRISO),sudo apt install xorriso)
	$(call CHECK_TOOL,$(QEMU),sudo apt install qemu-system-x86)
	$(call CHECK_TOOL,$(GDB),sudo apt install gdb)
	$(call CHECK_TOOL,$(PYTHON),sudo apt install python3)
	$(call CHECK_TOOL,cc,sudo apt install build-essential)
