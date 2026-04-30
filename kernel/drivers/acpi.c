/*
 * kernel/drivers/acpi.c — Minimal ACPI table discovery.
 *
 * Walks RSDP → XSDT/RSDT to find tables by signature. Used by
 * apic.c (for "APIC"/MADT) and hpet.c (for "HPET").
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/acpi.h>
#include <jnu/paging.h>
#include <jnu/string.h>
#include <jnu/types.h>

static uint64_t acpi_hhdm;
static const struct acpi_rsdp *cached_rsdp;

static void *hhdm(uint64_t pa)
{
	return (void *)(uintptr_t)(pa + acpi_hhdm);
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
		paging_ensure_hhdm(cached_rsdp->xsdt_address,
				   sizeof(struct acpi_sdt_header));
		const struct acpi_sdt_header *xsdt =
			hhdm(cached_rsdp->xsdt_address);
		paging_ensure_hhdm(cached_rsdp->xsdt_address, xsdt->length);
		size_t n = (xsdt->length - sizeof(*xsdt)) / 8;
		const uint64_t *ptrs =
			(const uint64_t *)((const uint8_t *)xsdt +
					   sizeof(*xsdt));
		for (size_t i = 0; i < n; i++) {
			paging_ensure_hhdm(ptrs[i],
					   sizeof(struct acpi_sdt_header));
			const struct acpi_sdt_header *t = hhdm(ptrs[i]);
			if (memcmp(t->signature, sig, 4) == 0) {
				return t;
			}
		}
	} else if (cached_rsdp->rsdt_address) {
		paging_ensure_hhdm(cached_rsdp->rsdt_address,
				   sizeof(struct acpi_sdt_header));
		const struct acpi_sdt_header *rsdt =
			hhdm(cached_rsdp->rsdt_address);
		paging_ensure_hhdm(cached_rsdp->rsdt_address, rsdt->length);
		size_t n = (rsdt->length - sizeof(*rsdt)) / 4;
		const uint32_t *ptrs =
			(const uint32_t *)((const uint8_t *)rsdt +
					   sizeof(*rsdt));
		for (size_t i = 0; i < n; i++) {
			paging_ensure_hhdm(ptrs[i],
					   sizeof(struct acpi_sdt_header));
			const struct acpi_sdt_header *t = hhdm(ptrs[i]);
			if (memcmp(t->signature, sig, 4) == 0) {
				return t;
			}
		}
	}
	return NULL;
}
