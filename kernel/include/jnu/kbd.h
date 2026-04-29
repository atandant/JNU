/*
 * include/jnu/kbd.h — PS/2 keyboard driver (i8042, scancode set 1).
 *
 * Initializes the i8042 controller, enables port 1 scanning, and
 * installs an IRQ handler on vector 33 (IOAPIC ISA IRQ 1). Decoded
 * key events are queued in a fixed-size ring buffer and exposed
 * through a struct char_device for higher layers.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/chardev.h>
#include <jnu/types.h>

/*
 * Initialize the PS/2 keyboard controller and install the IRQ.
 * Call after apic_init() and idt_init() so the IOAPIC routing works.
 */
void kbd_init(void);

/*
 * Get the keyboard as a char_device for read/poll. Returns NULL if
 * kbd_init() has not been called.
 */
struct char_device *kbd_get_chardev(void);
