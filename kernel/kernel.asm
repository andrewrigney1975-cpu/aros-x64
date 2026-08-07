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
    sti
    call paging_init

    mov rcx, rbx                        ; boot_info
    call pmm_init

    call fb_clear
    call draw_logo

    mov ecx, 4                          ; column
    mov edx, 29                          ; row
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
    mov ebx, eax                        ; ebx = allocated cluster

    mov ecx, ebx
    call exfat_bitmap_test_free
    test eax, eax
    jnz .btest_bad                      ; should now read as NOT free

    mov ecx, ebx
    call exfat_free_cluster
    test eax, eax
    jz .btest_bad

    mov ecx, ebx
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
    mov ebx, eax                        ; ebx = cluster A

    call exfat_alloc_cluster
    test eax, eax
    jz .ctest_bad
    mov esi, eax                        ; esi = cluster B

    mov ecx, ebx
    mov edx, esi
    call exfat_fat_set_entry            ; A -> B
    test eax, eax
    jz .ctest_bad

    mov ecx, esi
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry            ; B -> EOC
    test eax, eax
    jz .ctest_bad

    mov ecx, ebx
    call exfat_fat_next_cluster
    cmp eax, esi
    jne .ctest_bad

    mov ecx, esi
    call exfat_fat_next_cluster
    cmp eax, 0xFFFFFFFF
    jne .ctest_bad

    mov ecx, ebx
    xor edx, edx
    call exfat_fat_set_entry
    test eax, eax
    jz .ctest_bad
    mov ecx, esi
    xor edx, edx
    call exfat_fat_set_entry
    test eax, eax
    jz .ctest_bad

    mov ecx, ebx
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
    mov qword [r15+TCB_STACK], 0

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

    mov [r15+TCB_NEXT], r13             ; ring: main -> A -> B -> main
    mov [r13+TCB_NEXT], r14
    mov [r14+TCB_NEXT], r15

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
SHELL_TYPE_BUF_MAX equ 8191             ; exfat_test_buf is 8192 bytes; -1 for NUL

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
; shell_read_line: reads keys into shell_line_buf (NUL-terminated, up to
; SHELL_LINE_MAX-1 chars), echoing each to the console and honoring
; backspace, until Enter.
; -------------------------------------------------------------------------
shell_read_line:
    push rax
    push rcx
    push rdi

    xor ecx, ecx
    lea rdi, [rel shell_line_buf]
.loop:
    call kbd_read_char
    cmp al, 13
    je .enter
    cmp al, 8
    je .backspace
    cmp ecx, SHELL_LINE_MAX-1
    jge .loop                           ; line full -- drop the character
    mov [rdi+rcx], al
    inc ecx
    call console_putc
    jmp .loop
.backspace:
    test ecx, ecx
    jz .loop
    dec ecx
    call console_putc
    jmp .loop
.enter:
    mov byte [rdi+rcx], 0
    mov al, 13
    call console_putc
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
    call exfat_dir_list_start
.dir_loop:
    lea rcx, [rel shell_dir_name_buf]
    lea rdx, [rel shell_dir_datalen]
    call exfat_dir_list_next
    test eax, eax
    jz .out
    lea rcx, [rel shell_dir_name_buf]
    call console_puts
    lea rcx, [rel msg_shell_nl]
    call console_puts
    jmp .dir_loop

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
; shell_main: the interactive read-eval-print loop. Never returns.
; -------------------------------------------------------------------------
shell_main:
    call console_init

    lea rcx, [rel msg_shell_banner]
    call console_puts

.prompt:
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
msg_sched_ok:         db 'Scheduler: armed (main + 2 test tasks)', 13, 10, 0
msg_sched_bad:        db 'Scheduler: setup FAILED', 13, 10, 0
msg_sched_verify_ok:  db 'Scheduler: both test tasks made progress (preemption OK)', 13, 10, 0
msg_sched_verify_bad: db 'Scheduler: a test task made no progress (preemption FAILED)', 13, 10, 0
msg_task_a:           db '[taskA]', 13, 10, 0
msg_task_b:           db '[taskB]', 13, 10, 0

test_task_a_counter: dq 0
test_task_b_counter: dq 0
exfat_test_name:      db 'TEST.TXT', 0
exfat_marker:         db 'END-OF-FILE-MARKER'

align 8
exfat_find_result: times 24 db 0

align 4096
exfat_test_buf: times 8192 db 0
exfat_wtest_src:  times 6000 db 0
exfat_wtest_read: times 6000 db 0

console_col:  dd 0
console_row:  dd 0
console_cols: dd 0
console_rows: dd 0

shell_str_help:  db 'help', 0
shell_str_dir:   db 'dir', 0
shell_str_clear: db 'clear', 0
shell_str_type:  db 'type', 0
shell_str_write: db 'write', 0

msg_shell_banner:      db 'arOS-X64 shell. Type HELP for commands.', 13, 10, 0
msg_shell_prompt:      db '] ', 0
msg_shell_help:        db 'Commands: HELP  DIR  TYPE <file>  WRITE <file> <text>  CLEAR', 13, 10, 0
msg_shell_unknown:     db 'Unknown command. Type HELP for a list.', 13, 10, 0
msg_shell_nl:          db 13, 10, 0
msg_shell_type_usage:  db 'Usage: TYPE <filename>', 13, 10, 0
msg_shell_notfound:    db 'File not found.', 13, 10, 0
msg_shell_toobig:      db 'File too large to display.', 13, 10, 0
msg_shell_readfail:    db 'Error reading file.', 13, 10, 0
msg_shell_write_usage: db 'Usage: WRITE <filename> <text>', 13, 10, 0
msg_shell_write_ok:    db 'File written.', 13, 10, 0
msg_shell_write_fail:  db 'Write failed (name may already exist).', 13, 10, 0

shell_line_buf:     times SHELL_LINE_MAX db 0
shell_cmd_buf:      times 16 db 0
shell_arg_buf:       times 64 db 0
shell_dir_name_buf: times 256 db 0
shell_dir_datalen:  dq 0

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
%include "logo.inc"
%include "idt.inc"
%include "sched.inc"
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
%include "keyboard.inc"
