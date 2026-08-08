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
    mov eax, [rbx+0x10]                 ; HCCPARAMS1
    mov [rel xhci_hccparams1], eax
    and eax, 0x00000004                 ; CSZ (bit 2): 0 = 32-byte contexts,
                                         ; 1 = 64-byte contexts
    jz .csz32
    mov dword [rel xhci_context_size], 64
    jmp .csz_done
.csz32:
    mov dword [rel xhci_context_size], 32
.csz_done:

    movzx ecx, byte [rel xhci_caplength]
    mov rax, rbx
    add rax, rcx
    mov [rel xhci_op_base], rax         ; Operational Registers base

    mov ecx, [rbx+0x18]                 ; RTSOFF (bits 0-4 reserved, must
    and ecx, 0xFFFFFFE0                 ; be masked off per spec)
    mov rax, rbx
    add rax, rcx
    mov [rel xhci_rt_base], rax         ; Runtime Registers base

    mov ecx, [rbx+0x14]                 ; DBOFF (bits 0-1 reserved)
    and ecx, 0xFFFFFFFC
    mov rax, rbx
    add rax, rcx
    mov [rel xhci_db_base], rax         ; Doorbell Array base

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

; -------------------------------------------------------------------------
; xhci_scan_ports: reads PORTSC for every port the controller reports
; (HCSPARAMS1's MaxPorts field) and records which ones currently have a
; device connected (CCS, bit0) along with that port's negotiated speed
; (bits 10-13). Any connected port also gets a Port Reset issued and
; waited out here -- required before Speed is valid and before the
; device is ready to be addressed at all -- so this is no longer
; read-only against the controller for ports with something plugged
; in. Device enumeration (the next phase) needs to know which port to
; actually address, and the negotiated speed value goes straight into
; that device's Slot Context; this is that lookup. Returns EAX =
; number of ports with a device connected (0 is a valid, non-error
; result -- an idle QEMU xhci0 with nothing attached to it looks
; exactly like this).
; -------------------------------------------------------------------------
xhci_scan_ports:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8

    mov eax, [rel xhci_hcsparams1]
    shr eax, 24
    and eax, 0xFF                       ; MaxPorts
    mov [rel xhci_max_ports], eax

    lea rdi, [rel xhci_port_connected]
    mov ecx, eax
    xor eax, eax
    cld
    rep stosb
    lea rdi, [rel xhci_port_speed]
    mov ecx, [rel xhci_max_ports]
    xor eax, eax
    cld
    rep stosb

    mov rbx, [rel xhci_op_base]
    add rbx, 0x400                      ; first Port Register Set
    xor esi, esi                        ; esi = connected-port count
    xor ecx, ecx                        ; ecx = port index (0-based)
.port_loop:
    cmp ecx, [rel xhci_max_ports]
    jge .done

    mov eax, [rbx]                      ; PORTSC
    test eax, 0x00000001                ; CCS (Current Connect Status)
    jz .next_port

    lea rdx, [rel xhci_port_connected]
    mov byte [rdx+rcx], 1
    inc esi

    ; Port Reset: required before Speed is valid and the device is
    ; addressable at all -- USB2 link training completes here (a
    ; SuperSpeed port typically finishes its own equivalent near-
    ; instantly, but resetting unconditionally is harmless and keeps
    ; this one code path correct for every speed). Skipping this
    ; leaves the Speed field stale/zero and the device unresponsive to
    ; every later command -- Enable Slot/Address Device can still
    ; "succeed" against a bogus Speed=0 slot context, but no transfer
    ; ever completes. RW1C status bits (CSC/PEC/WRC/OCC/PRC/PLC/CEC,
    ; bits17-23) are masked off on every read-modify-write here so a
    ; write only clears the one status bit actually intended.
    mov eax, [rbx]                      ; re-read PORTSC
    and eax, ~0x00FE0000
    or eax, 0x00000010                  ; PR (Port Reset)
    mov [rbx], eax

    mov r8d, 5000000
.wait_reset_port:
    mov eax, [rbx]
    test eax, 0x00000010                ; PR self-clears when done
    jz .reset_port_done
    dec r8d
    jnz .wait_reset_port
.reset_port_done:
    mov eax, [rbx]
    and eax, ~0x00FE0000
    or eax, 0x00200000                  ; PRC (write-1-clear)
    mov [rbx], eax

    mov eax, [rbx]                      ; fresh PORTSC, post-reset
    mov edx, eax
    shr edx, 10
    and edx, 0x0F                       ; Port Speed (bits 10-13)
    lea rax, [rel xhci_port_speed]
    mov [rax+rcx], dl

.next_port:
    add rbx, 0x10                       ; each Port Register Set is 4
                                         ; dwords (PORTSC/PORTPMSC/PORTLI/
                                         ; PORTHLPMC)
    inc ecx
    jmp .port_loop
.done:
    mov eax, esi
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    add rsp, 8                          ; drop the saved rax -- the port
                                         ; count return value lives in
                                         ; eax already, restoring the
                                         ; caller's old rax would stomp it
    ret

; -------------------------------------------------------------------------
; xhci_noop_test: submits a No-Op Command TRB through the command ring,
; rings the command doorbell, and polls the event ring (bounded wait)
; for the resulting Command Completion Event. Proves the full ring
; mechanism end to end -- command submission, doorbell, controller
; processing, event generation, event ring polling -- before any of it
; gets reused for real device-slot commands. Returns EAX=1/0.
;
; The result is held in R15D until the very end rather than EAX,
; because ERDP has to be written back (updating the controller's
; dequeue pointer, clearing Event Handler Busy) on every path out of
; this routine regardless of outcome, and that update's own arithmetic
; needs EAX -- a lesson already paid for elsewhere this session.
; -------------------------------------------------------------------------
XHCI_TRB_TYPE_NOOP_CMD equ 23
XHCI_TRB_TYPE_CMD_COMPLETION equ 33

xhci_noop_test:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r15

    ; Write the No-Op Command TRB at the current enqueue slot.
    mov rax, [rel xhci_cmdring_enqueue]
    lea rdi, [rel xhci_cmdring]
    mov rcx, rax
    shl rcx, 4                          ; * 16 bytes/TRB
    add rdi, rcx

    mov qword [rdi+0], 0                ; Parameter
    mov dword [rdi+8], 0                ; Status
    mov eax, XHCI_TRB_TYPE_NOOP_CMD
    shl eax, 10
    mov rbx, [rel xhci_cmdring_cycle]
    or eax, ebx                         ; Cycle bit
    mov [rdi+12], eax

    ; Advance the enqueue pointer, following the Link TRB (slot 255,
    ; where this ring's usable range ends) and toggling cycle state
    ; exactly as the controller itself will when it reaches that link.
    mov rax, [rel xhci_cmdring_enqueue]
    inc rax
    cmp rax, 255
    jl .no_enq_wrap
    xor rax, rax
    mov rbx, [rel xhci_cmdring_cycle]
    xor rbx, 1
    mov [rel xhci_cmdring_cycle], rbx
.no_enq_wrap:
    mov [rel xhci_cmdring_enqueue], rax

    mov rbx, [rel xhci_db_base]
    mov dword [rbx+0], 0                ; ring doorbell 0 (command ring)

    mov r9d, 20000000
.poll:
    mov rax, [rel xhci_evtring_dequeue]
    lea rsi, [rel xhci_evtring]
    mov rcx, rax
    shl rcx, 4
    add rsi, rcx                        ; rsi -> next event TRB slot

    mov eax, [rsi+12]                   ; Control
    mov ebx, eax
    and ebx, 1                          ; this TRB's cycle bit
    mov rdx, [rel xhci_evtring_cycle]
    cmp ebx, edx
    jne .not_yet                        ; cycle mismatch: controller
                                         ; hasn't produced this entry yet

    mov ecx, eax
    shr ecx, 10
    and ecx, 0x3F                       ; TRB Type
    cmp ecx, XHCI_TRB_TYPE_CMD_COMPLETION
    jne .advance_dequeue

    mov r15d, 1
    call .advance_dequeue_once
    jmp .writeback

.advance_dequeue:
    call .advance_dequeue_once
    jmp .poll

.not_yet:
    dec r9d
    jnz .poll
    xor r15d, r15d
    jmp .writeback

.advance_dequeue_once:
    push rax
    push rbx
    mov rax, [rel xhci_evtring_dequeue]
    inc rax
    cmp rax, 256
    jl .adv_no_wrap
    xor rax, rax
    mov rbx, [rel xhci_evtring_cycle]
    xor rbx, 1
    mov [rel xhci_evtring_cycle], rbx
.adv_no_wrap:
    mov [rel xhci_evtring_dequeue], rax
    pop rbx
    pop rax
    ret

.writeback:
    ; Update ERDP to the current dequeue position, clearing Event
    ; Handler Busy -- required on every path out of here, success,
    ; failure, or timeout alike.
    mov rbx, [rel xhci_rt_base]
    add rbx, 0x20
    mov rax, [rel xhci_evtring_dequeue]
    shl rax, 4
    lea rdx, [rel xhci_evtring]
    add rax, rdx
    or al, 8                            ; EHB (write-1-clear)
    mov [rbx+0x18], rax                 ; ERDP

    mov eax, r15d
    pop r15
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_submit_command: generic command-ring submission, used by every
; xHCI command from here on (Enable Slot, Address Device, Configure
; Endpoint, ...) instead of duplicating xhci_noop_test's ring/doorbell/
; poll dance at each call site. Writes a TRB at the current enqueue
; slot, rings the command doorbell, and polls the event ring (bounded)
; for the matching Command Completion Event.
;
; Input:  RDI = TRB Parameter (64-bit, command-specific -- e.g. an Input
;         Context pointer for Address Device; 0 for Enable Slot),
;         ESI = TRB Status (32-bit, command-specific; 0 for both of the
;         above), ECX = TRB Type (6-bit value, e.g. 9=Enable Slot,
;         11=Address Device -- NOT pre-shifted), R10D = extra Control
;         bits already positioned at their final bit offsets (e.g. a
;         Slot ID at bits 24-31, or the BSR bit for Address Device; 0
;         for commands like Enable Slot that need none of this).
; Output: EAX = 1 if a Command Completion Event was seen before the
;         poll bound, 0 on timeout. On EAX=1: R12 = completion event's
;         Parameter (bits 0-31 hold the Input/Device Context pointer
;         low bits for some commands; unused for Enable Slot), R13D =
;         completion event's Status (bits 24-31 = Completion Code, 1 =
;         Success), R14D = completion event's Control (bits 24-31 =
;         Slot ID assigned/addressed).
; -------------------------------------------------------------------------
xhci_submit_command:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    mov r8, rdi                         ; r8 = TRB Parameter (survives the
                                         ; ring-address math below)
    mov r9d, esi                        ; r9d = TRB Status
    mov edx, ecx                        ; edx = TRB Type
                                         ; r10d = extra Control bits (as
                                         ; passed by the caller, untouched)

    mov rax, [rel xhci_cmdring_enqueue]
    lea rdi, [rel xhci_cmdring]
    mov rcx, rax
    shl rcx, 4                          ; * 16 bytes/TRB
    add rdi, rcx

    mov [rdi+0], r8                     ; Parameter
    mov [rdi+8], r9d                    ; Status
    mov eax, edx
    shl eax, 10
    or eax, r10d                        ; extra Control bits
    mov rbx, [rel xhci_cmdring_cycle]
    or eax, ebx                         ; Cycle bit
    mov [rdi+12], eax

    ; Advance the enqueue pointer, following the Link TRB (slot 255)
    ; and toggling cycle state exactly as the controller will.
    mov rax, [rel xhci_cmdring_enqueue]
    inc rax
    cmp rax, 255
    jl .no_enq_wrap
    xor rax, rax
    mov rbx, [rel xhci_cmdring_cycle]
    xor rbx, 1
    mov [rel xhci_cmdring_cycle], rbx
