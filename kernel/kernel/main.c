/*
 * kernel/kernel/main.c — Kernel entry, Phase 3.
 *
 * Boot flow:
 *   1. Verify Limine base revision support; declare requests.
 *   2. Bring up klog and serial; mark boot TSC and calibrate it.
 *   3. Banner.
 *   4. Init framebuffer console.
 *   5. GDT/TSS, IDT, PIC remap+mask, ACPI/MADT/APIC.
 *   6. PMM, paging, VMM, slab.
 *   7. HPET (optional TSC rate refinement).
 *   8. PIT (100 Hz timer via IOAPIC).
 *   9. RTC wall-clock read.
 *  10. Initramfs parse, scheduler init.
 *  11. LAPIC timer takes over as scheduler tick; PIT IRQ masked.
 *  12. PCI enumeration.
 *  13. ATA init + block device registration.
 *  14. Keyboard init.
 *  15. Selftests, gated on `selftest=1`.
 *  16. Optional debug hooks (`panictest=1`, `dump=mem`, `dump=blocks`).
 *  17. Spawn /init as a user task (boot task stays in ring 0).
 *  18. Idle loop (optional kernel kbd echo via kbd=kernel cmdline).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/arch_syscall.h>
#include <jnu/arch/cpu.h>
#include <jnu/arch/gdt.h>
#include <jnu/arch/idt.h>
#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/drivers/acpi.h>
#include <jnu/drivers/apic.h>
#include <jnu/drivers/ata.h>
#include <jnu/drivers/fbcon.h>
#include <jnu/drivers/hpet.h>
#include <jnu/drivers/kbd.h>
#include <jnu/drivers/lapic_timer.h>
#include <jnu/drivers/pci.h>
#include <jnu/drivers/pit.h>
#include <jnu/drivers/rtc.h>
#include <jnu/drivers/serial.h>
#include <jnu/drivers/virtio_blk.h>
#include <jnu/fs/block.h>
#include <jnu/fs/initramfs.h>
#include <jnu/fs/vfs.h>
#include <jnu/kernel/cmdline.h>
#include <jnu/kernel/elf64.h>
#include <jnu/kernel/exec.h>
#include <jnu/kernel/execprot.h>
#include <jnu/kernel/panic.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/kernel/selftest.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/prng.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/paging.h>
#include <jnu/mm/pmm.h>
#include <jnu/mm/slab.h>
#include <jnu/mm/vmm.h>
#include <uapi/jnu/errno.h>

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

/* exec code is split: initfs_exec.c and vfs_exec.c */

static int load_boot_exec(struct addr_space *space, const char *path,
			  struct exec_load_info *info, uint64_t *stack,
			  const char **source)
{
	int err;

	err = load_initramfs_exec(space, path, info, stack);
	if (!err) {
		*source = "initramfs";
		return 0;
	}
	if (err != -ENOENT) {
		return err;
	}

	err = load_vfs_exec(space, path, info, stack);
	if (!err) {
		*source = "vfs";
	}
	return err;
}

static void run_exec_probes(void)
{
	struct exec_load_info hello_info;
	struct exec_load_info minix_info;
	int err;

	err = validate_initramfs_exec("/bin/hello", &hello_info);
	if (err) {
		pr_warn("execprobe: /bin/hello not executable from initramfs "
			"(err=%d)\n",
			err);
	} else {
		pr_info("execprobe: /bin/hello initramfs entry=0x%lx "
			"range=0x%lx..0x%lx\n",
			(unsigned long)hello_info.entry,
			(unsigned long)hello_info.low,
			(unsigned long)hello_info.high);
	}

	err = validate_vfs_exec("/hello", &minix_info);
	if (err) {
		pr_warn("execprobe: /hello not executable from VFS "
			"(err=%d)\n",
			err);
	} else {
		pr_info("execprobe: /hello VFS entry=0x%lx "
			"range=0x%lx..0x%lx\n",
			(unsigned long)minix_info.entry,
			(unsigned long)minix_info.low,
			(unsigned long)minix_info.high);
	}
}

