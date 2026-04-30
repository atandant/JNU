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

	push r12			; preserve user r12 (also gives 16-byte align)
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
	; handlers (e.g. spawn / ELF load / ATA reads) would suppress the
	; LAPIC scheduler tick and starve the rest of the system.
	sti
	mov rdi, rsp			; struct syscall_args *
	call syscall_dispatch
	cli				; disable IRQs again before touching SYSRET state

	; Restore caller-saved registers from the saved frame.  Skipping
	; this step would leak whatever the C dispatcher left in
	; rdi/rsi/rdx/r10/r8/r9 back to userspace and break the syscall
	; ABI's "non-clobbered registers are preserved" promise.  rax
	; already holds the dispatcher return value.
	add rsp, 8			; discard saved nr (return value lives in rax)
	pop rdi
	pop rsi
	pop rdx
	pop r10
	pop r8
	pop r9
	pop r11				; user RFLAGS
	pop rcx				; user RIP

	; Stack now: [rsp]=saved user RSP, [rsp+8]=saved r12.
	; Restore r12 first (we still need rsp pointing at the slot to
	; load user RSP).
	mov r12, [rsp + 8]
	mov rsp, [rsp]			; switch back to user RSP

	swapgs				; restore user GS base
	o64 sysret
