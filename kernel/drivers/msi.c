/*
 * kernel/drivers/msi.c — MSI / MSI-X programming.
 *
 * Points a device's message-signalled interrupt at an x86 vector. The
 * message address is built for the boot CPU's LAPIC in physical
 * destination mode (0xFEE00000 | (apic_id << 12)); the message data is
 * just the vector (fixed delivery, edge triggered). The vector itself
 * must already have been allocated and have a handler installed via the
 * irq allocator.
 *
 * Reference: PCI Local Bus Spec §6.8 (MSI), §6.8.2 (MSI-X);
 * Intel SDM Vol. 3 §10.11 (message signalled interrupts).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/apic.h>
#include <jnu/drivers/msi.h>
#include <jnu/drivers/pci.h>
#include <jnu/lib/klog.h>
#include <jnu/mm/paging.h>
#include <uapi/jnu/errno.h>

/* MSI capability register offsets (from the capability pointer). */
#define MSI_CAP_CONTROL 0x02 /* 16-bit message control */
#define MSI_CAP_ADDR_LO 0x04 /* 32-bit message address (low) */
/* 64-bit layout: addr-high at 0x08, data at 0x0C. */
/* 32-bit layout: data at 0x08. */
#define MSI_CTRL_ENABLE (1u << 0)
#define MSI_CTRL_64BIT (1u << 7)

/* MSI-X capability register offsets. */
#define MSIX_CAP_CONTROL 0x02 /* 16-bit message control */
#define MSIX_CAP_TABLE 0x04   /* 32-bit table offset / BIR */
#define MSIX_CTRL_TABLE_SIZE 0x07FFu
#define MSIX_CTRL_FUNC_MASK (1u << 14)
#define MSIX_CTRL_ENABLE (1u << 15)
#define MSIX_TABLE_BIR_MASK 0x07u
#define MSIX_TABLE_OFFSET_MASK 0xFFFFFFF8u

/* MSI-X table entry (16 bytes). */
#define MSIX_ENTRY_SIZE 16u
#define MSIX_ENTRY_ADDR_LO 0u
#define MSIX_ENTRY_ADDR_HI 4u
#define MSIX_ENTRY_DATA 8u
#define MSIX_ENTRY_VECTOR_CTRL 12u
#define MSIX_VECTOR_CTRL_MASK (1u << 0)

/* Build the LAPIC-targeted message address for the boot CPU. */
static uint32_t msi_message_addr(void)
{
	return 0xFEE00000u | ((uint32_t)apic_bsp_id() << 12);
}

/* Message data: fixed delivery, edge triggered, physical destination. */
static uint32_t msi_message_data(uint8_t vector) { return (uint32_t)vector; }

int msix_enable(const struct pci_device *dev, unsigned entry, uint8_t vector)
{
	uint8_t cap;
	uint16_t ctrl;
	uint32_t table;
	uint8_t bir;
	uint32_t offset;
	unsigned table_size;
	struct pci_bar_info bar;
	uint64_t table_phys;
	uint64_t entry_phys;
	volatile uint32_t *e;

	if (!dev)
		return -EINVAL;

	cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
	if (!cap)
		return -ENODEV;

	ctrl = pci_read_config_word(dev->bus, dev->dev, dev->func,
				    (uint8_t)(cap + MSIX_CAP_CONTROL));
	table_size = (ctrl & MSIX_CTRL_TABLE_SIZE) + 1u;
	if (entry >= table_size)
		return -EINVAL;

	table = pci_read_config_dword(dev->bus, dev->dev, dev->func,
				      (uint8_t)(cap + MSIX_CAP_TABLE));
	bir = (uint8_t)(table & MSIX_TABLE_BIR_MASK);
	offset = table & MSIX_TABLE_OFFSET_MASK;

	if (pci_read_bar(dev, bir, &bar) != 0 || !bar.is_mmio)
		return -ENODEV;

	table_phys = bar.base + offset;
	entry_phys = table_phys + (uint64_t)entry * MSIX_ENTRY_SIZE;

	/* The entry must lie inside the BAR window. */
	if ((uint64_t)offset + (uint64_t)table_size * MSIX_ENTRY_SIZE >
	    bar.size)
		return -ENODEV;

	if (paging_ensure_hhdm((paddr_t)entry_phys, MSIX_ENTRY_SIZE) != 0)
		return -ENOMEM;

	e = (volatile uint32_t *)phys_to_virt((paddr_t)entry_phys);

	/* Mask the entry while we reprogram it, then unmask. */
	e[MSIX_ENTRY_VECTOR_CTRL / 4] = MSIX_VECTOR_CTRL_MASK;
	__asm__ __volatile__("" ::: "memory");
	e[MSIX_ENTRY_ADDR_LO / 4] = msi_message_addr();
	e[MSIX_ENTRY_ADDR_HI / 4] = 0;
	e[MSIX_ENTRY_DATA / 4] = msi_message_data(vector);
	__asm__ __volatile__("" ::: "memory");
	e[MSIX_ENTRY_VECTOR_CTRL / 4] = 0; /* unmask */

	/* Enable MSI-X, clear the global function mask. */
	ctrl |= MSIX_CTRL_ENABLE;
	ctrl &= (uint16_t)~MSIX_CTRL_FUNC_MASK;
	pci_write_config_word(dev->bus, dev->dev, dev->func,
			      (uint8_t)(cap + MSIX_CAP_CONTROL), ctrl);

	pr_info("msi: %02x:%02x.%x MSI-X entry %u -> vector %u\n",
		(unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->func,
		entry, (unsigned)vector);
	return 0;
}

int msi_enable(const struct pci_device *dev, uint8_t vector)
{
	uint8_t cap;
	uint16_t ctrl;
	bool is64;

	if (!dev)
		return -EINVAL;

	cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
	if (!cap)
		return -ENODEV;

	ctrl = pci_read_config_word(dev->bus, dev->dev, dev->func,
				    (uint8_t)(cap + MSI_CAP_CONTROL));
	is64 = (ctrl & MSI_CTRL_64BIT) != 0;

	/* Message address. */
	pci_write_config_dword(dev->bus, dev->dev, dev->func,
			       (uint8_t)(cap + MSI_CAP_ADDR_LO),
			       msi_message_addr());

	/* Message data sits at 0x0C (64-bit) or 0x08 (32-bit). */
	if (is64) {
		pci_write_config_dword(dev->bus, dev->dev, dev->func,
				       (uint8_t)(cap + 0x08), 0);
		pci_write_config_word(dev->bus, dev->dev, dev->func,
				      (uint8_t)(cap + 0x0C),
				      (uint16_t)msi_message_data(vector));
	} else {
		pci_write_config_word(dev->bus, dev->dev, dev->func,
				      (uint8_t)(cap + 0x08),
				      (uint16_t)msi_message_data(vector));
	}

	/*
	 * Enable MSI and force the device to a single message (Multiple
	 * Message Enable = 0, bits 6:4). Clearing those bits guarantees the
	 * device asserts exactly the one vector we programmed.
	 */
	ctrl &= (uint16_t)~0x0070u;
	ctrl |= MSI_CTRL_ENABLE;
	pci_write_config_word(dev->bus, dev->dev, dev->func,
			      (uint8_t)(cap + MSI_CAP_CONTROL), ctrl);

	pr_info("msi: %02x:%02x.%x MSI -> vector %u\n", (unsigned)dev->bus,
		(unsigned)dev->dev, (unsigned)dev->func, (unsigned)vector);
	return 0;
}