static void start_init(void)
{
	const char *init_path = cmdline_get("init");
	const char *source = NULL;
	struct exec_load_info init_info;
	struct process *proc;
	struct task *init_task;
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

	proc = kzalloc(sizeof(*proc));
	if (!proc) {
		panic("userspace: failed to allocate init process");
	}

	proc->pid = process_alloc_pid();
	if (proc->pid < 0) {
		panic("userspace: failed to allocate init pid (err=%d)",
		      proc->pid);
	}
	proc->state = PROCESS_ALIVE;
	fd_table_init(&proc->fds);

	space = vmm_create_space();
	if (!space) {
		panic("userspace: failed to create init address space");
	}
	proc->space = space;

	err = load_boot_exec(space, init_path, &init_info, &stack, &source);
	if (err) {
		panic("userspace: failed to load init '%s' (err=%d)", init_path,
		      err);
	}
	proc->user_entry = init_info.entry;
	proc->user_stack = stack;
	proc->has_user_frame = false;

	/*
	 * Register init before it becomes runnable so any early exit
	 * reparents onto a valid process instead of a stale pointer.
	 */
	process_set_init(proc);

	pr_info("userspace: init '%s' from %s entry=0x%lx "
		"range=0x%lx..0x%lx\n",
		init_path, source ? source : "unknown",
		(unsigned long)init_info.entry, (unsigned long)init_info.low,
		(unsigned long)init_info.high);

	err = sched_create_user_task("init", proc, &init_task);
	if (err) {
		panic("userspace: failed to schedule init (err=%d)", err);
	}

	pr_info("userspace: init scheduled tid=%d pid=%d\n", init_task->tid,
		proc->pid);
}

static void kernel_idle_loop(void)
{
	for (;;) {
		sched_yield();
		__asm__ __volatile__("sti; hlt; cli");
	}
}

/*
 * Kernel-side keyboard echo for noinit=1 or kbd=kernel debugging.
 * Normal boots read /dev/kbd from userspace instead — both paths share
 * the same char_device ring and must not run concurrently.
 */
static void kbd_echo_loop(void)
{
	struct char_device *kbd = kbd_get_chardev();
	char buf[32];

	if (!kbd || !kbd->ops || !kbd->ops->read) {
		kernel_idle_loop();
	}

	for (;;) {
		if (kbd->ops->poll(kbd)) {
			ssize_t n = kbd->ops->read(kbd, buf, sizeof(buf));

			for (ssize_t i = 0; i < n; i++) {
				char c = buf[i];

				if (c >= 0x20 && c <= 0x7E) {
					pr_info("kbd: typed '%c'\n", c);
				} else {
					pr_info("kbd: typed 0x%02x\n",
						(unsigned char)c);
				}
			}
		}
		sched_yield();
		__asm__ __volatile__("sti; hlt; cli");
	}
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
	struct block_device *bdev = block_lookup("vda");
	if (!bdev)
		bdev = block_lookup("hda");
	if (!bdev) {
		pr_warn("dump: no 'vda' or 'hda' block device\n");
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

void kernel_main(void);
void kernel_main(void)
{
	if (!LIMINE_BASE_REVISION_SUPPORTED) {
		for (;;) {
			__asm__ __volatile__("cli; hlt");
		}
	}

	cpu_mark_boot();

	klog_init();

	const char *cmd = NULL;
	if (kfile_request.response && kfile_request.response->kernel_file &&
	    kfile_request.response->kernel_file->cmdline) {
		cmd = kfile_request.response->kernel_file->cmdline;
	}
	cmdline_parse(cmd);

	serial_init();

	/* CPU bring-up and TSC calibration before banner so klog stamps are live. */
	cpu_init();

	banner();

	if (cmd && cmd[0]) {
		pr_info("cmdline: %s\n", cmd);
	}

	bring_up_fbcon();

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
	if (hpet_available()) {
		cpu_calibrate_tsc();
	}

	/* ACPI power management: parse the FADT for reboot/poweroff and
	 * the PM timer. Optional — reboot falls back to legacy methods. */
	acpi_pm_init();

	/* PIT timer: 100 Hz via IOAPIC. */
	pit_init();

	/* Seed the PRNG from hardware entropy (RDRAND/RDTSC/HPET).
	 * Must run after HPET and TSC calibration are done. */
	prng_seed();

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
	virtio_blk_init();
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

	const char *root_bdev = block_lookup("vda") ? "vda" : "hda";
	if (!block_lookup(root_bdev)) {
		panic("kernel: no root block device (vda/hda)");
	}
	int err = vfs_mount(root_bdev, "minix", "/");
	if (err) {
		panic("kernel: failed to mount rootfs on %s (err=%d)",
		      root_bdev, err);
	}
	pr_info("rootfs: mounted from %s\n", root_bdev);

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

	if (cmdline_bool("execprobe")) {
		run_exec_probes();
	}

	start_init();
	pr_info("kernel: boot complete; idle\n");

	if (cmdline_bool("noinit") ||
	    (cmdline_get("kbd") && strcmp(cmdline_get("kbd"), "kernel") == 0)) {
		kbd_echo_loop();
	}
	kernel_idle_loop();
}
