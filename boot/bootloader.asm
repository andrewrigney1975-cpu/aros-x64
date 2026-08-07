; =============================================================================
; arOS-X64 UEFI Bootloader
;
; A hand-written PE32+ (EFI_APPLICATION) image assembled directly with
; `nasm -f bin` -- no linker involved. The PE/COFF header below is emitted
; as raw bytes/labels so that NASM's flat-binary output *is* the .efi file.
;
; Calling convention: firmware calls EfiMain(RCX=ImageHandle, RDX=SystemTable)
; per the Microsoft x64 ABI (which UEFI mandates): 32-byte shadow space must
; be reserved by the caller before any call, and RSP must be 16-byte aligned
; immediately before a CALL instruction.
; =============================================================================

BITS 64
ORG 0

%define IMAGE_BASE       0x400000
%define SECT_ALIGN       0x20
%define FILE_ALIGN       0x20
%define KERNEL_LOAD_ADDR 0x200000

; -----------------------------------------------------------------------
; DOS header (64 bytes) -- only 'MZ' and e_lfanew matter to the PE loader
; -----------------------------------------------------------------------
dos_header:
    db 'MZ'
    times (0x3C - 2) db 0
    dd pe_header - dos_header          ; e_lfanew -> offset 0x40

; -----------------------------------------------------------------------
; PE header
; -----------------------------------------------------------------------
pe_header:
    db 'PE', 0, 0

coff_header:
    dw 0x8664                          ; Machine = IMAGE_FILE_MACHINE_AMD64
    dw 1                                ; NumberOfSections
    dd 0                                 ; TimeDateStamp
    dd 0                                 ; PointerToSymbolTable
    dd 0                                 ; NumberOfSymbols
    dw opt_header_end - opt_header       ; SizeOfOptionalHeader
    dw 0x0022                            ; Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

opt_header:
    dw 0x020B                          ; Magic = PE32+
    db 0                                ; MajorLinkerVersion
    db 0                                 ; MinorLinkerVersion
    dd code_end - code_start             ; SizeOfCode
    dd 0                                  ; SizeOfInitializedData
    dd 0                                   ; SizeOfUninitializedData
    dd entry - dos_header                   ; AddressOfEntryPoint (RVA)
    dd code_start - dos_header               ; BaseOfCode (RVA)
    dq IMAGE_BASE                             ; ImageBase
    dd SECT_ALIGN                              ; SectionAlignment
    dd FILE_ALIGN                               ; FileAlignment
    dw 0                                         ; MajorOperatingSystemVersion
    dw 0                                          ; MinorOperatingSystemVersion
    dw 0                                           ; MajorImageVersion
    dw 0                                            ; MinorImageVersion
    dw 0                                             ; MajorSubsystemVersion
    dw 0                                              ; MinorSubsystemVersion
    dd 0                                               ; Win32VersionValue
    dd image_end - dos_header                           ; SizeOfImage
    dd headers_end - dos_header                          ; SizeOfHeaders
    dd 0                                                  ; CheckSum
    dw 10                                                  ; Subsystem = EFI_APPLICATION
    dw 0                                                    ; DllCharacteristics
    dq 0x100000                                              ; SizeOfStackReserve
    dq 0x1000                                                 ; SizeOfStackCommit
    dq 0x100000                                                ; SizeOfHeapReserve
    dq 0x1000                                                   ; SizeOfHeapCommit
    dd 0                                                         ; LoaderFlags
    dd 16                                                         ; NumberOfRvaAndSizes
    times 16 dq 0                                                  ; Data directories (unused)
opt_header_end:

section_headers:
    db '.text', 0, 0, 0                ; Name
    dd code_end - code_start            ; VirtualSize
    dd code_start - dos_header           ; VirtualAddress (RVA)
    dd code_end - code_start              ; SizeOfRawData
    dd code_start - dos_header             ; PointerToRawData
    dd 0                                    ; PointerToRelocations
    dd 0                                     ; PointerToLinenumbers
    dw 0                                      ; NumberOfRelocations
    dw 0                                        ; NumberOfLinenumbers
    dd 0xE0000020                                ; CODE | EXECUTE | READ | WRITE

