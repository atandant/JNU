;
; kernel/arch/x86_64/syscall_entry.S - Early x86_64 syscall entry.
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only
;

bits 64

global syscall_entry
global syscall_kernel_stack_top
extern syscall_dispatch

section .bss
align 8
syscall_kernel_stack_top:
	resq 1
syscall_saved_user_rsp:
	resq 1

section .text

; rax nr, rdi/rsi/rdx/r10/r8/r9 args. syscall leaves user RIP in rcx,
; user RFLAGS in r11, and does not switch stacks; switch to the current
; task's kernel stack before touching C.
syscall_entry:
	mov [rel syscall_saved_user_rsp], rsp
	mov rsp, [rel syscall_kernel_stack_top]

	push r12
	push qword [rel syscall_saved_user_rsp]
	push rcx
	push r11
	push r9
	push r8
	push r10
	push rdx
	push rsi
	push rdi
	push rax
	mov rdi, rsp
	call syscall_dispatch
	add rsp, 56

	pop r11
	pop rcx
	pop r10
	pop r12
	mov rsp, r10
	o64 sysret
