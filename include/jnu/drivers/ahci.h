/*
 * include/jnu/drivers/ahci.h — AHCI (SATA) block device driver.
 *
 * Probes the first AHCI HBA on the PCI bus (class 0x01, subclass 0x06,
 * prog_if 0x01), brings up each implemented port that has a SATA disk
 * attached, and registers it as a struct block_device ("sda", "sdb",
 * …). Sector transfers use DMA via the command list / PRDT mechanism.
 *
 * Command completion uses a single MSI vector for the whole HBA; the
 * handler clears each flagged port's PxIS and wakes its waiter. If MSI
 * cannot be set up the driver falls back to polling PxCI, following the
 * virtio-blk path.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

/*
 * Probe the AHCI controller and register a block device for each
 * attached SATA disk. Call after pci_init() and block layer init.
 */
void ahci_init(void);

int ahci_selftest(void);
