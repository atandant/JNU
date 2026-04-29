/*
 * kernel/drivers/kbd.c — PS/2 keyboard via i8042 controller.
 *
 * Initializes the i8042 keyboard controller: disable ports, flush,
 * self-test, enable port 1, set scancode set 1, and install an IRQ
 * handler on vector 33 (IOAPIC ISA IRQ 1). Scancodes are translated
 * via a static US-QWERTY layout table into key events queued in a
 * ring buffer.
 *
 * The driver exposes a struct char_device for line-buffered reads
 * (one ASCII byte per key-down of printable keys).
 *
 * Reference: OSDev "8042 PS/2 Controller", IBM AT Technical Reference.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/apic.h>
#include <jnu/chardev.h>
#include <jnu/idt.h>
#include <jnu/io.h>
#include <jnu/kbd.h>
#include <jnu/klog.h>
#include <jnu/types.h>

/* i8042 I/O ports. */
#define KBD_DATA_PORT		0x60
#define KBD_STATUS_PORT		0x64
#define KBD_CMD_PORT		0x64

/* Status register bits. */
#define KBD_STATUS_OBF		0x01
#define KBD_STATUS_IBF		0x02

/* Controller commands. */
#define KBD_CMD_DISABLE_P1	0xAD
#define KBD_CMD_DISABLE_P2	0xA7
#define KBD_CMD_ENABLE_P1	0xAE
#define KBD_CMD_SELF_TEST	0xAA
#define KBD_CMD_READ_CONFIG	0x20
#define KBD_CMD_WRITE_CONFIG	0x60

/* Keyboard commands (sent to data port). */
#define KB_CMD_SET_SCANCODE	0xF0
#define KB_CMD_ENABLE_SCAN	0xF4

#define KBD_SELF_TEST_OK	0x55

/* Ring buffer for decoded ASCII characters. */
#define KBD_RING_SIZE		64

static char kbd_ring[KBD_RING_SIZE];
static volatile size_t kbd_ring_head;
static volatile size_t kbd_ring_tail;