.no_enq_wrap:
    mov [rel xhci_cmdring_enqueue], rax

    mov rbx, [rel xhci_db_base]
    mov dword [rbx+0], 0                ; ring doorbell 0 (command ring)

    mov r9d, 20000000
.poll:
    mov rax, [rel xhci_evtring_dequeue]
    lea rsi, [rel xhci_evtring]
    mov rcx, rax
    shl rcx, 4
    add rsi, rcx                        ; rsi -> next event TRB slot

    mov eax, [rsi+12]                   ; Control
    mov ebx, eax
    and ebx, 1                          ; this TRB's cycle bit
    mov rdx, [rel xhci_evtring_cycle]
    cmp ebx, edx
    jne .not_yet

    mov ecx, eax
    shr ecx, 10
    and ecx, 0x3F                       ; TRB Type
    cmp ecx, XHCI_TRB_TYPE_CMD_COMPLETION
    jne .advance_dequeue

    mov r12, [rsi+0]                    ; completion Parameter
    mov r13d, [rsi+8]                   ; completion Status
    mov r14d, [rsi+12]                  ; completion Control
    mov ebx, 1                          ; result, parked past the pops below
    call .advance_dequeue_once
    jmp .writeback

.advance_dequeue:
    call .advance_dequeue_once
    jmp .poll

