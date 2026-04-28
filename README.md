# JNU — J is not Unix

A monolithic, x86_64, freestanding kernel written from scratch in
GNU C17 + Intel-syntax NASM, booted by Limine. Licensed GPLv2.

The full contract for the kernel is in [jnuspec.md](jnuspec.md).

## Phase 1 status

This tree implements Phase 1: boot via Limine, klog over COM1 + framebuffer,
idle loop. See §7 of `jnuspec.md`.

## Prerequisites

Build host: Windows 11 + WSL2 (Ubuntu 22.04+).

In WSL:

```
sudo apt install clang lld nasm make xorriso mtools
```

For font generation (run once on host or inside WSL):

```
pip install pillow
```

QEMU runs from the **Windows desktop** install (e.g. installed from
`https://qemu.weilnetz.de/`). The `make run` target invokes the Windows
QEMU binary through a path you can override; see `scripts/run-qemu.ps1`
for the PowerShell counterpart you can run from Windows directly.

## Bootstrap (run once)

Clone Limine into `boot/limine/`:

```
git clone https://github.com/limine-bootloader/limine.git \
    --branch=v8.x-binary --depth=1 boot/limine
make -C boot/limine
```

## Build

From WSL, in this directory:

```
make             # builds build/kernel.elf and build/kernel.iso
```

## Run

From Windows PowerShell:

```
scripts\run-qemu.ps1
```

Or from WSL (if Windows QEMU is on PATH or via `$QEMU` env var):

```
make run
```

## Layout

See §5 of `jnuspec.md`. Phase-1-relevant pieces:

- `kernel/arch/x86_64/boot.S` — Limine entry stub.
- `kernel/arch/x86_64/linker.ld` — kernel image at `0xFFFFFFFF80000000`.
- `kernel/lib/printk.c` — vsnprintf, ring buffer, klog.
- `kernel/drivers/serial.c` — COM1 16550 polling output.
- `kernel/drivers/fbcon.c` — framebuffer console.
- `kernel/kernel/main.c` — `kernel_main`.
- `scripts/gen-font.py` — generates the embedded 8x16 console font.
