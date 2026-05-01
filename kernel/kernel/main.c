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
 *   7. PIT (100 Hz timer via IOAPIC, used for calibration).
 *   8. TSC calibration (klog timestamps now sensible).
 *   9. RTC wall-clock read.
 *  10. Initramfs parse, scheduler init.
 *  11. LAPIC timer takes over as scheduler tick; PIT IRQ masked.
 *  12. PCI enumeration.
 *  13. ATA init + block device registration.
 *  14. Keyboard init.
 *  15. Selftests, gated on `selftest=1`.
 *  16. Optional debug hooks (`panictest=1`, `dump=mem`, `dump=blocks`).
 *  17. Idle loop.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/apic.h>
#include <jnu/arch_syscall.h>
#include <jnu/ata.h>
#include <jnu/block.h>
#include <jnu/cmdline.h>
#include <jnu/compiler.h>
#include <jnu/cpu.h>
#include <jnu/elf64.h>
#include <jnu/exec.h>
#include <jnu/fbcon.h>
#include <jnu/gdt.h>
#include <jnu/hpet.h>
#include <jnu/idt.h>
#include <jnu/initramfs.h>
#include <jnu/kbd.h>
#include <jnu/klog.h>
#include <jnu/lapic_timer.h>
#include <jnu/paging.h>
#include <jnu/panic.h>
#include <jnu/pci.h>
#include <jnu/pit.h>
#include <jnu/pmm.h>
#include <jnu/process.h>
#include <jnu/rtc.h>
#include <jnu/sched.h>
#include <jnu/selftest.h>
#include <jnu/serial.h>
#include <jnu/slab.h>
#include <jnu/string.h>
#include <jnu/types.h>
#include <jnu/usermode.h>
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

__used __section(".limine_requests") static volatile LIMINE_BASE_REVISION(3)

    __used __section(
	".limine_requests") static volatile struct limine_framebuffer_request
    fb_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0,
};

__used __section(
    ".limine_requests") static volatile struct limine_kernel_address_request
    kaddr_request = {
	.id = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0,
};

__used __section(
    ".limine_requests") static volatile struct limine_kernel_file_request
    kfile_request = {
	.id = LIMINE_KERNEL_FILE_REQUEST,
	.revision = 0,
};

__used
    __section(".limine_requests") static volatile struct limine_memmap_request
    memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0,
};

__used __section(".limine_requests") static volatile struct limine_hhdm_request
    hhdm_request = {
	.id = LIMINE_HHDM_REQUEST,
	.revision = 0,
};

__used __section(".limine_requests") static volatile struct limine_rsdp_request
    rsdp_request = {
	.id = LIMINE_RSDP_REQUEST,
	.revision = 0,
};

__used
    __section(".limine_requests") static volatile struct limine_module_request
    module_request = {
	.id = LIMINE_MODULE_REQUEST,
	.revision = 0,
};

__used __section(
    ".limine_requests_start") static volatile LIMINE_REQUESTS_START_MARKER

    __used
    __section(".limine_requests_end") static volatile LIMINE_REQUESTS_END_MARKER

    /* -------------------------------------------------------------------------
     */
    /* Boot helpers */
    /* -------------------------------------------------------------------------
     */

    static void banner(void)
{
	pr_info("JNU %s (build %s, %s)\n", jnu_version, jnu_build,
		jnu_buildtime);
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
	    .addr = lfb->address,
	    .width = lfb->width,
	    .height = lfb->height,
	    .pitch = lfb->pitch,
	    .bpp = lfb->bpp,
	};

	int err = fbcon_init(&info);
	if (err) {
		pr_warn("fbcon: init failed (err=%d), serial-only\n", err);
		return;
	}

	pr_info("fbcon: %ux%u, %u bpp\n", (unsigned)lfb->width,
		(unsigned)lfb->height, (unsigned)lfb->bpp);
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

static struct limine_file *find_initramfs_module(void)
{
	if (!module_request.response ||
	    module_request.response->module_count == 0) {
		return NULL;
	}

