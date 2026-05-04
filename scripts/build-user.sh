#!/usr/bin/env bash
# scripts/build-user.sh - Build freestanding JNU userspace programs.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "usage: build-user.sh <build-dir>" >&2
	exit 2
fi

build="$1"
cc="${CC:-clang}"
ld="${LD:-ld.lld}"
nasm="${NASM:-nasm}"

obj="$build/obj/user"
out="$build/user"

cflags=(
	--target=x86_64-unknown-none-elf
	-std=gnu17
	-ffreestanding
	-fno-stack-protector
	-fno-pic -fno-pie
	-mno-red-zone
	-mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only
	-Wall -Wextra -Wpedantic -Werror
	-O2 -g3
	-nostdinc
	-Iuser/libjnu/include
)

ldflags=(
	-nostdlib
	--no-pie
	-static
	-z
	max-page-size=0x1000
	-Ttext=0x400000
)

lib_c=(
	user/libjnu/close.c
	user/libjnu/execve.c
	user/libjnu/exit.c
	user/libjnu/fstat.c
	user/libjnu/fork.c
	user/libjnu/getpid.c
	user/libjnu/lseek.c
	user/libjnu/open.c
	user/libjnu/read.c
	user/libjnu/waitpid.c
	user/libjnu/write.c
	user/libjnu/yield.c
)

mkdir -p "$obj/libjnu" "$out/bin"

"$nasm" -f elf64 -F dwarf -g user/libjnu/crt0.S -o "$obj/libjnu/crt0.o"
"$nasm" -f elf64 -F dwarf -g user/libjnu/syscall.S -o "$obj/libjnu/syscall.o"

lib_objs=("$obj/libjnu/crt0.o" "$obj/libjnu/syscall.o")
for src in "${lib_c[@]}"; do
	base="$(basename "$src" .c)"
	dst="$obj/libjnu/$base.o"
	"$cc" "${cflags[@]}" -c "$src" -o "$dst"
	lib_objs+=("$dst")
done

build_prog() {
	local src="$1"
	local dst="$2"
	local name
	name="$(basename "$(dirname "$src")")"
	mkdir -p "$(dirname "$dst")" "$obj/$name"
	"$cc" "${cflags[@]}" -c "$src" -o "$obj/$name/main.o"
	"$ld" "${ldflags[@]}" -o "$dst" "${lib_objs[@]}" "$obj/$name/main.o"
}

programs=()
while IFS= read -r src; do
	programs+=("$src")
done < <(find user -mindepth 2 -maxdepth 2 -name main.c \
	! -path 'user/libjnu/*' ! -path 'user/musl/*' ! -path 'user/musltest/*' | sort)

if [ "${#programs[@]}" -eq 0 ]; then
	echo "user: no user/<program>/main.c files found" >&2
	exit 1
fi

built=()
for src in "${programs[@]}"; do
	name="$(basename "$(dirname "$src")")"
	if [ "$name" = "init" ]; then
		dst="$out/init"
	else
		dst="$out/bin/$name"
	fi
	build_prog "$src" "$dst"
	built+=("$dst")
done

echo "user: built ${built[*]}"
