/*
 * kernel/fs/fat32/super.c — FAT32 mount, BPB parsing, and selftest.
 *
 * Owns the VFS operations table and mount-time BIOS Parameter Block
 * (BPB) parsing for the read-only FAT32 backend. The volume is
 * validated as genuine FAT32 (>= 65525 data clusters) before any inode
 * is built.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

#include <jnu/fs/fat32.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <uapi/jnu/errno.h>

/*
 * Parse and validate the BPB at sector 0 into `priv`.
 *
 * Returns 0 on a valid FAT32 volume, negative errno otherwise. Rejects
 * FAT12/FAT16 (which fail the 65525-cluster floor) so a mismatched
 * fstype does not silently mis-mount.
 */
static int fat32_parse_bpb(struct block_device *bdev, struct fat32_priv *priv)
{
	struct fat32_buffer *buf;
	const uint8_t *b;
	uint32_t bytes_per_sec, sec_per_clus, rsvd, num_fats;
	uint32_t root_ent_cnt, tot_sec16, fat_sz16, tot_sec, fat_sz, root_clus;
	uint32_t data_sec, cluster_count;
	uint64_t fat_region, data_sec64, fat_bytes, fat_entry_bytes;
	uint64_t last_data_sec;

	buf = fat32_bget(bdev, 0);
	if (!buf)
		return -EIO;
	b = buf->data;

	if (b[510] != 0x55 || b[511] != 0xaa) {
		fat32_bput(buf);
		return -EINVAL;
	}

	bytes_per_sec = fat32_rd16(b + 11);
	sec_per_clus = b[13];
	rsvd = fat32_rd16(b + 14);
	num_fats = b[16];
	root_ent_cnt = fat32_rd16(b + 17);
	tot_sec16 = fat32_rd16(b + 19);
	fat_sz16 = fat32_rd16(b + 22);
	tot_sec = fat32_rd32(b + 32);
	fat_sz = fat32_rd32(b + 36);
	root_clus = fat32_rd32(b + 44);
	fat32_bput(buf);

	if (bytes_per_sec != FAT32_SECTOR_SIZE)
		return -EINVAL;
	if (sec_per_clus == 0 || (sec_per_clus & (sec_per_clus - 1)) != 0 ||
	    sec_per_clus > 128)
		return -EINVAL; /* must be a power of two, 1..128 */
	if (rsvd == 0 || num_fats == 0 || fat_sz == 0)
		return -EINVAL;
	if (root_ent_cnt != 0 || fat_sz16 != 0)
		return -EINVAL; /* these are zero only on FAT32 */
	if (root_clus < 2)
		return -EINVAL;
	if (tot_sec == 0)
		tot_sec = tot_sec16;
	if (tot_sec == 0)
		return -EINVAL;

	/*
	 * #1: compute the data-region start in 64-bit. num_fats (<=255) times
	 * an attacker-controlled 32-bit fat_sz overflows uint32, which would
	 * wrap data_sec small, pass the data_sec >= tot_sec floor, and inflate
	 * cluster_count — driving later cluster->LBA math off the device.
	 */
	fat_region = (uint64_t)num_fats * fat_sz;
	data_sec64 = (uint64_t)rsvd + fat_region;
	if (data_sec64 >= tot_sec)
		return -EINVAL;
	data_sec = (uint32_t)data_sec64; /* < tot_sec <= sector_count: fits */
	cluster_count = (tot_sec - data_sec) / sec_per_clus;
	if (cluster_count < FAT32_MIN_DATA_CLUSTERS) {
		pr_err("fat32: only %u data clusters (< %u); not FAT32\n",
		       cluster_count, FAT32_MIN_DATA_CLUSTERS);
		return -EINVAL;
	}
	if (root_clus >= cluster_count + 2)
		return -EINVAL;
	if ((uint64_t)tot_sec > bdev->sector_count)
		return -EINVAL;

	/*
	 * #2: the FAT must be large enough to hold an entry for every data
	 * cluster (clusters are numbered from 2, so cluster_count + 2 entries
	 * of 4 bytes each). Otherwise fat32_next_cluster() reads "FAT" sectors
	 * that actually lie in the data region, fabricating bogus chains.
	 */
	fat_bytes = (uint64_t)fat_sz * bytes_per_sec;
	fat_entry_bytes = ((uint64_t)cluster_count + 2u) * 4u;
	if (fat_bytes < fat_entry_bytes)
		return -EINVAL;

	/*
	 * #3: file identity (fat32_dirent_ino) is lba*16 + slot truncated to
	 * 32 bits. If the data region's last sector pushes that past 32 bits,
	 * two distinct directory entries could alias onto one ino, and the VFS
	 * inode cache would fold them into a single inode (wrong-file reads).
	 * Reject such volumes at mount rather than serve aliased data.
	 */
	last_data_sec =
	    (uint64_t)data_sec + (uint64_t)cluster_count * sec_per_clus;
	if (last_data_sec * (FAT32_SECTOR_SIZE / FAT32_DIRENT_SIZE) >
	    0xffffffffu)
		return -EINVAL;

	priv->bytes_per_sec = bytes_per_sec;
	priv->sec_per_clus = sec_per_clus;
	priv->rsvd_sec = rsvd;
	priv->num_fats = num_fats;
	priv->fat_sz = fat_sz;
	priv->root_clus = root_clus;
	priv->data_start_sec = data_sec;
	priv->total_sectors = tot_sec;
	priv->cluster_count = cluster_count;
	priv->cluster_bytes = sec_per_clus * bytes_per_sec;
	return 0;
}

