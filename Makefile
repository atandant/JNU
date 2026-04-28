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
    kernel/drivers/serial.c \
    kernel/drivers/fbcon.c \
    kernel/kernel/cmdline.c \
    kernel/kernel/panic.c \
    kernel/kernel/main.c

S_SRCS := \
    kernel/arch/x86_64/boot.S

C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)
S_OBJS := $(S_SRCS:%.S=$(OBJDIR)/%.o)
BUILDINFO_OBJ := $(OBJDIR)/buildinfo.o

OBJS := $(S_OBJS) $(C_OBJS) $(BUILDINFO_OBJ)

# ---- top-level --------------------------------------------------------------

.PHONY: all iso font clean distclean run debug check-limine

all: iso

iso: $(KERNEL_ISO)

font: $(FONT_HDR)

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

run: $(KERNEL_ISO)
	"$(QEMU)" -machine q35 -m 256M \
	    -cdrom $(KERNEL_ISO) \
	    -boot d \
	    -serial stdio \
	    -no-reboot -no-shutdown

debug: $(KERNEL_ISO)
	"$(QEMU)" -machine q35 -m 256M \
	    -cdrom $(KERNEL_ISO) \
	    -boot d \
	    -serial stdio \
	    -no-reboot -no-shutdown \
	    -s -S

# ---- housekeeping -----------------------------------------------------------

clean:
	rm -rf $(BUILD) $(FONT_HDR)

distclean: clean
	rm -rf $(LIMINE_DIR)
