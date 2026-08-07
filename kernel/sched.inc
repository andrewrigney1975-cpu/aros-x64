; =============================================================================
; Preemptive round-robin scheduler.
;
; Design: a task's saved state is just an RSP pointing into its own kernel
; stack, at a spot laid out to look EXACTLY like what pic.inc's timer ISR
; produces after its GPR-save prologue -- 15 pushed GPRs immediately below
; a CPU interrupt frame (RIP/CS/RFLAGS/RSP/SS). That means the ISR's
; "resume a task" path (pop everything, iretq) works identically whether
; the task was really interrupted mid-execution or has never run at all;
; task_create just hand-builds that same layout once, pointed at the
; task's entry point instead of some mid-function RIP.
;
; Tasks are kernel-only (ring 0, flat GDT selectors already in use
; elsewhere), share the one kernel address space (no per-task CR3 yet),
; and never terminate in this phase -- an infinite loop is the only
; supported task body. Real teardown (freeing a finished task's TCB and
; stack, unlinking it from the ring) is a follow-up once something
; actually needs threads that end.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

TCB_RSP   equ 0      ; saved stack pointer (0 until this task's first save)
TCB_NEXT  equ 8       ; next task in the circular ready list
TCB_STACK equ 16       ; base of this task's allocated kernel stack
TCB_SIZE  equ 24

SCHED_QUANTUM equ 2   ; timer ticks per task (~20ms at the 100Hz PIT rate)

; -------------------------------------------------------------------------
; task_create: RCX = entry point, RDX = argument (delivered in RDI when the
; task first runs), R8 = kernel stack size in bytes. Returns RAX = new
; TCB pointer, or 0 on failure. The new task is NOT linked into the ready
; ring or otherwise made runnable -- the caller does that (see kernel.asm).
; -------------------------------------------------------------------------
task_create:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14

    mov r12, rcx                        ; r12 = entry point
    mov r13, rdx                        ; r13 = argument
    mov r14, r8                         ; r14 = stack size

    mov rcx, TCB_SIZE
    call kmalloc
    test rax, rax
    jz .fail
    mov rbx, rax                        ; rbx = new TCB

    mov rcx, r14
    call kmalloc                        ; the task's kernel stack
    test rax, rax
    jz .fail_free_tcb
    mov [rbx+TCB_STACK], rax

    mov rsi, rax
    add rsi, r14                        ; rsi = stack top
    and rsi, ~15                        ; 16-byte align (defensive)

    ; Build the fake interrupt frame (20 qwords, 160 bytes) that irq0_stub
    ; will pop when this task is first switched to. Order matters: it must
    ; mirror irq0_stub's push order exactly (see pic.inc).
    lea rdi, [rsi - 160]                ; rdi = this task's initial RSP
    mov qword [rdi+0], 0                ; r15
    mov qword [rdi+8], 0                ; r14
    mov qword [rdi+16], 0               ; r13
    mov qword [rdi+24], 0               ; r12
    mov qword [rdi+32], 0               ; r11
    mov qword [rdi+40], 0               ; r10
    mov qword [rdi+48], 0               ; r9
    mov qword [rdi+56], 0               ; r8
    mov qword [rdi+64], 0               ; rbp
    mov [rdi+72], r13                   ; rdi (the saved GPR) = argument
    mov qword [rdi+80], 0               ; rsi
    mov qword [rdi+88], 0               ; rdx
    mov qword [rdi+96], 0               ; rcx
    mov qword [rdi+104], 0              ; rbx
    mov qword [rdi+112], 0              ; rax
    mov [rdi+120], r12                  ; RIP = entry point
    mov qword [rdi+128], 0x08           ; CS  = kernel code selector
    mov qword [rdi+136], 0x202          ; RFLAGS = IF set, reserved bit 1
    mov [rdi+144], rsi                  ; RSP = stack top
    mov qword [rdi+152], 0x10           ; SS  = kernel data selector

    mov [rbx+TCB_RSP], rdi
    mov qword [rbx+TCB_NEXT], 0

    mov rax, rbx
    jmp .out
.fail_free_tcb:
    mov rcx, rbx
    call kfree
.fail:
    xor eax, eax
.out:
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

; =========================================================================
; Data
; =========================================================================
current_task:          dq 0    ; NULL until the scheduler is armed; irq0_stub
                                ; leaves the timer as a plain tick counter
                                ; until this is set (see pic.inc)
sched_quantum_counter:  dd 0
