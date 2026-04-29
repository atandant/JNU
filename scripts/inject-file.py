import sys
import struct
import math

BLOCK_SIZE = 1024
MINIX_IFREG = 0x8000
MINIX_ROOT_INO = 1
MINIX_DIRECT_ZONES = 7
MINIX_INDIRECT_PER_BLOCK = BLOCK_SIZE // 2
DIR_ENTRY_SIZE = 16

def main():
    if len(sys.argv) != 4:
        print("Usage: inject-file.py <minix.img> <filename> <content|@path>")
        sys.exit(1)
        
    img_path = sys.argv[1]
    filename = sys.argv[2][:14]
    if sys.argv[3].startswith('@'):
        with open(sys.argv[3][1:], 'rb') as src:
            content = src.read()
    else:
        content = sys.argv[3].encode('utf-8')
    
    with open(img_path, 'r+b') as f:
        # Read superblock
        f.seek(1024)
        sb = f.read(20)
        ninodes, nzones, imap_blocks, zmap_blocks, firstdatazone, log_zone_size, max_size, magic, state = struct.unpack('<HHHHHHIHH', sb)
        
        if magic not in (0x137F, 0x138F):
            print("Not a valid MINIX v1 image")
            sys.exit(1)
            
        if log_zone_size != 0:
            print("Only 1 KiB MINIX zones are supported")
            sys.exit(1)

        blocks_needed = max(1, math.ceil(len(content) / BLOCK_SIZE))
        if blocks_needed > MINIX_DIRECT_ZONES + MINIX_INDIRECT_PER_BLOCK:
            print(f"{filename}: too large for simple injector ({len(content)} bytes)")
            sys.exit(1)
        indirect_needed = blocks_needed > MINIX_DIRECT_ZONES
        total_zones_needed = blocks_needed + (1 if indirect_needed else 0)

        # Find a free inode. MINIX inode bitmap bit N represents inode N.
        imap_offset = 2 * BLOCK_SIZE
        f.seek(imap_offset)
        imap = bytearray(f.read(imap_blocks * BLOCK_SIZE))
        
        free_ino = 0
        for i in range(2, ninodes + 1):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (imap[byte_idx] & (1 << bit_idx)):
                free_ino = i
                imap[byte_idx] |= (1 << bit_idx)
                break
                
        if not free_ino:
            print("No free inodes")
            sys.exit(1)
            
        f.seek(imap_offset)
        f.write(imap)
        
        # Find free zones. MINIX zone bitmap bit N represents
        # zone firstdatazone - 1 + N.
        zmap_offset = imap_offset + imap_blocks * BLOCK_SIZE
        f.seek(zmap_offset)
        zmap = bytearray(f.read(zmap_blocks * BLOCK_SIZE))
        
        free_zones = []
        for zone in range(firstdatazone, nzones):
            bit = zone - firstdatazone + 1
            byte_idx = bit // 8
            bit_idx = bit % 8
            if byte_idx >= len(zmap):
                break
            if not (zmap[byte_idx] & (1 << bit_idx)):
                free_zones.append(zone)
                zmap[byte_idx] |= (1 << bit_idx)
                if len(free_zones) == total_zones_needed:
                    break
                
        if len(free_zones) != total_zones_needed:
            print("No free zones")
            sys.exit(1)
            
        f.seek(zmap_offset)
        f.write(zmap)
        
        # Write inode
        inode_table_offset = zmap_offset + zmap_blocks * BLOCK_SIZE
        ino_offset = inode_table_offset + (free_ino - 1) * 32
        
        # Mode: regular file | rw-r--r--
        mode = MINIX_IFREG | 0o644
        uid = 0
        size = len(content)
        time = 0
        gid = 0
        nlinks = 1
        data_zones = free_zones[:blocks_needed]
        indirect_zone = free_zones[blocks_needed] if indirect_needed else 0
        direct = data_zones[:MINIX_DIRECT_ZONES]
        zones = direct + [0] * (MINIX_DIRECT_ZONES - len(direct))
        zones.append(indirect_zone)
        zones.append(0)
        
        f.seek(ino_offset)
        f.write(struct.pack('<HHIIBB9H', mode, uid, size, time, gid, nlinks, *zones))
        
        # Write data
        for i, zone in enumerate(data_zones):
            chunk = content[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
            f.seek(zone * BLOCK_SIZE)
            f.write(chunk.ljust(BLOCK_SIZE, b'\0'))

        if indirect_needed:
            indirect_entries = data_zones[MINIX_DIRECT_ZONES:]
            indirect = bytearray(BLOCK_SIZE)
            for i, zone in enumerate(indirect_entries):
                struct.pack_into('<H', indirect, i * 2, zone)
            f.seek(indirect_zone * BLOCK_SIZE)
            f.write(indirect)
        
        # Add to root directory
        root_ino_offset = inode_table_offset + (MINIX_ROOT_INO - 1) * 32
        f.seek(root_ino_offset)
        r_mode, r_uid, r_size, r_time, r_gid, r_nlinks = struct.unpack('<HHIIBB', f.read(14))
        r_zones = struct.unpack('<9H', f.read(18))
        
        # We assume root dir is small and fits in the first zone
        r_zone0 = r_zones[0]
        f.seek(r_zone0 * BLOCK_SIZE)
        dir_data = bytearray(f.read(BLOCK_SIZE))
        
        # Find empty slot
        added = False
        encoded_name = filename.encode('ascii').ljust(14, b'\0')
        for i in range(0, r_size, DIR_ENTRY_SIZE):
            entry_ino, = struct.unpack('<H', dir_data[i:i+2])
            entry_name = dir_data[i + 2:i + DIR_ENTRY_SIZE].rstrip(b'\0')
            if entry_ino != 0 and entry_name == filename.encode('ascii'):
                dir_data[i:i+DIR_ENTRY_SIZE] = struct.pack('<H14s', free_ino, encoded_name)
                added = True
                break
            if entry_ino == 0:
                dir_data[i:i+DIR_ENTRY_SIZE] = struct.pack('<H14s', free_ino, encoded_name)
                added = True
                break
                
        if not added:
            # Append if there is space in the block
            if r_size + DIR_ENTRY_SIZE <= BLOCK_SIZE:
                dir_data[r_size:r_size+DIR_ENTRY_SIZE] = struct.pack('<H14s', free_ino, encoded_name)
                added = True
                r_size += DIR_ENTRY_SIZE
                f.seek(root_ino_offset + 4) # Update size
                f.write(struct.pack('<I', r_size))
            else:
                print("Root dir full (1 zone max for this simple script)")
                sys.exit(1)
                
        f.seek(r_zone0 * BLOCK_SIZE)
        f.write(dir_data)
        
        print(f"Successfully injected {filename} into MINIX image.")

if __name__ == '__main__':
    main()
