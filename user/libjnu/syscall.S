;
; user/libjnu/syscall.S - Native JNU syscall wrappers.
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only
;

bits 64

section .text

global jnu_syscall0
global jnu_syscall1
global jnu_syscall2
global jnu_syscall3

jnu_syscall0:
	mov rax, rdi
	syscall
	ret

jnu_syscall1:
	mov rax, rdi
	mov rdi, rsi
	syscall
	ret

jnu_syscall2:
	mov rax, rdi
	mov rdi, rsi
	mov rsi, rdx
	syscall
	ret

jnu_syscall3:
	mov rax, rdi
	mov rdi, rsi
	mov rsi, rdx
	mov rdx, rcx
	syscall
	ret
