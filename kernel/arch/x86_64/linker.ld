/*
 * kernel/arch/x86_64/linker.ld — Kernel image layout for Limine.
 *
 * Kernel image lands at 0xFFFFFFFF80000000 (kernel-mcmodel high half).
 * Limine clears .bss for us. The .limine_requests* sections wrap the
 * boot-protocol request structures so Limine can find them.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
	limine_requests	PT_LOAD;
	text		PT_LOAD;
	rodata		PT_LOAD;
	data		PT_LOAD;
}

SECTIONS
{
	. = 0xFFFFFFFF80000000;

	__kernel_start = .;

	.limine_requests : {
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
	} :limine_requests

	. = ALIGN(4K);

	.text : {
		*(.text .text.*)
	} :text

	. = ALIGN(4K);

	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(4K);

	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		*(COMMON)
		*(.bss .bss.*)
	} :data

	. = ALIGN(4K);
	__kernel_end = .;

	/DISCARD/ : {
		*(.eh_frame*)
		*(.note .note.*)
		*(.comment)
	}
}
