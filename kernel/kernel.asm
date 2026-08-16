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
    mov [rel basix_boot_info_ptr], rcx  ; stashed once so any fresh task
                                         ; (task_create's initial GPR frame
                                         ; is all zeros except RDI -- no
                                         ; inherited RBX) can still recover
                                         ; boot_info* -- see
                                         ; basix_task_launch_wrapper
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
    call mouse_install
    call paging_init
    call lapic_init
    call basix_fpu_init

    mov rcx, rbx                        ; boot_info
    call pmm_init

    call basix_heap_init
    call basix_slot_init_all

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

    ; Self-healing, like every other exFAT smoke-test further below
    ; (crtest/wftest/lntest/etc all "create it if this is the first
    ; boot against this VHD, then verify either way") -- this one used
    ; to be the ONE exception, a hard prerequisite that TEST.TXT
    ; already exist with an exact size and trailing marker, no
    ; fallback creation path at all. On any VHD where that one
    ; specific file was ever missing or resized (e.g. a shared dev/
    ; test VHD used for lots of other things too), this printed a
    ; scary "exFAT: mount, find, or read FAILED" even though the real
    ; exFAT driver was working completely fine -- confirmed misleading
    ; in exactly that scenario this session.
    lea rcx, [rel exfat_test_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .exfat_have_file

    ; exfat_write_file needs the allocation bitmap mounted first (to
    ; find a free cluster to write into) -- normally done by btest,
    ; much further down in this same boot sequence, too late for this
    ; earlier self-heal write. Safe/idempotent to call again there.
    call exfat_bitmap_mount
    test eax, eax
    jz .exfat_bad

    lea rdi, [rel exfat_test_buf]
    xor ecx, ecx
.exfat_gen_fill:
    cmp ecx, 5000 - 18
    jge .exfat_gen_fill_done
    mov eax, ecx
    and eax, 0xFF
    mov [rdi+rcx], al
    inc ecx
    jmp .exfat_gen_fill
.exfat_gen_fill_done:
    lea rsi, [rel exfat_marker]
    lea rdi, [rel exfat_test_buf + 5000 - 18]
    mov ecx, 18
.exfat_gen_marker:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec ecx
    jnz .exfat_gen_marker

    lea rcx, [rel exfat_test_name]
    lea r8, [rel exfat_test_buf]
    mov r9, 5000
    call exfat_write_file
    test eax, eax
    jz .exfat_bad

    lea rcx, [rel exfat_test_name]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .exfat_bad
.exfat_have_file:

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
    mov qword [r15+TCB_BACKBUF_PTR], 0
    mov dword [r15+TCB_BACKBUF_W], 0
    mov dword [r15+TCB_BACKBUF_H], 0

    mov rcx, 512
    call kmalloc                        ; kmalloc never touches r15
    test rax, rax
    jz .sched_bad
    fxsave [rax]
    mov [r15+TCB_FPU], rax

    mov rcx, 256
    call kmalloc
    test rax, rax
    jz .sched_bad
    mov [r15+TCB_KBD_BUF_PTR], rax
    mov dword [r15+TCB_KBD_HEAD], 0
    mov dword [r15+TCB_KBD_TAIL], 0
    mov qword [r15+TCB_LAUNCH_ARG_BUF_PTR], 0
    mov dword [r15+TCB_LAUNCH_ARG_LEN], 0
    mov dword [r15+TCB_DIRTY_VALID], 0
    ; Main never has a window (it's the desktop/shell layer, drawn
    ; full-screen with no chrome) -- TCB_WIN_W left/kept 0 is what
    ; every "is this task windowed?" check (basix_rt_ensure_backbuf,
    ; SCREENW/H, TEXTCOLS/ROWS, compositor_task) keys off of.
    mov dword [r15+TCB_WIN_X], 0
    mov dword [r15+TCB_WIN_Y], 0
    mov dword [r15+TCB_WIN_W], 0
    mov dword [r15+TCB_WIN_H], 0
    mov dword [r15+TCB_WIN_TITLE_LEN], 0
    mov dword [r15+TCB_WIN_CLOSE_REQ], 0
    mov dword [r15+TCB_PEND_VALID], 0

    ; Main starts out as the sole keyboard-focused task and the sole
    ; (back-most) z-order entry -- R2's z-order list (basix_zorder,
    ; see compositor_task) always has main seeded at index 0 so it's
    ; never empty; LAUNCH pushes every concurrent GUI child on top of
    ; it and pops them back off on exit.
    mov [rel basix_kbd_focus_task], r15
    mov [rel basix_zorder], r15
    mov dword [rel basix_zorder_count], 1

    ; R1: the compositor task -- see compositor_task's own comment.
    ; Needs boot_info* (RBX) to reach the real framebuffer, which
    ; task_create can't hand it implicitly (a fresh task's initial GPR
    ; frame is all zeros except RDI), so it's passed explicitly as the
    ; task_create argument and moved into RBX at the task's own start.
    mov rcx, compositor_task
    mov rdx, rbx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r12, rax                        ; r12 = compositor's TCB

    mov rcx, test_task_a
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r13, rax                        ; r13 = task A's TCB
    mov [rel test_task_a_tcb], rax      ; stashed so term_verify_done
                                         ; (much later, after every
                                         ; intervening exFAT self-test
                                         ; has long since clobbered
                                         ; r13/r14 for its own use) can
                                         ; still terminate A once its
                                         ; job -- proving the rest of
                                         ; the system stays alive while
                                         ; OTHER tasks terminate -- is
                                         ; done, instead of leaving it
                                         ; spinning and spamming serial
                                         ; forever (see test_task_a's
                                         ; own comment).

    mov rcx, test_task_b
    xor edx, edx
    mov r8, 16384
    call task_create
    test rax, rax
    jz .sched_bad
    mov r14, rax                        ; r14 = task B's TCB
    mov [rel test_task_b_tcb], rax      ; see test_task_a_tcb's comment

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

    mov [r15+TCB_NEXT], r12             ; ring: main -> compositor -> A -> B -> exit -> crash -> overflow -> main
    mov [r12+TCB_NEXT], r13
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
    ; test_task_a/b have now done their job -- every verification that
    ; needed them alive and still counting (both the preemption check
    ; above and the "rest of the system survives other tasks
    ; terminating" check just above) is done. Originally they just
    ; looped forever after this point (see their own comment) -- pure
    ; leftover R0-era bring-up scaffolding, but a real, ongoing cost:
    ; permanently occupying a ready-ring slot and spamming serial with
    ; "[taskA]"/"[taskB]" for the entire remaining lifetime of the
    ; system, competing for CPU with actual work (compositor_task,
    ; LAUNCHed GUI programs) on every single round-robin cycle from
    ; here on. Terminate them exactly the way task_exit terminates the
    ; CALLING task (TCB_STATE_TERMINATED) -- safe to do to another,
    ; not-currently-running task from here, since sched_pick_next
    ; already splices out and zombie-queues any task it finds in this
    ; state on its own, regardless of who set it.
    mov rax, [rel test_task_a_tcb]
    test rax, rax
    jz .no_a_term
    mov dword [rax+TCB_STATE], TCB_STATE_TERMINATED
.no_a_term:
    mov rax, [rel test_task_b_tcb]
    test rax, rax
    jz .no_b_term
    mov dword [rax+TCB_STATE], TCB_STATE_TERMINATED
.no_b_term:
    call sched_reap_zombies

    ; Smoke-test the BASIX64 compiler: compile and run a trivial program.
    ; This only proves the pipeline works end to end (compiles without
    ; error and executes without crashing) -- RUN in the shell is the
    ; real way to see a program's output.
    xor ecx, ecx
    call basix_compile_slot_begin       ; use slot 0 for this boot-time
                                         ; smoke test -- basix_compile
                                         ; itself no longer resolves an
                                         ; active code buffer on its own
    lea rcx, [rel basixtest_src]
    call basix_compile
    test eax, eax
    jz .basixtest_bad
    mov rax, [rel basix_active_code_buf_ptr]
    call rax
    lea rcx, [rel msg_basixtest_ran]
    call serial_puts
    jmp .basixtest_done
.basixtest_bad:
    lea rcx, [rel msg_basixtest_bad]
    call serial_puts
.basixtest_done:

    ; Ensure APPS/ exists at exFAT root -- COMPILE's default landing
    ; spot for .axb output, so it's always there without the user
    ; having to MKDIR it by hand first. Idempotent: exfat_cwd_cluster
    ; is still root here (nothing before this point OPENs into a
    ; subdirectory), so a plain root-relative find/create is enough.
    lea rcx, [rel apps_dirname]
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .apps_dir_ready
    lea rcx, [rel apps_dirname]
    call exfat_create_dir
.apps_dir_ready:

    call shell_main                     ; never returns

.hang:
    hlt
    jmp .hang

; -------------------------------------------------------------------------
; R2: z-order. basix_zorder is a back-to-front list of "windowed" task
; pointers (index 0 = back-most, [count-1] = front-most/topmost); it's
; the compositor's iteration order, and its front entry is always kept
; in sync with basix_kbd_focus_task (the invariant "topmost == focused"
; -- there's no independent click-to-focus routing yet, since no task
; has real window bounds until R4/R5's chrome/positioning work, so
; "which window is in front" and "which window gets the keyboard" are
; the same question for now). Seeded with just main at boot (see
; entry:); LAUNCH (basix_rt_launch) pushes a new child to the front,
; and the child removes itself (basix_zorder_remove) right before
; task_exit -- see basix_task_launch_wrapper.
; -------------------------------------------------------------------------
BASIX_ZORDER_MAX equ BASIX_SLOT_COUNT + 1   ; every execution slot, plus main

; -------------------------------------------------------------------------
; basix_zorder_push_front: RCX = task ptr to add as the new front
; (topmost, keyboard-focused) entry. Assumes RCX isn't already present
; -- true for every caller today (LAUNCH always creates a brand new
; TCB via task_create, never reuses one already in the list).
; -------------------------------------------------------------------------
basix_zorder_push_front:
    push rax
    mov eax, [rel basix_zorder_count]
    cmp eax, BASIX_ZORDER_MAX
    jae .full                           ; shouldn't happen (sized for
                                         ; every slot + main); drop
                                         ; silently rather than overrun
    mov [rel basix_zorder + rax*8], rcx
    inc eax
    mov [rel basix_zorder_count], eax
.full:
    mov [rel basix_kbd_focus_task], rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_zorder_remove: RCX = task ptr to remove from the z-order list
; (called by an exiting task on itself). Shifts every later entry down
; by one slot, then re-derives basix_kbd_focus_task from whatever is
; now the front entry -- main (index 0) is never removed, so the list
; can't go empty.
; -------------------------------------------------------------------------
basix_zorder_remove:
    push rax
    push rcx
    push rdx
    push r8
    mov eax, [rel basix_zorder_count]
    xor edx, edx                        ; edx = search index
.search:
    cmp edx, eax
    jge .not_found
    cmp [rel basix_zorder + rdx*8], rcx
    je .found
    inc edx
    jmp .search
