/*
 * kernel/kernel/main.c — Kernel entry, Phase 3.
 *
 * Boot flow:
 *   1. Verify Limine base revision support; declare requests.
 *   2. Bring up klog and serial.
 *   3. Banner.
 *   4. Init framebuffer console.
 *   5. CPU bring-up (CPUID, CR0/CR4/EFER, GS_BASE), GDT/TSS, IDT, PIC
 *      remap+mask, ACPI/MADT/APIC.
 *   6. PMM, paging, VMM, slab.
 *   7. PIT (100 Hz timer via IOAPIC).
 *   8. TSC calibration (klog timestamps now sensible).
 *   9. RTC wall-clock read.
 *  10. PCI enumeration.
 *  11. ATA init + block device registration.
 *  12. Keyboard init.
 *  13. Selftests, gated on `selftest=1`.
 *  14. Optional debug hooks (`panictest=1`, `dump=mem`, `dump=blocks`).
 *  15. Idle loop.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/apic.h>
#include <jnu/ata.h>
#include <jnu/block.h>
#include <jnu/cmdline.h>
#include <jnu/compiler.h>
#include <jnu/cpu.h>
#include <jnu/fbcon.h>
#include <jnu/gdt.h>
#include <jnu/idt.h>
#include <jnu/kbd.h>
#include <jnu/klog.h>
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/pci.h>
#include <jnu/pit.h>
#include <jnu/pmm.h>
#include <jnu/rtc.h>
#include <jnu/sched.h>
#include <jnu/selftest.h>
#include <jnu/serial.h>
#include <jnu/slab.h>
#include <jnu/string.h>
#include <jnu/types.h>
#include <jnu/vfs.h>
#include <jnu/vmm.h>

#include <limine.h>

extern const char jnu_version[];
extern const char jnu_build[];
extern const char jnu_buildtime[];

void pic_remap_and_mask(void);

/* ------------------------------------------------------------------------- */
/* Limine boot-protocol requests                                              */
/* ------------------------------------------------------------------------- */

__used __section(".limine_requests")
static volatile LIMINE_BASE_REVISION(3)

