/*
 * include/jnu/pci.h — PCI bus enumeration via legacy I/O (0xCF8/0xCFC).
 *
 * Enumerates all PCI devices on buses 0–255 using Configuration
 * Mechanism #1. Each discovered device is logged at INFO and can
 * be queried by class/subclass/prog_if for driver matching.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

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
