; =============================================================================
; Minimal local APIC bring-up.
;
; Scope: just enough to receive MSI/MSI-X interrupts and acknowledge them.
; MSI delivery doesn't go through the legacy 8259 PIC at all -- a device
; writes its message address (which encodes the target local APIC) and
; message data (which encodes the vector) directly, and the CPU's own
; local APIC is what turns that into an ordinary interrupt-gate dispatch.
; That means two things have to be true before any device's MSI can ever
; arrive: the local APIC must actually be enabled (both the MSR-level
; "hardware" enable and the Spurious-Interrupt-Vector-Register software
; enable -- firmware usually leaves both set, but this kernel doesn't
; trust firmware state it hasn't verified itself elsewhere either), and
; whoever's MSI ISR fires has to acknowledge it via the local APIC's own
; EOI register, not the PIC's -- there's no PIC involved to send one to.
;
; No IOAPIC/ACPI MADT parsing here -- single-CPU (BSP-only) target, and
; MSI/MSI-X target a specific local APIC directly by ID, which sidesteps
; needing IOAPIC redirection entries entirely.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

IA32_APIC_BASE_MSR equ 0x1B
APIC_BASE_ENABLE    equ 0x800             ; bit 11: APIC Global Enable

LAPIC_REG_ID   equ 0x020                  ; Local APIC ID register
LAPIC_REG_EOI  equ 0x0B0
LAPIC_REG_SVR  equ 0x0F0                  ; Spurious-Interrupt-Vector Register

LAPIC_SVR_ENABLE equ 0x100                ; bit 8: APIC Software Enable
LAPIC_SPURIOUS_VECTOR equ 0xFF            ; conventional choice: low nibble
                                           ; all 1s, matches every real-world
                                           ; OS's APIC bring-up

; -------------------------------------------------------------------------
; lapic_init: reads IA32_APIC_BASE, verifies/sets the MSR-level enable
; bit, records the local APIC's MMIO base (already covered by the
; identity map paging.inc built), and turns on the Spurious-Interrupt-
; Vector Register's software enable -- without that bit, the local APIC
; drops every interrupt, MSI included, even though it's otherwise "on".
; -------------------------------------------------------------------------
lapic_init:
    push rax
    push rbx
    push rcx
    push rdx

    mov ecx, IA32_APIC_BASE_MSR
    rdmsr                                ; edx:eax = MSR value

    test eax, APIC_BASE_ENABLE
    jnz .enabled
    or eax, APIC_BASE_ENABLE
    wrmsr
.enabled:

    mov ebx, eax
    and ebx, 0xFFFFF000                  ; bits 12-31 = MMIO base (this
                                          ; kernel never runs above 4GB
                                          ; physical for the APIC itself,
                                          ; so the high MSR bits are 0)
    mov [rel lapic_base], rbx

    mov eax, [rbx+LAPIC_REG_SVR]
    or eax, LAPIC_SVR_ENABLE
    or eax, LAPIC_SPURIOUS_VECTOR
    mov [rbx+LAPIC_REG_SVR], eax

    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; lapic_id: returns EAX = this CPU's local APIC ID (bits 24-31 of the ID
; register -- xAPIC mode layout).
; -------------------------------------------------------------------------
lapic_id:
    push rbx
    mov rbx, [rel lapic_base]
    mov eax, [rbx+LAPIC_REG_ID]
    shr eax, 24
    and eax, 0xFF
    pop rbx
    ret

; -------------------------------------------------------------------------
; lapic_eoi: acknowledges the current interrupt at the local APIC. Must
; be used instead of the PIC's `out 0x20, al` EOI by any ISR reached via
; MSI/MSI-X, since that path never goes through the 8259 at all.
; -------------------------------------------------------------------------
lapic_eoi:
    push rax
    push rbx
    mov rbx, [rel lapic_base]
    xor eax, eax
    mov [rbx+LAPIC_REG_EOI], eax
    pop rbx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
lapic_base: dq 0
