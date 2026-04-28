; kernel/arch/x86_64/boot.S — Limine entry stub.
;
; Limine hands control to `_start` already in long mode, with the
; kernel image mapped at its linker-script virtual address and a
; default stack provided. We rebind RSP to a kernel-owned 16 KiB
; stack, zero the frame pointer (so the eventual backtrace stops
; cleanly), clear DF (System V AMD64 ABI requirement), and call
; `kernel_main`. If `kernel_main` ever returns we halt forever.
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only

[BITS 64]
default rel

extern kernel_main

section .bss
align 16
boot_stack_bottom:
	resb 16384		; 16 KiB initial kernel stack
boot_stack_top:

section .text
global _start
_start:
	cli
	cld
	mov rsp, boot_stack_top
	xor rbp, rbp
	; SysV ABI: at the call site RSP must be 16-byte aligned, so the
	; pushed return address leaves the callee with (RSP+8) % 16 == 0.
	; boot_stack_top is 16-aligned by the .bss directive above.
	call kernel_main
.hang:
	cli
	hlt
	jmp .hang
