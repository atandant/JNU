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
global jnu_syscall4

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

; long jnu_syscall4(long nr, long a0, long a1, long a2, long a3);
; SysV: rdi=nr, rsi=a0, rdx=a1, rcx=a2, r8=a3
; SYSCALL: rax=nr, rdi=a0, rsi=a1, rdx=a2, r10=a3 (rcx is clobbered by RIP)
jnu_syscall4:
	mov rax, rdi
	mov rdi, rsi
	mov rsi, rdx
	mov rdx, rcx
	mov r10, r8
	syscall
	ret