.not_yet:
    dec r9d
    jnz .poll
    xor ebx, ebx
    jmp .writeback

.advance_dequeue_once:
    push rax
    push rdx
    mov rax, [rel xhci_evtring_dequeue]
    inc rax
    cmp rax, 256
    jl .adv_no_wrap
    xor rax, rax
    mov rdx, [rel xhci_evtring_cycle]
    xor rdx, 1
    mov [rel xhci_evtring_cycle], rdx
.adv_no_wrap:
    mov [rel xhci_evtring_dequeue], rax
    pop rdx
    pop rax
    ret

.writeback:
    mov rax, [rel xhci_rt_base]
    add rax, 0x20
    push rax
    mov rax, [rel xhci_evtring_dequeue]
    shl rax, 4
    lea rdx, [rel xhci_evtring]
    add rax, rdx
    or al, 8                            ; EHB (write-1-clear)
    pop rdx
    mov [rdx+0x18], rax                 ; ERDP

    mov eax, ebx
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_enable_slot: submits an Enable Slot Command and returns the Slot
; ID the controller assigned. First real device-management command --
; every later step (Address Device, Configure Endpoint, transfers) is
; addressed by this Slot ID. Returns EAX = Slot ID (1-255) on success,
; 0 on failure (no completion event, or a non-Success completion code).
; -------------------------------------------------------------------------
XHCI_TRB_TYPE_ENABLE_SLOT equ 9
XHCI_COMPLETION_SUCCESS equ 1

