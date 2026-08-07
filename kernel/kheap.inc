; =============================================================================
; Basic kernel heap: a free-list allocator on top of pmm_alloc_page.
;
; Scope: each allocation must fit within one 4KB page (max 4096-16 bytes,
; enforced below) -- growth always requests exactly one fresh page from the
; physical allocator and carves the whole thing into a single free block.
; Blocks are never split or coalesced yet, so a kmalloc/kfree churn
; pattern can fragment the heap; both are natural follow-ups once
; something actually needs them. Good enough for boot-time buffers
; (exFAT cluster reads, etc.) that don't need to outlive a single use.
;
; Block layout: [size:8][next:8] header immediately followed by `size`
; usable bytes. kmalloc returns a pointer just past the header.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

KHEAP_BLOCK_HEADER equ 16
KHEAP_MAX_ALLOC     equ 4096 - KHEAP_BLOCK_HEADER

; -------------------------------------------------------------------------
; kmalloc: RCX = requested size in bytes (<= KHEAP_MAX_ALLOC). Returns
; RAX = pointer to usable memory, or 0 on failure.
; -------------------------------------------------------------------------
kmalloc:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

    add rcx, 15
    and rcx, ~15                        ; round up to 16-byte alignment
    mov rbx, rcx                        ; rbx = aligned requested size
    cmp rbx, KHEAP_MAX_ALLOC
    ja .fail

    lea rsi, [rel kheap_free_list]      ; rsi = &(prev's next field, or head)
.search:
    mov rdi, [rsi]                      ; rdi = candidate block
    test rdi, rdi
    jz .grow
    mov rdx, [rdi+0]                    ; block's usable size
    cmp rdx, rbx
    jae .use_block
    lea rsi, [rdi+8]
    jmp .search

.grow:
    call pmm_alloc_page
    test rax, rax
    jz .fail
    mov rdi, rax                        ; new block == the fresh page itself
    mov qword [rdi+0], 4096 - KHEAP_BLOCK_HEADER
    mov rax, [rel kheap_free_list]
    mov [rdi+8], rax
    mov [rel kheap_free_list], rdi
    lea rsi, [rel kheap_free_list]
    jmp .search

.use_block:
    mov rax, [rdi+8]                    ; unlink: *rsi = block->next
    mov [rsi], rax
    lea rax, [rdi+16]                   ; usable pointer, past the header
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
; kfree: RCX = pointer previously returned by kmalloc. Pushes the block
; back onto the free list (no coalescing).
; -------------------------------------------------------------------------
kfree:
    push rax
    push rcx
    push rdx

    lea rax, [rcx-16]                   ; rax = block header
    mov rdx, [rel kheap_free_list]
    mov [rax+8], rdx
    mov [rel kheap_free_list], rax

    pop rdx
    pop rcx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
kheap_free_list: dq 0
