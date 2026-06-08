#!/usr/bin/env bash
# scripts/check-uapi-sync.sh — Verify generated jnu_syscall.h matches UAPI source.
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
gen="$root/build/generated/include/user/jnu_syscall.h"
out="$root/user/libjnu/include/jnu_syscall.h"
tmp="$(mktemp)"

trap 'rm -f "$tmp"' EXIT

bash "$root/scripts/gen-uapi.sh" "$tmp"

if ! cmp -s "$tmp" "$out"; then
	echo "check-uapi-sync: $out is out of date (run: make uapi)" >&2
	diff -u "$out" "$tmp" >&2 || true
	exit 1
fi

echo "check-uapi-sync: ok"
