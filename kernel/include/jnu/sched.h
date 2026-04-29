/*
 * include/jnu/sched.h — Scheduler stub.
 *
 * v0.0.1 has no scheduler. The placeholder is here so that callers
 * which will eventually need to coordinate with the scheduler (panic
 * suppression, locks, etc.) have a stable include path.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

void sched_init(void);
