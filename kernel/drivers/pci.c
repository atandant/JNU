/*
 * kernel/drivers/pci.c — PCI bus enumeration via legacy Configuration
 *                        Mechanism #1 (I/O ports 0xCF8 / 0xCFC).
 *
 * Scans all 256 buses × 32 devices × 8 functions and records every
 * device whose vendor ID is not 0xFFFF. Each discovered device is
 * logged at INFO with bus:dev.func, vendor:device, and class codes.
 *
 * The internal table has a fixed capacity (MAX_PCI_DEVICES); overflow
 * is logged but does not panic.
 *
 * Reference: PCI Local Bus Specification §3.2.2.3.1 (configuration
 * mechanism #1), OSDev "PCI".
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/io.h>
#include <jnu/drivers/pci.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

#define PCI_CONFIG_ADDR 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

#define MAX_PCI_DEVICES 64

static struct pci_device devices[MAX_PCI_DEVICES];
static size_t device_count;

/*
 * Build the 32-bit CONFIG_ADDRESS value.
 * Bit 31: enable. Bits 23:16: bus. 15:11: dev. 10:8: func. 7:0: offset.
 */
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
	return (1u << 31) | ((uint32_t)bus << 16) |
	       ((uint32_t)(dev & 0x1Fu) << 11) |
	       ((uint32_t)(func & 0x07u) << 8) | ((uint32_t)(offset & 0xFCu));
}

uint32_t pci_read_config_dword(uint8_t bus, uint8_t dev, uint8_t func,
			       uint8_t offset)
{
	outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
	return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t dev, uint8_t func,
			      uint8_t offset)
{
	uint32_t dw =
	    pci_read_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu));
	return (uint16_t)(dw >> ((offset & 2u) * 8));
}

uint8_t pci_read_config_byte(uint8_t bus, uint8_t dev, uint8_t func,
			     uint8_t offset)
{
	uint32_t dw =
	    pci_read_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu));
	return (uint8_t)(dw >> ((offset & 3u) * 8));
}

void pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t func,
			    uint8_t offset, uint32_t val)
{
	outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
	outl(PCI_CONFIG_DATA, val);
}

void pci_write_config_word(uint8_t bus, uint8_t dev, uint8_t func,
			   uint8_t offset, uint16_t val)
{
	uint32_t dw =
	    pci_read_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu));
	unsigned shift = (offset & 2u) * 8;
	dw &= ~(0xFFFFu << shift);
	dw |= ((uint32_t)val << shift);
	pci_write_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu), dw);
}

void pci_write_config_byte(uint8_t bus, uint8_t dev, uint8_t func,
			   uint8_t offset, uint8_t val)
{
	uint32_t dw =
	    pci_read_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu));
	unsigned shift = (offset & 3u) * 8;
	dw &= ~(0xFFu << shift);
	dw |= ((uint32_t)val << shift);
	pci_write_config_dword(bus, dev, func, (uint8_t)(offset & 0xFCu), dw);
}

static void probe_function(uint8_t bus, uint8_t dev, uint8_t func)
{
	uint16_t vendor = pci_read_config_word(bus, dev, func, 0x00);
	if (vendor == 0xFFFFu)
		return;

	uint16_t device_id = pci_read_config_word(bus, dev, func, 0x02);
	uint8_t class = pci_read_config_byte(bus, dev, func, 0x0B);
	uint8_t subcls = pci_read_config_byte(bus, dev, func, 0x0A);
	uint8_t progif = pci_read_config_byte(bus, dev, func, 0x09);
	uint8_t hdrtype = pci_read_config_byte(bus, dev, func, 0x0E);
	uint8_t irqline = pci_read_config_byte(bus, dev, func, 0x3C);

	if (device_count < MAX_PCI_DEVICES) {
		struct pci_device *p = &devices[device_count++];
		p->bus = bus;
		p->dev = dev;
		p->func = func;
		p->vendor_id = vendor;
		p->device_id = device_id;
		p->class_code = class;
		p->subclass = subcls;
		p->prog_if = progif;
		p->header_type = (uint8_t)(hdrtype & 0x7Fu);
		p->irq_line = irqline;
	}

	pr_info("pci: %02x:%02x.%x %04x:%04x class %02x.%02x.%02x\n",
		(unsigned)bus, (unsigned)dev, (unsigned)func, (unsigned)vendor,
		(unsigned)device_id, (unsigned)class, (unsigned)subcls,
		(unsigned)progif);
}

void pci_init(void)
{
	device_count = 0;

	for (unsigned bus = 0; bus < 256; bus++) {
		for (unsigned dev = 0; dev < 32; dev++) {
			uint16_t vendor = pci_read_config_word(
			    (uint8_t)bus, (uint8_t)dev, 0, 0x00);
			if (vendor == 0xFFFFu)
				continue;

			probe_function((uint8_t)bus, (uint8_t)dev, 0);

			/* Multi-function? Check bit 7 of header type. */
			uint8_t ht = pci_read_config_byte(
			    (uint8_t)bus, (uint8_t)dev, 0, 0x0E);
			if (ht & 0x80u) {
				for (unsigned func = 1; func < 8; func++) {
					probe_function((uint8_t)bus,
						       (uint8_t)dev,
						       (uint8_t)func);
				}
			}
		}
	}

	pr_info("pci: %u device%s found\n", (unsigned)device_count,
		device_count == 1 ? "" : "s");
}

