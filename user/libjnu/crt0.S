;
; user/libjnu/crt0.S - Minimal userspace entry.
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only
;

bits 64

extern main
extern exit

global _start

section .text
_start:
	xor rbp, rbp
	mov rdi, [rsp]
	lea rsi, [rsp + 8]
	lea rdx, [rsi + rdi * 8 + 8]
	call main
	mov rdi, rax
	call exit
	hlt
