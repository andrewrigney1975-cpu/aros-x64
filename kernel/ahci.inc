; =============================================================================
; Minimal AHCI driver: bring up one SATA port and read LBA 0.
;
; Scope for this phase: prove the kernel can drive real storage hardware
; directly (no firmware involved) end to end. Multi-sector/general
; read-write, multiple ports/devices, and NCQ are follow-up work once the
; exFAT layer needs them.
;
; All buffers below are statically reserved in the kernel's own flat image,
; which the bootloader loads at a fixed physical address with no kernel-
; owned page tables yet -- so "virtual address" and "physical address" are
; the same number here (we're still riding the firmware's identity map).
; That assumption gets revisited once the kernel builds its own paging.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

AHCI_GHC_AE       equ 0x80000000        ; GHC.AE (AHCI Enable)

AHCI_PXCMD_ST     equ 0x0001
AHCI_PXCMD_FRE    equ 0x0010
AHCI_PXCMD_FR     equ 0x4000
AHCI_PXCMD_CR     equ 0x8000

; -------------------------------------------------------------------------
; ahci_init: RCX = ABAR (AHCI MMIO base). Finds the first implemented port
; with an active SATA drive, brings its command engine up, and points it at
; our static command list / FIS-receive buffers.
; Returns EAX=1 on success, EAX=0 if no usable port was found.
; -------------------------------------------------------------------------
ahci_init:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov [rel ahci_abar], rcx
    mov rbx, rcx                        ; rbx = ABAR

    mov eax, [rbx+0x04]                 ; GHC
    or eax, AHCI_GHC_AE
    mov [rbx+0x04], eax

    mov edi, [rbx+0x0C]                 ; PI (ports implemented bitmap)
    xor r8d, r8d                        ; port index
.scan_ports:
    bt edi, r8d
    jnc .next_port

    mov esi, r8d
    shl esi, 7                          ; port reg offset = port * 0x80
    add esi, 0x100

    mov eax, [rbx+rsi+0x28]             ; PxSSTS
    mov edx, eax
    and edx, 0x0F                       ; DET
    cmp edx, 3
    jne .next_port
    mov edx, eax
    shr edx, 8
    and edx, 0x0F                       ; IPM
    cmp edx, 1
    jne .next_port

    mov eax, [rbx+rsi+0x24]             ; PxSIG
    cmp eax, 0x00000101                 ; SATA drive (not ATAPI/PM/SEMB)
    jne .next_port

    ; Found a port. Stop its command engine before reprogramming it.
    mov eax, [rbx+rsi+0x18]             ; PxCMD
    and eax, ~(AHCI_PXCMD_ST | AHCI_PXCMD_FRE)
    mov [rbx+rsi+0x18], eax

    mov r9d, 1000000
.wait_stop:
    mov eax, [rbx+rsi+0x18]
    test eax, (AHCI_PXCMD_CR | AHCI_PXCMD_FR)
    jz .stopped
    dec r9d
    jnz .wait_stop
.stopped:

    lea rax, [rel ahci_cmdlist]
    mov [rbx+rsi+0x00], eax             ; PxCLB
    mov dword [rbx+rsi+0x04], 0         ; PxCLBU
    lea rax, [rel ahci_fis]
    mov [rbx+rsi+0x08], eax             ; PxFB
    mov dword [rbx+rsi+0x0C], 0         ; PxFBU

    mov dword [rbx+rsi+0x10], 0xFFFFFFFF ; PxIS: clear all (write-1-to-clear)
    mov dword [rbx+rsi+0x30], 0xFFFFFFFF ; PxSERR: clear all

    mov eax, [rbx+rsi+0x18]
    or eax, AHCI_PXCMD_FRE
    mov [rbx+rsi+0x18], eax
    mov eax, [rbx+rsi+0x18]
    or eax, AHCI_PXCMD_ST
    mov [rbx+rsi+0x18], eax

    mov [rel ahci_port], r8d
    call ahci_identify           ; discover the real logical sector size;
                                  ; a failure here just leaves the 512-byte
                                  ; default in place, not fatal to init
    mov eax, 1
    jmp .done

.next_port:
    inc r8d
    cmp r8d, 32
    jl .scan_ports

    xor eax, eax                        ; no usable port found
.done:
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; ahci_identify: sends ATA IDENTIFY DEVICE to the port ahci_init selected
; and parses the real logical sector size out of it (word 106 bit 12: 1
; means "see words 117-118 for the real size in words"; otherwise the
; classic 512-byte default applies). ahci_sector_size is left at its
; 512-byte default if this fails or the device doesn't report one --
; not fatal to ahci_init. Returns EAX=1/0.
; -------------------------------------------------------------------------
ahci_identify:
    push rbx
    push rcx
    push rsi
    push r9

    mov rbx, [rel ahci_abar]
    mov eax, [rel ahci_port]
    mov esi, eax
    shl esi, 7
    add esi, 0x100

    lea rax, [rel ahci_cmdtable]
    lea rcx, [rel ahci_cmdlist]
    mov dword [rcx+0x00], (1 << 16) | 5 ; PRDTL=1, CFL=5, Write=0
    mov dword [rcx+0x04], 0
    mov [rcx+0x08], eax
    mov dword [rcx+0x0C], 0

    lea rcx, [rel ahci_cmdtable]
    mov dword [rcx+0x00], 0
    mov dword [rcx+0x04], 0
    mov dword [rcx+0x08], 0
    mov dword [rcx+0x0C], 0
    mov dword [rcx+0x10], 0
    mov byte  [rcx+0x00], 0x27          ; FIS type = H2D register
    mov byte  [rcx+0x01], 0x80          ; C=1
    mov byte  [rcx+0x02], 0xEC          ; command = IDENTIFY DEVICE
    mov byte  [rcx+0x07], 0x40

    lea rax, [rel ahci_identbuf]
    mov [rcx+0x80], eax                 ; DBA
    mov dword [rcx+0x84], 0
    mov dword [rcx+0x88], 0
    mov dword [rcx+0x8C], 511           ; 512 bytes

    mov dword [rbx+rsi+0x38], 1
    mov r9d, 20000000
