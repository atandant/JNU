; kernel/arch/x86_64/isr.S — 256 IDT stubs and the common dispatcher.
;
; Vectors that the CPU pushes a hardware error code for (8, 10–14, 17,
; 21, 29, 30) jump to a stub that pushes only the vector number; all
; others jump to a stub that pushes a fake zero error code first, then
; the vector number. Both arms then fall through to `isr_common`, which
; saves all 15 GPRs in `struct cpu_state` order and tail-calls
; `interrupt_dispatch` with a pointer to that frame.
;
; Stack layout at entry to `interrupt_dispatch`:
;
;   +----------------------+ <- highest address
;   |        SS            |  pushed by CPU
;   |        RSP           |
;   |        RFLAGS        |
;   |        CS            |
;   |        RIP           |
;   |        error_code    |  CPU or fake 0
;   |        vector        |  pushed by stub
;   |        rax           |  pushed by isr_common
;   |        rbx           |
;   |        rcx           |
;   |        rdx           |
;   |        rsi           |
;   |        rdi           |
;   |        rbp           |
;   |        r8..r15       |
;   +----------------------+ <- RSP, passed as RDI to dispatch
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only

[BITS 64]
default rel

extern interrupt_dispatch
global isr_table

section .text

; --- common tail ------------------------------------------------------------
isr_common:
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15

	; If the trap came from user (cs & 3 == 3), swap GS so kernel
	; reads the kernel per-CPU pointer. v0.0.1 has no userspace
	; yet, but the path is wired now to keep the IDT side complete.
	mov rax, [rsp + 15*8 + 24]	; saved CS
	test rax, 3
	jz .no_swap_in
	swapgs
.no_swap_in:

	cld
	mov rdi, rsp			; pointer to struct cpu_state
	call interrupt_dispatch

	mov rax, [rsp + 15*8 + 24]	; saved CS
	test rax, 3
	jz .no_swap_out
	swapgs
.no_swap_out:

	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rbp
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rbx
	pop rax

	add rsp, 16			; vector + error_code
	iretq

; --- per-vector stubs -------------------------------------------------------
; Each stub is exactly the same shape; we expand them with NASM macros so
; the IDT can be built from a flat `isr_table` of stub addresses.

%macro ISR_NOERR 1
isr_stub_%1:
	push qword 0			; fake error code
	push qword %1
	jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
	push qword %1
	jmp isr_common
%endmacro

; Vectors 0–31: architectural exceptions. CPU-pushed error codes for
; 8, 10, 11, 12, 13, 14, 17, 21, 29, 30; all others get a fake 0.

%assign v 0
%rep 32
%if v == 8 || v == 10 || v == 11 || v == 12 || v == 13 || v == 14 || v == 17 || v == 21 || v == 29 || v == 30
	ISR_ERR v
%else
	ISR_NOERR v
%endif
%assign v v+1
%endrep

; Vectors 32–255: external IRQs and software vectors. None of these have
; CPU-pushed error codes.
%assign v 32
%rep (256 - 32)
	ISR_NOERR v
%assign v v+1
%endrep

; --- table of stub addresses ------------------------------------------------
section .rodata
align 8
isr_table:
%assign v 0
%rep 256
	dq isr_stub_ %+ v
%assign v v+1
%endrep
