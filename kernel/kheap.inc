; =============================================================================
; Kernel heap: kmalloc/kfree, split across two paths.
;
; Small allocations (<= KHEAP_MAX_ALLOC, one page's worth) use a classic
; free-list scheme: growth requests one fresh physical page directly
; (identity-mapped, so no VMM involvement needed) and adds it as a single
; free block. kmalloc splits a block that's bigger than needed (leaving
; the leftover tail as a new, smaller free block) rather than handing out
; the whole thing, and kfree coalesces a freed block with any
; address-adjacent free neighbor(s) it finds in the list -- otherwise a
; page carved into many small allocations could never be reassembled into
; anything bigger than its smallest piece, even after everything on it is
; freed.
;
; Larger allocations go through kmalloc_large: several physical pages,
; likely non-contiguous, get mapped into one contiguous virtual range in
; the VMM heap arena (vmm.inc, VMM_VIRT_BASE and up). Since that arena
; starts at 1TB -- far above anything pmm_alloc_page can ever return --
; kfree tells the two paths apart just by comparing the pointer against
; VMM_VIRT_BASE, no explicit tag needed.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

KHEAP_BLOCK_HEADER equ 16
KHEAP_MAX_ALLOC     equ 4096 - KHEAP_BLOCK_HEADER

KHEAP_LARGE_MAX_ENTRIES equ 64              ; concurrent large allocations

; -------------------------------------------------------------------------
; kmalloc: RCX = requested size in bytes. Returns RAX = pointer to usable
; memory, or 0 on failure. Transparently dispatches to kmalloc_large for
; anything bigger than one page.
; -------------------------------------------------------------------------
kmalloc:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    add rcx, 15
    and rcx, ~15                        ; round up to 16-byte alignment
    mov rbx, rcx                        ; rbx = aligned requested size
    cmp rbx, KHEAP_MAX_ALLOC
    ja .large

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
    ; rdi = candidate block, rdx = its usable size, rbx = size actually
    ; needed. If there's enough left over to form a worthwhile block of
    ; its own (header + at least 16 bytes usable), split: shrink this
    ; block to exactly rbx and splice a new free block, covering the
    ; leftover, into the list in its place. Otherwise just hand out the
    ; whole block as-is (the leftover would be too small to ever satisfy
    ; another request anyway).
    mov rax, rdx
    sub rax, rbx                        ; rax = leftover bytes
    cmp rax, KHEAP_BLOCK_HEADER + 16
    jb .no_split

    mov [rdi+0], rbx                    ; this block now covers just rbx
    lea r8, [rdi+rbx+16]                ; r8 = new block's header address
    mov r9, rax
    sub r9, KHEAP_BLOCK_HEADER          ; r9 = new block's usable size
    mov [r8+0], r9
    mov rax, [rdi+8]                    ; old next
    mov [r8+8], rax
    mov [rsi], r8                       ; replace this block with the new one
    jmp .use_block_out

.no_split:
    mov rax, [rdi+8]                    ; unlink: *rsi = block->next
    mov [rsi], rax
.use_block_out:
    lea rax, [rdi+16]                   ; usable pointer, past the header
    jmp .out

.large:
    ; Unwind our own frame and tail-call kmalloc_large: it has its own
    ; prologue/epilogue, so leaving ours on the stack here would corrupt
    ; the return address once it hits its own RET.
    mov rax, rbx                        ; rax survives the pops below
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    mov rcx, rax
    jmp kmalloc_large

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
; kmalloc_large: RCX = size in bytes (> KHEAP_MAX_ALLOC). Allocates enough
; physical pages (possibly non-contiguous) and maps them into one
; contiguous range of the VMM heap arena. Returns RAX = pointer, or 0.
; -------------------------------------------------------------------------
kmalloc_large:
    push rbx
    push rsi
    push r12
    push r13
    push r14

    add rcx, 4095
    shr rcx, 12
    mov rbx, rcx                        ; rbx = page count needed

    lea rsi, [rel kheap_large_table]
    xor r14d, r14d                      ; r14 = slot index
.find_slot:
    cmp r14d, KHEAP_LARGE_MAX_ENTRIES
    jae .fail
    mov rax, [rsi]
    test rax, rax
    jz .got_slot
    add rsi, 16
    inc r14d
    jmp .find_slot

.got_slot:
    mov r13, [rel kheap_next_virt]      ; r13 = base virtual address
    mov rax, rbx
    shl rax, 12
    add rax, r13
    mov [rel kheap_next_virt], rax      ; bump the arena pointer

    xor r12, r12                        ; r12 = pages mapped so far
