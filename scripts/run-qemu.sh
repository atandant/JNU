#!/usr/bin/env bash
# scripts/run-qemu.sh — boot the JNU ISO under Linux QEMU.

set -euo pipefail

QEMU="${QEMU:-qemu-system-x86_64}"
ISO="build/kernel.iso"
MEMORY="850M"
CPU="qemu64,+smep,+smap"
DEBUG=0
DISK=""
DISK_TYPE="ide"
DISK2=""
DISK2_TYPE="ide"
FUZZY=0
FUZZ_SEED="${FUZZ_SEED:-}"

pick() {
    local idx=$((RANDOM % $#))
    local vals=("$@")
    echo "${vals[$idx]}"
}

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
    --fuzzy)
      FUZZY=1
      shift
      ;;
    --fuzz-seed)
      FUZZ_SEED="$2"
      shift 2
      ;;
    --disk-type)
      DISK_TYPE="$2"
      shift 2
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
    --disk2-type)
      DISK2_TYPE="$2"
      shift 2
      ;;
    --disk2)
      if [[ $# -gt 1 && "$2" != --* ]]; then
        DISK2="$2"
        shift 2
      else
        DISK2="build/disk2.img"
        shift
      fi
      ;;
    *)
      echo "usage: run-qemu.sh [--iso path] [--memory size] [--cpu model] [--disk [path]] [--disk-type ide|ahci|virtio|scsi] [--disk2 [path]] [--disk2-type ide|ahci|virtio|scsi] [--debug] [--fuzzy] [--fuzz-seed seed]" >&2
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

MACHINE="q35"
SMP=1
EXTRA_ARGS=()

if [ "$FUZZY" -eq 1 ]; then
    if [ -z "$FUZZ_SEED" ]; then
        FUZZ_SEED=$(( $(date +%s) ^ $$ ))
    fi
    RANDOM=$FUZZ_SEED

    MACHINE=$(pick q35 pc)
    MEMORY="$((64 + RANDOM % 3009))M"
    CPU=$(pick qemu64 qemu64,+smep,+smap max)
    SMP=$((1 + RANDOM % 4))
    DISK_TYPE=$(pick ide ahci virtio scsi)

    if [ $((RANDOM % 2)) -eq 0 ]; then
        EXTRA_ARGS+=("-device" "virtio-rng-pci")
    fi
    if [ $((RANDOM % 2)) -eq 0 ]; then
        EXTRA_ARGS+=("-netdev" "user,id=net0" "-device" "$(pick e1000 virtio-net-pci),netdev=net0")
    else
        EXTRA_ARGS+=("-nic" "none")
    fi

    echo "Fuzz seed: $FUZZ_SEED"
    echo "Fuzz profile: machine=$MACHINE memory=$MEMORY cpu=$CPU smp=$SMP disk_type=${DISK_TYPE:-none}"
fi

ARGS=("-machine" "$MACHINE" "-m" "$MEMORY" "-cpu" "$CPU" "-smp" "$SMP" "-cdrom" "$ISO" "-boot" "d" "-serial" "stdio" "-no-reboot" "-no-shutdown")
ARGS+=("${EXTRA_ARGS[@]}")

if [ "$DEBUG" -eq 1 ]; then
    ARGS+=("-s" "-S")
fi

if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    if [ "$DISK_TYPE" = "virtio" ]; then
        ARGS+=("-drive" "id=vd0,file=$DISK,format=raw,if=none" "-device" "virtio-blk-pci,drive=vd0")
    elif [ "$DISK_TYPE" = "ahci" ]; then
        ARGS+=("-device" "ich9-ahci,id=ahci" "-drive" "id=hd0,file=$DISK,format=raw,if=none" "-device" "ide-hd,drive=hd0,bus=ahci.0")
    elif [ "$DISK_TYPE" = "scsi" ]; then
        ARGS+=("-device" "virtio-scsi-pci,id=scsi0" "-drive" "id=sd0,file=$DISK,format=raw,if=none" "-device" "scsi-hd,drive=sd0,bus=scsi0.0")
    else
        ARGS+=("-device" "piix3-ide,id=ide" "-drive" "id=hd0,file=$DISK,format=raw,if=none" "-device" "ide-hd,drive=hd0,bus=ide.0")
    fi
elif [ -n "$DISK" ]; then
    echo "Error: disk image not found at $DISK." >&2
    exit 1
fi

# Optional second disk for multi-mount testing. Each transport gets its
# own controller/ids so it is independent of the primary disk's type. The
# ide case attaches as the primary-bus slave (unit=1), so JNU enumerates
# it as the second drive (hdb).
if [ -n "$DISK2" ] && [ -f "$DISK2" ]; then
    if [ "$DISK2_TYPE" = "virtio" ]; then
        ARGS+=("-drive" "id=vd1,file=$DISK2,format=raw,if=none" "-device" "virtio-blk-pci,drive=vd1")
    elif [ "$DISK2_TYPE" = "ahci" ]; then
        ARGS+=("-device" "ich9-ahci,id=ahci2" "-drive" "id=hd2,file=$DISK2,format=raw,if=none" "-device" "ide-hd,drive=hd2,bus=ahci2.0")
    elif [ "$DISK2_TYPE" = "scsi" ]; then
        ARGS+=("-device" "virtio-scsi-pci,id=scsi2" "-drive" "id=sd2,file=$DISK2,format=raw,if=none" "-device" "scsi-hd,drive=sd2,bus=scsi2.0")
    else
        if [ "$DISK_TYPE" != "ide" ]; then
            ARGS+=("-device" "piix3-ide,id=ide")
        fi
        ARGS+=("-drive" "id=hd1,file=$DISK2,format=raw,if=none" "-device" "ide-hd,drive=hd1,bus=ide.0,unit=1")
    fi
elif [ -n "$DISK2" ]; then
    echo "Error: disk2 image not found at $DISK2." >&2
    exit 1
fi

echo "Launching: $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
