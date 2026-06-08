/*
 * kernel/arch/x86_64/gdt.c — GDT, TSS, and IST stack setup.
 *
 * Builds a 7-entry GDT (null, kCS, kDS, uDS, uCS, TSS-low, TSS-high)
 * with the order required by future SYSRET, allocates the TSS, allocates
 * seven IST stacks of 16 KiB each, fills the IST array, and loads the
 * GDT and TSS. RSP0 is set to a sentinel; the boot stack is wired in
 * Phase 2 once `kernel_main` knows it.
 *
 * Reference: Intel SDM Vol. 3 §3.4.5 (descriptors), §7.2 (TSS in long
 * mode), §6.14.5 (interrupt stack table).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/gdt.h>
#include <jnu/base/compiler.h>
#include <jnu/base/types.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/string.h>

/* ------------------------------------------------------------------------- */
/* Descriptor encodings                                                      */
/* ------------------------------------------------------------------------- */

/*
 * We intentionally hand-encode descriptors instead of bitfields. The
 * encoding follows Intel SDM Vol. 3 §3.4.5 for code/data descriptors and
 * §7.2.3 for the 64-bit TSS descriptor.
 */

#define GDT_ACCESS_PRESENT (1u << 7)
#define GDT_ACCESS_RING0 (0u << 5)
#define GDT_ACCESS_RING3 (3u << 5)
#define GDT_ACCESS_S (1u << 4) /* code/data, not system */
#define GDT_ACCESS_EXEC (1u << 3)
#define GDT_ACCESS_RW (1u << 1)

#define GDT_FLAGS_LONG (1u << 1) /* L = 1 for 64-bit code */
#define GDT_FLAGS_GRAN_4K (1u << 3)

/* Long-mode 64-bit TSS available type. */
#define TSS_TYPE_AVAIL64 0x9

struct __packed gdt_entry {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t base_mid;
	uint8_t access;
	uint8_t flags_limit_hi;
	uint8_t base_hi;
};

struct __packed gdt_tss_entry {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t base_mid1;
	uint8_t access;
	uint8_t flags_limit_hi;
	uint8_t base_mid2;
	uint32_t base_hi;
	uint32_t zero;
};

struct __packed tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb;
};

struct __packed gdt_descriptor {
	uint16_t limit;
	uint64_t base;
};

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

#define GDT_ENTRIES 7 /* indices 0..4 normal, 5..6 TSS-low/high */

static struct gdt_entry gdt[GDT_ENTRIES] __aligned(16);
static struct tss64 tss __aligned(16);

#define IST_STACK_SIZE (16 * 1024)
static uint8_t ist_stacks[7][IST_STACK_SIZE] __aligned(16);

/* Compile-time sanity on the selector layout for future SYSRET. */
_Static_assert(GDT_KERNEL_CS == 0x08, "kCS slot");
_Static_assert(GDT_KERNEL_DS == 0x10, "kDS slot");
_Static_assert(GDT_USER_DS == 0x18, "uDS slot");
_Static_assert(GDT_USER_CS == 0x20, "uCS slot");
_Static_assert(GDT_TSS == 0x28, "TSS slot");

static void set_segment(struct gdt_entry *e, uint8_t access, uint8_t flags)
{
	e->limit_lo = 0xFFFF;
	e->base_lo = 0;
	e->base_mid = 0;
	e->access = access;
	e->flags_limit_hi = (uint8_t)((flags << 4) | 0x0F);
	e->base_hi = 0;
}

static void set_tss_entry(struct gdt_tss_entry *e, uint64_t base,
			  uint32_t limit)
{
	uint8_t access = (uint8_t)(GDT_ACCESS_PRESENT | TSS_TYPE_AVAIL64);

	e->limit_lo = (uint16_t)(limit & 0xFFFF);
	e->base_lo = (uint16_t)(base & 0xFFFF);
	e->base_mid1 = (uint8_t)((base >> 16) & 0xFF);
	e->access = access;
	e->flags_limit_hi = (uint8_t)((limit >> 16) & 0x0F);
	e->base_mid2 = (uint8_t)((base >> 24) & 0xFF);
	e->base_hi = (uint32_t)(base >> 32);
	e->zero = 0;
}

void tss_set_rsp0(uint64_t rsp0) { tss.rsp0 = rsp0; }

void gdt_init(void)
{
	memset(gdt, 0, sizeof(gdt));

	/* Slot 0: null. Already zeroed. */

	/* Slot 1: kernel CS, ring 0, long-mode code. */
	set_segment(&gdt[1],
		    (uint8_t)(GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
			      GDT_ACCESS_S | GDT_ACCESS_EXEC | GDT_ACCESS_RW),
		    GDT_FLAGS_LONG);

	/* Slot 2: kernel DS, ring 0, data. */
	set_segment(&gdt[2],
		    (uint8_t)(GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
			      GDT_ACCESS_S | GDT_ACCESS_RW),
		    GDT_FLAGS_GRAN_4K);

	/* Slot 3: user DS, ring 3, data. */
	set_segment(&gdt[3],
		    (uint8_t)(GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
			      GDT_ACCESS_S | GDT_ACCESS_RW),
		    GDT_FLAGS_GRAN_4K);

	/* Slot 4: user CS, ring 3, long-mode code. */
	set_segment(&gdt[4],
		    (uint8_t)(GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
			      GDT_ACCESS_S | GDT_ACCESS_EXEC | GDT_ACCESS_RW),
		    GDT_FLAGS_LONG);

	/* Slots 5/6: 16-byte TSS descriptor. */
	memset(&tss, 0, sizeof(tss));
	tss.iopb = sizeof(tss); /* I/O permission bitmap absent */
	tss.rsp0 = 0;		/* set later by tss_set_rsp0 */

	for (size_t i = 0; i < 7; i++) {
		uintptr_t top = (uintptr_t)&ist_stacks[i][IST_STACK_SIZE];
		top &= ~0xFull; /* keep IST entries 16-byte aligned */
		tss.ist[i] = (uint64_t)top;
	}

	struct gdt_tss_entry *te = (struct gdt_tss_entry *)&gdt[5];
	set_tss_entry(te, (uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);

	struct gdt_descriptor gdtr = {
	    .limit = sizeof(gdt) - 1,
	    .base = (uint64_t)(uintptr_t)gdt,
	};

	__asm__ __volatile__("lgdt %0\n\t"
			     "mov $0x10, %%ax\n\t"
			     "mov %%ax, %%ds\n\t"
			     "mov %%ax, %%es\n\t"
			     "mov %%ax, %%fs\n\t"
			     "mov %%ax, %%gs\n\t"
			     "mov %%ax, %%ss\n\t"
			     "pushq $0x08\n\t"
			     "leaq 1f(%%rip), %%rax\n\t"
			     "pushq %%rax\n\t"
			     "lretq\n\t"
			     "1:\n\t"
			     :
			     : "m"(gdtr)
			     : "rax", "memory");

	__asm__ __volatile__("ltr %%ax" ::"a"((uint16_t)GDT_TSS) : "memory");

	pr_info("gdt: 7 entries, TSS at %p, 7 IST stacks of %u KiB\n",
		(void *)&tss, (unsigned)(IST_STACK_SIZE / 1024));
}
