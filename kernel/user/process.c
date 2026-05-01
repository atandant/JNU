/*
 * kernel/user/process.c - Process and PID scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/errno.h>
#include <jnu/exec.h>
#include <jnu/fd.h>
#include <jnu/initramfs.h>
#include <jnu/kmalloc.h>
#include <jnu/process.h>
#include <jnu/sched.h>
#include <jnu/spinlock.h>
#include <jnu/types.h>
#include <jnu/vfs.h>
#include <jnu/vmm.h>

static int next_pid = 1;
static struct spinlock process_tree_lock = SPINLOCK_INITIALIZER;

int process_alloc_pid(void)
{
	int pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
	if (pid <= 0) {
		return -ENOMEM;
	}
	return pid;
}

void process_release_pid(int pid) { (void)pid; }

struct process *process_create_kernel(struct task *task)
{
	struct process *proc = kzalloc(sizeof(*proc));

	if (!proc) {
		return NULL;
	}

	proc->pid = task->pid;
	proc->state = PROCESS_ALIVE;
	proc->main_task = task;
	fd_table_init(&proc->fds);
	proc->space = vmm_kernel_space();
	task->process = proc;
	return proc;
}

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

int process_spawn(const char *path, char *const *argv, int *pid_out)
{
	struct task *parent_task = sched_current();
	struct process *parent;
	struct process *child;
	struct initramfs_file init_file;
	struct vfs_inode *vfs_ino;
	struct exec_image image;
	struct exec_load_info info;
	int err;

	(void)argv;

	if (!path || !pid_out || !parent_task || !parent_task->process) {
		return -EINVAL;
	}

	parent = parent_task->process;
	child = kzalloc(sizeof(*child));
	if (!child) {
		return -ENOMEM;
	}

	child->pid = process_alloc_pid();
	if (child->pid < 0) {
		err = child->pid;
		/* pr_info("process_spawn: alloc_pid failed (err=%d)\n", err);
		 */
		goto fail_child;
	}
	child->state = PROCESS_ALIVE;
	child->parent = parent;
	fd_table_init(&child->fds);

	child->space = vmm_create_space();
	if (!child->space) {
		err = -ENOMEM;
		/* pr_info("process_spawn: vmm_create_space failed\n"); */
		goto fail_pid;
	}

	err = open_exec_image(path, &image, &init_file, &vfs_ino);
	if (err) {
		/* pr_info("process_spawn: open_exec_image failed for path '%s'
		   (err=%d)\n", path, err); */
		goto fail_space;
	}

	err = elf64_load_image(child->space, &image, &info);
	if (!err) {
		err =
		    elf64_setup_initial_stack(child->space, &child->user_stack);
	}
	if (vfs_ino) {
		vfs_close(vfs_ino);
	}
	if (err) {
		/* pr_info("process_spawn: elf64 load/stack failed (err=%d)\n",
		 * err); */
		goto fail_space;
	}

	child->user_entry = info.entry;
	err = sched_create_user_task(path, child, NULL);
	if (err) {
		/* pr_info("process_spawn: sched_create_user_task failed
		 * (err=%d)\n", err); */
		goto fail_space;
	}

	uint64_t flags = spin_lock_irqsave(&process_tree_lock);
	child->next_sibling = parent->first_child;
	parent->first_child = child;
	spin_unlock_irqrestore(&process_tree_lock, flags);

	*pid_out = child->pid;
	return 0;

fail_space:
	vmm_destroy_space(child->space);
fail_pid:
	process_release_pid(child->pid);
fail_child:
	kfree(child);
	return err;
}

void process_exit_current(int status)
{
	struct task *task = sched_current();
	struct process *proc = task ? task->process : NULL;
	uint64_t flags;

	if (!proc) {
		return;
	}

	for (int fd = 0; fd < JNU_MAX_FDS; fd++) {
		file_destroy(fd_close(&proc->fds, fd));
	}

	flags = spin_lock_irqsave(&process_tree_lock);
	proc->exit_status = status & 0xFF;
	proc->state = PROCESS_ZOMBIE;
	if (proc->parent && proc->parent->main_task) {
		sched_wake(proc->parent->main_task);
	}
	spin_unlock_irqrestore(&process_tree_lock, flags);
}

static struct process *find_child(struct process *parent, int pid)
{
	for (struct process *child = parent->first_child; child;
	     child = child->next_sibling) {
		if (pid == -1 || child->pid == pid) {
			return child;
		}
	}
	return NULL;
}

static void unlink_child(struct process *parent, struct process *target)
{
	struct process **link = &parent->first_child;

	while (*link) {
		if (*link == target) {
			*link = target->next_sibling;
			target->next_sibling = NULL;
			target->parent = NULL;
			return;
		}
		link = &(*link)->next_sibling;
	}
}

int process_wait(int pid, int *status_out)
{
	struct task *task = sched_current();
	struct process *parent = task ? task->process : NULL;
	struct process *child;
	uint64_t flags;

	if (!parent || (pid != -1 && pid <= 0)) {
		return -EINVAL;
	}

	for (;;) {
		flags = spin_lock_irqsave(&process_tree_lock);
		child = find_child(parent, pid);
		if (!child) {
			spin_unlock_irqrestore(&process_tree_lock, flags);
			return -ECHILD;
		}
		if (child->state == PROCESS_ZOMBIE) {
			int child_pid = child->pid;

			if (status_out) {
				*status_out = child->exit_status;
			}
			unlink_child(parent, child);
			spin_unlock_irqrestore(&process_tree_lock, flags);
			return child_pid;
		}
		spin_unlock_irqrestore(&process_tree_lock, flags);
		sched_sleep_current();
	}
}

void process_destroy(struct process *proc) { kfree(proc); }

int process_selftest(void)
{
	int pid = process_alloc_pid();

	if (pid <= 0) {
		return -EINVAL;
	}
	process_release_pid(pid);
	return 0;
}
