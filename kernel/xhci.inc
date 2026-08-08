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

    movzx ecx, byte [rel xhci_caplength]
    mov rax, rbx
    add rax, rcx
    mov [rel xhci_op_base], rax         ; Operational Registers base

    mov ecx, [rbx+0x18]                 ; RTSOFF (bits 0-4 reserved, must
    and ecx, 0xFFFFFFE0                 ; be masked off per spec)
    mov rax, rbx
    add rax, rcx
    mov [rel xhci_rt_base], rax         ; Runtime Registers base

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

; -------------------------------------------------------------------------
; xhci_reset_and_start: stops (if running) and resets the controller,
; enables a modest number of device slots, and programs the Device
; Context Base Address Array, Command Ring, and primary Event Ring --
; then starts the controller (USBCMD.RUN) and confirms it actually left
; the halted state. No device slots are enabled or commands issued yet;
; this only proves the controller responds correctly to the bring-up
; sequence itself.
;
; Every wait loop here is bounded rather than open-ended: a controller
; (real or emulated) that never clears a status bit we're waiting on is
; a real possibility, and hanging the whole boot sequence over it would
; turn "USB isn't working yet" into "the kernel doesn't boot at all".
;
; Every register base this routine needs (op/runtime registers) is
; recomputed from the stored MMIO base + capability offsets rather than
; kept alive across calls in a register, and every scratch buffer is
; zeroed immediately before use rather than assumed clean -- this
; routine is long enough that a register surviving across the wrong
; stretch of code has already caused real bugs elsewhere this session.
;
; Returns EAX=1/0.
; -------------------------------------------------------------------------
XHCI_MAX_SLOTS equ 8                    ; a modest, deliberately small cap
                                         ; -- this phase proves the ring
                                         ; mechanism works, not maximum
                                         ; device fan-out

xhci_reset_and_start:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov rbx, [rel xhci_op_base]

    ; Stop, if the controller is currently running.
    mov eax, [rbx+0x00]                 ; USBCMD
    and eax, ~0x00000001                ; clear R/S
    mov [rbx+0x00], eax

    mov r9d, 5000000
.wait_halt:
    mov eax, [rbx+0x04]                 ; USBSTS
    test eax, 0x00000001                ; HCH (HC Halted)
    jnz .halted
    dec r9d
    jnz .wait_halt
    jmp .fail
.halted:

    ; Reset. HCRST self-clears when the reset completes.
    mov eax, [rbx+0x00]
    or eax, 0x00000002                  ; HCRST
    mov [rbx+0x00], eax

    mov r9d, 5000000
.wait_reset:
    mov eax, [rbx+0x00]
    test eax, 0x00000002
    jz .reset_done
    dec r9d
    jnz .wait_reset
    jmp .fail
.reset_done:

    mov r9d, 5000000
.wait_cnr:
    mov eax, [rbx+0x04]                 ; USBSTS
    test eax, 0x00000800                ; CNR (Controller Not Ready)
    jz .ready
    dec r9d
    jnz .wait_cnr
    jmp .fail
.ready:

    ; Enable a modest number of device slots.
    mov eax, [rel xhci_hcsparams1]
    and eax, 0xFF                       ; MaxSlots
    cmp eax, XHCI_MAX_SLOTS
    jbe .slots_ok
    mov eax, XHCI_MAX_SLOTS
.slots_ok:
    mov [rel xhci_slots_enabled], eax
    mov rbx, [rel xhci_op_base]
    mov [rbx+0x38], eax                 ; CONFIG.MaxSlotsEn

    ; Device Context Base Address Array: zeroed (every slot starts
    ; unused), DCBAAP points at it.
    lea rdi, [rel xhci_dcbaa]
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosb

    mov rbx, [rel xhci_op_base]
    lea rax, [rel xhci_dcbaa]
    mov [rbx+0x30], rax                 ; DCBAAP

    ; Command ring: zeroed, then a Link TRB at the last of 256 slots
    ; pointing back to the start with the Toggle Cycle bit set, so it's
    ; a genuine ring rather than a one-shot list. Cycle bit starts at 1,
    ; matching RCS=1 below.
    lea rdi, [rel xhci_cmdring]
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosb

    lea rsi, [rel xhci_cmdring]
    lea rdi, [rel xhci_cmdring]
    add rdi, 4096 - 16                  ; last of 256 16-byte TRB slots
    mov [rdi+0], rsi                    ; Link TRB Parameter = ring base
    mov dword [rdi+8], 0                ; Status
    mov dword [rdi+12], (6 << 10) | 0x02 | 0x01  ; Type=Link(6), TC=1, Cycle=1

    lea rax, [rel xhci_cmdring]
    or al, 1                            ; RCS = 1 (initial cycle state)
    mov rbx, [rel xhci_op_base]
    mov [rbx+0x18], rax                 ; CRCR
    mov qword [rel xhci_cmdring_cycle], 1
    mov qword [rel xhci_cmdring_enqueue], 0

    ; Primary event ring: one segment (256 TRBs), one Event Ring
    ; Segment Table entry describing it.
    lea rdi, [rel xhci_evtring]
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosb

    lea rdi, [rel xhci_erst]
    mov ecx, 16
    xor eax, eax
    cld
    rep stosb

    lea rax, [rel xhci_evtring]
    lea rdi, [rel xhci_erst]
    mov [rdi+0], rax                    ; Ring Segment Base Address
    mov dword [rdi+8], 256              ; Ring Segment Size, in TRBs
    mov dword [rdi+12], 0               ; reserved

    mov rbx, [rel xhci_rt_base]
    add rbx, 0x20                       ; Interrupter Register Set 0

    mov dword [rbx+0x08], 1             ; ERSTSZ = 1 segment
    lea rax, [rel xhci_erst]
    mov [rbx+0x10], rax                 ; ERSTBA
    lea rax, [rel xhci_evtring]         ; ERDP -- dequeue pointer starts
    mov [rbx+0x18], rax                 ; at the segment's first TRB
    mov qword [rel xhci_evtring_dequeue], 0
    mov qword [rel xhci_evtring_cycle], 1

    mov eax, [rbx+0x00]                 ; IMAN
    or eax, 0x00000002                  ; Interrupt Enable
    mov [rbx+0x00], eax

    ; Start the controller.
    mov rbx, [rel xhci_op_base]
    mov eax, [rbx+0x00]                 ; USBCMD
    or eax, 0x00000005                  ; R/S | INTE (bits 0 and 2)
    mov [rbx+0x00], eax

    mov r9d, 5000000
.wait_run:
    mov eax, [rbx+0x04]                 ; USBSTS
    test eax, 0x00000001                ; HCH should now be clear
    jz .running
    dec r9d
    jnz .wait_run
    jmp .fail
.running:

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rdi
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
xhci_op_base:     dq 0
xhci_rt_base:     dq 0
xhci_caplength:   db 0
xhci_hciversion:  dw 0
xhci_hcsparams1:  dd 0
xhci_slots_enabled: dd 0

xhci_cmdring_cycle:    dq 0
xhci_cmdring_enqueue:  dq 0
xhci_evtring_cycle:    dq 0
xhci_evtring_dequeue:  dq 0

align 4096
xhci_dcbaa:   times 4096 db 0
align 4096
xhci_cmdring: times 4096 db 0
align 4096
xhci_evtring: times 4096 db 0
align 64
xhci_erst:    times 16 db 0