.map_loop:
    cmp r12, rbx
    jae .map_done
    call pmm_alloc_page
    test rax, rax
    jz .fail                            ; leaks pages already mapped; see note below
    mov rdx, rax                        ; rdx = physical page
    mov rcx, r12
    shl rcx, 12
    add rcx, r13                        ; rcx = virtual address for this page
    call vmm_map_page
    test eax, eax
    jz .fail
    inc r12
    jmp .map_loop

.map_done:
    mov [rsi+0], r13
    mov [rsi+8], rbx
    mov rax, r13
    jmp .out
.fail:
    ; NOTE: on a failed grow, any pages already mapped for this attempt
    ; are leaked (not returned to the PMM) -- acceptable for a phase that
    ; doesn't yet expect to run out of memory mid-allocation; a proper
    ; unwind is a follow-up once that becomes a real scenario.
    xor eax, eax
.out:
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rbx
    ret

; -------------------------------------------------------------------------
; kfree: RCX = pointer previously returned by kmalloc/kmalloc_large. The
; small-block path coalesces the freed block with any free neighbor(s)
; immediately before or after it in memory, so pages carved into many
; small allocations can still reassemble into larger blocks once
; everything on them is freed again.
; -------------------------------------------------------------------------
kfree:
    push rax
    mov rax, VMM_VIRT_BASE
    cmp rcx, rax
    pop rax
    jae kfree_large

    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    lea r8, [rcx-16]                    ; r8 = freed block's header
    mov r9, [r8+0]                      ; r9 = its usable size

    lea rsi, [rel kheap_free_list]      ; rsi = &(prev's next field, or head)
.scan:
    mov rdi, [rsi]
    test rdi, rdi
    jz .done_scan
    mov rax, [rdi+0]                    ; candidate's usable size
    lea rbx, [rdi+rax+16]               ; candidate's end address
    cmp rbx, r8
    je .merge_pred                      ; candidate ends exactly where we start
    lea rbx, [r8+r9+16]                 ; our (possibly already-merged) end
    cmp rbx, rdi
    je .merge_succ                      ; candidate starts exactly where we end
    lea rsi, [rdi+8]
    jmp .scan

.merge_pred:
    mov rax, [rdi+8]                    ; unlink candidate
    mov [rsi], rax
    add r9, 16
    add r9, [rdi+0]                     ; candidate absorbs us: grow its size
    mov r8, rdi                         ; our block is now the candidate's
    lea rsi, [rel kheap_free_list]      ; restart -- the new extent may merge again
    jmp .scan

.merge_succ:
    mov rax, [rdi+8]                    ; unlink candidate
    mov [rsi], rax
    add r9, 16
    add r9, [rdi+0]                     ; we absorb candidate: grow our size
    lea rsi, [rel kheap_free_list]      ; restart -- the new extent may merge again
    jmp .scan

.done_scan:
    mov [r8+0], r9
    mov rax, [rel kheap_free_list]
    mov [r8+8], rax
    mov [rel kheap_free_list], r8

    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; kfree_large: RCX = pointer previously returned by kmalloc_large. Frees
; and unmaps every page in the matching tracking-table entry.
; -------------------------------------------------------------------------
kfree_large:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push r9
    push r12
    push r13

    mov r13, rcx                        ; r13 = virt_base to find

    lea rsi, [rel kheap_large_table]
    xor ebx, ebx
.find:
    cmp ebx, KHEAP_LARGE_MAX_ENTRIES
    jae .out                            ; not found: nothing to do
    mov rax, [rsi]
    cmp rax, r13
    je .found
    add rsi, 16
    inc ebx
    jmp .find

.found:
    mov r9, [rsi+8]                     ; r9 = page count
    xor r12, r12
.free_loop:
    cmp r12, r9
    jae .clear_slot
    mov rax, r12
    shl rax, 12
    add rax, r13                        ; rax = virt addr of this page

    mov rcx, rax
    call vmm_translate
    test rax, rax
    jz .skip_free                       ; defensive: shouldn't happen
    mov rcx, rax
    call pmm_free_page
.skip_free:
    mov rcx, r12
    shl rcx, 12
    add rcx, r13
    call vmm_unmap_page
    inc r12
    jmp .free_loop

.clear_slot:
    mov qword [rsi+0], 0
    mov qword [rsi+8], 0
.out:
    pop r13
    pop r12
    pop r9
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
kheap_free_list: dq 0
kheap_next_virt: dq VMM_VIRT_BASE

align 8
kheap_large_table: times (KHEAP_LARGE_MAX_ENTRIES * 16) db 0
