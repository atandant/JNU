/*
 * kernel/arch/x86_64/apic.c — LAPIC + IOAPIC + minimal ACPI/MADT parser.
 *
 * Phase-2 deliverable. Walks the ACPI RSDP/XSDT/RSDT to find the MADT
 * (signature "APIC"), pulls out the LAPIC list, IOAPIC base(s), and ISA
 * IRQ overrides. Programs the LAPIC spurious-vector register (vector
 * 0xFF, enable bit set) and masks every IOAPIC redirection entry.
 *
 * Reference: ACPI specification §5.2.12 (MADT), Intel SDM Vol. 3 §10
 * (LAPIC), 82093AA datasheet (IOAPIC).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/acpi.h>
#include <jnu/apic.h>
#include <jnu/compiler.h>
#include <jnu/cpu.h>
#include <jnu/klog.h>
#include <jnu/paging.h>
#include <jnu/string.h>
#include <jnu/types.h>

/* ------------------------------------------------------------------------- */
/* HHDM helpers                                                               */
/* ------------------------------------------------------------------------- */

static uint64_t apic_hhdm;

static void *hhdm(uint64_t pa) { return (void *)(uintptr_t)(pa + apic_hhdm); }

/* ------------------------------------------------------------------------- */
/* ACPI structures                                                            */
/* ------------------------------------------------------------------------- */

struct __packed acpi_madt {
	struct acpi_sdt_header hdr;
	uint32_t lapic_address;
	uint32_t flags;
	/* variable-length list of entries follows */
};

#define MADT_TYPE_LAPIC 0
#define MADT_TYPE_IOAPIC 1
#define MADT_TYPE_INT_OVERRIDE 2
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

struct __packed madt_entry_hdr {
	uint8_t type;
	uint8_t length;
};

struct __packed madt_lapic {
	struct madt_entry_hdr h;
	uint8_t processor_id;
	uint8_t apic_id;
	uint32_t flags;
};

struct __packed madt_ioapic {
	struct madt_entry_hdr h;
	uint8_t id;
	uint8_t reserved;
	uint32_t address;
	uint32_t gsi_base;
};

struct __packed madt_int_override {
	struct madt_entry_hdr h;
	uint8_t bus;
	uint8_t source;
	uint32_t gsi;
	uint16_t flags;
};

struct __packed madt_lapic_addr_override {
	struct madt_entry_hdr h;
	uint16_t reserved;
	uint64_t address;
};

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

#define MAX_LAPICS 32
#define MAX_IOAPICS 8
#define MAX_OVERRIDES 16

struct ioapic {
	uint8_t id;
	uint32_t gsi_base;
	uint32_t gsi_count;
	uint64_t mmio_phys;
	volatile uint32_t *mmio;
};

struct override {
	uint8_t isa_irq;
	uint32_t gsi;
	uint16_t flags;
	bool used;
};

static uint64_t lapic_phys;
static volatile uint32_t *lapic_mmio;

static struct ioapic ioapics[MAX_IOAPICS];
static size_t ioapic_count;

static struct override overrides[MAX_OVERRIDES];
static size_t override_count;

static uint8_t lapic_ids[MAX_LAPICS];
static size_t lapic_count;

/* ------------------------------------------------------------------------- */
/* RSDP / SDT walking                                                         */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* MADT parse                                                                 */
/* ------------------------------------------------------------------------- */

