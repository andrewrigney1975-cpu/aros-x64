; =============================================================================
; arOS-X64 Kernel (phase 1: boot + say hello)
;
; Loaded as a flat binary by the bootloader at KERNEL_LOAD_ADDR (0x200000)
; via AllocatePages(AllocateAddress) + File Read, then entered with a plain
; JMP (not CALL) so there is no return address on the stack:
;   RCX = EFI_HANDLE ImageHandle
;   RDX = EFI_SYSTEM_TABLE *SystemTable
; Boot Services are still active at this point (ExitBootServices has not
; been called yet), so we can use SystemTable->ConOut directly.
; =============================================================================

BITS 64
ORG 0x200000

entry:
    push rbx
    sub rsp, 40                        ; shadow space; see note below
    ; Entered via JMP (no return address pushed), so RSP starts 16B-aligned.
    ; 1 push (8B) + sub 40 (=8 mod16) -> back to 16B-aligned before CALLs.

    mov rbx, rdx                        ; rbx = SystemTable (non-volatile)

    call serial_init

    lea rcx, [rel msg_hello]
    call serial_puts

    ; SystemTable->ConOut->OutputString(ConOut, msg_hello_u16)
    mov rax, [rbx+0x40]                 ; rax = ConOut
    mov rcx, rax
    lea rdx, [rel msg_hello_u16]
    call qword [rax+8]

.hang:
    jmp .hang

; -------------------------------------------------------------------------
; serial_init: initialise COM1 (0x3F8) to 38400 8N1 with FIFO enabled.
; -------------------------------------------------------------------------
serial_init:
    mov dx, 0x3F9
    xor al, al
    out dx, al

    mov dx, 0x3FB
    mov al, 0x80
    out dx, al

    mov dx, 0x3F8
    mov al, 0x03
    out dx, al

    mov dx, 0x3F9
    xor al, al
    out dx, al

    mov dx, 0x3FB
    mov al, 0x03
    out dx, al

    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al

    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    ret

; -------------------------------------------------------------------------
; serial_putc: send AL out COM1, waiting for the transmit holding register.
; -------------------------------------------------------------------------
serial_putc:
    push rax
    push rdx
    push r9
    mov r9b, al
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, r9b
    out dx, al
    pop r9
    pop rdx
    pop rax
    ret

; -------------------------------------------------------------------------
; serial_puts: RCX = pointer to a NUL-terminated ASCII string.
; -------------------------------------------------------------------------
serial_puts:
    push rcx
    push rbx
    sub rsp, 8                          ; keep RSP 16B-aligned before CALLs
    mov rbx, rcx
.next:
    mov al, [rbx]
    test al, al
    jz .done
    call serial_putc
    inc rbx
    jmp .next
.done:
    add rsp, 8
    pop rbx
    pop rcx
    ret

; =========================================================================
; Data
; =========================================================================
msg_hello:      db 'Hello, kernal!', 13, 10, 0
msg_hello_u16:
    db __utf16le__(`Hello, kernal!\r\n`), 0, 0