__used __section(".limine_requests")
static volatile struct limine_framebuffer_request fb_request = {
	.id		= LIMINE_FRAMEBUFFER_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests")
static volatile struct limine_kernel_address_request kaddr_request = {
	.id		= LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests")
static volatile struct limine_kernel_file_request kfile_request = {
	.id		= LIMINE_KERNEL_FILE_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests")
static volatile struct limine_memmap_request memmap_request = {
	.id		= LIMINE_MEMMAP_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests")
static volatile struct limine_hhdm_request hhdm_request = {
	.id		= LIMINE_HHDM_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests")
static volatile struct limine_rsdp_request rsdp_request = {
	.id		= LIMINE_RSDP_REQUEST,
	.revision	= 0,
};

__used __section(".limine_requests_start")
static volatile LIMINE_REQUESTS_START_MARKER

__used __section(".limine_requests_end")
static volatile LIMINE_REQUESTS_END_MARKER

/* ------------------------------------------------------------------------- */
/* Boot helpers                                                               */
/* ------------------------------------------------------------------------- */

static void banner(void)
{
	pr_info("JNU %s (build %s, %s)\n",
		jnu_version, jnu_build, jnu_buildtime);
}

static void bring_up_fbcon(void)
{
	if (!fb_request.response ||
	    fb_request.response->framebuffer_count == 0) {
		pr_warn("fbcon: no framebuffer; serial-only\n");
		return;
	}

	struct limine_framebuffer *lfb = fb_request.response->framebuffers[0];
	struct fbcon_info info = {
		.addr	= lfb->address,
		.width	= lfb->width,
		.height	= lfb->height,
		.pitch	= lfb->pitch,
		.bpp	= lfb->bpp,
	};

	int err = fbcon_init(&info);
	if (err) {
		pr_warn("fbcon: init failed (err=%d), serial-only\n", err);
		return;
	}

	pr_info("fbcon: %ux%u, %u bpp\n",
		(unsigned)lfb->width, (unsigned)lfb->height,
		(unsigned)lfb->bpp);
}

static uint64_t resolve_hhdm(void)
{
	if (hhdm_request.response) {
		return hhdm_request.response->offset;
	}
	panic("limine: no HHDM response");
}

static uint64_t resolve_rsdp_phys(uint64_t hhdm)
{
	if (!rsdp_request.response || !rsdp_request.response->address) {
		return 0;
	}
	uint64_t addr = (uint64_t)(uintptr_t)rsdp_request.response->address;
	/*
	 * Limine v3+ returns a virtual (HHDM) pointer; older revisions
	 * returned a physical address. Detect by whether it sits above
	 * the kernel-half cutoff.
	 */
	if (addr >= 0xFFFF800000000000ull) {
		return addr - hhdm;
	}
	return addr;
}

/* ------------------------------------------------------------------------- */
/* Debug hooks                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Hex-dump the first 8 sectors of the first block device. Gated on
 * cmdline `dump=blocks`.
 */
static void dump_blocks(void)
{
	struct block_device *bdev = block_lookup("hda");
	if (!bdev) {
		pr_warn("dump: no 'hda' block device\n");
		return;
	}

	uint8_t buf[512];
	for (int sec = 0; sec < 8; sec++) {
		int err = block_read(bdev, (uint64_t)sec, 1, buf);
		if (err) {
			pr_err("dump: read sector %d failed (%d)\n",
			       sec, err);
			break;
		}

		pr_info("dump: --- sector %d ---\n", sec);
		for (int row = 0; row < 32; row++) {
			int off = row * 16;
			pr_info("dump: %04x: "
				"%02x %02x %02x %02x "
				"%02x %02x %02x %02x "
				"%02x %02x %02x %02x "
				"%02x %02x %02x %02x\n",
				(unsigned)(sec * 512 + off),
				buf[off+0],  buf[off+1],
				buf[off+2],  buf[off+3],
				buf[off+4],  buf[off+5],
				buf[off+6],  buf[off+7],
				buf[off+8],  buf[off+9],
				buf[off+10], buf[off+11],
				buf[off+12], buf[off+13],
				buf[off+14], buf[off+15]);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Entry                                                                      */
/* ------------------------------------------------------------------------- */

void kernel_main(void);
void kernel_main(void)
{
	if (!LIMINE_BASE_REVISION_SUPPORTED) {
		for (;;) {
			__asm__ __volatile__ ("cli; hlt");
		}
	}

	klog_init();

	const char *cmd = NULL;
	if (kfile_request.response && kfile_request.response->kernel_file &&
	    kfile_request.response->kernel_file->cmdline) {
		cmd = kfile_request.response->kernel_file->cmdline;
	}
	cmdline_parse(cmd);

	serial_init();

	banner();

	if (cmd && cmd[0]) {
		pr_info("cmdline: %s\n", cmd);
	}

	bring_up_fbcon();

	/* CPU + descriptor tables. */
	cpu_init();
	gdt_init();
	idt_init();
	pic_remap_and_mask();

	uint64_t hhdm = resolve_hhdm();
	pr_info("limine: HHDM offset 0x%lx\n", (unsigned long)hhdm);

	if (kaddr_request.response) {
		pr_info("limine: kernel phys=0x%lx virt=0x%lx\n",
			(unsigned long)kaddr_request.response->physical_base,
			(unsigned long)kaddr_request.response->virtual_base);
	}

	/* Memory: PMM first (it owns physical pages), then paging-helpers,
	 * then VMM, then slab/kmalloc on top. */
	if (!memmap_request.response) {
		panic("limine: no memmap response");
	}
	pmm_init(memmap_request.response, hhdm);
	paging_init(hhdm);
	vmm_init();
	slab_init();

	/* APIC needs ACPI tables, available via HHDM. */
	uint64_t rsdp_phys = resolve_rsdp_phys(hhdm);
	apic_init(rsdp_phys, hhdm);

	/* PIT timer: 100 Hz via IOAPIC. Must come before TSC calibration
	 * if we ever switch cpu_calibrate_tsc to use PIT channel 0 IRQs
	 * (currently it uses channel 2 polling, so order is flexible). */
	pit_init();

	/* TSC calibration: klog timestamps stop being zero from here. */
	cpu_calibrate_tsc();

	/* RTC: print wall-clock time at boot. */
	rtc_init();

	sched_init();

	/* Set RSP0 once to the boot stack top so a future user→kernel
	 * trap has a valid kernel stack to switch to. */
	uint64_t rsp_now;
	__asm__ __volatile__ ("mov %%rsp, %0" : "=r"(rsp_now));
	tss_set_rsp0((rsp_now + 0xFFFull) & ~0xFFFull);

	/* Phase 3 devices. */
	pci_init();
	ata_init();
	kbd_init();

	/* Optional debug dumps. */
	if (cmdline_bool("dump")) {
		const char *what = cmdline_get("dump");
		if (what && strcmp(what, "blocks") == 0) {
			dump_blocks();
		} else {
			pmm_dump();
		}
	}

	vfs_init();

	int err = vfs_mount("hda", "minix", "/");
	if (err) {
		panic("kernel: failed to mount rootfs (err=%d)", err);
	}

	struct vfs_inode *root_ino;
	if (vfs_open("/", &root_ino) == 0) {
		struct vfs_dirent de;
		size_t n = 0;
		while (vfs_readdir(root_ino, n, &de) == 1) n++;
		pr_info("rootfs: %u entries\n", (unsigned)n);
		vfs_close(root_ino);
	}

	/* Selftests, gated on cmdline. */
	if (cmdline_bool("selftest")) {
		int fails = selftest_run_all();
		if (fails) {
			panic("selftest: %d failure(s)", fails);
		}
	}

	if (cmdline_bool("panictest")) {
		panic("v0.0.1 panic check");
	}

	pr_info("kernel: boot complete; idle\n");

	struct char_device *kbd = kbd_get_chardev();
	for (;;) {
		if (kbd && kbd->ops->poll(kbd)) {
			char c;
			if (kbd->ops->read(kbd, &c, 1) == 1) {
				pr_info("kbd: typed '%c'\n", c >= 0x20 ? c : '.');
			}
		}
		__asm__ __volatile__ ("sti; hlt; cli");
	}
}
