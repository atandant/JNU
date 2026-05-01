/*
 * include/jnu/keycode.h — Hardware-agnostic key identifiers and events.
 *
 * Every physical key has a stable numeric keycode independent of the
 * underlying transport (PS/2 scancode set 1, USB HID, etc.).  Keyboard
 * drivers translate hardware codes into keycodes via the scandata
 * layer; higher-level consumers (the ring buffer, line discipline,
 * future signal delivery) work exclusively with keycodes.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/* ------------------------------------------------------------------ */
/* Key event types                                                     */
/* ------------------------------------------------------------------ */

#define KEY_EV_PRESS 0
#define KEY_EV_RELEASE 1

/* ------------------------------------------------------------------ */
/* Modifier bitmask carried in key_event.modifiers                     */
/* ------------------------------------------------------------------ */

#define KMOD_SHIFT (1u << 0)
#define KMOD_CTRL (1u << 1)
#define KMOD_ALT (1u << 2)
#define KMOD_CAPSLOCK (1u << 3)

/* ------------------------------------------------------------------ */
/* Key event — produced by the keyboard driver on every make/break     */
/* ------------------------------------------------------------------ */

struct key_event {
	uint16_t keycode;  /* KEY_* constant */
	uint8_t modifiers; /* KMOD_* bitmask at time of event */
	uint8_t type;	   /* KEY_EV_PRESS or KEY_EV_RELEASE */
	char ascii;	   /* ASCII translation, 0 if none */
};

/* ------------------------------------------------------------------ */
/* Keycode constants                                                   */
/* ------------------------------------------------------------------ */

enum {
	KEY_NONE = 0,

	/* Letters (alphabetically contiguous for easy range checks) */
	KEY_A,
	KEY_B,
	KEY_C,
	KEY_D,
	KEY_E,
	KEY_F,
	KEY_G,
	KEY_H,
	KEY_I,
	KEY_J,
	KEY_K,
	KEY_L,
	KEY_M,
	KEY_N,
	KEY_O,
	KEY_P,
	KEY_Q,
	KEY_R,
	KEY_S,
	KEY_T,
	KEY_U,
	KEY_V,
	KEY_W,
	KEY_X,
	KEY_Y,
	KEY_Z,

	/* Number row */
	KEY_1,
	KEY_2,
	KEY_3,
	KEY_4,
	KEY_5,
	KEY_6,
	KEY_7,
	KEY_8,
	KEY_9,
	KEY_0,

	/* Symbol keys */
	KEY_MINUS,
	KEY_EQUAL,
	KEY_LBRACKET,
	KEY_RBRACKET,
	KEY_BACKSLASH,
	KEY_SEMICOLON,
	KEY_APOSTROPHE,
	KEY_GRAVE,
	KEY_COMMA,
	KEY_PERIOD,
	KEY_SLASH,

	/* Whitespace / editing */
	KEY_ENTER,
	KEY_TAB,
	KEY_SPACE,
	KEY_BACKSPACE,
	KEY_ESCAPE,

	/* Modifiers */
	KEY_LSHIFT,
	KEY_RSHIFT,
	KEY_LCTRL,
	KEY_RCTRL,
	KEY_LALT,
	KEY_RALT,
	KEY_CAPSLOCK,
	KEY_NUMLOCK,
	KEY_SCROLLLOCK,

	/* Function keys */
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,
	KEY_F9,
	KEY_F10,
	KEY_F11,
	KEY_F12,

	/* Navigation */
	KEY_INSERT,
	KEY_DELETE,
	KEY_HOME,
	KEY_END,
	KEY_PAGEUP,
	KEY_PAGEDOWN,
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,

	/* Numpad */
	KEY_KP0,
	KEY_KP1,
	KEY_KP2,
	KEY_KP3,
	KEY_KP4,
	KEY_KP5,
	KEY_KP6,
	KEY_KP7,
	KEY_KP8,
	KEY_KP9,
	KEY_KPPLUS,
	KEY_KPMINUS,
	KEY_KPSTAR,
	KEY_KPSLASH,
	KEY_KPDOT,
	KEY_KPENTER,

	/* Misc */
	KEY_PRINTSCREEN,
	KEY_PAUSE,

	KEY_MAX,
};