xhci_enable_slot:
    push rbx
    push r12
    push r13
    push r14

    xor edi, edi                        ; Parameter = 0
    xor esi, esi                        ; Status = 0
    mov ecx, XHCI_TRB_TYPE_ENABLE_SLOT
    xor r10d, r10d                      ; no extra Control bits
    call xhci_submit_command
    test eax, eax
    jz .fail

    mov ebx, r13d
    shr ebx, 24
    and ebx, 0xFF                       ; Completion Code
    cmp ebx, XHCI_COMPLETION_SUCCESS
    jne .fail

    mov eax, r14d
    shr eax, 24
    and eax, 0xFF                       ; Slot ID
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_address_device: submits an Address Device Command against a
; slot's already-built Input Context (from xhci_setup_device_slot),
; assigning the device a real USB address and moving its Slot State to
; Addressed. BSR (Block Set Address Request) is left at 0 -- we want
; the controller to issue the actual SET_ADDRESS request, not just
; latch the context.
;
; Input:  ECX = Slot ID, RDI = Input Context pointer (from
;         xhci_setup_device_slot's xhci_input_ctx).
; Output: EAX = 1 on a Success completion, 0 on timeout or any other
;         completion code (e.g. a real device that NAKs SET_ADDRESS).
; -------------------------------------------------------------------------
XHCI_TRB_TYPE_ADDRESS_DEVICE equ 11

xhci_address_device:
    push rbx
    push rcx
    push r12
    push r13
    push r14

    mov ebx, ecx                        ; ebx = Slot ID (survives the
                                         ; RDI/ESI/ECX shuffle below)

    ; RDI already holds the Input Context pointer (this routine's own
    ; input register matches xhci_submit_command's Parameter input).
    xor esi, esi                        ; Status = 0
    mov ecx, XHCI_TRB_TYPE_ADDRESS_DEVICE
    mov r10d, ebx
    shl r10d, 24                        ; Slot ID at bits 24-31, BSR=0
    call xhci_submit_command
    test eax, eax
    jz .fail

    mov eax, r13d
    shr eax, 24
    and eax, 0xFF                       ; Completion Code
    cmp eax, XHCI_COMPLETION_SUCCESS
    jne .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r14
    pop r13
    pop r12
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_setup_device_slot: builds the Input Context (Input Control + Slot
; + EP0 contexts) and EP0 Transfer Ring for a device that has just been
; given a Slot ID via xhci_enable_slot, and points DCBAA[slot_id] at a
; freshly zeroed Device Context (the controller fills this in once
; Address Device succeeds -- we only need to allocate and zero it here).
; Does not touch the controller itself; xhci_address_device (next step)
; is what actually submits the Input Context to hardware.
;
; This phase only ever has one device attached (the QEMU usb-storage
; test device), so a single static set of buffers is enough -- a real
; multi-device driver would index these per slot ID, but that's beyond
; what this phase needs to prove.
;
; Input:  ECX = Slot ID (from xhci_enable_slot), EDX = port index
;         (0-based, into xhci_port_connected/xhci_port_speed -- the
;         port this device was found on by xhci_scan_ports).
; Output: EAX = 1/0. Only fails if the Slot ID is out of DCBAA's range.
; -------------------------------------------------------------------------
XHCI_EP_TYPE_CONTROL equ 4

xhci_setup_device_slot:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov r8d, ecx                        ; r8d = Slot ID
    mov r9d, edx                        ; r9d = port index

    cmp r8d, 0
    jz .fail
    cmp r8d, XHCI_MAX_SLOTS
    ja .fail

    ; EP0 Transfer Ring: same ring discipline as the command/event rings
    ; -- zeroed, Link TRB at the last of 256 slots, cycle state starts 1.
    lea rdi, [rel xhci_ep0_ring]
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosb

    lea rsi, [rel xhci_ep0_ring]
    lea rdi, [rel xhci_ep0_ring]
    add rdi, 4096 - 16
    mov [rdi+0], rsi
    mov dword [rdi+8], 0
    mov dword [rdi+12], (6 << 10) | 0x02 | 0x01   ; Type=Link, TC=1, Cycle=1
    mov qword [rel xhci_ep0_ring_cycle], 1
    mov qword [rel xhci_ep0_ring_enqueue], 0

    ; Device Context: zeroed, controller fills it in once Address Device
    ; succeeds. Sized for the worst case (64-byte contexts * 32 entries).
    lea rdi, [rel xhci_dev_ctx]
    mov ecx, 2048
    xor eax, eax
    cld
    rep stosb

    lea rax, [rel xhci_dev_ctx]
    lea rdx, [rel xhci_dcbaa]
    mov rcx, r8
    shl rcx, 3                           ; slot ID * 8 bytes/pointer
    mov [rdx+rcx], rax                   ; DCBAA[slot_id] = Device Context

    ; Input Context: zeroed, then Input Control + Slot + EP0 contexts
    ; filled in.
    lea rdi, [rel xhci_input_ctx]
    mov ecx, 2112
    xor eax, eax
    cld
    rep stosb

    mov ecx, [rel xhci_context_size]

    ; Input Control Context (index 0): A0 (Slot) and A1 (EP0) Add
    ; Context flags -- this Address Device command initializes both.
    lea rdi, [rel xhci_input_ctx]
    mov dword [rdi+4], 0x00000003

    ; Slot Context (index 1): Route String=0 (direct root-port attach),
    ; Speed=this port's negotiated speed, Context Entries=1 (only EP0
    ; is being initialized), Root Hub Port Number = port index + 1
    ; (PORTSC ports are 1-based in the Slot Context, 0-based in our own
    ; xhci_port_connected/xhci_port_speed arrays).
    lea rdi, [rel xhci_input_ctx]
    add rdi, rcx                         ; rdi -> Slot Context

    lea rax, [rel xhci_port_speed]
    movzx eax, byte [rax+r9]
    and eax, 0x0F
    shl eax, 20
    or eax, (1 << 27)                    ; Context Entries = 1
    mov [rdi+0], eax

    mov eax, r9d
    inc eax                              ; Root Hub Port Number (1-based)
    shl eax, 16
    mov [rdi+4], eax

    ; EP0 Context (index 2): Control endpoint, CErr=3 (max retry count),
    ; Max Packet Size per negotiated speed (the real value gets refined
    ; once the device descriptor is read; this is the safe default per
    ; spec), TR Dequeue Pointer = EP0 ring base with DCS=1, Average TRB
    ; Length=8 (a reasonable default for control transfers).
    lea rdi, [rel xhci_input_ctx]
    mov rax, rcx
    shl rax, 1
    add rdi, rax                         ; rdi -> EP0 Context

    call .ep0_max_packet_size            ; eax = max packet size for r9's port speed

    mov ebx, eax
    shl ebx, 16                          ; Max Packet Size (bits 16-31)
    or ebx, (XHCI_EP_TYPE_CONTROL << 3)  ; EP Type (bits 3-5)
    or ebx, (3 << 1)                     ; CErr (bits 1-2)
    mov [rdi+4], ebx

    lea rax, [rel xhci_ep0_ring]
    or al, 1                             ; DCS = 1 (initial cycle state)
    mov [rdi+8], rax                     ; TR Dequeue Pointer (64-bit)

    mov dword [rdi+16], (8 << 0)         ; Average TRB Length

    mov eax, 1
    jmp .out

.ep0_max_packet_size:
    ; r9d = port index (0-based). Looks up that port's negotiated speed
    ; and returns the spec-default EP0 max packet size for it in EAX.
    push rdx
    lea rdx, [rel xhci_port_speed]
    movzx eax, byte [rdx+r9]
    cmp eax, 4                          ; SuperSpeed and above: 512
    jae .mps_512
    cmp eax, 3                          ; High Speed: 64
    je .mps_64
    mov eax, 8                          ; Full/Low Speed: 8
    jmp .mps_out
.mps_64:
    mov eax, 64
    jmp .mps_out
.mps_512:
    mov eax, 512
.mps_out:
    pop rdx
    ret

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

; -------------------------------------------------------------------------
; xhci_get_device_descriptor: issues a standard GET_DESCRIPTOR(Device)
; control transfer against a slot's EP0 (Setup/Data/Status stage TRBs
; on the EP0 ring built by xhci_setup_device_slot, addressed via
; xhci_address_device), reading the full 18-byte Device Descriptor into
; xhci_device_descriptor. This is the proof point for enumeration: a
; real device descriptor back from real hardware/emulation means the
; whole ring/context/addressing chain built by this phase actually
; works, not just that commands complete.
;
; The Setup Stage TRB's Parameter field IS the 8-byte Setup Data itself
; (IDT -- Immediate Data -- is mandatory for Setup TRBs, so there's no
; separate buffer to build; the 8 setup-packet bytes are folded
; directly into one 64-bit immediate: bmRequestType=0x80 (device-to-
; host/standard/device), bRequest=0x06 (GET_DESCRIPTOR), wValue=0x0100
; (type=Device, index=0), wIndex=0, wLength=18).
;
; Input:  ECX = Slot ID.
; Output: EAX = 1 on Success/Short-Packet completion, 0 on timeout or
;         any other completion code.
; -------------------------------------------------------------------------
XHCI_TRB_TYPE_SETUP_STAGE  equ 2
XHCI_TRB_TYPE_DATA_STAGE   equ 3
XHCI_TRB_TYPE_STATUS_STAGE equ 4
XHCI_TRB_TYPE_TRANSFER_EVENT equ 32
XHCI_SETUP_GET_DEVICE_DESCRIPTOR equ 0x0012000001000680
XHCI_COMPLETION_SHORT_PACKET equ 13

