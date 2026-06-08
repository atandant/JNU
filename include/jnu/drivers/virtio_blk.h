/*
 * include/jnu/drivers/virtio_blk.h — VirtIO block device driver (PCI).
 *
 * Probes for a virtio-blk PCI function and registers it as "vda"
 * when present. Polled virtqueue I/O — no interrupts.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

void virtio_blk_init(void);
int virtio_blk_selftest(void);
