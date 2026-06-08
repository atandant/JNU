/*
 * kernel/drivers/scandata.c — Scancode set 1 to keycode tables + ASCII maps.
 *
 * This file is the single source of truth for translating raw hardware
 * bytes into the universal KEY_* keycode space and then into ASCII.
 * The data is driver-agnostic: any keyboard transport that produces
 * scancode set 1 bytes (PS/2, i8042, virtualised KBC) reuses these
 * tables via scandata_sc1_to_keycode().
 *
 * Tables are `const` and read-only at runtime; no locks required.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/base/types.h>
#include <jnu/drivers/keycode.h>
#include <jnu/drivers/scandata.h>

/* ------------------------------------------------------------------ */
/* Scancode set 1 → keycode (regular, no 0xE0 prefix)                 */
/* ------------------------------------------------------------------ */

static const uint16_t sc1_keycode[128] = {
    [0x01] = KEY_ESCAPE,     [0x02] = KEY_1,	     [0x03] = KEY_2,
    [0x04] = KEY_3,	     [0x05] = KEY_4,	     [0x06] = KEY_5,
    [0x07] = KEY_6,	     [0x08] = KEY_7,	     [0x09] = KEY_8,
    [0x0A] = KEY_9,	     [0x0B] = KEY_0,	     [0x0C] = KEY_MINUS,
    [0x0D] = KEY_EQUAL,	     [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = KEY_Q,	     [0x11] = KEY_W,	     [0x12] = KEY_E,
    [0x13] = KEY_R,	     [0x14] = KEY_T,	     [0x15] = KEY_Y,
    [0x16] = KEY_U,	     [0x17] = KEY_I,	     [0x18] = KEY_O,
    [0x19] = KEY_P,	     [0x1A] = KEY_LBRACKET,  [0x1B] = KEY_RBRACKET,
    [0x1C] = KEY_ENTER,	     [0x1D] = KEY_LCTRL,     [0x1E] = KEY_A,
    [0x1F] = KEY_S,	     [0x20] = KEY_D,	     [0x21] = KEY_F,
    [0x22] = KEY_G,	     [0x23] = KEY_H,	     [0x24] = KEY_J,
    [0x25] = KEY_K,	     [0x26] = KEY_L,	     [0x27] = KEY_SEMICOLON,
    [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,     [0x2A] = KEY_LSHIFT,
    [0x2B] = KEY_BACKSLASH,  [0x2C] = KEY_Z,	     [0x2D] = KEY_X,
    [0x2E] = KEY_C,	     [0x2F] = KEY_V,	     [0x30] = KEY_B,
    [0x31] = KEY_N,	     [0x32] = KEY_M,	     [0x33] = KEY_COMMA,
    [0x34] = KEY_PERIOD,     [0x35] = KEY_SLASH,     [0x36] = KEY_RSHIFT,
    [0x37] = KEY_KPSTAR,     [0x38] = KEY_LALT,	     [0x39] = KEY_SPACE,
    [0x3A] = KEY_CAPSLOCK,   [0x3B] = KEY_F1,	     [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,	     [0x3E] = KEY_F4,	     [0x3F] = KEY_F5,
    [0x40] = KEY_F6,	     [0x41] = KEY_F7,	     [0x42] = KEY_F8,
    [0x43] = KEY_F9,	     [0x44] = KEY_F10,	     [0x45] = KEY_NUMLOCK,
    [0x46] = KEY_SCROLLLOCK, [0x47] = KEY_KP7,	     [0x48] = KEY_KP8,
    [0x49] = KEY_KP9,	     [0x4A] = KEY_KPMINUS,   [0x4B] = KEY_KP4,
    [0x4C] = KEY_KP5,	     [0x4D] = KEY_KP6,	     [0x4E] = KEY_KPPLUS,
    [0x4F] = KEY_KP1,	     [0x50] = KEY_KP2,	     [0x51] = KEY_KP3,
    [0x52] = KEY_KP0,	     [0x53] = KEY_KPDOT,     [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

/* ------------------------------------------------------------------ */
/* Scancode set 1 → keycode (0xE0-prefixed extended keys)             */
/* ------------------------------------------------------------------ */

static const uint16_t sc1_e0_keycode[128] = {
    [0x1C] = KEY_KPENTER, [0x1D] = KEY_RCTRL,  [0x35] = KEY_KPSLASH,
    [0x38] = KEY_RALT,	  [0x47] = KEY_HOME,   [0x48] = KEY_UP,
    [0x49] = KEY_PAGEUP,  [0x4B] = KEY_LEFT,   [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,	  [0x50] = KEY_DOWN,   [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT,  [0x53] = KEY_DELETE,
};

/* ------------------------------------------------------------------ */
/* Keycode → ASCII (unshifted, US-QWERTY)                             */
/* ------------------------------------------------------------------ */

static const char ascii_unshift[KEY_MAX] = {
    /* Letters */
    [KEY_A] = 'a',
    [KEY_B] = 'b',
    [KEY_C] = 'c',
    [KEY_D] = 'd',
    [KEY_E] = 'e',
    [KEY_F] = 'f',
    [KEY_G] = 'g',
    [KEY_H] = 'h',
    [KEY_I] = 'i',
    [KEY_J] = 'j',
    [KEY_K] = 'k',
    [KEY_L] = 'l',
    [KEY_M] = 'm',
    [KEY_N] = 'n',
    [KEY_O] = 'o',
    [KEY_P] = 'p',
    [KEY_Q] = 'q',
    [KEY_R] = 'r',
    [KEY_S] = 's',
    [KEY_T] = 't',
    [KEY_U] = 'u',
    [KEY_V] = 'v',
    [KEY_W] = 'w',
    [KEY_X] = 'x',
    [KEY_Y] = 'y',
    [KEY_Z] = 'z',

    /* Number row */
    [KEY_1] = '1',
    [KEY_2] = '2',
    [KEY_3] = '3',
    [KEY_4] = '4',
    [KEY_5] = '5',
    [KEY_6] = '6',
    [KEY_7] = '7',
    [KEY_8] = '8',
    [KEY_9] = '9',
    [KEY_0] = '0',

    /* Symbols */
    [KEY_MINUS] = '-',
    [KEY_EQUAL] = '=',
    [KEY_LBRACKET] = '[',
    [KEY_RBRACKET] = ']',
    [KEY_BACKSLASH] = '\\',
    [KEY_SEMICOLON] = ';',
    [KEY_APOSTROPHE] = '\'',
    [KEY_GRAVE] = '`',
    [KEY_COMMA] = ',',
    [KEY_PERIOD] = '.',
    [KEY_SLASH] = '/',

    /* Whitespace / editing */
    [KEY_ENTER] = '\n',
    [KEY_TAB] = '\t',
    [KEY_SPACE] = ' ',
    [KEY_BACKSPACE] = '\b',
    [KEY_ESCAPE] = 0x1B,

    /* Numpad */
    [KEY_KP0] = '0',
    [KEY_KP1] = '1',
    [KEY_KP2] = '2',
    [KEY_KP3] = '3',
    [KEY_KP4] = '4',
    [KEY_KP5] = '5',
    [KEY_KP6] = '6',
    [KEY_KP7] = '7',
    [KEY_KP8] = '8',
    [KEY_KP9] = '9',
    [KEY_KPPLUS] = '+',
    [KEY_KPMINUS] = '-',
    [KEY_KPSTAR] = '*',
    [KEY_KPSLASH] = '/',
    [KEY_KPDOT] = '.',
    [KEY_KPENTER] = '\n',
};

/* ------------------------------------------------------------------ */
/* Keycode → ASCII (shifted, US-QWERTY)                               */
/* ------------------------------------------------------------------ */

static const char ascii_shift[KEY_MAX] = {
    /* Letters (uppercase) */
    [KEY_A] = 'A',
    [KEY_B] = 'B',
    [KEY_C] = 'C',
    [KEY_D] = 'D',
    [KEY_E] = 'E',
    [KEY_F] = 'F',
    [KEY_G] = 'G',
    [KEY_H] = 'H',
    [KEY_I] = 'I',
    [KEY_J] = 'J',
    [KEY_K] = 'K',
    [KEY_L] = 'L',
    [KEY_M] = 'M',
    [KEY_N] = 'N',
    [KEY_O] = 'O',
    [KEY_P] = 'P',
    [KEY_Q] = 'Q',
    [KEY_R] = 'R',
    [KEY_S] = 'S',
    [KEY_T] = 'T',
    [KEY_U] = 'U',
    [KEY_V] = 'V',
    [KEY_W] = 'W',
    [KEY_X] = 'X',
    [KEY_Y] = 'Y',
    [KEY_Z] = 'Z',

    /* Number row symbols */
    [KEY_1] = '!',
    [KEY_2] = '@',
    [KEY_3] = '#',
    [KEY_4] = '$',
    [KEY_5] = '%',
    [KEY_6] = '^',
    [KEY_7] = '&',
    [KEY_8] = '*',
    [KEY_9] = '(',
    [KEY_0] = ')',

    /* Shifted symbols */
    [KEY_MINUS] = '_',
    [KEY_EQUAL] = '+',
    [KEY_LBRACKET] = '{',
    [KEY_RBRACKET] = '}',
    [KEY_BACKSLASH] = '|',
    [KEY_SEMICOLON] = ':',
    [KEY_APOSTROPHE] = '"',
    [KEY_GRAVE] = '~',
    [KEY_COMMA] = '<',
    [KEY_PERIOD] = '>',
    [KEY_SLASH] = '?',

    /* These are the same shifted or unshifted */
    [KEY_ENTER] = '\n',
    [KEY_TAB] = '\t',
    [KEY_SPACE] = ' ',
    [KEY_BACKSPACE] = '\b',
    [KEY_ESCAPE] = 0x1B,

    /* Numpad (shift does not change numpad output) */
    [KEY_KP0] = '0',
    [KEY_KP1] = '1',
    [KEY_KP2] = '2',
    [KEY_KP3] = '3',
    [KEY_KP4] = '4',
    [KEY_KP5] = '5',
    [KEY_KP6] = '6',
    [KEY_KP7] = '7',
    [KEY_KP8] = '8',
    [KEY_KP9] = '9',
    [KEY_KPPLUS] = '+',
    [KEY_KPMINUS] = '-',
    [KEY_KPSTAR] = '*',
    [KEY_KPSLASH] = '/',
    [KEY_KPDOT] = '.',
    [KEY_KPENTER] = '\n',
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

uint16_t scandata_sc1_to_keycode(uint8_t scancode, bool e0_prefix)
{
	uint8_t idx = scancode & 0x7Fu;

	if (e0_prefix) {
		return sc1_e0_keycode[idx];
	}
	return sc1_keycode[idx];
}

char scandata_keycode_to_ascii(uint16_t keycode, uint8_t modifiers)
{
	char c;

	if (keycode >= KEY_MAX) {
		return '\0';
	}

	c = (modifiers & KMOD_SHIFT) ? ascii_shift[keycode]
				     : ascii_unshift[keycode];

	/*
	 * Caps Lock toggles case for letters only.  With Shift held,
	 * the character is already uppercase from ascii_shift; Caps Lock
	 * flips it back to lowercase.  Without Shift, the character is
	 * lowercase from ascii_unshift; Caps Lock flips it to uppercase.
	 */
	if (modifiers & KMOD_CAPSLOCK) {
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 32);
		} else if (c >= 'A' && c <= 'Z') {
			c = (char)(c + 32);
		}
	}

	/* Ctrl+letter → 0x01–0x1A (standard terminal control codes). */
	if ((modifiers & KMOD_CTRL) && c >= 'a' && c <= 'z') {
		c = (char)(c - 'a' + 1);
	}

	return c;
}

/* ------------------------------------------------------------------ */
/* Keycode → String name mapping                                      */
/* ------------------------------------------------------------------ */

static const char *const keycode_names[KEY_MAX] = {
    [KEY_NONE] = "KEY_NONE",
    [KEY_A] = "KEY_A",
    [KEY_B] = "KEY_B",
    [KEY_C] = "KEY_C",
    [KEY_D] = "KEY_D",
    [KEY_E] = "KEY_E",
    [KEY_F] = "KEY_F",
    [KEY_G] = "KEY_G",
    [KEY_H] = "KEY_H",
    [KEY_I] = "KEY_I",
    [KEY_J] = "KEY_J",
    [KEY_K] = "KEY_K",
    [KEY_L] = "KEY_L",
    [KEY_M] = "KEY_M",
    [KEY_N] = "KEY_N",
    [KEY_O] = "KEY_O",
    [KEY_P] = "KEY_P",
    [KEY_Q] = "KEY_Q",
    [KEY_R] = "KEY_R",
    [KEY_S] = "KEY_S",
    [KEY_T] = "KEY_T",
    [KEY_U] = "KEY_U",
    [KEY_V] = "KEY_V",
    [KEY_W] = "KEY_W",
    [KEY_X] = "KEY_X",
    [KEY_Y] = "KEY_Y",
    [KEY_Z] = "KEY_Z",

    [KEY_1] = "KEY_1",
    [KEY_2] = "KEY_2",
    [KEY_3] = "KEY_3",
    [KEY_4] = "KEY_4",
    [KEY_5] = "KEY_5",
    [KEY_6] = "KEY_6",
    [KEY_7] = "KEY_7",
    [KEY_8] = "KEY_8",
    [KEY_9] = "KEY_9",
    [KEY_0] = "KEY_0",

    [KEY_MINUS] = "KEY_MINUS",
    [KEY_EQUAL] = "KEY_EQUAL",
    [KEY_LBRACKET] = "KEY_LBRACKET",
    [KEY_RBRACKET] = "KEY_RBRACKET",
    [KEY_BACKSLASH] = "KEY_BACKSLASH",
    [KEY_SEMICOLON] = "KEY_SEMICOLON",
    [KEY_APOSTROPHE] = "KEY_APOSTROPHE",
    [KEY_GRAVE] = "KEY_GRAVE",
    [KEY_COMMA] = "KEY_COMMA",
    [KEY_PERIOD] = "KEY_PERIOD",
    [KEY_SLASH] = "KEY_SLASH",

    [KEY_ENTER] = "KEY_ENTER",
    [KEY_TAB] = "KEY_TAB",
    [KEY_SPACE] = "KEY_SPACE",
    [KEY_BACKSPACE] = "KEY_BACKSPACE",
    [KEY_ESCAPE] = "KEY_ESCAPE",

    [KEY_LSHIFT] = "KEY_LSHIFT",
    [KEY_RSHIFT] = "KEY_RSHIFT",
    [KEY_LCTRL] = "KEY_LCTRL",
    [KEY_RCTRL] = "KEY_RCTRL",
    [KEY_LALT] = "KEY_LALT",
    [KEY_RALT] = "KEY_RALT",
    [KEY_CAPSLOCK] = "KEY_CAPSLOCK",
    [KEY_NUMLOCK] = "KEY_NUMLOCK",
    [KEY_SCROLLLOCK] = "KEY_SCROLLLOCK",

    [KEY_F1] = "KEY_F1",
    [KEY_F2] = "KEY_F2",
    [KEY_F3] = "KEY_F3",
    [KEY_F4] = "KEY_F4",
    [KEY_F5] = "KEY_F5",
    [KEY_F6] = "KEY_F6",
    [KEY_F7] = "KEY_F7",
    [KEY_F8] = "KEY_F8",
    [KEY_F9] = "KEY_F9",
    [KEY_F10] = "KEY_F10",
    [KEY_F11] = "KEY_F11",
    [KEY_F12] = "KEY_F12",

    [KEY_INSERT] = "KEY_INSERT",
    [KEY_DELETE] = "KEY_DELETE",
    [KEY_HOME] = "KEY_HOME",
    [KEY_END] = "KEY_END",
    [KEY_PAGEUP] = "KEY_PAGEUP",
    [KEY_PAGEDOWN] = "KEY_PAGEDOWN",
    [KEY_UP] = "KEY_UP",
    [KEY_DOWN] = "KEY_DOWN",
    [KEY_LEFT] = "KEY_LEFT",
    [KEY_RIGHT] = "KEY_RIGHT",

    [KEY_KP0] = "KEY_KP0",
    [KEY_KP1] = "KEY_KP1",
    [KEY_KP2] = "KEY_KP2",
    [KEY_KP3] = "KEY_KP3",
    [KEY_KP4] = "KEY_KP4",
    [KEY_KP5] = "KEY_KP5",
    [KEY_KP6] = "KEY_KP6",
    [KEY_KP7] = "KEY_KP7",
    [KEY_KP8] = "KEY_KP8",
    [KEY_KP9] = "KEY_KP9",
    [KEY_KPPLUS] = "KEY_KPPLUS",
    [KEY_KPMINUS] = "KEY_KPMINUS",
    [KEY_KPSTAR] = "KEY_KPSTAR",
    [KEY_KPSLASH] = "KEY_KPSLASH",
    [KEY_KPDOT] = "KEY_KPDOT",
    [KEY_KPENTER] = "KEY_KPENTER",

    [KEY_PRINTSCREEN] = "KEY_PRINTSCREEN",
    [KEY_PAUSE] = "KEY_PAUSE",
};

const char *scandata_keycode_name(uint16_t keycode)
{
	if (keycode >= KEY_MAX) {
		return "KEY_UNKNOWN";
	}
	const char *name = keycode_names[keycode];
	return name ? name : "KEY_UNKNOWN";
}
