#!/usr/bin/env bash
# scripts/prepare-vmware-disk.sh — Build a MINIX disk image for VMware.
#
# Creates build/disk.img via make ata-disk, then converts it to
# build/disk.vmdk for attachment as IDE 0:0 in VMware Workstation/Fusion.
#
# Usage:
#   bash scripts/prepare-vmware-disk.sh
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RAW="$ROOT/build/disk.img"
VMDK="$ROOT/build/disk.vmdk"

cd "$ROOT"

if ! command -v qemu-img &>/dev/null; then
	echo "prepare-vmware-disk: qemu-img not found" >&2
	echo "prepare-vmware-disk: install qemu-utils (Debian/Ubuntu) or qemu-img" >&2
	exit 1
fi

make ata-disk

echo "prepare-vmware-disk: converting $RAW → $VMDK"
qemu-img convert -f raw -O vmdk "$RAW" "$VMDK"

echo "prepare-vmware-disk: done"
echo "prepare-vmware-disk: attach $VMDK (or $RAW) as IDE 0:0 in VMware"
echo "prepare-vmware-disk: remove any blank wizard-created disk from the VM"
