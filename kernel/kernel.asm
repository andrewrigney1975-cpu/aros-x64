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
    call kbd_install
    call paging_init
    call lapic_init
    call basix_fpu_init

    mov rcx, rbx                        ; boot_info
    call pmm_init

    call basix_heap_init

    ; Interrupts stay off until every bulk rep-stos/rep-movs boot-time
    ; clear above (the PMM bitmap fill, the VMM's page-table zeroing, the
    ; heap arena init) has fully run. A timer tick landing mid-REP on a
    ; Hyper-V/WHvp-backed hypervisor (VirtualBox on a Windows host with
    ; Hyper-V active) doesn't resume the interrupted REP cleanly -- seen
    ; as sporadic triple faults at different RIPs (always right after a
    ; rep stosd/stosq) depending on exactly when the tick landed. QEMU
    ; tolerates this fine; delaying STI until after the bulk clears
    ; sidesteps it either way.
    sti

    call fb_clear
    call draw_logo

    mov ecx, 4                          ; column
    mov edx, 29                          ; row
    lea r8, [rel msg_hello]
    call fb_draw_string

    call storage_init_and_test
    call msi_test
    call nvme_init_and_test
    call xhci_probe_and_report

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
    mov edx, 38
    lea r8, [rel msg_exfat_ok]
    call fb_draw_string
    jmp .exfat_done
