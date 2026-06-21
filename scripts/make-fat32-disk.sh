#!/usr/bin/env bash
# scripts/make-fat32-disk.sh — Create a raw FAT32 disk image for JNU testing.
#
# Usage:
#   bash scripts/make-fat32-disk.sh [output] [size_mib]
#
# Defaults:
#   output   = build/disk-fat32.img
#   size_mib = 36            (FAT32 requires at least 65525 clusters, ~33 MiB minimum)

set -e

OUT="${1:-build/disk-fat32.img}"
SIZE_MIB="${2:-36}"
if [ "$SIZE_MIB" -lt 36 ]; then
    SIZE_MIB=36
fi

mkdir -p "$(dirname "$OUT")"

echo "fat32-disk: creating ${SIZE_MIB} MiB raw image → ${OUT}"

# Allocate the raw image (sparse where possible).
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MIB" status=none

if command -v mkfs.vfat &>/dev/null && command -v mcopy &>/dev/null; then
    echo "fat32-disk: formatting FAT32 filesystem"
    mkfs.vfat -F 32 "$OUT" 2>&1 | sed 's/^/fat32-disk: /'

    echo "fat32-disk: injecting test file..."
    TMP_HELLO=$(mktemp)
    printf "Hello from JNU FAT32!\n" > "$TMP_HELLO"
    mcopy -i "$OUT" "$TMP_HELLO" ::HELLO.TXT
    rm -f "$TMP_HELLO"
else
    echo "fat32-disk: mkfs.vfat or mcopy not found — image is blank"
    echo "fat32-disk: Install dosfstools and mtools to get a populated FAT32 FS:"
    echo "fat32-disk:   sudo apt-get install dosfstools mtools"
fi

echo "fat32-disk: done — ${OUT} ($(du -h "$OUT" | cut -f1))"