xhci_get_device_descriptor:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov r8d, ecx                        ; r8d = Slot ID (survives the
                                         ; TRB-building calls below)

    ; Setup Stage TRB: IDT=1 (bit6), TRT=3 (IN Data Stage, bits16-17),
    ; TRB Transfer Length=8 (the setup packet's own length).
    mov rdi, XHCI_SETUP_GET_DEVICE_DESCRIPTOR
    mov esi, 8
    mov edx, (XHCI_TRB_TYPE_SETUP_STAGE << 10) | (3 << 16) | (1 << 6)
    call .write_ep0_trb

    ; Data Stage TRB: DIR=1 (bit16, IN), TRB Transfer Length=18 (the
    ; full Device Descriptor).
    lea rdi, [rel xhci_device_descriptor]
    mov esi, 18
    mov edx, (XHCI_TRB_TYPE_DATA_STAGE << 10) | (1 << 16)
    call .write_ep0_trb

    ; Status Stage TRB: DIR=0 (OUT -- opposite of the IN data stage),
    ; IOC=1 (bit5) so exactly one Transfer Event comes back for this
    ; whole three-TRB control transfer.
    xor edi, edi
    xor esi, esi
    mov edx, (XHCI_TRB_TYPE_STATUS_STAGE << 10) | (1 << 5)
    call .write_ep0_trb

    ; Ring this device's doorbell, Target=1 (EP0, Device Context Index
    ; 1 -- the only endpoint xhci_setup_device_slot initialized).
    mov rbx, [rel xhci_db_base]
    mov rcx, r8
    shl rcx, 2                           ; slot ID * 4 bytes/doorbell
    mov dword [rbx+rcx], 1

    mov r9d, 20000000
.poll:
    mov rax, [rel xhci_evtring_dequeue]
    lea rsi, [rel xhci_evtring]
    mov rcx, rax
    shl rcx, 4
    add rsi, rcx

    mov eax, [rsi+12]                   ; Control
    mov ebx, eax
    and ebx, 1
    mov rdx, [rel xhci_evtring_cycle]
    cmp ebx, edx
    jne .not_yet

    mov ecx, eax
    shr ecx, 10
    and ecx, 0x3F                       ; TRB Type
    cmp ecx, XHCI_TRB_TYPE_TRANSFER_EVENT
    jne .advance_dequeue

    mov eax, [rsi+8]                    ; completion Status
    shr eax, 24
    and eax, 0xFF                       ; Completion Code
    mov ebx, 0
    cmp eax, XHCI_COMPLETION_SUCCESS
    je .mark_ok
    cmp eax, XHCI_COMPLETION_SHORT_PACKET
    jne .not_ok
.mark_ok:
    mov ebx, 1
.not_ok:
    call .advance_dequeue_once
    jmp .writeback

.advance_dequeue:
    call .advance_dequeue_once
    jmp .poll

.not_yet:
    dec r9d
    jnz .poll
    xor ebx, ebx
    jmp .writeback

.write_ep0_trb:
    ; RDI = Parameter, ESI = Status, EDX = Control bits (Type + any
    ; flags, NOT including Cycle). Writes at the current EP0 ring
    ; enqueue slot, advances the enqueue pointer, following the Link
    ; TRB (slot 255) and toggling cycle state exactly as the
    ; controller will.
    push rax
    push rcx

    mov rax, [rel xhci_ep0_ring_enqueue]
    lea rcx, [rel xhci_ep0_ring]
    push rdx
    mov rdx, rax
    shl rdx, 4
    add rcx, rdx
    pop rdx

    mov [rcx+0], rdi
    mov [rcx+8], esi
    mov eax, edx
    mov rbx, [rel xhci_ep0_ring_cycle]
    or eax, ebx
    mov [rcx+12], eax

    mov rax, [rel xhci_ep0_ring_enqueue]
    inc rax
    cmp rax, 255
    jl .ep0_no_wrap
    xor rax, rax
    mov rbx, [rel xhci_ep0_ring_cycle]
    xor rbx, 1
    mov [rel xhci_ep0_ring_cycle], rbx
