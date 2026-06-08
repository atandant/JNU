/*
 * kernel/kernel/execve.c - execve() image replacement.
 *
 * Captures argv/envp out of the current user address space, loads the
 * requested ELF into a fresh addr_space, builds a new initial stack,
 * then swaps the process to the new image before destroying the old one.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/fs/initramfs.h>
#include <jnu/fs/vfs.h>
#include <jnu/kernel/elf64.h>
#include <jnu/kernel/exec.h>
#include <jnu/kernel/process.h>
#include <jnu/kernel/sched.h>
#include <jnu/lib/string.h>
#include <jnu/mm/kmalloc.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/syscall.h>
#include <jnu/user/usercopy.h>
#include <uapi/jnu/errno.h>

#define EXEC_MAX_TOTAL (64 * 1024)
#define EXEC_MAX_STRINGS 256

static ssize_t initramfs_exec_read(void *ctx, uint64_t off, void *buf,
				   size_t len)
{
	return initramfs_read_at(ctx, off, buf, len);
}

static ssize_t vfs_exec_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return vfs_read(ctx, off, len, buf);
}

static int open_exec_image(const char *path, struct exec_image *image,
			   struct initramfs_file *init_file,
			   struct vfs_inode **vfs_ino)
{
	int err;

	if (!path || !image || !init_file || !vfs_ino) {
		return -EINVAL;
	}

	*vfs_ino = NULL;
	err = initramfs_lookup(path, init_file);
	if (!err) {
		image->read_at = initramfs_exec_read;
		image->size = init_file->size;
		image->ctx = init_file;
		return 0;
	}

	err = vfs_open(path, vfs_ino);
	if (err) {
		return err;
	}

	image->read_at = vfs_exec_read;
	image->size = (*vfs_ino)->size;
	image->ctx = *vfs_ino;
	return 0;
}

static void release_vector(char **vec) { kfree(vec); }

static int charge_total(size_t *total, size_t add)
{
	if (add > EXEC_MAX_TOTAL || *total > EXEC_MAX_TOTAL - add) {
		return -E2BIG;
	}
	*total += add;
	return 0;
}

static int capture_vector(char *const *uvec, bool require_nonempty,
			  char **storage, size_t *storage_used, size_t *total,
			  char ***out_vec, size_t *out_count)
{
	char **vec;

	if (!out_vec || !out_count || !storage || !*storage || !storage_used ||
	    !total) {
		return -EINVAL;
	}

	*out_vec = NULL;
	*out_count = 0;

	if (!uvec) {
		if (require_nonempty) {
			return -EINVAL;
		}
		vec = kzalloc(sizeof(*vec));
		if (!vec) {
			return -ENOMEM;
		}
		if (charge_total(total, sizeof(char *))) {
			kfree(vec);
			return -E2BIG;
		}
		*out_vec = vec;
		return 0;
	}

	vec = kzalloc((EXEC_MAX_STRINGS + 1) * sizeof(*vec));
	if (!vec) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < EXEC_MAX_STRINGS; i++) {
		char *up;
		size_t remaining;
		size_t len;
		int err;

		err = copy_from_user(&up, &uvec[i], sizeof(up));
		if (err) {
			release_vector(vec);
			return err;
		}

		err = charge_total(total, sizeof(char *));
		if (err) {
			release_vector(vec);
			return err;
		}

		if (!up) {
			if (i == 0 && require_nonempty) {
				release_vector(vec);
				return -EINVAL;
			}
			*out_vec = vec;
			*out_count = i;
			return 0;
		}

		remaining = EXEC_MAX_TOTAL - *total;
		if (remaining == 0) {
			release_vector(vec);
			return -E2BIG;
		}

		err = copy_string_from_user(*storage + *storage_used, up,
					    remaining + 1);
		if (err == -ENAMETOOLONG) {
			err = -E2BIG;
		}
		if (err) {
			release_vector(vec);
			return err;
		}

		len = strlen(*storage + *storage_used) + 1;
		err = charge_total(total, len);
		if (err) {
			release_vector(vec);
			return err;
		}
		vec[i] = *storage + *storage_used;
		*storage_used += len;
	}

	release_vector(vec);
	return -E2BIG;
}

int exec_strings_capture(const char *user_path, char *const *user_argv,
			 char *const *user_envp, struct exec_strings *out)
{
	char *backing;
	size_t storage_used = 0;
	int err;

	if (!out) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	out->path = kmalloc(JNU_PATH_MAX);
	if (!out->path) {
		return -ENOMEM;
	}

	err = copy_string_from_user(out->path, user_path, JNU_PATH_MAX);
	if (err) {
		goto fail;
	}

	backing = kmalloc(EXEC_MAX_TOTAL + 1);
	if (!backing) {
		err = -ENOMEM;
		goto fail;
	}
	out->backing = backing;

	err = capture_vector(user_argv, true, &backing, &storage_used,
			     &out->total, &out->argv, &out->argc);
	if (err) {
		goto fail;
	}

	err = capture_vector(user_envp, false, &backing, &storage_used,
			     &out->total, &out->envp, &out->envc);
	if (err) {
		goto fail;
	}

	return 0;

fail:
	exec_strings_release(out);
	return err;
}

void exec_strings_release(struct exec_strings *strings)
{
	if (!strings) {
		return;
	}

	kfree(strings->path);
	release_vector(strings->argv);
	release_vector(strings->envp);
	kfree(strings->backing);
	memset(strings, 0, sizeof(*strings));
}

int process_execve(const char *user_path, char *const *user_argv,
		   char *const *user_envp, uint64_t *entry_out,
		   uint64_t *stack_out)
{
	struct task *task = sched_current();
	struct process *proc = task ? task->process : NULL;
	struct exec_strings strings;
	struct initramfs_file init_file;
	struct vfs_inode *vfs_ino = NULL;
	struct exec_image image;
	struct exec_load_info info;
	struct addr_space *old_space;
	struct addr_space *new_space = NULL;
	uint64_t stack;
	int err;

	if (!proc || !entry_out || !stack_out) {
		return -EINVAL;
	}

	err = exec_strings_capture(user_path, user_argv, user_envp, &strings);
	if (err) {
		return err;
	}

	err = open_exec_image(strings.path, &image, &init_file, &vfs_ino);
	if (err) {
		goto fail_strings;
	}

	err = elf64_validate_image(&image, &info);
	if (err) {
		goto fail_close;
	}

	new_space = vmm_create_space();
	if (!new_space) {
		err = -ENOMEM;
		goto fail_close;
	}

	err = elf64_load_image(new_space, &image, &info);
	if (err) {
		goto fail_space;
	}

	err = elf64_setup_initial_stack(new_space, &strings, &stack);
	if (err) {
		goto fail_space;
	}

	if (vfs_ino) {
		vfs_close(vfs_ino);
		vfs_ino = NULL;
	}

	old_space = proc->space;
	proc->space = new_space;
	proc->user_entry = info.entry;
	proc->user_stack = stack;
	proc->has_user_frame = false;
	vmm_switch_to(new_space);

	if (old_space && old_space != vmm_kernel_space()) {
		vmm_destroy_space(old_space);
	}

	exec_strings_release(&strings);
	*entry_out = info.entry;
	*stack_out = stack;
	return 0;

fail_space:
	vmm_destroy_space(new_space);
fail_close:
	if (vfs_ino) {
		vfs_close(vfs_ino);
	}
fail_strings:
	exec_strings_release(&strings);
	return err;
}
