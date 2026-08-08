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

; -------------------------------------------------------------------------
; pci_find_capability: walks a device's PCI capability list looking for a
; given capability ID (e.g. 0x05 = MSI, 0x11 = MSI-X). Needed before this
; file's own MSI/MSI-X setup can find where in a device's config space to
; program the message address/data registers.
;
; Input:  ECX = device's config address (offset 0, as returned by
;         pci_find_class), DL = capability ID to find.
; Output: CF=0 and EAX=that capability's own config address (its ID byte,
;         ready to pass straight to pci_read32/pci_write32 with further
;         offsets added), if found; CF=1 if the device has no capability
;         list, or none of its entries match.
; -------------------------------------------------------------------------
pci_find_capability:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8

    mov esi, ecx                        ; esi = device's config base
    movzx r8d, dl                       ; r8b = wanted capability ID

    mov ecx, esi
    add ecx, 0x04                       ; Command(lo)/Status(hi)
    call pci_read32
    shr eax, 16                         ; eax = Status register
    test eax, 0x0010                    ; Capabilities List supported?
    jz .not_found

    mov ecx, esi
    add ecx, 0x34                       ; Capabilities Pointer (byte 0)
    call pci_read32
    movzx ebx, al                       ; ebx = next capability offset

    mov edx, 48                         ; defensive loop bound -- config
                                         ; space is 256 bytes, >=4 per
                                         ; entry, so the list can never
                                         ; legitimately need more hops
.walk:
    test ebx, ebx
    jz .not_found
    test edx, edx
    jz .not_found
    dec edx

    mov ecx, esi
    add ecx, ebx
    call pci_read32                     ; byte0=cap ID, byte1=next pointer
    movzx r9d, al
    cmp r9b, r8b
    je .found

    mov ebx, eax
    shr ebx, 8
    and ebx, 0xFF
    jmp .walk

.found:
    mov eax, esi
    add eax, ebx
    clc
    jmp .out
.not_found:
    stc
.out:
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

PCI_CAP_MSI equ 0x05

; -------------------------------------------------------------------------
; pci_enable_msi: RCX = device's config address (offset 0), DL = target
; interrupt vector (0-255). Finds the device's MSI capability (the plain
; one, ID 0x05 -- not MSI-X, ID 0x11, which has a different per-vector
; table layout and is a separate routine if a device ever needs it) and
; programs it to fire that vector at this CPU's local APIC, single
; message, edge-triggered fixed delivery: Message Address encodes the
; destination local APIC (lapic_id), Message Data is just the vector
; with delivery/trigger mode bits left at their edge/fixed defaults.
; Handles both the 32-bit and 64-bit-capable Message Address layouts
; (the capability's own Message Control register says which one a given
; device implements). Returns EAX=1/0 -- 0 if the device has no plain
; MSI capability at all.
; -------------------------------------------------------------------------
pci_enable_msi:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    mov esi, ecx                        ; esi = device's config base
    movzx r8d, dl                       ; r8b = target vector

    mov dl, PCI_CAP_MSI
    call pci_find_capability
    jc .fail
    mov ebx, eax                        ; ebx = MSI capability's config addr

    mov ecx, ebx
    call pci_read32                     ; eax = CapID|Next|Control dword
    mov r9d, eax                        ; r9d = saved for the enable-bit write-back
    test eax, 0x00800000                ; Control bit7 (dword bit 23) =
                                         ; 64-bit Address Capable
    jnz .cap64

    ; 32-bit layout: Message Address at +4, Message Data at +8
    call lapic_id
    shl eax, 12
    or eax, 0xFEE00000
    mov ecx, ebx
    add ecx, 4
    call pci_write32

    mov eax, r8d                        ; Message Data = vector (fixed,
                                         ; edge -- all other bits 0)
    mov ecx, ebx
    add ecx, 8
    call pci_write32
    jmp .enable

.cap64:
    ; 64-bit-capable layout: Message Address at +4, Address-Upper at +8,
    ; Message Data at +12
    call lapic_id
    shl eax, 12
    or eax, 0xFEE00000
    mov ecx, ebx
    add ecx, 4
    call pci_write32

    xor eax, eax
    mov ecx, ebx
    add ecx, 8
    call pci_write32

    mov eax, r8d
    mov ecx, ebx
    add ecx, 12
    call pci_write32

.enable:
    mov eax, r9d
    or eax, 0x00010000                  ; Control bit0 (dword bit 16) =
                                         ; MSI Enable
    mov ecx, ebx
    call pci_write32

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret
