; kernel/arch/x86_64/context.S - Callee-saved kernel context switch.
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only

[BITS 64]
default rel

global context_switch

section .text

; void context_switch(struct context *prev, struct context *next)
; rdi = prev, rsi = next
context_switch:
	mov [rdi + 0x00], r15
	mov [rdi + 0x08], r14
	mov [rdi + 0x10], r13
	mov [rdi + 0x18], r12
	mov [rdi + 0x20], rbx
	mov [rdi + 0x28], rbp
	mov [rdi + 0x30], rsp
	lea rax, [rel .resume]
	mov [rdi + 0x38], rax
	mov [rdi + 0x40], rdi

	mov r15, [rsi + 0x00]
	mov r14, [rsi + 0x08]
	mov r13, [rsi + 0x10]
	mov r12, [rsi + 0x18]
	mov rbx, [rsi + 0x20]
	mov rbp, [rsi + 0x28]
	mov rsp, [rsi + 0x30]
	mov rdi, [rsi + 0x40]
	jmp qword [rsi + 0x38]

.resume:
	ret
