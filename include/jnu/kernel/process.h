/*
 * include/jnu/kernel/process.h - Process object scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/base/types.h>
#include <jnu/mm/vmm.h>
#include <jnu/user/fd.h>
#include <jnu/user/syscall.h>

struct task;

enum process_state {
	PROCESS_ALIVE,
	PROCESS_ZOMBIE,
};

struct process {
	int pid;
	enum process_state state;
	struct task *main_task;
	struct process *parent;
	struct process *first_child;
	struct process *next_sibling;
	int exit_status;
	struct fd_table fds;
	struct addr_space *space;
	uint64_t user_entry;
	uint64_t user_stack;
	bool has_user_frame;
	struct syscall_frame user_frame;
};

int process_alloc_pid(void);
void process_release_pid(int pid);

struct process *process_create_kernel(struct task *task);
void process_exit_current(int status);
int process_wait(int pid, int *status_out);
void process_destroy(struct process *proc);

/*
 * Register `proc` as the init process. Called once from main.c after
 * start_init() has loaded /init into a dedicated user process. Any
 * process that subsequently exits while it still has live (non-zombie)
 * children reparents those children to `proc` so they are not orphaned
 * onto a freed `parent` pointer. Init must outlive every other process;
 * if init itself ever exits, the kernel panics — there is nothing to
 * reparent its children onto.
 */
void process_set_init(struct process *proc);
struct process *process_get_init(void);

/*
 * Duplicate the calling process. Allocates a child process, deep-copies
 * the parent's address space and fd table, then schedules a user task
 * that resumes in userspace at `user_rip`/`user_rsp` with rax = 0.
 *
 * `*pid_out` receives the child pid in the parent on success. The
 * child does not "see" this pid because `process_fork` is the parent's
 * call: the child wakes up directly in userspace with the forged
 * register file. Returns 0 / -errno.
 */
int process_fork(const struct syscall_frame *frame, int *pid_out);

int process_selftest(void);