static void parse_madt(const struct acpi_madt *madt)
{
	lapic_phys = madt->lapic_address;

	const uint8_t *p = (const uint8_t *)(madt + 1);
	const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;

	while (p < end) {
		const struct madt_entry_hdr *h =
		    (const struct madt_entry_hdr *)p;
		if (h->length == 0) {
			break;
		}

		switch (h->type) {
		case MADT_TYPE_LAPIC: {
			const struct madt_lapic *e = (const void *)p;
			if ((e->flags & 1u) && lapic_count < MAX_LAPICS) {
				lapic_ids[lapic_count++] = e->apic_id;
			}
			break;
		}
		case MADT_TYPE_IOAPIC: {
			const struct madt_ioapic *e = (const void *)p;
			if (ioapic_count < MAX_IOAPICS) {
				struct ioapic *io = &ioapics[ioapic_count++];
				io->id = e->id;
				io->gsi_base = e->gsi_base;
				io->mmio_phys = e->address;
				paging_ensure_hhdm(e->address, PAGE_SIZE);
				io->mmio = hhdm(e->address);
				io->gsi_count = 0; /* filled later */
			}
			break;
		}
		case MADT_TYPE_INT_OVERRIDE: {
			const struct madt_int_override *e = (const void *)p;
			if (override_count < MAX_OVERRIDES) {
				struct override *o =
				    &overrides[override_count++];
				o->isa_irq = e->source;
				o->gsi = e->gsi;
				o->flags = e->flags;
				o->used = true;
			}
			break;
		}
		case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
			const struct madt_lapic_addr_override *e =
			    (const void *)p;
			lapic_phys = e->address;
			break;
		}
		default:
			break; /* spec §2.9: unknown entries are info */
		}

		p += h->length;
	}
}

/* ------------------------------------------------------------------------- */
/* LAPIC                                                                      */
/* ------------------------------------------------------------------------- */

#define LAPIC_REG_ID 0x020
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0

static uint32_t lapic_read(uint32_t off) { return lapic_mmio[off / 4]; }

static void lapic_write(uint32_t off, uint32_t val)
{
	lapic_mmio[off / 4] = val;
	(void)lapic_read(LAPIC_REG_ID); /* serializing read */
}

void apic_eoi(void)
{
	if (lapic_mmio) {
		lapic_write(LAPIC_REG_EOI, 0);
	}
}

volatile uint32_t *lapic_mmio_base(void) { return lapic_mmio; }

static void lapic_init(void)
{
	uint64_t apic_base = rdmsr(MSR_APIC_BASE);

	/* Bit 11: global enable; preserve BSP bit (8). */
	apic_base |= (1ull << 11);
	wrmsr(MSR_APIC_BASE, apic_base);

	uint64_t mmio_phys = apic_base & 0xFFFFFFFFFFFFF000ull;
	if (lapic_phys == 0) {
		lapic_phys = mmio_phys;
	}

	/* Ensure the LAPIC MMIO page is in the HHDM (same RESERVED-region
	 * issue as the RSDP — Limine may not map it). */
	paging_ensure_hhdm(lapic_phys, PAGE_SIZE);
	lapic_mmio = hhdm(lapic_phys);

	/* SVR: vector 0xFF, enable bit (8). */
	lapic_write(LAPIC_REG_SVR, 0x100u | VEC_SPURIOUS);

	pr_info("apic: LAPIC at phys 0x%lx, id=%u\n", (unsigned long)lapic_phys,
		(unsigned)(lapic_read(LAPIC_REG_ID) >> 24));
}

/* ------------------------------------------------------------------------- */
/* IOAPIC                                                                     */
/* ------------------------------------------------------------------------- */

#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_REDTBL 0x10

#define IOAPIC_REDIR_MASK (1u << 16)

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
	io->mmio[0] = reg;
	return io->mmio[4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t val)
{
	io->mmio[0] = reg;
	io->mmio[4] = val;
}

static void ioapic_set_redir(struct ioapic *io, uint32_t pin, uint64_t entry)
{
	ioapic_write(io, IOAPIC_REG_REDTBL + pin * 2,
		     (uint32_t)(entry & 0xFFFFFFFFu));
	ioapic_write(io, IOAPIC_REG_REDTBL + pin * 2 + 1,
		     (uint32_t)(entry >> 32));
}

static struct ioapic *ioapic_for_gsi(uint32_t gsi, uint32_t *pin)
{
	for (size_t i = 0; i < ioapic_count; i++) {
		struct ioapic *io = &ioapics[i];
		if (gsi >= io->gsi_base && gsi < io->gsi_base + io->gsi_count) {
			*pin = gsi - io->gsi_base;
			return io;
		}
	}
	return NULL;
}

static void resolve_isa(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags)
{
	for (size_t i = 0; i < override_count; i++) {
		if (overrides[i].used && overrides[i].isa_irq == isa_irq) {
			*gsi = overrides[i].gsi;
			*flags = overrides[i].flags;
			return;
		}
	}
	*gsi = isa_irq;
	*flags = 0; /* edge, active-high */
}

