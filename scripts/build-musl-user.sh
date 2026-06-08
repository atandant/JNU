#!/usr/bin/env bash
# scripts/build-musl-user.sh — Build a static musl-linked user program.
#
# Usage: build-musl-user.sh <build-dir>
#
# Expects musl installed at user/musl/install/ (headers + libc.a + crt*.o).
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "usage: build-musl-user.sh <build-dir>" >&2
	exit 2
fi

build="$1"
cc="${CC:-clang}"
ld="${LD:-ld.lld}"

musl="user/musl/install"
obj="$build/obj/musltest"
out="$build/user/bin"

if [ ! -f "$musl/lib/libc.a" ]; then
	echo "build-musl-user.sh: $musl/lib/libc.a not found." >&2
	echo "Build musl first — see docs/source/musl.rst" >&2
	exit 1
fi

mkdir -p "$obj" "$out"

# Compile — note: no -mgeneral-regs-only; user code may use SSE (musl does).
"$cc" \
	--target=x86_64-unknown-linux-musl \
	-std=gnu17 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-mno-red-zone \
	-nostdinc \
	-isystem "$musl/include" \
	-O2 -g3 \
	-c user/musltest/main.c -o "$obj/main.o"

# Link against musl's crt + libc.a, fully static.
"$ld" \
	-static \
	-nostdlib \
	"$musl/lib/crt1.o" \
	"$musl/lib/crti.o" \
	"$obj/main.o" \
	"$musl/lib/libc.a" \
	"$musl/lib/crtn.o" \
	-o "$out/musltest"

echo "build-musl-user.sh: built $out/musltest"
