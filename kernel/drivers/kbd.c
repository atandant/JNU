/*
 * kernel/drivers/kbd.c — PS/2 keyboard via i8042 controller.
 *
 * Initializes the i8042 keyboard controller: disable ports, flush,
 * self-test, enable port 1, set scancode set 1, and install an IRQ
 * handler on vector 33 (IOAPIC ISA IRQ 1).
 *
 * Raw scancodes are translated to keycodes via the hardware-agnostic
 * scandata layer, then to ASCII for the char_device ring buffer.
 * The 0xE0 extended-key prefix is tracked so navigation keys, arrow
 * keys, and right-hand modifiers are decoded correctly.
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
#include <jnu/keycode.h>
#include <jnu/klog.h>
#include <jnu/scandata.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>

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

#define KBD_SELF_TEST_OK 0x55

/* ------------------------------------------------------------------ */
/* Ring buffer for decoded ASCII characters                            */
/* ------------------------------------------------------------------ */

#define KBD_RING_SIZE 64

static char kbd_ring[KBD_RING_SIZE];
static size_t kbd_ring_head;
static size_t kbd_ring_tail;
static struct spinlock kbd_lock = SPINLOCK_INITIALIZER;

static void ring_put(char c) {
  uint64_t flags = spin_lock_irqsave(&kbd_lock);
  size_t next = (kbd_ring_head + 1) % KBD_RING_SIZE;

  if (next != kbd_ring_tail) {
    kbd_ring[kbd_ring_head] = c;
    kbd_ring_head = next;
  }
  spin_unlock_irqrestore(&kbd_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Modifier state and 0xE0 prefix tracking                             */
/* ------------------------------------------------------------------ */

static bool shift_held;
static bool ctrl_held;
static bool alt_held;
static bool caps_on;
static bool e0_pending;

/*
 * Build the current modifier bitmask for key_event / ASCII translation.
 */
static uint8_t current_modifiers(void) {
  uint8_t mods = 0;

  if (shift_held) {
    mods |= KMOD_SHIFT;
  }
  if (ctrl_held) {
    mods |= KMOD_CTRL;
  }
  if (alt_held) {
    mods |= KMOD_ALT;
  }
  if (caps_on) {
    mods |= KMOD_CAPSLOCK;
  }
  return mods;
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                         */
/* ------------------------------------------------------------------ */

static void kbd_irq_handler(struct cpu_state *st) {
  uint8_t sc = inb(KBD_DATA_PORT);
  uint16_t keycode;
  bool release;
  bool is_e0;

  (void)st;

  /* 0xE0 prefix: set flag and wait for the real scancode byte. */
  if (sc == 0xE0) {
    e0_pending = true;
    goto done;
  }

  is_e0 = e0_pending;
  e0_pending = false;

  /* Bit 7 set = break (release) code. */
  release = (sc & 0x80u) != 0;
  keycode = scandata_sc1_to_keycode(sc, is_e0);

  if (keycode == KEY_NONE) {
    goto done;
  }

  /* Update modifier state on make/break. */
  switch (keycode) {
  case KEY_LSHIFT:
  case KEY_RSHIFT:
    shift_held = !release;
    goto done;
  case KEY_LCTRL:
  case KEY_RCTRL:
    ctrl_held = !release;
    goto done;
  case KEY_LALT:
  case KEY_RALT:
    alt_held = !release;
    goto done;
  case KEY_CAPSLOCK:
    if (!release) {
      caps_on = !caps_on;
    }
    goto done;
  case KEY_NUMLOCK:
  case KEY_SCROLLLOCK:
    /* Tracked but not acted on in v0.0.2. */
    goto done;
  default:
    break;
  }

  /* Only key-down events produce characters. */
  if (release) {
    goto done;
  }

	{
		uint8_t mods = current_modifiers();
		char c = scandata_keycode_to_ascii(keycode, mods);

		pr_debug("kbd: sc=0x%02x kc=%s char='%c'\n", (unsigned)sc,
		         scandata_keycode_name(keycode), (c >= 0x20 && c <= 0x7E) ? c : '.');

		if (c) {
			ring_put(c);
		}
	}

done:
  apic_eoi();
}

/* ------------------------------------------------------------------ */
/* i8042 helpers                                                       */
/* ------------------------------------------------------------------ */

static void kbd_wait_input(void) {
  for (int t = 100000; (inb(KBD_STATUS_PORT) & KBD_STATUS_IBF) && t; t--)
    __asm__ __volatile__("pause");
}

static void kbd_wait_output(void) {
  for (int t = 100000; !(inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) && t; t--)
    __asm__ __volatile__("pause");
}

static void kbd_flush(void) {
  while (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF)
    (void)inb(KBD_DATA_PORT);
}

static void kbd_cmd(uint8_t cmd) {
  kbd_wait_input();
  outb(KBD_CMD_PORT, cmd);
}

static void kbd_data(uint8_t val) {
  kbd_wait_input();
  outb(KBD_DATA_PORT, val);
}

static uint8_t kbd_read(void) {
  kbd_wait_output();
  return inb(KBD_DATA_PORT);
}

/* ------------------------------------------------------------------ */
/* char_device interface                                               */
/* ------------------------------------------------------------------ */

static ssize_t kbd_cdev_read(struct char_device *dev, void *buf, size_t len) {
  (void)dev;
  char *p = buf;
  size_t n = 0;
  uint64_t flags = spin_lock_irqsave(&kbd_lock);

  while (n < len && kbd_ring_tail != kbd_ring_head) {
    p[n++] = kbd_ring[kbd_ring_tail];
    kbd_ring_tail = (kbd_ring_tail + 1) % KBD_RING_SIZE;
  }
  spin_unlock_irqrestore(&kbd_lock, flags);
  return (ssize_t)n;
}

static bool kbd_cdev_poll(struct char_device *dev) {
  (void)dev;
  uint64_t flags = spin_lock_irqsave(&kbd_lock);
  bool ready = (kbd_ring_tail != kbd_ring_head);
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

void kbd_init(void) {
  kbd_cmd(KBD_CMD_DISABLE_P1);
  kbd_cmd(KBD_CMD_DISABLE_P2);
  kbd_flush();

  kbd_cmd(KBD_CMD_SELF_TEST);
  uint8_t res = kbd_read();
  if (res != KBD_SELF_TEST_OK)
    pr_warn("kbd: i8042 self-test failed (0x%02x)\n", (unsigned)res);

  kbd_cmd(KBD_CMD_READ_CONFIG);
  uint8_t cfg = kbd_read();
  cfg |= 0x01u;
  cfg |= 0x40u; /* Enable translation from set 2 to set 1. */
  cfg &= (uint8_t)~0x10u;
  kbd_cmd(KBD_CMD_WRITE_CONFIG);
  kbd_data(cfg);

  kbd_cmd(KBD_CMD_ENABLE_P1);

  kbd_data(KB_CMD_SET_SCANCODE);
  (void)kbd_read();
  kbd_data(0x02); /* Tell keyboard to use set 2, i8042 translates. */
  (void)kbd_read();

  kbd_data(KB_CMD_ENABLE_SCAN);
  (void)kbd_read();

  idt_set_handler(VEC_KBD, kbd_irq_handler);
  ioapic_route_isa_irq(1, VEC_KBD);
  ioapic_unmask(1);

  kbd_ready = true;
  pr_info("kbd: PS/2 keyboard initialized (scancode set 1)\n");
}

struct char_device *kbd_get_chardev(void) {
  return kbd_ready ? &kbd_cdev : NULL;
}
