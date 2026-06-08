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
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_address;
	/* v2+ fields */
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t extended_checksum;
	uint8_t reserved[3];
};

struct __packed acpi_sdt_header {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
};

/*
 * ACPI Generic Address Structure (GAS) — describes a register's location.
 * Used by the FADT (reset/PM blocks) and the HPET table base address.
 */
enum acpi_addr_space {
	ACPI_ADDR_SPACE_MEM = 0,    /* System memory (MMIO) */
	ACPI_ADDR_SPACE_IO = 1,     /* System I/O port */
	ACPI_ADDR_SPACE_PCI = 2,    /* PCI configuration space */
};

struct __packed acpi_gas {
	uint8_t address_space_id;
	uint8_t bit_width;
	uint8_t bit_offset;
	uint8_t access_size;
	uint64_t address;
};

/*
 * One-time init: caches the RSDP pointer and HHDM offset.
 * Must be called before acpi_find_table().
 */
void acpi_init(uint64_t rsdp_phys, uint64_t hhdm_offset);

/*
 * Find an ACPI table by its 4-byte signature (e.g. "APIC", "HPET").
 * Returns a pointer into the HHDM, or NULL if not found or corrupt.
 *
 * The returned table is fully mapped into the HHDM and its checksum has
 * been verified, so the caller may read the entire table (including
 * variable-length entries past the header) without any further
 * paging_ensure_hhdm() calls.
 */
const struct acpi_sdt_header *acpi_find_table(const char *sig);

/* Validate an ACPI checksum over `len` bytes. */
bool acpi_checksum_ok(const void *p, size_t len);

/*
 * Fixed ACPI Description Table (signature "FACP").
 *
 * Only the fields JNU consumes are named; the layout up to X_PM_TMR_BLK
 * matches the ACPI spec exactly so offsets stay correct.  Extended (X_)
 * 64-bit fields are present from revision 2 onwards.
 */
struct __packed acpi_fadt {
	struct acpi_sdt_header hdr;
	uint32_t firmware_ctrl;
	uint32_t dsdt;
	uint8_t reserved0;
	uint8_t preferred_pm_profile;
	uint16_t sci_int;
	uint32_t smi_cmd;
	uint8_t acpi_enable;
	uint8_t acpi_disable;
	uint8_t s4bios_req;
	uint8_t pstate_cnt;
	uint32_t pm1a_evt_blk;
	uint32_t pm1b_evt_blk;
	uint32_t pm1a_cnt_blk;
	uint32_t pm1b_cnt_blk;
	uint32_t pm2_cnt_blk;
	uint32_t pm_tmr_blk;
	uint32_t gpe0_blk;
	uint32_t gpe1_blk;
	uint8_t pm1_evt_len;
	uint8_t pm1_cnt_len;
	uint8_t pm2_cnt_len;
	uint8_t pm_tmr_len;
	uint8_t gpe0_blk_len;
	uint8_t gpe1_blk_len;
	uint8_t gpe1_base;
	uint8_t cst_cnt;
	uint16_t p_lvl2_lat;
	uint16_t p_lvl3_lat;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t duty_offset;
	uint8_t duty_width;
	uint8_t day_alrm;
	uint8_t mon_alrm;
	uint8_t century;
	uint16_t iapc_boot_arch;
	uint8_t reserved1;
	uint32_t flags;
	struct acpi_gas reset_reg;
	uint8_t reset_value;
	uint16_t arm_boot_arch;
	uint8_t fadt_minor_version;
	uint64_t x_firmware_ctrl;
	uint64_t x_dsdt;
	struct acpi_gas x_pm1a_evt_blk;
	struct acpi_gas x_pm1b_evt_blk;
	struct acpi_gas x_pm1a_cnt_blk;
	struct acpi_gas x_pm1b_cnt_blk;
	struct acpi_gas x_pm2_cnt_blk;
	struct acpi_gas x_pm_tmr_blk;
};

/* FADT flags bits we care about. */
#define ACPI_FADT_FLAG_TMR_VAL_EXT (1u << 8)  /* PM timer is 32-bit */
#define ACPI_FADT_FLAG_RESET_REG_SUP (1u << 10)

/* PM1 control register bits. */
#define ACPI_PM1_CNT_SCI_EN (1u << 0)
#define ACPI_PM1_CNT_SLP_EN (1u << 13)

/*
 * Parse the FADT and prepare reboot/poweroff. Safe to call once after
 * acpi_init(); does nothing useful if no FADT is present (the reboot
 * fallbacks still work).
 */
void acpi_pm_init(void);

/* Reset the machine. Tries ACPI, then 0xCF9, then the 8042 controller. */
__noreturn void acpi_reboot(void);

/*
 * Power the machine off via ACPI S5. Returns only if every method
 * failed (caller should then halt).
 */
void acpi_poweroff(void);

/*
 * I/O port of the ACPI PM timer (24/32-bit, 3.579545 MHz), or 0 if the
 * FADT did not advertise one. Provided for a future clock source.
 */
uint32_t acpi_pm_timer_port(void);
