; =============================================================================
; PCI configuration-space access (legacy port I/O mechanism #1: 0xCF8/0xCFC).
; Works on every x86-64 machine without needing ACPI/MCFG parsing, which we
; don't have yet -- ECAM/MMIO config access is a later-phase upgrade.
;
; Internal calling convention for this file only (not the MS x64 ABI): plain
; 32-bit registers, arguments/results as documented per routine. These are
; leaf-ish helpers called only from our own kernel code.
; =============================================================================

PCI_CONFIG_ADDR equ 0xCF8
PCI_CONFIG_DATA equ 0xCFC

; -------------------------------------------------------------------------
; pci_read32: ECX = config address (bus<<16 | dev<<11 | func<<8 | offset,
; offset dword-aligned). Returns EAX = the 32-bit value. Does not modify ECX.
; -------------------------------------------------------------------------
pci_read32:
    push rdx
    mov eax, ecx
    or eax, 0x80000000
    mov dx, PCI_CONFIG_ADDR
    out dx, eax
    mov dx, PCI_CONFIG_DATA
    in eax, dx
    pop rdx
    ret

; -------------------------------------------------------------------------
; pci_write32: ECX = config address, EAX = value to write. Does not modify
; ECX or EAX.
; -------------------------------------------------------------------------
pci_write32:
    push rax
    push rbx
    push rdx
    mov ebx, eax
    mov eax, ecx
    or eax, 0x80000000
    mov dx, PCI_CONFIG_ADDR
    out dx, eax
    mov eax, ebx
    mov dx, PCI_CONFIG_DATA
    out dx, eax
    pop rdx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; pci_find_class: brute-force scan of all 256 buses x 32 devices x 8
; functions for a device matching a Class/Subclass/ProgIF triple. Skips
; functions 1-7 on single-function devices (header-type bit 7 clear) to
; keep the scan fast.
;
; Input:  CL=class, CH=subclass, DL=progif
; Output: CF=0 and ECX=matching device's config address (offset 0) if
;         found; CF=1 if no matching device exists.
; -------------------------------------------------------------------------
pci_find_class:
    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12

    movzx r8d, cl                       ; wanted class
    mov eax, ecx
    shr eax, 8
    movzx r9d, al                       ; wanted subclass (CH is off-limits:
                                         ; high-byte regs can't mix with the
                                         ; REX prefix r9d's encoding needs)
    movzx r10d, dl                      ; wanted progif

    xor ebx, ebx                        ; bus = 0
.bus_loop:
    xor r11d, r11d                      ; dev = 0
.dev_loop:
    xor r12d, r12d                      ; func = 0
.func_loop:
    mov esi, ebx
    shl esi, 16
    mov edi, r11d
    shl edi, 11
    or esi, edi
    mov edi, r12d
    shl edi, 8
    or esi, edi                         ; esi = config addr, offset 0

    mov ecx, esi
    call pci_read32
    cmp ax, 0xFFFF                      ; vendor ID (low word)
    je .empty_slot

    mov ecx, esi
    add ecx, 0x08
    call pci_read32                     ; byte3=class,2=subclass,1=progif,0=revid
    mov edx, eax
    shr edx, 24
    cmp dl, r8b
    jne .no_match
    mov edx, eax
    shr edx, 16
    cmp dl, r9b
    jne .no_match
    mov edx, eax
    shr edx, 8
    cmp dl, r10b
    jne .no_match

    mov ecx, esi
    clc
    jmp .return

.no_match:
    cmp r12d, 0
    jne .next_func
    mov ecx, esi
    add ecx, 0x0C
    call pci_read32                     ; byte2 = header type
    shr eax, 16
    test al, 0x80                       ; multi-function bit
    jnz .next_func
    jmp .next_dev

.empty_slot:
    cmp r12d, 0
    jne .next_func
    jmp .next_dev

.next_func:
    inc r12d
    cmp r12d, 8
    jl .func_loop
.next_dev:
    inc r11d
    cmp r11d, 32
    jl .dev_loop
    inc ebx
    cmp ebx, 256
    jl .bus_loop

    stc
.return:
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax
    ret
