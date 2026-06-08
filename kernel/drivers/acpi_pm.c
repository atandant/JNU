/*
 * kernel/drivers/acpi_pm.c — ACPI power management (reboot / poweroff).
 *
 * Parses the FADT ("FACP") to find the PM1 control registers, the SMI
 * command port, the reset register, and the PM timer.  Provides:
 *   - acpi_reboot()       — reset via ACPI / 0xCF9 / 8042, then halt
 *   - acpi_poweroff()     — ACPI S5 transition (+ VM fallbacks)
 *   - acpi_pm_timer_port()— I/O port of the ACPI PM timer, if any
 *
 * Determining the S5 sleep type requires the SLP_TYP values, which live
 * in the DSDT as AML.  Rather than interpret AML, we do a bounded scan
 * for the "_S5_" object and read the two byte constants out of its
 * package — the well-known hobby-kernel shortcut.  If the scan fails we
 * fall back to known VM poweroff ports.
 *
 * Reference: ACPI Specification 6.x §4.8 (Fixed Hardware), §5.2.9 (FADT).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/drivers/acpi.h>
#include <jnu/drivers/io.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>
#include <jnu/mm/paging.h>

/* ------------------------------------------------------------------ */
/* Cached FADT-derived state                                          */
/* ------------------------------------------------------------------ */

static bool fadt_ok;
static uint16_t pm1a_cnt; /* I/O port of PM1a_CNT (0 = none) */
static uint16_t pm1b_cnt; /* I/O port of PM1b_CNT (0 = none) */
static uint32_t smi_cmd;
static uint8_t acpi_enable_val;

static bool reset_supported;
static struct acpi_gas reset_reg;
static uint8_t reset_value;

static uint32_t pm_tmr_port;

static bool s5_found;
static uint16_t slp_typa;
static uint16_t slp_typb;

/* VM/firmware poweroff ports used as a last resort. */
#define POWEROFF_QEMU_PORT 0x604
#define POWEROFF_QEMU_VAL 0x2000
#define POWEROFF_BOCHS_PORT 0xB004
#define POWEROFF_BOCHS_VAL 0x2000
#define POWEROFF_VBOX_PORT 0x4004
#define POWEROFF_VBOX_VAL 0x3400

/* ------------------------------------------------------------------ */
/* DSDT _S5_ scraper (bounded; not an AML interpreter)                */
/* ------------------------------------------------------------------ */

/*
 * Locate the "_S5_" package in the DSDT and extract SLP_TYPa/SLP_TYPb.
 *
 * The expected encoding (ACPI AML) is:
 *   NameOp(0x08) ['\'] "_S5_" PackageOp(0x12) PkgLength NumElements
 *       [BytePrefix(0x0A)] SLP_TYPa  [BytePrefix(0x0A)] SLP_TYPb  ...
 *
 * Every access is bounded by the DSDT length, so a malformed table can
 * only cause us to return false, never read out of bounds.
 */
