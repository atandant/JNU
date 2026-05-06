#!/bin/bash
# scripts/run-qemu.sh — boot the JNU ISO under Linux QEMU.

QEMU="${QEMU:-qemu-system-x86_64}"
ISO="build/kernel.iso"
MEMORY="850M"
DEBUG=0
DISK=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --debug)
      DEBUG=1
      shift
      ;;
    --disk)
      DISK="build/disk.img"
      shift
      ;;
    *)
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

ARGS=("-machine" "q35" "-m" "$MEMORY" "-cpu" "qemu64,+smep,+smap" "-cdrom" "$ISO" "-boot" "d" "-serial" "stdio" "-no-reboot" "-no-shutdown")

if [ "$DEBUG" -eq 1 ]; then
    ARGS+=("-s" "-S")
fi

if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    ARGS+=("-device" "piix3-ide,id=ide" "-drive" "id=hd0,file=$DISK,format=raw,if=none" "-device" "ide-hd,drive=hd0,bus=ide.0")
fi

echo "Launching: $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
