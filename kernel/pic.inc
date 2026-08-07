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
; irq0_stub: timer ISR. Increments a tick counter and acknowledges the PIC.
; -------------------------------------------------------------------------
irq0_stub:
    push rax
    inc qword [rel timer_ticks]
    mov al, 0x20                        ; EOI
    out 0x20, al
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
