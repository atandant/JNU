# Makefile — top-level build orchestration for JNU.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

# ---- toolchain --------------------------------------------------------------

ifeq ($(origin CC),default)
CC      := clang
endif
ifeq ($(origin LD),default)
LD      := ld.lld
endif
NASM        ?= nasm
PYTHON      ?= python3
NM          ?= nm
QEMU        ?= qemu-system-x86_64
SPHINXBUILD ?= sphinx-build

# ---- project knobs ----------------------------------------------------------

VERSION ?= 0.0.3.1
SIZE    ?= 32
MEMORY  ?= 850M
CPU     ?= qemu64,+smep,+smap

# ---- paths ------------------------------------------------------------------

BUILD          := build
OBJDIR         := $(BUILD)/obj
GENERATED      := $(BUILD)/generated
GENERATED_INC  := $(GENERATED)/include
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_ISO     := $(BUILD)/kernel.iso
KERNEL_ISO_MUSL := $(BUILD)/kernel-musl.iso
INITRAMFS      := $(BUILD)/initramfs.cpio
INITRAMFS_MUSL := $(BUILD)/initramfs-musl.cpio
USER_ROOT      := $(BUILD)/user
USER_INIT      := $(USER_ROOT)/init
MUSL_TEST      := $(USER_ROOT)/bin/musltest
ISO_ROOT       := $(BUILD)/iso_root
ISO_ROOT_MUSL  := $(BUILD)/iso_root_musl
ATA_DISK       := $(BUILD)/disk.img

LIMINE_DIR     := boot/limine
FONT_HDR       := $(GENERATED_INC)/jnu/generated/font_data.h

USER_PROGRAM_SRCS := $(shell find user -mindepth 2 -maxdepth 2 -name main.c \
    ! -path 'user/libjnu/*' ! -path 'user/musl/*' ! -path 'user/musltest/*' 2>/dev/null | sort)
USER_LIBJNU_C_SRCS := $(shell find user/libjnu -maxdepth 1 -name '*.c' 2>/dev/null | sort)
USER_LIBJNU_ASM_SRCS := user/libjnu/crt0.S user/libjnu/syscall.S
USER_BUILD_DEPS := scripts/build-user.sh $(USER_PROGRAM_SRCS) $(USER_LIBJNU_C_SRCS) \
    $(USER_LIBJNU_ASM_SRCS) user/libjnu/include/jnu_syscall.h

# ---- flags ------------------------------------------------------------------

COMMON_WARNINGS := \
    -Wall -Wextra -Wpedantic \
    -Wshadow -Wconversion -Wsign-conversion \
    -Wmissing-prototypes -Wstrict-prototypes \
    -Wnull-dereference \
    -Werror

KERNEL_CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu17 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic -fno-pie \
    -mno-red-zone \
    -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
    -mcmodel=kernel \
    -fno-omit-frame-pointer \
    $(COMMON_WARNINGS) \
    -O2 -g3 \
    -nostdinc \
    -I$(GENERATED_INC) \
    -Ikernel/include \
    -I$(LIMINE_DIR)

NASMFLAGS := -f elf64 -F dwarf -g

KERNEL_LDFLAGS := \
    -nostdlib --no-pie -static \
    -T kernel/arch/x86_64/linker.ld \
    -z max-page-size=0x1000

# ---- sources ----------------------------------------------------------------

KERNEL_LIB_SRCS := \
    kernel/lib/string.c \
    kernel/lib/printk.c \
    kernel/lib/rbtree.c \
    kernel/lib/spinlock.c \
    kernel/lib/mutex.c \
    kernel/lib/prng.c

KERNEL_DRIVER_SRCS := \
    kernel/drivers/serial.c \
    kernel/drivers/fbcon.c \
    kernel/drivers/pit.c \
    kernel/drivers/rtc.c \
    kernel/drivers/kbd.c \
    kernel/drivers/scandata.c \
    kernel/drivers/pci.c \
    kernel/drivers/ata.c \
    kernel/drivers/acpi.c \
    kernel/drivers/hpet.c

