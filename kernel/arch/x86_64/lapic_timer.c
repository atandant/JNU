/*
 * kernel/arch/x86_64/lapic_timer.c — LAPIC timer scheduler tick.
 *
 * The Local APIC timer is the v0.0.2 scheduler tick (jnuspec2.md §2.7).
 * The 8254 PIT remains for one-shot calibration in cpu.c, but its
 * channel-0 IRQ is silenced by main.c after this module is up.
 *
 * Two modes are supported:
 *
 *   1. TSC-deadline mode, when CPUID.01h:ECX[24] is set. The LVT timer
 *      entry's mode field is programmed to 10b (TSC-deadline). The IRQ
 *      fires when the CPU's TSC reaches the value written to
 *      IA32_TSC_DEADLINE (MSR 0x6E0). The handler re-arms by writing a
 *      fresh deadline of `rdtsc() + tsc_per_tick`. No bus-frequency
 *      calibration is required — TSC frequency was already calibrated
 *      against the PIT in cpu.c. This is the path on QEMU q35 and on
 *      every Nehalem+ host.
 *
 *   2. Periodic mode, on older silicon. The divide configuration is /1
 *      and the initial-count register is loaded with bus_hz / TICK_HZ.
 *      We calibrate bus_hz here against the TSC by arming the timer
 *      masked, busy-waiting a known number of microseconds via TSC,
 *      and reading the current-count register.
 *
 * Either way the IRQ lands on VEC_LAPIC_TIMER and tail-calls
 * sched_tick(). The vector is distinct from VEC_TIMER (PIT) so we can
 * leave the PIT IRQ wired during calibration without conflicts.
 *
 * Reference: Intel SDM Vol. 3 §10.5.4 (LAPIC timer), §10.5.4.1
 * (TSC-deadline mode), §10.6.1 (LVT timer register).
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/apic.h>
#include <jnu/cpu.h>
#include <jnu/errno.h>
#include <jnu/idt.h>
#include <jnu/klog.h>
#include <jnu/lapic_timer.h>
#include <jnu/panic.h>
#include <jnu/sched.h>
#include <jnu/types.h>

/* LAPIC register offsets (bytes from MMIO base). */
#define LAPIC_REG_LVT_TIMER		0x320
#define LAPIC_REG_INIT_COUNT		0x380
#define LAPIC_REG_CURRENT_COUNT		0x390
#define LAPIC_REG_DIVIDE_CONFIG		0x3E0

/* LVT timer fields. */
#define LVT_VECTOR_MASK			0xFFu
#define LVT_DELIVERY_STATUS		(1u << 12)
#define LVT_MASKED			(1u << 16)
#define LVT_TIMER_MODE_ONESHOT		(0u << 17)
#define LVT_TIMER_MODE_PERIODIC		(1u << 17)
#define LVT_TIMER_MODE_TSC_DEADLINE	(2u << 17)
#define LVT_TIMER_MODE_MASK		(3u << 17)

/*
 * Divide configuration register encoding.  The architectural format
 * splits the divisor across bits {3,1,0}; 0xB = divide-by-1, which we
 * always use to keep math simple.
 */
#define LAPIC_DCR_DIV_1			0xBu

/* IA32_TSC_DEADLINE MSR. */
#define MSR_IA32_TSC_DEADLINE		0x6E0u

/* CPUID.01h:ECX bit 24 = TSC-deadline supported. */
#define CPUID_01_ECX_TSC_DEADLINE	(1u << 24)

/* Calibration window for periodic mode (microseconds). */
#define LAPIC_CAL_WINDOW_US		10000u

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

static bool		use_deadline;
static uint32_t		bus_init_count;	/* periodic-mode reload value */
static uint64_t		tsc_per_tick;	/* deadline-mode increment */
static volatile uint64_t timer_ticks;

/* ------------------------------------------------------------------------- */
/* Low-level helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline volatile uint32_t *lapic_reg(uint32_t off)
{
	return lapic_mmio_base() + (off / 4);
}

static inline uint32_t lapic_read(uint32_t off)
{
	return *lapic_reg(off);
}

static inline void lapic_write(uint32_t off, uint32_t val)
{
	*lapic_reg(off) = val;
}

static bool cpu_has_tsc_deadline(void)
{
	uint32_t a, b, c, d;
	__asm__ __volatile__ ("cpuid"
			      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			      : "a"(1u), "c"(0u));
	return (c & CPUID_01_ECX_TSC_DEADLINE) != 0;
}

/*
 * TSC-based busy-wait. Used during calibration so we do not depend on
 * any IRQ source. Requires cpu_calibrate_tsc() has already run.
 */
static void tsc_busy_wait_us(uint32_t us)
{
	uint64_t per_us = cpu_current()->tsc_per_us;
	uint64_t target = cpu_rdtsc() + (uint64_t)us * per_us;
	while (cpu_rdtsc() < target) {
		__asm__ __volatile__ ("pause");
	}
}

/* ------------------------------------------------------------------------- */
/* IRQ handler                                                                */
/* ------------------------------------------------------------------------- */

static void lapic_timer_irq(struct cpu_state *st)
{
	(void)st;

	timer_ticks++;

	/*
	 * In TSC-deadline mode the timer is one-shot: re-arm before EOI
	 * so the next deadline is queued even if the handler is preempted
	 * by another IRQ on EOI.
	 */
	if (use_deadline) {
		wrmsr(MSR_IA32_TSC_DEADLINE, cpu_rdtsc() + tsc_per_tick);
	}

	apic_eoi();
	sched_tick();
}

/* ------------------------------------------------------------------------- */
/* Calibration (periodic mode only)                                           */
/* ------------------------------------------------------------------------- */

