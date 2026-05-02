;
; kernel/arch/x86_64/syscall_entry.S - Early x86_64 syscall entry.
;
; The syscall scratch (saved user RSP + this task's kernel RSP) lives
; in a per-CPU struct addressed via the GS segment.  MSR_KERNEL_GS_BASE
; holds the address of that struct; SWAPGS makes it reachable as gs:[0]
; / gs:[8] from inside the kernel.  This replaces the old single-global
; `syscall_saved_user_rsp` and is the prerequisite for SMP — each CPU
; gets its own scratch when MSR_KERNEL_GS_BASE is set per-CPU.
;
; Every register that crosses the SYSRET boundary back to userspace is
; either restored from the saved frame (rdi, rsi, rdx, r10, r8, r9, rcx,
; r11, r12) or carries the explicit syscall return value (rax).  We do
; not leave kernel-side leftovers in caller-saved registers — that would
; leak kernel pointers (KASLR-class infoleak).
;
; Copyright (c) 2026 The JNU Authors.
; SPDX-License-Identifier: GPL-2.0-only
;

bits 64

global syscall_entry
extern syscall_dispatch

; struct syscall_scratch layout (matches arch_syscall.c):
;   gs:[0]  = saved user RSP
;   gs:[8]  = kernel stack top for the currently-scheduled task

section .text

; rax nr, rdi/rsi/rdx/r10/r8/r9 args. SYSCALL leaves user RIP in rcx,
; user RFLAGS in r11, and does not switch stacks; switch to the current
; task's kernel stack before touching C.
syscall_entry:
	swapgs				; GS base now points at this CPU's scratch
	mov [gs:0], rsp			; save user RSP into per-CPU slot
	mov rsp, [gs:8]			; switch to this task's kernel stack

	push r15			; preserve callee-saved user registers
	push r14
	push r13
	push rbp
	push rbx
	push r12
	mov r12, [gs:0]
	push r12			; saved user RSP, copied into the frame
	push rcx			; user RIP (for SYSRET)
	push r11			; user RFLAGS (for SYSRET)
	push r9				; arg5
	push r8				; arg4
	push r10			; arg3
	push rdx			; arg2
	push rsi			; arg1
	push rdi			; arg0
	push rax			; nr

	; Enable IRQs only after all user state is safely on the kernel
	; stack and the per-CPU scratch slot is no longer needed for
	; recovery.  Without this, every syscall ran with IF=0 — long
	; handlers (e.g. execve / ELF load / ATA reads) would suppress the
	; LAPIC scheduler tick and starve the rest of the system.
	sti
	mov rdi, rsp			; struct syscall_args *
	call syscall_dispatch
	cli				; disable IRQs again before touching SYSRET state

	; Restore user registers from the saved frame. rax already holds
	; the dispatcher return value.
	add rsp, 8			; discard saved nr (return value lives in rax)
	pop rdi
	pop rsi
	pop rdx
	pop r10
	pop r8
	pop r9
	pop r11				; user RFLAGS
	pop rcx				; user RIP

	; Stack now:
	;   [rsp+00] saved user RSP
	;   [rsp+08] r12, [rsp+16] rbx, [rsp+24] rbp,
	;   [rsp+32] r13, [rsp+40] r14, [rsp+48] r15.
	; Restore all saved registers before switching back to the user RSP.
	mov rbx, [rsp + 16]
	mov rbp, [rsp + 24]
	mov r13, [rsp + 32]
	mov r14, [rsp + 40]
	mov r15, [rsp + 48]
	mov r12, [rsp + 8]
	mov rsp, [rsp]			; switch back to user RSP

	swapgs				; restore user GS base
	o64 sysret
