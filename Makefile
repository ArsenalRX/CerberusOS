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
GDB       ?= gdb
PYTHON    ?= python3

KERNEL_ELF := $(BUILD)/kernel/lumen.elf
KERNEL_SYM := $(BUILD)/kernel/kernel.sym
ISO        := $(BUILD)/lumen.iso
LIMINE_BIN := $(BUILD)/limine-host/limine

VERSION    := 0.0.1
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

# ---------------------------------------------------------------------------
# Kernel flags (see docs/SPEC.md §1 "Non-negotiables")
# ---------------------------------------------------------------------------
KCXXFLAGS := -std=c++20 -ffreestanding -fno-stack-protector -fno-stack-check \
             -fno-omit-frame-pointer -fno-optimize-sibling-calls \
             -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel \
             -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
             -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
             -nostdlib -nostdinc++ -fno-builtin \
             -Wall -Wextra -Werror=return-type -Wno-unused-parameter \
             -O2 -g -MMD -MP \
             -I$(ROOT)/kernel -I$(ROOT)/tests -I$(LIMINE) \
             -DLUMEN_VERSION=\"$(VERSION)\" -DLUMEN_BUILD_DATE=\"$(BUILD_DATE)\"
DEBUG ?= 1
ifeq ($(DEBUG),1)
KCXXFLAGS += -DLUMEN_DEBUG
endif
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
.PHONY: all kernel iso run run-headless run-uefi debug gdb test clean check-tools vbox vbox-log dist help

all: check-tools kernel

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

test: $(ISO)
	@fail=0; \
	for t in $(INTEGRATION_TESTS); do \
	    name=$$(basename $$t .expect); \
	    if $(PYTHON) $(ROOT)/tools/qemu-probe.py $(ISO) --wait 8 --quiet --expect $$t > $(BUILD)/test-$$name.log 2>&1; then \
	        echo "PASS integration/$$name"; \
	    else \
	        echo "FAIL integration/$$name (see build/test-$$name.log)"; fail=1; \
	    fi; \
	done; \
	exit $$fail

clean:
	rm -rf $(BUILD)

help:
	@echo "targets: all kernel iso run run-headless run-uefi debug gdb test clean vbox vbox-log dist check-tools"

# ---------------------------------------------------------------------------
# Kernel build rules
# ---------------------------------------------------------------------------
# Objects also depend on the Makefile so a flag change rebuilds everything.
$(BUILD)/%.o: $(ROOT)/%.cpp $(ROOT)/Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(KCXXFLAGS) -c $< -o $@

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
# ISO: Limine (UEFI + BIOS hybrid) + kernel + initramfs
# ---------------------------------------------------------------------------
$(LIMINE_BIN): $(LIMINE)/limine.c
	@mkdir -p $(dir $@)
	cc -g -O2 -pipe -std=c99 $< -o $@

$(ISO): $(KERNEL_ELF) $(LIMINE_BIN) $(ROOT)/limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL_ELF) $(BUILD)/iso_root/boot/lumen.elf
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
VBOX_VM    ?= Lumen 0.0.1
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
	    $(VBOXMANAGE) modifyvm "$(VBOX_VM)" --memory 1024 --cpus 4 --firmware efi \
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

# Snapshot ISO for trying out in a VM (dist/ is not cleaned by `make clean`).
dist: $(ISO)
	@mkdir -p $(ROOT)/dist
	cp $(ISO) $(ROOT)/dist/lumen-$(VERSION).iso
	@echo "snapshot: $(ROOT)/dist/lumen-$(VERSION).iso"

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
