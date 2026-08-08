; =============================================================================
; xHCI (USB 2/3/4 host controller) driver -- bring-up in progress.
;
; Scope for this phase: locate the controller over PCI, map its MMIO BAR,
; and read the Capability Registers far enough to report the controller's
; shape (version, slot/port counts, operational-register offset). No
; state-changing operations on the controller yet -- reset, event ring,
; command ring, and device slot bring-up are the next slice of work, kept
; separate and tested independently since a mistake in controller reset
; is much harder to diagnose than a mistake in a read-only register dump.
;
; xHCI is the ONLY USB host controller interface this kernel targets:
; every 8th-gen-or-later Intel platform (the project's stated scope)
; exposes USB exclusively through it, handling low/full/high/super-speed
; devices uniformly. Legacy UHCI/OHCI/EHCI controllers don't exist on
; this hardware and aren't implemented here.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

; -------------------------------------------------------------------------
; xhci_find_and_probe: locates the xHCI controller over PCI (class 0x0C,
; subclass 0x03, prog-if 0x30), enables Memory Space + Bus Master, maps
; its MMIO BAR (BAR0/BAR1, a 64-bit BAR per spec), and reads just enough
; of the Capability Registers to confirm it's alive and report its
; shape. Returns EAX=1/0.
; -------------------------------------------------------------------------
xhci_find_and_probe:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    mov cl, 0x0C                        ; class    = serial bus controller
    mov ch, 0x03                        ; subclass = USB
    mov dl, 0x30                        ; progif   = xHCI
    call pci_find_class
    jc .not_found

    mov esi, ecx                        ; esi = device's config-space base
    mov [rel xhci_pci_addr], esi

    mov ecx, esi
    add ecx, 0x04                       ; PCI command register
    call pci_read32
    or eax, 0x00000006                  ; Memory Space Enable | Bus Master Enable
    call pci_write32

    ; BAR0/BAR1: a 64-bit MMIO BAR (bits 1-2 of BAR0 = 0b10 marks this).
    mov ecx, esi
    add ecx, 0x10                       ; BAR0
    call pci_read32
    and eax, 0xFFFFFFF0
    mov ebx, eax                        ; ebx = BAR0 low 32 bits

    mov ecx, esi
    add ecx, 0x14                       ; BAR1 (BAR0's upper 32 bits)
    call pci_read32
    mov r8d, eax                        ; r8d = BAR0 high 32 bits

    mov rax, r8
    shl rax, 32
    or rax, rbx                         ; rax = full 64-bit MMIO base
    mov [rel xhci_mmio_base], rax

    mov rbx, rax                        ; rbx = MMIO base
    movzx eax, byte [rbx+0x00]          ; CAPLENGTH
    mov [rel xhci_caplength], al
    movzx eax, word [rbx+0x02]          ; HCIVERSION
    mov [rel xhci_hciversion], ax
    mov eax, [rbx+0x04]                 ; HCSPARAMS1
    mov [rel xhci_hcsparams1], eax

    mov eax, 1
    jmp .out
.not_found:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; =========================================================================
; Data
; =========================================================================
xhci_pci_addr:    dd 0
xhci_mmio_base:   dq 0
xhci_caplength:   db 0
xhci_hciversion:  dw 0
xhci_hcsparams1:  dd 0
