; =============================================================================
; PS/2 mouse driver (IRQ12, standard 3-byte packet, aux port on the i8042
; controller shared with the keyboard). Mirrors keyboard.inc's shape:
; mouse_install registers the IRQ12 handler and unmasks it; the ISR just
; updates a small piece of global state (mouse_x/mouse_y/mouse_btn) for
; BASIX64's MOUSEX/MOUSEY/MOUSEBTN to poll -- no blocking-read buffer like
; kbd_read_char, since a GUI main loop wants "where is it right now", not
; a queue of past positions.
; =============================================================================

; Packet byte 0 bit layout (buttons + sign/overflow flags):
MOUSE_PKT_LBTN   equ 0x01
MOUSE_PKT_RBTN   equ 0x02
MOUSE_PKT_MBTN   equ 0x04
MOUSE_PKT_XSIGN  equ 0x10
MOUSE_PKT_YSIGN  equ 0x20

; -------------------------------------------------------------------------
; mouse_wait_write: spins until the i8042 input buffer is empty (safe to
; write a byte to port 0x60/0x64).
; -------------------------------------------------------------------------
mouse_wait_write:
    push rax
    push rcx
    mov ecx, 100000
.spin:
    in al, 0x64
    test al, 0x02
    jz .ready
    dec ecx
    jnz .spin
.ready:
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; mouse_wait_read: spins until the i8042 output buffer is full (a byte is
; waiting to be read from port 0x60).
; -------------------------------------------------------------------------
mouse_wait_read:
    push rax
    push rcx
    mov ecx, 100000
.spin:
    in al, 0x64
    test al, 0x01
    jnz .ready
    dec ecx
    jnz .spin
.ready:
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; mouse_write_aux: AL = byte to send to the mouse itself (via the 0xD4
; "next byte goes to the aux device" controller prefix).
; -------------------------------------------------------------------------
mouse_write_aux:
    push rax
    mov ah, al
    call mouse_wait_write
    mov al, 0xD4
    out 0x64, al
    call mouse_wait_write
    mov al, ah
    out 0x60, al
    pop rax
    ret

; -------------------------------------------------------------------------
; mouse_install: enables the aux port, turns on IRQ12 + the aux clock in
; the controller's command byte, tells the mouse itself to start sending
; movement packets, then registers and unmasks the IRQ12 handler. Must
; run after kbd_install (relies on IRQ2, the master->slave cascade line,
; already being unmasked there).
; -------------------------------------------------------------------------
mouse_install:
    push rax
    push rcx
    push rdx

    call mouse_wait_write
    mov al, 0xA8                        ; enable auxiliary device
    out 0x64, al

    call mouse_wait_write
    mov al, 0x20                        ; command: read controller command byte
    out 0x64, al
    call mouse_wait_read
    in al, 0x60
    or al, 0x02                         ; bit1: enable IRQ12
    and al, 0xDF                        ; bit5=0: enable the aux (mouse) clock
    mov ah, al

    call mouse_wait_write
    mov al, 0x60                        ; command: write controller command byte
    out 0x64, al
    call mouse_wait_write
    mov al, ah
    out 0x60, al

    mov al, 0xF6                        ; mouse command: set defaults
    call mouse_write_aux
    call mouse_wait_read
    in al, 0x60                         ; discard ACK (0xFA)

    mov al, 0xF4                        ; mouse command: enable data reporting
    call mouse_write_aux
    call mouse_wait_read
    in al, 0x60                         ; discard ACK (0xFA)

    mov dword [rel mouse_packet_idx], 0

    mov rcx, 44                         ; vector 32 + IRQ12
    lea rdx, [rel mouse_irq_stub]
    call idt_set_gate

    in al, 0xA1
    and al, 0xEF                        ; unmask IRQ12 (bit4 of the slave PIC)
    out 0xA1, al

    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; mouse_irq_stub: IRQ12 handler. Accumulates one 3-byte packet
; (buttons+sign flags, delta-X, delta-Y), then on the 3rd byte decodes
; the signed deltas, updates mouse_x/mouse_y (clamped to the real
; framebuffer's dimensions -- rbx holds boot_info for the whole kernel's
; lifetime, safe to read here) and mouse_btn, and resets for the next
; packet. Y is inverted (PS/2 reports +Y as "up"; screen/framebuffer +Y
; is "down").
; -------------------------------------------------------------------------
mouse_irq_stub:
    push rax
    push rbx
    push rcx
    push rdx

    in al, 0x60
    movzx ecx, byte [rel mouse_packet_idx]
    test ecx, ecx
    jnz .not_first_byte
    test al, 0x08                        ; byte 0's sync bit must be set --
    jz .eoi                              ; otherwise we're out of alignment
                                          ; (e.g. after a dropped IRQ); drop
                                          ; this byte and wait for a real
                                          ; first byte instead of ever
                                          ; decoding a permanently-desynced
                                          ; stream
.not_first_byte:
    lea rdx, [rel mouse_packet]
    mov [rdx+rcx], al
    inc ecx
    cmp ecx, 3
    jl .store_idx

    ; full packet: decode
    xor ecx, ecx
    movzx eax, byte [rdx+0]              ; flags byte
    mov [rel mouse_btn], al

    movzx ecx, byte [rdx+1]              ; raw dx (0-255)
    test al, MOUSE_PKT_XSIGN
    jz .dx_ok
    or ecx, 0xFFFFFF00                   ; sign-extend to a 32-bit negative
.dx_ok:
    movzx eax, byte [rdx+2]              ; raw dy (0-255)
    test byte [rdx+0], MOUSE_PKT_YSIGN
    jz .dy_ok
    or eax, 0xFFFFFF00
.dy_ok:
    neg eax                              ; PS/2 +Y=up -> framebuffer +Y=down

    mov edx, [rel mouse_x]
    add edx, ecx
    jns .x_nonneg
    xor edx, edx
.x_nonneg:
    cmp edx, [rbx+FB_WIDTH]
    jl .x_ok
    mov edx, [rbx+FB_WIDTH]
    dec edx
.x_ok:
    mov [rel mouse_x], edx

    mov edx, [rel mouse_y]
    add edx, eax
    jns .y_nonneg
    xor edx, edx
.y_nonneg:
    cmp edx, [rbx+FB_HEIGHT]
    jl .y_ok
    mov edx, [rbx+FB_HEIGHT]
    dec edx
.y_ok:
    mov [rel mouse_y], edx

    xor ecx, ecx
.store_idx:
    mov [rel mouse_packet_idx], ecx

.eoi:
    mov al, 0x20                        ; EOI both PICs -- IRQ12 is on the
    out 0xA0, al                        ; slave, cascaded through master
    out 0x20, al                        ; IRQ2, so both must be acked

    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; =========================================================================
; Data
; =========================================================================
mouse_x:           dd 0
mouse_y:           dd 0
mouse_btn:         dd 0
mouse_packet:      times 3 db 0
mouse_packet_idx:  dd 0
