/*
 * kernel/fs/fat32/fat.c — Cluster-chain arithmetic and the FAT walk.
 *
 * The File Allocation Table is the heart of FAT32: every directory walk
 * and every file read follows a singly linked list of clusters whose
 * "next" pointers live in the FAT. This module owns cluster<->sector
 * translation and the FAT lookup; the rest of the driver never touches
 * the FAT region directly.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "internal.h"

/*
 * First LBA of a data cluster. Clusters are numbered from 2, so cluster
 * 2 maps to the first sector of the data region.
 */
uint64_t fat32_cluster_lba(const struct fat32_priv *priv, uint32_t cluster)
{
	return (uint64_t)priv->data_start_sec +
	       (uint64_t)(cluster - 2u) * priv->sec_per_clus;
}

/*
 * True if `cluster` names a real data cluster. Free (0), reserved (1),
 * bad (0x0ffffff7), and end-of-chain markers all fail this test, so a
 * chain walk naturally terminates when it leaves the valid range.
 */
bool fat32_cluster_valid(const struct fat32_priv *priv, uint32_t cluster)
{
	return cluster >= 2u && cluster < priv->cluster_count + 2u;
}

/*
 * Extract the masked 28-bit FAT entry at a byte offset within a FAT
 * sector. Pure helper so the parser selftest can exercise it without a
 * block device.
 */
uint32_t fat32_fat_entry(const uint8_t *fat_sector, uint32_t off_in_sector)
{
	return fat32_rd32(fat_sector + off_in_sector) & FAT32_ENTRY_MASK;
}

/*
 * Return the cluster that follows `cluster` in its chain, or an
 * end-of-chain marker (>= FAT32_EOC_MIN) on the last cluster or on I/O
 * failure. Callers treat a read error as end-of-chain, which truncates
 * rather than corrupts.
 */
uint32_t fat32_next_cluster(struct vfs_mount *mnt, uint32_t cluster)
{
	struct fat32_priv *priv = mnt->priv;
	uint64_t fat_byte = (uint64_t)cluster * 4u;
	uint64_t fat_sec = priv->rsvd_sec + fat_byte / priv->bytes_per_sec;
	uint32_t off = (uint32_t)(fat_byte % priv->bytes_per_sec);
	struct fat32_buffer *buf;
	uint32_t next;

	buf = fat32_bget(mnt->bdev, fat_sec);
	if (!buf)
		return FAT32_EOC_MIN;
	next = fat32_fat_entry(buf->data, off);
	fat32_bput(buf);
	return next;
}