static void ioapic_init_all(void)
{
	for (size_t i = 0; i < ioapic_count; i++) {
		struct ioapic *io = &ioapics[i];
		uint32_t ver = ioapic_read(io, IOAPIC_REG_VER);
		uint32_t maxred = (ver >> 16) & 0xFFu;
		io->gsi_count = maxred + 1;

		for (uint32_t pin = 0; pin < io->gsi_count; pin++) {
			ioapic_set_redir(io, pin, (uint64_t)IOAPIC_REDIR_MASK);
		}

		pr_info("apic: IOAPIC %u at phys 0x%lx, gsi %u..%u\n",
			(unsigned)io->id, (unsigned long)io->mmio_phys,
			(unsigned)io->gsi_base,
			(unsigned)(io->gsi_base + io->gsi_count - 1));
	}
}

void ioapic_route_isa_irq(uint8_t isa_irq, uint8_t vector)
{
	uint32_t gsi;
	uint16_t flags;
	resolve_isa(isa_irq, &gsi, &flags);

	uint32_t pin;
	struct ioapic *io = ioapic_for_gsi(gsi, &pin);
	if (!io) {
		pr_warn("apic: no IOAPIC for ISA IRQ %u (gsi=%u)\n",
			(unsigned)isa_irq, (unsigned)gsi);
		return;
	}

	uint64_t entry = vector;
	/* Polarity: bit 13 (1 = active-low). MADT flags 2:1: 0/1=conform,
	 * 2=reserved, 3=active-low. */
	if ((flags & 0x3) == 0x3) {
		entry |= (1ull << 13);
	}
	/* Trigger: bit 15 (1 = level). Flags 3:2: 3=level. */
	if ((flags & 0xC) == 0xC) {
		entry |= (1ull << 15);
	}
	/* Destination LAPIC id 0 in physical mode (bits 56–59). */
	ioapic_set_redir(io, pin, entry);
}

void ioapic_mask(uint8_t isa_irq)
{
	uint32_t gsi;
	uint16_t flags;
	resolve_isa(isa_irq, &gsi, &flags);

	uint32_t pin;
	struct ioapic *io = ioapic_for_gsi(gsi, &pin);
	if (!io) {
		return;
	}

	uint32_t lo = ioapic_read(io, IOAPIC_REG_REDTBL + pin * 2);
	lo |= IOAPIC_REDIR_MASK;
	ioapic_write(io, IOAPIC_REG_REDTBL + pin * 2, lo);
}

void ioapic_unmask(uint8_t isa_irq)
{
	uint32_t gsi;
	uint16_t flags;
	resolve_isa(isa_irq, &gsi, &flags);

	uint32_t pin;
	struct ioapic *io = ioapic_for_gsi(gsi, &pin);
	if (!io) {
		return;
	}

	uint32_t lo = ioapic_read(io, IOAPIC_REG_REDTBL + pin * 2);
	lo &= ~IOAPIC_REDIR_MASK;
	ioapic_write(io, IOAPIC_REG_REDTBL + pin * 2, lo);
}

/* ------------------------------------------------------------------------- */
/* Top-level                                                                  */
/* ------------------------------------------------------------------------- */

void apic_init(uint64_t rsdp_phys, uint64_t hhdm_offset)
{
	apic_hhdm = hhdm_offset;

	if (!rsdp_phys) {
		pr_err("apic: no RSDP from Limine\n");
		return;
	}

	acpi_init(rsdp_phys, hhdm_offset);

	const struct acpi_sdt_header *madt_h = acpi_find_table("APIC");
	if (!madt_h) {
		pr_err("apic: MADT not found\n");
		return;
	}

	/* Map the full MADT before walking its variable-length entries. */
	paging_ensure_hhdm(virt_to_phys((void *)madt_h), madt_h->length);
	parse_madt((const struct acpi_madt *)madt_h);

	pr_info("apic: MADT: %u LAPIC%s, %u IOAPIC%s, %u override%s\n",
		(unsigned)lapic_count, lapic_count == 1 ? "" : "s",
		(unsigned)ioapic_count, ioapic_count == 1 ? "" : "s",
		(unsigned)override_count, override_count == 1 ? "" : "s");

	lapic_init();
	ioapic_init_all();
}
