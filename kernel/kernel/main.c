/*
 * kernel/kernel/main.c — Phase-1 kernel entry.
 *
 * Boot flow:
 *   1. Verify Limine base revision support; declare requests.
 *   2. Parse the kernel cmdline Limine handed us.
 *   3. Bring up COM1 serial as the first klog backend.
 *   4. Print the version banner.
 *   5. Bring up the framebuffer console.
 *   6. Announce phase-1 boot complete.
 *   7. Idle: `for (;;) hlt`.
 *
 * The Limine boot protocol header lives at boot/limine/limine.h and is
 * vendored via a `git clone` step; the Makefile adds that directory
 * to the include path.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cmdline.h>
#include <jnu/compiler.h>
#include <jnu/fbcon.h>
#include <jnu/klog.h>
#include <jnu/panic.h>
#include <jnu/serial.h>
#include <jnu/string.h>
#include <jnu/types.h>

#include <limine.h>

extern const char jnu_version[];
extern const char jnu_build[];
extern const char jnu_buildtime[];

/* ------------------------------------------------------------------------- */
/* Limine boot-protocol requests                                              */
/* ------------------------------------------------------------------------- */

__used __section(".limine_requests")
static volatile LIMINE_BASE_REVISION(3)

__used __section(".limine_requests")
static volatile struct limine_framebuffer_request fb_request = {
	.id       = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0,
};

__used __section(".limine_requests")
static volatile struct limine_kernel_address_request kaddr_request = {
	.id       = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0,
};

/*
 * The cmdline lives inside the kernel-file response (Limine API
 * revision 0 spelling, which is what the v8.x-binary header uses by
 * default). We pluck `kernel_file->cmdline` in kernel_main.
 */
__used __section(".limine_requests")
static volatile struct limine_kernel_file_request kfile_request = {
	.id       = LIMINE_KERNEL_FILE_REQUEST,
	.revision = 0,
};

__used __section(".limine_requests_start")
static volatile LIMINE_REQUESTS_START_MARKER

__used __section(".limine_requests_end")
static volatile LIMINE_REQUESTS_END_MARKER

/* ------------------------------------------------------------------------- */
/* Boot                                                                      */
/* ------------------------------------------------------------------------- */

static void banner(void)
{
	pr_info("JNU %s (build %s, %s)\n",
		jnu_version, jnu_build, jnu_buildtime);
	pr_info("kernel: phase 1 boot\n");
}

static void bring_up_fbcon(void)
{
	if (fb_request.response == NULL ||
	    fb_request.response->framebuffer_count == 0) {
		pr_warn("fbcon: no framebuffer from Limine; serial-only\n");
		return;
	}

	struct limine_framebuffer *lfb =
		fb_request.response->framebuffers[0];

	struct fbcon_info info = {
		.addr   = lfb->address,
		.width  = lfb->width,
		.height = lfb->height,
		.pitch  = lfb->pitch,
		.bpp    = lfb->bpp,
	};

	int err = fbcon_init(&info);
	if (err) {
		pr_warn("fbcon: init failed (err=%d), serial-only\n", err);
		return;
	}

	pr_info("fbcon: %ux%u, %u bpp, %u cols x %u rows\n",
		(unsigned)lfb->width, (unsigned)lfb->height,
		(unsigned)lfb->bpp,
		(unsigned)(lfb->width / 8),
		(unsigned)(lfb->height / 16));
}

void kernel_main(void);
void kernel_main(void)
{
	if (!LIMINE_BASE_REVISION_SUPPORTED) {
		/*
		 * No klog yet; we have not even brought up serial. Fall
		 * straight into halt — this is unreachable on any modern
		 * Limine build, but the spec demands we check.
		 */
		for (;;) {
			__asm__ __volatile__ ("cli; hlt");
		}
	}

	klog_init();

	const char *cmd = NULL;
	if (kfile_request.response &&
	    kfile_request.response->kernel_file &&
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

	pr_info("kernel: phase 1 boot complete\n");

	if (cmdline_bool("panictest")) {
		panic("panictest requested via cmdline");
	}

	for (;;) {
		__asm__ __volatile__ ("hlt");
	}
}