.found:
    mov r8d, edx                        ; r8d = shift-write cursor
.shift:
    mov edx, r8d
    inc edx                             ; edx = shift-read cursor (one ahead)
    cmp edx, eax
    jge .shift_done
    mov rcx, [rel basix_zorder + rdx*8]
    mov [rel basix_zorder + r8*8], rcx
    inc r8d
    jmp .shift
.shift_done:
    dec eax
    mov [rel basix_zorder_count], eax
    ; R4: a removed entry may have had chrome (title bar/border) drawn
    ; around it -- like a drag, closing/exiting leaves a screen region
    ; nothing's own dirty rect reflects (nothing drew there, the
    ; window just stopped existing), so force one full recomposite
    ; pass to paint over whatever's now exposed there.
    mov dword [rel basix_wm_force_full], 1
    test eax, eax
    jz .out                             ; shouldn't happen (main never
                                         ; removed) -- leave focus alone
    mov rdx, [rel basix_zorder + rax*8 - 8]
    mov [rel basix_kbd_focus_task], rdx
.not_found:
.out:
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_zorder_bring_front: RCX = task ptr already present somewhere in
; the z-order. Moves it to the front (topmost, keyboard-focused) --
; real click-to-focus, see wm_tick's own comment on when this gets
; called. Just basix_zorder_remove followed by basix_zorder_push_front
; on the same task: remove already preserves RCX across its own call
; (pushed/popped internally, same as every other register it uses as
; scratch) and re-derives a temporary focus/order that push_front then
; correctly overrides, so no extra bookkeeping is needed here.
; -------------------------------------------------------------------------
basix_zorder_bring_front:
    call basix_zorder_remove
    call basix_zorder_push_front
    ret

basix_zorder: times BASIX_ZORDER_MAX dq 0
basix_zorder_count: dd 0

; -------------------------------------------------------------------------
; R4: window chrome + a minimal window manager. Chrome (title bar +
; text + close gadget + border) is drawn straight to the REAL
; framebuffer by the compositor itself, OUTSIDE every windowed task's
; own back buffer -- a program never draws its own title bar (unlike
; Workbench's own hand-drawn internal "window" before this, which is
; unrelated: that's Workbench's own in-app UI, not OS-level chrome).
; Only the frontmost (topmost, z-order[count-1]) windowed task's
; chrome is interactive (click-drag the title bar to move, click the
; close gadget) -- consistent with R2's existing "topmost == focused"
; invariant; there's still no real click-to-focus-a-background-window
; routing.
; -------------------------------------------------------------------------
; Palette + bevel style sampled from real Workbench 2.04 screenshots
; (guidebookgallery.org/screenshots/amigaos204) -- a flat, solid blue
; title bar with BLACK text (not white), light-gray gadget boxes, and
; a raised 2px bevel (light top/left, dark bottom/right) around both
; the window frame and each gadget box. This is the real Kickstart
; 2.0+ "3D look": there's no gradient/gloss anywhere in 2.04 itself --
; every surface is a flat fill, and all the depth comes from that one
; consistent light/dark edge-pair convention.
WM_TITLE_H     equ 22   ; title bar height, px
WM_BORDER      equ 4    ; border thickness, px (matches wm_fb_bevel_rect's
                         ; own WM_EDGE bevel-line thickness below, so the
                         ; frame's raised edge fills the whole border,
                         ; not just a thin line inside a wider gray margin)
WM_EDGE        equ 2    ; bevel light/dark line thickness, px -- real
                         ; Workbench 2.04 frames read as thick/embossed,
                         ; not hairline; widened from 1px for that
WM_CLOSE_SIZE  equ 16   ; close gadget square size, px -- matches
                         ; font8x16's own glyph height exactly (see the
                         ; close-gadget 'X' glyph below), so it sits
                         ; fully inside the box with no overflow/overlap
                         ; onto the box's own bevel edges