.exfat_bad:
    lea rcx, [rel msg_exfat_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 38
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
    mov edx, 40
    lea r8, [rel msg_timer_ok]
    call fb_draw_string
    jmp .timer_done
.timer_bad:
    lea rcx, [rel msg_timer_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 40
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
    mov edx, 42
    lea r8, [rel msg_pmm_ok]
    call fb_draw_string
    jmp .pmm_done
.pmm_bad:
    lea rcx, [rel msg_pmm_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 42
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
    lea rcx, [rel msg_dbg_fa]
    call serial_puts
    mov rax, r12
    call dbg_hex64
    call pmm_alloc_page
    test rax, rax
    jz .vmm_bad
    mov r13, rax                        ; r13 = physical frame B
    lea rcx, [rel msg_dbg_fb]
    call serial_puts
    mov rax, r13
    call dbg_hex64

    mov rcx, VMM_VIRT_BASE
    mov rdx, r12
    call vmm_map_page
    mov r14d, eax                       ; stash return code
    lea rcx, [rel msg_dbg_m1]
    call serial_puts
    mov eax, r14d
    call dbg_hex64
    test r14d, r14d
    jz .vmm_bad
    mov rcx, VMM_VIRT_BASE + 4096
    mov rdx, r13
    call vmm_map_page
    mov r14d, eax
    lea rcx, [rel msg_dbg_m2]
    call serial_puts
    mov eax, r14d
    call dbg_hex64
    test r14d, r14d
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
    mov edx, 43
    lea r8, [rel msg_vmm_ok]
    call fb_draw_string
    jmp .vmm_done
.vmm_bad:
    lea rcx, [rel msg_vmm_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 43
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

    ; Coalescing: two freshly carved, address-adjacent blocks, freed in
    ; order (so the second free() finds the first, already-free one as
    ; its predecessor and merges into it), must reassemble into one block
    ; big enough for a request neither alone could satisfy -- and the
    ; result must start exactly where the first block did, proving a
    ; genuine merge rather than two blocks that just happen to both be
    ; free.
    mov ecx, 96
    call kmalloc
    test rax, rax
    jz .kheap_bad
    mov r12, rax                        ; r12 = block C

    mov ecx, 96
    call kmalloc
    test rax, rax
    jz .kheap_bad
    mov r13, rax                        ; r13 = block D (immediately after C)

    mov rcx, r12
    call kfree                          ; free C first
    mov rcx, r13
    call kfree                          ; then D -- should coalesce into C's block

    mov ecx, 150                        ; bigger than either C or D alone
    call kmalloc
    test rax, rax
    jz .kheap_bad
    cmp rax, r12
    jne .kheap_bad                      ; must be the coalesced C+D block

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
    mov edx, 44
    lea r8, [rel msg_kheap_ok]
    call fb_draw_string
    jmp .kheap_done
.kheap_bad:
    lea rcx, [rel msg_kheap_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 44
    lea r8, [rel msg_kheap_bad]
    call fb_draw_string
.kheap_done:

    ; Verify storage_write_sectors against a scratch LBA far beyond
    ; anything the exFAT volume currently uses, so this can't corrupt the
    ; boot sector, partition table, or the TEST.TXT content already there.
    ; (AHCI's write path was separately hand-verified the same way,
    ; against its own smaller vvfat-backed disk -- not kept as a
    ; permanent test since forcing a driver switch mid-boot is more
    ; disruptive than it's worth for routine regression checking.)
    mov rcx, 100000
    mov edx, 1
    lea r8, [rel exfat_test_buf]
    mov byte [rel exfat_test_buf], 0x5A
    mov byte [rel exfat_test_buf+511], 0xA5
    call storage_write_sectors
    test eax, eax
    jz .wtest_bad

    mov rcx, 100000
    mov edx, 1
    lea r8, [rel exfat_test_buf]
    mov byte [rel exfat_test_buf], 0
    mov byte [rel exfat_test_buf+511], 0
    call storage_read_sectors
    test eax, eax
    jz .wtest_bad
    cmp byte [rel exfat_test_buf], 0x5A
    jne .wtest_bad
    cmp byte [rel exfat_test_buf+511], 0xA5
    jne .wtest_bad

    lea rcx, [rel msg_wtest_ok]
    call serial_puts
    jmp .wtest_done
.wtest_bad:
    lea rcx, [rel msg_wtest_bad]
    call serial_puts
.wtest_done:

    ; Verify the allocation bitmap: mount it, allocate a cluster, confirm
    ; it now reads back as allocated, then free it again. This ends with
    ; the on-disk bitmap byte-for-byte unchanged (alloc then free of the
    ; same cluster is a net no-op), so it's safe to run every boot.
    call exfat_bitmap_mount
    test eax, eax
    jz .btest_bad

    call exfat_alloc_cluster
    test eax, eax
    jz .btest_bad
    mov r13d, eax                       ; r13d = allocated cluster (NOT ebx --
                                         ; rbx is boot_info* for the whole
                                         ; kernel; clobbering it here bit us
                                         ; once already, see basix_rt_print_int)

    mov ecx, r13d
    call exfat_bitmap_test_free
    test eax, eax
    jnz .btest_bad                      ; should now read as NOT free

    mov ecx, r13d
    call exfat_free_cluster
    test eax, eax
    jz .btest_bad

    mov ecx, r13d
    call exfat_bitmap_test_free
    test eax, eax
    jz .btest_bad                       ; should be free again

    lea rcx, [rel msg_btest_ok]
    call serial_puts
    jmp .btest_done
.btest_bad:
    lea rcx, [rel msg_btest_bad]
    call serial_puts
.btest_done:
    ; Verify FAT chain writing: allocate two clusters, link A->B, terminate
    ; B with an end-of-chain marker, confirm exfat_fat_next_cluster follows
    ; the link and then the EOC, then clear both FAT entries and free both
    ; clusters again -- a net no-op on the real volume, safe every boot.
    call exfat_alloc_cluster
    test eax, eax
    jz .ctest_bad
    mov r13d, eax                       ; r13d = cluster A (not ebx -- see note above)

    call exfat_alloc_cluster
    test eax, eax
    jz .ctest_bad
    mov esi, eax                        ; esi = cluster B

    mov ecx, r13d
    mov edx, esi
    call exfat_fat_set_entry            ; A -> B
    test eax, eax
    jz .ctest_bad

    mov ecx, esi
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry            ; B -> EOC
    test eax, eax
    jz .ctest_bad

    mov ecx, r13d
    call exfat_fat_next_cluster
    cmp eax, esi
    jne .ctest_bad

    mov ecx, esi
    call exfat_fat_next_cluster
    cmp eax, 0xFFFFFFFF
    jne .ctest_bad

    mov ecx, r13d
    xor edx, edx
    call exfat_fat_set_entry
    test eax, eax
    jz .ctest_bad
    mov ecx, esi
    xor edx, edx
    call exfat_fat_set_entry
    test eax, eax
    jz .ctest_bad

    mov ecx, r13d
    call exfat_free_cluster
    test eax, eax
    jz .ctest_bad
    mov ecx, esi
    call exfat_free_cluster
    test eax, eax
    jz .ctest_bad

    lea rcx, [rel msg_ctest_ok]
    call serial_puts
    jmp .ctest_done
.ctest_bad:
    lea rcx, [rel msg_ctest_bad]
    call serial_puts
.ctest_done:

    ; Verify directory entry construction: create an empty test file in
    ; the root directory. Idempotent across reboots of the same VHD -- if
    ; a prior boot already created it, this just confirms it's still
    ; found rather than creating a duplicate entry.
    lea rcx, [rel exfat_crtest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .crtest_exists

    lea rcx, [rel exfat_crtest_name]
    call exfat_create_file
    test eax, eax
    jz .crtest_bad

    lea rcx, [rel exfat_crtest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .crtest_bad
.crtest_exists:
    cmp qword [rel exfat_find_result+8], 0   ; DataLength must be 0 (empty)
    jne .crtest_bad

    lea rcx, [rel msg_crtest_ok]
    call serial_puts
    jmp .crtest_done
.crtest_bad:
    lea rcx, [rel msg_crtest_bad]
    call serial_puts
.crtest_done:

    ; Verify exfat_write_file: create a new file spanning more than one
    ; cluster (exercises FAT chain writing on the data itself, not just
    ; the bitmap/FAT tests above), then read it back and confirm the
    ; content round-trips. Idempotent across reboots of the same VHD.
    lea rcx, [rel exfat_wtest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .wftest_verify

    lea rdi, [rel exfat_wtest_src]
    xor ecx, ecx
.wftest_fill:
    cmp ecx, 6000
    jge .wftest_fill_done
    mov eax, ecx
    and eax, 0xFF
    mov [rdi+rcx], al
    inc ecx
    jmp .wftest_fill
.wftest_fill_done:

    lea rcx, [rel exfat_wtest_name]
    lea r8, [rel exfat_wtest_src]
    mov r9, 6000
    call exfat_write_file
    test eax, eax
    jz .wftest_bad

    lea rcx, [rel exfat_wtest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .wftest_bad

.wftest_verify:
    cmp qword [rel exfat_find_result+8], 6000
    jne .wftest_bad

    mov ecx, [rel exfat_find_result+0]   ; FirstCluster
    mov rdx, [rel exfat_find_result+8]   ; DataLength
    lea r8, [rel exfat_wtest_read]
    mov r9d, [rel exfat_find_result+16]  ; NoFatChain
    call exfat_read_file
    test eax, eax
    jz .wftest_bad

    lea rsi, [rel exfat_wtest_read]
    xor ecx, ecx
.wftest_cmp:
    cmp ecx, 6000
    jge .wftest_ok
    mov al, [rsi+rcx]
    mov ah, cl
    cmp al, ah
    jne .wftest_bad
    inc ecx
    jmp .wftest_cmp
.wftest_ok:
    lea rcx, [rel msg_wftest_ok]
    call serial_puts
    jmp .wftest_done
.wftest_bad:
    lea rcx, [rel msg_wftest_bad]
    call serial_puts
.wftest_done:

    ; Verify long-filename support (exfat_lntest_name is 40 characters --
    ; well past the old 15-char/one-FileName-entry cap, exercising the
    ; multi-FileName-entry write path). Idempotent across reboots.
    lea rcx, [rel exfat_lntest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .lntest_verify

    lea rcx, [rel exfat_lntest_name]
    lea r8, [rel exfat_lntest_content]
    mov r9, exfat_lntest_content_len
    call exfat_write_file
    test eax, eax
    jz .lntest_bad

    lea rcx, [rel exfat_lntest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .lntest_bad

.lntest_verify:
    cmp qword [rel exfat_find_result+8], exfat_lntest_content_len
    jne .lntest_bad

    mov ecx, [rel exfat_find_result+0]
    mov rdx, [rel exfat_find_result+8]
    lea r8, [rel exfat_lntest_read]
    mov r9d, [rel exfat_find_result+16]
    call exfat_read_file
    test eax, eax
    jz .lntest_bad

    lea rsi, [rel exfat_lntest_read]
    lea rdi, [rel exfat_lntest_content]
    xor ecx, ecx
.lntest_cmp:
    cmp ecx, exfat_lntest_content_len
    jge .lntest_ok
    mov al, [rsi+rcx]
    cmp al, [rdi+rcx]
    jne .lntest_bad
    inc ecx
    jmp .lntest_cmp
.lntest_ok:
    lea rcx, [rel msg_lntest_ok]
    call serial_puts
    jmp .lntest_done
.lntest_bad:
    lea rcx, [rel msg_lntest_bad]
    call serial_puts
.lntest_done:

    ; Verify exfat_delete_file: create a temp file, confirm it exists,
    ; delete it, confirm it's gone. Naturally idempotent across reboots
    ; -- the file never survives a successful run, so "create" always
    ; starts fresh next time.
    lea rcx, [rel exfat_deltest_name]
    call exfat_create_file
    test eax, eax
    jz .deltest_bad

    lea rcx, [rel exfat_deltest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .deltest_bad

    lea rcx, [rel exfat_deltest_name]
    call exfat_delete_file
    test eax, eax
    jz .deltest_bad

    lea rcx, [rel exfat_deltest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .deltest_bad             ; must be gone now

    lea rcx, [rel msg_deltest_ok]
    call serial_puts
    jmp .deltest_done
.deltest_bad:
    lea rcx, [rel msg_deltest_bad]
    call serial_puts
.deltest_done:

    ; Verify exfat_rename_file: write a file under one name, rename it,
    ; confirm the new name has the same content and the old name is
    ; gone. Idempotent: if the destination already exists (a prior boot
    ; already renamed it), skip straight to verification.
    lea rcx, [rel exfat_rentest_dst]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .rentest_verify

    lea rcx, [rel exfat_rentest_src]
    lea r8, [rel exfat_rentest_content]
    mov r9, exfat_rentest_content_len
    call exfat_write_file
    test eax, eax
    jz .rentest_bad

    lea rcx, [rel exfat_rentest_src]
    lea rdx, [rel exfat_rentest_dst]
    call exfat_rename_file
    test eax, eax
    jz .rentest_bad

.rentest_verify:
    lea rcx, [rel exfat_rentest_src]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .rentest_bad             ; old name must be gone

    lea rcx, [rel exfat_rentest_dst]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .rentest_bad
    cmp qword [rel exfat_find_result+8], exfat_rentest_content_len
    jne .rentest_bad

    mov ecx, [rel exfat_find_result+0]
    mov rdx, [rel exfat_find_result+8]
    lea r8, [rel exfat_rentest_read]
    mov r9d, [rel exfat_find_result+16]
    call exfat_read_file
    test eax, eax
    jz .rentest_bad

    lea rsi, [rel exfat_rentest_read]
    lea rdi, [rel exfat_rentest_content]
    xor ecx, ecx
.rentest_cmp:
    cmp ecx, exfat_rentest_content_len
    jge .rentest_ok
    mov al, [rsi+rcx]
    cmp al, [rdi+rcx]
    jne .rentest_bad
    inc ecx
    jmp .rentest_cmp
.rentest_ok:
    lea rcx, [rel msg_rentest_ok]
    call serial_puts
    jmp .rentest_done
.rentest_bad:
    lea rcx, [rel msg_rentest_bad]
    call serial_puts
.rentest_done:

    ; Verify exfat_truncate_file: write a multi-cluster file, truncate
    ; it down to a short prefix, confirm the length and content match.
    ; Idempotent, and tolerant of a prior run that got interrupted
    ; between creating the file and truncating it (e.g. a build that
    ; failed mid-development): the file already having the truncated
    ; (short) length skips straight to verification, and the file
    ; existing at some OTHER length just means "still needs truncating"
    ; rather than "something is wrong" -- only a genuinely fresh name
    ; needs the initial full-size write.
    lea rcx, [rel exfat_trunctest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .trunctest_create
    cmp qword [rel exfat_find_result+8], EXFAT_TRUNCTEST_SHORT_LEN
    je .trunctest_verify
    jmp .trunctest_do_truncate
.trunctest_create:
    lea rdi, [rel exfat_trunctest_src]
    xor ecx, ecx
.trunctest_fill:
    cmp ecx, 5000
    jge .trunctest_fill_done
    mov eax, ecx
    and eax, 0xFF
    mov [rdi+rcx], al
    inc ecx
    jmp .trunctest_fill
.trunctest_fill_done:

    lea rcx, [rel exfat_trunctest_name]
    lea r8, [rel exfat_trunctest_src]
    mov r9, 5000
    call exfat_write_file
    test eax, eax
    jz .trunctest_bad

.trunctest_do_truncate:
    lea rcx, [rel exfat_trunctest_name]
    mov rdx, EXFAT_TRUNCTEST_SHORT_LEN
    call exfat_truncate_file
    test eax, eax
    jz .trunctest_bad

.trunctest_verify:
    lea rcx, [rel exfat_trunctest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .trunctest_bad
    cmp qword [rel exfat_find_result+8], EXFAT_TRUNCTEST_SHORT_LEN
    jne .trunctest_bad

    mov ecx, [rel exfat_find_result+0]
    mov rdx, [rel exfat_find_result+8]
    lea r8, [rel exfat_trunctest_read]
    mov r9d, [rel exfat_find_result+16]
    call exfat_read_file
    test eax, eax
    jz .trunctest_bad

    lea rsi, [rel exfat_trunctest_read]
    xor ecx, ecx
.trunctest_cmp:
    cmp ecx, EXFAT_TRUNCTEST_SHORT_LEN
    jge .trunctest_ok
    mov al, [rsi+rcx]
    mov ah, cl
    cmp al, ah
    jne .trunctest_bad
    inc ecx
    jmp .trunctest_cmp
.trunctest_ok:
    lea rcx, [rel msg_trunctest_ok]
    call serial_puts
    jmp .trunctest_done
.trunctest_bad:
    lea rcx, [rel msg_trunctest_bad]
    call serial_puts
.trunctest_done:

    ; Verify exfat_append_file: write a file, append more data spanning
    ; new clusters, confirm the combined content round-trips. Idempotent,
    ; and tolerant of a prior run that got interrupted between the
    ; initial write and the append (a name that exists but isn't yet at
    ; the full post-append length just means "still needs appending to",
    ; not "something is wrong").
    lea rcx, [rel exfat_apptest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .apptest_create
    cmp qword [rel exfat_find_result+8], EXFAT_APPTEST_TOTAL_LEN
    je .apptest_verify
    jmp .apptest_fill2
.apptest_create:
    lea rdi, [rel exfat_apptest_part1]
    xor ecx, ecx
.apptest_fill1:
    cmp ecx, EXFAT_APPTEST_PART1_LEN
    jge .apptest_fill1_done
    mov eax, ecx
    and eax, 0xFF
    mov [rdi+rcx], al
    inc ecx
    jmp .apptest_fill1
.apptest_fill1_done:

    lea rcx, [rel exfat_apptest_name]
    lea r8, [rel exfat_apptest_part1]
    mov r9, EXFAT_APPTEST_PART1_LEN
    call exfat_write_file
    test eax, eax
    jz .apptest_bad

.apptest_fill2:
    lea rdi, [rel exfat_apptest_part2]
    xor ecx, ecx
.apptest_fill2_loop:
    cmp ecx, EXFAT_APPTEST_PART2_LEN
    jge .apptest_fill2_done
    mov eax, ecx
    add eax, 77
    and eax, 0xFF
    mov [rdi+rcx], al
    inc ecx
    jmp .apptest_fill2_loop
.apptest_fill2_done:

    lea rcx, [rel exfat_apptest_name]
    lea r8, [rel exfat_apptest_part2]
    mov r9, EXFAT_APPTEST_PART2_LEN
    call exfat_append_file
    test eax, eax
    jz .apptest_bad

.apptest_verify:
    lea rcx, [rel exfat_apptest_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .apptest_bad
    cmp qword [rel exfat_find_result+8], EXFAT_APPTEST_TOTAL_LEN
    jne .apptest_bad

    mov ecx, [rel exfat_find_result+0]
    mov rdx, [rel exfat_find_result+8]
    lea r8, [rel exfat_apptest_read]
    mov r9d, [rel exfat_find_result+16]
    call exfat_read_file
    test eax, eax
    jz .apptest_bad

    lea rsi, [rel exfat_apptest_read]
    xor ecx, ecx
.apptest_cmp:
    cmp ecx, EXFAT_APPTEST_TOTAL_LEN
    jge .apptest_ok
    cmp ecx, EXFAT_APPTEST_PART1_LEN
    jl .apptest_cmp_part1
    mov eax, ecx
    sub eax, EXFAT_APPTEST_PART1_LEN
    add eax, 77
    and eax, 0xFF
    jmp .apptest_cmp_have_expected
.apptest_cmp_part1:
    mov eax, ecx
    and eax, 0xFF
.apptest_cmp_have_expected:
    mov ah, al
    mov al, [rsi+rcx]
    cmp al, ah
    jne .apptest_bad
    inc ecx
    jmp .apptest_cmp
.apptest_ok:
    lea rcx, [rel msg_apptest_ok]
    call serial_puts
    jmp .apptest_done
.apptest_bad:
    lea rcx, [rel msg_apptest_bad]
    call serial_puts
.apptest_done:

    ; Bring up the scheduler: the current flow becomes the "main" task
    ; (its TCB.rsp gets filled in on its own first save -- it doesn't need
    ; a hand-built frame like task_create produces, since it's already
    ; running), plus two test tasks that loop forever printing a tag and
    ; incrementing a counter. Both counters advancing, and their serial
    ; output interleaving, is the proof the timer really is preempting
    ; and round-robining between them -- not just running one to
    ; completion before the other starts.
    mov rcx, TCB_SIZE
    call kmalloc
    test rax, rax
    jz .sched_bad
    mov r15, rax                        ; r15 = main task's TCB
    mov qword [r15+TCB_RSP], 0
    mov qword [r15+TCB_CR3], 0          ; no private paging -- runs under
                                         ; the shared global pml4
    mov qword [r15+TCB_STACK_PAGES], 0
    mov dword [r15+TCB_STATE], TCB_STATE_ALIVE

    mov rcx, 512
    call kmalloc                        ; kmalloc never touches r15
    test rax, rax
    jz .sched_bad
    fxsave [rax]
    mov [r15+TCB_FPU], rax

    mov rcx, test_task_a
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r13, rax                        ; r13 = task A's TCB

    mov rcx, test_task_b
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r14, rax                        ; r14 = task B's TCB

    mov rcx, test_task_exit
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r10, rax                        ; r10 = exit-demo task's TCB

    mov rcx, test_task_crash
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r11, rax                        ; r11 = crash-demo task's TCB

    mov rcx, test_task_overflow
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r9, rax                         ; r9 = overflow-demo task's TCB

    mov [r15+TCB_NEXT], r13             ; ring: main -> A -> B -> exit -> crash -> overflow -> main
    mov [r13+TCB_NEXT], r14
    mov [r14+TCB_NEXT], r10
    mov [r10+TCB_NEXT], r11
    mov [r11+TCB_NEXT], r9
    mov [r9+TCB_NEXT], r15

    mov [rel current_task], r15         ; arms the scheduler (irq0_stub
                                         ; starts switching on the next tick)

    lea rcx, [rel msg_sched_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 46
    lea r8, [rel msg_sched_ok]
    call fb_draw_string
    jmp .sched_done
.sched_bad:
    lea rcx, [rel msg_sched_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 46
    lea r8, [rel msg_sched_bad]
    call fb_draw_string
.sched_done:

    ; Let the round-robin run for a while (the main task keeps its slice
    ; too, so this delay itself gets preempted many times), then check
    ; both test tasks actually made progress.
    mov rcx, 300000000
.sched_wait:
    dec rcx
    jnz .sched_wait

    mov rax, [rel test_task_a_counter]
    test rax, rax
    jz .sched_verify_bad
    mov rax, [rel test_task_b_counter]
    test rax, rax
    jz .sched_verify_bad
    lea rcx, [rel msg_sched_verify_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 47
    lea r8, [rel msg_sched_verify_ok]
    call fb_draw_string
    jmp .sched_verify_done
.sched_verify_bad:
    lea rcx, [rel msg_sched_verify_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 47
    lea r8, [rel msg_sched_verify_bad]
    call fb_draw_string
.sched_verify_done:

    ; Verify process termination: by now test_task_exit, test_task_crash,
    ; and test_task_overflow should each have hit their counter bound (3)
    ; and ended themselves -- voluntarily, via a deliberate #DE exception,
    ; and via a deliberate stack overflow caught by the canary guard,
    ; respectively. Capture their counters, wait again, and confirm: (a)
    ; they stopped exactly at 3 (never got a 4th increment, proving they
    ; really terminated rather than just running slowly), and (b) A/B
    ; kept advancing throughout (proving none of the termination paths
    ; took the rest of the system down with it).
    mov rax, [rel test_task_exit_counter]
    cmp rax, 3
    jne .term_verify_bad
    mov rax, [rel test_task_crash_counter]
    cmp rax, 3
    jne .term_verify_bad
    mov rax, [rel test_task_overflow_counter]
    cmp rax, 3
    jne .term_verify_bad

    mov r12, [rel test_task_a_counter]  ; r12 = A's counter, pre-second-wait
    mov r13, [rel test_task_b_counter]  ; r13 = B's counter, pre-second-wait

    mov rcx, 300000000
.term_wait:
    dec rcx
    jnz .term_wait

    cmp qword [rel test_task_exit_counter], 3
    jne .term_verify_bad                ; must not have incremented again
    cmp qword [rel test_task_crash_counter], 3
    jne .term_verify_bad
    cmp qword [rel test_task_overflow_counter], 3
    jne .term_verify_bad

    mov rax, [rel test_task_a_counter]
    cmp rax, r12
    jle .term_verify_bad                ; A must have kept advancing
    mov rax, [rel test_task_b_counter]
    cmp rax, r13
    jle .term_verify_bad                ; B must have kept advancing

    ; Both terminated tasks should now be sitting on the zombie list,
    ; discovered whenever something walked past them in the ring.
    ; Reaping runs from ordinary context (never from the ISR/exception
    ; handler, which only queue) -- confirm it actually frees them.
    call sched_reap_zombies
    cmp qword [rel sched_zombie_list], 0
    jne .term_verify_bad

    lea rcx, [rel msg_term_verify_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 48
    lea r8, [rel msg_term_verify_ok]
    call fb_draw_string
    jmp .term_verify_done
.term_verify_bad:
    lea rcx, [rel msg_term_verify_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 48
    lea r8, [rel msg_term_verify_bad]
    call fb_draw_string
.term_verify_done:

    ; Smoke-test the BASIX64 compiler: compile and run a trivial program.
    ; This only proves the pipeline works end to end (compiles without
    ; error and executes without crashing) -- RUN in the shell is the
    ; real way to see a program's output.
    lea rcx, [rel basixtest_src]
    call basix_compile
    test eax, eax
    jz .basixtest_bad
    call basix_code_buf
    lea rcx, [rel msg_basixtest_ran]
    call serial_puts
    jmp .basixtest_done
.basixtest_bad:
    lea rcx, [rel msg_basixtest_bad]
    call serial_puts
.basixtest_done:

    call shell_main                     ; never returns

.hang:
    hlt
    jmp .hang

; -------------------------------------------------------------------------
; test_task_a / test_task_b: preemptive-scheduling demo tasks. Each loops
; forever: bump its own counter, print a tag, spin for a bit. Neither
; ever yields voluntarily -- proving preemption, not cooperation, is what
; interleaves them.
; -------------------------------------------------------------------------
test_task_a:
.loop:
    inc qword [rel test_task_a_counter]
    lea rcx, [rel msg_task_a]
    call serial_puts
    mov rcx, 5000000
.delay:
    dec rcx
    jnz .delay
    jmp .loop

test_task_b:
.loop:
    inc qword [rel test_task_b_counter]
    lea rcx, [rel msg_task_b]
    call serial_puts
    mov rcx, 5000000
.delay:
    dec rcx
    jnz .delay
    jmp .loop

; -------------------------------------------------------------------------
; test_task_exit / test_task_crash: process-termination demo/regression
; tasks. Each bumps its counter a few times like A/B, then deliberately
; ends itself -- one voluntarily (task_exit), one via an unhandled CPU
; exception (integer divide by zero, vector 0). Neither counter should
; ever exceed 3; if the kernel is still alive and A/B are still counting
; afterward, both termination paths worked without taking the system
; down with them.
; -------------------------------------------------------------------------
test_task_exit:
.loop:
    inc qword [rel test_task_exit_counter]
    mov rcx, 5000000
.delay:
    dec rcx
    jnz .delay
    cmp qword [rel test_task_exit_counter], 3
    jl .loop
    call task_exit                      ; never returns

test_task_crash:
.loop:
    inc qword [rel test_task_crash_counter]
    mov rcx, 5000000
.delay:
    dec rcx
    jnz .delay
    cmp qword [rel test_task_crash_counter], 3
    jl .loop
    xor edx, edx                        ; deliberate #DE (divide by zero)
    mov eax, 1
    xor ecx, ecx
    div ecx                             ; the exception handler terminates
    jmp .loop                           ; us here; never actually reached

; -------------------------------------------------------------------------
; test_task_overflow: canary-guard demo/regression task. Bumps its counter
; like the others, then deliberately blows its own stack -- exactly 2048
; qword pushes (16384 bytes) against a 16384-byte (2048-qword)
; allocation, with no matching pops, landing the last push's write
; exactly on the canary at the stack's lowest address.
;
; The push count is exact, not "comfortably past the canary": each
; task's private stack (see sched.inc's TASK_STACK_VIRT_BASE) is now
; individually page-mapped with unmapped guard space immediately below
; it, not a single kmalloc'd block sitting inside the middle of a larger
; heap page. Overflowing further than the canary itself would walk off
; the mapped pages entirely and take a hardware #PF instead of being
; caught softly by the software canary check this test exists to prove
; -- a real, if narrower, hardening property of the new design, just
; not what THIS demo is testing.
;
; RSP is restored to a safe position (the stack's top) right after --
; the canary byte itself stays corrupted (nothing un-writes it, only
; the pointer moves), but leaving RSP sitting at the very bottom would
; make the NEXT timer interrupt's own GPR-save push the thing that
; walks off the mapped pages, before the scheduler ever gets a chance
; to notice and terminate this task via the software check.
; -------------------------------------------------------------------------
test_task_overflow:
.loop:
    inc qword [rel test_task_overflow_counter]
    mov rcx, 5000000
.delay:
    dec rcx
    jnz .delay
    cmp qword [rel test_task_overflow_counter], 3
    jl .loop
    mov rcx, 2048
.blow:
    push rax
    dec rcx
    jnz .blow
    add rsp, 2048*8                     ; restore a safe RSP -- the
                                         ; canary byte is still clobbered
.hang:
    hlt
    jmp .hang                           ; wait to be switched away from --
                                         ; the canary check ends this task
                                         ; for good on the next context switch

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
    mov [rel ahci_pci_addr], esi        ; kept for later MSI setup

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
    mov edx, 31
    lea r8, [rel msg_ahci_ok]
    call fb_draw_string

    mov ax, [rel ahci_databuf+510]
    cmp ax, 0xAA55
    jne .bad_sig
    lea rcx, [rel msg_sig_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 32
    lea r8, [rel msg_sig_ok]
    call fb_draw_string
    jmp .done

.bad_sig:
    lea rcx, [rel msg_sig_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 32
    lea r8, [rel msg_sig_bad]
    call fb_draw_string
    jmp .done

.read_failed:
    lea rcx, [rel msg_ahci_read_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 31
    lea r8, [rel msg_ahci_read_fail]
    call fb_draw_string
    jmp .done

.no_port:
    lea rcx, [rel msg_ahci_no_port]
    call serial_puts
    mov ecx, 4
    mov edx, 31
    lea r8, [rel msg_ahci_no_port]
    call fb_draw_string
    jmp .done

.no_controller:
    lea rcx, [rel msg_ahci_not_found]
    call serial_puts
    mov ecx, 4
    mov edx, 31
    lea r8, [rel msg_ahci_not_found]
    call fb_draw_string

.done:
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; msi_test: proof-of-concept for MSI interrupt delivery -- arms MSI on
; the AHCI controller storage_init_and_test already found, enables its
; port-level completion interrupt, issues a benign re-read of LBA 0 (the
; same one storage_init_and_test already proved works via polling), and
; confirms an interrupt actually arrived at msi_test_isr. Skipped
; entirely if no AHCI controller was found. Global AHCI interrupt enable
; is turned back off afterward regardless of outcome -- every other AHCI
; access in this kernel is polling-based and was never meant to field
; interrupts.
; -------------------------------------------------------------------------
msi_test:
    push rax
    push rbx
    push rcx
    push rdx
    push r8

    cmp dword [rel storage_active_driver], STORAGE_AHCI
    jne .skip

    mov rcx, MSI_TEST_VECTOR
    lea rdx, [rel msi_test_isr]
    call idt_set_gate

    mov rbx, [rel ahci_abar]

    ; Clear any interrupt status already pending from earlier polling-
    ; based activity before arming anything.
    mov eax, 0xFFFFFFFF
    mov [rbx+0x08], eax                 ; IS (global)
    mov eax, [rel ahci_port]
    shl eax, 7
    add eax, 0x100                      ; eax = this port's register base
    mov r8d, eax
    mov eax, 0xFFFFFFFF
    mov [rbx+r8+0x10], eax              ; PxIS

    mov eax, 0x00000001                 ; DHRS: Device-to-Host Register
    mov [rbx+r8+0x14], eax              ; FIS Interrupt -- fires on
                                         ; ordinary (non-NCQ) command
                                         ; completion, which is all this
                                         ; driver ever issues

    mov eax, [rbx+0x04]                 ; GHC
    or eax, 0x00000002                  ; IE (Interrupt Enable)
    mov [rbx+0x04], eax

    mov ecx, [rel ahci_pci_addr]
    mov dl, MSI_TEST_VECTOR
    call pci_enable_msi
    test eax, eax
    jz .no_msi_cap

    mov qword [rel msi_test_counter], 0

    xor ecx, ecx                        ; LBA 0 again -- same read
    mov edx, 1                          ; storage_init_and_test already
    lea r8, [rel ahci_databuf]          ; proved works via polling
    call ahci_read_sectors

    mov rcx, 30000000
.wait:
    cmp qword [rel msi_test_counter], 0
    jne .teardown
    dec rcx
    jnz .wait

.teardown:
    ; Recompute rather than trust r8/rbx survived the calls above --
    ; cheap, and avoids relying on preservation this file doesn't
    ; document for every callee.
    mov rbx, [rel ahci_abar]
    mov eax, [rbx+0x04]
    and eax, ~0x00000002
    mov [rbx+0x04], eax
    mov eax, [rel ahci_port]
    shl eax, 7
    add eax, 0x100
    mov r8d, eax
    mov dword [rbx+r8+0x14], 0

    cmp qword [rel msi_test_counter], 0
    jz .fail

    lea rcx, [rel msg_msi_ok]
    call serial_puts
    jmp .done2
.no_msi_cap:
    lea rcx, [rel msg_msi_no_cap]
    call serial_puts
    jmp .done2
.fail:
    lea rcx, [rel msg_msi_bad]
    call serial_puts
    jmp .done2
.skip:
    lea rcx, [rel msg_msi_skip]
    call serial_puts
.done2:
    pop r8
    pop rdx
    pop rcx
    pop rbx
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
    mov edx, 34
    lea r8, [rel msg_nvme_ok]
    call fb_draw_string

    mov ax, [rel nvme_databuf+510]
    cmp ax, 0xAA55
    jne .bad_sig
    lea rcx, [rel msg_sig_ok]
    call serial_puts
    mov ecx, 4
    mov edx, 35
    lea r8, [rel msg_sig_ok]
    call fb_draw_string
    jmp .done

.bad_sig:
    lea rcx, [rel msg_sig_bad]
    call serial_puts
    mov ecx, 4
    mov edx, 35
    lea r8, [rel msg_sig_bad]
    call fb_draw_string
    jmp .done

.read_failed:
    lea rcx, [rel msg_nvme_read_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 34
    lea r8, [rel msg_nvme_read_fail]
    call fb_draw_string
    jmp .done

.init_failed:
    lea rcx, [rel msg_nvme_init_fail]
    call serial_puts
    mov ecx, 4
    mov edx, 34
    lea r8, [rel msg_nvme_init_fail]
    call fb_draw_string
    jmp .done

.no_controller:
    lea rcx, [rel msg_nvme_not_found]
    call serial_puts
    mov ecx, 4
    mov edx, 34
    lea r8, [rel msg_nvme_not_found]
    call fb_draw_string

.done:
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; xhci_probe_and_report: calls xhci_find_and_probe and prints what it
; found (or that nothing was found) -- read-only detection/reporting
; step, no controller reset or ring bring-up yet.
; -------------------------------------------------------------------------
xhci_probe_and_report:
    push rax

    call xhci_find_and_probe
    test eax, eax
    jz .not_found

    lea rcx, [rel msg_xhci_ok]
    call serial_puts
    movzx eax, byte [rel xhci_caplength]
    call dbg_hex64
    movzx eax, word [rel xhci_hciversion]
    call dbg_hex64
    mov eax, [rel xhci_hcsparams1]
    call dbg_hex64

    call xhci_reset_and_start
    test eax, eax
    jz .reset_failed

    lea rcx, [rel msg_xhci_running]
    call serial_puts

    call xhci_noop_test
    test eax, eax
    jz .noop_failed
    lea rcx, [rel msg_xhci_noop_ok]
    call serial_puts

    call xhci_scan_ports
    push rax                            ; connected-port count -- serial_puts
                                         ; below doesn't preserve eax
    lea rcx, [rel msg_xhci_ports]
    call serial_puts
    pop rax
    call dbg_hex64

    test eax, eax
    jz .out

    call xhci_enable_slot
    test eax, eax
    jz .enable_slot_failed
    push rax                            ; slot ID
    lea rcx, [rel msg_xhci_slot_ok]
    call serial_puts
    pop rax
    push rax
    call dbg_hex64
    pop rax

    ; Find the first connected port's index (xhci_scan_ports already
    ; recorded which ones -- this is the port the just-enabled slot
    ; will be addressed against).
    mov ecx, eax                        ; ecx = slot ID
    xor edx, edx                        ; edx = port index (0-based)
.find_port:
    cmp edx, [rel xhci_max_ports]
    jge .out
    lea rax, [rel xhci_port_connected]
    cmp byte [rax+rdx], 0
    jne .port_found
    inc edx
    jmp .find_port
.port_found:
    push rcx                            ; slot ID -- xhci_setup_device_slot
                                         ; preserves ecx itself, but the
                                         ; Address Device call below needs
                                         ; it back in ecx after RDI is set
    call xhci_setup_device_slot
    test eax, eax
    jz .setup_slot_failed
    lea rcx, [rel msg_xhci_setup_ok]
    call serial_puts

    pop rcx                             ; slot ID
    push rcx                            ; ...and re-save it -- serial_puts
                                         ; clobbers ecx, and it's needed
                                         ; again for xhci_get_device_descriptor
    lea rdi, [rel xhci_input_ctx]
    call xhci_address_device
    test eax, eax
    jnz .address_device_ok
    add rsp, 8                          ; drop the re-pushed slot ID
    jmp .address_device_failed
.address_device_ok:
    lea rcx, [rel msg_xhci_addr_ok]
    call serial_puts

    pop rcx                             ; slot ID
    push rcx                            ; ...and re-save it, needed again
                                         ; below after serial_puts clobbers
                                         ; ecx
    call xhci_get_device_descriptor
    test eax, eax
    jnz .get_desc_ok
    add rsp, 8                          ; drop the re-pushed slot ID
    lea rcx, [rel msg_xhci_desc_fail]
    call serial_puts
    jmp .out
.get_desc_ok:
    lea rcx, [rel msg_xhci_desc_ok]
    call serial_puts
    movzx eax, byte [rel xhci_device_descriptor+8]   ; idVendor low byte
    movzx ecx, byte [rel xhci_device_descriptor+9]   ; idVendor high byte
    shl ecx, 8
    or eax, ecx
    call dbg_hex64
    movzx eax, byte [rel xhci_device_descriptor+10]  ; idProduct low byte
    movzx ecx, byte [rel xhci_device_descriptor+11]  ; idProduct high byte
    shl ecx, 8
    or eax, ecx
    call dbg_hex64

    pop rcx                             ; slot ID
    push rcx                            ; ...and re-save it, needed again
                                         ; below after serial_puts clobbers
                                         ; ecx
    call xhci_get_config_descriptor
    test eax, eax
    jnz .get_config_ok
    add rsp, 8                          ; drop the re-pushed slot ID
    lea rcx, [rel msg_xhci_config_fail]
    call serial_puts
    jmp .out
.get_config_ok:
    lea rcx, [rel msg_xhci_config_ok]
    call serial_puts
    mov eax, [rel xhci_bulk_in_ep_index]
    call dbg_hex64
    mov eax, [rel xhci_bulk_out_ep_index]
    call dbg_hex64
    mov eax, [rel xhci_bulk_in_max_packet]
    call dbg_hex64
    mov eax, [rel xhci_bulk_out_max_packet]
    call dbg_hex64

    pop rcx                             ; slot ID
    push rcx
    call xhci_set_configuration
    test eax, eax
    jnz .set_config_ok
    add rsp, 8
    lea rcx, [rel msg_xhci_setcfg_fail]
    call serial_puts
    jmp .out
.set_config_ok:
    lea rcx, [rel msg_xhci_setcfg_ok]
    call serial_puts

    pop rcx                             ; slot ID
    push rcx
    call xhci_configure_endpoints
    test eax, eax
    jnz .configure_ep_ok
    add rsp, 8
    lea rcx, [rel msg_xhci_configep_fail]
    call serial_puts
    jmp .out
.configure_ep_ok:
    lea rcx, [rel msg_xhci_configep_ok]
    call serial_puts

    pop rcx                             ; slot ID
    push rcx
    call xhci_msd_inquiry
    test eax, eax
    jnz .inquiry_ok
    add rsp, 8
    lea rcx, [rel msg_xhci_inquiry_fail]
    call serial_puts
    jmp .out
.inquiry_ok:
    lea rcx, [rel msg_xhci_inquiry_ok]
    call serial_puts
    movzx eax, byte [rel xhci_msd_inquiry_data+8]    ; T10 Vendor ID, byte 0
    call dbg_hex64

    pop rcx                             ; slot ID
    mov [rel xhci_msd_slot_id], ecx
    call xhci_msd_read_capacity
    test eax, eax
    jz .read_capacity_failed
    lea rcx, [rel msg_xhci_readcap_ok]
    call serial_puts
    mov eax, [rel xhci_msd_sector_count]
    call dbg_hex64
    mov eax, [rel xhci_msd_sector_size]
    call dbg_hex64

    ; USB is only the active storage backend as a fallback of last
    ; resort -- if AHCI or NVMe already claimed a working device
    ; earlier in boot, leave that alone (this project's exFAT tests
    ; run against whichever backend storage_active_driver names, and
    ; the QEMU test disk image behind this USB device isn't formatted
    ; exFAT).
    cmp dword [rel storage_active_driver], STORAGE_NONE
    jne .skip_active_driver
    mov dword [rel storage_active_driver], STORAGE_USB
.skip_active_driver:

    xor ecx, ecx                        ; LBA 0
    mov edx, 1                          ; 1 sector
    lea r8, [rel xhci_msd_test_sector]
    call usb_msd_read_sectors
    test eax, eax
    jz .msd_read_failed
    lea rcx, [rel msg_xhci_msdread_ok]
    call serial_puts
    jmp .out
.msd_read_failed:
    lea rcx, [rel msg_xhci_msdread_fail]
    call serial_puts
    jmp .out
.read_capacity_failed:
    lea rcx, [rel msg_xhci_readcap_fail]
    call serial_puts
    jmp .out
.address_device_failed:
    lea rcx, [rel msg_xhci_addr_fail]
    call serial_puts
    jmp .out
.setup_slot_failed:
    add rsp, 8                          ; drop the pushed slot ID
    lea rcx, [rel msg_xhci_setup_fail]
    call serial_puts
    jmp .out
.enable_slot_failed:
    lea rcx, [rel msg_xhci_slot_fail]
    call serial_puts
    jmp .out
.noop_failed:
    lea rcx, [rel msg_xhci_noop_fail]
    call serial_puts
    jmp .out
.reset_failed:
    lea rcx, [rel msg_xhci_reset_fail]
    call serial_puts
    jmp .out
.not_found:
    lea rcx, [rel msg_xhci_not_found]
    call serial_puts
.out:
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
; Paints both the glyph's set pixels (fb_text_color, white by default) AND
; its clear ones (black), not just the set ones -- this is what makes it
; safe to redraw a cell that previously held a *different*, wider/taller-
; stroked character (the shell's line editor depends on this for
; backspace/delete/insert redraws).
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
    test r12d, 0x80
    jz .clear_pixel
    mov eax, [rel fb_text_color]
    mov dword [rdi], eax
    jmp .col_next
.clear_pixel:
    mov dword [rdi], 0                  ; black -- erases whatever was here before
.col_next:
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
; console_init: clears the screen and resets the scrolling text console
; (used by the shell) to the top-left, computing its size in 8x16 cells
; from the framebuffer's actual resolution.
; -------------------------------------------------------------------------
console_init:
    push rax
    push rcx
    push rdx

    call fb_clear

    mov eax, [rbx+FB_WIDTH]
    xor edx, edx
    mov ecx, 8
    div ecx
    mov [rel console_cols], eax

    mov eax, [rbx+FB_HEIGHT]
    xor edx, edx
    mov ecx, 16
    div ecx
    mov [rel console_rows], eax

    mov dword [rel console_col], 0
    mov dword [rel console_row], 0

    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; console_scroll_up: shifts the whole framebuffer up by one text row (16
; scanlines) and clears the newly-exposed bottom row.
; -------------------------------------------------------------------------
console_scroll_up:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    mov edx, [rbx+FB_STRIDE]            ; edx = pixels (== dwords) per scanline

    mov eax, [rbx+FB_HEIGHT]
    sub eax, 16
    imul eax, edx                       ; eax = dwords to move (rest of screen)
    mov ecx, eax

    mov rsi, [rbx+FB_BASE]
    mov rax, rdx
    shl rax, 4                          ; 16 scanlines worth of dwords
    lea rsi, [rsi + rax*4]              ; rsi = fb_base + 16 scanlines (src)
    mov rdi, [rbx+FB_BASE]              ; rdi = fb_base (dst)
    cld
    rep movsd

    ; rdi now points at the start of the newly-exposed bottom row; clear it
    mov eax, edx
    shl eax, 4                          ; 16 scanlines worth of dwords
    mov ecx, eax
    xor eax, eax
    rep stosd

    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; console_putc: AL = character. Handles CR/LF (newline), backspace
; (erase within the current line only -- no cross-line backspace in v1),
; and ordinary printable characters, including cursor advance, wrap, and
; scroll.
; -------------------------------------------------------------------------
console_putc:
    push rax
    push rcx
    push rdx
    push r8

    call serial_putc                    ; mirror everything to serial too

    cmp al, 13
    je .newline
    cmp al, 10
    je .newline
    cmp al, 8
    je .backspace

    mov r8b, al
    mov ecx, [rel console_col]
    mov edx, [rel console_row]
    call fb_draw_char
    inc dword [rel console_col]
    mov eax, [rel console_col]
    cmp eax, [rel console_cols]
    jl .out
    jmp .newline

.backspace:
    cmp dword [rel console_col], 0
    je .out
    dec dword [rel console_col]
    mov r8b, ' '
    mov ecx, [rel console_col]
    mov edx, [rel console_row]
    call fb_draw_char
    jmp .out

.newline:
    mov dword [rel console_col], 0
    inc dword [rel console_row]
    mov eax, [rel console_row]
    cmp eax, [rel console_rows]
    jl .out
    call console_scroll_up
    mov eax, [rel console_rows]
    dec eax
    mov [rel console_row], eax

.out:
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; console_puts: RCX = pointer to a NUL-terminated ASCII string.
; -------------------------------------------------------------------------
console_puts:
    push rax
    push rcx
    push rsi

    mov rsi, rcx
.loop:
    movzx eax, byte [rsi]
    test al, al
    jz .done
    call console_putc
    inc rsi
    jmp .loop
.done:
    pop rsi
    pop rcx
    pop rax
    ret

; =============================================================================
; Basic interactive shell: reads a command line from the keyboard, echoes
; it to the scrolling text console, and dispatches HELP / DIR / TYPE /
; WRITE / CLEAR. Runs as the tail of the "main" task (see the scheduler
; bring-up above), so rbx (boot_info*) is already valid here.
; =============================================================================
SHELL_LINE_MAX equ 120
SHELL_TYPE_BUF_MAX equ 65535             ; exfat_test_buf is 65536 bytes; -1 for NUL
SHELL_HISTORY_MAX equ 8                 ; recalled via Up/Down, see shell_history_recall

; -------------------------------------------------------------------------
; shell_streq: RCX/RDX = two NUL-terminated ASCII strings. Returns
; EAX=1/0.
; -------------------------------------------------------------------------
shell_streq:
    push rcx
    push rdx
.loop:
    mov al, [rcx]
    mov ah, [rdx]
    cmp al, ah
    jne .neq
    test al, al
    jz .eq
    inc rcx
    inc rdx
    jmp .loop
.eq:
    mov eax, 1
    jmp .out
.neq:
    xor eax, eax
.out:
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; shell_cursor_to: repositions the console's logical cursor (console_col/
; console_row) to a given index within the line being edited, and draws
; the cursor indicator (a solid underscore glyph) there. R8D=target
; index (0..line length), R9D=the line's starting column, R10D=the
; line's starting row, R11D=the line's current length (shell_line_buf[0
; .. R11D-1]).
;
; Before moving, repaints the WHOLE line from its start via
; shell_redraw_range -- this is what erases the cursor glyph left behind
; by wherever it was last drawn, without this function (or any of its
; many call sites) needing to separately track/restore that position:
; the cursor is always within [0, length] of the very same line
; (inclusive -- it can sit one past the last real character, in the
; empty cell right after it), so the redraw covers [0, length) PLUS one
; trailing blank space to reach that cell too, and always correctly
; restores whatever cell the previous cursor draw was overlaying.
; Callers that already did their own partial redraw before calling this
; (insert/delete) end up doing some redundant redraw work, which is
; cheap and harmless at SHELL_LINE_MAX (120).
;
; Does not account for the line wrapping past the bottom of the screen
; and triggering a scroll mid-edit -- acceptable for SHELL_LINE_MAX (120)
; against any reasonable console width/height, not a general terminal.
; -------------------------------------------------------------------------
shell_cursor_to:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12

    mov r12d, r8d                       ; r12d = target index (survives
                                         ; the shell_redraw_range call
                                         ; below, which reuses r8/r9/r10)

    mov [rel console_col], r9d
    mov [rel console_row], r10d

    push r9                              ; save start_col/start_row across
    push r10                             ; shell_redraw_range's own r9/r10 use
    xor r8d, r8d
    mov r9d, r11d
    mov r10d, 1                          ; +1 trailing blank: the cursor
                                         ; can sit one past the last real
                                         ; character (the empty tail cell)
    call shell_redraw_range
    pop r10
    pop r9

    mov eax, r9d
    add eax, r12d                       ; eax = start_col + index
    xor edx, edx
    div dword [rel console_cols]        ; eax = row delta, edx = column
    add eax, r10d
    mov [rel console_row], eax
    mov [rel console_col], edx

    mov ecx, edx
    mov edx, eax
    mov r8b, '_'
    call fb_draw_char

    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_redraw_range: draws shell_line_buf[R8D..R9D-1] via console_putc
; (so it naturally advances/wraps/scrolls like ordinary typing), then
; R10D additional trailing spaces (to erase stale characters left behind
; by a delete/backspace that shortened the line). Does not touch the
; cursor position afterward -- the caller repositions via shell_cursor_to.
; -------------------------------------------------------------------------
shell_redraw_range:
    push rax
    push rcx
    push rsi
    push rdi

    lea rdi, [rel shell_line_buf]
    mov esi, r8d
.chars:
    cmp esi, r9d
    jge .trailing
    movzx eax, byte [rdi+rsi]
    call console_putc
    inc esi
    jmp .chars
.trailing:
    mov ecx, r10d
    test ecx, ecx
    jz .done
.spaces:
    mov al, ' '
    call console_putc
    dec ecx
    jnz .spaces
.done:
    pop rdi
    pop rsi
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_print_spaces: ECX = count. Prints that many space characters.
; -------------------------------------------------------------------------
shell_print_spaces:
    push rax
    push rcx
.sp_loop:
    test ecx, ecx
    jz .sp_done
    mov al, ' '
    call console_putc
    dec ecx
    jmp .sp_loop
.sp_done:
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_strcpy: RCX = dest, RDX = src (NUL-terminated). Copies src to
; dest, including the NUL. Returns EAX = length copied, excluding NUL.
; -------------------------------------------------------------------------
shell_strcpy:
    push rcx
    push rdx
    push rsi
    push rdi

    mov rdi, rcx
    mov rsi, rdx
    xor eax, eax
.sc_loop:
    mov cl, [rsi]
    mov [rdi], cl
    test cl, cl
    jz .sc_done
    inc rsi
    inc rdi
    inc eax
    jmp .sc_loop
.sc_done:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; shell_format_dec: RCX = value (unsigned qword), RDX = dest buffer.
; Writes its decimal digits (no NUL) into the buffer. Returns EAX =
; digit count written. Sibling of bl_print_dec (boot/bootloader.asm)
; but writes to a buffer instead of printing directly, and works with a
; full qword since exFAT DataLength is a qword (values seen in practice
; are always far smaller, but this is correct for the general case).
; -------------------------------------------------------------------------
shell_format_dec:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov rdi, rdx                        ; rdi = dest buffer
    mov rax, rcx                        ; rax = remaining value

    test rax, rax
    jnz .fd_have_digits
    mov byte [rdi], '0'
    mov eax, 1
    jmp .fd_out

.fd_have_digits:
    lea r8, [rel shell_dec_scratch + 24] ; r8 = end of scratch; work backwards
    xor esi, esi                        ; esi = digit count
    mov r9, 10
.fd_divloop:
    test rax, rax
    jz .fd_copyout
    xor edx, edx
    div r9                              ; rax = rax/10, rdx = rax%10
    add dl, '0'
    dec r8
    mov [r8], dl
    inc esi
    jmp .fd_divloop
.fd_copyout:
    mov ecx, esi
.fd_copy:
    cmp ecx, 0
    je .fd_copy_done
    mov al, [r8]
    mov [rdi], al
    inc r8
    inc rdi
    dec ecx
    jmp .fd_copy
.fd_copy_done:
    mov eax, esi
.fd_out:
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; shell_line_insert: inserts AL at cursor index R12D into shell_line_buf,
; growing ECX (length) and R12D (cursor) by one, and redraws the (now
; shifted) tail of the line. R13D/R14D = the line's starting column/row
; (for repositioning the cursor afterward).
; -------------------------------------------------------------------------
shell_line_insert:
    push rax
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    lea rdi, [rel shell_line_buf]
    mov r9d, eax                        ; r9d = the character (AL survives calls below)
    mov esi, ecx                        ; esi = old length, shifting down to r12d
.shift:
    cmp esi, r12d
    je .place
    dec esi
    mov dl, [rdi+rsi]
    mov [rdi+rsi+1], dl
    jmp .shift
.place:
    mov [rdi+r12], r9b
    inc ecx

    mov r8d, r12d
    mov r9d, ecx
    xor r10d, r10d
    call shell_redraw_range

    inc r12d
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_line_delete_before: removes the character immediately before
; cursor index R12D (backspace). Shrinks ECX/R12D and redraws the tail
; plus one trailing space. No-op if R12D==0 (caller already checks, but
; safe either way).
; -------------------------------------------------------------------------
shell_line_delete_before:
    test r12d, r12d
    jz .out
    push rax
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    lea rdi, [rel shell_line_buf]
    dec r12d
    mov r9d, ecx
    dec r9d                             ; r9d = old length - 1 (exclusive bound)
    mov esi, r12d
.shift:
    cmp esi, r9d
    jge .shifted
    mov dl, [rdi+rsi+1]
    mov [rdi+rsi], dl
    inc esi
    jmp .shift
.shifted:
    dec ecx

    ; the video cursor is still wherever it was before this backspace (the
    ; PRE-decrement position) -- reposition to the new cursor index first,
    ; or the redraw below draws one column too far right instead of over
    ; the character that needs erasing.
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

    mov r8d, r12d
    mov r9d, ecx
    mov r10d, 1
    call shell_redraw_range

    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rax
.out:
    ret

; -------------------------------------------------------------------------
; shell_line_delete_at: removes the character AT cursor index R12D
; (forward delete / the Delete key). Shrinks ECX (cursor stays put) and
; redraws the tail plus one trailing space. No-op if R12D>=ECX.
; -------------------------------------------------------------------------
shell_line_delete_at:
    cmp r12d, ecx
    jge .out
    push rax
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    lea rdi, [rel shell_line_buf]
    mov r9d, ecx
    dec r9d                             ; r9d = old length - 1 (exclusive bound)
    mov esi, r12d
.shift:
    cmp esi, r9d
    jge .shifted
    mov dl, [rdi+rsi+1]
    mov [rdi+rsi], dl
    inc esi
    jmp .shift
.shifted:
    dec ecx

    ; cursor index doesn't change for a forward delete, but reposition
    ; the video cursor explicitly anyway rather than relying on it
    ; already being in sync -- cheap, and matches delete_before's fix.
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

    mov r8d, r12d
    mov r9d, ecx
    mov r10d, 1
    call shell_redraw_range

    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rax
.out:
    ret

; -------------------------------------------------------------------------
; shell_history_push: RCX = pointer to a NUL-terminated line just entered.
; Appends it to shell_history's ring buffer unless it's empty. Overwrites
; the oldest entry once the ring is full.
; -------------------------------------------------------------------------
shell_history_push:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    cmp byte [rcx], 0
    je .out                             ; don't clutter history with blank lines

    mov eax, [rel shell_history_write]
    mov edx, eax
    imul edx, SHELL_LINE_MAX
    lea rdi, [rel shell_history]
    add rdi, rdx                        ; rdi = this slot
    mov rsi, rcx
.copy:
    mov dl, [rsi]
    mov [rdi], dl
    test dl, dl
    jz .copied
    inc rsi
    inc rdi
    jmp .copy
.copied:
    inc eax
    cmp eax, SHELL_HISTORY_MAX
    jl .no_wrap
    xor eax, eax
.no_wrap:
    mov [rel shell_history_write], eax

    mov eax, [rel shell_history_count]
    cmp eax, SHELL_HISTORY_MAX
    jge .out
    inc eax
    mov [rel shell_history_count], eax
.out:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_history_recall: R8D = +1 (Up: older) or -1 (Down: newer). Loads a
; history entry into shell_line_buf, replacing the current in-progress
; line, and redraws it. In/out via ECX (length), R12D (cursor), R15D
; (browse depth: 0 = editing a fresh line, N>0 = showing the Nth-most-
; recent history entry); R13D/R14D = the line's starting column/row.
; -------------------------------------------------------------------------
shell_history_recall:
    push rax
    push rdx
    push rsi
    push rdi
    push r9
    push r10
    push r11

    mov r11d, [rel shell_history_count]
    test r11d, r11d
    jz .out                             ; no history at all -- nothing to do

    cmp r8d, 0
    jg .up
    ; Down: browsing off the oldest edge just clears back to an empty line.
    test r15d, r15d
    jz .out                             ; not browsing -- nothing to come back to
    dec r15d
    jmp .load
.up:
    cmp r15d, r11d
    jge .out                            ; already at the oldest entry
    inc r15d

.load:
    ; blank out whatever's on screen for the current content first
    mov eax, ecx
    xor ecx, ecx
    xor r12d, r12d
    mov r9d, r13d
    mov r10d, r14d
    push r8
    mov r8d, 0
    mov r11d, ecx                       ; r11d = 0 (new length); the
                                         ; history-count value r11d held
                                         ; is no longer needed past here
    call shell_cursor_to
    pop r8

    test r15d, r15d
    jnz .load_entry
    ; back to an empty, fresh line
    lea rdi, [rel shell_line_buf]
    mov byte [rdi], 0
    jmp .redraw

.load_entry:
    ; slot = (write_index - browse_depth + MAX) mod MAX, i.e. the r15d-th
    ; most recently written entry
    mov edx, [rel shell_history_write]
    sub edx, r15d
    add edx, SHELL_HISTORY_MAX
    xor eax, eax
.mod:
    cmp edx, SHELL_HISTORY_MAX
    jl .have_slot
    sub edx, SHELL_HISTORY_MAX
    jmp .mod
.have_slot:
    imul edx, SHELL_LINE_MAX
    lea rsi, [rel shell_history]
    add rsi, rdx
    lea rdi, [rel shell_line_buf]
.copy:
    mov al, [rsi]
    mov [rdi], al
    test al, al
    jz .redraw
    inc rsi
    inc rdi
    jmp .copy

.redraw:
    lea rdi, [rel shell_line_buf]
    xor esi, esi
.len:
    cmp byte [rdi+rsi], 0
    je .have_len
    inc esi
    jmp .len
.have_len:
    mov ecx, esi                        ; ecx = new length
    mov r12d, ecx                       ; cursor lands at end, like a real shell

    mov r8d, 0
    mov r9d, ecx
    xor r10d, r10d
    call shell_redraw_range

    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to

.out:
    pop r11
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_read_line: reads keys into shell_line_buf (NUL-terminated, up to
; SHELL_LINE_MAX-1 chars), echoing each to the console, until Enter.
; Supports mid-line editing (Left/Right/Home/End/Delete/Backspace, with a
; real insert-at-cursor rather than append-only) and command-history
; recall (Up/Down) via shell_history.
; -------------------------------------------------------------------------
shell_read_line:
    push rax
    push rcx
    push rdi
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15

    xor ecx, ecx                        ; ecx = line length
    xor r12d, r12d                      ; r12d = cursor index
    xor r15d, r15d                      ; r15d = history browse depth
    mov r13d, [rel console_col]         ; r13d = this line's starting column
    mov r14d, [rel console_row]         ; r14d = this line's starting row
    lea rdi, [rel shell_line_buf]

    mov r8d, r12d                       ; draw the cursor indicator at the
    mov r9d, r13d                       ; empty prompt, before any keystroke
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
.loop:
    call kbd_read_char
    cmp al, 13
    je .enter
    cmp al, 8
    je .backspace
    cmp al, KBD_KEY_DELETE
    je .fwd_delete
    cmp al, KBD_KEY_LEFT
    je .move_left
    cmp al, KBD_KEY_RIGHT
    je .move_right
    cmp al, KBD_KEY_HOME
    je .move_home
    cmp al, KBD_KEY_END
    je .move_end
    cmp al, KBD_KEY_UP
    je .hist_up
    cmp al, KBD_KEY_DOWN
    je .hist_down
    cmp al, ' '
    jb .loop                            ; other control bytes: ignore
    cmp al, 0x7E
    ja .loop                            ; DEL (0x7F) and virtual key codes: ignore
    cmp ecx, SHELL_LINE_MAX-1
    jge .loop                           ; line full -- drop the character
    call shell_line_insert
    jmp .loop
.backspace:
    call shell_line_delete_before
    jmp .loop
.fwd_delete:
    call shell_line_delete_at
    jmp .loop
.move_left:
    test r12d, r12d
    jz .loop
    dec r12d
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
    jmp .loop
.move_right:
    cmp r12d, ecx
    jge .loop
    inc r12d
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
    jmp .loop
.move_home:
    xor r12d, r12d
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
    jmp .loop
.move_end:
    mov r12d, ecx
    mov r8d, r12d
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
    jmp .loop
.hist_up:
    mov r8d, 1
    call shell_history_recall
    jmp .loop
.hist_down:
    mov r8d, -1
    call shell_history_recall
    jmp .loop
.enter:
    mov byte [rdi+rcx], 0
    mov r8d, ecx
    mov r9d, r13d
    mov r10d, r14d
    mov r11d, ecx
    call shell_cursor_to
    mov al, 13
    call console_putc
    lea rcx, [rel shell_line_buf]
    call shell_history_push
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_dispatch: RCX = ptr to a NUL-terminated command line. Parses the
; first whitespace-delimited word as the command (case-folded to
; lowercase) and dispatches on it.
; -------------------------------------------------------------------------
shell_dispatch:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    mov rsi, rcx
.skip_lead:
    cmp byte [rsi], ' '
    jne .after_lead
    inc rsi
    jmp .skip_lead
.after_lead:
    cmp byte [rsi], 0
    je .out

    lea rdi, [rel shell_cmd_buf]
    xor ecx, ecx
.copy_cmd:
    mov al, [rsi]
    test al, al
    jz .cmd_done
    cmp al, ' '
    je .cmd_done
    cmp ecx, 15
    jge .cmd_done
    cmp al, 'A'
    jb .no_fold
    cmp al, 'Z'
    ja .no_fold
    add al, 0x20
.no_fold:
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .copy_cmd
.cmd_done:
    mov byte [rdi+rcx], 0

.skip_arg_lead:
    cmp byte [rsi], ' '
    jne .have_args
    inc rsi
    jmp .skip_arg_lead
.have_args:                             ; rsi -> argument text (maybe empty)

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_help]
    call shell_streq
    test eax, eax
    jnz .do_help

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_dir]
    call shell_streq
    test eax, eax
    jnz .do_dir

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_clear]
    call shell_streq
    test eax, eax
    jnz .do_clear

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_type]
    call shell_streq
    test eax, eax
    jnz .do_type

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_write]
    call shell_streq
    test eax, eax
    jnz .do_write

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_run]
    call shell_streq
    test eax, eax
    jnz .do_run

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_del]
    call shell_streq
    test eax, eax
    jnz .do_del

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_rename]
    call shell_streq
    test eax, eax
    jnz .do_rename

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_append]
    call shell_streq
    test eax, eax
    jnz .do_append

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_truncate]
    call shell_streq
    test eax, eax
    jnz .do_truncate

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_mkdir]
    call shell_streq
    test eax, eax
    jnz .do_mkdir

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_open]
    call shell_streq
    test eax, eax
    jnz .do_open

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_up]
    call shell_streq
    test eax, eax
    jnz .do_up

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_tree]
    call shell_streq
    test eax, eax
    jnz .do_tree

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_move]
    call shell_streq
    test eax, eax
    jnz .do_move

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_rmdir]
    call shell_streq
    test eax, eax
    jnz .do_rmdir

    lea rcx, [rel msg_shell_unknown]
    call console_puts
    jmp .out

.do_help:
    lea rcx, [rel msg_shell_help]
    call console_puts
    jmp .out

.do_clear:
    call console_init
    jmp .out

.do_dir:
    mov ecx, [rel exfat_cwd_cluster]
    xor edx, edx                        ; DIR always uses depth 0
    call shell_dir_collect
    xor edx, edx
    call shell_dir_print_grid
    lea rcx, [rel msg_shell_lf]         ; blank line to separate the listing
    call console_puts                   ; from the next prompt -- the grid
                                         ; itself already ends on a fresh
                                         ; line, so one more LF is the gap
    jmp .out

.do_type:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.type_copy:
    mov al, [rsi]
    test al, al
    jz .type_arg_done
    cmp al, ' '
    je .type_arg_done
    cmp ecx, 63
    jge .type_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .type_copy
.type_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .type_usage

    lea rcx, [rel shell_arg_buf]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .type_notfound
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .type_isdir

    mov rax, [rel exfat_find_result+8]
    cmp rax, SHELL_TYPE_BUF_MAX
    ja .type_toobig

    mov ecx, [rel exfat_find_result+0]
    mov rdx, [rel exfat_find_result+8]
    lea r8, [rel exfat_test_buf]
    mov r9d, [rel exfat_find_result+16]
    call exfat_read_file
    test eax, eax
    jz .type_readfail

    mov rax, [rel exfat_find_result+8]
    lea rdi, [rel exfat_test_buf]
    mov byte [rdi+rax], 0
    lea rcx, [rel exfat_test_buf]
    call console_puts
    lea rcx, [rel msg_shell_nl]
    call console_puts
    jmp .out
.type_usage:
    lea rcx, [rel msg_shell_type_usage]
    call console_puts
    jmp .out
.type_notfound:
    lea rcx, [rel msg_shell_notfound]
    call console_puts
    jmp .out
.type_isdir:
    lea rcx, [rel msg_shell_isdir]
    call console_puts
    jmp .out
.type_toobig:
    lea rcx, [rel msg_shell_toobig]
    call console_puts
    jmp .out
.type_readfail:
    lea rcx, [rel msg_shell_readfail]
    call console_puts
    jmp .out

.do_write:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.write_name_copy:
    mov al, [rsi]
    test al, al
    jz .write_name_done
    cmp al, ' '
    je .write_name_done
    cmp ecx, 63
    jge .write_name_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .write_name_copy
.write_name_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .write_usage

.write_skip_sp:
    cmp byte [rsi], ' '
    jne .write_have_content
    inc rsi
    jmp .write_skip_sp
.write_have_content:
    mov r10, rsi                        ; r10 = content ptr
    xor r11d, r11d                      ; r11d = content length
.write_len:
    cmp byte [r10+r11], 0
    je .write_len_done
    inc r11d
    jmp .write_len
.write_len_done:

    lea rcx, [rel shell_arg_buf]
    mov r8, r10
    mov r9, r11
    call exfat_write_file
    test eax, eax
    jz .write_fail

    lea rcx, [rel msg_shell_write_ok]
    call console_puts
    jmp .out
.write_usage:
    lea rcx, [rel msg_shell_write_usage]
    call console_puts
    jmp .out
.write_fail:
    lea rcx, [rel msg_shell_write_fail]
    call console_puts
    jmp .out

.do_run:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.run_copy:
    mov al, [rsi]
    test al, al
    jz .run_arg_done
    cmp al, ' '
    je .run_arg_done
    cmp ecx, 63
    jge .run_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .run_copy
.run_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .run_usage

    lea rcx, [rel shell_arg_buf]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .run_notfound

    ; Stream-compile straight from exFAT (basix_compile_file /
    ; basix_lex_init_stream) instead of reading the whole program into
    ; one buffer first -- program size is no longer bounded by
    ; exfat_test_buf/SHELL_TYPE_BUF_MAX at all (that limit still
    ; applies to TYPE, which genuinely needs the whole file resident
    ; to display it).
    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    mov rdx, [rel exfat_find_result+8]  ; DataLength
    mov r8d, [rel exfat_find_result+16] ; NoFatChain
    call basix_compile_file
    test eax, eax
    jz .run_compilefail

    call basix_code_buf
    mov dword [rel fb_text_color], 0xFFFFFFFF  ; a program may leave COLOR
                                                ; set to something other than
                                                ; white -- don't let that leak
                                                ; into the shell's own prompt
    jmp .out
.run_usage:
    lea rcx, [rel msg_shell_run_usage]
    call console_puts
    jmp .out
.run_notfound:
    lea rcx, [rel msg_shell_notfound]
    call console_puts
    jmp .out
.run_compilefail:
    lea rcx, [rel msg_shell_run_compilefail]
    call console_puts
    jmp .out

.do_del:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.del_copy:
    mov al, [rsi]
    test al, al
    jz .del_arg_done
    cmp al, ' '
    je .del_arg_done
    cmp ecx, 63
    jge .del_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .del_copy
.del_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .del_usage

    lea rcx, [rel shell_arg_buf]
    call exfat_delete_file
    test eax, eax
    jz .del_fail

    lea rcx, [rel msg_shell_del_ok]
    call console_puts
    jmp .out
.del_usage:
    lea rcx, [rel msg_shell_del_usage]
    call console_puts
    jmp .out
.del_fail:
    lea rcx, [rel msg_shell_del_fail]
    call console_puts
    jmp .out

.do_rename:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.rename_copy1:
    mov al, [rsi]
    test al, al
    jz .rename_arg1_done
    cmp al, ' '
    je .rename_arg1_done
    cmp ecx, 63
    jge .rename_arg1_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .rename_copy1
.rename_arg1_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .rename_usage

.rename_skip_sp:
    cmp byte [rsi], ' '
    jne .rename_copy2_start
    inc rsi
    jmp .rename_skip_sp
.rename_copy2_start:
    lea rdi, [rel shell_arg_buf2]
    xor ecx, ecx
.rename_copy2:
    mov al, [rsi]
    test al, al
    jz .rename_arg2_done
    cmp al, ' '
    je .rename_arg2_done
    cmp ecx, 63
    jge .rename_arg2_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .rename_copy2
.rename_arg2_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .rename_usage

    lea rcx, [rel shell_arg_buf]
    lea rdx, [rel shell_arg_buf2]
    call exfat_rename_file
    test eax, eax
    jz .rename_fail

    lea rcx, [rel msg_shell_rename_ok]
    call console_puts
    jmp .out
.rename_usage:
    lea rcx, [rel msg_shell_rename_usage]
    call console_puts
    jmp .out
.rename_fail:
    lea rcx, [rel msg_shell_rename_fail]
    call console_puts
    jmp .out

.do_append:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.append_name_copy:
    mov al, [rsi]
    test al, al
    jz .append_name_done
    cmp al, ' '
    je .append_name_done
    cmp ecx, 63
    jge .append_name_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .append_name_copy
.append_name_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .append_usage

.append_skip_sp:
    cmp byte [rsi], ' '
    jne .append_have_content
    inc rsi
    jmp .append_skip_sp
.append_have_content:
    mov r10, rsi                        ; r10 = content ptr
    xor r11d, r11d                      ; r11d = content length
.append_len:
    cmp byte [r10+r11], 0
    je .append_len_done
    inc r11d
    jmp .append_len
.append_len_done:

    lea rcx, [rel shell_arg_buf]
    mov r8, r10
    mov r9, r11
    call exfat_append_file
    test eax, eax
    jz .append_fail

    lea rcx, [rel msg_shell_append_ok]
    call console_puts
    jmp .out
.append_usage:
    lea rcx, [rel msg_shell_append_usage]
    call console_puts
    jmp .out
.append_fail:
    lea rcx, [rel msg_shell_append_fail]
    call console_puts
    jmp .out

.do_truncate:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.truncate_name_copy:
    mov al, [rsi]
    test al, al
    jz .truncate_name_done
    cmp al, ' '
    je .truncate_name_done
    cmp ecx, 63
    jge .truncate_name_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .truncate_name_copy
.truncate_name_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .truncate_usage

.truncate_skip_sp:
    cmp byte [rsi], ' '
    jne .truncate_parse_num
    inc rsi
    jmp .truncate_skip_sp
.truncate_parse_num:
    cmp byte [rsi], 0
    je .truncate_usage
    xor rdx, rdx                        ; rdx = parsed size
.truncate_num_loop:
    movzx eax, byte [rsi]
    test al, al
    jz .truncate_num_done
    cmp al, '0'
    jb .truncate_usage
    cmp al, '9'
    ja .truncate_usage
    sub al, '0'
    movzx eax, al
    imul rdx, rdx, 10
    add rdx, rax
    inc rsi
    jmp .truncate_num_loop
.truncate_num_done:

    lea rcx, [rel shell_arg_buf]
    call exfat_truncate_file
    test eax, eax
    jz .truncate_fail

    lea rcx, [rel msg_shell_truncate_ok]
    call console_puts
    jmp .out
.truncate_usage:
    lea rcx, [rel msg_shell_truncate_usage]
    call console_puts
    jmp .out
.truncate_fail:
    lea rcx, [rel msg_shell_truncate_fail]
    call console_puts
    jmp .out

.do_mkdir:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.mkdir_copy:
    mov al, [rsi]
    test al, al
    jz .mkdir_arg_done
    cmp al, ' '
    je .mkdir_arg_done
    cmp ecx, 63
    jge .mkdir_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .mkdir_copy
.mkdir_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .mkdir_usage

    lea rcx, [rel shell_arg_buf]
    call exfat_create_dir
    test eax, eax
    jz .mkdir_fail

    lea rcx, [rel msg_shell_mkdir_ok]
    call console_puts
    jmp .out
.mkdir_usage:
    lea rcx, [rel msg_shell_mkdir_usage]
    call console_puts
    jmp .out
.mkdir_fail:
    lea rcx, [rel msg_shell_mkdir_fail]
    call console_puts
    jmp .out

.do_open:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.open_copy:
    mov al, [rsi]
    test al, al
    jz .open_arg_done
    cmp al, ' '
    je .open_arg_done
    cmp ecx, 63
    jge .open_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .open_copy
.open_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .open_usage

    lea rcx, [rel shell_arg_buf]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .open_notfound
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jz .open_notadir

    mov eax, [rel exfat_cwd_depth]
    cmp eax, EXFAT_CWD_MAX_DEPTH
    jge .open_toodeep

    mov ecx, [rel exfat_cwd_cluster]
    lea rdx, [rel exfat_cwd_stack]
    mov [rdx+rax*4], ecx

    push rsi
    push rdi
    mov ecx, eax
    imul ecx, EXFAT_CWD_NAME_LEN
    lea rdi, [rel exfat_cwd_name_stack]
    add rdi, rcx
    lea rsi, [rel shell_arg_buf]
    mov ecx, EXFAT_CWD_NAME_LEN
    cld
    rep movsb                           ; record the name for the prompt breadcrumb
    pop rdi
    pop rsi

    inc eax
    mov [rel exfat_cwd_depth], eax

    mov eax, [rel exfat_find_result+0]  ; target's FirstCluster
    mov [rel exfat_cwd_cluster], eax
    jmp .out
.open_usage:
    lea rcx, [rel msg_shell_open_usage]
    call console_puts
    jmp .out
.open_notfound:
    lea rcx, [rel msg_shell_open_notfound]
    call console_puts
    jmp .out
.open_notadir:
    lea rcx, [rel msg_shell_open_notadir]
    call console_puts
    jmp .out
.open_toodeep:
    lea rcx, [rel msg_shell_open_toodeep]
    call console_puts
    jmp .out

.do_up:
    mov eax, [rel exfat_cwd_depth]
    test eax, eax
    jz .up_atroot
    dec eax
    mov [rel exfat_cwd_depth], eax
    lea rdx, [rel exfat_cwd_stack]
    mov ecx, [rdx+rax*4]
    mov [rel exfat_cwd_cluster], ecx
    jmp .out
.up_atroot:
    lea rcx, [rel msg_shell_up_atroot]
    call console_puts
    jmp .out

.do_tree:
    mov ecx, [rel exfat_cwd_cluster]
    xor edx, edx
    call shell_tree_walk
    lea rcx, [rel msg_shell_lf]          ; blank line to separate the listing
    call console_puts                    ; from the next prompt -- shell_tree_
                                          ; walk already ends on a fresh line,
                                          ; so one more LF is the gap
    jmp .out

.do_move:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.move_copy1:
    mov al, [rsi]
    test al, al
    jz .move_arg1_done
    cmp al, ' '
    je .move_arg1_done
    cmp ecx, 63
    jge .move_arg1_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .move_copy1
.move_arg1_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .move_usage

.move_skip_sp:
    cmp byte [rsi], ' '
    jne .move_copy2_start
    inc rsi
    jmp .move_skip_sp
.move_copy2_start:
    lea rdi, [rel shell_arg_buf2]
    xor ecx, ecx
.move_copy2:
    mov al, [rsi]
    test al, al
    jz .move_arg2_done
    cmp al, ' '
    je .move_arg2_done
    cmp ecx, 63
    jge .move_arg2_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .move_copy2
.move_arg2_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .move_usage

    lea rcx, [rel shell_arg_buf]
    lea rdx, [rel shell_arg_buf2]
    call exfat_move_file
    test eax, eax
    jz .move_fail

    lea rcx, [rel msg_shell_move_ok]
    call console_puts
    jmp .out
.move_usage:
    lea rcx, [rel msg_shell_move_usage]
    call console_puts
    jmp .out
.move_fail:
    lea rcx, [rel msg_shell_move_fail]
    call console_puts
    jmp .out

.do_rmdir:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.rmdir_copy:
    mov al, [rsi]
    test al, al
    jz .rmdir_arg_done
    cmp al, ' '
    je .rmdir_arg_done
    cmp ecx, 63
    jge .rmdir_arg_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .rmdir_copy
.rmdir_arg_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .rmdir_usage

    lea rcx, [rel shell_arg_buf]
    call exfat_delete_dir
    test eax, eax
    jz .rmdir_fail

    lea rcx, [rel msg_shell_rmdir_ok]
    call console_puts
    jmp .out
.rmdir_usage:
    lea rcx, [rel msg_shell_rmdir_usage]
    call console_puts
    jmp .out
.rmdir_fail:
    lea rcx, [rel msg_shell_rmdir_fail]
    call console_puts
    jmp .out

.out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; shell_dir_collect: ECX = directory cluster, EDX = depth index
; (0..EXFAT_CWD_MAX_DEPTH-1). Drains exfat_dir_list_next into this
; depth's slice of the shell_dir_col_* arrays (raw name, FileAttributes,
; FirstCluster, DataLength), bounded at DIR_LIST_MAX_ENTRIES -- extra
; entries beyond that are silently dropped, matching this codebase's
; existing bounded-limit conventions elsewhere. Records the final count
; in shell_dir_col_count[depth]. Does not format or print anything.
; -------------------------------------------------------------------------
shell_dir_collect:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r12
    push r13

    mov r12d, edx                       ; r12d = depth
    call exfat_dir_list_start_at        ; ecx = cluster, still the input value

    xor r13d, r13d                      ; r13d = count so far
.dc_loop:
    cmp r13d, DIR_LIST_MAX_ENTRIES
    jge .dc_done

    mov eax, r12d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r13d                       ; eax = flat entry index
    mov r8d, eax                        ; r8d = flat index (exfat_dir_list_next
                                         ; preserves r8 -- see its own push list)
    imul eax, DIR_NAME_SLOT_LEN
    lea rcx, [rel shell_dir_col_names]
    add rcx, rax                        ; rcx = this entry's raw-name slot

    lea rdx, [rel shell_dir_entry]
    call exfat_dir_list_next
    test eax, eax
    jz .dc_done

    lea rdi, [rel shell_dir_col_attrs]
    mov eax, [rel shell_dir_entry+4]
    mov [rdi + r8*4], eax

    lea rdi, [rel shell_dir_col_clusters]
    mov eax, [rel shell_dir_entry+0]
    mov [rdi + r8*4], eax

    lea rdi, [rel shell_dir_col_sizes]
    mov rax, [rel shell_dir_entry+8]
    mov [rdi + r8*8], rax

    inc r13d
    jmp .dc_loop
.dc_done:
    lea rdi, [rel shell_dir_col_count]
    mov [rdi + r12*4], r13d

    pop r13
    pop r12
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_dir_format_entry: ECX = depth, EDX = index within that depth's
; collected entries. Formats that entry's display text -- "<DIR>" or the
; decimal size, space-padded to DIR_TAG_FIELD_WIDTH (7) characters, then
; a 2-character gutter, then the name -- so names always start in the
; same column regardless of tag/digit-count length, e.g. "<DIR>    name"
; / "137      name" both line up. Written into
; shell_dir_col_text[depth][index] (NUL-terminated, via
; shell_strcpy's own copy-including-NUL behavior on the final append)
; and records its length (excluding NUL) into
; shell_dir_col_textlen[depth][index].
; -------------------------------------------------------------------------
shell_dir_format_entry:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r12
    push r13
    push r14
    push r15

    mov r12d, ecx                       ; r12d = depth
    mov r13d, edx                       ; r13d = index

    mov eax, r12d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r13d
    mov r14d, eax                       ; r14d = flat index

    mov eax, r14d
    imul eax, DIR_TEXT_SLOT_LEN
    lea r15, [rel shell_dir_col_text]
    add r15, rax                        ; r15 = this entry's text slot

    lea rsi, [rel shell_dir_col_attrs]
    mov eax, [rsi + r14*4]
    test al, ATTR_DIRECTORY
    jz .fe_file

    mov rcx, r15
    lea rdx, [rel msg_shell_dir_tag]
    call shell_strcpy
    mov r8d, eax
    jmp .fe_pad_field

.fe_file:
    lea rsi, [rel shell_dir_col_sizes]
    mov rcx, [rsi + r14*8]
    mov rdx, r15
    call shell_format_dec
    mov r8d, eax

.fe_pad_field:
    ; space-pad the <DIR>/size field to DIR_TAG_FIELD_WIDTH so names start
    ; in the same column regardless of tag/digit-count length
.fe_pad_loop:
    cmp r8d, DIR_TAG_FIELD_WIDTH
    jge .fe_pad_done
    mov byte [r15 + r8], ' '
    inc r8d
    jmp .fe_pad_loop
.fe_pad_done:

    lea rcx, [r15 + r8]
    lea rdx, [rel msg_shell_dir_sep]
    call shell_strcpy
    add r8d, eax

.fe_append_name:
    lea rcx, [r15 + r8]
    mov eax, r14d
    imul eax, DIR_NAME_SLOT_LEN
    lea rdx, [rel shell_dir_col_names]
    add rdx, rax
    call shell_strcpy
    add r8d, eax

    lea rdi, [rel shell_dir_col_textlen]
    mov [rdi + r14*4], r8d

    pop r15
    pop r14
    pop r13
    pop r12
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_dir_print_grid: EDX = depth index whose entries (already
; collected via shell_dir_collect) should be printed. Formats every
; entry at this depth, finds the widest formatted entry, computes how
; many columns fit the console width (each column = widest-entry-width
; + DIR_GUTTER, at least 1 column), and prints them row-major with
; depth*2 leading spaces on every row. No-op if this depth has zero
; collected entries.
; -------------------------------------------------------------------------
shell_dir_print_grid:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r12
    push r13
    push r14
    push r15

    mov r12d, edx                       ; r12d = depth
    lea rax, [rel shell_dir_col_count]
    mov r13d, [rax + r12*4]             ; r13d = entry count at this depth
    test r13d, r13d
    jz .pg_out

    ; Pass 1: format every entry, track the widest.
    xor r14d, r14d                      ; r14d = index 0..count-1
    xor r15d, r15d                      ; r15d = running max length
.pg_fmt_loop:
    cmp r14d, r13d
    jge .pg_fmt_done
    mov ecx, r12d
    mov edx, r14d
    call shell_dir_format_entry

    mov eax, r12d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r14d
    lea rdi, [rel shell_dir_col_textlen]
    mov eax, [rdi + rax*4]
    cmp eax, r15d
    jle .pg_fmt_next
    mov r15d, eax
.pg_fmt_next:
    inc r14d
    jmp .pg_fmt_loop
.pg_fmt_done:

    ; Columns = console width / (widest + gutter), at least 1.
    mov eax, [rel console_cols]
    xor edx, edx
    mov ecx, r15d
    add ecx, DIR_GUTTER
    div ecx
    test eax, eax
    jnz .pg_have_cols
    mov eax, 1
.pg_have_cols:
    mov r8d, eax                        ; r8d = column count

    ; Pass 2: print row-major, r8d columns wide, r12d*2-space indent.
    xor r14d, r14d                      ; r14d = index 0..count-1
    mov ecx, r12d
    call shell_dir_print_indent
.pg_print_loop:
    cmp r14d, r13d
    jge .pg_print_finish

    mov eax, r12d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r14d
    mov ecx, eax
    imul ecx, DIR_TEXT_SLOT_LEN
    lea rdx, [rel shell_dir_col_text]
    add rdx, rcx
    mov rcx, rdx
    call console_puts

    mov eax, r12d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r14d
    lea rdx, [rel shell_dir_col_textlen]
    mov eax, [rdx + rax*4]
    mov ecx, r15d
    sub ecx, eax
    add ecx, DIR_GUTTER
    call shell_print_spaces

    mov eax, r14d
    inc eax
    mov r14d, eax
    xor edx, edx
    div r8d
    test edx, edx
    jnz .pg_print_loop               ; not a row boundary yet

    cmp r14d, r13d
    jge .pg_print_loop               ; last entry landed exactly on a row
                                      ; boundary -- let the loop-top check
                                      ; reach .pg_print_finish for the
                                      ; single trailing newline below
    lea rcx, [rel msg_shell_lf]
    call console_puts
    mov ecx, r12d
    call shell_dir_print_indent
    jmp .pg_print_loop

.pg_print_finish:
    ; Always end on a fresh line -- whether the grid's last row was full
    ; or partial, and whether or not more content (a caller's own
    ; trailing blank line, or TREE recursing into a subdirectory
    ; collected at this level) follows immediately afterward.
    lea rcx, [rel msg_shell_lf]
    call console_puts

.pg_out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_dir_print_indent: ECX = depth. Prints depth*2 spaces.
; -------------------------------------------------------------------------
shell_dir_print_indent:
    push rcx
    shl ecx, 1
    call shell_print_spaces
    pop rcx
    ret

; -------------------------------------------------------------------------
; shell_tree_walk: ECX = directory cluster to list, EDX = indentation
; depth (0 at the top). Collects and prints this directory's own
; contents as a multi-column grid (shell_dir_collect +
; shell_dir_print_grid), THEN recurses into each subdirectory found at
; this level -- collecting a whole level before printing (needed for
; column-width sizing) naturally means siblings are grouped together in
; the output before any of their children appear, rather than
; interleaving a subdirectory's contents immediately after its own
; name the way the old single-column version did. Recursion is capped
; at EXFAT_CWD_MAX_DEPTH to guard against a corrupt/cyclic cluster
; chain. Since each level is fully collected into its own depth-indexed
; array slice before any recursive call happens, there is no shared
; iterator state to save/restore across recursion (unlike the old
; version) -- exfat_list_cluster/exfat_list_offset are safely reused by
; the time a recursive call touches them again.
;
; NOTE: never stash cross-call scratch in rbx here -- it is a global
; invariant elsewhere in this kernel (holds boot_info*, read by
; fb_draw_char via console_putc) and is not preserved across the
; console_puts calls this function (and its helpers) make.
; -------------------------------------------------------------------------
shell_tree_walk:
    push rcx
    push rdx
    push r12
    push r13
    push r14

    cmp edx, EXFAT_CWD_MAX_DEPTH
    jge .out
    mov r13d, edx                       ; r13d = this call's depth

    call shell_dir_collect               ; ecx=cluster, edx=depth: still
                                         ; the original input values

    mov edx, r13d
    call shell_dir_print_grid

    lea rax, [rel shell_dir_col_count]
    mov r14d, [rax + r13*4]             ; r14d = entry count at this depth
    xor r12d, r12d                       ; r12d = recursion loop index
.walk_recurse_loop:
    cmp r12d, r14d
    jge .out

    mov eax, r13d
    imul eax, DIR_LIST_MAX_ENTRIES
    add eax, r12d                        ; eax = flat index

    lea rdx, [rel shell_dir_col_attrs]
    mov edx, [rdx + rax*4]
    test dl, ATTR_DIRECTORY
    jz .walk_recurse_next

    lea rdx, [rel shell_dir_col_clusters]
    mov ecx, [rdx + rax*4]              ; ecx = child's FirstCluster
    test ecx, ecx
    jz .walk_recurse_next               ; defensive: nothing to descend into

    mov edx, r13d
    inc edx
    call shell_tree_walk

.walk_recurse_next:
    inc r12d
    jmp .walk_recurse_loop

.out:
    pop r14
    pop r13
    pop r12
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; shell_print_cwd_path: prints the current-directory breadcrumb ahead of
; the prompt, e.g. "ROOT > GRAPHICS > WIREFRAME" -- built purely from
; exfat_cwd_name_stack/exfat_cwd_depth (see exfat.inc), never touching
; the disk.
; -------------------------------------------------------------------------
shell_print_cwd_path:
    push rax
    push rcx
    push rdx

    lea rcx, [rel msg_shell_root_label]
    call console_puts

    xor edx, edx
.loop:
    cmp edx, [rel exfat_cwd_depth]
    jge .done

    lea rcx, [rel msg_shell_path_sep]
    call console_puts

    mov eax, edx
    imul eax, EXFAT_CWD_NAME_LEN
    lea rcx, [rel exfat_cwd_name_stack]
    add rcx, rax
    call console_puts

    inc edx
    jmp .loop
.done:
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; shell_main: the interactive read-eval-print loop. Never returns.
; -------------------------------------------------------------------------
shell_main:
    call console_init

    lea rcx, [rel msg_shell_banner]
    call console_puts

.prompt:
    call sched_reap_zombies              ; free any tasks that ended since last prompt

    call shell_print_cwd_path
    lea rcx, [rel msg_shell_prompt]
    call console_puts

    call shell_read_line

    lea rcx, [rel shell_line_buf]
    call shell_dispatch

    jmp .prompt

draw_logo:
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    lea rsi, [rel logo_lines]
    xor r9d, r9d                        ; r9d = row
.next_line:
    cmp r9d, LOGO_LINE_COUNT
    jge .done
    xor ecx, ecx                        ; column 0
    mov edx, r9d
    mov r8, [rsi]
    call fb_draw_string
    add rsi, 8
    inc r9d
    jmp .next_line
.done:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
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
msg_dbg_fa:            db 'DBG frameA=', 0
msg_dbg_fb:            db 'DBG frameB=', 0
msg_dbg_m1:            db 'DBG map1 ret=', 0
msg_dbg_m2:            db 'DBG map2 ret=', 0
msg_vmm_ok:           db 'VMM: virtual->physical mapping OK', 13, 10, 0
msg_vmm_bad:          db 'VMM: virtual->physical mapping FAILED', 13, 10, 0
msg_kheap_ok:         db 'kmalloc/kfree: alloc/content/reuse OK', 13, 10, 0
msg_kheap_bad:        db 'kmalloc/kfree: FAILED', 13, 10, 0
msg_wtest_ok:         db 'storage_write_sectors: scratch write/read-back OK', 13, 10, 0
msg_wtest_bad:        db 'storage_write_sectors: scratch write/read-back FAILED', 13, 10, 0
msg_btest_ok:         db 'exFAT: allocation bitmap alloc/free round-trip OK', 13, 10, 0
msg_btest_bad:        db 'exFAT: allocation bitmap alloc/free round-trip FAILED', 13, 10, 0
msg_ctest_ok:         db 'exFAT: FAT chain link/follow/free round-trip OK', 13, 10, 0
msg_ctest_bad:        db 'exFAT: FAT chain link/follow/free round-trip FAILED', 13, 10, 0
msg_crtest_ok:        db 'exFAT: directory entry creation OK', 13, 10, 0
msg_crtest_bad:       db 'exFAT: directory entry creation FAILED', 13, 10, 0
exfat_crtest_name:    db 'CRTEST.TXT', 0
msg_wftest_ok:        db 'exFAT: exfat_write_file multi-cluster round-trip OK', 13, 10, 0
msg_wftest_bad:       db 'exFAT: exfat_write_file multi-cluster round-trip FAILED', 13, 10, 0
exfat_wtest_name:     db 'WFTEST.TXT', 0
msg_lntest_ok:        db 'exFAT: long filename (multi-FileName-entry) round-trip OK', 13, 10, 0
msg_lntest_bad:       db 'exFAT: long filename (multi-FileName-entry) round-trip FAILED', 13, 10, 0
exfat_lntest_name:    db 'THIS_IS_A_LONG_FILENAME_OVER_15_CHARS.TXT', 0
exfat_lntest_content: db 'long filename round-trip test payload'
exfat_lntest_content_len equ $ - exfat_lntest_content

msg_deltest_ok:       db 'exFAT: exfat_delete_file round-trip OK', 13, 10, 0
msg_deltest_bad:      db 'exFAT: exfat_delete_file round-trip FAILED', 13, 10, 0
exfat_deltest_name:   db 'DELTEST.TXT', 0

msg_rentest_ok:       db 'exFAT: exfat_rename_file round-trip OK', 13, 10, 0
msg_rentest_bad:      db 'exFAT: exfat_rename_file round-trip FAILED', 13, 10, 0
exfat_rentest_src:    db 'RENSRC.TXT', 0
exfat_rentest_dst:    db 'RENDST.TXT', 0
exfat_rentest_content: db 'rename round-trip test payload'
exfat_rentest_content_len equ $ - exfat_rentest_content

msg_trunctest_ok:     db 'exFAT: exfat_truncate_file round-trip OK', 13, 10, 0
msg_trunctest_bad:    db 'exFAT: exfat_truncate_file round-trip FAILED', 13, 10, 0
exfat_trunctest_name: db 'TRUNCTS2.TXT', 0
EXFAT_TRUNCTEST_SHORT_LEN equ 137

msg_apptest_ok:       db 'exFAT: exfat_append_file round-trip OK', 13, 10, 0
msg_apptest_bad:      db 'exFAT: exfat_append_file round-trip FAILED', 13, 10, 0
exfat_apptest_name:   db 'APPTEST2.TXT', 0
EXFAT_APPTEST_PART1_LEN equ 3000
EXFAT_APPTEST_PART2_LEN equ 5000
EXFAT_APPTEST_TOTAL_LEN equ EXFAT_APPTEST_PART1_LEN + EXFAT_APPTEST_PART2_LEN

msg_sched_ok:         db 'Scheduler: armed (main + 5 test tasks)', 13, 10, 0
msg_sched_bad:        db 'Scheduler: setup FAILED', 13, 10, 0
msg_sched_verify_ok:  db 'Scheduler: both test tasks made progress (preemption OK)', 13, 10, 0
msg_sched_verify_bad: db 'Scheduler: a test task made no progress (preemption FAILED)', 13, 10, 0
msg_term_verify_ok:   db 'Scheduler: task_exit and exception isolation both OK, zombies reaped', 13, 10, 0
msg_term_verify_bad:  db 'Scheduler: process termination verification FAILED', 13, 10, 0
msg_task_a:           db '[taskA]', 13, 10, 0
msg_task_b:           db '[taskB]', 13, 10, 0

test_task_a_counter: dq 0
test_task_b_counter: dq 0
test_task_exit_counter:  dq 0
test_task_crash_counter: dq 0
test_task_overflow_counter: dq 0
exfat_test_name:      db 'TEST.TXT', 0
exfat_marker:         db 'END-OF-FILE-MARKER'

align 8
exfat_find_result: times 36 db 0

align 4096
exfat_test_buf: times 65536 db 0
exfat_wtest_src:  times 6000 db 0
exfat_wtest_read: times 6000 db 0
exfat_lntest_read: times 64 db 0
exfat_rentest_read: times 64 db 0
exfat_trunctest_src:  times 5000 db 0
exfat_trunctest_read: times EXFAT_TRUNCTEST_SHORT_LEN db 0
exfat_apptest_part1: times EXFAT_APPTEST_PART1_LEN db 0
exfat_apptest_part2: times EXFAT_APPTEST_PART2_LEN db 0
exfat_apptest_read:  times EXFAT_APPTEST_TOTAL_LEN db 0

console_col:  dd 0
console_row:  dd 0
console_cols: dd 0
console_rows: dd 0
fb_text_color: dd 0xFFFFFFFF        ; current glyph foreground color, set by
                                     ; BASIX64's COLOR statement -- see
                                     ; basix_rt_set_text_color and fb_draw_char

shell_str_help:  db 'help', 0
shell_str_dir:   db 'dir', 0
shell_str_clear: db 'clear', 0
shell_str_type:  db 'type', 0
shell_str_write: db 'write', 0
shell_str_run:   db 'run', 0
shell_str_del:      db 'del', 0
shell_str_rename:   db 'rename', 0
shell_str_append:   db 'append', 0
shell_str_truncate: db 'truncate', 0
shell_str_mkdir:    db 'mkdir', 0
shell_str_open:      db 'open', 0
shell_str_up:        db 'up', 0
shell_str_tree:      db 'tree', 0
shell_str_move:       db 'move', 0
shell_str_rmdir:      db 'rmdir', 0

msg_shell_banner:      db 'arOS-X64 shell. Type HELP for commands.', 13, 10, 0
msg_shell_prompt:      db ' > : ', 0
msg_shell_help:        db 'Commands: HELP  DIR  TREE  TYPE <file>  WRITE <file> <text>  APPEND <file> <text>', 13, 10
                       db '  DEL <file>  RENAME <old> <new>  TRUNCATE <file> <size>  RUN <file.bas>  CLEAR', 13, 10
                       db '  MKDIR <name>  RMDIR <name>  OPEN <name>  UP  MOVE <file> <folder>', 13, 10, 0
msg_shell_unknown:     db 'Unknown command. Type HELP for a list.', 13, 10, 0
msg_shell_nl:          db 13, 10, 0
msg_shell_lf:          db 10, 0        ; single newline -- console_putc treats
                                        ; CR and LF as independent full
                                        ; newlines, so msg_shell_nl's CR+LF
                                        ; advances two lines; DIR/TREE want
                                        ; exactly one line per entry
msg_shell_type_usage:  db 'Usage: TYPE <filename>', 13, 10, 0
msg_shell_notfound:    db 'File not found.', 13, 10, 0
msg_shell_toobig:      db 'File too large to display.', 13, 10, 0
msg_shell_readfail:    db 'Error reading file.', 13, 10, 0
msg_shell_write_usage: db 'Usage: WRITE <filename> <text>', 13, 10, 0
msg_shell_write_ok:    db 'File written.', 13, 10, 0
msg_shell_write_fail:  db 'Write failed (name may already exist).', 13, 10, 0
msg_shell_run_usage:       db 'Usage: RUN <filename.bas>', 13, 10, 0
msg_shell_run_compilefail: db 'BASIX64 compile error.', 13, 10, 0
msg_shell_del_usage:  db 'Usage: DEL <filename>', 13, 10, 0
msg_shell_del_ok:     db 'File deleted.', 13, 10, 0
msg_shell_del_fail:   db 'Delete failed (file may not exist).', 13, 10, 0
msg_shell_rename_usage: db 'Usage: RENAME <oldname> <newname>', 13, 10, 0
msg_shell_rename_ok:    db 'File renamed.', 13, 10, 0
msg_shell_rename_fail:  db 'Rename failed (old name missing or new name already exists).', 13, 10, 0
msg_shell_append_usage: db 'Usage: APPEND <filename> <text>', 13, 10, 0
msg_shell_append_ok:    db 'Appended.', 13, 10, 0
msg_shell_append_fail:  db 'Append failed (file may not exist).', 13, 10, 0
msg_shell_truncate_usage: db 'Usage: TRUNCATE <filename> <newsize>', 13, 10, 0
msg_shell_truncate_ok:    db 'File truncated.', 13, 10, 0
msg_shell_truncate_fail:  db 'Truncate failed (file may not exist, or newsize > current size).', 13, 10, 0
msg_shell_dir_tag:        db '<DIR>', 0
msg_shell_dir_sep:        db '  ', 0
msg_shell_isdir:          db 'Cannot TYPE a directory.', 13, 10, 0
msg_shell_mkdir_usage:    db 'Usage: MKDIR <name>', 13, 10, 0
msg_shell_mkdir_ok:       db 'Directory created.', 13, 10, 0
msg_shell_mkdir_fail:     db 'MKDIR failed (name may already exist).', 13, 10, 0
msg_shell_open_usage:     db 'Usage: OPEN <name>', 13, 10, 0
msg_shell_open_notfound:  db 'Directory not found.', 13, 10, 0
msg_shell_open_notadir:   db 'Not a directory.', 13, 10, 0
msg_shell_open_toodeep:   db 'Too many nested directories.', 13, 10, 0
msg_shell_up_atroot:      db 'Already at the root directory.', 13, 10, 0
msg_shell_move_usage:     db 'Usage: MOVE <filename> <destination folder>', 13, 10, 0
msg_shell_move_ok:        db 'File moved.', 13, 10, 0
msg_shell_move_fail:      db 'Move failed (file/folder missing, folder is not a directory, or name already exists there).', 13, 10, 0
msg_shell_rmdir_usage:    db 'Usage: RMDIR <name>', 13, 10, 0
msg_shell_rmdir_ok:       db 'Directory deleted.', 13, 10, 0
msg_shell_rmdir_fail:     db 'RMDIR failed (not found, not a directory, or not empty).', 13, 10, 0
msg_shell_root_label:     db 'ROOT', 0
msg_shell_path_sep:       db ' > ', 0

shell_line_buf:     times SHELL_LINE_MAX db 0
shell_history:      times (SHELL_HISTORY_MAX * SHELL_LINE_MAX) db 0
shell_history_count: dd 0
shell_history_write: dd 0
shell_cmd_buf:      times 16 db 0
shell_arg_buf:       times 64 db 0
shell_arg_buf2:      times 64 db 0
align 8
shell_dir_entry:    times 16 db 0    ; FirstCluster(dd), FileAttributes(dd), DataLength(dq)

; -------------------------------------------------------------------------
; DIR/TREE column-layout state. A directory's entries must all be known
; before printing can start (column width depends on the widest entry,
; and TREE needs to know which entries are subdirectories to recurse
; into only *after* this level's grid is printed) -- so both commands
; buffer a whole directory's worth of entries into these per-depth
; arrays first. TREE needs one array slice per recursion depth (not a
; single shared buffer) since a child level's buffering would otherwise
; overwrite its parent's before the parent finishes printing/recursing;
; DIR always uses depth 0. Sized for EXFAT_CWD_MAX_DEPTH (16) levels.
; -------------------------------------------------------------------------
DIR_LIST_MAX_ENTRIES equ 32             ; per directory level
DIR_NAME_SLOT_LEN     equ 256           ; matches exfat_dir_list_next's own
                                         ; "256-byte buffer recommended" doc
DIR_TEXT_SLOT_LEN     equ 320           ; name slot + room for the widest
                                         ; possible "<DIR>  "/size prefix
DIR_TAG_FIELD_WIDTH   equ 7             ; <DIR>/filesize field, space-padded
                                         ; to this width so names line up
DIR_GUTTER            equ 5
; TIMES needs its count resolved at the point it's assembled, which
; EXFAT_CWD_MAX_DEPTH (defined in exfat.inc, %included after this data
; section) isn't yet -- unlike ordinary code operands, which resolve
; fine via NASM's multi-pass address resolution (see ATTR_DIRECTORY
; used the same way elsewhere in this file). A local mirror constant
; sidesteps that; the runtime depth-cap checks in shell_tree_walk still
; use EXFAT_CWD_MAX_DEPTH directly. Keep this in sync if that changes.
DIR_MAX_DEPTH         equ 16

align 8
shell_dir_col_names:    times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES * DIR_NAME_SLOT_LEN) db 0
shell_dir_col_text:     times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES * DIR_TEXT_SLOT_LEN) db 0
shell_dir_col_textlen:  times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES) dd 0
shell_dir_col_attrs:    times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES) dd 0
shell_dir_col_clusters: times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES) dd 0
shell_dir_col_sizes:    times (DIR_MAX_DEPTH * DIR_LIST_MAX_ENTRIES) dq 0
shell_dir_col_count:    times DIR_MAX_DEPTH dd 0
shell_dec_scratch:      times 24 db 0

basixtest_src: db 'LET x = 5', 10, 'PRINT x * 3 + 2', 10, 'GOSUB sub1', 10, 'END', 10, 'sub1:', 10, 'PRINT 1', 10, 'RETURN', 10, 0
msg_basixtest_ran:      db 'basixtest: compiled and ran OK', 13, 10, 0
msg_basixtest_bad:      db 'basixtest: COMPILE FAILED', 13, 10, 0

ahci_pci_addr:        dd 0    ; PCI config address of the AHCI controller
                              ; storage_init_and_test found, kept around
                              ; for the MSI proof-of-concept test
msg_hello:            db 'Hello, kernal!', 13, 10, 0
msg_ahci_not_found:   db 'AHCI: no controller found', 13, 10, 0
msg_ahci_no_port:     db 'AHCI: no active SATA port', 13, 10, 0
msg_ahci_read_fail:   db 'AHCI: LBA0 read failed/timed out', 13, 10, 0
msg_ahci_ok:          db 'AHCI: LBA0 read OK', 13, 10, 0
msg_sig_ok:           db 'boot signature 0xAA55 OK', 13, 10, 0
msg_sig_bad:          db 'boot signature mismatch', 13, 10, 0
msg_msi_ok:           db 'MSI: interrupt delivered via local APIC OK', 13, 10, 0
msg_msi_bad:          db 'MSI: armed but no interrupt arrived (FAILED)', 13, 10, 0
msg_msi_no_cap:       db 'MSI: device has no MSI capability, skipped', 13, 10, 0
msg_msi_skip:         db 'MSI: no AHCI controller active, skipped', 13, 10, 0

msg_nvme_not_found:   db 'NVMe: no controller found', 13, 10, 0
msg_nvme_init_fail:   db 'NVMe: controller/queue init failed', 13, 10, 0
msg_nvme_read_fail:   db 'NVMe: LBA0 read failed/timed out', 13, 10, 0
msg_nvme_ok:          db 'NVMe: LBA0 read OK', 13, 10, 0

msg_xhci_not_found:   db 'xHCI: no controller found', 13, 10, 0
msg_xhci_ok:          db 'xHCI: controller found (CAPLENGTH, HCIVERSION, HCSPARAMS1):', 13, 10, 0
msg_xhci_running:     db 'xHCI: controller reset and running OK', 13, 10, 0
msg_xhci_reset_fail:  db 'xHCI: reset/bring-up FAILED (timed out)', 13, 10, 0
msg_xhci_noop_ok:     db 'xHCI: No-Op command completion event received OK', 13, 10, 0
msg_xhci_noop_fail:   db 'xHCI: No-Op command FAILED (no completion event)', 13, 10, 0
msg_xhci_ports:       db 'xHCI: ports with a device connected:', 13, 10, 0
msg_xhci_slot_ok:     db 'xHCI: Enable Slot OK, slot ID:', 13, 10, 0
msg_xhci_slot_fail:   db 'xHCI: Enable Slot FAILED', 13, 10, 0
msg_xhci_setup_ok:    db 'xHCI: device slot context/EP0 ring setup OK', 13, 10, 0
msg_xhci_setup_fail:  db 'xHCI: device slot context/EP0 ring setup FAILED', 13, 10, 0
msg_xhci_addr_ok:     db 'xHCI: Address Device OK', 13, 10, 0
msg_xhci_addr_fail:   db 'xHCI: Address Device FAILED', 13, 10, 0
msg_xhci_desc_ok:     db 'xHCI: GET_DESCRIPTOR(Device) OK, idVendor/idProduct:', 13, 10, 0
msg_xhci_desc_fail:   db 'xHCI: GET_DESCRIPTOR(Device) FAILED', 13, 10, 0
msg_xhci_config_ok:   db 'xHCI: GET_DESCRIPTOR(Config) OK, bulk in/out EP index, in/out max packet:', 13, 10, 0
msg_xhci_config_fail: db 'xHCI: GET_DESCRIPTOR(Config) FAILED (no BOT mass-storage interface found)', 13, 10, 0
msg_xhci_setcfg_ok:   db 'xHCI: SET_CONFIGURATION OK', 13, 10, 0
msg_xhci_setcfg_fail: db 'xHCI: SET_CONFIGURATION FAILED', 13, 10, 0
msg_xhci_configep_ok:   db 'xHCI: Configure Endpoint (Bulk IN/OUT) OK', 13, 10, 0
msg_xhci_configep_fail: db 'xHCI: Configure Endpoint (Bulk IN/OUT) FAILED', 13, 10, 0
msg_xhci_inquiry_ok:    db 'xHCI: USB MSD SCSI INQUIRY OK, vendor ID byte0:', 13, 10, 0
msg_xhci_inquiry_fail:  db 'xHCI: USB MSD SCSI INQUIRY FAILED', 13, 10, 0
msg_xhci_readcap_ok:    db 'xHCI: USB MSD READ CAPACITY(10) OK, last LBA/sector size:', 13, 10, 0
msg_xhci_readcap_fail:  db 'xHCI: USB MSD READ CAPACITY(10) FAILED', 13, 10, 0
msg_xhci_msdread_ok:    db 'xHCI: USB MSD READ(10) sector 0 OK', 13, 10, 0
msg_xhci_msdread_fail:  db 'xHCI: USB MSD READ(10) sector 0 FAILED', 13, 10, 0

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
%include "logo.inc"
%include "idt.inc"
%include "sched.inc"
%include "pic.inc"
%include "paging.inc"
%include "apic.inc"
%include "pmm.inc"
%include "vmm.inc"
%include "kheap.inc"
%include "pci.inc"
%include "ahci.inc"
%include "nvme.inc"
%include "xhci.inc"
%include "storage.inc"
%include "exfat.inc"
%include "keyboard.inc"
%include "basix_lexer.inc"
%include "basix_codegen.inc"
%include "basix_symbols.inc"
%include "basix_runtime.inc"
%include "basix_parser.inc"
