import sys
import struct

def main():
    if len(sys.argv) != 4:
        print("Usage: inject-file.py <minix.img> <filename> <content>")
        sys.exit(1)
        
    img_path = sys.argv[1]
    filename = sys.argv[2][:14]
    content = sys.argv[3].encode('utf-8')
    
    with open(img_path, 'r+b') as f:
        # Read superblock
        f.seek(1024)
        sb = f.read(20)
        ninodes, nzones, imap_blocks, zmap_blocks, firstdatazone, log_zone_size, max_size, magic, state = struct.unpack('<HHHHHHIHH', sb)
        
        if magic not in (0x137F, 0x138F):
            print("Not a valid MINIX v1 image")
            sys.exit(1)
            
        # Find a free inode
        imap_offset = 2048
        f.seek(imap_offset)
        imap = bytearray(f.read(imap_blocks * 1024))
        
        free_ino = 0
        for i in range(ninodes, 2, -1):
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
        
        # Find a free zone
        zmap_offset = imap_offset + imap_blocks * 1024
        f.seek(zmap_offset)
        zmap = bytearray(f.read(zmap_blocks * 1024))
        
        free_zone = 0
        for i in range(nzones - 1, firstdatazone, -1):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (zmap[byte_idx] & (1 << bit_idx)):
                free_zone = i
                zmap[byte_idx] |= (1 << bit_idx)
                break
                
        if not free_zone:
            print("No free zones")
            sys.exit(1)
            
        f.seek(zmap_offset)
        f.write(zmap)
        
        # Write inode
        inode_table_offset = zmap_offset + zmap_blocks * 1024
        ino_offset = inode_table_offset + (free_ino - 1) * 32
        
        # Mode: regular file | rw-r--r--
        mode = 0x8000 | 0o644
        uid = 0
        size = len(content)
        time = 0
        gid = 0
        nlinks = 1
        zones = [free_zone] + [0] * 8
        
        f.seek(ino_offset)
        f.write(struct.pack('<HHIIBB9H', mode, uid, size, time, gid, nlinks, *zones))
        
        # Write data
        f.seek(free_zone * 1024)
        f.write(content.ljust(1024, b'\0'))
        
        # Add to root directory
        root_ino_offset = inode_table_offset + 0 # Root inode is 1
        f.seek(root_ino_offset)
        r_mode, r_uid, r_size, r_time, r_gid, r_nlinks = struct.unpack('<HHIIBB', f.read(14))
        r_zones = struct.unpack('<9H', f.read(18))
        
        # We assume root dir is small and fits in the first zone
        r_zone0 = r_zones[0]
        f.seek(r_zone0 * 1024)
        dir_data = bytearray(f.read(1024))
        
        # Find empty slot
        added = False
        for i in range(0, r_size, 16):
            entry_ino, = struct.unpack('<H', dir_data[i:i+2])
            if entry_ino == 0:
                dir_data[i:i+16] = struct.pack('<H14s', free_ino, filename.encode('ascii').ljust(14, b'\0'))
                added = True
                break
                
        if not added:
            # Append if there is space in the block
            if r_size < 1024:
                dir_data[r_size:r_size+16] = struct.pack('<H14s', free_ino, filename.encode('ascii').ljust(14, b'\0'))
                added = True
                r_size += 16
                f.seek(root_ino_offset + 4) # Update size
                f.write(struct.pack('<I', r_size))
            else:
                print("Root dir full (1 zone max for this simple script)")
                sys.exit(1)
                
        f.seek(r_zone0 * 1024)
        f.write(dir_data)
        
        print(f"Successfully injected {filename} into MINIX image.")

if __name__ == '__main__':
    main()