.ep0_no_wrap:
    mov [rel xhci_ep0_ring_enqueue], rax

    pop rcx
    pop rax
    ret

.advance_dequeue_once:
    push rax
    push rdx
    mov rax, [rel xhci_evtring_dequeue]
    inc rax
    cmp rax, 256
    jl .adv_no_wrap
    xor rax, rax
    mov rdx, [rel xhci_evtring_cycle]
    xor rdx, 1
    mov [rel xhci_evtring_cycle], rdx
.adv_no_wrap:
    mov [rel xhci_evtring_dequeue], rax
    pop rdx
    pop rax
    ret

.writeback:
    mov rax, [rel xhci_rt_base]
    add rax, 0x20
    push rax
    mov rax, [rel xhci_evtring_dequeue]
    shl rax, 4
    lea rdx, [rel xhci_evtring]
    add rax, rdx
    or al, 8                            ; EHB (write-1-clear)
    pop rdx
    mov [rdx+0x18], rax                 ; ERDP

    mov eax, ebx
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_ring_enqueue_trb: writes a TRB at a transfer ring's current
; enqueue slot and advances the enqueue pointer (following that ring's
; own Link TRB at slot 255 and toggling its cycle state exactly as the
; controller will) -- the same "write TRB, advance, handle wrap" logic
; every transfer ring needs (EP0's control-transfer ring, and Phase D's
; Bulk IN/OUT rings), factored out once rather than copy-pasted again.
; A copy-pasted version of exactly this logic already produced a real
; offset bug once this phase (the EP0 Context's TR Dequeue Pointer
; field) -- one implementation is one place left to get it right.
;
; Input:  R10 = ring base address (a 4096-byte, 256-TRB-slot buffer,
;         already carrying its own Link TRB at slot 255), R11 =
;         pointer to that ring's cycle-state qword, R12 = pointer to
;         that ring's enqueue-index qword, RDI = TRB Parameter (64-
;         bit), ESI = TRB Status (32-bit), EDX = TRB Control bits
;         (Type + any flags, NOT including Cycle -- this routine ORs
;         the ring's current cycle bit in itself).
; Output: none. R10/R11/R12 and the TRB fields are all consumed, not
;         preserved -- same as every other internal helper in this
;         file, callers that need them again reload from their own
;         source of truth rather than relying on survival across the
;         call.
; -------------------------------------------------------------------------
xhci_ring_enqueue_trb:
    push rax
    push rbx
    push rcx

    mov rax, [r12]
    mov rbx, rax
    shl rbx, 4
    add rbx, r10                        ; rbx -> current TRB slot

    mov [rbx+0], rdi
    mov [rbx+8], esi
    mov ecx, edx
    mov rax, [r11]
    or ecx, eax
    mov [rbx+12], ecx

    mov rax, [r12]
    inc rax
    cmp rax, 255
    jl .no_wrap
    xor rax, rax
    mov rcx, [r11]
    xor rcx, 1
    mov [r11], rcx
.no_wrap:
    mov [r12], rax

    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; xhci_control_transfer: generic three-stage (Setup/optional-Data/
; Status) EP0 control transfer, for every request beyond the
; hardcoded GET_DESCRIPTOR(Device) that xhci_get_device_descriptor
; already proved the ring/context/addressing chain with. Handles both
; with-data and no-data requests (SET_CONFIGURATION has no data
; stage), deriving TRT and the Status stage's direction the same way
; a real driver does: no data stage -> TRT=0, Status=IN always; a data
; stage -> TRT/Data-stage DIR matches the caller's requested direction,
; Status direction is the opposite of it.
;
; Input:  ECX = Slot ID, RSI = pointer to an 8-byte Setup Packet
;         (caller-built -- bmRequestType/bRequest/wValue/wIndex/
;         wLength, standard USB layout), RDI = data buffer pointer (0
;         if this request has no data stage), R8D = data length in
;         bytes (0 if none), R9B = data direction (1=IN, 0=OUT; only
;         meaningful when R8D != 0).
; Output: EAX = 1 on a Success/Short-Packet completion, 0 on timeout
;         or any other completion code.
; -------------------------------------------------------------------------
xhci_control_transfer:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r13d, ecx                       ; r13d = Slot ID
    mov r14, rdi                        ; r14 = data buffer pointer
    mov r15d, r8d                       ; r15d = data length
    mov bl, r9b                         ; bl = data direction (1=IN/0=OUT)

    lea r10, [rel xhci_ep0_ring]
    lea r11, [rel xhci_ep0_ring_cycle]
    lea r12, [rel xhci_ep0_ring_enqueue]

    ; Setup Stage TRB: IDT=1, TRB Transfer Length=8. TRT is 0 (No Data
    ; Stage) unless the caller asked for a data stage, in which case
    ; it matches the requested direction.
    mov rdi, [rsi]                      ; the 8 raw setup-packet bytes
                                         ; (must be read before RSI/ESI
                                         ; get reused below)
    mov esi, 8
    mov eax, XHCI_TRB_TYPE_SETUP_STAGE
    shl eax, 10
    or eax, (1 << 6)                    ; IDT
    mov edx, eax
    test r15d, r15d
    jz .no_data_trt
    cmp bl, 1
    je .trt_in
    or edx, (2 << 16)                   ; TRT = OUT Data Stage
    jmp .no_data_trt
.trt_in:
    or edx, (3 << 16)                   ; TRT = IN Data Stage
.no_data_trt:
    call xhci_ring_enqueue_trb

    ; Data Stage TRB, only if the caller supplied one.
    test r15d, r15d
    jz .no_data_stage
    mov rdi, r14
    mov esi, r15d
    mov eax, XHCI_TRB_TYPE_DATA_STAGE
    shl eax, 10
    mov edx, eax
    cmp bl, 1
    jne .data_dir_done
    or edx, (1 << 16)                   ; DIR = IN
.data_dir_done:
    call xhci_ring_enqueue_trb
.no_data_stage:

    ; Status Stage TRB: IOC=1 so exactly one Transfer Event comes
    ; back. DIR is IN unless a data stage happened and it was IN
    ; (then Status is OUT, the opposite).
    xor edi, edi
    xor esi, esi
    mov eax, XHCI_TRB_TYPE_STATUS_STAGE
    shl eax, 10
    or eax, (1 << 5)                    ; IOC
    mov edx, eax
    test r15d, r15d
    jz .status_dir_in
    cmp bl, 1
    je .status_dir_done
.status_dir_in:
    or edx, (1 << 16)                   ; DIR = IN
.status_dir_done:
    call xhci_ring_enqueue_trb

    ; Ring this device's doorbell, Target=1 (EP0).
    mov rbx, [rel xhci_db_base]
    mov rcx, r13
    shl rcx, 2
    mov dword [rbx+rcx], 1

    mov r9d, 20000000
.poll:
    mov rax, [rel xhci_evtring_dequeue]
    lea rsi, [rel xhci_evtring]
    mov rcx, rax
    shl rcx, 4
    add rsi, rcx

    mov eax, [rsi+12]                   ; Control
    mov ebx, eax
    and ebx, 1
    mov rdx, [rel xhci_evtring_cycle]
    cmp ebx, edx
    jne .not_yet

    mov ecx, eax
    shr ecx, 10
    and ecx, 0x3F                       ; TRB Type
    cmp ecx, XHCI_TRB_TYPE_TRANSFER_EVENT
    jne .advance_dequeue

    mov eax, [rsi+8]                    ; completion Status
    shr eax, 24
    and eax, 0xFF                       ; Completion Code
    mov ebx, 0
    cmp eax, XHCI_COMPLETION_SUCCESS
    je .mark_ok
    cmp eax, XHCI_COMPLETION_SHORT_PACKET
    jne .not_ok
.mark_ok:
    mov ebx, 1
.not_ok:
    call .advance_dequeue_once
    jmp .writeback

.advance_dequeue:
    call .advance_dequeue_once
    jmp .poll

.not_yet:
    dec r9d
    jnz .poll
    xor ebx, ebx
    jmp .writeback

.advance_dequeue_once:
    push rax
    push rdx
    mov rax, [rel xhci_evtring_dequeue]
    inc rax
    cmp rax, 256
    jl .adv_no_wrap
    xor rax, rax
    mov rdx, [rel xhci_evtring_cycle]
    xor rdx, 1
    mov [rel xhci_evtring_cycle], rdx
.adv_no_wrap:
    mov [rel xhci_evtring_dequeue], rax
    pop rdx
    pop rax
    ret

.writeback:
    mov rax, [rel xhci_rt_base]
    add rax, 0x20
    push rax
    mov rax, [rel xhci_evtring_dequeue]
    shl rax, 4
    lea rdx, [rel xhci_evtring]
    add rax, rdx
    or al, 8                            ; EHB (write-1-clear)
    pop rdx
    mov [rdx+0x18], rax                 ; ERDP

    mov eax, ebx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; xhci_get_config_descriptor: fetches the Configuration Descriptor
; (first a 9-byte header read to learn wTotalLength, then the full
; descriptor -- Configuration + Interface + Endpoint descriptors, and
; on a SuperSpeed device a SuperSpeed Endpoint Companion Descriptor
; after each Endpoint one, though this only needs to skip those, not
; parse them) into xhci_config_descriptor, then walks it by each
; entry's own bLength to find the Interface descriptor (confirming
; Mass Storage / SCSI transparent / Bulk-Only: class 0x08, subclass
; 0x06, protocol 0x50) and the two Bulk endpoint descriptors.
;
; Input:  ECX = Slot ID.
; Output: EAX = 1/0. On success: xhci_config_value (bConfigurationValue
;         for SET_CONFIGURATION), xhci_bulk_in_ep_index/
;         xhci_bulk_out_ep_index (Device Context Index for each Bulk
;         endpoint) and xhci_bulk_in_max_packet/xhci_bulk_out_max_packet
;         are all populated.
; -------------------------------------------------------------------------
XHCI_DESC_TYPE_INTERFACE equ 4
XHCI_DESC_TYPE_ENDPOINT  equ 5
XHCI_CONFIG_DESC_MAX_LEN equ 128

