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
; (bits 10-13) -- read-only, no state changes to the controller. Device
; enumeration (the next phase) needs to know which port to actually
; address, and the negotiated speed value goes straight into that
; device's Slot Context; this is that lookup. Returns EAX = number of
; ports with a device connected (0 is a valid, non-error result -- an
; idle QEMU xhci0 with nothing attached to it looks exactly like this).
; -------------------------------------------------------------------------
xhci_scan_ports:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

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
;         11=Address Device -- NOT pre-shifted).
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

    mov r8, rdi                         ; r8 = TRB Parameter (survives the
                                         ; ring-address math below)
    mov r9d, esi                        ; r9d = TRB Status
    mov edx, ecx                        ; edx = TRB Type

    mov rax, [rel xhci_cmdring_enqueue]
    lea rdi, [rel xhci_cmdring]
    mov rcx, rax
    shl rcx, 4                          ; * 16 bytes/TRB
    add rdi, rcx

    mov [rdi+0], r8                     ; Parameter
    mov [rdi+8], r9d                    ; Status
    mov eax, edx
    shl eax, 10
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