/* Scancode set 1 → ASCII (unshifted). */
static const char sc_unshift[128] = {
	[0x01] = 0x1B,
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
	[0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
	[0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
	[0x0E] = '\b', [0x0F] = '\t',
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
	[0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
	[0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
	[0x1C] = '\n',
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
	[0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
	[0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
	[0x2B] = '\\',
	[0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
	[0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
	[0x34] = '.', [0x35] = '/',
	[0x39] = ' ',
};

/* Scancode set 1 → ASCII (shifted). */
static const char sc_shift[128] = {
	[0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
	[0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
	[0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
	[0x0E] = '\b', [0x0F] = '\t',
	[0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
	[0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
	[0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
	[0x1C] = '\n',
	[0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
	[0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
	[0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
	[0x2B] = '|',
	[0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
	[0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
	[0x34] = '>', [0x35] = '?',
	[0x39] = ' ',
};

/* Modifier state. */
static bool shift_held;
static bool ctrl_held;
static bool caps_on;

#define SC_LSHIFT_MAKE		0x2A
#define SC_RSHIFT_MAKE		0x36
#define SC_LSHIFT_BREAK		0xAA
#define SC_RSHIFT_BREAK		0xB6
#define SC_LCTRL_MAKE		0x1D
#define SC_LCTRL_BREAK		0x9D
#define SC_CAPS_MAKE		0x3A

static void ring_put(char c)
{
	size_t next = (kbd_ring_head + 1) % KBD_RING_SIZE;
	if (next == kbd_ring_tail)
		return;
	kbd_ring[kbd_ring_head] = c;
	kbd_ring_head = next;
}

static void kbd_irq_handler(struct cpu_state *st)
{
	(void)st;
	uint8_t sc = inb(KBD_DATA_PORT);

	switch (sc) {
	case SC_LSHIFT_MAKE: case SC_RSHIFT_MAKE:
		shift_held = true;  goto done;
	case SC_LSHIFT_BREAK: case SC_RSHIFT_BREAK:
		shift_held = false; goto done;
	case SC_LCTRL_MAKE:
		ctrl_held = true;   goto done;
	case SC_LCTRL_BREAK:
		ctrl_held = false;  goto done;
	case SC_CAPS_MAKE:
		caps_on = !caps_on; goto done;
	default:
		break;
	}

	if (sc & 0x80u)
		goto done;

	char c = shift_held ? sc_shift[sc & 0x7Fu]
			    : sc_unshift[sc & 0x7Fu];

	if (caps_on && c >= 'a' && c <= 'z')
		c = (char)(c - 32);

	if (c) {
		if (ctrl_held && c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 1);
		ring_put(c);
		pr_debug("kbd: sc=0x%02x char='%c'\n",
			 (unsigned)sc, (c >= 0x20) ? c : '.');
	}

done:
	apic_eoi();
}

/* ---- i8042 helpers -------------------------------------------------------- */

static void kbd_wait_input(void)
{
	for (int t = 100000; (inb(KBD_STATUS_PORT) & KBD_STATUS_IBF) && t; t--)
		__asm__ __volatile__ ("pause");
}

static void kbd_wait_output(void)
{
	for (int t = 100000; !(inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) && t; t--)
		__asm__ __volatile__ ("pause");
}

static void kbd_flush(void)
{
	while (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF)
		(void)inb(KBD_DATA_PORT);
}

static void kbd_cmd(uint8_t cmd)
{
	kbd_wait_input();
	outb(KBD_CMD_PORT, cmd);
}

static void kbd_data(uint8_t val)
{
	kbd_wait_input();
	outb(KBD_DATA_PORT, val);
}

static uint8_t kbd_read(void)
{
	kbd_wait_output();
	return inb(KBD_DATA_PORT);
}

/* ---- char_device ---------------------------------------------------------- */

static ssize_t kbd_cdev_read(struct char_device *dev, void *buf, size_t len)
{
	(void)dev;
	char *p = buf;
	size_t n = 0;
	while (n < len && kbd_ring_tail != kbd_ring_head) {
		p[n++] = kbd_ring[kbd_ring_tail];
		kbd_ring_tail = (kbd_ring_tail + 1) % KBD_RING_SIZE;
	}
	return (ssize_t)n;
}

static bool kbd_cdev_poll(struct char_device *dev)
{
	(void)dev;
	return kbd_ring_tail != kbd_ring_head;
}

static const struct char_ops kbd_ops = {
	.read = kbd_cdev_read,
	.poll = kbd_cdev_poll,
};

static struct char_device kbd_cdev = {
	.name = "kbd",
	.ops  = &kbd_ops,
};

static bool kbd_ready;

/* ---- public --------------------------------------------------------------- */

void kbd_init(void)
{
	kbd_cmd(KBD_CMD_DISABLE_P1);
	kbd_cmd(KBD_CMD_DISABLE_P2);
	kbd_flush();

	kbd_cmd(KBD_CMD_SELF_TEST);
	uint8_t res = kbd_read();
	if (res != KBD_SELF_TEST_OK)
		pr_warn("kbd: i8042 self-test failed (0x%02x)\n",
			(unsigned)res);

	kbd_cmd(KBD_CMD_READ_CONFIG);
	uint8_t cfg = kbd_read();
	cfg |= 0x01u;
	cfg |= 0x40u; /* Enable translation from set 2 to set 1 */
	cfg &= (uint8_t)~0x10u;
	kbd_cmd(KBD_CMD_WRITE_CONFIG);
	kbd_data(cfg);

	kbd_cmd(KBD_CMD_ENABLE_P1);

	kbd_data(KB_CMD_SET_SCANCODE);
	(void)kbd_read();
	kbd_data(0x02); /* Tell keyboard to use set 2, i8042 translates to set 1 */
	(void)kbd_read();

	kbd_data(KB_CMD_ENABLE_SCAN);
	(void)kbd_read();

	idt_set_handler(VEC_KBD, kbd_irq_handler);
	ioapic_route_isa_irq(1, VEC_KBD);
	ioapic_unmask(1);

	kbd_ready = true;
	pr_info("kbd: PS/2 keyboard initialized (scancode set 1)\n");
}

struct char_device *kbd_get_chardev(void)
{
	return kbd_ready ? &kbd_cdev : NULL;
}
