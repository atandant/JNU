# Makefile — top-level build orchestration for JNU.
#
# Targets:
#   make            kernel ELF + bootable ISO
#   make iso        bootable ISO only
#   make font       (re)generate kernel/drivers/font_data.h
#   make run        boot in QEMU (uses Windows desktop QEMU through $(QEMU))
#   make debug      boot QEMU with -s -S, ready for gdb attach
#   make clean      delete build/
#   make distclean  delete build/ and force re-clone of Limine
#
# Conventions:
#   - clang as compiler, lld as linker, NASM for asm.
#   - Built freestanding, kernel mcmodel.
#   - All flag knobs live in this file; subsystem .c files only need
#     `#include <jnu/...>`.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

# ---- toolchain --------------------------------------------------------------

# Make pre-defines CC and LD ("cc" / "ld"), so a plain ?= leaves them as
# "cc"/"ld". We force-bind them to clang / ld.lld unless the user passed
# CC=... / LD=... on the command line or via the environment.
ifeq ($(origin CC),default)
CC      := clang
endif
ifeq ($(origin LD),default)
LD      := ld.lld
endif
NASM    ?= nasm
PYTHON  ?= python3

# Path to a Windows-desktop qemu-system-x86_64 reachable from WSL. Override
# on the command line, e.g.:
#   make run QEMU="/mnt/c/Program Files/qemu/qemu-system-x86_64.exe"
QEMU    ?= qemu-system-x86_64.exe

# ---- paths ------------------------------------------------------------------

BUILD       := build
OBJDIR      := $(BUILD)/obj
KERNEL_ELF  := $(BUILD)/kernel.elf
KERNEL_ISO  := $(BUILD)/kernel.iso
ISO_ROOT    := $(BUILD)/iso_root
ATA_DISK    := $(BUILD)/disk.img

LIMINE_DIR  := boot/limine
FONT_HDR    := kernel/drivers/font_data.h

# ---- flags ------------------------------------------------------------------

CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu17 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic -fno-pie \
    -mno-red-zone \
    -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
    -mcmodel=kernel \
    -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic \
    -Wshadow -Wconversion -Wsign-conversion \
    -Wmissing-prototypes -Wstrict-prototypes \
    -Wnull-dereference \
    -Werror \
    -O2 -g3 \
    -nostdinc \
    -Ikernel/include \
    -I$(LIMINE_DIR)

NASMFLAGS := -f elf64 -F dwarf -g

LDFLAGS := \
    -nostdlib --no-pie -static \
    -T kernel/arch/x86_64/linker.ld \
    -z max-page-size=0x1000

# ---- sources ----------------------------------------------------------------

C_SRCS := \
    kernel/lib/string.c \
    kernel/lib/printk.c \
    kernel/lib/rbtree.c \
    kernel/lib/spinlock.c \
    kernel/drivers/serial.c \
    kernel/drivers/fbcon.c \
    kernel/drivers/pit.c \
    kernel/drivers/rtc.c \
    kernel/drivers/kbd.c \
    kernel/drivers/pci.c \
    kernel/drivers/ata.c \
    kernel/arch/x86_64/cpu.c \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/exceptions.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/apic.c \
    kernel/arch/x86_64/paging.c \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/vma.c \
    kernel/mm/slab.c \
    kernel/fs/block.c \
    kernel/fs/vfs.c \
    kernel/fs/minix.c \
    kernel/kernel/cmdline.c \
    kernel/kernel/panic.c \
    kernel/kernel/sched.c \
    kernel/kernel/selftest.c \
    kernel/kernel/symbols.c \
    kernel/kernel/main.c

S_SRCS := \
    kernel/arch/x86_64/boot.S \
    kernel/arch/x86_64/isr.S

C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)
S_OBJS := $(S_SRCS:%.S=$(OBJDIR)/%.o)
BUILDINFO_OBJ := $(OBJDIR)/buildinfo.o

OBJS := $(S_OBJS) $(C_OBJS) $(BUILDINFO_OBJ)

# ---- top-level --------------------------------------------------------------

