; =============================================================================
; Minimal IDT + CPU exception handlers.
;
; Scope: vectors 0-31 (CPU exceptions) get a real handler that dumps
; vector/error-code/RIP/CR2 over serial and to the screen. Vectors
; 32-255 are left "not present" for now -- until the PIC/timer phase
; enables external interrupts, nothing should ever fire them, and if
; something unexpectedly does, an unhandled-vector #GP/#NP still goes
; through this same diagnostic path instead of a silent hang.
;
; This exists mainly so every subsystem built after it (exFAT, the
; allocator, ...) fails LOUDLY instead of the silent hangs that made the
; NVMe driver so painful to debug.
;
; What happens after the dump depends on whether the scheduler is armed
; (current_task != NULL, see sched.inc): if not, there's nothing else to
; run, so the system halts, same as always. If it IS armed, the fault
; only takes down the ONE task that caused it -- terminate it (same
; TCB_STATE flag task_exit uses) and switch to the next task via the
; exact same sched_pick_next the timer ISR uses, rather than halting
; the whole system over one task's bug. This works because a fault
; lands on the faulting task's own kernel stack (IST=0, no alternate
; exception stack) with a GPR frame this handler builds identically to
; irq0_stub's, so "switch to a different task's saved RSP and pop+iretq"
; is exactly as valid here as it is on a normal timer preemption.
; =============================================================================

; -------------------------------------------------------------------------
; idt_set_gate: RCX = vector (0-255), RDX = handler address.
; -------------------------------------------------------------------------
idt_set_gate:
    push rax
    push rbx
    push rdx

    mov rax, rcx
    shl rax, 4                          ; * 16 bytes/entry
    lea rbx, [rel idt_table]
    add rbx, rax

    mov [rbx+0], dx                     ; handler offset bits 0:15
    mov word [rbx+2], 0x08              ; selector = our flat code segment
    mov byte [rbx+4], 0                 ; IST = 0
    mov byte [rbx+5], 0x8E              ; present, DPL0, 64-bit interrupt gate
    mov rax, rdx
    shr rax, 16
    mov [rbx+6], ax                     ; handler offset bits 16:31
    mov rax, rdx
    shr rax, 32
    mov [rbx+8], eax                    ; handler offset bits 32:63
    mov dword [rbx+12], 0               ; reserved

    pop rdx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; idt_install: registers the 32 exception stubs and loads the IDT.
; -------------------------------------------------------------------------
idt_install:
    push rcx
    push rdx

%assign i 0
%rep 32
    mov rcx, i
    lea rdx, [rel isr_stub_ %+ i]
    call idt_set_gate
%assign i i+1
%endrep

    lea rax, [rel idt_descriptor]
    lidt [rax]

    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; ISR stubs: vectors that push their own error code (8,10-14,17,21,29,30)
; get ISR_ERR; everyone else gets a dummy 0 pushed first, so isr_common
; always sees a uniform [vector][errcode] pair on the stack.
; -------------------------------------------------------------------------
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Stack layout at entry to isr_common (after our vector/errcode pushes,
; below the CPU-pushed interrupt frame):
EXC_VECTOR  equ 120
EXC_ERRCODE equ 128
EXC_RIP     equ 136

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

    mov rcx, rsp                        ; RCX = saved-register block pointer
    call exception_dump

    mov rax, [rel current_task]
    test rax, rax
    jz .hang                            ; scheduler not armed -- nothing to switch to

    mov dword [rax+TCB_STATE], TCB_STATE_TERMINATED
    mov [rax+TCB_RSP], rsp              ; save the faulting frame (never resumed,
                                         ; but sched_pick_next only reads TCB_NEXT)
    call sched_pick_next                ; rax=outgoing (unchanged); rcx=next
    mov [rel current_task], rcx

    ; Switch to the surviving task's own page tables before touching its
    ; RSP -- see the matching comment in pic.inc's irq0_stub, which this
    ; mirrors exactly.
    mov rdx, [rcx+TCB_CR3]
    test rdx, rdx
    jnz .use_task_cr3
    lea rdx, [rel pml4]
.use_task_cr3:
    mov cr3, rdx

    mov rsp, [rcx+TCB_RSP]              ; switch to the surviving task's stack

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
    iretq

.hang:
    cli
    hlt
    jmp .hang

; -------------------------------------------------------------------------
; exception_dump: RCX = pointer to isr_common's saved-register block.
; -------------------------------------------------------------------------
exception_dump:
    push rax
    push rbx
    push rdx
    mov rbx, rcx                        ; rbx = frame pointer

    lea rcx, [rel msg_exc_banner]
    call serial_puts
    mov ecx, 4
    mov edx, 10
    lea r8, [rel msg_exc_banner]
    call fb_draw_string

    lea rcx, [rel msg_exc_v]
    call serial_puts
    mov rax, [rbx+EXC_VECTOR]
    call dbg_hex64

    lea rcx, [rel msg_exc_err]
    call serial_puts
    mov rax, [rbx+EXC_ERRCODE]
    call dbg_hex64

    lea rcx, [rel msg_exc_rip]
    call serial_puts
    mov rax, [rbx+EXC_RIP]
    call dbg_hex64

    lea rcx, [rel msg_exc_cr2]
    call serial_puts
    mov rax, cr2
    call dbg_hex64

    pop rdx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; dbg_hex64: dump RAX as 16 hex digits + CRLF over serial. General-purpose
; debug utility -- kept permanently (earlier phases kept hand-rolling and
; then deleting this exact routine for one-off debugging).
; -------------------------------------------------------------------------
dbg_hex64:
    push rax
    push rbx
    push rcx
    mov rbx, rax
    mov rcx, 60
.loop:
    mov rax, rbx
    shr rax, cl
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    call serial_putc
    sub rcx, 4
    jns .loop
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    pop rcx
    pop rbx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
msg_exc_banner: db 'unhandled exception', 0
msg_exc_v:      db 'EXCEPTION vector=', 0
msg_exc_err:    db 'errcode=', 0
msg_exc_rip:    db 'rip=', 0
msg_exc_cr2:    db 'cr2=', 0

align 16
idt_table: times (256*16) db 0
idt_table_end:

idt_descriptor:
    dw idt_table_end - idt_table - 1
    dq idt_table
