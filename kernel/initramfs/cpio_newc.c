/*
 * kernel/initramfs/cpio_newc.c - cpio newc archive parser.
 *
 * Parses the ASCII-header "newc" variant used by the v0.0.2 initramfs.
 * The parser is bounds-first: every offset and length is checked before
 * the caller receives pointers into the archive.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/fs/cpio_newc.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

static size_t align4(size_t v) { return (v + 3u) & ~(size_t)3u; }

static int checked_add(size_t a, size_t b, size_t *out)
{
	size_t v = a + b;

	if (v < a) {
		return -ERANGE;
	}
	*out = v;
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static int parse_hex8(const char *p, uint32_t *out)
{
	uint32_t v = 0;

	for (int i = 0; i < 8; i++) {
		int n = hexval(p[i]);
		if (n < 0) {
			return -EINVAL;
		}
		v = (v << 4) | (uint32_t)n;
	}

	*out = v;
	return 0;
}

int cpio_newc_next(const void *archive, size_t archive_len, size_t off,
		   struct cpio_newc_entry *out)
{
	const char *base = archive;
	const char *hdr;
	uint32_t mode;
	uint32_t namesize;
	uint32_t filesize;
	size_t name_off;
	size_t data_off;
	size_t next_off;
	int err;

	if (!archive || !out) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	if (off >= archive_len) {
		return 0;
	}
	if (archive_len - off < CPIO_NEWC_HEADER_SIZE) {
		return -EINVAL;
	}

	hdr = base + off;
	if (memcmp(hdr, CPIO_NEWC_MAGIC, 6) != 0) {
		return -EINVAL;
	}

	err = parse_hex8(hdr + 14, &mode);
	if (err) {
		return err;
	}
	err = parse_hex8(hdr + 54, &filesize);
	if (err) {
		return err;
	}
	err = parse_hex8(hdr + 94, &namesize);
	if (err) {
		return err;
	}
	if (namesize == 0) {
		return -EINVAL;
	}

	err = checked_add(off, CPIO_NEWC_HEADER_SIZE, &name_off);
	if (err) {
		return err;
	}
	err = checked_add(name_off, namesize, &data_off);
	if (err) {
		return err;
	}
	if (data_off > archive_len) {
		return -EINVAL;
	}
	if (base[data_off - 1] != '\0') {
		return -EINVAL;
	}

	data_off = align4(data_off);
	err = checked_add(data_off, filesize, &next_off);
	if (err) {
		return err;
	}
	if (next_off > archive_len) {
		return -EINVAL;
	}
	next_off = align4(next_off);
	if (next_off > archive_len) {
		return -EINVAL;
	}

	out->name = base + name_off;
	out->name_len = namesize - 1;
	out->data = base + data_off;
	out->data_len = filesize;
	out->mode = mode;
	out->next_off = next_off;

	if (out->name_len == strlen(CPIO_NEWC_TRAILER) &&
	    memcmp(out->name, CPIO_NEWC_TRAILER, out->name_len) == 0) {
		return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------------- */
/* Selftest                                                                   */
/* ------------------------------------------------------------------------- */

#define H(m, f, n)                                                             \
	"070701"                                                               \
	"00000000" m "00000000"                                                \
	"00000000"                                                             \
	"00000001"                                                             \
	"00000000" f "00000000"                                                \
	"00000000"                                                             \
	"00000000"                                                             \
	"00000000" n "00000000"

int cpio_newc_selftest(void)
{
	static const char good[] =
	    H("000081A4", "00000005",
	      "00000005") "init\0\0"
			  "hello\0\0\0" H("00000000", "00000000",
					  "0000000B") "TRAILER!!!\0\0\0\0";
	static const char bad_magic[] = "BADBAD"
					"00000000"
					"00000000"
					"00000000"
					"00000000"
					"00000001"
					"00000000"
					"00000000"
					"00000000"
					"00000000"
					"00000000"
					"00000000"
					"00000001"
					"00000000"
					"\0";
	struct cpio_newc_entry e;
	int err;

	err = cpio_newc_next(good, sizeof(good) - 1, 0, &e);
	if (err != 1) {
		return -EINVAL;
	}
	if (e.name_len != 4 || memcmp(e.name, "init", 4) != 0) {
		return -EINVAL;
	}
	if (e.data_len != 5 || memcmp(e.data, "hello", 5) != 0) {
		return -EINVAL;
	}

	err = cpio_newc_next(good, sizeof(good) - 1, e.next_off, &e);
	if (err != 0) {
		return -EINVAL;
	}

	err = cpio_newc_next(bad_magic, sizeof(bad_magic) - 1, 0, &e);
	if (err != -EINVAL) {
		return -EINVAL;
	}

	return 0;
}
