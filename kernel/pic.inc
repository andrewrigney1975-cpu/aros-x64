; =============================================================================
; 8259 PIC remap + PIT timer (IRQ0), the last piece of "own our CPU"
; infrastructure before the memory allocator. IRQs land at vectors 32-47
; (clear of the 0-31 CPU exception range idt.inc already owns) so a real
; hardware interrupt can never be confused with a CPU fault.
; =============================================================================

; -------------------------------------------------------------------------
; pic_remap: standard 8259 initialization sequence. Masks every IRQ except
; IRQ2 (the master->slave cascade line) and IRQ0 (timer) to start.
; -------------------------------------------------------------------------
pic_remap:
    push rax

    mov al, 0x11                        ; ICW1: edge-triggered, cascade, ICW4 follows
    out 0x20, al
    out 0x80, al                        ; small delay (unused port)
    out 0xA0, al
    out 0x80, al

    mov al, 0x20                        ; ICW2: master IRQ0-7 -> vectors 0x20-0x27
    out 0x21, al
    out 0x80, al
    mov al, 0x28                        ; ICW2: slave IRQ8-15 -> vectors 0x28-0x2F
    out 0xA1, al
    out 0x80, al

    mov al, 0x04                        ; ICW3: master has a slave on IRQ2
    out 0x21, al
    out 0x80, al
    mov al, 0x02                        ; ICW3: slave's cascade identity
    out 0xA1, al
    out 0x80, al

    mov al, 0x01                        ; ICW4: 8086 mode
    out 0x21, al
    out 0x80, al
    out 0xA1, al
    out 0x80, al

    mov al, 0xFC                        ; mask all except IRQ0 (timer), IRQ2 (cascade)
    out 0x21, al
    mov al, 0xFF                        ; mask all on slave
    out 0xA1, al

    pop rax
    ret

; -------------------------------------------------------------------------
; pit_init: programs PIT channel 0 for ~100Hz (mode 3, square wave).
; -------------------------------------------------------------------------
PIT_HZ equ 100
PIT_DIVISOR equ 1193182 / PIT_HZ

pit_init:
    push rax

    mov al, 0x36
    out 0x43, al
    mov ax, PIT_DIVISOR
    out 0x40, al
    mov al, ah
    out 0x40, al

    pop rax
    ret

; -------------------------------------------------------------------------
; irq0_stub: timer ISR. Ticks the counter, then -- once the scheduler has
; been armed (current_task != NULL, see sched.inc) -- performs a full
; context switch every SCHED_QUANTUM ticks: save the interrupted task's
; RSP (which by now points at a complete GPR+interrupt frame on its own
; stack, built either by this same prologue or by task_create), ask
; sched_pick_next for the next task to run (skipping/reaping any
; terminated ones found along the way), switch to its own page tables
; (TCB_CR3, or the shared global ones for a task with no private paging),
; load its RSP, and let the shared pop+iretq tail resume it. Interrupts
; stay off for the whole handler (interrupt-gate entry clears IF
; automatically), so no nested timer tick can land mid-switch.
; -------------------------------------------------------------------------
irq0_stub:
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

    inc qword [rel timer_ticks]

    mov al, 0x20                        ; EOI
    out 0x20, al

    mov rax, [rel current_task]
    test rax, rax
    jz .no_switch                       ; scheduler not armed yet

    inc dword [rel sched_quantum_counter]
    cmp dword [rel sched_quantum_counter], SCHED_QUANTUM
    jl .no_switch
    mov dword [rel sched_quantum_counter], 0

    mov [rax+TCB_RSP], rsp              ; save the outgoing task's context
    call sched_pick_next                ; rax=outgoing (unchanged); rcx=next
    mov [rel current_task], rcx

    ; Switch to the incoming task's own page tables (its private stack
    ; mapping only exists there) before touching its RSP -- 0 means "no
    ; private paging" (the main task), which uses the shared global
    ; pml4. No stack access happens between this and the RSP load right
    ; below, so there's no window where CR3 and RSP could disagree about
    ; which task's stack is current.
    mov rdx, [rcx+TCB_CR3]
    test rdx, rdx
    jnz .use_task_cr3
    lea rdx, [rel pml4]
.use_task_cr3:
    mov cr3, rdx

    mov rsp, [rcx+TCB_RSP]              ; switch to the incoming task's stack

.no_switch:
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

; -------------------------------------------------------------------------
; irq_install: registers the timer handler and starts the PIC + PIT.
; Interrupts are left masked (no STI) until the caller is ready.
; -------------------------------------------------------------------------
irq_install:
    push rcx
    push rdx

    call pic_remap
    call pit_init

    mov rcx, 32
    lea rdx, [rel irq0_stub]
    call idt_set_gate

    pop rdx
    pop rcx
    ret

; =========================================================================
; Data
; =========================================================================
timer_ticks: dq 0
