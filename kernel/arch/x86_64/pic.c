/*
 * kernel/arch/x86_64/pic.c — Legacy 8259 PIC remap and mask.
 *
 * The 8259 fires vectors 0x08–0x0F by default, which collide with our
 * architectural exceptions. We remap to 0x20–0x2F, then mask every
 * line. Once masked the PIC stays out of the way for the lifetime of
 * the kernel; LAPIC + IOAPIC are the live controllers from this point
 * on (§2.4).
 *
 * Reference: i8259A datasheet, Intel Multiprocessor Specification §3.6.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/io.h>
#include <jnu/klog.h>
#include <jnu/types.h>

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_remap_and_mask(void);

void pic_remap_and_mask(void)
{
	outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();
	outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();

	outb(PIC1_DATA, 0x20);
	io_wait(); /* master vec base */
	outb(PIC2_DATA, 0x28);
	io_wait(); /* slave vec base */

	outb(PIC1_DATA, 0x04);
	io_wait(); /* slave on IRQ2 */
	outb(PIC2_DATA, 0x02);
	io_wait();

	outb(PIC1_DATA, ICW4_8086);
	io_wait();
	outb(PIC2_DATA, ICW4_8086);
	io_wait();

	/* Mask everything. */
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);

	pr_info("pic: 8259 remapped to 0x20-0x2F and fully masked\n");
}