/*
 * Mount a FAT32 filesystem from a block device.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int fat32_mount(struct vfs_mount *mnt, struct block_device *bdev)
{
	struct fat32_priv *priv;
	struct vfs_inode *root;
	int err;

	if (bdev->sector_size != FAT32_SECTOR_SIZE)
		return -EINVAL;

	priv = kzalloc(sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	err = fat32_parse_bpb(bdev, priv);
	if (err) {
		kfree(priv);
		return err;
	}

	mnt->priv = priv;
	err = fat32_inode_root(mnt, &root);
	if (err) {
		mnt->priv = NULL;
		kfree(priv);
		return err;
	}
	mnt->root = root;
	return 0;
}

const struct vfs_ops fat32_ops = {
    .mount = fat32_mount,
    .lookup = fat32_lookup,
    .readdir = fat32_readdir,
    .read = fat32_read,
    .write = NULL,
    .truncate = NULL,
    .create = NULL,
    .unlink = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
    .rename = NULL,
    .fsync = NULL,
    .close = fat32_close,
};

/*
 * Selftest.
 *
 * Two layers, per the test-strategy debate:
 *   1. Hermetic parser unit tests over fat.c / dir.c, which need no
 *      block device — this is the every-boot regression coverage.
 *   2. Opportunistic end-to-end read of /mnt/HELLO.TXT, which only runs
 *      when a real FAT32 image is attached (see `make run-fat`); it is
 *      skipped, not failed, when no FAT32 volume is mounted.
 */
int fat32_selftest(void)
{
	static const char want[] = "Hello from JNU FAT32!\n";
	struct vfs_inode *ino;
	char buf[64];
	ssize_t n;
	int err;

	/* 1a. Masked 28-bit FAT entry extraction. */
	{
		uint8_t fat_sec[16] = {0};

		/* Entry at offset 12 = 0xf0000005; reserved nibble masked. */
		fat_sec[12] = 0x05;
		fat_sec[15] = 0xf0;
		if (fat32_fat_entry(fat_sec, 12) != 0x00000005u) {
			pr_err("fat32_selftest: FAT entry decode wrong\n");
			return -EIO;
		}
	}

	/* 1b. 8.3 short-name decode (with and without an extension). */
	{
		const uint8_t f[FAT32_NAME_LEN] = {'H', 'E', 'L', 'L', 'O', ' ',
						   ' ', ' ', 'T', 'X', 'T'};
		const uint8_t d[FAT32_NAME_LEN] = {'D', 'I', 'R', ' ', ' ', ' ',
						   ' ', ' ', ' ', ' ', ' '};
		char out[16];

		fat32_decode_83(f, out);
		if (strcmp(out, "HELLO.TXT") != 0) {
			pr_err("fat32_selftest: 8.3 decode '%s'\n", out);
			return -EIO;
		}
		fat32_decode_83(d, out);
		if (strcmp(out, "DIR") != 0) {
			pr_err("fat32_selftest: 8.3 dir decode '%s'\n", out);
			return -EIO;
		}
	}

	/* 2. Opportunistic end-to-end against a real FAT32 disk on /mnt. */
	err = vfs_open("/mnt/HELLO.TXT", &ino);
	if (err) {
		pr_info("fat32_selftest: parser [ OK ] "
			"(no FAT32 disk on /mnt, e2e skipped)\n");
		return 0;
	}
	memset(buf, 0, sizeof(buf));
	n = vfs_read(ino, 0, sizeof(buf) - 1, buf);
	vfs_close(ino);
	if (n < (ssize_t)(sizeof(want) - 1) ||
	    memcmp(buf, want, sizeof(want) - 1) != 0) {
		pr_err("fat32_selftest: /mnt/HELLO.TXT mismatch (n=%d)\n",
		       (int)n);
		return -EIO;
	}

	pr_info("fat32_selftest: read /mnt/HELLO.TXT [ OK ]\n");
	return 0;
}
