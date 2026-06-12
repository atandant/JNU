/*
 * kernel/drivers/kbd.c — PS/2 keyboard via i8042 controller.
 *
 * Initializes the i8042 keyboard controller: disable ports, flush,
 * self-test, enable port 1, set scancode set 1, and install an IRQ
 * handler on vector 33 (IOAPIC ISA IRQ 1).
 *
 * Raw scancodes are translated to keycodes via the hardware-agnostic
 * scandata layer.  Every make/break is queued as a struct key_event;
 * ASCII for userspace is produced on read() via scandata_keycode_to_ascii().
 * The 0xE0 extended-key prefix and 0xE1 Pause sequence are tracked so
 * navigation keys, Win keys, and Pause decode correctly.
 *
 * The driver exposes a struct char_device for byte-stream reads.
 * Printable keys yield one ASCII byte; Left-Alt (Meta) combos yield
 * ESC + base character (GNU/xterm style).  Super/Win and navigation
 * keys produce no output on read().
 *
 * Reference: OSDev "8042 PS/2 Controller", IBM AT Technical Reference.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/arch/idt.h>
#include <jnu/base/types.h>
#include <jnu/drivers/apic.h>
#include <jnu/drivers/chardev.h>
#include <jnu/drivers/io.h>
#include <jnu/drivers/kbd.h>
#include <jnu/drivers/keycode.h>
#include <jnu/drivers/scandata.h>
#include <jnu/kernel/cmdline.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/spinlock.h>
#include <jnu/lib/string.h>

/* i8042 I/O ports. */
#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_CMD_PORT 0x64

/* Status register bits. */
#define KBD_STATUS_OBF 0x01
#define KBD_STATUS_IBF 0x02

/* Controller commands. */
#define KBD_CMD_DISABLE_P1 0xAD
#define KBD_CMD_DISABLE_P2 0xA7
#define KBD_CMD_ENABLE_P1 0xAE
#define KBD_CMD_SELF_TEST 0xAA
#define KBD_CMD_READ_CONFIG 0x20
#define KBD_CMD_WRITE_CONFIG 0x60

/* Keyboard commands (sent to data port). */
#define KB_CMD_SET_SCANCODE 0xF0
#define KB_CMD_ENABLE_SCAN 0xF4
#define KB_CMD_SET_LEDS 0xED
#define KB_CMD_IDENTIFY 0xF2

#define KBD_LED_SCROLL (1u << 0)
#define KBD_LED_NUMLOCK (1u << 1)
#define KBD_LED_CAPS (1u << 2)

/* Keyboard responses (received from data port). */
#define KB_RESP_ACK 0xFA
#define KB_RESP_RESEND 0xFE
#define KB_MAX_RESEND 3

#define KBD_SELF_TEST_OK 0x55

/* Pause key set-1 sequence: E1 1D 45 E1 9D C5 */
#define E1_SEQ_LEN 6
static const uint8_t e1_pause_seq[E1_SEQ_LEN] = {0xE1, 0x1D, 0x45,
						 0xE1, 0x9D, 0xC5};

/* ------------------------------------------------------------------ */
/* key_event ring buffer                                               */
/* ------------------------------------------------------------------ */

#define KBD_RING_SIZE 128

static struct key_event kbd_ring[KBD_RING_SIZE];
static size_t kbd_ring_head;
static size_t kbd_ring_tail;
static struct spinlock kbd_lock = SPINLOCK_INITIALIZER;
static unsigned int kbd_ring_drops;