	for (uint64_t i = 0; i < module_request.response->module_count; i++) {
		struct limine_file *file = module_request.response->modules[i];
		if (file && file->cmdline &&
		    strcmp(file->cmdline, "initramfs") == 0) {
			return file;
		}
	}

	if (module_request.response->module_count == 1) {
		return module_request.response->modules[0];
	}

	return NULL;
}

static void bring_up_initramfs(void)
{
	struct limine_file *file = find_initramfs_module();

	if (!file) {
		panic("initramfs: no Limine module found");
	}

	/*
	 * Re-enable when debugging Limine module loading.
	 * const uint8_t *b = file->address;
	 * pr_info("initramfs: module path='%s' cmdline='%s' size=%lu "
	 *	   "magic=%02x %02x %02x %02x %02x %02x\n",
	 *	   file->path ? file->path : "(none)",
	 *	   file->cmdline ? file->cmdline : "(none)",
	 *	   (unsigned long)file->size,
	 *	   b[0], b[1], b[2], b[3], b[4], b[5]);
	 */

	int err = initramfs_init(file->address, (size_t)file->size);
	if (err) {
		panic("initramfs: parse failed (err=%d)", err);
	}
}

static ssize_t initramfs_exec_read(void *ctx, uint64_t off, void *buf,
				   size_t len)
{
	return initramfs_read_at(ctx, off, buf, len);
}

static int validate_initramfs_exec(const char *path,
				   struct exec_load_info *info)
{
	struct initramfs_file file;
	struct exec_image image;
	int err;

	err = initramfs_lookup(path, &file);
	if (err) {
		return err;
	}

	image.read_at = initramfs_exec_read;
	image.size = file.size;
	image.ctx = &file;

	return elf64_validate_image(&image, info);
}

static int load_initramfs_exec(struct addr_space *space, const char *path,
			       struct exec_load_info *info, uint64_t *stack)
{
	struct initramfs_file file;
	struct exec_image image;
	int err;

	err = initramfs_lookup(path, &file);
	if (err) {
		return err;
	}

	image.read_at = initramfs_exec_read;
	image.size = file.size;
	image.ctx = &file;

	err = elf64_load_image(space, &image, info);
	if (err) {
		return err;
	}

	return elf64_setup_initial_stack(space, stack);
}

static ssize_t vfs_exec_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return vfs_read(ctx, off, len, buf);
}

static int validate_vfs_exec(const char *path, struct exec_load_info *info)
{
	struct vfs_inode *ino;
	struct exec_image image;
	int err;

	err = vfs_open(path, &ino);
	if (err) {
		return err;
	}

	image.read_at = vfs_exec_read;
	image.size = ino->size;
	image.ctx = ino;

	err = elf64_validate_image(&image, info);
	vfs_close(ino);
	return err;
}

