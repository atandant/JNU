/*
 * include/jnu/serial.h — COM1 16550 UART, polling output.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

/*
 * Bring up COM1 (port 0x3F8, 115200 8N1) and register it as a klog
 * backend. Safe to call before klog_init: writes go nowhere until a
 * backend exists, and `serial_init` registers itself only after it has
 * configured the line.
 */
void serial_init(void);

/* Polling write of `len` bytes to COM1. Always blocks until drained. */
void serial_write(const char *buf, size_t len);