void pci_for_each(pci_callback_t cb, void *ctx)
{
	for (size_t i = 0; i < device_count; i++)
		cb(&devices[i], ctx);
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
					uint8_t prog_if)
{
	for (size_t i = 0; i < device_count; i++) {
		if (devices[i].class_code == class_code &&
		    devices[i].subclass == subclass &&
		    devices[i].prog_if == prog_if)
			return &devices[i];
	}
	return NULL;
}

const struct pci_device *pci_find_vendor(uint16_t vendor_id, uint16_t device_id)
{
	for (size_t i = 0; i < device_count; i++) {
		if (devices[i].vendor_id == vendor_id &&
		    devices[i].device_id == device_id)
			return &devices[i];
	}
	return NULL;
}

int pci_read_bar(const struct pci_device *dev, unsigned bar_idx,
		 struct pci_bar_info *out)
{
	uint8_t bus;
	uint8_t pci_dev;
	uint8_t func;
	uint8_t off;
	uint32_t orig_lo;
	uint32_t orig_hi = 0;
	uint32_t probe_lo;
	uint32_t probe_hi = 0;
	uint64_t mask;

	if (!dev || !out || bar_idx > 5)
		return -EINVAL;

	bus = dev->bus;
	pci_dev = dev->dev;
	func = dev->func;
	off = (uint8_t)(0x10u + bar_idx * 4u);

	orig_lo = pci_read_config_dword(bus, pci_dev, func, off);
	if (orig_lo == 0)
		return -EINVAL;

	if (orig_lo & 1u) {
		/* I/O BAR */
		out->base = (uint64_t)(orig_lo & 0xFFFFFFFCu);
		pci_write_config_dword(bus, pci_dev, func, off, 0xFFFFFFFCu);
		probe_lo = pci_read_config_dword(bus, pci_dev, func, off);
		pci_write_config_dword(bus, pci_dev, func, off, orig_lo);
		mask = (uint64_t)(probe_lo & 0xFFFFFFFCu);
		out->is_mmio = false;
	} else if ((orig_lo & 0x6u) == 0x4u) {
		/* 64-bit memory BAR uses BAR n and BAR n+1. */
		if (bar_idx > 4)
			return -EINVAL;
		orig_hi = pci_read_config_dword(bus, pci_dev, func,
						(uint8_t)(off + 4u));
		out->base = ((uint64_t)(orig_lo & 0xFFFFFFF0u)) |
			    ((uint64_t)orig_hi << 32);
		pci_write_config_dword(bus, pci_dev, func, off, 0xFFFFFFF0u);
		pci_write_config_dword(bus, pci_dev, func, (uint8_t)(off + 4u),
				       0xFFFFFFFFu);
		probe_lo = pci_read_config_dword(bus, pci_dev, func, off);
		probe_hi = pci_read_config_dword(bus, pci_dev, func,
						 (uint8_t)(off + 4u));
		pci_write_config_dword(bus, pci_dev, func, off, orig_lo);
		pci_write_config_dword(bus, pci_dev, func, (uint8_t)(off + 4u),
				       orig_hi);
		mask = ((uint64_t)(probe_hi & 0xFFFFFFFFu) << 32) |
		       (uint64_t)(probe_lo & 0xFFFFFFF0u);
		out->is_mmio = true;
	} else {
		/* 32-bit memory BAR */
		out->base = (uint64_t)(orig_lo & 0xFFFFFFF0u);
		pci_write_config_dword(bus, pci_dev, func, off, 0xFFFFFFF0u);
		probe_lo = pci_read_config_dword(bus, pci_dev, func, off);
		pci_write_config_dword(bus, pci_dev, func, off, orig_lo);
		mask = (uint64_t)(probe_lo & 0xFFFFFFF0u);
		out->is_mmio = true;
	}

	if (mask == 0)
		return -EINVAL;

	out->size = (uint64_t)(~mask + 1u);
	if (out->size == 0)
		return -EINVAL;

	return 0;
}

void pci_enable_device(const struct pci_device *dev)
{
	uint16_t cmd;

	if (!dev)
		return;

	cmd = pci_read_config_word(dev->bus, dev->dev, dev->func, 0x04);
	cmd |= 0x0007u; /* I/O + memory + bus master */
	pci_write_config_word(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

int pci_selftest(void)
{
	/*
	 * On QEMU q35, the host bridge (bus 0, dev 0, func 0) always
	 * exists. Assert at least one device was enumerated.
	 */
	if (device_count == 0)
		return -ENODEV;
	return 0;
}