KERNEL_ARCH_X86_64_C_SRCS := \
    kernel/arch/x86_64/cpu.c \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/exceptions.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/apic.c \
    kernel/arch/x86_64/lapic_timer.c \
    kernel/arch/x86_64/arch_syscall.c \
    kernel/arch/x86_64/usermode.c \
    kernel/arch/x86_64/paging.c \
    kernel/arch/x86_64/fpu.c \
    kernel/arch/x86_64/entropy.c

KERNEL_MM_SRCS := \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/vma.c \
    kernel/mm/slab.c \
    kernel/mm/clone_space.c \
    kernel/mm/mmap.c

KERNEL_INITRAMFS_SRCS := \
    kernel/initramfs/cpio_newc.c \
    kernel/initramfs/initramfs.c

KERNEL_EXEC_SRCS := \
    kernel/exec/elf64.c

KERNEL_SYSCALL_SRCS := \
    kernel/syscall/common.c \
    kernel/syscall/dispatch.c \
    kernel/syscall/sys_close.c \
    kernel/syscall/sys_exit.c \
    kernel/syscall/sys_fstat.c \
    kernel/syscall/sys_getpid.c \
    kernel/syscall/sys_fs_write.c \
    kernel/syscall/sys_lseek.c \
    kernel/syscall/sys_open.c \
    kernel/syscall/sys_read.c \
    kernel/syscall/sys_waitpid.c \
    kernel/syscall/sys_write.c \
    kernel/syscall/sys_yield.c \
    kernel/syscall/sys_fork.c \
    kernel/syscall/sys_execve.c \
    kernel/syscall/sys_writev.c \
    kernel/syscall/sys_arch_prctl.c \
    kernel/syscall/sys_clock_gettime.c \
    kernel/syscall/sys_nanosleep.c \
    kernel/syscall/sys_getrandom.c \
    kernel/syscall/sys_set_tid_address.c \
    kernel/syscall/sys_rt_sigaction.c \
    kernel/syscall/sys_rt_sigprocmask.c \
    kernel/syscall/sys_ioctl.c \
    kernel/syscall/sys_exit_group.c

KERNEL_USER_SRCS := \
    kernel/user/copy.c \
    kernel/user/fd.c \
    kernel/user/process.c

KERNEL_FS_SRCS := \
    kernel/fs/block.c \
    kernel/fs/vfs.c \
    kernel/fs/minix/buffer.c \
    kernel/fs/minix/bitmap.c \
    kernel/fs/minix/dir.c \
    kernel/fs/minix/file.c \
    kernel/fs/minix/inode.c \
    kernel/fs/minix/super.c

KERNEL_CORE_SRCS := \
    kernel/kernel/cmdline.c \
    kernel/kernel/fork.c \
    kernel/kernel/execve.c \
    kernel/kernel/initfs_exec.c \
    kernel/kernel/vfs_exec.c \
    kernel/kernel/panic.c \
    kernel/kernel/sched.c \
    kernel/kernel/selftest.c \
    kernel/kernel/symbols.c \
    kernel/kernel/main.c

C_SRCS := \
    $(KERNEL_LIB_SRCS) \
    $(KERNEL_DRIVER_SRCS) \
    $(KERNEL_ARCH_X86_64_C_SRCS) \
    $(KERNEL_MM_SRCS) \
    $(KERNEL_INITRAMFS_SRCS) \
    $(KERNEL_EXEC_SRCS) \
    $(KERNEL_SYSCALL_SRCS) \
    $(KERNEL_USER_SRCS) \
    $(KERNEL_FS_SRCS) \
    $(KERNEL_CORE_SRCS)

S_SRCS := \
    kernel/arch/x86_64/boot.S \
    kernel/arch/x86_64/isr.S \
    kernel/arch/x86_64/context.S \
    kernel/arch/x86_64/syscall_entry.S

C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)
S_OBJS := $(S_SRCS:%.S=$(OBJDIR)/%.o)
BUILDINFO_OBJ := $(OBJDIR)/buildinfo.o
OBJS := $(S_OBJS) $(C_OBJS) $(BUILDINFO_OBJ)

# ---- top-level --------------------------------------------------------------

