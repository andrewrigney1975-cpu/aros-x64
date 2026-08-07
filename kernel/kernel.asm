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
    call idt_install
    call irq_install
    sti
    call paging_init

    mov rcx, rbx                        ; boot_info
    call pmm_init

    call fb_clear
    mov ecx, 4                          ; column
    mov edx, 2                          ; row
    lea r8, [rel msg_hello]
    call fb_draw_string

    call storage_init_and_test
    call nvme_init_and_test

    call exfat_mount
    test eax, eax
    jz .exfat_bad

    lea rcx, [rel exfat_test_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .exfat_bad

    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    mov rdx, [rel exfat_find_result+8]  ; DataLength
    lea r8, [rel exfat_test_buf]
    mov r9d, [rel exfat_find_result+16] ; NoFatChain
    call exfat_read_file
    test eax, eax
    jz .exfat_bad

    ; DataLength must be exactly 5000, and the file must end with the
    ; known marker string -- proves both correct file size and that the
    ; FAT-chain-following read the file's second cluster correctly.
    mov rax, [rel exfat_find_result+8]
    cmp rax, 5000
    jne .exfat_bad
    lea rsi, [rel exfat_test_buf + 5000 - 18]   ; "END-OF-FILE-MARKER" is 18 bytes
    lea rdi, [rel exfat_marker]
    mov ecx, 18
.cmp_loop:
    mov al, [rsi]
    cmp al, [rdi]
    jne .exfat_bad
    inc rsi
    inc rdi
    dec ecx
    jnz .cmp_loop

    lea rcx, [rel msg_exfat_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 11
    lea r8, [rel msg_exfat_ok]
    call fb_draw_string
    jmp .exfat_done
.exfat_bad:
    lea rcx, [rel msg_exfat_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 11
    lea r8, [rel msg_exfat_bad]
    call fb_draw_string
.exfat_done:

    ; Prove the timer IRQ is actually firing: busy-wait a bit, then check
    ; that timer_ticks moved. A real scheduler will use this tick later;
    ; for now it's just evidence interrupts genuinely work end to end.
    mov rcx, 200000000
.delay:
    dec rcx
    jnz .delay

    mov rax, [rel timer_ticks]
    test rax, rax
    jz .timer_bad
    lea rcx, [rel msg_timer_ok]
    call serial_puts
    mov rax, [rel timer_ticks]
    call dbg_hex64
    mov ecx, 4
    mov edx, 13
    lea r8, [rel msg_timer_ok]
    call fb_draw_string
    jmp .timer_done
.timer_bad:
    lea rcx, [rel msg_timer_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 13
    lea r8, [rel msg_timer_bad]
    call fb_draw_string
.timer_done:

    ; Exercise the physical allocator: two distinct pages, free one, then
    ; confirm the freed frame gets reused (first-fit scans from the start).
    call pmm_alloc_page
    test rax, rax
    jz .pmm_bad
    mov r12, rax                        ; r12 = page A
    call pmm_alloc_page
    test rax, rax
    jz .pmm_bad
    mov r13, rax                        ; r13 = page B
    cmp r12, r13
    je .pmm_bad                         ; must be distinct frames

    mov rcx, r12
    call pmm_free_page
    call pmm_alloc_page
    test rax, rax
    jz .pmm_bad
    cmp rax, r12
    jne .pmm_bad                        ; freed frame should be reused

    lea rcx, [rel msg_pmm_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 15
    lea r8, [rel msg_pmm_ok]
    call fb_draw_string
    jmp .pmm_done
.pmm_bad:
    lea rcx, [rel msg_pmm_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 15
    lea r8, [rel msg_pmm_bad]
    call fb_draw_string
.pmm_done:

    ; Exercise the VMM: map two physical frames into adjacent pages of a
    ; virtual arena that is NOT part of the identity map (VMM_VIRT_BASE,
    ; 1TB), write through the virtual addresses, and confirm the data
    ; really landed at the underlying physical addresses -- proving this
    ; is genuine translation, not coincidental identity mapping.
    call pmm_alloc_page
    test rax, rax
    jz .vmm_bad
    mov r12, rax                        ; r12 = physical frame A
    call pmm_alloc_page
    test rax, rax
    jz .vmm_bad
    mov r13, rax                        ; r13 = physical frame B

    mov rcx, VMM_VIRT_BASE
    mov rdx, r12
    call vmm_map_page
    test eax, eax
    jz .vmm_bad
    mov rcx, VMM_VIRT_BASE + 4096
    mov rdx, r13
    call vmm_map_page
    test eax, eax
    jz .vmm_bad

    mov rax, VMM_VIRT_BASE
    mov byte [rax], 0xCC
    mov rax, VMM_VIRT_BASE + 4096
    mov byte [rax], 0xDD

    cmp byte [r12], 0xCC                ; landed at the real physical frame?
    jne .vmm_bad
    cmp byte [r13], 0xDD
    jne .vmm_bad

    lea rcx, [rel msg_vmm_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 16
    lea r8, [rel msg_vmm_ok]
    call fb_draw_string
    jmp .vmm_done
.vmm_bad:
    lea rcx, [rel msg_vmm_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 16
    lea r8, [rel msg_vmm_bad]
    call fb_draw_string
.vmm_done:

    ; Exercise the heap: two live blocks with different content (proving
    ; no overlap), free one, and confirm the freed block gets reused.
    mov ecx, 64
    call kmalloc
    test rax, rax
    jz .kheap_bad
    mov r12, rax                        ; r12 = block A
    mov byte [rax], 0xAA

    mov ecx, 128
    call kmalloc
    test rax, rax
    jz .kheap_bad
    mov r13, rax                        ; r13 = block B
    mov byte [rax], 0xBB

    cmp byte [r12], 0xAA                ; A must still read back correctly
    jne .kheap_bad
    cmp byte [r13], 0xBB
    jne .kheap_bad

    mov rcx, r12
    call kfree
    mov ecx, 64
    call kmalloc
    test rax, rax
    jz .kheap_bad
    cmp rax, r12
    jne .kheap_bad                      ; freed block should be reused

    ; Multi-page allocation: 10000 bytes spans 3 pages. Write recognizable
    ; bytes at the start, at each page boundary, and near the end, proving
    ; the VMM really stitched (possibly non-contiguous) physical pages
    ; into one contiguous virtual range rather than just getting lucky.
    mov ecx, 10000
    call kmalloc
    test rax, rax
    jz .kheap_bad
    mov r14, rax                        ; r14 = large block
    mov byte [rax+0], 0x11
    mov byte [rax+4096], 0x22
    mov byte [rax+8192], 0x33
    mov byte [rax+9999], 0x44

    cmp byte [r14+0], 0x11
    jne .kheap_bad
    cmp byte [r14+4096], 0x22
    jne .kheap_bad
    cmp byte [r14+8192], 0x33
    jne .kheap_bad
    cmp byte [r14+9999], 0x44
    jne .kheap_bad

    mov rcx, r14
    call kfree
    mov ecx, 20000                      ; a second, larger multi-page alloc
    call kmalloc
    test rax, rax
    jz .kheap_bad

    lea rcx, [rel msg_kheap_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 17
    lea r8, [rel msg_kheap_ok]
    call fb_draw_string
    jmp .kheap_done
.kheap_bad:
    lea rcx, [rel msg_kheap_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 17
    lea r8, [rel msg_kheap_bad]
    call fb_draw_string
.kheap_done:

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

    xor ecx, ecx                        ; LBA 0
    mov edx, 1                          ; 1 sector
    lea r8, [rel ahci_databuf]
    call ahci_read_sectors
    test eax, eax
    jz .read_failed

    mov dword [rel storage_active_driver], STORAGE_AHCI

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
; nvme_init_and_test: locate an NVMe controller over PCI, bring up its
; admin queue, create one I/O queue pair, read LBA 0 and check the 0xAA55
; boot-sector signature. Same verification approach as storage_init_and_test,
; for the other half of "boot from NVMe or AHCI".
; -------------------------------------------------------------------------
nvme_init_and_test:
    push rax
    push rcx
    push rdx
    push rsi

    mov cl, 0x01                        ; class    = mass storage
    mov ch, 0x08                        ; subclass = NVMe
    mov dl, 0x02                        ; progif   = NVMe I/O controller
    call pci_find_class
    jc .no_controller

    mov esi, ecx                        ; esi = device's config-space base

    mov ecx, esi
    add ecx, 0x04                       ; PCI command register
    call pci_read32
    or eax, 0x00000006                  ; Memory Space Enable | Bus Master Enable
    call pci_write32

    mov ecx, esi
    add ecx, 0x10                       ; BAR0 (low 32 bits of MMIO base)
    call pci_read32
    and eax, 0xFFFFFFF0
    push rax
    mov ecx, esi
    add ecx, 0x14                       ; BAR1 (high 32 bits, 64-bit BAR)
    call pci_read32
    shl rax, 32
    pop rdx
    or rax, rdx
    mov rcx, rax                        ; NVMe MMIO base

    call nvme_init
    test eax, eax
    jz .init_failed

    call nvme_identify_namespace ; discover the real block size; a
                                  ; failure here just leaves the 512-byte
                                  ; default, not fatal to init

    call nvme_create_io_queues
    test eax, eax
    jz .init_failed

    xor ecx, ecx                        ; LBA 0
    mov edx, 1                          ; 1 sector
    lea r8, [rel nvme_databuf]
    call nvme_read_sectors
    test eax, eax
    jz .read_failed

    mov dword [rel storage_active_driver], STORAGE_NVME

    lea rcx, [rel msg_nvme_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 7
    lea r8, [rel msg_nvme_ok]
    call fb_draw_string

    mov ax, [rel nvme_databuf+510]
    cmp ax, 0xAA55
    jne .bad_sig
    lea rcx, [rel msg_sig_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 8
    lea r8, [rel msg_sig_ok]
    call fb_draw_string
    jmp .done

.bad_sig:
    lea rcx, [rel msg_sig_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 8
    lea r8, [rel msg_sig_bad]
    call fb_draw_string
    jmp .done

.read_failed:
    lea rcx, [rel msg_nvme_read_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 7
    lea r8, [rel msg_nvme_read_fail]
    call fb_draw_string
    jmp .done

.init_failed:
    lea rcx, [rel msg_nvme_init_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 7
    lea r8, [rel msg_nvme_init_fail]
    call fb_draw_string
    jmp .done

.no_controller:
    lea rcx, [rel msg_nvme_not_found]
    call serial_puts
    mov ecx, 4
    mov edx, 7
    lea r8, [rel msg_nvme_not_found]
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
msg_exfat_ok:         db 'exFAT: TEST.TXT found and read back correctly', 13, 10, 0
msg_exfat_bad:        db 'exFAT: mount, find, or read FAILED', 13, 10, 0
msg_timer_ok:         db 'Timer: IRQ0 firing, ticks=', 0
msg_timer_bad:        db 'Timer: no ticks observed', 13, 10, 0
msg_pmm_ok:           db 'PMM: alloc/free/reuse OK', 13, 10, 0
msg_pmm_bad:          db 'PMM: alloc/free/reuse FAILED', 13, 10, 0
msg_vmm_ok:           db 'VMM: virtual->physical mapping OK', 13, 10, 0
msg_vmm_bad:          db 'VMM: virtual->physical mapping FAILED', 13, 10, 0
msg_kheap_ok:         db 'kmalloc/kfree: alloc/content/reuse OK', 13, 10, 0
msg_kheap_bad:        db 'kmalloc/kfree: FAILED', 13, 10, 0
exfat_test_name:      db 'TEST.TXT', 0
exfat_marker:         db 'END-OF-FILE-MARKER'

align 8
exfat_find_result: times 24 db 0

align 4096
exfat_test_buf: times 8192 db 0

msg_hello:            db 'Hello, kernal!', 13, 10, 0
msg_ahci_not_found:   db 'AHCI: no controller found', 13, 10, 0
msg_ahci_no_port:     db 'AHCI: no active SATA port', 13, 10, 0
msg_ahci_read_fail:   db 'AHCI: LBA0 read failed/timed out', 13, 10, 0
msg_ahci_ok:          db 'AHCI: LBA0 read OK', 13, 10, 0
msg_sig_ok:           db 'boot signature 0xAA55 OK', 13, 10, 0
msg_sig_bad:          db 'boot signature mismatch', 13, 10, 0

msg_nvme_not_found:   db 'NVMe: no controller found', 13, 10, 0
msg_nvme_init_fail:   db 'NVMe: controller/queue init failed', 13, 10, 0
msg_nvme_read_fail:   db 'NVMe: LBA0 read failed/timed out', 13, 10, 0
msg_nvme_ok:          db 'NVMe: LBA0 read OK', 13, 10, 0

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
%include "idt.inc"
%include "pic.inc"
%include "paging.inc"
%include "pmm.inc"
%include "vmm.inc"
%include "kheap.inc"
%include "pci.inc"
%include "ahci.inc"
%include "nvme.inc"
%include "storage.inc"
%include "exfat.inc"
