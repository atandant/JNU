/*
 * include/jnu/process.h - Process object scaffolding.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/fd.h>
#include <jnu/types.h>
#include <jnu/vmm.h>

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
};

int process_alloc_pid(void);
void process_release_pid(int pid);

struct process *process_create_kernel(struct task *task);
int process_spawn(const char *path, char *const *argv, int *pid_out);
void process_exit_current(int status);
int process_wait(int pid, int *status_out);
void process_destroy(struct process *proc);

int process_selftest(void);