static bool scrape_s5(uint64_t dsdt_pa, uint16_t *out_a, uint16_t *out_b)
{
	if (!dsdt_pa) {
		return false;
	}

	paging_ensure_hhdm(dsdt_pa, sizeof(struct acpi_sdt_header));
	const struct acpi_sdt_header *dsdt = phys_to_virt(dsdt_pa);

	if (memcmp(dsdt->signature, "DSDT", 4) != 0 ||
	    dsdt->length < sizeof(*dsdt)) {
		return false;
	}

	paging_ensure_hhdm(dsdt_pa, dsdt->length);

	const uint8_t *aml = (const uint8_t *)dsdt + sizeof(*dsdt);
	size_t aml_len = dsdt->length - sizeof(*dsdt);

	for (size_t i = 0; i + 4 <= aml_len; i++) {
		if (memcmp(aml + i, "_S5_", 4) != 0) {
			continue;
		}

		/* Confirm this name is introduced by a NameOp, optionally
		 * preceded by a root-scope '\' prefix. */
		bool is_name = false;
		if (i >= 1 && aml[i - 1] == 0x08) {
			is_name = true;
		} else if (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\') {
			is_name = true;
		}
		if (!is_name) {
			continue;
		}

		size_t off = i + 4; /* first byte after "_S5_" */
		if (off >= aml_len || aml[off] != 0x12) {
			continue; /* not a PackageOp */
		}
		off++; /* skip PackageOp */
		if (off >= aml_len) {
			return false;
		}
		/* Skip PkgLength: top 2 bits of the lead byte give the
		 * number of *additional* length bytes that follow it. */
		off += (size_t)(aml[off] >> 6) + 1;
		if (off >= aml_len) {
			return false;
		}
		off++; /* skip NumElements */
		if (off >= aml_len) {
			return false;
		}

		/* Element 0: SLP_TYPa (optionally a BytePrefix const). */
		if (aml[off] == 0x0A && off + 1 < aml_len) {
			off++;
		}
		if (off >= aml_len) {
			return false;
		}
		*out_a = aml[off];
		off++;

		/* Element 1: SLP_TYPb (optional). */
		*out_b = 0;
		if (off < aml_len) {
			if (aml[off] == 0x0A && off + 1 < aml_len) {
				off++;
			}
			if (off < aml_len) {
				*out_b = aml[off];
			}
		}
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* FADT parsing                                                       */
/* ------------------------------------------------------------------ */

#define FADT_HAS(f, field)                                                     \
	((f)->hdr.length >=                                                    \
	 __builtin_offsetof(struct acpi_fadt, field) + sizeof((f)->field))

void acpi_pm_init(void)
{
	const struct acpi_sdt_header *h = acpi_find_table("FACP");
	if (!h) {
		pr_info("acpi: no FADT; reboot will use legacy methods\n");
		return;
	}

	const struct acpi_fadt *f = (const struct acpi_fadt *)h;
	const bool ext = h->revision >= 2;

	/* PM1 control registers: prefer legacy I/O ports, fall back to the
	 * extended GAS address when the legacy field is zero. */
	pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
	pm1b_cnt = (uint16_t)f->pm1b_cnt_blk;
	if (ext) {
		if (!pm1a_cnt && FADT_HAS(f, x_pm1a_cnt_blk) &&
		    f->x_pm1a_cnt_blk.address) {
			pm1a_cnt = (uint16_t)f->x_pm1a_cnt_blk.address;
		}
		if (!pm1b_cnt && FADT_HAS(f, x_pm1b_cnt_blk) &&
		    f->x_pm1b_cnt_blk.address) {
			pm1b_cnt = (uint16_t)f->x_pm1b_cnt_blk.address;
		}
	}

	smi_cmd = f->smi_cmd;
	acpi_enable_val = f->acpi_enable;
	pm_tmr_port = f->pm_tmr_blk;

	/* Reset register (rev2+ and only if the table is long enough). */
	if ((f->flags & ACPI_FADT_FLAG_RESET_REG_SUP) &&
	    FADT_HAS(f, reset_value)) {
		reset_reg = f->reset_reg;
		reset_value = f->reset_value;
		reset_supported = true;
	}

	fadt_ok = true;

	/* DSDT for the _S5_ scrape. */
	uint64_t dsdt_pa = f->dsdt;
	if (ext && FADT_HAS(f, x_dsdt) && f->x_dsdt) {
		dsdt_pa = f->x_dsdt;
	}
	s5_found = scrape_s5(dsdt_pa, &slp_typa, &slp_typb);

	pr_info(
	    "acpi: FADT: PM1a_CNT=0x%x PM1b_CNT=0x%x SMI=0x%x PM_TMR=0x%x\n",
	    pm1a_cnt, pm1b_cnt, (unsigned)smi_cmd, (unsigned)pm_tmr_port);
	if (s5_found) {
		pr_info("acpi: S5 sleep types: SLP_TYPa=%u SLP_TYPb=%u\n",
			(unsigned)slp_typa, (unsigned)slp_typb);
	} else {
		pr_warn(
		    "acpi: _S5_ not found; poweroff will use VM fallbacks\n");
	}
}

uint32_t acpi_pm_timer_port(void) { return pm_tmr_port; }

/* ------------------------------------------------------------------ */
/* Reboot                                                             */
/* ------------------------------------------------------------------ */

static void try_acpi_reset(void)
{
	if (!reset_supported) {
		return;
	}

	switch (reset_reg.address_space_id) {
	case ACPI_ADDR_SPACE_IO:
		outb((uint16_t)reset_reg.address, reset_value);
		break;
	case ACPI_ADDR_SPACE_MEM: {
		paging_ensure_hhdm(reset_reg.address, 1);
		volatile uint8_t *p = phys_to_virt(reset_reg.address);
		*p = reset_value;
		break;
	}
	default:
		/* PCI-config-space resets are unsupported; fall through to
		 * the legacy methods below. */
		break;
	}
	io_wait();
}

__noreturn void acpi_reboot(void)
{
	__asm__ __volatile__("cli");

	try_acpi_reset();

	/* 0xCF9 PCI reset control: set RST_CPU|SYS_RST. */
	outb(0xCF9, 0x02);
	io_wait();
	outb(0xCF9, 0x06);
	io_wait();

	/* 8042 keyboard controller: pulse the CPU reset line. Drain the
	 * input buffer first so the controller accepts the command. */
	for (int i = 0; i < 1000 && (inb(0x64) & 0x02); i++) {
		io_wait();
	}
	outb(0x64, 0xFE);
	io_wait();

	/* Nothing worked — halt forever. */
	for (;;) {
		__asm__ __volatile__("hlt");
	}
}

/* ------------------------------------------------------------------ */
/* Poweroff                                                           */
/* ------------------------------------------------------------------ */

static void vm_poweroff_fallbacks(void)
{
	outw(POWEROFF_QEMU_PORT, POWEROFF_QEMU_VAL);
	outw(POWEROFF_BOCHS_PORT, POWEROFF_BOCHS_VAL);
	outw(POWEROFF_VBOX_PORT, POWEROFF_VBOX_VAL);
}

void acpi_poweroff(void)
{
	if (!fadt_ok || !pm1a_cnt || !s5_found) {
		vm_poweroff_fallbacks();
		return;
	}

	/* Enter ACPI mode if firmware is still in legacy mode. */
	if (smi_cmd && acpi_enable_val &&
	    !(inw(pm1a_cnt) & ACPI_PM1_CNT_SCI_EN)) {
		outb((uint16_t)smi_cmd, acpi_enable_val);
		for (int i = 0; i < 300; i++) {
			if (inw(pm1a_cnt) & ACPI_PM1_CNT_SCI_EN) {
				break;
			}
			io_wait();
		}
	}

	outw(pm1a_cnt,
	     (uint16_t)(((unsigned)slp_typa << 10) | ACPI_PM1_CNT_SLP_EN));
	if (pm1b_cnt) {
		outw(pm1b_cnt, (uint16_t)(((unsigned)slp_typb << 10) |
					  ACPI_PM1_CNT_SLP_EN));
	}

	/* If we are still running, ACPI poweroff was rejected. Try the
	 * well-known virtual-machine ports before giving up. */
	vm_poweroff_fallbacks();
}
