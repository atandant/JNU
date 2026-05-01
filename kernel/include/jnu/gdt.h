/*
 * include/jnu/gdt.h — GDT, TSS, IST setup.
 *
 * 7-entry GDT with the order required for future SYSRET:
 *   0: null
 *   1: kernel CS  (selector 0x08)
 *   2: kernel DS  (selector 0x10)
 *   3: user DS    (selector 0x18)
 *   4: user CS    (selector 0x20)
 *   5/6: TSS-low / TSS-high (one 16-byte system descriptor)
 *
 * The TSS holds RSP0 (kernel stack for ring-3→ring-0 entry, unused in
 * v0.0.1 — there is no userspace yet) and seven IST stacks. We dedicate
 * IST slots to the high-trust exceptions per §2.4:
 *   IST1: #DF (double fault)
 *   IST2: #NMI
 *   IST3: #MC (machine check)
 *   IST4: #PF (page fault)
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <jnu/types.h>

#define GDT_NULL 0x00
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS 0x18
#define GDT_USER_CS 0x20
#define GDT_TSS 0x28 /* 16-byte system descriptor */

#define IST_NONE 0
#define IST_DF 1
#define IST_NMI 2
#define IST_MC 3
#define IST_PF 4

void gdt_init(void);

/*
 * Update RSP0 in the active TSS. Called whenever a thread switches
 * onto a fresh kernel stack. v0.0.1 has no scheduler so this is set
 * once at boot to the boot stack top.
 */
void tss_set_rsp0(uint64_t rsp0);
