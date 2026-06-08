/*
 * kernel/drivers/pit.c — 8254 PIT timer, 100 Hz on channel 0.
 *
 * Programs PIT channel 0 in mode 2 (rate generator) at ~100 Hz and
 * installs an IRQ handler on vector 32 via the IOAPIC. Each tick
 * increments a 64-bit jiffies counter used as a coarse monotonic
 * clock. The PIT is also used for TSC calibration in cpu.c (which
 * uses channel 2 independently).
 *
 * In v0.0.2 the LAPIC timer replaces this as the tick source and the
 * PIT channel 0 is silenced.
 *
 * Reference: Intel 8254 datasheet §2 (mode register), §3 (mode 2).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/idt.h>
#include <jnu/base/types.h>
#include <jnu/drivers/apic.h>
#include <jnu/drivers/io.h>
#include <jnu/drivers/pit.h>
#include <jnu/lib/klog.h>

/* 8254 PIT oscillator frequency: 1.193182 MHz. */
#define PIT_BASE_HZ 1193182u

/* I/O ports. */
#define PIT_CH0_DATA 0x40
#define PIT_CMD 0x43

/*
 * Reload value for ~100 Hz:
 * 1193182 / 100 = 11931 (0x2E9B), giving ~100.006 Hz — close enough.
 */
#define PIT_RELOAD (PIT_BASE_HZ / PIT_FREQUENCY_HZ)

static volatile uint64_t jiffies;

static void pit_irq_handler(struct cpu_state *st)
{
	(void)st;
	jiffies++;
	apic_eoi();
}

void pit_init(void)
{
	/*
	 * Channel 0, access mode lobyte/hibyte, mode 2 (rate generator),
	 * binary counting.
	 * Command byte: 00 11 010 0 = 0x34.
	 */
	outb(PIT_CMD, 0x34);
	outb(PIT_CH0_DATA, (uint8_t)(PIT_RELOAD & 0xFFu));
	outb(PIT_CH0_DATA, (uint8_t)((PIT_RELOAD >> 8) & 0xFFu));

	/* Route ISA IRQ 0 (timer) to vector 32 via the IOAPIC. */
	idt_set_handler(VEC_TIMER, pit_irq_handler);
	ioapic_route_isa_irq(0, VEC_TIMER);
	ioapic_unmask(0);

	pr_info("pit: channel 0 at %u Hz (reload=%u)\n",
		(unsigned)PIT_FREQUENCY_HZ, (unsigned)PIT_RELOAD);
}

uint64_t pit_get_ticks(void) { return jiffies; }

void pit_sleep_ms(uint32_t ms)
{
	uint64_t target =
	    jiffies + ((uint64_t)ms * PIT_FREQUENCY_HZ + 999u) / 1000u;
	while (jiffies < target) {
		__asm__ __volatile__("sti; hlt; cli");
	}
}
