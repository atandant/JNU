/*
 * kernel/drivers/acpi.c — Minimal ACPI table discovery.
 *
 * Walks RSDP → XSDT/RSDT to find tables by signature. Used by
 * apic.c (for "APIC"/MADT) and hpet.c (for "HPET").
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/acpi.h>
#include <jnu/lib/string.h>
#include <jnu/mm/paging.h>

static uint64_t acpi_hhdm;
static const struct acpi_rsdp *cached_rsdp;
static bool acpi_initialised;

static void *hhdm(uint64_t pa) { return (void *)(uintptr_t)(pa + acpi_hhdm); }

/*
 * Map a discovered SDT fully into the HHDM and validate it.
 *
 * Maps the header, then the full table length, and verifies both the
 * signature (if `sig` is non-NULL) and the table checksum.  Returns a
 * usable HHDM pointer to the whole table, or NULL if the table does not
 * match or is corrupt.
 */
static const struct acpi_sdt_header *map_and_validate(uint64_t pa,
						      const char *sig)
{
	if (!pa) {
		return NULL;
	}

	paging_ensure_hhdm(pa, sizeof(struct acpi_sdt_header));
	const struct acpi_sdt_header *t = hhdm(pa);

	if (sig && memcmp(t->signature, sig, 4) != 0) {
		return NULL;
	}
	if (t->length < sizeof(*t)) {
		return NULL;
	}

	paging_ensure_hhdm(pa, t->length);
	if (!acpi_checksum_ok(t, t->length)) {
		return NULL;
	}

	return t;
}

bool acpi_checksum_ok(const void *p, size_t len)
{
	const uint8_t *b = p;
	uint8_t sum = 0;
	for (size_t i = 0; i < len; i++) {
		sum = (uint8_t)(sum + b[i]);
	}
	return sum == 0;
}

void acpi_init(uint64_t rsdp_phys, uint64_t hhdm_offset)
{
	/*
	 * Idempotent: the first successful call wins.  apic_init() and
	 * hpet_init() each call this defensively so they remain
	 * self-contained, but only the first call does any real work.
	 */
	if (acpi_initialised) {
		return;
	}
	acpi_initialised = true;

	acpi_hhdm = hhdm_offset;
	cached_rsdp = NULL;

	if (!rsdp_phys) {
		return;
	}

	paging_ensure_hhdm(rsdp_phys, sizeof(struct acpi_rsdp));
	const struct acpi_rsdp *rsdp = hhdm(rsdp_phys);

	if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 ||
	    !acpi_checksum_ok(rsdp, 20)) {
		return;
	}

	cached_rsdp = rsdp;
}

const struct acpi_sdt_header *acpi_find_table(const char *sig)
{
	if (!cached_rsdp || !sig) {
		return NULL;
	}

	if (cached_rsdp->revision >= 2 && cached_rsdp->xsdt_address) {
		const struct acpi_sdt_header *xsdt =
		    map_and_validate(cached_rsdp->xsdt_address, "XSDT");
		if (!xsdt) {
			return NULL;
		}
		size_t n = (xsdt->length - sizeof(*xsdt)) / 8;
		const uint8_t *ptrs = (const uint8_t *)xsdt + sizeof(*xsdt);
		for (size_t i = 0; i < n; i++) {
			/* XSDT entries are only 4-byte aligned; copy out. */
			uint64_t pa;
			memcpy(&pa, ptrs + i * 8, sizeof(pa));
			const struct acpi_sdt_header *t =
			    map_and_validate(pa, sig);
			if (t) {
				return t;
			}
		}
	} else if (cached_rsdp->rsdt_address) {
		const struct acpi_sdt_header *rsdt =
		    map_and_validate(cached_rsdp->rsdt_address, "RSDT");
		if (!rsdt) {
			return NULL;
		}
		size_t n = (rsdt->length - sizeof(*rsdt)) / 4;
		const uint8_t *ptrs = (const uint8_t *)rsdt + sizeof(*rsdt);
		for (size_t i = 0; i < n; i++) {
			uint32_t pa;
			memcpy(&pa, ptrs + i * 4, sizeof(pa));
			const struct acpi_sdt_header *t =
			    map_and_validate(pa, sig);
			if (t) {
				return t;
			}
		}
	}
	return NULL;
}
