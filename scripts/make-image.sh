#!/usr/bin/env bash
# scripts/make-image.sh — build a Limine bootable ISO.
#
# Usage:
#   make-image.sh <kernel.elf> <limine.cfg> <limine-dir> <iso-root> <out-iso>
#
# Stages the kernel + limine binaries into <iso-root>, then runs
# xorriso to produce a hybrid BIOS/UEFI bootable ISO. Finally, runs
# `limine bios-install` on the produced ISO so the BIOS boot sector is
# patched in.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [ "$#" -ne 5 ]; then
	echo "usage: make-image.sh <kernel.elf> <limine.cfg> <limine-dir> <iso-root> <out-iso>" >&2
	exit 2
fi

kernel="$1"
limine_cfg="$2"
limine_dir="$3"
iso_root="$4"
out_iso="$5"

if [ ! -d "$limine_dir" ] || [ ! -f "$limine_dir/limine-bios.sys" ]; then
	echo "make-image.sh: $limine_dir is missing or has no built Limine." >&2
	echo "Run:" >&2
	echo "  git clone https://github.com/limine-bootloader/limine.git \\" >&2
	echo "      --branch=v8.x-binary --depth=1 $limine_dir" >&2
	echo "  make -C $limine_dir" >&2
	exit 1
fi

rm -rf "$iso_root"
mkdir -p "$iso_root/EFI/BOOT"

cp "$kernel"     "$iso_root/kernel.elf"
cp "$limine_cfg" "$iso_root/limine.cfg"

cp "$limine_dir/limine-bios.sys"     "$iso_root/"
cp "$limine_dir/limine-bios-cd.bin"  "$iso_root/"
cp "$limine_dir/limine-uefi-cd.bin"  "$iso_root/"
cp "$limine_dir/BOOTX64.EFI"         "$iso_root/EFI/BOOT/"
if [ -f "$limine_dir/BOOTIA32.EFI" ]; then
	cp "$limine_dir/BOOTIA32.EFI" "$iso_root/EFI/BOOT/"
fi

xorriso -as mkisofs \
	-b limine-bios-cd.bin \
	-no-emul-boot -boot-load-size 4 -boot-info-table \
	--efi-boot limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label \
	"$iso_root" -o "$out_iso"

if [ -x "$limine_dir/limine" ]; then
	"$limine_dir/limine" bios-install "$out_iso"
else
	echo "make-image.sh: $limine_dir/limine binary not found; \
the ISO will boot under UEFI but BIOS boot is not stamped." >&2
fi

echo "make-image.sh: wrote $out_iso"
