#!/usr/bin/env python3
# scripts/install-headers.py — Install a header tree onto a MINIX v1 image.
#
# Usage: install-headers.py <minix.img> <staging-root>
#
# Copyright (c) 2026 The JNU Authors.
# SPDX-License-Identifier: GPL-2.0-only

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

BLOCK_SIZE = 1024
MINIX_IFREG = 0x8000
MINIX_IFDIR = 0x4000
MINIX_ROOT_INO = 1
MINIX_DIRECT_ZONES = 7
MINIX_INDIRECT_PER_BLOCK = BLOCK_SIZE // 2
DIR_ENTRY_SIZE = 16


class MinixImage:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.f = path.open("r+b")
        self.f.seek(1024)
        sb = self.f.read(20)
        (
            self.ninodes,
            self.nzones,
            self.imap_blocks,
            self.zmap_blocks,
            self.firstdatazone,
            self.log_zone_size,
            self.max_size,
            self.magic,
            self.state,
        ) = struct.unpack("<HHHHHHIHH", sb)
        if self.magic not in (0x137F, 0x138F):
            raise SystemExit("Not a valid MINIX v1 image")
        if self.log_zone_size != 0:
            raise SystemExit("Only 1 KiB MINIX zones are supported")
        self.imap_offset = 2 * BLOCK_SIZE
        self.zmap_offset = self.imap_offset + self.imap_blocks * BLOCK_SIZE
        self.inode_table_offset = self.zmap_offset + self.zmap_blocks * BLOCK_SIZE
        self.imap = bytearray(self._read_at(self.imap_offset, self.imap_blocks * BLOCK_SIZE))
        self.zmap = bytearray(self._read_at(self.zmap_offset, self.zmap_blocks * BLOCK_SIZE))
        self.path_ino: dict[str, int] = {"/": MINIX_ROOT_INO}

    def _read_at(self, offset: int, size: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(size)

    def _write_at(self, offset: int, data: bytes) -> None:
        self.f.seek(offset)
        self.f.write(data)

    def _alloc_inode(self) -> int:
        for i in range(2, self.ninodes + 1):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (self.imap[byte_idx] & (1 << bit_idx)):
                self.imap[byte_idx] |= 1 << bit_idx
                return i
        raise SystemExit("No free inodes")

    def _alloc_zones(self, count: int) -> list[int]:
        zones: list[int] = []
        for zone in range(self.firstdatazone, self.nzones):
            bit = zone - self.firstdatazone + 1
            byte_idx = bit // 8
            bit_idx = bit % 8
            if byte_idx >= len(self.zmap):
                break
            if not (self.zmap[byte_idx] & (1 << bit_idx)):
                self.zmap[byte_idx] |= 1 << bit_idx
                zones.append(zone)
                if len(zones) == count:
                    return zones
        raise SystemExit("No free zones")

    def _read_inode(self, ino: int) -> tuple:
        off = self.inode_table_offset + (ino - 1) * 32
        raw = self._read_at(off, 32)
        mode, uid, size, time, gid, nlinks = struct.unpack("<HHIIBB", raw[:14])
        zones = struct.unpack("<9H", raw[14:32])
        return mode, uid, size, time, gid, nlinks, zones

    def _write_inode(
        self,
        ino: int,
        mode: int,
        size: int,
        zones: list[int],
        nlinks: int = 1,
    ) -> None:
        direct = zones[:MINIX_DIRECT_ZONES]
        direct += [0] * (MINIX_DIRECT_ZONES - len(direct))
        indirect = zones[MINIX_DIRECT_ZONES] if len(zones) > MINIX_DIRECT_ZONES else 0
        packed = direct + [indirect, 0]
        off = self.inode_table_offset + (ino - 1) * 32
        self._write_at(
            off,
            struct.pack("<HHIIBB9H", mode, 0, size, 0, 0, nlinks, *packed),
        )

    def _write_file_data(self, zones: list[int], content: bytes) -> None:
        blocks_needed = max(1, math.ceil(len(content) / BLOCK_SIZE))
        data_zones = zones[:blocks_needed]
        indirect_needed = blocks_needed > MINIX_DIRECT_ZONES
        if indirect_needed:
            indirect_zone = zones[blocks_needed]
            indirect_entries = data_zones[MINIX_DIRECT_ZONES:]
            indirect = bytearray(BLOCK_SIZE)
            for i, zone in enumerate(indirect_entries):
                struct.pack_into("<H", indirect, i * 2, zone)
            self._write_at(indirect_zone * BLOCK_SIZE, indirect)
        for i, zone in enumerate(data_zones):
            chunk = content[i * BLOCK_SIZE : (i + 1) * BLOCK_SIZE]
            self._write_at(zone * BLOCK_SIZE, chunk.ljust(BLOCK_SIZE, b"\0"))

    def _read_dir(self, ino: int) -> bytearray:
        mode, _, size, _, _, _, zones = self._read_inode(ino)
        if (mode & 0xF000) != MINIX_IFDIR:
            raise SystemExit(f"inode {ino} is not a directory")
        zone0 = zones[0]
        return bytearray(self._read_at(zone0 * BLOCK_SIZE, max(size, BLOCK_SIZE)))

    def _write_dir(self, ino: int, data: bytearray, size: int) -> None:
        _, _, _, _, _, _, zones = self._read_inode(ino)
        self._write_at(zones[0] * BLOCK_SIZE, data[:BLOCK_SIZE])
        mode, uid, _, time, gid, nlinks, z = self._read_inode(ino)
        self._write_inode(ino, mode, size, list(z[:MINIX_DIRECT_ZONES]), nlinks)

    def _lookup(self, parent_ino: int, name: str) -> int | None:
        data = self._read_dir(parent_ino)
        mode, _, size, _, _, _, _ = self._read_inode(parent_ino)
        encoded = name.encode("ascii")
        if len(encoded) > 14:
            raise SystemExit(f"MINIX name too long: {name}")
        for i in range(0, size, DIR_ENTRY_SIZE):
            entry_ino, = struct.unpack("<H", data[i : i + 2])
            entry_name = data[i + 2 : i + DIR_ENTRY_SIZE].rstrip(b"\0")
            if entry_ino and entry_name == encoded:
                return entry_ino
        return None

    def _add_dir_entry(self, parent_ino: int, name: str, child_ino: int) -> None:
        data = self._read_dir(parent_ino)
        _, _, size, _, _, _, _ = self._read_inode(parent_ino)
        encoded = name.encode("ascii").ljust(14, b"\0")
        for i in range(0, size, DIR_ENTRY_SIZE):
            entry_ino, = struct.unpack("<H", data[i : i + 2])
            entry_name = data[i + 2 : i + DIR_ENTRY_SIZE].rstrip(b"\0")
            if entry_ino == child_ino and entry_name == name.encode("ascii"):
                return
            if entry_ino == 0 or entry_name == name.encode("ascii"):
                data[i : i + DIR_ENTRY_SIZE] = struct.pack("<H14s", child_ino, encoded)
                self._write_dir(parent_ino, data, size)
                return
        if size + DIR_ENTRY_SIZE > BLOCK_SIZE:
            raise SystemExit(f"Directory full: inode {parent_ino}")
        data[size : size + DIR_ENTRY_SIZE] = struct.pack("<H14s", child_ino, encoded)
        self._write_dir(parent_ino, data, size + DIR_ENTRY_SIZE)

    def _mkdir(self, parent_ino: int, name: str) -> int:
        existing = self._lookup(parent_ino, name)
        if existing is not None:
            mode, _, _, _, _, _, _ = self._read_inode(existing)
            if (mode & 0xF000) == MINIX_IFDIR:
                return existing
            raise SystemExit(f"Path component exists and is not a directory: {name}")
        ino = self._alloc_inode()
        zone = self._alloc_zones(1)[0]
        self._write_inode(ino, MINIX_IFDIR | 0o755, 2 * DIR_ENTRY_SIZE, [zone], 2)
        empty = bytearray(BLOCK_SIZE)
        empty[0:DIR_ENTRY_SIZE] = struct.pack("<H14s", ino, b".\0".ljust(14, b"\0"))
        empty[DIR_ENTRY_SIZE : 2 * DIR_ENTRY_SIZE] = struct.pack(
            "<H14s", parent_ino, b"..\0".ljust(14, b"\0")
        )
        self._write_at(zone * BLOCK_SIZE, empty)
        self._add_dir_entry(parent_ino, name, ino)
        return ino

    def ensure_path(self, path: str) -> int:
        if path in self.path_ino:
            return self.path_ino[path]
        parts = [p for p in path.strip("/").split("/") if p]
        ino = MINIX_ROOT_INO
        built = ""
        for part in parts:
            built = f"{built}/{part}" if built else f"/{part}"
            if built in self.path_ino:
                ino = self.path_ino[built]
                continue
            ino = self._mkdir(ino, part)
            self.path_ino[built] = ino
        return ino

    def install_file(self, relpath: str, content: bytes) -> None:
        parts = relpath.strip("/").split("/")
        if len(parts) < 2:
            raise SystemExit(f"Refusing flat install path: {relpath}")
        parent_path = "/" + "/".join(parts[:-1])
        name = parts[-1]
        if len(name.encode("ascii")) > 14:
            raise SystemExit(f"MINIX filename too long: {name}")
        parent_ino = self.ensure_path(parent_path)
        existing = self._lookup(parent_ino, name)
        if existing is not None:
            mode, _, _, _, _, _, zones = self._read_inode(existing)
            if (mode & 0xF000) != MINIX_IFREG:
                raise SystemExit(f"Cannot replace non-file: {relpath}")
            blocks_needed = max(1, math.ceil(len(content) / BLOCK_SIZE))
            total_zones = blocks_needed + (1 if blocks_needed > MINIX_DIRECT_ZONES else 0)
            if total_zones > len([z for z in zones if z]):
                raise SystemExit(f"Cannot grow existing file in place: {relpath}")
            self._write_file_data(list(zones), content)
            self._write_inode(existing, MINIX_IFREG | 0o644, len(content), list(zones))
            return
        ino = self._alloc_inode()
        blocks_needed = max(1, math.ceil(len(content) / BLOCK_SIZE))
        indirect_needed = blocks_needed > MINIX_DIRECT_ZONES
        total_zones = blocks_needed + (1 if indirect_needed else 0)
        zones = self._alloc_zones(total_zones)
        self._write_file_data(zones, content)
        self._write_inode(ino, MINIX_IFREG | 0o644, len(content), zones)
        self._add_dir_entry(parent_ino, name, ino)

    def finalize(self) -> None:
        self._write_at(self.imap_offset, self.imap)
        self._write_at(self.zmap_offset, self.zmap)
        self.f.close()


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: install-headers.py <minix.img> <staging-root>", file=sys.stderr)
        sys.exit(2)
    img = Path(sys.argv[1])
    staging = Path(sys.argv[2])
    if not img.is_file():
        print(f"install-headers.py: missing image {img}", file=sys.stderr)
        sys.exit(1)
    if not staging.is_dir():
        print(f"install-headers.py: missing staging dir {staging}", file=sys.stderr)
        sys.exit(1)

    image = MinixImage(img)
    count = 0
    skipped = 0
    for path in sorted(staging.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(staging).as_posix()
        name = path.name
        if len(name.encode("ascii")) > 14:
            print(f"install-headers: skip (name >14): {rel}", file=sys.stderr)
            skipped += 1
            continue
        image.install_file(rel, path.read_bytes())
        count += 1
    image.finalize()
    print(f"install-headers: installed {count} files from {staging} ({skipped} skipped)")


if __name__ == "__main__":
    main()