headers_end:
    align FILE_ALIGN, db 0

; =========================================================================
; Code section
; =========================================================================
code_start:

entry:
    ; RCX = EFI_HANDLE ImageHandle, RDX = EFI_SYSTEM_TABLE *SystemTable
    push rbx
    push rsi
    push rdi
    push r12
    sub rsp, 40                        ; 32B shadow space + 8B to fix 16B align
                                        ; (4 pushes leave RSP@+8 mod16; +40 flips
                                        ; it back to 0 mod16, required before CALL)

    mov rbx, rdx                        ; rbx = SystemTable (non-volatile)
    mov r12, rcx                         ; r12 = ImageHandle (non-volatile)

    call serial_init

    lea rcx, [rel msg_banner]
    call serial_puts

    ; SystemTable->ConOut->OutputString(ConOut, msg_banner_u16)
    mov rax, [rbx+0x40]                 ; rax = ConOut
    mov rcx, rax
    lea rdx, [rel msg_banner_u16]
    call qword [rax+8]

    ; ---------------------------------------------------------------
    ; Locate the volume we were loaded from and open \AROS\KERNEL.BIN
    ; ---------------------------------------------------------------
    mov rax, [rbx+0x60]                 ; rax = BootServices
    mov rcx, r12                         ; ImageHandle
    lea rdx, [rel guid_loaded_image]
    lea r8,  [rel var_loaded_image]
    call qword [rax+152]                  ; BS->HandleProtocol
    test eax, eax
    jnz .efi_fail

    mov rax, [rel var_loaded_image]      ; EFI_LOADED_IMAGE_PROTOCOL*
    mov rcx, [rax+24]                     ; DeviceHandle
    mov rax, [rbx+0x60]                    ; BootServices
    lea rdx, [rel guid_simple_fs]
    lea r8,  [rel var_fs]
    call qword [rax+152]                    ; BS->HandleProtocol
    test eax, eax
    jnz .efi_fail

    mov rax, [rel var_fs]                 ; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*
    mov rcx, rax
    lea rdx, [rel var_root]
    call qword [rax+8]                      ; ->OpenVolume
    test eax, eax
    jnz .efi_fail

    mov rax, [rel var_root]                ; EFI_FILE_PROTOCOL* (root dir)
    mov rcx, rax
    lea rdx, [rel var_file]
    lea r8,  [rel kernel_path_u16]
    mov r9, 1                                ; EFI_FILE_MODE_READ
    mov qword [rsp+32], 0                     ; Attributes (5th arg, on stack)
    call qword [rax+8]                         ; ->Open
    test eax, eax
    jnz .efi_fail

    ; ---------------------------------------------------------------
    ; Query file size via GetInfo(EFI_FILE_INFO_GUID)
    ; ---------------------------------------------------------------
    mov rax, [rel var_file]
    mov rcx, rax
    lea rdx, [rel guid_file_info]
    mov qword [rel var_info_size], 512
    lea r8,  [rel var_info_size]
    lea r9,  [rel var_info_buf]
    call qword [rax+64]                       ; ->GetInfo
    test eax, eax
    jnz .efi_fail

    mov rax, [rel var_info_buf+8]              ; EFI_FILE_INFO.FileSize
    mov [rel var_kernel_size_io], rax          ; Read() BufferSize (in = FileSize)
    add rax, 4095
    shr rax, 12                                 ; pages = ceil(size / 4096)
    mov [rel var_pages], rax

    ; ---------------------------------------------------------------
    ; AllocatePages(AllocateAddress, EfiLoaderCode, pages, &KERNEL_LOAD_ADDR)
    ; ---------------------------------------------------------------
    mov qword [rel var_kernel_addr], KERNEL_LOAD_ADDR
    mov rax, [rbx+0x60]
    mov rcx, 2                                  ; AllocateAddress
    mov rdx, 1                                   ; EfiLoaderCode
    mov r8,  [rel var_pages]
    lea r9,  [rel var_kernel_addr]
    call qword [rax+40]                           ; BS->AllocatePages
    test eax, eax
    jnz .efi_fail

    ; ---------------------------------------------------------------
    ; Read(file, &size, buffer)
    ; ---------------------------------------------------------------
    mov rax, [rel var_file]
    mov rcx, rax
    lea rdx, [rel var_kernel_size_io]
    mov r8,  [rel var_kernel_addr]
    call qword [rax+32]                           ; ->Read
    test eax, eax
    jnz .efi_fail

    lea rcx, [rel msg_jumping]
    call serial_puts

    ; ---------------------------------------------------------------
    ; Hand off to the kernel: RCX=ImageHandle, RDX=SystemTable
    ; ---------------------------------------------------------------
    mov rcx, r12
    mov rdx, rbx
    mov rax, [rel var_kernel_addr]
    jmp rax

