/*
 * kernel/initramfs/initramfs.c - Read-only initramfs lookup.
 *
 * The initramfs is a Limine-loaded cpio newc archive. v0.0.2 keeps this
 * layer deliberately scan-based; the archive is small and the important
 * contract is safety, not indexing performance.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/cpio_newc.h>
#include <jnu/errno.h>
#include <jnu/initramfs.h>
#include <jnu/klog.h>
#include <jnu/string.h>

static const void *initrd_base;
static size_t initrd_len;
static bool initrd_ready;

static const char *skip_slash(const char *path)
{
	while (*path == '/') {
		path++;
	}
	return path;
}

static bool path_match(const struct cpio_newc_entry *e, const char *path)
{
	path = skip_slash(path);
	return strlen(path) == e->name_len &&
	       memcmp(path, e->name, e->name_len) == 0;
}

int initramfs_init(void *base, size_t len)
{
	struct cpio_newc_entry e;
	size_t off = 0;
	size_t files = 0;
	int err;

	if (!base || len == 0) {
		return -EINVAL;
	}

	initrd_base = base;
	initrd_len = len;
	initrd_ready = false;

	for (;;) {
		err = cpio_newc_next(initrd_base, initrd_len, off, &e);
		if (err < 0) {
			return err;
		}
		if (err == 0) {
			break;
		}
		files++;
		off = e.next_off;
	}

	initrd_ready = true;
	pr_info("initramfs: loaded %u entries (%u bytes)\n", (unsigned)files,
		(unsigned)initrd_len);
	return 0;
}

bool initramfs_ready(void) { return initrd_ready; }

int initramfs_lookup(const char *path, struct initramfs_file *out)
{
	struct cpio_newc_entry e;
	size_t off = 0;
	int err;

	if (!initrd_ready) {
		return -ENODEV;
	}
	if (!path || !out) {
		return -EINVAL;
	}

	while ((err = cpio_newc_next(initrd_base, initrd_len, off, &e)) > 0) {
		if (path_match(&e, path)) {
			out->name = e.name;
			out->name_len = e.name_len;
			out->data = e.data;
			out->size = e.data_len;
			out->mode = e.mode;
			return 0;
		}
		off = e.next_off;
	}

	if (err < 0) {
		return err;
	}
	return -ENOENT;
}

ssize_t initramfs_read_at(const struct initramfs_file *file, uint64_t off,
			  void *buf, size_t len)
{
	size_t avail;

	if (!file || !buf) {
		return -EINVAL;
	}
	if (off >= file->size) {
		return 0;
	}

	avail = file->size - (size_t)off;
	if (len > avail) {
		len = avail;
	}
	memcpy(buf, (const uint8_t *)file->data + off, len);
	return (ssize_t)len;
}

int initramfs_selftest(void)
{
	static const char archive[] = "070701"
				      "00000000"
				      "000081A4"
				      "00000000"
				      "00000000"
				      "00000001"
				      "00000000"
				      "00000002"
				      "00000000"
				      "00000000"
				      "00000000"
				      "00000000"
				      "00000005"
				      "00000000"
				      "init\0\0"
				      "OK\0\0"
				      "070701"
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
				      "0000000B"
				      "00000000"
				      "TRAILER!!!\0\0\0\0";
	const void *save_base = initrd_base;
	size_t save_len = initrd_len;
	bool save_ready = initrd_ready;
	struct initramfs_file file;
	char buf[2];
	int err;

	err = cpio_newc_selftest();
	if (err) {
		return err;
	}

	err = initramfs_init((void *)archive, sizeof(archive) - 1);
	if (err) {
		goto restore;
	}

	err = initramfs_lookup("/init", &file);
	if (err) {
		goto restore;
	}
	if (file.size != 2) {
		err = -EINVAL;
		goto restore;
	}

	err = (int)initramfs_read_at(&file, 0, buf, sizeof(buf));
	if (err != 2) {
		err = -EINVAL;
		goto restore;
	}
	if (memcmp(buf, "OK", 2) != 0) {
		err = -EINVAL;
		goto restore;
	}

	err = initramfs_lookup("/missing", &file);
	if (err != -ENOENT) {
		err = -EINVAL;
		goto restore;
	}

	err = 0;

restore:
	initrd_base = save_base;
	initrd_len = save_len;
	initrd_ready = save_ready;
	return err;
}
