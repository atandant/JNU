/*
 * include/jnu/scandata.h — Scancode-to-keycode translation.
 *
 * Hardware-agnostic interface for mapping raw scan codes into the
 * universal KEY_* keycode space defined in <jnu/keycode.h>.  The
 * current implementation covers scancode set 1 (as received through
 * the i8042's set-2-to-set-1 translation).  Future transports (USB
 * HID, etc.) would add their own tables behind the same API shape.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/*
 * Translate a scancode set 1 byte into a KEY_* constant.
 * `e0_prefix` must be true when this byte was preceded by 0xE0.
 * Returns KEY_NONE for unmapped or reserved scancodes.
 */
uint16_t scandata_sc1_to_keycode(uint8_t scancode, bool e0_prefix);

/*
 * Translate a keycode + active modifier bitmask (KMOD_*) into an
 * ASCII character.  Returns '\0' for non-printable keys (F-keys,
 * modifiers, navigation, etc.).
 *
 * Caps Lock is handled: letters toggle case regardless of Shift.
 * Ctrl+letter produces 0x01–0x1A.
 */
char scandata_keycode_to_ascii(uint16_t keycode, uint8_t modifiers);

/*
 * Return a short human-readable name for a keycode (e.g. "KEY_A",
 * "KEY_F1", "KEY_UP").  Returns "KEY_NONE" for KEY_NONE and
 * "KEY_UNKNOWN" for values >= KEY_MAX.  Never returns NULL.
 * The returned pointer is into a static read-only table.
 */
const char *scandata_keycode_name(uint16_t keycode);