.efi_fail:
    push rax
    lea rcx, [rel msg_efi_fail]
    call serial_puts
    pop rax
    call print_hex_rax

.hang:
    jmp .hang

; -------------------------------------------------------------------------
; print_hex_rax: dump RAX as 16 hex digits over serial (debug helper).
; -------------------------------------------------------------------------
print_hex_rax:
    push rax
    push rbx
    push rcx
    mov rbx, rax
    mov rcx, 60
.loop:
    mov rax, rbx
    shr rax, cl
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    call serial_putc
    sub rcx, 4
    jns .loop
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; serial_init: initialise COM1 (0x3F8) to 38400 8N1 with FIFO enabled.
; -------------------------------------------------------------------------
serial_init:
    mov dx, 0x3F9
    xor al, al
    out dx, al                          ; disable UART interrupts

    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                          ; enable DLAB

    mov dx, 0x3F8
    mov al, 0x03
    out dx, al                          ; divisor low  (115200/3 = 38400)

    mov dx, 0x3F9
    xor al, al
    out dx, al                          ; divisor high

    mov dx, 0x3FB
    mov al, 0x03
    out dx, al                          ; 8N1, clear DLAB

    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al                          ; enable + clear FIFOs, 14-byte thresh

    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al                          ; RTS/DSR set, IRQs off
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
msg_banner:     db 'arOS-X64 bootloader: UEFI entry OK (serial)', 13, 10, 0
msg_jumping:    db 'arOS-X64 bootloader: kernel loaded, jumping to it...', 13, 10, 0
msg_efi_fail:   db 'arOS-X64 bootloader: an EFI call failed, halting.', 13, 10, 0

msg_banner_u16:
    db __utf16le__(`arOS-X64 bootloader: UEFI entry OK\r\n`), 0, 0

; EFI_LOADED_IMAGE_PROTOCOL_GUID {5B1B31A1-9562-11d2-8E3F-00A0C969723B}
guid_loaded_image:
    dd 0x5B1B31A1
    dw 0x9562
    dw 0x11d2
    db 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B

; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID {964E5B22-6459-11d2-8E39-00A0C969723B}
guid_simple_fs:
    dd 0x964E5B22
    dw 0x6459
    dw 0x11d2
    db 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B

; EFI_FILE_INFO_GUID {09576E92-6D3F-11d2-8E39-00A0C969723B}
guid_file_info:
    dd 0x09576E92
    dw 0x6D3F
    dw 0x11d2
    db 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B

kernel_path_u16:
    db __utf16le__(`\\AROS\\KERNEL.BIN`), 0, 0

var_loaded_image:    dq 0
var_fs:               dq 0
var_root:              dq 0
var_file:                dq 0
var_info_size:            dq 0
var_kernel_size_io:        dq 0
var_pages:                   dq 0
var_kernel_addr:              dq 0
var_info_buf:                   times 512 db 0

code_end:
    align FILE_ALIGN, db 0
image_end:
