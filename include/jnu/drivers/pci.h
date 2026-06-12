/*
 * include/jnu/drivers/pci.h — PCI bus enumeration via legacy I/O (0xCF8/0xCFC).
 *
 * Enumerates all PCI devices on buses 0–255 using Configuration
 * Mechanism #1. Each discovered device is logged at INFO and can
 * be queried by class/subclass/prog_if for driver matching.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>

struct pci_device {
	uint8_t bus;
	uint8_t dev;
	uint8_t func;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t header_type;
	uint8_t irq_line;
};

typedef void (*pci_callback_t)(const struct pci_device *dev, void *ctx);

/*
 * Scan the PCI bus and log every device found. Must be called after
 * klog is up.
 */
void pci_init(void);

/*
 * Walk every known PCI device, calling `cb` for each.
 */
void pci_for_each(pci_callback_t cb, void *ctx);

/*
 * Find the first device matching `class`, `subclass`, and `prog_if`.
 * Returns a pointer to the internal device table entry, or NULL.
 */
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
					uint8_t prog_if);

/*
 * Find the first device matching vendor and device ID.
 * Returns a pointer to the internal device table entry, or NULL.
 */
const struct pci_device *pci_find_vendor(uint16_t vendor_id,
					 uint16_t device_id);

struct pci_bar_info {
	bool is_mmio;
	uint64_t base;
	uint64_t size;
};

/*
 * Decode a PCI BAR (0–5). Writes the decoded base address and region
 * size into *out. Returns 0 on success or -EINVAL if the BAR is unused.
 */
int pci_read_bar(const struct pci_device *dev, unsigned bar_idx,
		 struct pci_bar_info *out);

/*
 * Enable I/O space, memory space, and bus mastering on a PCI device.
 */
void pci_enable_device(const struct pci_device *dev);

/* Standard PCI capability IDs (config space, capability list). */
#define PCI_CAP_ID_PM 0x01     /* Power Management */
#define PCI_CAP_ID_MSI 0x05    /* Message Signalled Interrupts */
#define PCI_CAP_ID_VENDOR 0x09 /* Vendor specific (e.g. virtio) */
#define PCI_CAP_ID_PCIE 0x10   /* PCI Express */
#define PCI_CAP_ID_MSIX 0x11   /* MSI-X */

/*
 * Walk the device's capability list and return the config-space offset
 * of the first capability whose ID equals `cap_id`, or 0 if absent.
 * Returns 0 when the device reports no capability list.
 */
uint8_t pci_find_capability(const struct pci_device *dev, uint8_t cap_id);

/* Configuration space accessors. */
uint8_t pci_read_config_byte(uint8_t bus, uint8_t dev, uint8_t func,
			     uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t dev, uint8_t func,
			      uint8_t offset);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t dev, uint8_t func,
			       uint8_t offset);
void pci_write_config_byte(uint8_t bus, uint8_t dev, uint8_t func,
			   uint8_t offset, uint8_t val);
void pci_write_config_word(uint8_t bus, uint8_t dev, uint8_t func,
			   uint8_t offset, uint16_t val);
void pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t func,
			    uint8_t offset, uint32_t val);

int pci_selftest(void);
