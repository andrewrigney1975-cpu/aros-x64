; =============================================================================
; Physical memory manager: a bitmap-based 4KB frame allocator built from the
; UEFI memory map the bootloader captured before ExitBootServices (passed
; through boot_info at +32/+40/+48; see boot/bootloader.asm).
;
; Scope: tracks only the first 4GB of physical memory (1M frames, 128KB
; bitmap) -- a documented limit for this phase, not a design ceiling;
; extending it just means a bigger (or dynamically sized) bitmap once that
; matters. EfiBootServicesCode/Data and EfiConventionalMemory regions
; become free; everything else (including our own EfiLoaderCode/Data
; image, ACPI/MMIO/reserved regions) stays marked reserved.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

PMM_MAX_FRAMES      equ 1048576                   ; 4GB / 4KB
PMM_BITMAP_BYTES    equ PMM_MAX_FRAMES / 8         ; 128KB

; boot_info field offsets for the memory map (see boot_info in bootloader.asm)
BI_MMAP_BASE  equ 32
BI_MMAP_SIZE  equ 40
BI_MMAP_DESC  equ 48

EFI_BOOT_SERVICES_CODE  equ 3
EFI_BOOT_SERVICES_DATA  equ 4
EFI_CONVENTIONAL_MEMORY equ 7

; -------------------------------------------------------------------------
; pmm_init: RCX = boot_info pointer. Marks every tracked frame reserved,
; then frees the ones covered by usable-type regions in the memory map.
; -------------------------------------------------------------------------
pmm_init:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    mov r10, rcx                        ; r10 = boot_info

    lea rdi, [rel pmm_bitmap]
    mov eax, 0xFFFFFFFF
    mov ecx, PMM_BITMAP_BYTES / 4
    cld
    rep stosd

    mov rsi, [r10+BI_MMAP_BASE]         ; rsi = current descriptor
    mov r9, [r10+BI_MMAP_SIZE]
    add r9, rsi                          ; r9 = one-past-the-end address
    mov r8, [r10+BI_MMAP_DESC]            ; r8 = descriptor stride

.desc_loop:
    cmp rsi, r9
    jae .done

    mov eax, [rsi+0]                      ; Type
    cmp eax, EFI_BOOT_SERVICES_CODE
    je .free_region
    cmp eax, EFI_BOOT_SERVICES_DATA
    je .free_region
    cmp eax, EFI_CONVENTIONAL_MEMORY
    jne .next_desc
.free_region:
    mov rbx, [rsi+8]                      ; PhysicalStart
    mov rcx, [rsi+24]                      ; NumberOfPages
    call pmm_free_range
.next_desc:
    add rsi, r8
    jmp .desc_loop

.done:
    ; Frame 0 (physical address 0) is deliberately kept reserved even if
    ; the memory map says it's free: pmm_alloc_page returns 0 to signal
    ; "out of memory", so 0 must never also be a real, valid allocation.
    or byte [rel pmm_bitmap], 1

    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; pmm_free_range: RBX = physical start address, RCX = page count. Clears
; the corresponding bitmap bits (clipped to PMM_MAX_FRAMES).
; -------------------------------------------------------------------------
pmm_free_range:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8

    mov rsi, rbx
    shr rsi, 12                         ; rsi = first frame index
    cmp rsi, PMM_MAX_FRAMES
    jae .out

    mov rdx, rsi
    add rdx, rcx                        ; rdx = one-past-last frame index
    cmp rdx, PMM_MAX_FRAMES
    jbe .no_clip
    mov rdx, PMM_MAX_FRAMES
.no_clip:

    lea rdi, [rel pmm_bitmap]
.loop:
    cmp rsi, rdx
    jae .out
    mov r8, rsi
    shr r8, 3                           ; byte index
    mov rcx, rsi
    and cl, 7                           ; bit index
    mov al, 1
    shl al, cl
    not al
    and [rdi+r8], al
    inc rsi
    jmp .loop

.out:
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; pmm_alloc_page: returns RAX = physical address of a freshly-reserved 4KB
; frame, or 0 if none are free. Linear first-fit scan.
; -------------------------------------------------------------------------
pmm_alloc_page:
    push rbx
    push rcx
    push rdx
    push rdi

    lea rdi, [rel pmm_bitmap]
    xor rbx, rbx                        ; rbx = byte index
.byte_loop:
    cmp rbx, PMM_BITMAP_BYTES
    jae .fail
    cmp byte [rdi+rbx], 0xFF
    je .next_byte

    xor edx, edx                        ; edx = bit index 0-7
.bit_loop:
    mov al, 1
    mov cl, dl
    shl al, cl
    test byte [rdi+rbx], al
    jz .found
    inc edx
    cmp edx, 8
    jl .bit_loop
.next_byte:
    inc rbx
    jmp .byte_loop

.found:
    or byte [rdi+rbx], al                ; mark used
    mov rax, rbx
    shl rax, 3
    add rax, rdx                          ; rax = frame index
    shl rax, 12                            ; rax = physical address
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; pmm_free_page: RCX = physical address (must be 4KB-aligned). Clears the
; corresponding bit.
; -------------------------------------------------------------------------
pmm_free_page:
    push rax
    push rbx
    push rcx
    push rdi

    mov rax, rcx
    shr rax, 12
    cmp rax, PMM_MAX_FRAMES
    jae .out

    mov rbx, rax
    shr rbx, 3
    and al, 7
    mov cl, al
    mov al, 1
    shl al, cl
    not al
    lea rdi, [rel pmm_bitmap]
    and [rdi+rbx], al
.out:
    pop rdi
    pop rcx
    pop rbx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
align 4096
pmm_bitmap: times PMM_BITMAP_BYTES db 0
