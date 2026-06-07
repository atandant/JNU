# scripts/run-qemu.ps1 — boot the JNU ISO under Windows-desktop QEMU.
#
# Run from the repository root (PowerShell):
#
#   .\scripts\run-qemu.ps1
#
# Override the QEMU binary path if it is not on PATH:
#
#   $env:QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
#   .\scripts\run-qemu.ps1
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [string]$Iso = "build/kernel.iso",
    [string]$Memory = "850M",
    [string]$Cpu = "qemu64,+smep,+smap",
    [string]$Disk = "",
    [switch]$Debug
)

$ErrorActionPreference = "Stop"

$qemu = $env:QEMU
if (-not $qemu) {
    $candidates = @(
        "qemu-system-x86_64.exe",
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
    )
    foreach ($c in $candidates) {
        try {
            if (Get-Command $c -ErrorAction SilentlyContinue) {
                $qemu = $c
                break
            }
        } catch { }
        if (Test-Path $c) {
            $qemu = $c
            break
        }
    }
}

if (-not $qemu) {
    Write-Error "qemu-system-x86_64.exe not found. Set `$env:QEMU to the install path."
    exit 1
}

if (-not (Test-Path $Iso)) {
    Write-Error "ISO not found at $Iso. Build it first (run 'make' in WSL)."
    exit 1
}

$args = @(
    "-machine", "q35",
    "-m",       $Memory,
    "-cpu",     $Cpu,
    "-cdrom",   $Iso,
    "-boot",    "d",
    "-serial",  "stdio",
    "-no-reboot", "-no-shutdown"
)

if ($Debug) {
    $args += @("-s", "-S")
}

if ($Disk) {
    if (-not (Test-Path $Disk)) {
        Write-Error "Disk image not found at $Disk."
        exit 1
    }
    $args += @(
        "-device", "piix3-ide,id=ide",
        "-drive",  "id=hd0,file=$Disk,format=raw,if=none",
        "-device", "ide-hd,drive=hd0,bus=ide.0"
    )
}

Write-Host "Launching: $qemu $($args -join ' ')"
& $qemu @args
