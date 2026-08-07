; =============================================================================
; arOS-X64 Kernel (phase 2: own GDT + GOP framebuffer text console)
;
; Loaded as a flat binary by the bootloader at KERNEL_LOAD_ADDR (0x200000)
; and entered with a plain JMP (not CALL, so no return address on the
; stack) after the bootloader has already called ExitBootServices:
;   RCX = boot_info* (see field offsets below)
; Boot Services, ConOut and the UEFI console are GONE by this point -- the
; only way to talk to the outside world is the framebuffer we were handed
; and raw hardware I/O (COM1 serial).
; =============================================================================

BITS 64
ORG 0x200000

; boot_info field offsets (struct built by the bootloader; see boot_info in
; boot/bootloader.asm)
FB_BASE     equ 0     ; qword: physical framebuffer address
FB_SIZE     equ 8     ; qword: framebuffer size in bytes
FB_WIDTH    equ 16    ; dword: horizontal resolution, in pixels
FB_HEIGHT   equ 20    ; dword: vertical resolution, in pixels
FB_STRIDE   equ 24    ; dword: pixels per scanline (may exceed FB_WIDTH)
FB_PIXFMT   equ 28    ; dword: 0=RGBX8888, 1=BGRX8888, 2=BitMask, 3=BltOnly

entry:
    mov rbx, rcx                        ; rbx = boot_info (kept for the
                                         ; whole kernel; must not be
                                         ; clobbered by any helper below)
    sub rsp, 32                         ; shadow space (JMP entry starts
                                         ; 16B-aligned, so no parity fixup
                                         ; is needed here -- see bootloader
                                         ; for the CALL-entry case, which
                                         ; needs an extra 8 bytes)

    call serial_init

    lea rcx, [rel msg_hello]
    call serial_puts

    call gdt_install

    call fb_clear
    mov ecx, 4                          ; column
    mov edx, 2                          ; row
    lea r8, [rel msg_hello]
    call fb_draw_string

    call storage_init_and_test

.hang:
    jmp .hang