/*
 * Calibrate the LAPIC bus frequency against the TSC. The LAPIC counter
 * decrements every (bus_clock / divisor) ticks; with divisor = 1 the
 * delta over a known-time window is the bus frequency itself.
 *
 * Returns the periodic-mode initial-count value (bus_hz / TICK_HZ).
 * Panics on a degenerate calibration result.
 */
static uint32_t calibrate_periodic(void)
{
	const uint32_t start = 0xFFFFFFFFu;

	lapic_write(LAPIC_REG_DIVIDE_CONFIG, LAPIC_DCR_DIV_1);
	/* Mask the LVT entry while we measure: no IRQ during calibration. */
	lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED | LVT_TIMER_MODE_ONESHOT);
	lapic_write(LAPIC_REG_INIT_COUNT, start);

	tsc_busy_wait_us(LAPIC_CAL_WINDOW_US);

	uint32_t cur = lapic_read(LAPIC_REG_CURRENT_COUNT);
	/* Stop the timer before we touch anything else. */
	lapic_write(LAPIC_REG_INIT_COUNT, 0);

	if (cur >= start) {
		panic("lapic_timer: calibration counter did not move");
	}

	uint32_t elapsed = start - cur;
	/*
	 * elapsed counts in 10 ms (LAPIC_CAL_WINDOW_US). Per-tick reload =
	 *     elapsed * (10 ms / tick_period) = elapsed * (HZ * 10 ms / s)
	 *
	 * For HZ = 100 and window = 10 ms that is exactly elapsed.
	 * Compute it the long way so changing either constant stays
	 * correct.
	 */
	uint64_t reload = (uint64_t)elapsed * 1000000ull /
			  ((uint64_t)LAPIC_CAL_WINDOW_US *
			   (uint64_t)LAPIC_TIMER_HZ);
	if (reload == 0 || reload > 0xFFFFFFFFull) {
		panic("lapic_timer: implausible reload value");
	}

	uint64_t bus_hz = (uint64_t)elapsed * 1000000ull /
			  (uint64_t)LAPIC_CAL_WINDOW_US;
	pr_info("lapic_timer: bus ~%lu MHz, reload=%lu (periodic, %u Hz)\n",
		(unsigned long)(bus_hz / 1000000ull),
		(unsigned long)reload,
		(unsigned)LAPIC_TIMER_HZ);

	return (uint32_t)reload;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void lapic_timer_init(void)
{
	if (!lapic_mmio_base()) {
		pr_err("lapic_timer: LAPIC MMIO not mapped; cannot init\n");
		return;
	}
	if (cpu_current()->tsc_per_us == 0) {
		pr_err("lapic_timer: TSC not calibrated; cannot init\n");
		return;
	}

	idt_set_handler(VEC_LAPIC_TIMER, lapic_timer_irq);

	use_deadline = cpu_has_tsc_deadline();

	if (use_deadline) {
		/*
		 * TSC-deadline path: program the LVT entry once and arm
		 * the first deadline. The mode bits in the LVT must be
		 * set before writing the deadline MSR (SDM §10.5.4.1).
		 */
		lapic_write(LAPIC_REG_LVT_TIMER,
			    LVT_TIMER_MODE_TSC_DEADLINE | VEC_LAPIC_TIMER);

		/* Serialize: SDM requires an mfence between the LVT
		 * update and the first IA32_TSC_DEADLINE write. */
		__asm__ __volatile__ ("mfence" ::: "memory");

		uint64_t per_us = cpu_current()->tsc_per_us;
		tsc_per_tick = per_us * (1000000ull / LAPIC_TIMER_HZ);
		wrmsr(MSR_IA32_TSC_DEADLINE,
		      cpu_rdtsc() + tsc_per_tick);

		pr_info("lapic_timer: TSC-deadline mode, "
			"%lu TSC/tick, %u Hz\n",
			(unsigned long)tsc_per_tick,
			(unsigned)LAPIC_TIMER_HZ);
	} else {
		bus_init_count = calibrate_periodic();
		lapic_write(LAPIC_REG_DIVIDE_CONFIG, LAPIC_DCR_DIV_1);
		lapic_write(LAPIC_REG_LVT_TIMER,
			    LVT_TIMER_MODE_PERIODIC | VEC_LAPIC_TIMER);
		lapic_write(LAPIC_REG_INIT_COUNT, bus_init_count);
	}
}

bool lapic_timer_is_deadline_mode(void)
{
	return use_deadline;
}

uint64_t lapic_timer_ticks(void)
{
	return timer_ticks;
}

int lapic_timer_selftest(void)
{
	if (!lapic_mmio_base()) {
		return -ENODEV;
	}

	/*
	 * Enable interrupts briefly and confirm the tick counter
	 * advances. Use a TSC-based deadline so we are not at the mercy
	 * of any other timer source. Hard cap of 50 ms (5 ticks at
	 * 100 Hz) is plenty.
	 */
	uint64_t before = timer_ticks;
	uint64_t per_us = cpu_current()->tsc_per_us;
	uint64_t deadline = cpu_rdtsc() + 50000ull * per_us;

	__asm__ __volatile__ ("sti");
	while (timer_ticks - before < 2 && cpu_rdtsc() < deadline) {
		__asm__ __volatile__ ("pause");
	}
	__asm__ __volatile__ ("cli");

	if (timer_ticks - before < 2) {
		pr_err("lapic_timer: selftest saw only %lu tick(s) in 50 ms\n",
		       (unsigned long)(timer_ticks - before));
		return -EIO;
	}

	pr_info("lapic_timer: selftest %lu ticks, mode=%s\n",
		(unsigned long)(timer_ticks - before),
		use_deadline ? "tsc-deadline" : "periodic");
	return 0;
}
