/*
 * kernel/drivers/hpet.c — HPET (High Precision Event Timer) driver.
 *
 * Parses the ACPI "HPET" table to find the MMIO base, enables the main
 * counter, and exposes:
 *   - hpet_read_counter()  — raw 64-bit monotonic ticks
 *   - hpet_udelay()        — busy-wait calibration primitive
 *   - hpet_read_ns/us()    — wall-clock monotonic helpers
 *
 * The HPET is NOT used as a tick source (LAPIC timer owns that). Its
 * two roles are (1) more accurate TSC calibration than PIT channel 2,
 * and (2) a high-resolution monotonic reference clock.
 *
 * Reference: IA-PC HPET Specification 1.0a §2.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/acpi.h>
#include <jnu/compiler.h>
#include <jnu/errno.h>
#include <jnu/hpet.h>
#include <jnu/klog.h>
#include <jnu/paging.h>
#include <jnu/types.h>

/*
 * HPET-specific extension of the generic SDT header.
 * The generic acpi_rsdp / acpi_sdt_header types come from <jnu/acpi.h>;
 * only the HPET-specific body fields live here.
 */
struct __packed acpi_hpet {
	struct acpi_sdt_header hdr;
	uint32_t event_timer_block_id;
	struct acpi_gas base_addr;
	uint8_t hpet_number;
	uint16_t minimum_tick;
	uint8_t page_protection;
};

/* ------------------------------------------------------------------ */
/* HPET MMIO registers (Table 2 of the HPET spec)                    */
/* ------------------------------------------------------------------ */

#define HPET_REG_CAP 0x000     /* General Capabilities and ID */
#define HPET_REG_CFG 0x010     /* General Configuration */
#define HPET_REG_STATUS 0x020  /* General Interrupt Status */
#define HPET_REG_COUNTER 0x0F0 /* Main Counter Value */

#define HPET_CFG_ENABLE (1ull << 0)
#define HPET_CFG_LEGACY (1ull << 1)

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static volatile uint64_t *hpet_mmio;
static uint64_t hpet_period_fs; /* femtoseconds per tick (from CAP) */
static bool hpet_ready;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t hpet_read(uint32_t reg)
{
	return *(volatile uint64_t *)((volatile uint8_t *)hpet_mmio + reg);
}

static void hpet_write(uint32_t reg, uint64_t val)
{
	*(volatile uint64_t *)((volatile uint8_t *)hpet_mmio + reg) = val;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int hpet_init(uint64_t rsdp_phys, uint64_t hhdm_offset)
{
	uint64_t cap;

	/*
	 * acpi_init() may have already been called by acpi.c/apic.c;
	 * calling it again with the same arguments is harmless — it just
	 * overwrites the same cached values.  We call it here so that
	 * hpet_init() remains self-contained when the caller has not
	 * initialised ACPI separately.
	 */
	acpi_init(rsdp_phys, hhdm_offset);

	const struct acpi_sdt_header *hdr = acpi_find_table("HPET");
	if (!hdr) {
		pr_info("hpet: no ACPI HPET table found\n");
		return -ENODEV;
	}

	/* acpi_find_table() has already mapped and checksummed the full
	 * table; we only need to confirm it is large enough for us. */
	if (hdr->length < sizeof(struct acpi_hpet)) {
		pr_warn("hpet: ACPI HPET table truncated (length %u < %zu)\n",
			hdr->length, sizeof(struct acpi_hpet));
		return -ENODEV;
	}

	const struct acpi_hpet *tbl = (const struct acpi_hpet *)hdr;

	/* Only memory-mapped I/O is supported. */
	if (tbl->base_addr.address_space_id != ACPI_ADDR_SPACE_MEM) {
		pr_warn("hpet: non-MMIO address space (%u), skipping\n",
			(unsigned)tbl->base_addr.address_space_id);
		return -ENODEV;
	}

	uint64_t base_phys = tbl->base_addr.address;
	if (!base_phys) {
		pr_warn("hpet: MMIO base address is zero\n");
		return -ENODEV;
	}
	paging_ensure_hhdm(base_phys, PAGE_SIZE);
	hpet_mmio = (volatile uint64_t *)phys_to_virt(base_phys);

	/* Read capabilities: bits 63:32 = period in femtoseconds. */
	cap = hpet_read(HPET_REG_CAP);
	hpet_period_fs = cap >> 32;
	if (hpet_period_fs == 0) {
		pr_warn("hpet: period is zero, hardware broken\n");
		hpet_mmio = NULL;
		return -ENODEV;
	}
	hpet_write(HPET_REG_CFG, 0);
	hpet_write(HPET_REG_COUNTER, 0);
	hpet_write(HPET_REG_CFG, HPET_CFG_ENABLE);

	hpet_ready = true;

	uint64_t freq_hz = 1000000000000000ull / hpet_period_fs;
	uint32_t num_timers = (uint32_t)((cap >> 8) & 0x1F) + 1;
	bool is_64bit = (cap & (1ull << 13)) != 0;

	pr_info("hpet: %u timer%s, %s-bit counter, %lu.%03lu MHz "
		"(period %lu fs)\n",
		num_timers, num_timers == 1 ? "" : "s", is_64bit ? "64" : "32",
		(unsigned long)(freq_hz / 1000000),
		(unsigned long)((freq_hz % 1000000) / 1000),
		(unsigned long)hpet_period_fs);

	return 0;
}

bool hpet_available(void) { return hpet_ready; }

uint64_t hpet_read_counter(void)
{
	if (!hpet_ready) {
		return 0;
	}
	return hpet_read(HPET_REG_COUNTER);
}

uint64_t hpet_ticks_to_ns(uint64_t ticks)
{
	/*
	 * ns = ticks * period_fs / 1,000,000.
	 * To avoid overflow on large tick values, split the division:
	 * period_fs is typically ~10,000,000 (100 ns period for 10 MHz),
	 * so dividing it by 1,000,000 first keeps intermediate products
	 * manageable.
	 */
	return ticks * (hpet_period_fs / 1000000);
}

void hpet_udelay(uint64_t us)
{
	if (!hpet_ready) {
		return;
	}

	/*
	 * 1 microsecond = 1e9 femtoseconds.
	 * ticks_per_us = 1e9 / period_fs.
	 * For a 100 MHz HPET (period 10,000,000 fs).
	 */
	uint64_t ticks_per_us = 1000000000ull / hpet_period_fs;
	if (ticks_per_us == 0) {
		ticks_per_us = 1;
	}
	uint64_t target = us * ticks_per_us;
	uint64_t start = hpet_read(HPET_REG_COUNTER);

	while (hpet_read(HPET_REG_COUNTER) - start < target) {
		__asm__ __volatile__("pause");
	}
}

uint64_t hpet_read_ns(void) { return hpet_ticks_to_ns(hpet_read_counter()); }

uint64_t hpet_read_us(void) { return hpet_read_ns() / 1000; }