static void event_put(uint16_t keycode, uint8_t modifiers, bool release)
{
	uint64_t flags = spin_lock_irqsave(&kbd_lock);
	size_t next = (kbd_ring_head + 1) % KBD_RING_SIZE;

	if (next != kbd_ring_tail) {
		kbd_ring[kbd_ring_head].keycode = keycode;
		kbd_ring[kbd_ring_head].modifiers = modifiers;
		kbd_ring[kbd_ring_head].type =
		    release ? KEY_EV_RELEASE : KEY_EV_PRESS;
		kbd_ring[kbd_ring_head].ascii = 0;
		kbd_ring_head = next;
	} else {
		kbd_ring_drops++;
		if ((kbd_ring_drops & 0x3Fu) == 1u) {
			pr_warn("kbd: event ring overflow (drops=%u)\n",
				kbd_ring_drops);
		}
	}
	spin_unlock_irqrestore(&kbd_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Modifier state and prefix tracking                                  */
/* ------------------------------------------------------------------ */

static bool shift_held;
static bool ctrl_held;
static bool lalt_held;
static bool ralt_held;
static bool super_held;
static bool caps_on;
static bool numlock_on;
static bool kbd_has_numpad = true;
static bool kbd_leds_dirty;
static bool e0_pending;
static uint8_t e1_state;

static void kbd_sync_leds(void);
static void kbd_request_led_sync(void);
static void kbd_sync_leds_if_needed(void);

static uint8_t current_modifiers(void)
{
	uint8_t mods = 0;

	if (shift_held) {
		mods |= KMOD_SHIFT;
	}
	if (ctrl_held) {
		mods |= KMOD_CTRL;
	}
	if (lalt_held) {
		mods |= KMOD_ALT;
	}
	if (ralt_held) {
		mods |= KMOD_ALTGR;
	}
	if (super_held) {
		mods |= KMOD_SUPER;
	}
	if (caps_on) {
		mods |= KMOD_CAPSLOCK;
	}
	if (numlock_on) {
		mods |= KMOD_NUMLOCK;
	}
	return mods;
}

static void kbd_update_modifiers(uint16_t keycode, bool release)
{
	switch (keycode) {
	case KEY_LSHIFT:
	case KEY_RSHIFT:
		shift_held = !release;
		break;
	case KEY_LCTRL:
	case KEY_RCTRL:
		ctrl_held = !release;
		break;
	case KEY_LALT:
		lalt_held = !release;
		break;
	case KEY_RALT:
		ralt_held = !release;
		break;
	case KEY_LGUI:
	case KEY_RGUI:
		super_held = !release;
		break;
	case KEY_CAPSLOCK:
		if (!release) {
			caps_on = !caps_on;
			kbd_request_led_sync();
		}
		break;
	case KEY_NUMLOCK:
		if (!release) {
			numlock_on = !numlock_on;
			kbd_request_led_sync();
		}
		break;
	default:
		break;
	}
}

static void kbd_deliver_keycode(uint16_t keycode, bool release)
{
	uint8_t mods;

	if (keycode == KEY_NONE) {
		return;
	}

	mods = current_modifiers();
	event_put(keycode, mods, release);
	kbd_update_modifiers(keycode, release);
}

/*
 * Feed one raw scancode byte (after prefix handling).  `is_e0` is true
 * when this byte immediately followed a 0xE0 prefix.
 */
static void kbd_feed_scancode(uint8_t sc, bool is_e0)
{
	uint16_t keycode;
	bool release;

	release = (sc & 0x80u) != 0;
	keycode = scandata_sc1_to_keycode(sc, is_e0);
	kbd_deliver_keycode(keycode, release);
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                         */
/* ------------------------------------------------------------------ */

static void kbd_irq_handler(struct cpu_state *st)
{
	uint8_t sc;

	(void)st;

	sc = inb(KBD_DATA_PORT);

	/* 0xE0 prefix: wait for the real scancode byte. */
	if (sc == 0xE0) {
		e0_pending = true;
		e1_state = 0;
		goto done;
	}

	/* Collect Pause sequence bytes (E1 1D 45 E1 9D C5) before decode. */
	if (e1_state > 0) {
		if (sc == e1_pause_seq[e1_state]) {
			e1_state++;
			if (e1_state == E1_SEQ_LEN) {
				kbd_deliver_keycode(KEY_PAUSE, false);
				e1_state = 0;
			}
			goto done;
		}
		e1_state = 0;
		/* Fall through and try to decode this byte normally. */
	}

	/* 0xE1 begins the Pause sequence. */
	if (sc == 0xE1) {
		e0_pending = false;
		e1_state = 1;
		goto done;
	}

	{
		bool is_e0 = e0_pending;

		e0_pending = false;
		kbd_feed_scancode(sc, is_e0);
	}

done:
	apic_eoi();
}

/* ------------------------------------------------------------------ */
/* i8042 helpers                                                       */
/* ------------------------------------------------------------------ */

static void kbd_wait_input(void)
{
	for (int t = 100000; (inb(KBD_STATUS_PORT) & KBD_STATUS_IBF) && t; t--)
		__asm__ __volatile__("pause");
}

static bool kbd_wait_output(void)
{
	for (int t = 100000; t; t--) {
		if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF)
			return true;
		__asm__ __volatile__("pause");
	}
	return false;
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

/*
 * Read one byte from the data port, bounded by a timeout. Returns true
 * and stores the byte in *out on success; returns false (and leaves
 * *out untouched) if no byte arrived, so callers never act on stale
 * data left in the port from an earlier transaction.
 */
static bool kbd_read_response(uint8_t *out)
{
	if (!kbd_wait_output())
		return false;
	*out = inb(KBD_DATA_PORT);
	return true;
}

/*
 * Bounded wait for an optional trailing response byte (e.g. the second
 * byte of a keyboard identify reply).  Returns false on timeout.
 */
static bool kbd_try_read_response(uint8_t *out, int spins)
{
	for (int t = spins; t; t--) {
		if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
			*out = inb(KBD_DATA_PORT);
			return true;
		}
		__asm__ __volatile__("pause");
	}
	return false;
}

/*
 * Send a byte to the keyboard (i8042 port 1 device) and wait for its
 * ACK. The keyboard answers every byte with 0xFA (ACK) or 0xFE
 * (RESEND); on RESEND we retransmit up to KB_MAX_RESEND times. Returns
 * true on ACK, false on timeout, an unexpected response, or too many
 * resends — so a wedged keyboard cannot leave init believing a mode
 * change succeeded.
 */
static bool kbd_send(uint8_t val)
{
	for (int tries = 0; tries < KB_MAX_RESEND; tries++) {
		uint8_t resp;

		kbd_data(val);
		if (!kbd_read_response(&resp))
			return false;
		if (resp == KB_RESP_ACK)
			return true;
		if (resp != KB_RESP_RESEND)
			return false;
	}
	return false;
}

static void kbd_sync_leds(void)
{
	uint8_t leds = 0;

	if (numlock_on) {
		leds |= KBD_LED_NUMLOCK;
	}
	if (caps_on) {
		leds |= KBD_LED_CAPS;
	}
	if (!kbd_send(KB_CMD_SET_LEDS) || !kbd_send(leds)) {
		pr_warn("kbd: failed to set keyboard LEDs\n");
	}
}

static void kbd_request_led_sync(void) { kbd_leds_dirty = true; }

static void kbd_sync_leds_if_needed(void)
{
	if (!kbd_leds_dirty) {
		return;
	}
	kbd_sync_leds();
	kbd_leds_dirty = false;
}

/*
 * PS/2 identify (0xF2): 0xAB alone → 84-key AT (no numpad); 0xAB 0x83 or
 * 0xAB 0x41 → MF2 101/102-key (numpad region in layout).  TKL/laptop
 * keyboards may still report MF2; numlock= cmdline overrides defaults.
 */
static void kbd_probe_layout(void)
{
	uint8_t id0 = 0;
	uint8_t id1 = 0;

	kbd_has_numpad = true;

	if (!kbd_send(KB_CMD_IDENTIFY)) {
		pr_warn("kbd: keyboard identify failed (no ACK)\n");
		return;
	}

	if (!kbd_read_response(&id0)) {
		pr_warn("kbd: keyboard identify timed out\n");
		return;
	}

	if (id0 == 0xAB && kbd_try_read_response(&id1, 100000)) {
		if (id1 == 0x83 || id1 == 0x41) {
			kbd_has_numpad = true;
			pr_info(
			    "kbd: MF2 keyboard (ID AB %02x), numpad present\n",
			    (unsigned)id1);
		} else {
			kbd_has_numpad = true;
			pr_info("kbd: keyboard ID AB %02x (treating as MF2)\n",
				(unsigned)id1);
		}
	} else if (id0 == 0xAB) {
		kbd_has_numpad = false;
		pr_info("kbd: AT keyboard (ID AB), no numpad\n");
	} else {
		pr_info("kbd: unknown keyboard ID 0x%02x (assuming numpad)\n",
			(unsigned)id0);
	}

	kbd_flush();
}

static void kbd_apply_numlock_boot_state(void)
{
	const char *v = cmdline_get("numlock");

	if (v && strcmp(v, "off") == 0) {
		numlock_on = false;
	} else if (v && strcmp(v, "on") == 0) {
		numlock_on = true;
	} else {
		numlock_on = kbd_has_numpad;
	}

	pr_info("kbd: Num Lock %s (numpad %s)\n", numlock_on ? "on" : "off",
		kbd_has_numpad ? "present" : "absent");
}

/*
 * Try to encode one key_event into out[out_off..out_cap).  Returns true
 * when the event is consumed (dequeue it); false when out_cap is too
 * small for a multi-byte sequence (leave the event queued).
 */
static bool kbd_try_emit_event(const struct key_event *ev, char *out,
			       size_t out_cap, size_t out_off, size_t *written)
{
	char c;
	char meta_c;

	*written = 0;

	if (ev->type != KEY_EV_PRESS) {
		return true;
	}

	if ((ev->modifiers & KMOD_ALT) && !(ev->modifiers & KMOD_ALTGR) &&
	    !(ev->modifiers & KMOD_CTRL) && !(ev->modifiers & KMOD_SUPER)) {
		meta_c = scandata_keycode_to_ascii_unmodified(ev->keycode,
							      ev->modifiers);
		if (!meta_c) {
			return true;
		}
		if (out_off + 2 > out_cap) {
			return false;
		}
		out[out_off] = '\x1b';
		out[out_off + 1] = meta_c;
		*written = 2;
		return true;
	}

	c = scandata_keycode_to_ascii(ev->keycode, ev->modifiers);
	if (!c) {
		return true;
	}
	if (out_off + 1 > out_cap) {
		return false;
	}
	out[out_off] = c;
	*written = 1;
	return true;
}

/* ------------------------------------------------------------------ */
/* char_device interface                                               */
/* ------------------------------------------------------------------ */

static ssize_t kbd_cdev_read(struct char_device *dev, void *buf, size_t len)
{
	char *p = buf;
	size_t n = 0;

	(void)dev;

	kbd_sync_leds_if_needed();

	while (n < len) {
		struct key_event ev;
		size_t w;
		uint64_t flags = spin_lock_irqsave(&kbd_lock);

		if (kbd_ring_tail == kbd_ring_head) {
			spin_unlock_irqrestore(&kbd_lock, flags);
			break;
		}

		ev = kbd_ring[kbd_ring_tail];
		spin_unlock_irqrestore(&kbd_lock, flags);

		if (!kbd_try_emit_event(&ev, p, len, n, &w)) {
			break;
		}

		flags = spin_lock_irqsave(&kbd_lock);
		kbd_ring_tail = (kbd_ring_tail + 1) % KBD_RING_SIZE;
		spin_unlock_irqrestore(&kbd_lock, flags);
		n += w;
	}

	return (ssize_t)n;
}

static bool kbd_cdev_poll(struct char_device *dev)
{
	bool ready;

	(void)dev;

	kbd_sync_leds_if_needed();

	uint64_t flags = spin_lock_irqsave(&kbd_lock);
	ready = (kbd_ring_tail != kbd_ring_head);
	spin_unlock_irqrestore(&kbd_lock, flags);
	return ready;
}

static const struct char_ops kbd_ops = {
    .read = kbd_cdev_read,
    .poll = kbd_cdev_poll,
};

static struct char_device kbd_cdev = {
    .name = "kbd",
    .ops = &kbd_ops,
};

static bool kbd_ready;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void kbd_init(void)
{
	kbd_cmd(KBD_CMD_DISABLE_P1);
	kbd_cmd(KBD_CMD_DISABLE_P2);
	kbd_flush();

	kbd_cmd(KBD_CMD_SELF_TEST);
	uint8_t res = 0;
	if (!kbd_read_response(&res))
		pr_warn("kbd: i8042 self-test timed out\n");
	else if (res != KBD_SELF_TEST_OK)
		pr_warn("kbd: i8042 self-test failed (0x%02x)\n",
			(unsigned)res);

	kbd_cmd(KBD_CMD_READ_CONFIG);
	uint8_t cfg = 0;
	if (!kbd_read_response(&cfg))
		pr_warn("kbd: failed to read controller config\n");
	cfg |= 0x01u;
	cfg |= 0x40u; /* Enable translation from set 2 to set 1. */
	cfg &= (uint8_t)~0x10u;
	kbd_cmd(KBD_CMD_WRITE_CONFIG);
	kbd_data(cfg);

	kbd_cmd(KBD_CMD_ENABLE_P1);

	/*
	 * Tell the keyboard to emit scancode set 2 (the i8042 translates it
	 * back to set 1 for us). The set-scancode command takes an argument
	 * byte; both bytes are individually ACKed.
	 */
	if (!kbd_send(KB_CMD_SET_SCANCODE) || !kbd_send(0x02))
		pr_warn("kbd: failed to select scancode set 2\n");

	kbd_flush();
	kbd_probe_layout();
	kbd_apply_numlock_boot_state();
	kbd_sync_leds();

	if (!kbd_send(KB_CMD_ENABLE_SCAN))
		pr_warn("kbd: failed to enable scanning\n");

	kbd_flush();

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
