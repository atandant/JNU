#!/usr/bin/env bash
# scripts/make-ata-disk.sh — Create a raw ATA disk image for JNU testing.
#
# Usage:
#   bash scripts/make-ata-disk.sh [output] [size_mib]
#
# Defaults:
#   output   = build/disk.img
#   size_mib = 32            (32 MiB, enough for a MINIX v1 test FS)
#
# The image is a raw, unpartitioned disk. The first sector is left
# blank (or written with a trivial test pattern so ata_selftest can
# verify sector 0 was read successfully). The rest of the image is
# zero-filled so QEMU's ATA PIO emulation returns clean data.
#
# Requires: dd (coreutils). Optional: mkfs.minix (util-linux) for a
# real MINIX v1 filesystem — the script fills it if present.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -e

OUT="${1:-build/disk.img}"
SIZE_MIB="${2:-32}"

mkdir -p "$(dirname "$OUT")"

echo "ata-disk: creating ${SIZE_MIB} MiB raw image → ${OUT}"

# Allocate the raw image (sparse where possible).
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MIB" status=none

# Write a recognisable magic signature at bytes 0–7 of sector 0 so
# ata_selftest can confirm the sector was actually read from disk and
# not just zeroed memory. "JNUDISK\n" (8 bytes, no null yet — the
# null lands at offset 8 naturally due to dd padding).
printf 'JNUDISK\n' | dd of="$OUT" bs=1 count=8 conv=notrunc status=none

# Optionally lay down a MINIX v1 filesystem if mkfs.minix is present.
# The filesystem starts at offset 0; MINIX v1 superblock is at byte
# 1024 with magic 0x137F. We use -1 (v1) and -n 14 (14-byte names).
if command -v mkfs.minix &>/dev/null; then
    echo "ata-disk: mkfs.minix found — laying MINIX v1 filesystem"
    mkfs.minix -1 -n 14 "$OUT" 2>&1 | sed 's/^/ata-disk: /'
    
    if command -v python3 &>/dev/null; then
        echo "ata-disk: injecting test file..."
        python3 scripts/inject-file.py "$OUT" "test.txt" "Hello from JNU test file!"
        if [ -d build/user/bin ]; then
            for prog in build/user/bin/*; do
                [ -f "$prog" ] || continue
                name="$(basename "$prog")"
                echo "ata-disk: injecting userspace program /$name"
                python3 scripts/inject-file.py "$OUT" "$name" "@$prog"
            done
        fi
    fi
else
    echo "ata-disk: mkfs.minix not found — image has signature only (no FS)"
    echo "ata-disk: Install util-linux and re-run to get a MINIX v1 FS:"
    echo "ata-disk:   sudo apt-get install util-linux"
fi

echo "ata-disk: done — ${OUT} ($(du -h "$OUT" | cut -f1))"
