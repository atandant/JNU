#!/usr/bin/env bash
# scripts/run-qemu.sh — boot the JNU ISO under Linux QEMU.

set -euo pipefail

QEMU="${QEMU:-qemu-system-x86_64}"
ISO="build/kernel.iso"
MEMORY="850M"
CPU="qemu64,+smep,+smap"
DEBUG=0
DISK=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --iso)
      ISO="$2"
      shift 2
      ;;
    --memory)
      MEMORY="$2"
      shift 2
      ;;
    --cpu)
      CPU="$2"
      shift 2
      ;;
    --debug)
      DEBUG=1
      shift
      ;;
    --disk)
      if [[ $# -gt 1 && "$2" != --* ]]; then
        DISK="$2"
        shift 2
      else
        DISK="build/disk.img"
        shift
      fi
      ;;
    *)
      echo "usage: run-qemu.sh [--iso path] [--memory size] [--cpu model] [--disk [path]] [--debug]" >&2
      exit 2
      shift
      ;;
  esac
done

if ! command -v "$QEMU" &> /dev/null; then
    echo "Error: $QEMU not found on PATH."
    exit 1
fi

if [ ! -f "$ISO" ]; then
    echo "Error: ISO not found at $ISO. Build it first."
    exit 1
fi

ARGS=("-machine" "q35" "-m" "$MEMORY" "-cpu" "$CPU" "-cdrom" "$ISO" "-boot" "d" "-serial" "stdio" "-no-reboot" "-no-shutdown")

if [ "$DEBUG" -eq 1 ]; then
    ARGS+=("-s" "-S")
fi

if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    ARGS+=("-device" "piix3-ide,id=ide" "-drive" "id=hd0,file=$DISK,format=raw,if=none" "-device" "ide-hd,drive=hd0,bus=ide.0")
elif [ -n "$DISK" ]; then
    echo "Error: disk image not found at $DISK." >&2
    exit 1
fi

echo "Launching: $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