.PHONY: all iso font ata-disk clean clean-disk distclean run run-disk debug debug-disk check-limine

all: iso

iso: $(KERNEL_ISO)

font: $(FONT_HDR)

#
# ata-disk: create (or recreate) build/disk.img.
# Pass SIZE=N to override the default 32 MiB, e.g.: make ata-disk SIZE=64
#
ata-disk:
	@bash scripts/make-ata-disk.sh "$(ATA_DISK)" "$(or $(SIZE),32)"

# ---- build rules ------------------------------------------------------------

$(FONT_HDR): scripts/gen-font.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/gen-font.py $@

$(OBJDIR)/%.o: %.c $(FONT_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILDINFO_OBJ): scripts/gen-buildinfo.sh FORCE
	@mkdir -p $(dir $@)
	@VERSION="0.0.1" \
	  SHA="$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
	  BUILDTIME="$$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
	  bash scripts/gen-buildinfo.sh $(OBJDIR)/buildinfo.c
	$(CC) $(CFLAGS) -c $(OBJDIR)/buildinfo.c -o $@

FORCE:

$(KERNEL_ELF): $(OBJS) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ---- ISO --------------------------------------------------------------------

check-limine:
	@if [ ! -f "$(LIMINE_DIR)/limine.h" ]; then \
	  echo "ERROR: $(LIMINE_DIR)/limine.h missing."; \
	  echo "Clone limine first:"; \
	  echo "  git clone https://github.com/limine-bootloader/limine.git \\"; \
	  echo "      --branch=v8.x-binary --depth=1 $(LIMINE_DIR)"; \
	  echo "  make -C $(LIMINE_DIR)"; \
	  exit 1; \
	fi

$(KERNEL_ISO): $(KERNEL_ELF) boot/limine.cfg | check-limine
	@bash scripts/make-image.sh "$(KERNEL_ELF)" "boot/limine.cfg" \
	    "$(LIMINE_DIR)" "$(ISO_ROOT)" "$@"

# ---- run / debug ------------------------------------------------------------

# QEMU flags shared between run targets.
QEMU_COMMON := -machine q35 -m 256M \
    -cdrom $(KERNEL_ISO) \
    -boot d \
    -serial stdio \
    -no-reboot -no-shutdown

# Attach an ATA disk only when build/disk.img exists.
# q35 has no legacy IDE controller, so we add piix3-ide explicitly and
# wire the drive image through it. This gives the ATA PIO driver real
# devices at ports 0x1F0/0x170.
comma := ,
QEMU_IDE := -device piix3-ide$(comma)id=ide \
    -drive id=hd0$(comma)file=$(ATA_DISK)$(comma)format=raw$(comma)if=none \
    -device ide-hd$(comma)drive=hd0$(comma)bus=ide.0

ifdef NODISK
QEMU_DISK :=
else
QEMU_DISK := $(if $(wildcard $(ATA_DISK)),$(QEMU_IDE),)
endif

run: $(KERNEL_ISO)
	"$(QEMU)" $(QEMU_COMMON) $(QEMU_DISK)

# run-disk: always require the disk image (fail if missing).
run-disk: $(KERNEL_ISO) $(ATA_DISK)
	"$(QEMU)" $(QEMU_COMMON) $(QEMU_IDE)

debug: $(KERNEL_ISO)
	"$(QEMU)" $(QEMU_COMMON) $(QEMU_DISK) -s -S

debug-disk: $(KERNEL_ISO) $(ATA_DISK)
	"$(QEMU)" $(QEMU_COMMON) $(QEMU_IDE) -s -S

# ---- housekeeping -----------------------------------------------------------

# clean: remove build artifacts but keep disk.img (it takes time to generate).
clean:
	rm -rf $(BUILD)/obj $(BUILD)/kernel.elf $(BUILD)/kernel.iso \
	       $(BUILD)/iso_root $(FONT_HDR)

# clean-disk: also wipe the ATA disk image.
clean-disk: clean
	rm -f $(ATA_DISK)

distclean: clean-disk
	rm -rf $(LIMINE_DIR)