; Vertical margin that centers a 16px-tall glyph cell (title text, and
; the close gadget's own WM_CLOSE_SIZE=16 box) inside the title bar's
; own VISIBLE fill height (WM_TITLE_H minus the WM_EDGE border strip
; along its top -- see the title-fill/close-gadget draw code, which
; both need to measure their vertical position from the fill's own
; top, not the window's outer frame top, or they end up offset by
; WM_BORDER extra px from where the bar actually visually starts --
; a real bug found this way: the close gadget's own box bled 1px past
; the bottom of the title bar because of exactly that miscalculation).
WM_TITLE_TEXT_MARGIN equ (WM_TITLE_H - WM_EDGE - 16) / 2
WM_TITLE_BG    equ 0x6F87C6
WM_TITLE_FG    equ 0x000000
WM_BODY_BG     equ 0xAAAAAA   ; gadget box / border fill -- classic
                               ; Amiga Workbench gray, matches
                               ; workbench.bas's own desktop color
WM_BEVEL_LIGHT equ 0xFFFFFF
WM_BEVEL_DARK  equ 0x000000
WM_CLOSE_FG    equ 0x000000

WM_SCROLLBAR_W equ 16   ; scrollbar track/arrow-gadget/resize-gadget
                         ; thickness, px -- matches WM_CLOSE_SIZE so
                         ; every chrome gadget in this window system
                         ; reads as the same size. Purely decorative
                         ; for now (see the scrollbar/resize-gadget
                         ; draw block below, right before content_blit)
                         ; -- no scroll or resize behavior wired up
                         ; yet, just the real Workbench 2.04 visual
                         ; elements (arrow gadgets, track, thumb,
                         ; corner resize gadget) on every open window.

basix_wm_dragging:    dq 0     ; task ptr currently being title-bar-
                                ; dragged, or 0
basix_wm_drag_off_x:  dd 0
basix_wm_drag_off_y:  dd 0
basix_wm_prev_btn:    dd 0     ; last pass's mouse_btn, for edge
                                ; detection (press/release)
basix_wm_force_full:  dd 0     ; set for one pass when a drag moved a
                                ; window -- see wm_tick

; -------------------------------------------------------------------------
; wm_fb_fill_rect: RCX=x0, RDX=y0, R8=w, R9=h, R10d=color (packed
; 32-bit pixel). Fills a rect directly on the REAL framebuffer,
; clipped to its own bounds. RBX must be boot_info*.
; -------------------------------------------------------------------------
wm_fb_fill_rect:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r11
    push r12

    mov r11d, ecx                       ; x0
    mov r12d, edx                       ; y0
    add r8d, r11d                       ; x1 = x0+w
    add r9d, r12d                       ; y1 = y0+h

    cmp r11d, 0
    jge .x0_ok
    xor r11d, r11d
.x0_ok:
    cmp r12d, 0
    jge .y0_ok
    xor r12d, r12d
.y0_ok:
    cmp r8d, [rbx+FB_WIDTH]
    jle .x1_ok
    mov r8d, [rbx+FB_WIDTH]
.x1_ok:
    cmp r9d, [rbx+FB_HEIGHT]
    jle .y1_ok
    mov r9d, [rbx+FB_HEIGHT]
.y1_ok:
    cmp r11d, r8d
    jge .out
    cmp r12d, r9d
    jge .out

    mov esi, r12d                       ; row
.row_loop:
    cmp esi, r9d
    jge .out

    mov eax, esi
    imul eax, [rbx+FB_STRIDE]
    add eax, r11d
    mov rdi, [rbx+FB_BASE]
    lea rdi, [rdi + rax*4]

    mov ecx, r8d
    sub ecx, r11d                       ; span width
    mov eax, r10d
.col_loop:
    test ecx, ecx
    jz .row_next
    mov [rdi], eax
    add rdi, 4
    dec ecx
    jmp .col_loop

.row_next:
    inc esi
    jmp .row_loop

.out:
    pop r12
    pop r11
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; wm_fb_bevel_rect: RCX=x0, RDX=y0, R8=w, R9=h, R10d=fill color. Fills
; the rect (via wm_fb_fill_rect), then outlines it with the real
; Workbench 2.0+ "3D look": a 1px WM_BEVEL_LIGHT line along the top and
; left edges, WM_BEVEL_DARK along the bottom and right -- reads as a
; raised/embossed surface. Used for every gadget box and the window
; frame itself; nowhere in real 2.04 chrome is there a gradient, only
; this one flat-fill-plus-bevel-pair convention repeated everywhere.
; -------------------------------------------------------------------------
; Note: wm_fb_fill_rect preserves RCX/RDX/R8/R9 across its own call
; (see its own push/pop list) -- but only if THIS function doesn't
; overwrite them itself first. Each edge below needs a DIFFERENT w/h
; (the true w or h for one dimension, a flat 1 for the other), so x0/
; y0/w/h are reloaded from the stack before every single edge rather
; than threaded through registers across calls. R10 (color) is never
; preserved by wm_fb_fill_rect, but every call below sets it fresh
; anyway.
wm_fb_bevel_rect:
    push rcx
    push rdx
    push r8
    push r9

    call wm_fb_fill_rect                ; fill, using the original args

    ; top edge: (x0, y0, w, WM_EDGE)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8, [rsp+8]
    mov r9d, WM_EDGE
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_fill_rect

    ; left edge: (x0, y0, WM_EDGE, h)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8d, WM_EDGE
    mov r9, [rsp+0]
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_fill_rect

    ; bottom edge: (x0, y0+h-WM_EDGE, w, WM_EDGE)
    mov rcx, [rsp+24]
    mov edx, [rsp+16]
    add edx, [rsp+0]
    sub edx, WM_EDGE
    mov r8, [rsp+8]
    mov r9d, WM_EDGE
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect

    ; right edge: (x0+w-WM_EDGE, y0, WM_EDGE, h)
    mov ecx, [rsp+24]
    add ecx, [rsp+8]
    sub ecx, WM_EDGE
    mov rdx, [rsp+16]
    mov r8d, WM_EDGE
    mov r9, [rsp+0]
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect

    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; wm_fb_bevel_rect_inset: same args/fill as wm_fb_bevel_rect, but with
; light/dark edges swapped (dark top/left, light bottom/right) --
; reads as pressed-in rather than raised. Used for the scrollbar
; thumbs (see the scrollbar draw block below): a raised bevel there
; would look like another gadget button, but a real Workbench thumb
; sits inside the sunken track, not on top of it.
; -------------------------------------------------------------------------
wm_fb_bevel_rect_inset:
    push rcx
    push rdx
    push r8
    push r9

    call wm_fb_fill_rect                ; fill, using the original args

    ; top edge: (x0, y0, w, WM_EDGE)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8, [rsp+8]
    mov r9d, WM_EDGE
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect

    ; left edge: (x0, y0, WM_EDGE, h)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8d, WM_EDGE
    mov r9, [rsp+0]
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect

    ; bottom edge: (x0, y0+h-WM_EDGE, w, WM_EDGE)
    mov rcx, [rsp+24]
    mov edx, [rsp+16]
    add edx, [rsp+0]
    sub edx, WM_EDGE
    mov r8, [rsp+8]
    mov r9d, WM_EDGE
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_fill_rect

    ; right edge: (x0+w-WM_EDGE, y0, WM_EDGE, h)
    mov ecx, [rsp+24]
    add ecx, [rsp+8]
    sub ecx, WM_EDGE
    mov rdx, [rsp+16]
    mov r8d, WM_EDGE
    mov r9, [rsp+0]
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_fill_rect

    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; wm_fb_outline_rect: RCX=x0, RDX=y0, R8=w, R9=h, R10D=color. Draws a
; 1px unfilled rectangle outline (no interior fill, unlike
; wm_fb_bevel_rect) -- used to mark the scrollbar track boundaries
; (see the scrollbar draw block below) so the area a thumb can move
; within reads as a distinct groove rather than blending into the
; surrounding gray gadget-box fill.
; -------------------------------------------------------------------------
wm_fb_outline_rect:
    push rcx
    push rdx
    push r8
    push r9

    ; top edge: (x0, y0, w, 1)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8, [rsp+8]
    mov r9d, 1
    call wm_fb_fill_rect

    ; bottom edge: (x0, y0+h-1, w, 1)
    mov rcx, [rsp+24]
    mov edx, [rsp+16]
    add edx, [rsp+0]
    sub edx, 1
    mov r8, [rsp+8]
    mov r9d, 1
    call wm_fb_fill_rect

    ; left edge: (x0, y0, 1, h)
    mov rcx, [rsp+24]
    mov rdx, [rsp+16]
    mov r8d, 1
    mov r9, [rsp+0]
    call wm_fb_fill_rect

    ; right edge: (x0+w-1, y0, 1, h)
    mov ecx, [rsp+24]
    add ecx, [rsp+8]
    sub ecx, 1
    mov rdx, [rsp+16]
    mov r8d, 1
    mov r9, [rsp+0]
    call wm_fb_fill_rect

    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; wm_fb_draw_arrow_{up,down,left,right}: RCX=box x0, RDX=box y0, R10D=
; color. Draws a small solid triangle glyph inside a WM_SCROLLBAR_W
; (16px) square box, via four hardcoded RECT strips rather than a
; generic per-row loop -- the box size is a fixed constant, so there's
; no real variability to handle generically, and four literal strips
; is far less to get wrong than a parameterized triangle rasterizer.
; Used for the four scrollbar arrow gadgets below (purely decorative,
; matching real Workbench 2.04's caret glyphs -- no scroll behavior).
; -------------------------------------------------------------------------
; Each of the four uses rbx=box x0, r14=box y0, r15=color -- all three
; are callee-saved by wm_fb_fill_rect (see its own push/pop list), so
; they survive every one of the four calls below untouched with no
; per-call save/restore needed, unlike r8/r9/r10 (fill_rect's own
; w/h/color args), which it's free to clobber as scratch internally.
; -------------------------------------------------------------------------
; Real bug found and fixed here (worth remembering if this ever needs
; touching again): each of these originally stashed "box x0" in EBX
; before calling wm_fb_fill_rect -- but RBX must stay boot_info*
; everywhere in this compositor (see wm_fb_fill_rect's own "RBX must
; be boot_info*" doc comment; it dereferences [rbx+FB_WIDTH] etc.).
; Overwriting it with a small window-coordinate value made every
; fill_rect call inside these functions read FB_WIDTH/HEIGHT/STRIDE/
; BASE through a bogus pointer -- no crash (the address happened to be
; mapped), just silently wrong/empty clip bounds, so the arrow gadget
; BOXES rendered fine (drawn by the CALLER, which still had a valid
; RBX) while the glyphs inside them never appeared at all. Confirmed
; via byte-for-byte verification of the compiled call site and
; function prologue (both correct) before finding this. Fixed by
; using r13 for box x0 instead -- untouched by wm_fb_fill_rect (see
; its own push list), same as r14/r15 already used here for box y0
; and color.
; -------------------------------------------------------------------------
wm_fb_draw_arrow_up:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r13
    push r14
    push r15
    mov r13d, ecx                       ; box x0
    mov r14d, edx                       ; box y0
    mov r15d, r10d                      ; color

    mov ecx, r13d
    add ecx, 7
    mov edx, r14d
    add edx, 4
    mov r8d, 2
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 6
    mov edx, r14d
    add edx, 6
    mov r8d, 4
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 5
    mov edx, r14d
    add edx, 8
    mov r8d, 6
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 4
    mov edx, r14d
    add edx, 10
    mov r8d, 8
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    pop r15
    pop r14
    pop r13
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; Same footprint as wm_fb_draw_arrow_up (8x8, margin 4 in the 16x16
; box) so up/down glyphs read the same size as the left/right ones --
; an earlier version used a flatter 8x4 footprint for up/down only,
; which looked visibly smaller/squatter next to left/right's 8x8 (real
; Workbench 2.04's caret glyphs are the same size in every direction).
wm_fb_draw_arrow_down:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r13
    push r14
    push r15
    mov r13d, ecx
    mov r14d, edx
    mov r15d, r10d

    mov ecx, r13d
    add ecx, 4
    mov edx, r14d
    add edx, 4
    mov r8d, 8
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 5
    mov edx, r14d
    add edx, 6
    mov r8d, 6
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 6
    mov edx, r14d
    add edx, 8
    mov r8d, 4
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 7
    mov edx, r14d
    add edx, 10
    mov r8d, 2
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    pop r15
    pop r14
    pop r13
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

wm_fb_draw_arrow_left:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r13
    push r14
    push r15
    mov r13d, ecx
    mov r14d, edx
    mov r15d, r10d

    mov ecx, r13d
    add ecx, 4
    mov edx, r14d
    add edx, 7
    mov r8d, 2
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 6
    mov edx, r14d
    add edx, 6
    mov r8d, 2
    mov r9d, 4
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 8
    mov edx, r14d
    add edx, 5
    mov r8d, 2
    mov r9d, 6
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 10
    mov edx, r14d
    add edx, 4
    mov r8d, 2
    mov r9d, 8
    mov r10d, r15d
    call wm_fb_fill_rect

    pop r15
    pop r14
    pop r13
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

wm_fb_draw_arrow_right:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r13
    push r14
    push r15
    mov r13d, ecx
    mov r14d, edx
    mov r15d, r10d

    mov ecx, r13d
    add ecx, 4
    mov edx, r14d
    add edx, 4
    mov r8d, 2
    mov r9d, 8
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 6
    mov edx, r14d
    add edx, 5
    mov r8d, 2
    mov r9d, 6
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 8
    mov edx, r14d
    add edx, 6
    mov r8d, 2
    mov r9d, 4
    mov r10d, r15d
    call wm_fb_fill_rect

    mov ecx, r13d
    add ecx, 10
    mov edx, r14d
    add edx, 7
    mov r8d, 2
    mov r9d, 2
    mov r10d, r15d
    call wm_fb_fill_rect

    pop r15
    pop r14
    pop r13
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; wm_cursor_bitmap: classic arrow-pointer shape, 16 rows, one 16-bit
; mask per row (MSB = leftmost pixel), same row-major format as
; font8x16's glyphs but word-wide instead of byte-wide. Replaces the
; old plain-white-square cursor, which was drawn as part of
; workbench.bas's own content (see its own comment on why that hid the
; cursor behind any window in front of it) -- this is a real
; compositor-level overlay instead, see wm_fb_draw_cursor below.
; -------------------------------------------------------------------------
wm_cursor_bitmap:
    dw 1000000000000000b
    dw 1100000000000000b
    dw 1110000000000000b
    dw 1111000000000000b
    dw 1111100000000000b
    dw 1111110000000000b
    dw 1111111000000000b
    dw 1111111100000000b
    dw 1111111110000000b
    dw 1111100000000000b
    dw 1101100000000000b
    dw 1000110000000000b
    dw 0000110000000000b
    dw 0000011000000000b
    dw 0000011000000000b
    dw 0000000000000000b

WM_CURSOR_FG equ 0x000000
WM_CURSOR_W  equ 16
WM_CURSOR_H  equ 16

; -------------------------------------------------------------------------
; wm_fb_draw_cursor: RCX=x, RDX=y (top-left of the 16x16 cursor cell,
; NOT a hotspot offset -- the shape's own "point" is already at its
; top-left corner). Paints only SET bits, leaving the rest alone, same
; convention as wm_fb_draw_glyph -- straight to the real framebuffer,
; clipped to screen bounds. RBX must be boot_info*.
; -------------------------------------------------------------------------
wm_fb_draw_cursor:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r14

    mov r9d, ecx                        ; r9d = x0
    mov r10d, edx                       ; r10d = y0
    lea rsi, [rel wm_cursor_bitmap]
    xor r8d, r8d                        ; r8d = row index
.row_loop:
    cmp r8d, WM_CURSOR_H
    jge .done
    mov edi, r10d
    add edi, r8d
    cmp edi, 0
    jl .row_next
    cmp edi, [rbx+FB_HEIGHT]
    jge .row_next

    ; r11d holds this row's 16-bit mask for the WHOLE column loop below
    ; -- must NOT be reused as scratch anywhere in that loop (an
    ; earlier version of this function used ECX/RCX for both this AND
    ; the pixel-write address math a few lines down, which clobbered
    ; the mask after the first set bit in any row -- every row after
    ; its own first pixel then tested garbage for the rest of its
    ; columns, producing scattered/sparse pixels instead of a solid
    ; shape).
    movzx r11d, word [rsi + r8*2]
    xor edx, edx                        ; edx = column index within row
.col_loop:
    cmp edx, WM_CURSOR_W
    jge .row_next
    mov eax, r11d
    mov r14d, 15
    sub r14d, edx
    bt eax, r14d
    jnc .col_next

    mov eax, r9d
    add eax, edx
    cmp eax, 0
    jl .col_next
    cmp eax, [rbx+FB_WIDTH]
    jge .col_next

    push rax
    mov eax, edi
    imul eax, [rbx+FB_STRIDE]
    pop rcx
    add eax, ecx
    mov rcx, [rbx+FB_BASE]
    mov dword [rcx + rax*4], WM_CURSOR_FG

.col_next:
    inc edx
    jmp .col_loop
.row_next:
    inc r8d
    jmp .row_loop
.done:
    pop r14
    pop r11
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
; wm_fb_draw_glyph: AL=char code, R9D=x, R10D=y, R11D=color. Same
; glyph format/semantics as basix_draw_glyph (basix_runtime.inc) --
; paints only SET pixels, leaving the rest alone -- but targets the
; REAL framebuffer (FB_BASE/FB_STRIDE) instead of a task's back
; buffer, since chrome text isn't drawn by any task. RBX must be
; boot_info*.
; -------------------------------------------------------------------------
wm_fb_draw_glyph:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r15

    mov r15, rbx                        ; save boot_info* (rbx is
                                         ; about to become scratch,
                                         ; same trick basix_draw_glyph
                                         ; itself uses)

    sub eax, 32
    cmp eax, 95
    jae .out
    shl eax, 4
    lea rsi, [rel font8x16]
    add rsi, rax

    xor ecx, ecx
.row_loop:
    cmp ecx, 16
    jge .out
    movzx ebx, byte [rsi + rcx]
    mov edi, r10d
    add edi, ecx
    cmp edi, 0
    jl .row_skip
    cmp edi, [r15+FB_HEIGHT]
    jge .row_skip

    mov eax, edi
    imul eax, [r15+FB_STRIDE]
    mov r8d, eax                        ; r8d = rowbase = y*stride

    xor edx, edx
.col_loop:
    cmp edx, 8
    jge .row_skip
    test ebx, 0x80
    jz .col_skip
    mov eax, r9d
    add eax, edx
    cmp eax, 0
    jl .col_skip
    cmp eax, [r15+FB_WIDTH]
    jge .col_skip
    add eax, r8d
    mov rdi, [r15+FB_BASE]
    lea rdi, [rdi + rax*4]
    mov [rdi], r11d
.col_skip:
    shl ebx, 1
    inc edx
    jmp .col_loop

.row_skip:
    inc ecx
    jmp .row_loop

.out:
    mov rbx, r15
    pop r15
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; wm_fb_draw_title: RCX=task ptr, RDX=x, R8D=y, R9D=color. Draws that
; task's TCB_WIN_TITLE (TCB_WIN_TITLE_LEN bytes) at the given pixel
; position via wm_fb_draw_glyph, one 8px cell per character.
; -------------------------------------------------------------------------
wm_fb_draw_title:
    push rax
    push rcx
    push rdx
    push rsi
    push r9
    push r10
    push r11
    push r12
    push r13

    mov r12, rcx                        ; task ptr
    mov r13d, edx                       ; running x
    mov r11d, r9d                       ; color
    mov r10d, r8d                       ; y

    lea rsi, [r12+TCB_WIN_TITLE]
    xor ecx, ecx
.loop:
    cmp ecx, [r12+TCB_WIN_TITLE_LEN]
    jge .out
    movzx eax, byte [rsi+rcx]
    mov r9d, r13d
    call wm_fb_draw_glyph
    add r13d, 8
    inc ecx
    jmp .loop
.out:
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; wm_tick: called once per compositor pass, before compositing. Every
; windowed task (index 0/main never has a window) is click-to-focus
; interactive, not just the frontmost one:
; -------------------------------------------------------------------------
; - A fresh left-button-down anywhere inside a window's full rect
;   (title bar, body, or the scrollbar strips -- same widened bounds
;   the compositor's own outer frame uses) brings THAT window to the
;   front of the z-order and gives it keyboard focus (see
;   basix_zorder_bring_front), if it wasn't already frontmost. The
;   z-order is scanned front-to-back so an occluded window behind
;   whatever's on top never steals a click meant for the visible one.
; - THEN, on that same click and using the now-focused window's own
;   geometry: left button down inside its title bar (excluding the
;   close gadget) starts a drag; held, moves the window (updates
;   TCB_WIN_X/Y to track the mouse, offset by where inside the title
;   bar the drag started); released, ends it. Left button down on its
;   close gadget sets TCB_WIN_CLOSE_REQ instead -- cooperative, not a
;   forced kill (see basix_rt_winclose). A click anywhere else in the
;   window (its body/content, or the not-yet-interactive scrollbar
;   area) just focuses it, no drag or close.
; A drag that actually moved the window, or any click that reordered
; the z-order, sets basix_wm_force_full for this pass, so
; compositor_task's own dirty-rect union (which only ever reflects
; what a task itself drew) doesn't miss whatever's now exposed behind
; the window's OLD position/stacking order.
; -------------------------------------------------------------------------
wm_tick:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12

    mov dword [rel basix_wm_force_full], 0

    mov eax, [rel basix_zorder_count]
    cmp eax, 1
    jle .no_front                       ; only main -- nothing windowed

    mov eax, [rel mouse_x]
    mov edx, [rel mouse_y]
    mov ecx, [rel mouse_btn]
    and ecx, 1                          ; left button bit only

    mov r8, [rel basix_wm_dragging]
    test r8, r8
    jz .not_dragging

    ; Currently dragging -- released?
    test ecx, ecx
    jnz .still_dragging
    mov qword [rel basix_wm_dragging], 0
    jmp .out

.still_dragging:
    ; Move the dragged task (r8, not necessarily still r12 if focus
    ; somehow changed mid-drag -- track the ORIGINAL task) to track
    ; the mouse, offset by the original grab point.
    mov r9d, eax
    sub r9d, [rel basix_wm_drag_off_x]
    mov r10d, edx
    sub r10d, [rel basix_wm_drag_off_y]
    cmp dword [r8+TCB_WIN_X], r9d
    je .same_x
    mov [r8+TCB_WIN_X], r9d
    mov dword [rel basix_wm_force_full], 1
.same_x:
    cmp dword [r8+TCB_WIN_Y], r10d
    je .out
    mov [r8+TCB_WIN_Y], r10d
    mov dword [rel basix_wm_force_full], 1
    jmp .out

.not_dragging:
    ; Not dragging -- a fresh left-button-down this pass (edge, not
    ; level, so a held-down button from before wm_tick even started
    ; polling doesn't retroactively grab anything)?
    test ecx, ecx
    jz .out
    test dword [rel basix_wm_prev_btn], 1
    jnz .out                            ; already was down last pass

    ; Real click-to-focus: scan the z-order FRONT to back (skip index
    ; 0/main, which is never windowed) for the topmost window whose
    ; full chrome+content rect -- including the scrollbar strips, same
    ; widened bounds the compositor's own outer frame uses -- contains
    ; this click. First (topmost) match wins, matching what's actually
    ; visible on screen; a window further back that happens to overlap
    ; the same point is occluded and shouldn't steal the click.
    mov r9d, [rel basix_zorder_count]
    dec r9d
.focus_scan:
    cmp r9d, 0
    jle .no_hit
    mov r8, [rel basix_zorder + r9*8]
    test r8, r8
    jz .focus_scan_next
    cmp dword [r8+TCB_WIN_W], 0
    je .focus_scan_next

    mov r10d, [r8+TCB_WIN_X]
    sub r10d, WM_BORDER
    cmp eax, r10d
    jl .focus_scan_next
    mov r11d, [r8+TCB_WIN_W]
    add r11d, WM_BORDER*2
    add r11d, WM_SCROLLBAR_W
    add r11d, r10d
    cmp eax, r11d
    jge .focus_scan_next
    mov r11d, [r8+TCB_WIN_Y]
    sub r11d, WM_TITLE_H
    sub r11d, WM_BORDER
    cmp edx, r11d
    jl .focus_scan_next
    mov r10d, [r8+TCB_WIN_H]
    add r10d, WM_TITLE_H
    add r10d, WM_BORDER*2
    add r10d, WM_SCROLLBAR_W
    add r10d, r11d
    cmp edx, r10d
    jge .focus_scan_next
    jmp .hit
.focus_scan_next:
    dec r9d
    jmp .focus_scan
.no_hit:
    jmp .out

.hit:
    ; r8 = the window under the click. Bring it to front (z-order +
    ; keyboard focus both, see basix_zorder_bring_front) if it isn't
    ; already, BEFORE the close-gadget/drag hit-tests below -- so
    ; clicking a background window's own close gadget or title bar
    ; both focuses it AND performs that action in the same click, the
    ; way real window managers do, instead of needing a second click
    ; once it's already frontmost.
    mov ecx, [rel basix_zorder_count]
    dec ecx
    cmp [rel basix_zorder + rcx*8], r8
    je .already_front
    mov rcx, r8
    call basix_zorder_bring_front
.already_front:
    mov r12, r8

    ; Compute this window's title bar + close gadget rects.
    mov r9d, [r12+TCB_WIN_X]
    sub r9d, WM_BORDER                  ; title_x0
    mov r10d, [r12+TCB_WIN_Y]
    sub r10d, WM_TITLE_H
    sub r10d, WM_BORDER                 ; title_y0
    mov r11d, [r12+TCB_WIN_W]
    add r11d, WM_BORDER*2               ; title_w

    ; Close gadget: top-right corner of the title bar. Must match the
    ; compositor's own drawn position exactly (see the close gadget
    ; draw block, kernel.asm) -- close_x0 there is
    ; TCB_WIN_X+TCB_WIN_W+WM_SCROLLBAR_W-WM_CLOSE_SIZE, and r9d+r11d
    ; here is (TCB_WIN_X-WM_BORDER)+(TCB_WIN_W+WM_BORDER*2) =
    ; TCB_WIN_X+TCB_WIN_W+WM_BORDER, so -WM_BORDER converts one to the
    ; other. A stale copy of this hit-test (still using the OLD drawn
    ; position, from before the scrollbar-width widening) is exactly
    ; what silently broke clicking the close gadget after that change
    ; -- the drawn X moved right but this rect didn't move with it.
    mov r8d, r9d
    add r8d, r11d
    sub r8d, WM_BORDER
    add r8d, WM_SCROLLBAR_W
    sub r8d, WM_CLOSE_SIZE               ; close_x0
    cmp eax, r8d
    jl .not_close
    mov ecx, r8d
    add ecx, WM_CLOSE_SIZE
    cmp eax, ecx
    jge .not_close
    cmp edx, r10d
    jl .not_close
    mov ecx, r10d
    add ecx, WM_TITLE_H
    cmp edx, ecx
    jge .not_close
    mov dword [r12+TCB_WIN_CLOSE_REQ], 1
    jmp .out

.not_close:
    ; Anywhere else in the title bar starts a drag. Right bound
    ; extended by WM_SCROLLBAR_W same as the close gadget's own
    ; position above -- the title bar is drawn that far now (it meets
    ; the close gadget in the true widened-frame corner), so the drag
    ; region should cover the same visual strip instead of leaving a
    ; thin non-draggable gap just left of the close gadget.
    cmp eax, r9d
    jl .out
    mov ecx, r9d
    add ecx, r11d
    add ecx, WM_SCROLLBAR_W
    cmp eax, ecx
    jge .out
    cmp edx, r10d
    jl .out
    mov ecx, r10d
    add ecx, WM_TITLE_H
    cmp edx, ecx
    jge .out

    mov [rel basix_wm_dragging], r12
    mov ecx, eax
    sub ecx, [r12+TCB_WIN_X]
    mov [rel basix_wm_drag_off_x], ecx
    mov ecx, edx
    sub ecx, [r12+TCB_WIN_Y]
    mov [rel basix_wm_drag_off_y], ecx

.no_front:
.out:
    mov eax, [rel mouse_btn]
    mov [rel basix_wm_prev_btn], eax
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
; compositor_task: R1's compositor, R2's z-order-aware version. RDI =
; boot_info* (task_create's argument delivery -- moved into RBX
; immediately, since a fresh task's initial GPR frame doesn't carry
; the "RBX=boot_info* always" convention the rest of the kernel relies
; on; every basix_rt_* caller up to now only ever ran on a task that
; inherited RBX from `entry:`'s own setup, never a brand-new one).
;
; Two-phase pass, not "blit only whichever tasks are individually
; dirty": (1) scan basix_zorder and union together the dirty rects of
; every entry that has one, (2) if anything was dirty at all, blit
; THAT union rect from EVERY z-order entry's current back buffer
; (back to front), not just the ones that happened to be dirty this
; pass, then clear every dirty flag. Iterating back-to-front means a
; frontmost task's content ends up painted last, on top -- the whole
; point of tracking z-order.
;
; Blitting non-dirty entries too (using their unchanged, already-
; current buffer content) is the part that actually makes z-order mean
; something once a foreground window stops redrawing every single
; frame -- e.g. EDITOR.BAS blocked on GETKEY between keystrokes, with
; Workbench still running underneath and redrawing unconditionally
; every ~20ms. The original single-phase version only ever blitted a
; task when ITS OWN dirty flag was set: on any pass where only
; Workbench (behind, in z-order) was dirty and Editor (in front)
; wasn't, only Workbench's full-screen blit happened, painting its own
; fresh frame straight over Editor's still-current, unchanged content
; -- Editor flickered away until its own next keystroke redrew it.
; There's still no real per-window bounds/clipping (R4/R5's job, once
; back buffers stop all being full-screen origin-(0,0) -- see
; basix_rt_ensure_backbuf), so this is a coarser "redraw everyone
; whenever anyone changes" fix rather than true occlusion, but it's
; enough to keep a topmost window visibly on top under real
; preemption.
;
; This is what FLIP used to do synchronously, inline, in the calling
; program itself -- now it happens independently, on its own
; schedule, from a dedicated task, which is the actual point of R1:
; presentation is decoupled from any one program's own execution, so
; multiple concurrent programs no longer each fight to blit their own
; frame straight to the screen.
;
; Paced with a plain busy-wait between passes (same style as
; test_task_a/b below) rather than tightly looping -- there's nothing
; to do most passes (nothing dirty), so there's no reason to burn a
; full timer quantum on this every single time round the ready ring.
; -------------------------------------------------------------------------
compositor_task:
    mov rbx, rdi                        ; rbx = boot_info*
.loop:
    call wm_tick

    ; Phase 1: union together every z-order entry's dirty rect, each
    ; converted to SCREEN space first (a windowed entry's TCB_DIRTY_*
    ; is in its own back buffer's LOCAL coordinates -- R4 gave
    ; windowed tasks a back buffer sized to their own window, not the
    ; screen, so local and screen coordinates aren't the same thing
    ; anymore the way they were when every buffer was full-screen
    ; origin-(0,0); TCB_WIN_X/Y, 0 for main, is the offset between
    ; them).
    mov dword [rel basix_comp_any_dirty], 0
    mov dword [rel basix_comp_idx], 0
.scan_loop:
    mov eax, [rel basix_comp_idx]
    cmp eax, [rel basix_zorder_count]
    jge .scan_done
    mov eax, [rel basix_comp_idx]
    mov rcx, [rel basix_zorder + rax*8]
    test rcx, rcx
    jz .scan_next
    cmp dword [rcx+TCB_DIRTY_VALID], 0
    je .scan_next
    ; Clear this entry's dirty flag HERE, now, rather than in a
    ; separate blind "clear every entry" pass at the very end (the
    ; old approach) -- that indiscriminate final clear raced against
    ; a task's own FLIP: if a task (e.g. a just-LAUNCHed editor.bas)
    ; set TCB_DIRTY_VALID=1 AFTER this scan already passed it by this
    ; pass, but BEFORE the old end-of-pass clear ran, its legitimate
    ; new dirty notification got silently wiped WITHOUT ever being
    ; blitted -- its content stayed correctly drawn in its own back
    ; buffer forever, just never copied to the screen. Clearing right
    ; here instead means only entries that are ACTUALLY being unioned
    ; into (and therefore correctly blitted as part of) THIS pass ever
    ; get cleared; anything that turns dirty in that race window keeps
    ; its flag set for the very next pass's scan to pick up correctly.
    mov dword [rcx+TCB_DIRTY_VALID], 0
    mov r8d, [rcx+TCB_DIRTY_X0]
    mov r9d, [rcx+TCB_DIRTY_Y0]
    mov r10d, [rcx+TCB_DIRTY_X1]
    mov r11d, [rcx+TCB_DIRTY_Y1]
    mov eax, [rcx+TCB_WIN_X]
    add r8d, eax
    add r10d, eax
    mov eax, [rcx+TCB_WIN_Y]
    add r9d, eax
    add r11d, eax
    cmp dword [rel basix_comp_any_dirty], 0
    jne .union_merge
    mov [rel basix_comp_union_x0], r8d
    mov [rel basix_comp_union_y0], r9d
    mov [rel basix_comp_union_x1], r10d
    mov [rel basix_comp_union_y1], r11d
    mov dword [rel basix_comp_any_dirty], 1
    jmp .scan_next
.union_merge:
    mov eax, [rel basix_comp_union_x0]
    cmp r8d, eax
    jge .ux0_ok
    mov [rel basix_comp_union_x0], r8d
.ux0_ok:
    mov eax, [rel basix_comp_union_y0]
    cmp r9d, eax
    jge .uy0_ok
    mov [rel basix_comp_union_y0], r9d
.uy0_ok:
    mov eax, [rel basix_comp_union_x1]
    cmp r10d, eax
    jle .ux1_ok
    mov [rel basix_comp_union_x1], r10d
.ux1_ok:
    mov eax, [rel basix_comp_union_y1]
    cmp r11d, eax
    jle .scan_next
    mov [rel basix_comp_union_y1], r11d
.scan_next:
    mov eax, [rel basix_comp_idx]
    inc eax
    mov [rel basix_comp_idx], eax
    jmp .scan_loop
.scan_done:
    ; Cursor movement dirty-tracking: the compositor-drawn cursor
    ; overlay (wm_fb_draw_cursor, drawn later in this same pass) is
    ; outside any task's own dirty-rect tracking entirely -- moving it
    ; needs the OLD position's pixels properly redrawn from whatever
    ; window content is actually there, plus the NEW position painted,
    ; so union in a small rect covering both bounds whenever the mouse
    ; has moved since the last pass -- same "force a redraw for
    ; something no task's own dirty rect reflects" pattern
    ; basix_wm_force_full already uses for window drags/closes below,
    ; just scoped to a small cursor-sized rect instead of the whole
    ; screen.
    mov eax, [rel mouse_x]
    mov ecx, [rel mouse_y]
    cmp eax, [rel basix_cursor_last_x]
    jne .cursor_moved
    cmp ecx, [rel basix_cursor_last_y]
    je .cursor_not_moved
.cursor_moved:
    mov r8d, [rel basix_cursor_last_x]
    cmp r8d, eax
    jle .cx0_ok
    mov r8d, eax
.cx0_ok:
    mov r9d, [rel basix_cursor_last_y]
    cmp r9d, ecx
    jle .cy0_ok
    mov r9d, ecx
.cy0_ok:
    mov r10d, [rel basix_cursor_last_x]
    cmp r10d, eax
    jge .cx1_ok
    mov r10d, eax
.cx1_ok:
    add r10d, WM_CURSOR_W
    mov r11d, [rel basix_cursor_last_y]
    cmp r11d, ecx
    jge .cy1_ok
    mov r11d, ecx
.cy1_ok:
    add r11d, WM_CURSOR_H

    cmp dword [rel basix_comp_any_dirty], 0
    jne .cursor_union_merge
    mov [rel basix_comp_union_x0], r8d
    mov [rel basix_comp_union_y0], r9d
    mov [rel basix_comp_union_x1], r10d
    mov [rel basix_comp_union_y1], r11d
    mov dword [rel basix_comp_any_dirty], 1
    jmp .cursor_not_moved
.cursor_union_merge:
    mov edx, [rel basix_comp_union_x0]
    cmp r8d, edx
    jge .ccx0_ok
    mov [rel basix_comp_union_x0], r8d
.ccx0_ok:
    mov edx, [rel basix_comp_union_y0]
    cmp r9d, edx
    jge .ccy0_ok
    mov [rel basix_comp_union_y0], r9d
.ccy0_ok:
    mov edx, [rel basix_comp_union_x1]
    cmp r10d, edx
    jle .ccx1_ok
    mov [rel basix_comp_union_x1], r10d
.ccx1_ok:
    mov edx, [rel basix_comp_union_y1]
    cmp r11d, edx
    jle .cursor_not_moved
    mov [rel basix_comp_union_y1], r11d
.cursor_not_moved:
    mov eax, [rel mouse_x]
    mov [rel basix_cursor_last_x], eax
    mov eax, [rel mouse_y]
    mov [rel basix_cursor_last_y], eax

    ; A drag actually moving a window (wm_tick) means the OLD window
    ; position needs covering too, which no task's own dirty rect
    ; reflects (nothing drew there -- the window just moved away from
    ; it) -- force a full-screen recomposite for this one pass rather
    ; than tracking old-vs-new rects separately.
    cmp dword [rel basix_wm_force_full], 0
    je .check_any_dirty
    mov dword [rel basix_comp_union_x0], 0
    mov dword [rel basix_comp_union_y0], 0
    mov eax, [rbx+FB_WIDTH]
    mov [rel basix_comp_union_x1], eax
    mov eax, [rbx+FB_HEIGHT]
    mov [rel basix_comp_union_y1], eax
    mov dword [rel basix_comp_any_dirty], 1
.check_any_dirty:
    cmp dword [rel basix_comp_any_dirty], 0
    je .clear_pass

    ; Phase 2: for each z-order entry, back to front, draw its chrome
    ; (if windowed -- index 0/main never is) and THEN its own content
    ; blit, as one interleaved per-entry pair -- NOT "every entry's
    ; chrome, then every entry's content" as two separate passes (an
    ; earlier version of this fix did exactly that, to solve a
    ; DIFFERENT bug -- see the content-blit block's own comment on
    ; that). Batching chrome-then-content phase-wide broke multi-
    ; window z-order: with two overlapping chrome'd windows, the BACK
    ; window's content blit (which only knows its own bounds, not
    ; that a front window's chrome is sitting in part of them) ran
    ; AFTER the FRONT window's chrome had already been drawn, so it
    ; painted over that chrome wherever the two windows overlapped --
    ; the front window visibly lost its title bar/border/close gadget
    ; entirely. Interleaving per-entry, back to front, means each
    ; window's own content always lands right after its own chrome
    ; (fixing the original bug), and the NEXT entry forward in z-order
    ; still correctly draws over everything from every entry behind it
    ; -- chrome AND content both -- exactly like real window stacking.
    mov dword [rel basix_comp_idx], 0
.entry_loop:
    mov eax, [rel basix_comp_idx]
    cmp eax, [rel basix_zorder_count]
    jge .cursor_draw
    mov eax, [rel basix_comp_idx]
    mov r15, [rel basix_zorder + rax*8]
    test r15, r15
    jz .entry_next
    cmp dword [r15+TCB_WIN_W], 0
    je .content_blit                    ; not windowed (main) -- no chrome

    ; Outer frame: one raised bevel (see wm_fb_bevel_rect) around the
    ; whole window -- title bar and client area together -- filled
    ; gray. The title bar's own flat blue fill (next) draws right over
    ; this frame's top edge, which is fine: the title bar's own
    ; bottom edge, where it meets the gray client area, already reads
    ; as a clear boundary on its own. Left/right/bottom stay visible
    ; as the window's raised outline.
    mov ecx, [r15+TCB_WIN_X]
    sub ecx, WM_BORDER
    mov edx, [r15+TCB_WIN_Y]
    sub edx, WM_TITLE_H
    sub edx, WM_BORDER
    mov r8d, [r15+TCB_WIN_W]
    add r8d, WM_BORDER*2
    add r8d, WM_SCROLLBAR_W             ; room for the right-edge
                                         ; vertical scrollbar strip
    mov r9d, [r15+TCB_WIN_H]
    add r9d, WM_TITLE_H
    add r9d, WM_BORDER*2
    add r9d, WM_SCROLLBAR_W             ; room for the bottom-edge
                                         ; horizontal scrollbar strip
    mov r10d, WM_BODY_BG
    call wm_fb_bevel_rect

    ; Title bar background -- flat, no bevel of its own (real 2.04
    ; chrome never puts a bevel on the title bar itself, only on
    ; gadgets/frames), but inset by WM_EDGE on the top/left/right so
    ; the outer frame's own border (drawn just above, wrapping the
    ; WHOLE window) stays visible alongside and above the title bar
    ; too, not just around the body -- previously this fill extended
    ; all the way to the frame's own edges and painted directly over
    ; its top/left/right border lines within the title bar's height,
    ; so the border only ever looked like it wrapped the body.
    mov ecx, [r15+TCB_WIN_X]
    sub ecx, WM_BORDER
    add ecx, WM_EDGE
    mov edx, [r15+TCB_WIN_Y]
    sub edx, WM_TITLE_H
    sub edx, WM_BORDER
    add edx, WM_EDGE
    mov r8d, [r15+TCB_WIN_W]
    add r8d, WM_BORDER*2
    sub r8d, WM_EDGE*2
    add r8d, WM_SCROLLBAR_W             ; span the full widened frame
                                         ; (see the outer-frame comment
                                         ; above) -- without this the
                                         ; title bar stopped short of
                                         ; the frame's true right edge,
                                         ; leaving a gray notch there
                                         ; with the close gadget (still
                                         ; positioned off the OLD,
                                         ; unwidened edge) sitting to
                                         ; its left instead of in the
                                         ; actual top-right corner.
    mov r9d, WM_TITLE_H
    sub r9d, WM_EDGE
    mov r10d, WM_TITLE_BG
    call wm_fb_fill_rect

    ; Title text -- vertically centered against the title bar's own
    ; VISIBLE fill (WIN_Y - TITLE_H - BORDER + EDGE, matching the fill
    ; rect's own y0 just above), not "WIN_Y - TITLE_H" alone -- that
    ; earlier version silently forgot the -BORDER+EDGE terms, so it
    ; measured from a point BORDER-EDGE=2px below where the bar
    ; actually starts, throwing centering off (see
    ; WM_TITLE_TEXT_MARGIN's own comment).
    mov rcx, r15
    mov edx, [r15+TCB_WIN_X]
    add edx, 4
    mov r8d, [r15+TCB_WIN_Y]
    sub r8d, WM_TITLE_H
    sub r8d, WM_BORDER
    add r8d, WM_EDGE
    add r8d, WM_TITLE_TEXT_MARGIN
    mov r9d, WM_TITLE_FG
    call wm_fb_draw_title

    ; Close gadget -- its own small raised bevel box, vertically
    ; centered the same way (same fix as the title text just above --
    ; this used to bleed 1px past the title bar's own bottom edge
    ; because of the identical missing -BORDER+EDGE terms).
    mov ecx, [r15+TCB_WIN_X]
    sub ecx, WM_BORDER
    add ecx, [r15+TCB_WIN_W]
    add ecx, WM_BORDER
    add ecx, WM_SCROLLBAR_W             ; same widened-frame fix as the
                                         ; title bar fill above -- the
                                         ; close gadget belongs in the
                                         ; TRUE top-right corner
    sub ecx, WM_CLOSE_SIZE               ; no extra right-margin trim
                                         ; here anymore -- WM_CLOSE_SIZE
                                         ; == WM_SCROLLBAR_W, so this
                                         ; now lines the close gadget's
                                         ; own left edge up exactly
                                         ; with the vertical scrollbar
                                         ; arrow gadgets' left edge
                                         ; below it, per request
    mov edx, [r15+TCB_WIN_Y]
    sub edx, WM_TITLE_H
    sub edx, WM_BORDER
    add edx, WM_EDGE
    add edx, WM_TITLE_TEXT_MARGIN
    mov r8d, WM_CLOSE_SIZE
    mov r9d, WM_CLOSE_SIZE
    mov r10d, WM_BODY_BG
    call wm_fb_bevel_rect

    ; 'X' glyph inside the close gadget box -- the kernel-drawn chrome
    ; never had ANY icon here before (just the blank bevel box above),
    ; unlike workbench.bas's own OLD hand-drawn window, which LOADPNGed
    ; a real close icon for its own close button (that hand-drawn
    ; window no longer exists -- see filebrowser.bas). ecx/edx still
    ; hold the close gadget's own x0/y0 from the bevel_rect call just
    ; above (wm_fb_bevel_rect preserves them). Glyph cell is 8x16 (see
    ; basix_draw_glyph/font8x16) against a WM_CLOSE_SIZE=16 box -- an
    ; EXACT height match (no vertical offset needed at all, unlike the
    ; old WM_CLOSE_SIZE=14 box, which overflowed the glyph 1px into the
    ; box's own top/bottom bevel edges), centered horizontally with a
    ; 4px margin each side ((16-8)/2).
    mov al, 'X'
    mov r9d, ecx
    add r9d, 4
    mov r10d, edx
    mov r11d, WM_CLOSE_FG
    call wm_fb_draw_glyph

    ; Scrollbars + resize gadget -- real Workbench 2.04 visual elements
    ; on every open window (right-edge vertical bar, bottom-edge
    ; horizontal bar, corner resize gadget where they meet), but purely
    ; decorative for now: no scroll or resize behavior wired up to
    ; them yet, just the graphics. The outer frame above was already
    ; widened by WM_SCROLLBAR_W on the right and bottom to make room
    ; for these; content_blit below still only touches the unwidened
    ; TCB_WIN_W/H area, so a program's own drawing never overlaps
    ; them.
    ; Active/inactive gadget-background color: the focused (== frontmost,
    ; per the kernel-wide "topmost == focused" invariant -- see R2's own
    ; comment) window's scrollbar track/arrow/resize backgrounds fill
    ; with the same blue as its own title bar; every other window's
    ; fill gray, same as the rest of its chrome. Computed once here into
    ; r12d (untouched by every wm_fb_* call below -- same reasoning as
    ; r13d just above) and reused for every fill in this whole block;
    ; the glyphs/bevel edges drawn on top stay their own fixed colors
    ; regardless (only the flat background actually changes).
    xor r12d, r12d
    mov r12d, WM_BODY_BG
    cmp r15, [rel basix_kbd_focus_task]
    jne .scrollbar_bg_ready
    mov r12d, WM_TITLE_BG
.scrollbar_bg_ready:

    mov eax, [r15+TCB_WIN_X]
    add eax, [r15+TCB_WIN_W]            ; eax = content's right edge
                                         ; (screen x) -- where the
                                         ; vertical scrollbar starts
    mov r13d, [r15+TCB_WIN_Y]
    add r13d, [r15+TCB_WIN_H]           ; r13d = content's bottom edge
                                         ; (screen y) -- where the
                                         ; horizontal scrollbar starts.
                                         ; NOT rbx: every wm_fb_* call
                                         ; below requires RBX to still
                                         ; be boot_info* (see their own
                                         ; "RBX must be boot_info*"
                                         ; doc comments) -- r13 is
                                         ; untouched by all of them.

    ; Vertical scrollbar track (full content height, behind the arrow
    ; gadgets -- they're drawn on top right after).
    mov ecx, eax
    mov edx, [r15+TCB_WIN_Y]
    mov r8d, WM_SCROLLBAR_W
    mov r9d, [r15+TCB_WIN_H]
    mov r10d, r12d
    call wm_fb_fill_rect

    ; Up arrow gadget (top of vertical bar).
    mov ecx, eax
    mov edx, [r15+TCB_WIN_Y]
    mov r8d, WM_SCROLLBAR_W
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_bevel_rect
    mov r10d, WM_BEVEL_DARK
    call wm_fb_draw_arrow_up

    ; Down arrow gadget (bottom of vertical bar).
    mov ecx, eax
    mov edx, [r15+TCB_WIN_Y]
    add edx, [r15+TCB_WIN_H]
    sub edx, WM_SCROLLBAR_W
    mov r8d, WM_SCROLLBAR_W
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_bevel_rect
    mov r10d, WM_BEVEL_DARK
    call wm_fb_draw_arrow_down

    ; Vertical track outline -- marks the groove the thumb (drawn
    ; next) travels within, between the two arrow gadgets.
    mov ecx, eax
    mov edx, [r15+TCB_WIN_Y]
    add edx, WM_SCROLLBAR_W
    mov r8d, WM_SCROLLBAR_W
    mov r9d, [r15+TCB_WIN_H]
    sub r9d, WM_SCROLLBAR_W*2
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_outline_rect

    ; Vertical thumb -- static (no real scroll position yet), placed a
    ; short way below the up arrow.
    mov ecx, eax
    add ecx, 2
    mov edx, [r15+TCB_WIN_Y]
    add edx, WM_SCROLLBAR_W
    add edx, 6
    mov r8d, WM_SCROLLBAR_W
    sub r8d, 4
    mov r9d, 32
    mov r10d, r12d
    call wm_fb_bevel_rect_inset

    ; Horizontal scrollbar track (full content width).
    mov ecx, [r15+TCB_WIN_X]
    mov edx, r13d
    mov r8d, [r15+TCB_WIN_W]
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_fill_rect

    ; Left arrow gadget.
    mov ecx, [r15+TCB_WIN_X]
    mov edx, r13d
    mov r8d, WM_SCROLLBAR_W
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_bevel_rect
    mov r10d, WM_BEVEL_DARK
    call wm_fb_draw_arrow_left

    ; Right arrow gadget.
    mov ecx, [r15+TCB_WIN_X]
    add ecx, [r15+TCB_WIN_W]
    sub ecx, WM_SCROLLBAR_W
    mov edx, r13d
    mov r8d, WM_SCROLLBAR_W
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_bevel_rect
    mov r10d, WM_BEVEL_DARK
    call wm_fb_draw_arrow_right

    ; Horizontal track outline -- same as the vertical one above, the
    ; groove between the two arrow gadgets.
    mov ecx, [r15+TCB_WIN_X]
    add ecx, WM_SCROLLBAR_W
    mov edx, r13d
    mov r8d, [r15+TCB_WIN_W]
    sub r8d, WM_SCROLLBAR_W*2
    mov r9d, WM_SCROLLBAR_W
    mov r10d, WM_BEVEL_LIGHT
    call wm_fb_outline_rect

    ; Horizontal thumb -- static, placed a short way right of the left
    ; arrow.
    mov ecx, [r15+TCB_WIN_X]
    add ecx, WM_SCROLLBAR_W
    add ecx, 6
    mov edx, r13d
    add edx, 2
    mov r8d, 32
    mov r9d, WM_SCROLLBAR_W
    sub r9d, 4
    mov r10d, r12d
    call wm_fb_bevel_rect_inset

    ; Resize gadget -- corner box where the two scrollbars meet, with
    ; a couple of short ridge lines suggesting a grip (purely
    ; decorative, same as everything else in this block).
    mov ecx, eax
    mov edx, r13d
    mov r8d, WM_SCROLLBAR_W
    mov r9d, WM_SCROLLBAR_W
    mov r10d, r12d
    call wm_fb_bevel_rect
    mov ecx, eax
    add ecx, 4
    mov edx, r13d
    add edx, 10
    mov r8d, 8
    mov r9d, 1
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect
    mov ecx, eax
    add ecx, 4
    mov edx, r13d
    add edx, 6
    mov r8d, 8
    mov r9d, 1
    mov r10d, WM_BEVEL_DARK
    call wm_fb_fill_rect

    ; Content blit -- this entry's own current back buffer, straight
    ; over whatever chrome (just above, if any) drew. Runs for EVERY
    ; entry including index 0/main (no chrome, but still content), not
    ; just windowed ones. Must come after chrome, not before: the
    ; chrome frame's own fill covers this entry's ENTIRE window
    ; rectangle, client area included (see the outer-frame comment
    ; above), so drawing it after content would paint flat gray
    ; straight over whatever this task just correctly drew -- never
    ; visibly caught before this investigation because every window
    ; tested up to that point happened to be blank (gray-on-gray
    ; content is indistinguishable from the chrome fill covering it).
.content_blit:
    mov rsi, [r15+TCB_BACKBUF_PTR]
    test rsi, rsi
    jz .entry_next

    ; Every entry repaints its own FULL bounds on any compositor pass,
    ; rather than being clamped to the shared cross-window dirty union
    ; (the old approach here). Clamping to the union meant an IDLE
    ; window -- nothing of its OWN dirty this particular pass, e.g.
    ; EDITOR.BAS blocked on GETKEY between keystrokes while
    ; FILEBROWSER.BAS keeps redrawing every ~20ms -- only got re-
    ; blitted within whatever narrow region some OTHER window's own
    ; redraw (or even just the cursor moving) happened to cover that
    ; pass. Confirmed live: EDITOR.BAS's content area visibly
    ; contracted to exactly FILEBROWSER.BAS's overlapping screen
    ; bounds, then expanded back to full width for one frame on
    ; EDITOR's own next keystroke before contracting again. Re-
    ; blitting a window's already-computed back buffer is a plain
    ; memcpy (cheap) regardless of how much of it actually changed --
    ; the expensive part (each program's own drawing) already happened
    ; before FLIP -- so there's no real cost to always copying the
    ; whole buffer once ANY pass is triggered at all (still gated by
    ; basix_comp_any_dirty in phase 1, so a fully idle desktop doesn't
    ; burn cycles compositing nothing).
    mov r12d, [r15+TCB_BACKBUF_W]
    mov r13d, [r15+TCB_BACKBUF_H]
    xor r8d, r8d
    xor r9d, r9d
    mov r10d, r12d
    mov r11d, r13d

    cmp r8d, r10d
    jge .entry_next                      ; zero-size buffer -- nothing
                                         ; to blit
    cmp r9d, r11d
    jge .entry_next

    mov r14d, r9d                       ; r14d = LOCAL row index
.row_loop:
    cmp r14d, r11d
    jge .entry_next

    mov eax, r14d
    imul eax, r12d
    add eax, r8d
    mov rcx, rsi
    lea rcx, [rcx + rax*4]              ; source: this row's local span

    mov eax, r14d
    add eax, [r15+TCB_WIN_Y]            ; eax = screen row
    imul eax, [rbx+FB_STRIDE]
    mov edx, r8d
    add edx, [r15+TCB_WIN_X]            ; edx = screen col start
    add eax, edx
    mov rdx, [rbx+FB_BASE]
    lea rdx, [rdx + rax*4]              ; dest: same span in the real fb

    push rsi
    push rdi
    mov rsi, rcx
    mov rdi, rdx
    mov ecx, r10d
    sub ecx, r8d                        ; span width, in pixels
    cld
    rep movsd
    pop rdi
    pop rsi

    inc r14d
    jmp .row_loop

.entry_next:
    mov eax, [rel basix_comp_idx]
    inc eax
    mov [rel basix_comp_idx], eax
    jmp .entry_loop

; Cursor overlay: drawn dead last, after every window's own chrome and
; content, so it's always on top regardless of which window the
; pointer happens to be over -- see wm_fb_draw_cursor's own comment.
.cursor_draw:
    mov ecx, [rel mouse_x]
    mov edx, [rel mouse_y]
    call wm_fb_draw_cursor

.clear_pass:
    ; Each entry's own dirty flag is already cleared as part of phase
    ; 1's scan (see its own comment) -- nothing left to do here except
    ; fall through to the delay. Label kept (rather than removed and
    ; every jump retargeted) since both "nothing at all was dirty" and
    ; "chrome_phase just finished" branch here.

.delay:
    ; No explicit busy-wait: checking "is anything dirty" is a handful
    ; of memory reads, cheap enough to just do again immediately.
    ; Round-robin scheduling already caps how much of the CPU this can
    ; ever take -- SCHED_QUANTUM (sched.inc) preempts back to the next
    ; ready task at the same fixed ~20ms cadence whether this loop
    ; spins tightly or sleeps, so a busy-wait here would only add
    ; guesswork about how many cycles is "long enough" (this kernel's
    ; TCG emulation speed varies widely across environments -- a fixed
    ; iteration count tuned on a fast host, like this one originally
    ; was, could leave a slow host waiting tens of seconds for the
    ; first composite pass) without changing the actual worst-case CPU
    ; share this task can consume.
    jmp .loop

basix_comp_any_dirty: dd 0
basix_comp_idx: dd 0
basix_comp_union_x0: dd 0
basix_comp_union_y0: dd 0
basix_comp_union_x1: dd 0
basix_comp_union_y1: dd 0
; Last position the cursor overlay was actually drawn at (see
; wm_fb_draw_cursor's own comment) -- sentinel far off-screen so the
; very first compositor pass always treats it as "moved" and draws it.
basix_cursor_last_x: dd -100000
basix_cursor_last_y: dd -100000

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
; basix_load_program: RCX = NUL-terminated path (may be a deep path --
; see exfat_resolve_path). RDX = target slot index (0..
; BASIX_SLOT_COUNT-1, see basix_symbols.inc) -- the caller has already
; basix_slot_alloc'd this slot; basix_load_program always compiles a
; fresh, fully-reset program into it via basix_compile_slot_begin (no
; more "child mode" sharing -- every load gets its own isolated slot,
; so a LAUNCHed program's variables/arrays simply can't collide with
; its launcher's, in a different slot's own dedicated memory. See
; basix_rt_launch, which now allocates a real slot instead of sharing
; the caller's).
;
; Resolves the path and gets a runnable program sitting in the active
; slot's code buffer (basix_active_code_buf_ptr), ready to be CALLed,
; either by copying a precompiled .AXB straight in (magic-sniffed, see
; basix_codegen.inc's format comment) or by stream-compiling .bas
; source -- the shared core behind RUN, GUI, and LAUNCH, so all three
; go through the exact same resolve/sniff/load logic. Never touches
; console output; callers decide how (or whether) to report a failure.
; Caller must hold basix_compile_lock across this call (and, for
; .source below, the whole exfat_resolve_path+compile sequence is what
; the lock is actually protecting -- see basix_compile_slot_begin).
; Returns EAX = status: 0=ok, 1=not found, 2=BASIX64 compile error,
; 3=.axb stale/corrupt/oversized for this kernel build.
; -------------------------------------------------------------------------
basix_load_program:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13

    mov r13, rcx                        ; r13 = path ptr, saved before RCX
                                         ; gets reused as basix_compile_
                                         ; slot_begin's slot-index arg below
    mov r12d, edx                       ; r12d = target slot index (saved
                                         ; before rdx gets reused below)
    mov ecx, r12d
    call basix_compile_slot_begin       ; repoints active_* ptrs, resets
                                         ; compile-time bookkeeping

    mov rcx, r13
    lea r8, [rel exfat_find_result]
    call exfat_resolve_path
    test eax, eax
    jz .notfound

    cmp qword [rel exfat_find_result+8], 4   ; DataLength >= 4?
    jb .source

    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    xor edx, edx                        ; offset 0
    mov r8d, 16
    lea r9, [rel basix_axb_header_buf]
    mov r10d, [rel exfat_find_result+16] ; NoFatChain
    call exfat_read_file_at             ; preserves RCX/R10 across the
                                         ; call -- both still hold
                                         ; FirstCluster/NoFatChain below
    test eax, eax
    jz .source                          ; header read failed -- fall back

    mov eax, [rel basix_axb_header_buf+0]
    cmp eax, 0x31425841                 ; 'A','X','B','1' little-endian
    jne .source

    mov eax, [rel basix_axb_header_buf+4]  ; kernel_id
    cmp eax, BASIX_AXB_KERNEL_ID
    jne .axb_stale

    mov r11d, [rel basix_axb_header_buf+8] ; code_size
    cmp r11d, BASIX_CODE_BUF_SIZE
    ja .axb_stale                       ; too big to be a real match -- refuse

    mov edx, 16                         ; code bytes start right after the header
    mov r8d, r11d
    mov r9, [rel basix_active_code_buf_ptr]
    call exfat_read_file_at
    test eax, eax
    jz .axb_stale

    mov [rel basix_code_pos], r11d
    mov dword [rel basix_compile_ok], 1
    xor eax, eax
    jmp .out

.source:
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
    jz .compilefail
    xor eax, eax
    jmp .out

.notfound:
    mov eax, 1
    jmp .out
.compilefail:
    mov eax, 2
    jmp .out
.axb_stale:
    mov eax, 3
.out:
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
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
    lea rdx, [rel shell_str_compile]
    call shell_streq
    test eax, eax
    jnz .do_compile

    lea rcx, [rel shell_cmd_buf]
    lea rdx, [rel shell_str_gui]
    call shell_streq
    test eax, eax
    jnz .do_gui

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

    ; exfat_resolve_path (not exfat_find_root_file) so a deep path like
    ; "GRAPHICS/cube.bas" or "/APPS/foo.axb" works directly, the same
    ; way editor/viewer's OPEN-then-bare-name navigation does, but
    ; without needing an OPEN first and without touching cwd.
    call basix_compile_lock_acquire
    call basix_slot_alloc
    cmp eax, 0xFFFFFFFF
    je .run_noslot
    mov r12d, eax                        ; r12d = this run's slot

    lea rcx, [rel shell_arg_buf]
    mov edx, r12d
    call basix_load_program
    mov r13d, eax                        ; r13d = load status
    ; Capture the code entry point while still under the lock -- a
    ; concurrent compile on another task could repoint the shared
    ; basix_active_code_buf_ptr the instant the lock is released.
    ; RAX, not RBX: RBX is the kernel-wide "boot_info*" register, still
    ; relied on by fb_draw_char (console text -- see basix_rt_print_str
    ; et al) for the whole duration the called program runs, so it must
    ; not be clobbered by the code-pointer-to-call itself.
    mov rax, [rel basix_active_code_buf_ptr]
    call basix_compile_lock_release

    cmp r13d, 1
    je .run_notfound_freed
    cmp r13d, 2
    je .run_compilefail_freed
    cmp r13d, 3
    je .run_axb_stale_freed

    call rax
    mov dword [rel fb_text_color], 0xFFFFFFFF  ; a program may leave COLOR
                                                ; set to something other than
                                                ; white -- don't let that leak
                                                ; into the shell's own prompt
    mov ecx, r12d
    call basix_slot_free
    jmp .out
.run_notfound_freed:
    mov ecx, r12d
    call basix_slot_free
    jmp .run_notfound
.run_compilefail_freed:
    mov ecx, r12d
    call basix_slot_free
    jmp .run_compilefail
.run_axb_stale_freed:
    mov ecx, r12d
    call basix_slot_free
    jmp .run_axb_stale
.run_noslot:
    call basix_compile_lock_release
    lea rcx, [rel msg_shell_run_noslot]
    call console_puts
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
.run_axb_stale:
    lea rcx, [rel msg_shell_run_axb_stale]
    call console_puts
    jmp .out

; -------------------------------------------------------------------------
; COMPILE <source.bas> <output.axb> -- runs the same basix_compile_file
; pipeline RUN uses, but instead of jumping into the result, serializes
; basix_code_buf to disk as an .AXB (see basix_codegen.inc's format
; comment) so it can be loaded straight back by RUN's fast path later
; without re-running the lexer/parser/codegen at all. Both filename
; arguments accept deep paths ("GRAPHICS/cube.bas", "APPS/foo.axb");
; the destination's parent directory must already exist (APPS/ is
; created once at boot -- see the mount sequence).
; -------------------------------------------------------------------------
.do_compile:
    lea rdi, [rel shell_arg_buf]
    xor ecx, ecx
.compile_copy1:
    mov al, [rsi]
    test al, al
    jz .compile_arg1_done
    cmp al, ' '
    je .compile_arg1_done
    cmp ecx, 63
    jge .compile_arg1_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .compile_copy1
.compile_arg1_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .compile_usage

.compile_skip_sp:
    cmp byte [rsi], ' '
    jne .compile_copy2_start
    inc rsi
    jmp .compile_skip_sp
.compile_copy2_start:
    lea rdi, [rel shell_arg_buf2]
    xor ecx, ecx
.compile_copy2:
    mov al, [rsi]
    test al, al
    jz .compile_arg2_done
    cmp al, ' '
    je .compile_arg2_done
    cmp ecx, 63
    jge .compile_arg2_done
    mov [rdi+rcx], al
    inc ecx
    inc rsi
    jmp .compile_copy2
.compile_arg2_done:
    mov byte [rdi+rcx], 0
    test ecx, ecx
    jz .compile_usage

    call basix_compile_lock_acquire
    call basix_slot_alloc
    cmp eax, 0xFFFFFFFF
    je .compile_noslot
    mov r15d, eax                       ; r15d = this compile's slot
    mov ecx, r15d
    call basix_compile_slot_begin

    lea rcx, [rel shell_arg_buf]
    lea r8, [rel exfat_find_result]
    call exfat_resolve_path
    test eax, eax
    jz .compile_notfound_freed

    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    mov rdx, [rel exfat_find_result+8]  ; DataLength
    mov r8d, [rel exfat_find_result+16] ; NoFatChain
    call basix_compile_file
    test eax, eax
    jz .compile_compilefail_freed

    ; The compile lock stays held through the disk write below (kept
    ; simple rather than capturing basix_active_code_buf_ptr/
    ; basix_code_pos across calls whose register-clobber contract
    ; isn't guaranteed to preserve them) -- COMPILE is an interactive,
    ; infrequent shell command, not a hot path, so a slightly longer
    ; lock hold here is a non-issue.
    lea rcx, [rel shell_arg_buf2]
    call exfat_resolve_parent_dir
    test eax, eax
    jz .compile_badpath
    mov r12d, ecx                       ; r12d = destination parent cluster
    mov r13, rdx                        ; r13 = ptr to final filename component

    mov r14d, [rel exfat_cwd_cluster]   ; save cwd -- exfat_write_file/
                                         ; exfat_append_file both operate
                                         ; against it, and COMPILE must
                                         ; not leave the shell's cwd
                                         ; changed as a side effect
    mov [rel exfat_cwd_cluster], r12d

    ; basix_axb_header_buf is shared with RUN/LAUNCH's .axb-loading
    ; path, which overwrites it with whatever kernel_id an OLD file on
    ; disk claims (to compare it against this build's real one) -- if
    ; that ever ran earlier in this session, bytes 4-7 here no longer
    ; hold this build's true BASIX_AXB_KERNEL_ID. Always write the real
    ; constant explicitly rather than trusting it "still" holds the
    ; value it was statically initialized with.
    mov dword [rel basix_axb_header_buf+4], BASIX_AXB_KERNEL_ID
    mov eax, [rel basix_code_pos]
    mov [rel basix_axb_header_buf+8], eax  ; code_size

    mov rcx, r13
    lea r8, [rel basix_axb_header_buf]
    mov r9, 16
    call exfat_write_file
    test eax, eax
    jz .compile_writefail

    mov rcx, r13
    mov r8, [rel basix_active_code_buf_ptr]
    mov r9d, [rel basix_code_pos]
    call exfat_append_file
    test eax, eax
    jz .compile_writefail

    mov [rel exfat_cwd_cluster], r14d
    mov ecx, r15d
    call basix_slot_free
    call basix_compile_lock_release
    lea rcx, [rel msg_shell_compile_ok]
    call console_puts
    jmp .out

.compile_usage:
    lea rcx, [rel msg_shell_compile_usage]
    call console_puts
    jmp .out
.compile_noslot:
    call basix_compile_lock_release
    lea rcx, [rel msg_shell_run_noslot]
    call console_puts
    jmp .out
.compile_notfound_freed:
    mov ecx, r15d
    call basix_slot_free
    call basix_compile_lock_release
    jmp .compile_notfound
.compile_notfound:
    lea rcx, [rel msg_shell_notfound]
    call console_puts
    jmp .out
.compile_compilefail_freed:
    mov ecx, r15d
    call basix_slot_free
    call basix_compile_lock_release
    jmp .compile_compilefail
.compile_compilefail:
    lea rcx, [rel msg_shell_run_compilefail]
    call console_puts
    jmp .out
.compile_badpath:
    mov ecx, r15d
    call basix_slot_free
    call basix_compile_lock_release
    lea rcx, [rel msg_shell_compile_badpath]
    call console_puts
    jmp .out
.compile_writefail:
    mov ecx, r15d
    call basix_slot_free
    call basix_compile_lock_release
    mov [rel exfat_cwd_cluster], r14d
    lea rcx, [rel msg_shell_compile_writefail]
    call console_puts
    jmp .out

; -------------------------------------------------------------------------
; GUI -- launches WORKBENCH.BAS (the desktop shell) exactly like RUN,
; via the shared basix_load_program. When it exits (its own END, e.g.
; on Escape), the screen is left showing whatever the GUI last drew
; (drawn straight to the visible framebuffer via FLIP, bypassing the
; text console entirely) -- fb_clear it and reset the console cursor
; so the shell prompt reappears on a clean screen instead of on top of
; stale desktop pixels.
; -------------------------------------------------------------------------
.do_gui:
    call basix_compile_lock_acquire
    call basix_slot_alloc
    cmp eax, 0xFFFFFFFF
    je .gui_noslot
    mov r12d, eax                        ; r12d = this GUI run's slot

    lea rcx, [rel gui_program_name]
    mov edx, r12d
    call basix_load_program
    mov r13d, eax
    ; RAX, not RBX: RBX is the kernel-wide "boot_info*" register, still
    ; needed (by fb_draw_char, fb_clear below, etc.) for the whole
    ; duration the GUI program runs -- see the identical fix in .do_run.
    mov rax, [rel basix_active_code_buf_ptr]   ; capture before unlocking
    call basix_compile_lock_release
    test r13d, r13d
    jnz .gui_load_failed_freed

    call rax
    mov dword [rel fb_text_color], 0xFFFFFFFF

    call fb_clear
    mov dword [rel console_row], 0
    mov dword [rel console_col], 0
    mov ecx, r12d
    call basix_slot_free
    jmp .out

.gui_noslot:
    call basix_compile_lock_release
    lea rcx, [rel msg_shell_run_noslot]
    call console_puts
    jmp .out
.gui_load_failed_freed:
    mov ecx, r12d
    call basix_slot_free
.gui_load_failed:
    lea rcx, [rel msg_shell_gui_notfound]
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

basix_boot_info_ptr:  dq 0

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
test_task_a_tcb: dq 0
test_task_b_tcb: dq 0
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
shell_str_compile: db 'compile', 0
shell_str_gui:      db 'gui', 0

apps_dirname: db 'APPS', 0
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
                       db '  DEL <file>  RENAME <old> <new>  TRUNCATE <file> <size>  CLEAR', 13, 10
                       db '  RUN <file.bas|file.axb>  COMPILE <source.bas> <output.axb>  GUI', 13, 10
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
msg_shell_run_usage:       db 'Usage: RUN <filename.bas|filename.axb>', 13, 10, 0
msg_shell_run_compilefail: db 'BASIX64 compile error.', 13, 10, 0
msg_shell_run_axb_stale:   db 'That .axb was compiled for a different kernel build (or is corrupt) -- recompile it.', 13, 10, 0
msg_shell_run_noslot:      db 'All 8 execution slots are busy -- close a running program first.', 13, 10, 0
msg_shell_compile_usage:      db 'Usage: COMPILE <source.bas> <output.axb>', 13, 10, 0
msg_shell_compile_ok:         db 'Compiled.', 13, 10, 0
msg_shell_compile_badpath:    db 'Bad path (a component is missing or not a directory).', 13, 10, 0
msg_shell_compile_writefail:  db 'Write failed (name may already exist).', 13, 10, 0
msg_shell_gui_notfound:       db 'WORKBENCH.BAS not found (or failed to compile).', 13, 10, 0

gui_program_name: db 'WORKBENCH.BAS', 0
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
%include "mouse.inc"
%include "basix_lexer.inc"
%include "basix_codegen.inc"
%include "basix_symbols.inc"
%include "basix_runtime.inc"
%include "png.inc"
%include "basix_parser.inc"
