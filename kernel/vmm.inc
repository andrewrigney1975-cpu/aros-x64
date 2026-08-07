; =============================================================================
; Virtual memory manager: real page-table mapping (4KB granularity) on top
; of the identity-map PML4 paging.inc already built and loaded into CR3.
;
; The identity map's blanket 1GB pages cover PML4 entries 0 and 1 (0-1TB)
; for driver/MMIO access. This file adds mappings under PML4 entry 2
; (1TB-1.5TB, VMM_VIRT_BASE) by walking/building real PDPT->PD->PT chains,
; allocating page-table pages via pmm_alloc_page as needed -- that PML4
; slot is otherwise completely unused, so there's no collision. Because
; the intermediate page-table pages themselves come from the identity-
; mapped physical range, walking the tables never needs a separate
; physical<->virtual translation step: a table's physical address IS a
; valid pointer to it.
;
; This is what makes multi-page kmalloc possible: several physical frames
; that pmm_alloc_page handed out non-contiguously can be mapped into one
; contiguous virtual range here.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

VMM_VIRT_BASE equ 0x10000000000              ; 1TB (PML4 index 2)

; -------------------------------------------------------------------------
; vmm_next_level: RDI = current table's base address, RAX = index (0-511).
; Ensures table[index] is present, allocating and zeroing a fresh page-
; table page via pmm_alloc_page if not. Returns RAX = the next-level
; table's physical address, or 0 on allocation failure.
; -------------------------------------------------------------------------
vmm_next_level:
    push rbx
    push rcx
    push rdi

    mov rcx, rax
    shl rcx, 3
    add rdi, rcx                        ; rdi = &table[index]
    mov rax, [rdi]
    test rax, 1                         ; present?
    jnz .present

    push rdi
    call pmm_alloc_page
    pop rdi
    test rax, rax
    jz .out                             ; failure: rax already 0
    mov rbx, rax                        ; rbx = new table's physical addr

    push rdi
    mov rdi, rbx
    xor eax, eax
    mov ecx, 512
    cld
    rep stosq
    pop rdi

    mov rax, rbx
    or rax, 0x03                        ; present | writable
    mov [rdi], rax
    mov rax, rbx
    jmp .out

.present:
    and rax, 0xFFFFFFFFFFFFF000
.out:
    pop rdi
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; vmm_find_level: RDI = current table's base address, RAX = index. Same as
; vmm_next_level but never allocates -- returns RAX = 0 if not present.
; -------------------------------------------------------------------------
vmm_find_level:
    push rcx
    push rdi

    mov rcx, rax
    shl rcx, 3
    add rdi, rcx
    mov rax, [rdi]
    test rax, 1
    jz .absent
    and rax, 0xFFFFFFFFFFFFF000
    jmp .out
.absent:
    xor eax, eax
.out:
    pop rdi
    pop rcx
    ret

; -------------------------------------------------------------------------
; vmm_map_page: RCX = virtual address, RDX = physical address (both should
; be 4KB-aligned). Builds any missing PDPT/PD/PT levels and installs a
; present|writable leaf entry. Returns EAX=1/0.
; -------------------------------------------------------------------------
vmm_map_page:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

    mov rsi, rcx                        ; rsi = virtual address
    mov rbx, rdx                        ; rbx = physical address

    mov rax, rsi
    shr rax, 39
    and eax, 0x1FF
    lea rdi, [rel pml4]
    call vmm_next_level
    test rax, rax
    jz .fail
    mov rdi, rax

    mov rax, rsi
    shr rax, 30
    and eax, 0x1FF
    call vmm_next_level
    test rax, rax
    jz .fail
    mov rdi, rax

    mov rax, rsi
    shr rax, 21
    and eax, 0x1FF
    call vmm_next_level
    test rax, rax
    jz .fail
    mov rdi, rax

    mov rax, rsi
    shr rax, 12
    and eax, 0x1FF
    mov rcx, rax
    shl rcx, 3
    add rdi, rcx                        ; rdi = &pt[index]
    mov rax, rbx
    or rax, 0x03                        ; present | writable
    mov [rdi], rax

    invlpg [rsi]

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; vmm_translate: RCX = virtual address. Returns RAX = the physical address
; it's mapped to, or 0 if unmapped.
; -------------------------------------------------------------------------
vmm_translate:
    push rdi
    push rsi
    push rcx

    mov rsi, rcx

    lea rdi, [rel pml4]
    mov rax, rsi
    shr rax, 39
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 30
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 21
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 12
    and eax, 0x1FF
    mov rcx, rax
    shl rcx, 3
    add rdi, rcx
    mov rax, [rdi]
    test rax, 1
    jz .not_present
    and rax, 0xFFFFFFFFFFFFF000
    jmp .out
.not_present:
    xor eax, eax
.out:
    pop rcx
    pop rsi
    pop rdi
    ret

; -------------------------------------------------------------------------
; vmm_unmap_page: RCX = virtual address. Clears its PT entry if the whole
; walk exists; a no-op if any level is missing. Intermediate page-table
; pages are never freed (acceptable for now -- they're small and the
; heap arena doesn't yet shrink).
; -------------------------------------------------------------------------
vmm_unmap_page:
    push rax
    push rcx
    push rdi
    push rsi

    mov rsi, rcx

    lea rdi, [rel pml4]
    mov rax, rsi
    shr rax, 39
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 30
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 21
    and eax, 0x1FF
    call vmm_find_level
    test rax, rax
    jz .out
    mov rdi, rax

    mov rax, rsi
    shr rax, 12
    and eax, 0x1FF
    mov rcx, rax
    shl rcx, 3
    add rdi, rcx
    mov qword [rdi], 0
    invlpg [rsi]

.out:
    pop rsi
    pop rdi
    pop rcx
    pop rax
    ret
