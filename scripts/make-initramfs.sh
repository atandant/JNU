#!/usr/bin/env bash
# scripts/make-initramfs.sh - Build the v0.0.2 bootstrap initramfs.
#
# Usage:
#   bash scripts/make-initramfs.sh <out.cpio> [user-root]
#
# The archive is cpio "newc". Phase 1 ships placeholder files so the
# kernel can validate Limine module loading and archive parsing before
# real userspace binaries exist.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: make-initramfs.sh <out.cpio> [user-root]" >&2
	exit 2
fi

out="$1"
user_root="${2:-}"
mkdir -p "$(dirname "$out")"

python3 - "$out" "$user_root" <<'PY'
import os
import stat
import sys

out = sys.argv[1]
user_root = sys.argv[2]

def read_or_placeholder(path, data):
    if user_root:
        try:
            with open(f"{user_root}/{path}", "rb") as f:
                return f.read()
        except FileNotFoundError:
            pass
    return data

entries = [
    ("init", stat.S_IFREG | 0o755, read_or_placeholder("init", b"JNU phase 4 placeholder /init\n")),
    ("bin", stat.S_IFDIR | 0o755, b""),
]

if user_root and os.path.isdir(f"{user_root}/bin"):
    for name in sorted(os.listdir(f"{user_root}/bin")):
        path = f"bin/{name}"
        full = f"{user_root}/{path}"
        if os.path.isfile(full):
            with open(full, "rb") as f:
                entries.append((path, stat.S_IFREG | 0o755, f.read()))
else:
    entries.append(("bin/hello", stat.S_IFREG | 0o755,
                    read_or_placeholder("bin/hello", b"JNU phase 4 placeholder /bin/hello\n")))

entries += [
    ("etc", stat.S_IFDIR | 0o755, b""),
    ("etc/motd", stat.S_IFREG | 0o644, b"Welcome to JNU 0.0.2 phase 4\n"),
]

def pad4(blob):
    return blob + (b"\0" * ((4 - (len(blob) % 4)) % 4))

def pad_archive(archive):
    archive += b"\0" * ((4 - (len(archive) % 4)) % 4)

def hdr(name, mode, data):
    namesize = len(name.encode("utf-8")) + 1
    fields = [
        "070701",
        f"{0:08x}",
        f"{mode:08x}",
        f"{0:08x}",
        f"{0:08x}",
        f"{1:08x}",
        f"{0:08x}",
        f"{len(data):08x}",
        f"{0:08x}",
        f"{0:08x}",
        f"{0:08x}",
        f"{0:08x}",
        f"{namesize:08x}",
        f"{0:08x}",
    ]
    return "".join(fields).encode("ascii")

archive = bytearray()
for name, mode, data in entries:
    archive += hdr(name, mode, data)
    archive += name.encode("utf-8") + b"\0"
    pad_archive(archive)
    archive += data
    pad_archive(archive)

trailer = "TRAILER!!!"
archive += hdr(trailer, 0, b"")
archive += trailer.encode("ascii") + b"\0"
pad_archive(archive)

with open(out, "wb") as f:
    f.write(archive)
PY

echo "initramfs: wrote $out"
