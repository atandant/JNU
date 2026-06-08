#!/usr/bin/env bash
# scripts/install-headers.sh — Install JNU UAPI headers onto a MINIX disk.
#
# Usage: install-headers.sh <minix.img>
#
# Installs include/uapi/jnu/*.h → usr/include/uapi/jnu/ on the image.
# No-op if the image has no MINIX filesystem (mkfs.minix was unavailable).
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "usage: install-headers.sh <minix.img>" >&2
	exit 2
fi

img="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

if [ ! -f "$img" ]; then
	echo "install-headers.sh: image not found: $img" >&2
	exit 1
fi

magic="$(python3 - "$img" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    f.seek(1024 + 16)
    print(struct.unpack("<H", f.read(2))[0])
PY
)"
if [ "$magic" != "4983" ] && [ "$magic" != "4991" ]; then
	echo "install-headers.sh: no MINIX v1 FS on $img (magic=$magic) — skipping"
	exit 0
fi

mkdir -p "$staging/usr/include/uapi/jnu"
cp -a "$root/include/uapi/jnu/." "$staging/usr/include/uapi/jnu/"

python3 "$root/scripts/install-headers.py" "$img" "$staging"
echo "install-headers.sh: done"