.PHONY: all help doctor bootstrap bootstrap-limine check-limine iso iso-musl \
    kernel user musltest initramfs font ata-disk docs format clean clean-disk \
    distclean run run-disk debug debug-disk list-user-programs FORCE

all: iso

help:
	@printf '%s\n' \
	  'JNU build targets:' \
	  '  make doctor             Check required and optional host tools.' \
	  '  make bootstrap-limine   Clone/build Limine into boot/limine.' \
	  '  make                    Build native userspace, kernel ELF, and ISO.' \
	  '  make run                Build and boot the ISO in QEMU.' \
	  '  make ata-disk           Create build/disk.img using mkfs.minix when available.' \
	  '  make run-disk           Build, create disk image, and boot with the disk.' \
	  '  make debug              Boot QEMU paused for GDB (-s -S).' \
	  '  make user               Build native JNU userspace programs.' \
	  '  make musltest           Build optional musl-linked test program.' \
	  '  make iso-musl           Build ISO including musltest.' \
	  '  make docs               Build Sphinx HTML docs.' \
	  '  make clean              Remove reproducible build outputs, keep disk.img.' \
	  '  make distclean          Remove build outputs, disk image, and Limine.'

doctor:
	@missing=0; \
	for tool in $(CC) $(LD) $(NASM) $(PYTHON) make xorriso git; do \
	  if command -v "$$tool" >/dev/null 2>&1; then \
	    echo "ok: $$tool"; \
	  else \
	    echo "missing: $$tool"; missing=1; \
	  fi; \
	done; \
	if command -v mkfs.minix >/dev/null 2>&1; then echo "ok: mkfs.minix"; else echo "optional missing: mkfs.minix (needed for populated make ata-disk)"; fi; \
	if command -v $(QEMU) >/dev/null 2>&1; then echo "ok: $(QEMU)"; else echo "optional missing: $(QEMU) (needed for make run)"; fi; \
	if command -v $(SPHINXBUILD) >/dev/null 2>&1; then echo "ok: $(SPHINXBUILD)"; else echo "optional missing: $(SPHINXBUILD) (needed for make docs)"; fi; \
	if [ -f "$(LIMINE_DIR)/limine.h" ]; then echo "ok: $(LIMINE_DIR)"; else echo "missing: $(LIMINE_DIR) (run: make bootstrap-limine)"; missing=1; fi; \
	if [ "$$missing" -ne 0 ]; then \
	  echo; \
	  echo "Debian/Ubuntu: sudo apt install clang lld nasm make xorriso git mtools util-linux qemu-system-x86 sphinx-doc"; \
	  exit 1; \
	fi

bootstrap: bootstrap-limine

bootstrap-limine:
	@if [ -f "$(LIMINE_DIR)/limine.h" ]; then \
	  echo "bootstrap-limine: $(LIMINE_DIR) already exists"; \
	else \
	  git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 "$(LIMINE_DIR)"; \
	fi
	$(MAKE) -C "$(LIMINE_DIR)"

iso: $(KERNEL_ISO)

iso-musl: $(KERNEL_ISO_MUSL)

kernel: $(KERNEL_ELF)

user: $(USER_INIT)

musltest: $(MUSL_TEST)

initramfs: $(INITRAMFS)

font: $(FONT_HDR)

list-user-programs:
	@printf '%s\n' $(USER_PROGRAM_SRCS)

ata-disk: $(ATA_DISK)

docs:
	$(MAKE) -C docs html SPHINXBUILD=$(SPHINXBUILD)

format:
	clang-format -i $(C_SRCS) $(wildcard kernel/include/jnu/*.h) $(wildcard user/libjnu/*.c user/*/*.c)

# ---- build rules ------------------------------------------------------------

$(FONT_HDR): scripts/gen-font.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/gen-font.py $@

$(OBJDIR)/%.o: %.c $(FONT_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILDINFO_OBJ): scripts/gen-buildinfo.sh FORCE
	@mkdir -p $(dir $@)
	@VERSION="$(VERSION)" \
	  SHA="$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
	  BUILDTIME="$$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
	  bash scripts/gen-buildinfo.sh $(OBJDIR)/buildinfo.c
	$(CC) $(KERNEL_CFLAGS) -c $(OBJDIR)/buildinfo.c -o $@