; -------------------------------------------------------------------------
; storage_init_and_test: locate an AHCI controller over PCI, bring up its
; first active SATA port, read LBA 0 and check the 0xAA55 boot-sector
; signature. Reports the outcome via serial and on-screen text. This is
; phase 3's proof that the kernel can drive real storage hardware without
; any help from firmware -- general read/write and the exFAT layer that
; will sit on top of it are follow-up work.
; -------------------------------------------------------------------------
storage_init_and_test:
    push rax
    push rcx
    push rdx
    push rsi

    mov cl, 0x01                        ; class    = mass storage
    mov ch, 0x06                        ; subclass = SATA
    mov dl, 0x01                        ; progif   = AHCI
    call pci_find_class
    jc .no_controller

    mov esi, ecx                        ; esi = device's config-space base

    mov ecx, esi
    add ecx, 0x04                       ; PCI command register
    call pci_read32
    or eax, 0x00000006                  ; Memory Space Enable | Bus Master Enable
    call pci_write32

    mov ecx, esi
    add ecx, 0x24                       ; BAR5 = ABAR
    call pci_read32
    and eax, 0xFFFFFFF0
    mov ecx, eax                        ; zero-extends into rcx

    call ahci_init
    test eax, eax
    jz .no_port

    lea rcx, [rel ahci_databuf]
    call ahci_read_lba0
    test eax, eax
    jz .read_failed

    lea rcx, [rel msg_ahci_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 4
    lea r8, [rel msg_ahci_ok]
    call fb_draw_string

    mov ax, [rel ahci_databuf+510]
    cmp ax, 0xAA55
    jne .bad_sig
    lea rcx, [rel msg_sig_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 5
    lea r8, [rel msg_sig_ok]
    call fb_draw_string
    jmp .done

.bad_sig:
    lea rcx, [rel msg_sig_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 5
    lea r8, [rel msg_sig_bad]
    call fb_draw_string
    jmp .done

.read_failed:
    lea rcx, [rel msg_ahci_read_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 4
    lea r8, [rel msg_ahci_read_fail]
    call fb_draw_string
    jmp .done

.no_port:
    lea rcx, [rel msg_ahci_no_port]
    call serial_puts
    mov ecx, 4
    mov edx, 4
    lea r8, [rel msg_ahci_no_port]
    call fb_draw_string
    jmp .done

.no_controller:
    lea rcx, [rel msg_ahci_not_found]
    call serial_puts
    mov ecx, 4
    mov edx, 4
    lea r8, [rel msg_ahci_not_found]
    call fb_draw_string

.done:
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; gdt_install: load our own flat 64-bit GDT and reload every segment
; register, so we stop depending on whatever descriptor table the firmware
; happened to leave behind.
; -------------------------------------------------------------------------
gdt_install:
    lgdt [rel gdt_descriptor]
    push 0x08                           ; CODE_SEL (pushed first = popped 2nd)
    lea rax, [rel .reload_cs]
    push rax                            ; return RIP (pushed last = popped 1st)
    o64 retf
.reload_cs:
    mov ax, 0x10                        ; DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; -------------------------------------------------------------------------
; fb_clear: fill the whole framebuffer with black.
; -------------------------------------------------------------------------
fb_clear:
    push rdi
    push rax
    push rcx
    cld
    mov rdi, [rbx+FB_BASE]
    mov rcx, [rbx+FB_SIZE]
    shr rcx, 2                          ; count in dwords
    xor eax, eax
    rep stosd
    pop rcx
    pop rax
    pop rdi
    ret

; -------------------------------------------------------------------------
; fb_draw_char: draw one glyph from font8x16. ECX=column, EDX=row (in 8x16
; character cells), R8B=ASCII code. Characters outside 32..126 render blank.
; -------------------------------------------------------------------------
fb_draw_char:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12

    mov r9d, ecx
    shl r9d, 3                          ; r9d = px = column * 8
    mov r10d, edx
    shl r10d, 4                         ; r10d = py = row * 16

    movzx eax, r8b
    sub eax, 32
    cmp eax, 95                         ; 95 printable glyphs (32..126)
    jae .done
    shl eax, 4                          ; * 16 bytes per glyph
    lea rsi, [rel font8x16]
    add rsi, rax                        ; rsi = this glyph's 16 scanline bytes

    xor r11d, r11d                      ; glyph scanline index 0..15
.row_loop:
    movzx r12d, byte [rsi + r11]        ; scanline bits, bit7 = leftmost pixel
    mov ecx, 8
.col_loop:
    test r12d, 0x80
    jz .skip_pixel
    mov eax, 8
    sub eax, ecx
    add eax, r9d                        ; eax = x
    mov edx, r10d
    add edx, r11d                       ; edx = y
    imul edx, dword [rbx+FB_STRIDE]
    add edx, eax                        ; edx = y*stride + x (zero-extends rdx)
    shl rdx, 2                          ; * 4 bytes/pixel
    mov rdi, [rbx+FB_BASE]
    add rdi, rdx
    mov dword [rdi], 0xFFFFFFFF         ; white; channel order doesn't matter
.skip_pixel:
    shl r12d, 1
    dec ecx
    jnz .col_loop

    inc r11d
    cmp r11d, 16
    jl .row_loop

.done:
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; fb_draw_string: ECX=start column, EDX=row, R8=pointer to NUL-terminated
; ASCII string. Draws left to right, one 8-pixel-wide cell per character.
; -------------------------------------------------------------------------
fb_draw_string:
    push rax
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    mov rsi, r8                         ; rsi = string pointer
    mov r9d, ecx                        ; r9d = current column
.next_char:
    movzx eax, byte [rsi]
    test al, al
    jz .done
    mov ecx, r9d
    mov r8b, al
    call fb_draw_char
    inc r9d
    inc rsi
    jmp .next_char
.done:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

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
msg_hello:            db 'Hello, kernal!', 13, 10, 0
msg_ahci_not_found:   db 'AHCI: no controller found', 13, 10, 0
msg_ahci_no_port:     db 'AHCI: no active SATA port', 13, 10, 0
msg_ahci_read_fail:   db 'AHCI: LBA0 read failed/timed out', 13, 10, 0
msg_ahci_ok:          db 'AHCI: LBA0 read OK', 13, 10, 0
msg_sig_ok:           db 'AHCI: boot signature 0xAA55 OK', 13, 10, 0
msg_sig_bad:          db 'AHCI: boot signature mismatch', 13, 10, 0

; -------------------------------------------------------------------------
; GDT: null, flat 64-bit code, flat 64-bit data.
; -------------------------------------------------------------------------
align 8
gdt_start:
    dq 0                                 ; null descriptor
gdt_code:
    dw 0xFFFF                            ; limit 0:15 (ignored in long mode)
    dw 0x0000                            ; base 0:15
    db 0x00                              ; base 16:23
    db 0x9A                              ; access: present,ring0,code,r/x
    db 0xAF                              ; flags(G=1,L=1) | limit 16:19
    db 0x00                              ; base 24:31
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                              ; access: present,ring0,data,r/w
    db 0xCF                              ; flags(G=1,D=1) | limit 16:19
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1           ; limit
    dq gdt_start                         ; base (absolute: fixed load addr)

%include "font8x16.inc"
%include "pci.inc"
%include "ahci.inc"