xhci_get_config_descriptor:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r15

    mov r15d, ecx                       ; r15d = Slot ID (survives the
                                         ; two control transfers below --
                                         ; R8D/R9B are xhci_control_
                                         ; transfer's OWN input registers,
                                         ; so the Slot ID can't live there
                                         ; across the call)

    ; Short read: just the 9-byte Configuration Descriptor header, to
    ; learn wTotalLength.
    lea rdi, [rel xhci_setup_packet]
    mov byte [rdi+0], 0x80              ; bmRequestType: device-to-host,
                                         ; standard, device
    mov byte [rdi+1], 0x06              ; bRequest = GET_DESCRIPTOR
    mov byte [rdi+2], 0x00              ; wValue low = index 0
    mov byte [rdi+3], 0x02              ; wValue high = type Configuration
    mov word [rdi+4], 0                 ; wIndex = 0
    mov word [rdi+6], 9                 ; wLength = 9

    mov ecx, r15d
    lea rsi, [rel xhci_setup_packet]
    lea rdi, [rel xhci_config_descriptor]
    mov r8d, 9
    mov r9b, 1                          ; IN
    call xhci_control_transfer
    test eax, eax
    jz .fail

    movzx eax, byte [rel xhci_config_descriptor+2]
    movzx ecx, byte [rel xhci_config_descriptor+3]
    shl ecx, 8
    or eax, ecx                         ; eax = wTotalLength
    cmp eax, XHCI_CONFIG_DESC_MAX_LEN
    jbe .len_ok
    mov eax, XHCI_CONFIG_DESC_MAX_LEN
