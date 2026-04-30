/*
 * kernel/syscall/sys_open.c - open syscall.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/fd.h>
#include <jnu/initramfs.h>
#include <jnu/kbd.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/string.h>
#include <jnu/syscall.h>
#include <jnu/vfs.h>

/*
 * Resolve a synthetic /dev/ path to a registered char_device.
 * Returns NULL when the path is not a known device node.  This is the
 * narrowest entry point that lets userspace read PS/2 keyboard input
 * (open("/dev/kbd") + read()) without granting char drivers direct
 * access to user pointers.  Future device nodes register here.
 */
static struct char_device *resolve_dev_chardev(const char *path) {
  if (strcmp(path, "/dev/kbd") == 0) {
    return kbd_get_chardev();
  }
  return NULL;
}

int64_t sys_open(const char *upath, int flags) {
  char path[JNU_PATH_MAX];
  struct task *task;
  struct file *file;
  struct char_device *cdev;
  int err;

  if (flags != 0) {
    return -EINVAL;
  }

  err = syscall_copy_path(path, upath);
  if (err) {
    return err;
  }

  task = sched_current();
  if (!task || !task->process) {
    return -EINVAL;
  }

  file = kzalloc(sizeof(*file));
  if (!file) {
    return -ENOMEM;
  }

  cdev = resolve_dev_chardev(path);
  if (cdev) {
    file->type = JNU_FILE_CHARDEV;
    file->u.chardev = cdev;
    goto alloc_fd;
  }

  err = initramfs_lookup(path, &file->u.initramfs);
  if (!err) {
    file->type = JNU_FILE_INITRAMFS;
    goto alloc_fd;
  }

  err = vfs_open(path, &file->u.vfs);
  if (err) {
    goto fail_file;
  }
  file->type = JNU_FILE_VFS;

alloc_fd:
  err = fd_alloc(&task->process->fds, file);
  if (err < 0) {
    goto fail_backing;
  }
  return err;

fail_backing:
  if (file->type == JNU_FILE_VFS) {
    vfs_close(file->u.vfs);
  }
fail_file:
  kfree(file);
  return err;
}
