#!/usr/bin/env python3
# scripts/migrate-includes.py — one-shot include tree reorganization.
# Copyright (c) 2026 The JNU Authors. SPDX-License-Identifier: GPL-2.0-only

from __future__ import annotations

import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_INCLUDE = ROOT / "kernel" / "include"
DST_INCLUDE = ROOT / "include"

UAPI_HEADERS = {"syscall_nr.h", "errno.h", "mman.h"}

JNU_MAP: dict[str, str] = {
    "types.h": "jnu/base/types.h",
    "compiler.h": "jnu/base/compiler.h",
    "pmm.h": "jnu/mm/pmm.h",
    "vmm.h": "jnu/mm/vmm.h",
    "vma.h": "jnu/mm/vma.h",
    "slab.h": "jnu/mm/slab.h",
    "kmalloc.h": "jnu/mm/kmalloc.h",
    "paging.h": "jnu/mm/paging.h",
    "vfs.h": "jnu/fs/vfs.h",
    "minix.h": "jnu/fs/minix.h",
    "block.h": "jnu/fs/block.h",
    "initramfs.h": "jnu/fs/initramfs.h",
    "cpio_newc.h": "jnu/fs/cpio_newc.h",
    "ata.h": "jnu/drivers/ata.h",
    "pci.h": "jnu/drivers/pci.h",
    "virtio_blk.h": "jnu/drivers/virtio_blk.h",
    "serial.h": "jnu/drivers/serial.h",
    "pit.h": "jnu/drivers/pit.h",
    "rtc.h": "jnu/drivers/rtc.h",
    "kbd.h": "jnu/drivers/kbd.h",
    "fbcon.h": "jnu/drivers/fbcon.h",
    "acpi.h": "jnu/drivers/acpi.h",
    "apic.h": "jnu/drivers/apic.h",
    "hpet.h": "jnu/drivers/hpet.h",
    "lapic_timer.h": "jnu/drivers/lapic_timer.h",
    "chardev.h": "jnu/drivers/chardev.h",
    "scandata.h": "jnu/drivers/scandata.h",
    "keycode.h": "jnu/drivers/keycode.h",
    "io.h": "jnu/drivers/io.h",
    "cpu.h": "jnu/arch/cpu.h",
    "gdt.h": "jnu/arch/gdt.h",
    "idt.h": "jnu/arch/idt.h",
    "context.h": "jnu/arch/context.h",
    "usermode.h": "jnu/arch/usermode.h",
    "arch_syscall.h": "jnu/arch/arch_syscall.h",
    "sched.h": "jnu/kernel/sched.h",
    "process.h": "jnu/kernel/process.h",
    "panic.h": "jnu/kernel/panic.h",
    "cmdline.h": "jnu/kernel/cmdline.h",
    "symbols.h": "jnu/kernel/symbols.h",
    "selftest.h": "jnu/kernel/selftest.h",
    "exec.h": "jnu/kernel/exec.h",
    "elf64.h": "jnu/kernel/elf64.h",
    "execprot.h": "jnu/kernel/execprot.h",
    "string.h": "jnu/lib/string.h",
    "rbtree.h": "jnu/lib/rbtree.h",
    "spinlock.h": "jnu/lib/spinlock.h",
    "mutex.h": "jnu/lib/mutex.h",
    "klog.h": "jnu/lib/klog.h",
    "prng.h": "jnu/lib/prng.h",
    "fd.h": "jnu/user/fd.h",
    "syscall.h": "jnu/user/syscall.h",
    "usercopy.h": "jnu/user/usercopy.h",
}


def remap_include_path(path: str) -> str:
    if path.startswith("jnu/"):
        base = path.split("/", 1)[1]
        if base in UAPI_HEADERS:
            return f"uapi/jnu/{base}"
        if base in JNU_MAP:
            return JNU_MAP[base]
    if path == "jnu/types.h":
        return "jnu/base/types.h"
    return path


def rewrite_includes(text: str) -> str:
    def repl(m: re.Match[str]) -> str:
        old = m.group(1)
        new = remap_include_path(old)
        return f'#include <{new}>'

    return re.sub(r'#include\s+<([^>]+)>', repl, text)


def main() -> None:
    jnu_src = SRC_INCLUDE / "jnu"
    if not jnu_src.is_dir():
        raise SystemExit(f"missing {jnu_src}")

    if DST_INCLUDE.exists():
        shutil.rmtree(DST_INCLUDE)
    DST_INCLUDE.mkdir()

    # UAPI headers from existing jnu/
    uapi_dir = DST_INCLUDE / "uapi" / "jnu"
    uapi_dir.mkdir(parents=True)
    for name in UAPI_HEADERS:
        src = jnu_src / name
        text = src.read_text()
        text = rewrite_includes(text)
        text = text.replace("include/jnu/", "include/uapi/jnu/")
        (uapi_dir / name).write_text(text)

    # stat.h extracted from fd.h
    stat_h = """\
/*
 * include/uapi/jnu/stat.h — Userspace-visible file metadata.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>

struct jnu_stat {
	uint64_t ino;
	uint64_t size;
	uint32_t mode;
	uint32_t type;
};
"""
    (uapi_dir / "stat.h").write_text(stat_h)

    arch_prctl_h = """\
/*
 * include/uapi/jnu/arch_prctl.h — arch_prctl request codes (x86_64).
 *
 * Linux-compatible values for musl TLS setup via ARCH_SET_FS.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004
"""
    (uapi_dir / "arch_prctl.h").write_text(arch_prctl_h)

    # jnu/ subsystem headers
    for name, rel in JNU_MAP.items():
        src = jnu_src / name
        dst = DST_INCLUDE / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        text = src.read_text()
        text = rewrite_includes(text)
        text = text.replace("include/jnu/", f"include/{rel.rsplit('/', 1)[0]}/")
        if name == "fd.h":
            text = re.sub(
                r"struct jnu_stat \{[^}]+\};\n\n",
                "",
                text,
                count=1,
                flags=re.DOTALL,
            )
            if "#include <uapi/jnu/stat.h>" not in text:
                text = text.replace(
                    "#pragma once\n\n",
                    "#pragma once\n\n#include <uapi/jnu/stat.h>\n",
                )
        (dst).write_text(text)

    # stdint.h shim at include root
    (DST_INCLUDE / "stdint.h").write_text(
        "#pragma once\n#include <jnu/base/types.h>\n"
    )

    # Rewrite all source files
    patterns = ("*.c", "*.h", "*.S", "*.sh", "*.py")
    skip_dirs = {".git", "build", "boot/limine", "third_party"}
    for pat in patterns:
        for path in ROOT.rglob(pat):
            if any(part in skip_dirs for part in path.parts):
                continue
            if path.is_relative_to(DST_INCLUDE):
                continue
            if path.is_relative_to(SRC_INCLUDE):
                continue
            old = path.read_text()
            new = rewrite_includes(old)
            if new != old:
                path.write_text(new)

    # Remove old kernel/include
    shutil.rmtree(SRC_INCLUDE)
    print(f"migrated {len(JNU_MAP)} jnu headers + {len(UAPI_HEADERS)} uapi headers")
    print(f"removed {SRC_INCLUDE}")


if __name__ == "__main__":
    main()
