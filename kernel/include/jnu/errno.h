/*
 * include/jnu/errno.h — Negative errno-style return codes.
 *
 * Kernel functions return 0 on success or a negative value from this
 * file on failure, matching the Linux convention.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define EPERM		1	/* Operation not permitted */
#define ENOENT		2	/* No such entry */
#define EIO		5	/* I/O error */
#define ENXIO		6	/* No such device or address */
#define E2BIG		7	/* Argument list too long */
#define ENOEXEC		8	/* Executable format error */
#define EAGAIN		11	/* Try again */
#define ENOMEM		12	/* Out of memory */
#define EACCES		13	/* Permission denied */
#define EFAULT		14	/* Bad address */
#define EBUSY		16	/* Resource busy */
#define EEXIST		17	/* Already exists */
#define ENODEV		19	/* No such device */
#define ENOTDIR		20	/* Not a directory */
#define EISDIR		21	/* Is a directory */
#define ECHILD		10	/* No child processes */
#define EINVAL		22	/* Invalid argument */
#define ENOSPC		28	/* No space left */
#define EMFILE		24	/* Too many open files */
#define ERANGE		34	/* Out of range */
#define ENAMETOOLONG	36	/* File name too long */
#define ENOSYS		38	/* Not implemented */
#define ENOTSUP		95	/* Not supported */
