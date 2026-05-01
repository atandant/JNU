/*
 * kernel/drivers/ata.c — ATA PIO block device driver.
 *
 * Probes the primary (0x1F0) and secondary (0x170) ATA channels for
 * attached drives via the IDENTIFY command. Each responsive drive is
 * registered as a struct block_device ("hda", "hdb", …). Sector
 * reads use 28-bit PIO (LBA28) — sufficient for v0.0.1 MINIX images
 * well under the 128 GiB limit.
 *
 * No DMA, no interrupts for data transfer — pure polled PIO. This is
 * intentionally simple; AHCI arrives in v0.0.2.
 *
 * Reference: ATA/ATAPI-6 §9 (PIO data-in), OSDev "ATA PIO Mode".
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/ata.h>
#include <jnu/block.h>
#include <jnu/errno.h>
#include <jnu/io.h>
#include <jnu/klog.h>
#include <jnu/string.h>
#include <jnu/types.h>

/* ATA register offsets from the base I/O port. */
#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_SECCOUNT 0x02
#define ATA_REG_LBA_LO 0x03
#define ATA_REG_LBA_MID 0x04
#define ATA_REG_LBA_HI 0x05
#define ATA_REG_DRIVE 0x06
#define ATA_REG_STATUS 0x07
#define ATA_REG_CMD 0x07

/* ATA control port offset from the control base. */
#define ATA_REG_ALTSTATUS 0x00
#define ATA_REG_DEVCTRL 0x00

/* Status bits. */
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

/* Commands. */
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_PIO 0x20

#define ATA_SECTOR_SIZE 512

/* Channel descriptor. */
struct ata_channel {
	uint16_t io_base;   /* e.g. 0x1F0 */
	uint16_t ctrl_base; /* e.g. 0x3F6 */
};

/* Drive descriptor. */
struct ata_drive {
	struct ata_channel *channel;
	uint8_t drive_sel; /* 0xA0 or 0xB0 */
	bool present;
	uint64_t sectors;
	char model[41];
	struct block_device bdev;
};

static struct ata_channel channels[2] = {
    {.io_base = 0x1F0, .ctrl_base = 0x3F6},
    {.io_base = 0x170, .ctrl_base = 0x376},
};

#define MAX_ATA_DRIVES 4
static struct ata_drive drives[MAX_ATA_DRIVES];
static size_t drive_count;

/* ---- low-level I/O --------------------------------------------------------
 */

static void ata_400ns_delay(struct ata_channel *ch)
{
	/* Reading the alternate status register 4 times gives ~400 ns. */
	(void)inb(ch->ctrl_base);
	(void)inb(ch->ctrl_base);
	(void)inb(ch->ctrl_base);
	(void)inb(ch->ctrl_base);
}

static int ata_wait_ready(struct ata_channel *ch)
{
	for (int i = 0; i < 100000; i++) {
		uint8_t s = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
		if (s & ATA_SR_ERR)
			return -EIO;
		if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY))
			return 0;
		__asm__ __volatile__("pause");
	}
	return -EIO;
}

static int ata_wait_drq(struct ata_channel *ch)
{
	for (int i = 0; i < 100000; i++) {
		uint8_t s = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
		if (s & ATA_SR_ERR)
			return -EIO;
		if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
			return 0;
		__asm__ __volatile__("pause");
	}
	return -EIO;
}

static void ata_select_drive(struct ata_channel *ch, uint8_t sel)
{
	outb((uint16_t)(ch->io_base + ATA_REG_DRIVE), sel);
	ata_400ns_delay(ch);
}

/* ---- IDENTIFY -------------------------------------------------------------
 */

static bool ata_identify(struct ata_channel *ch, uint8_t sel, uint16_t *id_buf)
{
	ata_select_drive(ch, sel);

	/* Zero out sector count and LBA registers before IDENTIFY. */
	outb((uint16_t)(ch->io_base + ATA_REG_SECCOUNT), 0);
	outb((uint16_t)(ch->io_base + ATA_REG_LBA_LO), 0);
	outb((uint16_t)(ch->io_base + ATA_REG_LBA_MID), 0);
	outb((uint16_t)(ch->io_base + ATA_REG_LBA_HI), 0);
	outb((uint16_t)(ch->io_base + ATA_REG_CMD), ATA_CMD_IDENTIFY);

	ata_400ns_delay(ch);

	uint8_t status = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
	if (status == 0)
		return false; /* no drive */

	/* Wait for BSY to clear. */
	for (int i = 0; i < 100000; i++) {
		status = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
		if (!(status & ATA_SR_BSY))
			break;
		__asm__ __volatile__("pause");
	}

	/* If LBA_MID or LBA_HI are non-zero, this is not an ATA device. */
	if (inb((uint16_t)(ch->io_base + ATA_REG_LBA_MID)) != 0 ||
	    inb((uint16_t)(ch->io_base + ATA_REG_LBA_HI)) != 0)
		return false;

	/* Wait for DRQ or ERR. */
	for (int i = 0; i < 100000; i++) {
		status = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
		if (status & ATA_SR_ERR)
			return false;
		if (status & ATA_SR_DRQ)
			break;
		__asm__ __volatile__("pause");
	}

	if (!(status & ATA_SR_DRQ))
		return false;

	/* Read the 256-word identification block. */
	insw((uint16_t)(ch->io_base + ATA_REG_DATA), id_buf, 256);
	return true;
}

