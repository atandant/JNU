/*
 * include/jnu/drivers/ata.h — ATA PIO driver.
 *
 * Detects ATA drives on the primary (0x1F0) and secondary (0x170)
 * channels via IDENTIFY, then registers each as a struct block_device.
 * Uses PIO mode (no DMA) — adequate for a v0.0.1 MINIX FS reader.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

/*
 * Probe primary and secondary ATA channels. For each drive that
 * responds to IDENTIFY, register a block device ("hda", "hdb", etc.).
 * Call after pci_init() and block layer init.
 */
void ata_init(void);

int ata_selftest(void);
