/*
 * include/jnu/acpi.h — Minimal ACPI table discovery.
 *
 * Shared RSDP/SDT structures and a generic table finder used by
 * apic.c, hpet.c, and any future ACPI consumers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/compiler.h>
#include <jnu/types.h>

struct __packed acpi_rsdp {
	char		signature[8];
	uint8_t		checksum;
	char		oem_id[6];
	uint8_t		revision;
	uint32_t	rsdt_address;
	/* v2+ fields */
	uint32_t	length;
	uint64_t	xsdt_address;
	uint8_t		extended_checksum;
	uint8_t		reserved[3];
};

struct __packed acpi_sdt_header {
	char		signature[4];
	uint32_t	length;
	uint8_t		revision;
	uint8_t		checksum;
	char		oem_id[6];
	char		oem_table_id[8];
	uint32_t	oem_revision;
	uint32_t	creator_id;
	uint32_t	creator_revision;
};

/*
 * One-time init: caches the RSDP pointer and HHDM offset.
 * Must be called before acpi_find_table().
 */
void acpi_init(uint64_t rsdp_phys, uint64_t hhdm_offset);

/*
 * Find an ACPI table by its 4-byte signature (e.g. "APIC", "HPET").
 * Returns a pointer into the HHDM, or NULL if not found.
 * The caller must call paging_ensure_hhdm() on the full table length
 * before accessing fields past the header.
 */
const struct acpi_sdt_header *acpi_find_table(const char *sig);

/* Validate an ACPI checksum over `len` bytes. */
bool acpi_checksum_ok(const void *p, size_t len);