static void load_userspace_probe(void)
{
	const char *init_path = cmdline_get("init");
	struct exec_load_info init_info;
	struct exec_load_info hello_info;
	struct exec_load_info minix_info;
	struct task *task;
	struct addr_space *space;
	uint64_t stack;
	int err;

	if (cmdline_bool("noinit")) {
		pr_info("userspace: disabled by noinit=1\n");
		return;
	}

	if (!init_path) {
		init_path = "/init";
	}

	task = sched_current();
	if (!task || !task->process) {
		panic("userspace: no current process");
	}

	space = vmm_create_space();
	if (!space) {
		panic("userspace: failed to create init address space");
	}
	task->process->space = space;

	vmm_switch_to(space);
	err = load_initramfs_exec(space, init_path, &init_info, &stack);
	if (err) {
		panic("userspace: failed to load %s from initramfs (err=%d)",
		      init_path, err);
	}
	task->process->user_entry = init_info.entry;
	task->process->user_stack = stack;

	pr_info("userspace: %s ELF64 entry=0x%lx range=0x%lx..0x%lx\n",
		init_path, (unsigned long)init_info.entry,
		(unsigned long)init_info.low, (unsigned long)init_info.high);

	err = validate_initramfs_exec("/bin/hello", &hello_info);
	if (err) {
		panic("userspace: failed to load /bin/hello from initramfs "
		      "(err=%d)",
		      err);
	}

	pr_info("userspace: /bin/hello ELF64 entry=0x%lx range=0x%lx..0x%lx\n",
		(unsigned long)hello_info.entry, (unsigned long)hello_info.low,
		(unsigned long)hello_info.high);

	err = validate_vfs_exec("/hello", &minix_info);
	if (err) {
		pr_warn("userspace: /hello not executable from MINIX yet "
			"(err=%d)\n",
			err);
	} else {
		pr_info("userspace: /hello MINIX ELF64 entry=0x%lx "
			"range=0x%lx..0x%lx\n",
			(unsigned long)minix_info.entry,
			(unsigned long)minix_info.low,
			(unsigned long)minix_info.high);
	}

	pr_info("userspace: entering ring 3 at 0x%lx stack=0x%lx\n",
		(unsigned long)init_info.entry, (unsigned long)stack);
	err = usermode_enter(init_info.entry, stack);
	panic("userspace: ring-3 entry returned (err=%d)", err);
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
			pr_err("dump: read sector %d failed (%d)\n", sec, err);
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
				(unsigned)(sec * 512 + off), buf[off + 0],
				buf[off + 1], buf[off + 2], buf[off + 3],
				buf[off + 4], buf[off + 5], buf[off + 6],
				buf[off + 7], buf[off + 8], buf[off + 9],
				buf[off + 10], buf[off + 11], buf[off + 12],
				buf[off + 13], buf[off + 14], buf[off + 15]);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Entry                                                                      */
/* ------------------------------------------------------------------------- */

void kernel_main(void); /* author here: not sure why there is a duplicate
			 * kernel_main, investigating this tommorow. FIXME
			 * maybe?(atandant) */
void kernel_main(void)
{
	if (!LIMINE_BASE_REVISION_SUPPORTED) {
		for (;;) {
			__asm__ __volatile__("cli; hlt");
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
	arch_syscall_init();
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

	/* HPET: high-precision reference counter for TSC calibration
	 * and monotonic timing. Optional — falls back to PIT. */
	hpet_init(rsdp_phys, hhdm);

	/* PIT timer: 100 Hz via IOAPIC. Must come before TSC calibration
	 * if we ever switch cpu_calibrate_tsc to use PIT channel 0 IRQs
	 * (currently it uses channel 2 polling, so order is flexible). */
	pit_init();

	/* TSC calibration: uses HPET if available, else PIT channel 2. */
	cpu_calibrate_tsc();

	/* RTC: print wall-clock time at boot. */
	rtc_init();

	bring_up_initramfs();

	sched_init();

	/*
	 * Replace the PIT (channel 0) as the tick source with the LAPIC
	 * timer (jnuspec2.md §2.7). lapic_timer_init() registers the IRQ
	 * handler on VEC_LAPIC_TIMER and arms the timer. Once it returns,
	 * silence the legacy PIT IRQ at the IOAPIC so jiffies stops ticking
	 * and we get exactly one timer source.
	 */
	lapic_timer_init();
	ioapic_mask(0);

	/* Set RSP0 once to the boot stack top so a future user→kernel
	 * trap has a valid kernel stack to switch to. */
	uint64_t rsp_now;
	__asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp_now));
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
		while (vfs_readdir(root_ino, n, &de) == 1)
			n++;
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

	load_userspace_probe();

	pr_info("kernel: boot complete; idle\n");

	struct char_device *kbd = kbd_get_chardev();
	for (;;) {
		if (kbd && kbd->ops->poll(kbd)) {
			char c;
			if (kbd->ops->read(kbd, &c, 1) == 1) {
				pr_info("kbd: typed '%c'\n",
					c >= 0x20 ? c : '.');
			}
		}
		__asm__ __volatile__("sti; hlt; cli");
	}
}