.len_ok:
    mov [rel xhci_config_total_length], eax

    ; Full read, now that wTotalLength is known.
    lea rdi, [rel xhci_setup_packet]
    mov eax, [rel xhci_config_total_length]
    mov [rdi+6], ax                     ; wLength (word)

    mov ecx, r15d
    lea rsi, [rel xhci_setup_packet]
    lea rdi, [rel xhci_config_descriptor]
    mov eax, [rel xhci_config_total_length]
    mov r8d, eax
    mov r9b, 1                          ; IN
    call xhci_control_transfer
    test eax, eax
    jz .fail

    movzx eax, byte [rel xhci_config_descriptor+5]
    mov [rel xhci_config_value], eax    ; bConfigurationValue

    mov dword [rel xhci_bulk_in_ep_index], 0
    mov dword [rel xhci_bulk_out_ep_index], 0
    xor ebx, ebx                        ; ebx = 1 once the MSD/BOT
                                         ; interface has been seen
    mov ecx, 9                          ; ecx = walk offset, skip the
                                         ; Configuration descriptor's
                                         ; own 9 bytes
.walk:
    cmp ecx, [rel xhci_config_total_length]
    jge .walk_done
    lea rdx, [rel xhci_config_descriptor]
    add rdx, rcx
    movzx eax, byte [rdx+0]             ; bLength
    test eax, eax
    jz .walk_done                       ; malformed -- stop rather
                                         ; than spin forever
    movzx r9d, byte [rdx+1]             ; bDescriptorType

    cmp r9d, XHCI_DESC_TYPE_INTERFACE
    jne .not_iface
    cmp eax, 9
    jl .next_desc
    cmp byte [rdx+5], 0x08              ; bInterfaceClass = Mass Storage
    jne .next_desc
    cmp byte [rdx+6], 0x06              ; bInterfaceSubClass = SCSI
    jne .next_desc
    cmp byte [rdx+7], 0x50              ; bInterfaceProtocol = Bulk-Only
    jne .next_desc
    mov ebx, 1
    jmp .next_desc

.not_iface:
    cmp r9d, XHCI_DESC_TYPE_ENDPOINT
    jne .next_desc
    cmp eax, 7
    jl .next_desc
    movzx esi, byte [rdx+3]             ; bmAttributes
    and esi, 0x03
    cmp esi, 2                          ; Transfer Type = Bulk
    jne .next_desc

    movzx esi, byte [rdx+2]             ; bEndpointAddress
    movzx edi, byte [rdx+2]
    and edi, 0x0F
    shl edi, 1
    test esi, 0x80
    jz .ep_out
    inc edi                             ; DCI = epnum*2 + 1 (IN)
    mov [rel xhci_bulk_in_ep_index], edi
    movzx eax, word [rdx+4]             ; wMaxPacketSize
    mov [rel xhci_bulk_in_max_packet], eax
    jmp .next_desc
.ep_out:
    mov [rel xhci_bulk_out_ep_index], edi   ; DCI = epnum*2 (OUT)
    movzx eax, word [rdx+4]
    mov [rel xhci_bulk_out_max_packet], eax

.next_desc:
    movzx eax, byte [rdx+0]
    add ecx, eax
    jmp .walk
.walk_done:

    test ebx, ebx
    jz .fail
    cmp dword [rel xhci_bulk_in_ep_index], 0
    je .fail
    cmp dword [rel xhci_bulk_out_ep_index], 0
    je .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r15
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
xhci_db_base:     dq 0
xhci_max_ports:   dd 0
xhci_port_connected: times 32 db 0      ; 1 byte per port index (0-based);
                                         ; MaxPorts is an 8-bit HCSPARAMS1
                                         ; field, so 32 is a generous cap
xhci_port_speed:  times 32 db 0         ; PORTSC bits 10-13 per port index
xhci_caplength:   db 0
xhci_hciversion:  dw 0
xhci_hcsparams1:  dd 0
xhci_hccparams1:  dd 0
xhci_context_size: dd 32                ; 32 or 64 bytes/context, per CSZ
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

xhci_ep0_ring_cycle:   dq 0
xhci_ep0_ring_enqueue: dq 0

align 4096
xhci_ep0_ring: times 4096 db 0
align 64
xhci_dev_ctx:   times 2048 db 0         ; 32 contexts * 64 bytes (worst case)
align 64
xhci_input_ctx: times 2112 db 0         ; 33 contexts * 64 bytes (worst case)

align 16
xhci_device_descriptor: times 18 db 0   ; standard USB Device Descriptor

align 16
xhci_setup_packet: times 8 db 0         ; scratch 8-byte Setup Packet,
                                         ; built fresh by each caller of
                                         ; xhci_control_transfer/
                                         ; xhci_get_config_descriptor
                                         ; before use -- fine as a single
                                         ; shared buffer since this is a
                                         ; synchronous, one-request-at-a-
                                         ; time driver

align 16
xhci_config_descriptor: times XHCI_CONFIG_DESC_MAX_LEN db 0
xhci_config_total_length: dd 0
xhci_config_value:        dd 0
xhci_bulk_in_ep_index:     dd 0
xhci_bulk_out_ep_index:    dd 0
xhci_bulk_in_max_packet:   dd 0
xhci_bulk_out_max_packet:  dd 0