static void extract_model(const uint16_t *id, char *out)
{
	/*
	 * Model string is at words 27–46 (40 bytes), big-endian byte
	 * pairs within each word. Swap and null-terminate.
	 */
	for (int i = 0; i < 20; i++) {
		uint16_t w = id[27 + i];
		out[i * 2] = (char)(w >> 8);
		out[i * 2 + 1] = (char)(w & 0xFF);
	}
	out[40] = '\0';
	for (int i = 39; i >= 0 && out[i] == ' '; i--)
		out[i] = '\0';
}

/* ---- block_device ops -----------------------------------------------------
 */

static int ata_bdev_read(struct block_device *bdev, uint64_t lba, size_t count,
			 void *buf)
{
	struct ata_drive *drv = bdev->priv;
	struct ata_channel *ch = drv->channel;
	uint8_t *p = buf;

	for (size_t i = 0; i < count; i++) {
		uint64_t sector = lba + i;
		if (sector >= drv->sectors)
			return -EINVAL;

		ata_select_drive(ch,
				 (uint8_t)(drv->drive_sel |
					   (uint8_t)((sector >> 24) & 0x0Fu)));

		outb((uint16_t)(ch->io_base + ATA_REG_SECCOUNT), 1);
		outb((uint16_t)(ch->io_base + ATA_REG_LBA_LO),
		     (uint8_t)(sector & 0xFFu));
		outb((uint16_t)(ch->io_base + ATA_REG_LBA_MID),
		     (uint8_t)((sector >> 8) & 0xFFu));
		outb((uint16_t)(ch->io_base + ATA_REG_LBA_HI),
		     (uint8_t)((sector >> 16) & 0xFFu));
		outb((uint16_t)(ch->io_base + ATA_REG_CMD), ATA_CMD_READ_PIO);

		int err = ata_wait_drq(ch);
		if (err)
			return err;

		insw((uint16_t)(ch->io_base + ATA_REG_DATA), p,
		     ATA_SECTOR_SIZE / 2);
		p += ATA_SECTOR_SIZE;
	}

	return 0;
}

static int ata_bdev_write(struct block_device *bdev, uint64_t lba, size_t count,
			  const void *buf)
{
	(void)bdev;
	(void)lba;
	(void)count;
	(void)buf;
	return -ENOSYS; /* read-only in v0.0.1 */
}

static const struct block_ops ata_blk_ops = {
    .read = ata_bdev_read,
    .write = ata_bdev_write,
};

/* Drive name table. */
static const char *drive_names[] = {"hda", "hdb", "hdc", "hdd"};

/* ---- public ---------------------------------------------------------------
 */

void ata_init(void)
{
	uint16_t id_buf[256];

	for (int ch_idx = 0; ch_idx < 2; ch_idx++) {
		struct ata_channel *ch = &channels[ch_idx];

		/*
		 * Detect floating bus: if the status register reads 0xFF
		 * the channel has no device attached at all.
		 */
		uint8_t s = inb((uint16_t)(ch->io_base + ATA_REG_STATUS));
		if (s == 0xFF) {
			pr_info("ata: channel %d not present\n", ch_idx);
			continue;
		}

		/* Software reset: set SRST bit, wait, clear. */
		outb(ch->ctrl_base, 0x04);
		ata_400ns_delay(ch);
		outb(ch->ctrl_base, 0x00);
		ata_400ns_delay(ch);
		(void)ata_wait_ready(ch);

		for (int drv_idx = 0; drv_idx < 2; drv_idx++) {
			uint8_t sel = (uint8_t)(0xA0 | (drv_idx << 4));

			memset(id_buf, 0, sizeof(id_buf));
			if (!ata_identify(ch, sel, id_buf))
				continue;

			if (drive_count >= MAX_ATA_DRIVES)
				break;

			struct ata_drive *d = &drives[drive_count];
			d->channel = ch;
			d->drive_sel = (uint8_t)(0xE0 | (drv_idx << 4));
			d->present = true;

			/* LBA28 sector count at word 60–61. */
			d->sectors =
			    (uint64_t)id_buf[60] | ((uint64_t)id_buf[61] << 16);

			extract_model(id_buf, d->model);

			d->bdev.name = drive_names[drive_count];
			d->bdev.sector_size = ATA_SECTOR_SIZE;
			d->bdev.sector_count = d->sectors;
			d->bdev.ops = &ata_blk_ops;
			d->bdev.priv = d;

			block_register(&d->bdev);

			pr_info("ata: %s: '%s' %llu sectors (%llu MiB)\n",
				d->bdev.name, d->model,
				(unsigned long long)d->sectors,
				(unsigned long long)(d->sectors / 2048));

			drive_count++;
		}
	}

	if (drive_count == 0)
		pr_info("ata: no drives found\n");
}

int ata_selftest(void)
{
	if (drive_count == 0) {
		pr_info("ata_selftest: skipped (no drives)\n");
		return 0;
	}

	/* Read sector 0 from the first drive and verify non-zero size. */
	struct ata_drive *d = &drives[0];
	uint8_t buf[ATA_SECTOR_SIZE];
	memset(buf, 0, sizeof(buf));

	int err = ata_bdev_read(&d->bdev, 0, 1, buf);
	if (err) {
		pr_err("ata_selftest: read sector 0 failed (err=%d)\n", err);
		return err;
	}

	pr_info("ata_selftest: sector 0 read OK\n");
	return 0;
}