.wait_cmd:
    mov eax, [rbx+rsi+0x38]
    test eax, 1
    jz .cmd_done
    dec r9d
    jnz .wait_cmd
    xor eax, eax
    jmp .out
.cmd_done:
    mov eax, [rbx+rsi+0x20]
    test al, 0x01
    jnz .fail

    movzx eax, word [rel ahci_identbuf + 212]   ; word 106
    test ax, 0x1000                              ; bit12
    jz .use_default
    mov eax, [rel ahci_identbuf + 234]            ; words 117-118 (in words)
    shl eax, 1                                     ; -> bytes
    mov [rel ahci_sector_size], eax
.use_default:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop rsi
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; ahci_read_sectors: RCX = LBA (48-bit), RDX = sector count (1-65535),
; R8 = destination buffer (contiguous, >= count*ahci_sector_size bytes).
; Single PRDT entry, so count is capped by what one entry can address.
; Returns EAX=1 on success, EAX=0 on timeout/error.
; -------------------------------------------------------------------------
ahci_read_sectors:
    push rbx
    push rdx
    push rsi
    push rdi
    push r9
    push r10

    mov r10, rcx                        ; r10 = LBA
    mov rdi, r8                         ; rdi = destination buffer

    mov rbx, [rel ahci_abar]
    mov eax, [rel ahci_port]
    mov esi, eax
    shl esi, 7
    add esi, 0x100                      ; esi = this port's register offset

    ; Command header 0: CFL=5 dwords, Write=0, PRDTL=1
    lea rax, [rel ahci_cmdtable]
    lea rcx, [rel ahci_cmdlist]
    mov dword [rcx+0x00], (1 << 16) | 5 ; PRDTL=1, CFL=5, Write=0
    mov dword [rcx+0x04], 0             ; PRDBC
    mov [rcx+0x08], eax                 ; CTBA
    mov dword [rcx+0x0C], 0             ; CTBAU

    ; Command table: H2D register FIS requesting READ DMA EXT
    lea rcx, [rel ahci_cmdtable]
    mov dword [rcx+0x00], 0             ; zero the CFIS area first
    mov dword [rcx+0x04], 0
    mov dword [rcx+0x08], 0
    mov dword [rcx+0x0C], 0
    mov dword [rcx+0x10], 0
    mov byte  [rcx+0x00], 0x27          ; FIS type = H2D register
    mov byte  [rcx+0x01], 0x80          ; C=1 (command), PM port 0
    mov byte  [rcx+0x02], 0x25          ; command = READ DMA EXT
    mov rax, r10                        ; LBA
    mov [rcx+0x04], al
    shr rax, 8
    mov [rcx+0x05], al
    shr rax, 8
    mov [rcx+0x06], al
    mov byte [rcx+0x07], 0x40           ; device: LBA mode
    shr rax, 8
    mov [rcx+0x08], al
    shr rax, 8
    mov [rcx+0x09], al
    shr rax, 8
    mov [rcx+0x0A], al
    mov [rcx+0x0C], dx                  ; sector count (not 0's-based)

    ; PRDT entry 0 at command-table offset 0x80
    mov eax, edi
    mov [rcx+0x80], eax                 ; DBA
    mov dword [rcx+0x84], 0             ; DBAU
    mov dword [rcx+0x88], 0             ; reserved
    movzx eax, dx
    imul eax, [rel ahci_sector_size]
    dec eax                             ; byte count - 1
    mov [rcx+0x8C], eax

    ; Ring the doorbell for slot 0 and wait for completion.
    mov dword [rbx+rsi+0x38], 1         ; PxCI
    mov r9d, 20000000
.wait_cmd:
    mov eax, [rbx+rsi+0x38]
    test eax, 1
    jz .cmd_done
    dec r9d
    jnz .wait_cmd
    xor eax, eax                        ; timed out
    jmp .out

.cmd_done:
    mov eax, [rbx+rsi+0x20]             ; PxTFD
    test al, 0x01                       ; ERR bit
    jnz .fail
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    ret

; =========================================================================
; Data (statically reserved, physically addressed via the identity map)
; =========================================================================
ahci_abar:        dq 0
ahci_port:        dd 0
ahci_sector_size: dd 512

align 512
ahci_identbuf: times 512 db 0

align 1024
ahci_cmdlist: times 1024 db 0

align 256
ahci_fis: times 256 db 0

align 256
ahci_cmdtable: times 256 db 0

align 128
ahci_databuf: times 512 db 0
