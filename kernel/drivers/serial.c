/*
 * kernel/drivers/serial.c — COM1 16550 UART, polling output.
 *
 * Phase-1 driver. Initializes the UART at 0x3F8 to 115200 8N1, no
 * interrupts, no FIFO IRQ — we poll the LSR THRE bit for every byte
 * out. That is enough to make `printk` survive any kernel state, and
 * since panic also writes here we keep it allocation-free and
 * lock-free.
 *
 * Reference: PC16550D datasheet (TI, 1995). Register offsets and bit
 * meanings are from §3 / Table 1 of that document.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/io.h>
#include <jnu/klog.h>
#include <jnu/serial.h>
#include <jnu/spinlock.h>
#include <jnu/string.h>
#include <jnu/types.h>

#define COM1_PORT		0x3F8

/* Register offsets, DLAB=0 unless noted. */
#define UART_THR		0	/* W: transmit holding */
#define UART_RBR		0	/* R: receive buffer  */
#define UART_DLL		0	/* DLAB=1: divisor low */
#define UART_DLH		1	/* DLAB=1: divisor high */
#define UART_IER		1	/* interrupt enable */
#define UART_FCR		2	/* FIFO control (W) */
#define UART_LCR		3	/* line control */
#define UART_MCR		4	/* modem control */
#define UART_LSR		5	/* line status */
#define UART_MSR		6	/* modem status */
#define UART_SCR		7	/* scratch */

#define LCR_8N1			0x03
#define LCR_DLAB		0x80
#define FCR_ENABLE_FIFO		0x01
#define FCR_CLEAR_RX		0x02
#define FCR_CLEAR_TX		0x04
#define FCR_TRIGGER_14		0xC0
#define MCR_DTR			0x01
#define MCR_RTS			0x02
#define MCR_OUT2		0x08	/* required for IRQ delivery later */
#define LSR_THRE		0x20	/* transmit holding empty */



static bool serial_ready;
static struct spinlock serial_lock = SPINLOCK_INITIALIZER;

static void put_byte(uint8_t b)
{
	while ((inb(COM1_PORT + UART_LSR) & LSR_THRE) == 0) {
		__asm__ __volatile__ ("pause");
	}
	outb(COM1_PORT + UART_THR, b);
}

void serial_write(const char *buf, size_t len)
{
	uint64_t flags = spin_lock_irqsave(&serial_lock);
	if (!serial_ready) {
		spin_unlock_irqrestore(&serial_lock, flags);
		return;
	}

	for (size_t i = 0; i < len; i++) {
		uint8_t b = (uint8_t)buf[i];
		if (b == '\n') {
			put_byte('\r');
		}
		put_byte(b);
	}
	spin_unlock_irqrestore(&serial_lock, flags);
}

static struct klog_backend serial_backend = {
	.name  = "com1",
	.flags = KLOG_BACKEND_ANSI,
	.write = serial_write,
};

void serial_init(void)
{
	/* Disable interrupts, set 115200 baud (divisor 1), 8N1. */
	outb(COM1_PORT + UART_IER, 0x00);
	outb(COM1_PORT + UART_LCR, LCR_DLAB);
	outb(COM1_PORT + UART_DLL, 0x01);	/* 115200 / 1 = 115200 */
	outb(COM1_PORT + UART_DLH, 0x00);
	outb(COM1_PORT + UART_LCR, LCR_8N1);

	outb(COM1_PORT + UART_FCR,
	     FCR_ENABLE_FIFO | FCR_CLEAR_RX |
	     FCR_CLEAR_TX | FCR_TRIGGER_14);

	outb(COM1_PORT + UART_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

	serial_ready = true;
	klog_register(&serial_backend);
}
