/*
 * include/jnu/drivers/msi.h — MSI / MSI-X programming.
 *
 * Helpers that point a device's message-signalled interrupt at an x86
 * vector previously obtained from the irq allocator. The caller owns
 * the vector (alloc, install handler) and the table-entry / queue
 * binding that is specific to its device; these helpers only program
 * the generic PCI MSI capability or the MSI-X table entry.
 *
 * The message address always targets the boot CPU's LAPIC in physical
 * destination mode, fixed delivery, edge triggered.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/drivers/pci.h>

/*
 * Program MSI-X table `entry` of `dev` to deliver `vector`, unmask the
 * entry, and enable MSI-X on the device (function mask cleared). The
 * MSI-X table BAR is mapped into the HHDM as needed.
 *
 * Returns 0 on success, -ENODEV if the device has no MSI-X capability
 * or its table BAR is unusable, or -EINVAL on bad arguments / an entry
 * index past the table size.
 */
int msix_enable(const struct pci_device *dev, unsigned entry, uint8_t vector);

/*
 * Program the device's MSI capability to deliver `vector` (single
 * message) and enable MSI. Handles both the 32-bit and 64-bit message
 * address capability layouts.
 *
 * Returns 0 on success, -ENODEV if the device has no MSI capability, or
 * -EINVAL on bad arguments.
 */
int msi_enable(const struct pci_device *dev, uint8_t vector);