FORCE:

$(KERNEL_ELF): $(OBJS) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(OBJS)

# ---- ISO / userspace --------------------------------------------------------

check-limine:
	@if [ ! -f "$(LIMINE_DIR)/limine.h" ]; then \
	  echo "ERROR: $(LIMINE_DIR)/limine.h missing."; \
	  echo "Run: make bootstrap-limine"; \
	  exit 1; \
	fi

$(USER_INIT): $(USER_BUILD_DEPS)
	@bash scripts/build-user.sh "$(BUILD)"

$(MUSL_TEST): scripts/build-musl-user.sh user/musltest/main.c
	@bash scripts/build-musl-user.sh "$(BUILD)"

$(INITRAMFS): scripts/make-initramfs.sh $(USER_INIT) FORCE
	@mkdir -p $(dir $@)
	@bash scripts/make-initramfs.sh "$@" "$(USER_ROOT)"

$(INITRAMFS_MUSL): scripts/make-initramfs.sh $(USER_INIT) $(MUSL_TEST) FORCE
	@mkdir -p $(dir $@)
	@bash scripts/make-initramfs.sh "$@" "$(USER_ROOT)"

$(KERNEL_ISO): $(KERNEL_ELF) $(INITRAMFS) boot/limine.cfg | check-limine
	@bash scripts/make-image.sh "$(KERNEL_ELF)" "boot/limine.cfg" \
	    "$(LIMINE_DIR)" "$(ISO_ROOT)" "$@" "$(INITRAMFS)"

$(KERNEL_ISO_MUSL): $(KERNEL_ELF) $(INITRAMFS_MUSL) boot/limine.cfg | check-limine
	@bash scripts/make-image.sh "$(KERNEL_ELF)" "boot/limine.cfg" \
	    "$(LIMINE_DIR)" "$(ISO_ROOT_MUSL)" "$@" "$(INITRAMFS_MUSL)"

$(ATA_DISK): $(USER_INIT) scripts/make-ata-disk.sh scripts/inject-file.py
	@bash scripts/make-ata-disk.sh "$@" "$(SIZE)"

# ---- run / debug ------------------------------------------------------------

ifeq ($(OS),Windows_NT)
RUN_SCRIPT := powershell.exe -ExecutionPolicy Bypass -File scripts\run-qemu.ps1 -Iso $(KERNEL_ISO) -Memory $(MEMORY) -Cpu $(CPU)
RUN_DISK_ARG = -Disk $(ATA_DISK)
else
RUN_SCRIPT := bash scripts/run-qemu.sh --iso $(KERNEL_ISO) --memory $(MEMORY) --cpu $(CPU)
RUN_DISK_ARG = --disk $(ATA_DISK)
endif

run: $(KERNEL_ISO)
	$(RUN_SCRIPT) $(if $(wildcard $(ATA_DISK)),$(RUN_DISK_ARG),)

run-disk: $(KERNEL_ISO) $(ATA_DISK)
	$(RUN_SCRIPT) $(RUN_DISK_ARG)

debug: $(KERNEL_ISO)
	$(RUN_SCRIPT) --debug

debug-disk: $(KERNEL_ISO) $(ATA_DISK)
	$(RUN_SCRIPT) $(RUN_DISK_ARG) --debug

# ---- housekeeping -----------------------------------------------------------

clean:
	rm -rf $(BUILD)/obj $(BUILD)/generated $(BUILD)/kernel.elf \
	       $(BUILD)/kernel.iso $(BUILD)/kernel-musl.iso $(BUILD)/iso_root \
	       $(BUILD)/iso_root_musl $(BUILD)/initramfs.cpio \
	       $(BUILD)/initramfs-musl.cpio $(BUILD)/user docs/build \
	       kernel/drivers/font_data.h

clean-disk: clean
	rm -f $(ATA_DISK)

distclean: clean-disk
	rm -rf $(LIMINE_DIR)
