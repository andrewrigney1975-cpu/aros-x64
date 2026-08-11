; =============================================================================
; Minimal exFAT reader/writer, built against Microsoft's public exFAT
; specification.
;
; Write support (phase 7) so far: sector-level writes (storage.inc),
; allocation-bitmap discovery and cluster alloc/free (exfat_bitmap_mount /
; exfat_alloc_cluster / exfat_free_cluster). FAT chain writing, directory
; entry construction, and exfat_write_file are still to come.
;
; Scope: single FAT. exfat_mount looks for the
; exFAT signature at LBA 0 first (whole-disk/superfloppy volume); if
; that's not it, it tries GPT (LBA 1 signature "EFI PART", preferring a
; partition typed as Microsoft Basic Data, else the first non-empty
; entry), and finally falls back to a legacy MBR partition table at LBA 0
; -- which is what Windows' own diskpart/format actually produces on a
; fresh VHD (confirmed against a real one: LBA 0 turned out to be MBR
; boot code, not the VBR). All LBAs this file computes are relative to
; exfat_base_lba, the start of whichever of those the volume turned out
; to be at.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

; -------------------------------------------------------------------------
; exfat_mount: locates and validates the exFAT VBR, and stores the volume
; geometry. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_mount:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    xor ecx, ecx
    mov edx, 1
    lea r8, [rel exfat_vbr_buf]
    call storage_read_sectors
    test eax, eax
    jz .fail

    lea rsi, [rel exfat_vbr_buf]
    mov eax, [rsi+3]                    ; FileSystemName[0:4] = "EXFA"
    cmp eax, 0x41465845
    jne .try_gpt
    cmp byte [rsi+7], 'T'               ; FileSystemName[4] = "T"
    jne .try_gpt

    mov dword [rel exfat_base_lba], 0
    jmp .parse_vbr

.try_gpt:
    mov ecx, 1                          ; LBA 1 = GPT header
    mov edx, 1
    lea r8, [rel exfat_gpt_buf]
    call storage_read_sectors
    test eax, eax
    jz .try_mbr

    lea rsi, [rel exfat_gpt_buf]
    mov rax, [rsi+0]
    mov rbx, 0x5452415020494645         ; "EFI PART"
    cmp rax, rbx
    jne .try_mbr

    mov eax, [rsi+84]                   ; SizeOfPartitionEntry
    mov [rel exfat_gpt_entry_size], eax

    mov ecx, [rsi+72]                   ; PartitionEntryLBA (assumed < 4G)
    mov edx, 1
    lea r8, [rel exfat_gpt_buf]
    call storage_read_sectors
    test eax, eax
    jz .try_mbr

    call storage_sector_size             ; eax = the device's native sector size
    xor edx, edx
    mov ecx, [rel exfat_gpt_entry_size]
    test ecx, ecx
    jz .try_mbr
    div ecx                             ; eax = entries that fit in one sector
    mov ebx, eax                        ; ebx = loop bound

    ; Pass 1: prefer an entry typed as Microsoft Basic Data.
    lea rsi, [rel exfat_gpt_buf]
    xor edi, edi
.gpt_pass1:
    cmp edi, ebx
    jge .gpt_pass2
    mov eax, edi
    mul dword [rel exfat_gpt_entry_size]
    lea rdx, [rsi+rax]
    mov rax, [rdx+0]
    mov r9, 0x4433B9E5EBD0A0A2           ; Basic Data Partition GUID, qword 1
    cmp rax, r9
    jne .gpt_pass1_next
    mov rax, [rdx+8]
    mov r9, 0xC79926B7B668C087           ; Basic Data Partition GUID, qword 2
    cmp rax, r9
    jne .gpt_pass1_next
    mov eax, [rdx+32]                   ; StartingLBA (assumed < 4G)
    mov [rel exfat_base_lba], eax
    jmp .reread_vbr
.gpt_pass1_next:
    inc edi
    jmp .gpt_pass1

    ; Pass 2: no Basic Data entry; fall back to the first non-empty one.
.gpt_pass2:
    xor edi, edi
.gpt_pass2_loop:
    cmp edi, ebx
    jge .try_mbr                        ; nothing usable; fall back to MBR
    mov eax, edi
    mul dword [rel exfat_gpt_entry_size]
    lea rdx, [rsi+rax]
    mov rax, [rdx+0]
    mov r9, [rdx+8]
    or rax, r9
    jz .gpt_pass2_next
    mov eax, [rdx+32]
    mov [rel exfat_base_lba], eax
    jmp .reread_vbr
.gpt_pass2_next:
    inc edi
    jmp .gpt_pass2_loop

.try_mbr:
    lea rsi, [rel exfat_vbr_buf]
    cmp word [rsi+510], 0xAA55          ; MBR boot signature
    jne .fail

    mov ecx, 0                          ; scan the 4 partition table entries
.mbr_scan:
    lea rax, [rsi+0x1BE]
    mov edx, ecx
    shl edx, 4
    add rax, rdx                        ; rax = &entry[ecx]
    cmp byte [rax+4], 0                 ; partition type byte
    je .mbr_next
    mov edx, [rax+8]                    ; partition's starting LBA
    mov [rel exfat_base_lba], edx
    jmp .reread_vbr
.mbr_next:
    inc ecx
    cmp ecx, 4
    jl .mbr_scan
    jmp .fail                           ; no partition found

.reread_vbr:
    mov ecx, [rel exfat_base_lba]
    mov edx, 1
    lea r8, [rel exfat_vbr_buf]
    call storage_read_sectors
    test eax, eax
    jz .fail
    lea rsi, [rel exfat_vbr_buf]        ; may still point at exfat_gpt_buf
    mov eax, [rsi+3]                     ; from whichever path got us here
    cmp eax, 0x41465845
    jne .fail
    cmp byte [rsi+7], 'T'
    jne .fail

.parse_vbr:
    mov eax, [rsi+80]
    mov [rel exfat_fat_offset], eax
    mov eax, [rsi+84]
    mov [rel exfat_fat_length], eax
    mov eax, [rsi+88]
    mov [rel exfat_cluster_heap_offset], eax
    mov eax, [rsi+92]
    mov [rel exfat_cluster_count], eax
    mov eax, [rsi+96]
    mov [rel exfat_root_dir_cluster], eax
    mov [rel exfat_cwd_cluster], eax
    mov dword [rel exfat_cwd_depth], 0

    movzx ecx, byte [rsi+108]           ; BytesPerSectorShift
    mov [rel exfat_sector_shift], cl
    mov eax, 1
    shl eax, cl
    mov [rel exfat_bytes_per_sector], eax
    dec eax
    mov [rel exfat_sector_mask], eax    ; bytes_per_sector - 1

    movzx ecx, byte [rsi+109]           ; SectorsPerClusterShift
    mov eax, 1
    shl eax, cl
    mov [rel exfat_sectors_per_cluster], eax

    ; Every LBA/count this file computes for storage_read_sectors/
    ; storage_write_sectors is expressed in units of exFAT sectors
    ; (BytesPerSectorShift), but those calls hand that number straight to
    ; the device driver, which interprets it in units of the device's own
    ; native sector size. That's only correct if the two actually match --
    ; true for every real volume/device combination seen so far (Windows'
    ; own exFAT formatter always matches the device's reported logical
    ; sector size), but silently wrong data corruption is a much worse
    ; failure mode than refusing to mount, so check it explicitly.
    call storage_sector_size
    cmp eax, [rel exfat_bytes_per_sector]
    jne .sector_size_mismatch

    mov eax, 1
    jmp .out
.sector_size_mismatch:
    lea rcx, [rel msg_exfat_sector_mismatch]
    call serial_puts
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
; exfat_cluster_to_lba: ECX = cluster number (cluster 2 is the heap's
; first cluster). Returns EAX = absolute LBA.
; -------------------------------------------------------------------------
exfat_cluster_to_lba:
    push rcx
    push rdx
    sub ecx, 2
    mov eax, ecx
    mov edx, [rel exfat_sectors_per_cluster]
    mul edx                             ; eax = (cluster-2) * sectors/cluster
    add eax, [rel exfat_cluster_heap_offset]
    add eax, [rel exfat_base_lba]
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_read_cluster: ECX = cluster number, R8 = destination buffer
; (>= sectors_per_cluster * bytes_per_sector). Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_read_cluster:
    push rcx
    push rdx

    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    call storage_read_sectors

    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_fat_next_cluster: ECX = cluster number. Returns EAX = the next
; cluster in its chain (or an end-of-chain marker >= 0xFFFFFFF8), or 0 on
; a read error (0 is never a legal cluster number, so it's safe to use
; as this function's own failure signal).
; -------------------------------------------------------------------------
exfat_fat_next_cluster:
    push rbx
    push rcx
    push rdx
    push r8

    mov ebx, ecx                        ; ebx = cluster
    ; byte offset within the FAT = cluster*4; sector = FatOffset +
    ; offset/bytes_per_sector (the volume's own sector size, not a
    ; hardcoded 512 -- see the mount-time check in exfat_mount for why
    ; that's also required to equal the device's native sector size)
    mov eax, ebx
    shl eax, 2
    mov edx, eax
    movzx ecx, byte [rel exfat_sector_shift]
    shr edx, cl                         ; edx = sector index within the FAT
    and eax, [rel exfat_sector_mask]    ; eax = byte offset within that sector

    push rax                            ; save in-sector byte offset
    mov ecx, [rel exfat_fat_offset]
    add ecx, edx
    add ecx, [rel exfat_base_lba]
    mov edx, 1
    lea r8, [rel exfat_fat_sector_buf]
    call storage_read_sectors
    pop rdx                             ; rdx = in-sector byte offset
    test eax, eax
    jz .fail

    lea rax, [rel exfat_fat_sector_buf]
    add rax, rdx
    mov eax, [rax]
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r8
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_fat_set_entry: ECX = cluster number, EDX = value to store in its
; FAT entry (a next-cluster number, or an end-of-chain marker such as
; 0xFFFFFFFF). Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_fat_set_entry:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    mov r9d, edx                        ; save value to store
    mov ebx, ecx                        ; ebx = cluster
    mov eax, ebx
    shl eax, 2
    mov edx, eax
    movzx ecx, byte [rel exfat_sector_shift]
    shr edx, cl                         ; edx = sector index within the FAT
    and eax, [rel exfat_sector_mask]    ; eax = byte offset within that sector
    mov esi, eax                        ; esi = in-sector byte offset

    mov ecx, [rel exfat_fat_offset]
    add ecx, edx
    add ecx, [rel exfat_base_lba]
    mov ebx, ecx                        ; ebx = FAT sector LBA

    mov edx, 1
    lea r8, [rel exfat_fat_sector_buf]
    call storage_read_sectors
    test eax, eax
    jz .fail

    lea rax, [rel exfat_fat_sector_buf]
    add rax, rsi
    mov [rax], r9d

    mov rcx, rbx
    mov edx, 1
    lea r8, [rel exfat_fat_sector_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_dir_list_start: resets the directory iterator used by
; exfat_dir_list_next to the start of the current directory
; (exfat_cwd_cluster).
; -------------------------------------------------------------------------
exfat_dir_list_start:
    push rcx
    mov ecx, [rel exfat_cwd_cluster]
    call exfat_dir_list_start_at
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_dir_list_start_at: resets the directory iterator used by
; exfat_dir_list_next to the start of an explicit directory cluster.
; ECX = starting cluster. Used by TREE to descend into subdirectories
; without disturbing exfat_cwd_cluster.
; -------------------------------------------------------------------------
exfat_dir_list_start_at:
    push rax
    mov eax, ecx
    mov [rel exfat_list_cluster], eax
    mov dword [rel exfat_list_offset], 0
    pop rax
    ret

; -------------------------------------------------------------------------
; exfat_dir_list_next: iterates the current directory (as set by
; exfat_dir_list_start / exfat_dir_list_start_at), one File entry at a
; time (skipping non-File entries such as the Allocation Bitmap/Up-case
; Table/Volume Label). RCX = output buffer for the ASCII name (caller-
; sized, NUL-terminated -- names can be up to 255 chars per spec, so a
; 256-byte buffer is recommended), RDX = output pointer for a 16-byte
; struct: [0]=FirstCluster(dd), [4]=FileAttributes(dd, low byte only --
; test bit ATTR_DIRECTORY), [8]=DataLength(dq). Call exfat_dir_list_start
; (or _at) first. Returns EAX=1 with the outputs filled in, or EAX=0 once
; the directory is exhausted (safe to call again afterward -- keeps
; returning 0 without re-reading).
; -------------------------------------------------------------------------
exfat_dir_list_next:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    mov r10, rcx                        ; r10 = output name buffer
    mov r11, rdx                        ; r11 = output struct ptr

    cmp dword [rel exfat_list_cluster], 0xFFFFFFFF
    je .done

.cluster_loop:
    mov ecx, [rel exfat_list_cluster]
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .exhausted

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ebx, eax                        ; ebx = cluster size, bytes

    lea rsi, [rel exfat_dir_buf]
    mov edx, [rel exfat_list_offset]
.entry_scan:
    cmp edx, ebx
    jge .next_cluster
    movzx eax, byte [rsi+rdx]
    test al, al
    jz .exhausted                       ; end marker
    cmp al, 0x85
    jne .skip_entry

    ; Bounds check: see the matching comment in exfat_find_root_file --
    ; the whole entry set must fit in this cluster before reading its
    ; Stream Extension/FileName fields.
    movzx eax, byte [rsi+rdx+1]         ; SecondaryCount
    mov ecx, eax
    inc ecx
    shl ecx, 5
    add ecx, edx
    cmp ecx, ebx
    jg .skip_entry

    movzx r9d, byte [rsi+rdx+32+3]      ; NameLength (Stream Ext, offset+3)
    mov eax, [rsi+rdx+32+20]            ; FirstCluster (Stream Ext, offset+20)
    mov [r11+0], eax
    movzx eax, word [rsi+rdx+4]         ; FileAttributes (primary entry, +4)
    mov [r11+4], eax
    mov rax, [rsi+rdx+32+24]            ; DataLength (Stream Ext, offset+24)
    mov [r11+8], rax

    lea rdi, [rsi+rdx+64]               ; first FileName entry
    xor ecx, ecx                        ; chars copied so far
    xor r8d, r8d                        ; offset within current entry's 15 chars
.copy_loop:
    cmp ecx, r9d
    jge .copy_done
    cmp r8d, 15
    jl .copy_have_entry
    add rdi, 32                         ; next FileName entry
    xor r8d, r8d
.copy_have_entry:
    movzx eax, word [rdi+2+r8*2]
    mov [r10+rcx], al
    inc ecx
    inc r8d
    jmp .copy_loop
.copy_done:
    mov byte [r10+rcx], 0               ; NUL-terminate

    movzx eax, byte [rsi+rdx+1]         ; SecondaryCount
    inc eax
    shl eax, 5                          ; entries * 32 bytes
    add edx, eax
    mov [rel exfat_list_offset], edx

    mov eax, 1
    jmp .out

.skip_entry:
    add edx, 32
    jmp .entry_scan

.next_cluster:
    mov ecx, [rel exfat_list_cluster]
    call exfat_fat_next_cluster
    test eax, eax
    jz .exhausted
    cmp eax, 0xFFFFFFF8
    jae .exhausted
    mov [rel exfat_list_cluster], eax
    mov dword [rel exfat_list_offset], 0
    jmp .cluster_loop

.exhausted:
    mov dword [rel exfat_list_cluster], 0xFFFFFFFF
.done:
    xor eax, eax
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
; exfat_bitmap_mount: locates the volume's Allocation Bitmap directory
; entry (EntryType 0x81) in the root directory and records its first
; cluster + length. Must be called after a successful exfat_mount.
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_bitmap_mount:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r10

    mov dword [rel exfat_bitmap_cache_valid], 0

    mov ecx, [rel exfat_root_dir_cluster]
.cluster_loop:
    mov r10d, ecx
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    lea rsi, [rel exfat_dir_buf]
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ebx, eax                        ; ebx = cluster size in bytes
    xor edx, edx                        ; edx = offset within cluster
.entry_loop:
    cmp edx, ebx
    jge .next_cluster
    movzx eax, byte [rsi+rdx]
    test al, al
    jz .fail                            ; end marker: no bitmap entry found
    cmp al, 0x81
    jne .skip_entry

    mov eax, [rsi+rdx+20]               ; FirstCluster
    mov [rel exfat_bitmap_cluster], eax
    mov rax, [rsi+rdx+24]               ; DataLength
    mov [rel exfat_bitmap_length], rax
    mov eax, 1
    jmp .out

.skip_entry:
    add edx, 32
    jmp .entry_loop

.next_cluster:
    mov ecx, r10d
    call exfat_fat_next_cluster
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ecx, eax
    jmp .cluster_loop

.fail:
    xor eax, eax
.out:
    pop r10
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_bitmap_find_cluster: ECX = chain index (0-based cluster count from
; the bitmap file's first cluster). Returns EAX = actual cluster number, or
; 0 on error/premature end-of-chain. Preserves the caller's RCX.
; -------------------------------------------------------------------------
exfat_bitmap_find_cluster:
    push rbx
    push rcx

    mov ebx, [rel exfat_bitmap_cluster]
    test ecx, ecx
    jz .done
.walk:
    push rcx
    mov ecx, ebx
    call exfat_fat_next_cluster
    pop rcx
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ebx, eax
    dec ecx
    jnz .walk
.done:
    mov eax, ebx
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_bitmap_ensure_loaded: ECX = chain index of the bitmap-file cluster
; that must be resident in exfat_bitmap_buf. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_bitmap_ensure_loaded:
    push rcx
    push r8

    cmp dword [rel exfat_bitmap_cache_valid], 0
    je .miss
    cmp [rel exfat_bitmap_cache_chain_idx], ecx
    jne .miss
    mov eax, 1
    jmp .out

.miss:
    call exfat_bitmap_find_cluster      ; preserves ecx (chain idx)
    test eax, eax
    jz .fail
    mov dword [rel exfat_bitmap_cache_cluster], eax
    push rcx
    mov ecx, eax
    lea r8, [rel exfat_bitmap_buf]
    call exfat_read_cluster
    pop rcx
    test eax, eax
    jz .fail

    mov dword [rel exfat_bitmap_cache_valid], 1
    mov [rel exfat_bitmap_cache_chain_idx], ecx
    mov eax, 1
    jmp .out
.fail:
    mov dword [rel exfat_bitmap_cache_valid], 0
    xor eax, eax
.out:
    pop r8
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_bitmap_test_free: ECX = cluster number (>=2). Returns EAX=1 if the
; cluster is free (bit clear), EAX=0 if allocated or on any error (a read
; failure is treated as "not free" so a caller never hands out a cluster
; it couldn't actually verify).
; -------------------------------------------------------------------------
exfat_bitmap_test_free:
    push rbx
    push rcx
    push rdx
    push rsi

    sub ecx, 2
    mov ebx, ecx                        ; ebx = bit index
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov esi, eax                        ; esi = bitmap-file cluster size, bytes

    mov eax, ebx
    shr eax, 3                          ; eax = byte index within whole bitmap
    xor edx, edx
    div esi                             ; eax = chain idx, edx = offset in cluster
    push rdx

    mov ecx, eax
    call exfat_bitmap_ensure_loaded
    pop rdx
    test eax, eax
    jz .fail

    lea rax, [rel exfat_bitmap_buf]
    add rax, rdx
    movzx eax, byte [rax]
    mov ecx, ebx
    and ecx, 7
    shr eax, cl
    and eax, 1
    xor eax, 1                          ; invert: clear bit (free) -> eax=1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_bitmap_set_bit: ECX = cluster number, EDX = desired bit value (0/1).
; Updates the cached bitmap cluster and writes it back to disk immediately.
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_bitmap_set_bit:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r9

    mov r9d, edx                        ; save desired bit value
    sub ecx, 2
    mov ebx, ecx                        ; ebx = bit index
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov esi, eax                        ; esi = bitmap-file cluster size, bytes

    mov eax, ebx
    shr eax, 3
    xor edx, edx
    div esi                             ; eax = chain idx, edx = offset in cluster
    push rdx

    mov ecx, eax
    call exfat_bitmap_ensure_loaded
    pop rdx
    test eax, eax
    jz .fail

    lea rsi, [rel exfat_bitmap_buf]
    add rsi, rdx                        ; rsi -> target byte
    mov ecx, ebx
    and ecx, 7                          ; bit position
    mov eax, 1
    shl eax, cl                         ; eax = bit mask

    test r9d, r9d
    jz .clear_bit
    or byte [rsi], al
    jmp .write_back
.clear_bit:
    not eax
    and byte [rsi], al
.write_back:
    mov ecx, [rel exfat_bitmap_cache_cluster]
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_bitmap_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_alloc_cluster: scans the allocation bitmap for the first free
; cluster in the heap, marks it allocated, and returns it. Requires
; exfat_bitmap_mount to have succeeded first. Returns EAX = cluster number
; (>=2), or 0 if the volume is full or on error.
; -------------------------------------------------------------------------
exfat_alloc_cluster:
    push rbx
    push rcx
    push rdx

    mov ebx, 2                          ; ebx = candidate cluster
.scan:
    mov eax, [rel exfat_cluster_count]
    add eax, 2
    cmp ebx, eax
    jge .full

    mov ecx, ebx
    call exfat_bitmap_test_free
    test eax, eax
    jnz .found
    inc ebx
    jmp .scan

.found:
    mov ecx, ebx
    mov edx, 1
    call exfat_bitmap_set_bit
    test eax, eax
    jz .full                            ; write failed -- treat as no allocation
    mov eax, ebx
    jmp .out
.full:
    xor eax, eax
.out:
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_free_cluster: ECX = cluster number. Clears its bitmap bit. Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_free_cluster:
    push rdx
    xor edx, edx
    call exfat_bitmap_set_bit
    pop rdx
    ret

; -------------------------------------------------------------------------
; exfat_name_hash: RCX = ptr to a NUL-terminated ASCII name. Returns EAX =
; the 16-bit NameHash per the exFAT spec, computed over the up-cased
; UTF-16LE encoding (ASCII-only up-casing, matching exfat_name_matches's
; folding -- full Unicode up-case table is future work).
; -------------------------------------------------------------------------
exfat_name_hash:
    push rcx
    push rdx

    xor edx, edx                        ; dx = running hash accumulator
.loop:
    movzx eax, byte [rcx]
    test al, al
    jz .done
    cmp al, 'a'
    jb .no_lower
    cmp al, 'z'
    ja .no_lower
    sub al, 0x20
.no_lower:
    ror dx, 1
    add dx, ax                          ; low byte of the UTF-16LE char
    ror dx, 1                           ; high byte is always 0 for ASCII
    inc rcx
    jmp .loop
.done:
    movzx eax, dx
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_checksum_entries: RCX = ptr to the first (primary) directory entry
; of a set, EDX = number of 32-byte entries in the set. Returns EAX = the
; 16-bit SetChecksum value (bytes 2-3 of the primary entry -- the checksum
; field itself -- are excluded from the sum, per spec).
; -------------------------------------------------------------------------
exfat_checksum_entries:
    push rbx
    push rcx
    push rdx
    push rsi

    mov rsi, rcx                        ; rsi = pointer, indexed by rcx below
    mov eax, edx
    shl eax, 5
    mov ebx, eax                        ; ebx = total byte length
    xor edx, edx                        ; dx = running checksum
    xor ecx, ecx                        ; ecx = byte index
.loop:
    cmp ecx, ebx
    jge .done
    cmp ecx, 2
    je .next
    cmp ecx, 3
    je .next
    movzx eax, byte [rsi+rcx]
    ror dx, 1
    add dx, ax
.next:
    inc ecx
    jmp .loop
.done:
    movzx eax, dx
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_dir_find_slot: ECX = number of consecutive 32-byte directory
; entries needed. Finds (or makes, by extending the current directory's
; (exfat_cwd_cluster) cluster chain) a run of that many free entries. Returns EAX=1/0; on
; success EBX = the cluster containing the run, EDX = byte offset of the
; run's start within exfat_dir_buf (which is left holding that cluster's
; current on-disk content, ready for the caller to patch in place and
; write back).
; -------------------------------------------------------------------------
exfat_dir_find_slot:
    push rcx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    mov r9d, ecx                        ; r9d = entries needed

    mov ecx, [rel exfat_cwd_cluster]
.cluster_loop:
    mov r10d, ecx                       ; r10d = current cluster
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov edi, eax                        ; edi = cluster size, bytes

    lea rsi, [rel exfat_dir_buf]
    xor edx, edx                        ; edx = offset within cluster
.entry_scan:
    cmp edx, edi
    jge .this_cluster_full
    movzx eax, byte [rsi+rdx]
    test al, al
    jnz .not_free
    mov eax, r9d
    shl eax, 5
    mov ecx, edx
    add ecx, eax
    cmp ecx, edi
    jg .pad_and_extend                  ; free run starts here but won't fit
    mov ebx, r10d                       ; ebx = cluster containing the run
    jmp .found
.not_free:
    add edx, 32
    jmp .entry_scan

.pad_and_extend:
    ; A 0x00 EntryType means "nothing valid follows anywhere in the
    ; directory" to any compliant scan (exfat_find_root_file,
    ; exfat_dir_list_next) -- both stop dead the moment they see one,
    ; even if more clusters follow in the chain. If we just moved on to
    ; a new cluster and wrote there without touching this leftover
    ; free-but-too-small run, whatever we write would be permanently
    ; unreachable: every future scan would still stop right here. Pad it
    ; with a non-zero, non-0x85 filler (anything else is just skipped
    ; over, same as this scan's own .not_free path does) so scanning
    ; continues on to the next cluster instead of stopping early.
    push rcx
    push rdi
    mov ecx, edi
    sub ecx, edx
    lea rdi, [rsi+rdx]
    mov al, 0x01
    cld
    rep stosb
    pop rdi
    pop rcx

    push rcx
    push rdx
    mov ecx, r10d
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    pop rdx
    pop rcx
    test eax, eax
    jz .fail

.this_cluster_full:
    mov ecx, r10d
    call exfat_fat_next_cluster
    test eax, eax
    jz .extend
    cmp eax, 0xFFFFFFF8
    jae .extend
    mov ecx, eax
    jmp .cluster_loop

.extend:
    call exfat_alloc_cluster
    test eax, eax
    jz .fail
    mov ebx, eax                        ; ebx = newly allocated cluster
    mov ecx, r10d
    mov edx, ebx
    call exfat_fat_set_entry            ; link old last cluster -> new one
    test eax, eax
    jz .fail
    mov ecx, ebx
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry            ; terminate the new cluster
    test eax, eax
    jz .fail

    lea rdi, [rel exfat_dir_buf]
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ecx, eax
    xor eax, eax
    cld
    rep stosb

    mov ecx, ebx
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    xor edx, edx                        ; the run starts at offset 0
    jmp .found

.found:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rcx
    ret

EXFAT_MAX_NAME_LEN equ 64               ; matches shell_arg_buf's own cap
ATTR_ARCHIVE       equ 0x20
ATTR_DIRECTORY     equ 0x10

; -------------------------------------------------------------------------
; exfat_dir_write_entry_set: RCX = ptr to a NUL-terminated ASCII filename,
; <= EXFAT_MAX_NAME_LEN characters (ASCII only -- full Unicode is future
; work); EDX = FirstCluster; R8 = DataLength (qword, also used as
; ValidDataLength); R9B = Stream Extension GeneralSecondaryFlags (bit0
; AllocationPossible, bit1 NoFatChain); AL = FileAttributes (low byte --
; ATTR_ARCHIVE or ATTR_DIRECTORY). Builds and writes a File/
; StreamExtension/FileName... entry set (one FileName entry per 15
; characters of the name, per spec) in the current directory
; (exfat_cwd_cluster). Timestamps are left zeroed (future work).
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_dir_write_entry_set:
    push rbx
    push rbp
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    movzx eax, al
    push rax                            ; [rsp] = FileAttributes, until popped in .out

    mov r10, rcx                        ; r10 = name ptr
    mov r11d, edx                       ; r11d = FirstCluster
    mov r12, r8                         ; r12 = DataLength
    mov r13b, r9b                       ; r13b = GeneralSecondaryFlags

    xor r14d, r14d
.namelen:
    cmp byte [r10+r14], 0
    je .namelen_done
    inc r14d
    cmp r14d, EXFAT_MAX_NAME_LEN
    ja .fail
    jmp .namelen
.namelen_done:
    test r14d, r14d
    jz .fail

    mov eax, r14d
    add eax, 14
    mov ecx, 15
    xor edx, edx
    div ecx
    mov r15d, eax                       ; r15d = FileName entries needed

    mov ecx, r15d
    add ecx, 2                          ; + File + StreamExt
    call exfat_dir_find_slot
    test eax, eax
    jz .fail
    ; ebx = directory cluster, edx = byte offset within exfat_dir_buf
    mov ebp, ebx                        ; ebp = directory cluster (kept
                                         ; safe across ebx's later reuse
                                         ; as the FileName-entry loop index)

    lea rsi, [rel exfat_dir_buf]
    add rsi, rdx                        ; rsi -> primary (File) entry

    mov r9d, r15d
    add r9d, 2                          ; r9d = total entry count

    push rsi
    mov rdi, rsi
    xor eax, eax
    mov ecx, r9d
    shl ecx, 5                          ; total entry count * 32 bytes
    cld
    rep stosb
    pop rsi

    mov byte [rsi+0], 0x85              ; EntryType: File
    mov eax, r15d
    inc eax                             ; SecondaryCount: StreamExt + N FileName
    mov [rsi+1], al
    mov al, [rsp]                       ; FileAttributes (pushed at entry)
    mov ah, 0
    mov [rsi+4], ax                     ; FileAttributes

    mov byte [rsi+32], 0xC0             ; Stream Extension EntryType
    mov [rsi+33], r13b                  ; GeneralSecondaryFlags
    mov [rsi+35], r14b                  ; NameLength
    mov [rsi+40], r12                   ; ValidDataLength
    mov [rsi+52], r11d                  ; FirstCluster
    mov [rsi+56], r12                   ; DataLength

    mov rcx, r10
    call exfat_name_hash
    mov [rsi+36], ax                    ; NameHash

    ; FileName entries: one per 15 characters, zero-padded in the last one
    xor ebx, ebx                        ; ebx = FileName entry index
.name_entry_loop:
    cmp ebx, r15d
    jge .name_entries_done
    mov eax, ebx
    imul eax, 15                        ; eax = index of this entry's first char
    lea rdi, [rsi+64]
    mov edx, ebx
    imul edx, 32
    add rdi, rdx                        ; rdi -> this FileName entry
    mov byte [rdi], 0xC1

    xor ecx, ecx
.copy_name_chars:
    cmp ecx, 15
    jge .name_entry_next
    mov edx, eax
    add edx, ecx
    cmp edx, r14d
    jge .copy_name_chars_next           ; past the real name -- leave zero
    movzx r8d, byte [r10+rdx]
    mov [rdi+2+rcx*2], r8w
.copy_name_chars_next:
    inc ecx
    jmp .copy_name_chars
.name_entry_next:
    inc ebx
    jmp .name_entry_loop
.name_entries_done:

    mov rcx, rsi
    mov edx, r9d
    call exfat_checksum_entries
    mov [rsi+2], ax                     ; SetChecksum

    mov ecx, ebp
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    add rsp, 8                          ; discard the FileAttributes local
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbp
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_create_file: RCX = ptr to a NUL-terminated ASCII filename, <=
; EXFAT_MAX_NAME_LEN characters. Creates an empty (FirstCluster=0,
; DataLength=0) file entry in the root directory. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_create_file:
    push rdx
    push r8
    push r9

    xor edx, edx                        ; FirstCluster = 0
    xor r8, r8                          ; DataLength = 0
    mov r9b, 0x01                       ; AllocationPossible=1, NoFatChain=0
    mov al, ATTR_ARCHIVE
    call exfat_dir_write_entry_set

    pop r9
    pop r8
    pop rdx
    ret

; -------------------------------------------------------------------------
; exfat_create_dir: RCX = ptr to a NUL-terminated ASCII name, <=
; EXFAT_MAX_NAME_LEN characters. Creates a new, empty subdirectory of the
; current directory (exfat_cwd_cluster): allocates and zero-fills one
; cluster (an all-zero cluster is this codebase's existing "no entries"
; convention, same end marker every scan already respects), terminates
; its FAT chain entry, then writes a File entry set with
; FileAttributes=ATTR_DIRECTORY pointing at it. Fails (and frees the
; cluster back) if the name already exists. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_create_dir:
    push rbx
    push rcx
    push rdx
    push r8
    push r9

    push rcx
    lea r8, [rel exfat_wf_scratch]
    call exfat_find_root_file
    pop rcx
    test eax, eax
    jnz .fail                           ; already exists

    call exfat_alloc_cluster
    test eax, eax
    jz .fail
    mov ebx, eax                        ; ebx = new directory's cluster

    mov ecx, ebx
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry            ; terminate its FAT chain
    test eax, eax
    jz .fail_free

    lea rdi, [rel exfat_dir_buf]
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ecx, eax
    xor eax, eax
    cld
    rep stosb

    mov ecx, ebx
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail_free

    pop r9
    pop r8
    pop rdx
    pop rcx                             ; rcx = name ptr again

    mov edx, ebx                        ; FirstCluster
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov r8d, eax                        ; DataLength = one cluster, bytes
    mov r9b, 0x01                       ; AllocationPossible=1, NoFatChain=0
    mov al, ATTR_DIRECTORY
    call exfat_dir_write_entry_set
    jmp .out

.fail_free:
    mov ecx, ebx
    call exfat_free_cluster
.fail:
    pop r9
    pop r8
    pop rdx
    pop rcx
    xor eax, eax
.out:
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_write_file: RCX = ptr to a NUL-terminated ASCII filename (<=
; EXFAT_MAX_NAME_LEN chars), R8 = ptr to source data, R9 = data length
; in bytes. Creates a NEW file -- fails if the name already exists in
; the root directory (see exfat_append_file for extending an existing
; one). Allocates and FAT-chains enough
; clusters to hold the data (always via a real FAT chain -- NoFatChain is
; never set, so allocation need not be contiguous), writes the data, and
; writes the file's directory entry with the real FirstCluster/
; DataLength. Returns EAX=1/0; on failure partway through, any clusters
; already allocated are freed and the chain is left with no entry
; pointing at it, so nothing is left dangling that a later free-space
; scan couldn't reclaim on a fresh exfat_alloc_cluster pass regardless.
; -------------------------------------------------------------------------
exfat_write_file:
    push rbp
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r10, rcx                        ; r10 = name ptr
    mov r11, r8                         ; r11 = source data ptr (advances)
    mov r12, r9                         ; r12 = remaining bytes
    mov rbp, r9                         ; rbp = original total length (for the dir entry)
    xor r13d, r13d                      ; r13d = first_cluster (0 until set)
    xor r14d, r14d                      ; r14d = previous cluster in chain (0 until set)

    mov rcx, r10
    lea r8, [rel exfat_wf_scratch]
    call exfat_find_root_file
    test eax, eax
    jnz .fail                           ; already exists -- v1 has no overwrite support

    test r12, r12
    jz .data_done                       ; zero-length file: no clusters needed

.alloc_loop:
    test r12, r12
    jz .data_done

    call exfat_alloc_cluster
    test eax, eax
    jz .fail
    mov ebx, eax                        ; ebx = this iteration's new cluster

    test r13d, r13d
    jnz .link
    mov r13d, ebx
    jmp .after_link
.link:
    mov ecx, r14d
    mov edx, ebx
    call exfat_fat_set_entry
    test eax, eax
    jz .fail
.after_link:
    mov r14d, ebx

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov r9d, eax                        ; r9d = cluster size, bytes

    cmp r12, r9
    jbe .copylen_ok
    mov r15, r9
    jmp .copylen_set
.copylen_ok:
    mov r15, r12
.copylen_set:
    lea rdi, [rel exfat_write_buf]
    mov rsi, r11
    mov rcx, r15
    cld
    rep movsb                           ; copies r15 bytes; rdi/rsi advance

    mov rax, r9
    sub rax, r15                        ; rax = zero-pad length (0 for a full cluster)
    jz .no_pad
    mov rcx, rax
    xor eax, eax
    cld
    rep stosb                           ; rdi already positioned past the copied data
.no_pad:

    mov ecx, ebx
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_write_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    add r11, r15
    sub r12, r15
    jmp .alloc_loop

.data_done:
    test r13d, r13d
    jz .write_entry                     ; zero-length file: no chain to terminate
    mov ecx, r14d
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry
    test eax, eax
    jz .fail

.write_entry:
    mov rcx, r10
    mov edx, r13d
    mov r8, rbp                         ; DataLength = original total length
    mov r9b, 0x01                       ; AllocationPossible=1, NoFatChain=0
    mov al, ATTR_ARCHIVE
    call exfat_dir_write_entry_set
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rbp
    ret

; -------------------------------------------------------------------------
; exfat_name_matches: compares a directory FileName-entry run against an
; ASCII target name, case-insensitively (ASCII a-z/A-Z folding only --
; full Unicode case folding via the volume's up-case table is future work).
;   RCX = pointer to the first 0xC1 FileName entry
;   R8D = number of consecutive 0xC1 entries making up the full name
;   R11 = pointer to the NUL-terminated ASCII target name
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_name_matches:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r9

    mov rsi, r11                        ; rsi = target ptr
    mov rdi, rcx                        ; rdi = current 0xC1 entry ptr
    mov r9d, r8d                        ; r9d = remaining FileName entries

.next_entry:
    test r9d, r9d
    jz .check_end
    cmp byte [rdi], 0xC1
    jne .fail
    lea rbx, [rdi+2]                    ; UTF-16 chars start (15 chars max)
    mov ecx, 15
.char_loop:
    movzx eax, word [rbx]
    test al, al
    jz .name_ended                      ; zero padding -> end of actual name
    cmp al, 'a'
    jb .no_lower1
    cmp al, 'z'
    ja .no_lower1
    sub al, 0x20
.no_lower1:
    movzx edx, byte [rsi]
    test dl, dl
    jz .fail
    cmp dl, 'a'
    jb .no_lower2
    cmp dl, 'z'
    ja .no_lower2
    sub dl, 0x20
.no_lower2:
    cmp al, dl
    jne .fail
    inc rsi
    add rbx, 2
    dec ecx
    jnz .char_loop

    add rdi, 32
    dec r9d
    jmp .next_entry

.name_ended:
.check_end:
    cmp byte [rsi], 0
    jne .fail
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_find_root_file: scans the root directory (following its FAT chain)
; for a File entry whose name matches.
;   RCX = pointer to the NUL-terminated ASCII target name
;   R8  = pointer to a result struct: [0]=FirstCluster(dd), [8]=DataLength(dq),
;         [16]=NoFatChain(dd, nonzero if the file is contiguously allocated
;         with no real FAT chain -- common for freshly-written files, and
;         confirmed against a real Windows-written exFAT volume),
;         [24]=EntryDirCluster(dd), [28]=EntryOffset(dd) -- the on-disk
;         location of the File entry itself, for callers that need to
;         edit or delete it in place (exfat_delete_file/exfat_rename_file/
;         exfat_truncate_file/exfat_append_file); [32]=FileAttributes(dd,
;         low byte only -- test bit ATTR_DIRECTORY to tell a folder from
;         a regular file). Needs a 36-byte buffer; older callers that
;         only care about the first three fields can still pass a
;         smaller one as long as nothing else follows it in memory, but
;         every caller in this codebase uses the same shared, 36-byte
;         exfat_find_result buffer.
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_find_root_file:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r9
    push r10
    push r11
    push r12

    mov r12, r8                         ; r12 = result output ptr
    mov r11, rcx                        ; r11 = target name ptr

    mov ecx, [rel exfat_cwd_cluster]
.cluster_loop:
    mov r10d, ecx                       ; r10d = current cluster
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .not_found

    lea rsi, [rel exfat_dir_buf]
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ebx, eax                        ; ebx = cluster size in bytes
    xor edi, edi                        ; edi = offset within cluster
.entry_loop:
    cmp edi, ebx
    jge .next_cluster

    movzx eax, byte [rsi+rdi]
    test al, al
    jz .not_found                       ; end marker: no more entries anywhere
    cmp al, 0x85
    jne .skip_entry

    movzx r8d, byte [rsi+rdi+1]         ; SecondaryCount

    ; Bounds check: the WHOLE entry set (this File entry plus its
    ; SecondaryCount secondary entries) must fit within this cluster --
    ; NOT just "edi < ebx" (already implied by the loop-top check, so
    ; checking it again here was a no-op bug: it never caught the real
    ; case, a File entry landing near a cluster's end whose Stream
    ; Extension/FileName entries would be read from one cluster too
    ; early, silently matching against garbage). Cross-cluster entry
    ; sets are legal per spec but unsupported here -- skip rather than
    ; read out of bounds.
    mov eax, r8d
    inc eax
    shl eax, 5
    add eax, edi
    cmp eax, ebx
    jg .skip_entry

    lea rdx, [rsi+rdi+32]               ; rdx -> Stream Extension entry
    cmp byte [rdx], 0xC0
    jne .skip_entry

    lea rcx, [rsi+rdi+64]               ; rcx -> first 0xC1 FileName entry
    dec r8d                             ; -1 for the Stream Extension entry
    call exfat_name_matches
    test eax, eax
    jz .skip_entry

    mov eax, [rdx+20]                   ; FirstCluster
    mov [r12+0], eax
    mov rax, [rdx+24]                   ; DataLength
    mov [r12+8], rax
    movzx eax, byte [rdx+1]             ; GeneralSecondaryFlags
    and eax, 0x02                       ; bit1 = NoFatChain
    mov [r12+16], eax
    mov [r12+24], r10d                  ; EntryDirCluster (for delete/rename)
    mov [r12+28], edi                   ; EntryOffset within that cluster
    movzx eax, byte [rsi+rdi+4]         ; FileAttributes (primary entry, +4)
    mov [r12+32], eax
    mov eax, 1
    jmp .out

.skip_entry:
    add edi, 32
    jmp .entry_loop

.next_cluster:
    mov ecx, r10d
    call exfat_fat_next_cluster
    test eax, eax
    jz .not_found
    cmp eax, 0xFFFFFFF8
    jae .not_found
    mov ecx, eax
    jmp .cluster_loop

.not_found:
    xor eax, eax
.out:
    pop r12
    pop r11
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_read_file: RCX = first cluster, RDX = data length (bytes),
; R8 = destination buffer, R9 = NoFatChain flag (nonzero: the file is
; contiguously allocated and its FAT entries are unused -- just walk
; cluster, cluster+1, cluster+2, ... instead of consulting the FAT. This
; is the common case for a freshly-written, unfragmented file; confirmed
; against a real Windows-written exFAT volume, where the FAT entries for
; such a file are genuinely left at 0.).
; Reads the file until RDX bytes have been copied. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_read_file:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r9
    push r10
    push r11

    mov r11, r9                         ; r11 = NoFatChain flag (save before
                                         ; r9 gets reused below)
    mov r9, rdx                         ; r9 = remaining bytes
    mov r10, r8                         ; r10 = current destination pointer
    mov ebx, ecx                        ; ebx = current cluster

.loop:
    test r9, r9
    jz .done
    mov ecx, ebx
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov rcx, r9
    cmp rcx, rax
    jbe .copy_amt_ok
    mov rcx, rax
.copy_amt_ok:
    push rcx
    lea rsi, [rel exfat_dir_buf]
    mov rdi, r10
    cld
    rep movsb
    pop rcx

    add r10, rcx
    sub r9, rcx
    test r9, r9
    jz .done

    test r11d, r11d
    jz .use_fat
    inc ebx                             ; NoFatChain: next cluster is just +1
    jmp .loop
.use_fat:
    mov ecx, ebx
    call exfat_fat_next_cluster
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ebx, eax
    jmp .loop

.done:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r11
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_chain_advance: ECX = starting cluster, EDX = number of steps
; forward, R10D = NoFatChain flag. Returns EAX = the cluster reached
; (ECX itself if EDX==0), or 0 on error (ran off the end of the chain).
; NoFatChain files are contiguous, so this is just cluster+steps; a real
; FAT chain is walked one exfat_fat_next_cluster call per step.
; -------------------------------------------------------------------------
exfat_chain_advance:
    push rbx
    push rcx
    push rdx

    mov ebx, ecx                        ; ebx = current cluster
    test r10d, r10d
    jz .use_fat_steps
    add ebx, edx                        ; NoFatChain: contiguous
    jmp .done
.use_fat_steps:
    test edx, edx
    jz .done
.step:
    mov ecx, ebx
    call exfat_fat_next_cluster
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ebx, eax
    dec edx
    jnz .step
.done:
    mov eax, ebx
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_read_file_at: RCX = first cluster, RDX = starting byte offset
; within the file, R8 = byte count to read, R9 = destination buffer,
; R10D = NoFatChain flag. A sibling of exfat_read_file that can start
; mid-file -- used by the BASIX64 compiler's streaming lexer (RUN no
; longer needs the whole program resident in one buffer). Uses
; exfat_chain_advance to skip to the cluster containing the start
; offset, then runs the same read-cluster/copy/advance loop
; exfat_read_file already uses, except the first cluster's copy starts
; partway through it (at the offset within that cluster) instead of at
; its beginning. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_read_file_at:
    push rbx
    push rbp
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r15d, r10d                      ; r15d = NoFatChain flag
    mov r14, r8                         ; r14 = remaining bytes to read
    mov r13, r9                         ; r13 = current destination ptr

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov r12d, eax                       ; r12d = bytes per cluster

    ; clusters_to_skip = start_offset / bytes_per_cluster,
    ; sub_offset = start_offset mod bytes_per_cluster
    mov eax, edx                        ; start offset (32 bits is ample
                                         ; for a BASIX64 source file)
    xor edx, edx
    div r12d
    mov r11d, eax                       ; r11d = clusters_to_skip
    mov ebp, edx                        ; ebp = sub-offset, first
                                         ; cluster of this read only --
                                         ; zeroed after that cluster

    mov edx, r11d
    mov r10d, r15d
    call exfat_chain_advance            ; ECX (first cluster) already
                                         ; holds the right argument
    test eax, eax
    jz .fail                            ; 0 = ran off the end (cluster
                                         ; 0 is never a valid data
                                         ; cluster in exFAT)
    mov ebx, eax                        ; ebx = starting cluster

.loop:
    test r14, r14
    jz .done
    mov ecx, ebx
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov eax, r12d
    sub eax, ebp                        ; room left in this cluster
    mov rcx, r14
    cmp rcx, rax
    jbe .copy_amt_ok
    mov rcx, rax
.copy_amt_ok:
    push rcx
    lea rsi, [rel exfat_dir_buf]
    add rsi, rbp
    mov rdi, r13
    cld
    rep movsb
    pop rcx

    add r13, rcx
    sub r14, rcx
    xor ebp, ebp                        ; only the first cluster of
                                         ; this read has a sub-offset
    test r14, r14
    jz .done

    test r15d, r15d
    jz .use_fat
    inc ebx                             ; NoFatChain: contiguous
    jmp .loop
.use_fat:
    mov ecx, ebx
    call exfat_fat_next_cluster
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ebx, eax
    jmp .loop

.done:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbp
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_free_cluster_run: ECX = starting cluster, R9 = count (>=1),
; R10D = NoFatChain flag. Frees exactly `count` consecutive clusters
; starting at ECX (contiguous +1 for NoFatChain, or following the real
; FAT chain otherwise). Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_free_cluster_run:
    push rbx
    push rcx
    push r9

    mov ebx, ecx                        ; ebx = current cluster
.loop:
    test r9, r9
    jz .ok
    mov ecx, ebx
    call exfat_free_cluster
    test eax, eax
    jz .fail
    dec r9
    test r9, r9
    jz .ok

    test r10d, r10d
    jz .use_fat
    inc ebx
    jmp .loop
.use_fat:
    mov ecx, ebx
    call exfat_fat_next_cluster
    test eax, eax
    jz .fail
    cmp eax, 0xFFFFFFF8
    jae .fail
    mov ebx, eax
    jmp .loop

.ok:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_data_length_to_clusters: RDX = data length in bytes. Returns
; EAX = ceil(length / cluster_size), or 0 for a zero length.
; -------------------------------------------------------------------------
exfat_data_length_to_clusters:
    push rcx
    push rdx

    test rdx, rdx
    jz .zero
    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov ecx, eax
    mov rax, rdx
    add rax, rcx
    dec rax
    xor edx, edx
    div rcx
    jmp .out
.zero:
    xor eax, eax
.out:
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; exfat_free_file_clusters: ECX = FirstCluster, RDX = DataLength (bytes),
; R9D = NoFatChain flag. Frees every cluster belonging to a file. No-op
; if FirstCluster==0 (an empty file with nothing allocated). Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_free_file_clusters:
    push rbx
    push rcx
    push rdx
    push r9
    push r10

    test ecx, ecx
    jz .ok                               ; empty file: nothing to free
    mov ebx, ecx                         ; ebx = FirstCluster
    mov r10d, r9d                        ; r10d = NoFatChain flag

    call exfat_data_length_to_clusters
    test eax, eax
    jnz .have_count
    mov eax, 1                           ; a nonzero FirstCluster always
                                          ; owns >=1 cluster even if
                                          ; DataLength somehow reads 0
.have_count:
    mov r9, rax

    mov ecx, ebx
    call exfat_free_cluster_run
    jmp .out

.ok:
    mov eax, 1
    jmp .out
.out:
    pop r10
    pop r9
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_mark_entries_unused: EBX = directory cluster (already loaded into
; exfat_dir_buf, e.g. by a prior exfat_find_root_file call), EDX = byte
; offset of the primary File entry within it. Clears the InUse bit
; (0x80) of the File entry and every one of its secondary entries, then
; writes the cluster back. Used by delete (permanently) and rename
; (immediately followed by writing a fresh entry set elsewhere).
; Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_mark_entries_unused:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8

    lea rsi, [rel exfat_dir_buf]
    add rsi, rdx                        ; rsi -> primary (File) entry

    movzx ecx, byte [rsi+1]             ; SecondaryCount
    inc ecx                             ; + the primary entry itself
    xor r8d, r8d
.clear_loop:
    cmp r8d, ecx
    jge .clear_done
    mov eax, r8d
    shl eax, 5
    and byte [rsi+rax], 0x7F            ; clear InUse (bit 7)
    inc r8d
    jmp .clear_loop
.clear_done:

    mov ecx, ebx
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors

    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_delete_file: RCX = ptr to a NUL-terminated ASCII filename. Frees
; the file's data clusters and marks its directory entries unused.
; Returns EAX=1/0 (0 if the file doesn't exist, or on I/O failure).
; -------------------------------------------------------------------------
exfat_delete_file:
    push rbx
    push rcx
    push rdx
    push r8
    push r9

    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .fail                           ; DEL refuses to target a folder

    ; exfat_dir_buf still holds the cluster exfat_find_root_file just
    ; matched in (it stopped scanning right there), so mark it in place
    ; before anything else has a chance to reuse that buffer.
    mov ebx, [rel exfat_find_result+24]  ; EntryDirCluster
    mov edx, [rel exfat_find_result+28]  ; EntryOffset
    call exfat_mark_entries_unused
    test eax, eax
    jz .fail

    mov ecx, [rel exfat_find_result+0]   ; FirstCluster
    mov rdx, [rel exfat_find_result+8]   ; DataLength
    mov r9d, [rel exfat_find_result+16]  ; NoFatChain
    call exfat_free_file_clusters
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_delete_dir: RCX = ptr to a NUL-terminated ASCII directory name (a
; direct child of the current directory, exfat_cwd_cluster). Fails if
; the name doesn't exist, isn't a directory, or the directory is not
; empty. Frees its data cluster(s) and marks its own directory entry
; unused in the parent (does not touch exfat_cwd_cluster). Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_delete_dir:
    push rbx
    push rcx
    push rdx
    push r8
    push r9

    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jz .fail                            ; not a directory

    ; Emptiness check: list the target directory's own contents. This
    ; borrows the single-instance exfat_list_cluster/exfat_list_offset
    ; iterator, which exfat_find_root_file above never touches, so it's
    ; safe here before anything is committed.
    mov ecx, [rel exfat_find_result+0]  ; target's FirstCluster
    call exfat_dir_list_start_at
    lea rcx, [rel exfat_rmdir_name_buf]
    lea rdx, [rel exfat_wf_scratch]
    call exfat_dir_list_next
    test eax, eax
    jnz .fail                           ; not empty

    ; exfat_dir_buf no longer holds the parent's cluster (dir_list_next
    ; just read the target directory's own cluster instead) -- re-read
    ; the parent's cluster before marking the entry unused there.
    mov ecx, [rel exfat_find_result+24] ; EntryDirCluster
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov ebx, [rel exfat_find_result+24] ; EntryDirCluster
    mov edx, [rel exfat_find_result+28] ; EntryOffset
    call exfat_mark_entries_unused
    test eax, eax
    jz .fail

    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    mov rdx, [rel exfat_find_result+8]  ; DataLength
    mov r9d, [rel exfat_find_result+16] ; NoFatChain
    call exfat_free_file_clusters
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_rename_file: RCX = old NUL-terminated ASCII name, RDX = new one
; (<= EXFAT_MAX_NAME_LEN chars). Fails if the old name doesn't exist or
; the new one already does. The file's data (FirstCluster/DataLength/
; NoFatChain) is carried over unchanged -- only the directory entries
; change, by deleting the old entry set and writing a fresh one (the new
; name may need a different number of FileName entries than the old one,
; so editing in place isn't generally possible). Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_rename_file:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    push r9
    push r10
    push r11

    mov r10, rcx                        ; r10 = old name
    mov r11, rdx                        ; r11 = new name

    mov rcx, r11
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .fail                           ; new name already exists

    mov rcx, r10
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail                            ; old name doesn't exist
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .fail                           ; RENAME refuses to target a folder

    mov ebx, [rel exfat_find_result+0]  ; ebx = FirstCluster (saved --
                                         ; exfat_mark_entries_unused
                                         ; doesn't touch it, but the
                                         ; upcoming dir_find_slot call
                                         ; will overwrite exfat_dir_buf)
    mov rsi, [rel exfat_find_result+8]  ; rsi = DataLength
    mov r9d, [rel exfat_find_result+16] ; r9d = NoFatChain

    push rbx
    push rsi
    push r9

    mov ebx, [rel exfat_find_result+24] ; EntryDirCluster
    mov edx, [rel exfat_find_result+28] ; EntryOffset
    call exfat_mark_entries_unused
    test eax, eax
    jnz .marked
    add rsp, 24
    jmp .fail
.marked:

    pop r9                              ; NoFatChain
    pop r8                              ; DataLength (into r8 for the call below)
    pop rdx                             ; FirstCluster (into edx for the call below)

    mov ecx, r9d                        ; GeneralSecondaryFlags: r9d is
                                         ; already bit1-shaped (0x00 or
                                         ; 0x02, see exfat_find_root_file's
                                         ; "and eax, 0x02" when it captured
                                         ; this), NOT a plain boolean -- so
                                         ; just OR in AllocationPossible
                                         ; (bit0) whenever there's any data,
                                         ; no shift.
    test rdx, rdx
    jz .no_alloc_bit
    or ecx, 0x01
.no_alloc_bit:
    mov r9b, cl

    mov rcx, r11                        ; new name
    mov al, ATTR_ARCHIVE                ; renaming a directory is rejected
                                         ; before reaching this point
    call exfat_dir_write_entry_set
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; exfat_move_file: RCX = ptr to a NUL-terminated ASCII filename, RDX =
; ptr to a NUL-terminated ASCII name of a subdirectory of the current
; directory (exfat_cwd_cluster) to move it into. Fails if the filename
; doesn't exist in the current directory, is itself a directory, the
; destination name doesn't exist or isn't a directory, or a file of the
; same name already exists in the destination. The file's data
; (FirstCluster/DataLength/NoFatChain) is carried over unchanged -- a
; fresh entry set is written in the destination FIRST, and only once
; that succeeds is the original entry set removed, so a failure before
; that point leaves the source file untouched. (A failure removing the
; original entry, after the destination write already succeeded, leaves
; a duplicate rather than losing data -- same risk tolerance as the rest
; of this codebase's multi-step directory-entry operations.) Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_move_file:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r10, rcx                        ; r10 = filename
    mov r11, rdx                        ; r11 = destination folder name
    mov r12d, [rel exfat_cwd_cluster]   ; r12d = original cwd cluster

    mov rcx, r11
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail                            ; destination doesn't exist
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jz .fail                            ; destination isn't a directory
    mov r13d, [rel exfat_find_result+0] ; r13d = destination's FirstCluster

    mov rcx, r10
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail                            ; source doesn't exist
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .fail                           ; MOVE refuses to target a folder

    mov r14d, [rel exfat_find_result+0] ; r14d = source's FirstCluster
    mov r15, [rel exfat_find_result+8]  ; r15 = source's DataLength
    mov ebx, [rel exfat_find_result+16] ; ebx = source's NoFatChain
    mov r8d, [rel exfat_find_result+24] ; source's EntryDirCluster
    mov r9d, [rel exfat_find_result+28] ; source's EntryOffset
    push r8
    push r9

    ; Switch into the destination directory to check for a name
    ; collision and, if clear, write the moved entry there.
    mov [rel exfat_cwd_cluster], r13d

    mov rcx, r10
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jnz .collision                      ; name already exists in destination

    mov ecx, ebx                        ; GeneralSecondaryFlags: ebx is
                                         ; already bit1-shaped (0x00 or
                                         ; 0x02, see exfat_find_root_file's
                                         ; "and eax, 0x02" when it captured
                                         ; this), NOT a plain boolean -- so
                                         ; just OR in AllocationPossible
                                         ; (bit0) whenever there's any data,
                                         ; no shift.
    test r14d, r14d
    jz .no_alloc_bit
    or ecx, 0x01
.no_alloc_bit:
    mov r9b, cl

    mov rcx, r10                        ; filename
    mov edx, r14d                       ; FirstCluster
    mov r8, r15                         ; DataLength
    mov al, ATTR_ARCHIVE
    call exfat_dir_write_entry_set
    test eax, eax
    jz .restore_fail

    ; Written successfully -- restore CWD, then remove the original entry.
    mov [rel exfat_cwd_cluster], r12d

    pop r9                               ; EntryOffset
    pop r8                               ; EntryDirCluster
    mov ecx, r8d
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov ebx, ecx                        ; ebx = EntryDirCluster
    mov edx, r9d                        ; edx = EntryOffset
    call exfat_mark_entries_unused
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out

.collision:
.restore_fail:
    mov [rel exfat_cwd_cluster], r12d
    pop r9
    pop r8
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
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
; exfat_truncate_file: RCX = ptr to a NUL-terminated ASCII filename,
; RDX = new length in bytes (must be <= the file's current DataLength --
; this only shrinks; see exfat_append_file to grow). Frees whichever
; trailing clusters the new length no longer needs and updates the
; Stream Extension entry's DataLength/ValidDataLength in place. Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_truncate_file:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12

    mov r11, rdx                        ; r11 = new length
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .fail                           ; TRUNCATE refuses to target a folder

    mov r10, [rel exfat_find_result+8]  ; r10 = current DataLength
    cmp r11, r10
    ja .fail                           ; growing isn't this function's job

    mov rdx, r11
    call exfat_data_length_to_clusters
    mov r12d, eax                       ; r12d = clusters to keep

    mov ebx, [rel exfat_find_result+0]  ; ebx = FirstCluster
    mov r9d, [rel exfat_find_result+16] ; r9d = NoFatChain
    test ebx, ebx
    jz .resize_entry                    ; already empty -- nothing to free
    test r12d, r12d
    jz .free_all

    mov rdx, r10
    call exfat_data_length_to_clusters
    mov esi, eax                        ; esi = clusters currently used
    cmp r12d, esi
    jae .resize_entry                   ; shrinking within the last kept
                                         ; cluster -- nothing to free

    ; Find the first cluster to free BEFORE touching any FAT links --
    ; walking r12d steps from FirstCluster needs the very link that
    ; terminating the chain (below) would destroy if done first.
    mov r10d, r9d                       ; exfat_chain_advance's NoFatChain input
    mov ecx, ebx
    mov edx, r12d
    call exfat_chain_advance            ; eax = first cluster to free
    test eax, eax
    jz .fail
    mov r8d, eax                        ; r8d = first cluster to free (kept
                                         ; safe across the calls below)

    ; terminate the chain at the last kept cluster (real FAT chains only
    ; -- a NoFatChain file just relies on DataLength to bound how much
    ; of its contiguous run is "the file", no in-chain marker to fix)
    test r9d, r9d
    jnz .free_tail
    mov ecx, ebx
    mov edx, r12d
    dec edx
    mov r10d, r9d
    call exfat_chain_advance
    test eax, eax
    jz .fail
    mov ecx, eax
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry
    test eax, eax
    jz .fail

.free_tail:
    mov ecx, r8d
    mov r9d, esi
    sub r9d, r12d                       ; r9d = clusters to free
    mov r10d, [rel exfat_find_result+16] ; NoFatChain, for the run-freer
    call exfat_free_cluster_run
    test eax, eax
    jz .fail
    jmp .resize_entry

.free_all:
    mov ecx, ebx
    mov rdx, r10
    mov r9d, [rel exfat_find_result+16]
    call exfat_free_file_clusters
    test eax, eax
    jz .fail
    xor ebx, ebx                        ; FirstCluster becomes 0

.resize_entry:
    mov ecx, [rel exfat_find_result+24] ; EntryDirCluster
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster             ; re-fetch: earlier calls in this
                                         ; function may have reused the buffer
    test eax, eax
    jz .fail

    mov edx, [rel exfat_find_result+28] ; EntryOffset
    lea rsi, [rel exfat_dir_buf]
    add rsi, rdx                        ; rsi -> File entry (Stream
                                         ; Extension fields below are at
                                         ; rsi+N, matching the layout
                                         ; exfat_dir_write_entry_set uses
                                         ; -- rsi+32 is where the Stream
                                         ; Extension entry itself starts,
                                         ; but its FIELDS are addressed
                                         ; from rsi, not from rsi+32)

    test r12d, r12d
    jnz .keep_first_cluster
    mov dword [rsi+52], 0               ; FirstCluster = 0 (fully truncated)
    jmp .set_lengths
.keep_first_cluster:
    mov eax, [rel exfat_find_result+0]
    mov [rsi+52], eax                   ; FirstCluster unchanged
.set_lengths:
    mov [rsi+40], r11                   ; ValidDataLength
    mov [rsi+56], r11                   ; DataLength

    movzx ecx, byte [rsi+1]
    inc ecx                             ; SecondaryCount + primary entry
    mov edx, ecx
    mov rcx, rsi
    call exfat_checksum_entries
    mov [rsi+2], ax

    mov ecx, [rel exfat_find_result+24]
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r12
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
; exfat_append_file: RCX = ptr to a NUL-terminated ASCII filename
; (must already exist -- use exfat_write_file to create), R8 = ptr to
; data to append, R9 = length in bytes. Fills any unused space in the
; file's last cluster first, then allocates and FAT-chains new clusters
; for the rest. If the file was NoFatChain (contiguously allocated, no
; real FAT links -- the common case for a freshly-written file) and new
; clusters are needed, the existing run is first retrofitted with real
; FAT links (a newly allocated cluster has no reason to land adjacent to
; the old run) and the flag is cleared; if everything still fits in
; existing + newly *contiguous* space this retrofit is skipped. Returns
; EAX=1/0.
; -------------------------------------------------------------------------
exfat_append_file:
    push rbx
    push rbp
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r13, r8                         ; r13 = source data ptr (advances)
    mov r14, r9                         ; r14 = remaining bytes (consumed)
    mov rbp, r9                         ; rbp = original append length (kept)
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail                            ; appending to a nonexistent file
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jnz .fail                           ; APPEND refuses to target a folder

    mov ebx, [rel exfat_find_result+0]  ; ebx = FirstCluster
    mov r15, [rel exfat_find_result+8]  ; r15 = old DataLength
    mov r10d, [rel exfat_find_result+16]; r10d = NoFatChain

    mov eax, [rel exfat_sectors_per_cluster]
    imul eax, [rel exfat_bytes_per_sector]
    mov r12d, eax                       ; r12d = cluster size, bytes

    test r14, r14
    jz .append_done_no_data             ; appending zero bytes: no-op

    test ebx, ebx
    jz .no_last_cluster                 ; empty file: nothing to top up

    ; find the last existing cluster and how much of it is already used
    mov rdx, r15
    call exfat_data_length_to_clusters
    mov esi, eax                        ; esi = clusters currently used
    mov ecx, ebx
    mov edx, esi
    dec edx
    call exfat_chain_advance
    test eax, eax
    jz .fail
    mov edi, eax                        ; edi = last existing cluster

    mov eax, r15d
    xor edx, edx
    mov ecx, r12d
    div ecx                             ; edx = bytes used in the last cluster
    test edx, edx
    jz .have_last_cluster               ; exactly full -- nothing to top up,
                                         ; edi (set above) is already correct

    mov eax, r12d
    sub eax, edx                        ; eax = leftover space in last cluster
    cmp rax, r14
    jbe .topup_amt_ok
    mov rax, r14
.topup_amt_ok:
    push rax                            ; save top-up amount

    mov ecx, edi
    lea r8, [rel exfat_write_buf]
    call exfat_read_cluster
    test eax, eax
    jz .topup_fail_pop

    pop rax                             ; top-up amount
    push rax
    mov r9d, edi                        ; save the cluster number -- the
                                         ; copy below clobbers edi (used as
                                         ; the destination pointer)
    lea rdi, [rel exfat_write_buf]
    add rdi, rdx                        ; rdx still holds bytes-used-in-cluster
    mov rsi, r13
    mov rcx, rax
    cld
    rep movsb

    mov ecx, r9d
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_write_buf]
    call storage_write_sectors
    test eax, eax
    jz .topup_fail_pop
    mov edi, r9d                        ; restore -- still "the last
                                         ; existing cluster" for the
                                         ; alloc_loop below, if reached

    pop rax                             ; top-up amount
    add r13, rax
    sub r14, rax
    jmp .have_last_cluster
.topup_fail_pop:
    add rsp, 8
    jmp .fail

.no_last_cluster:
    xor edi, edi                        ; no cluster to link the first
                                         ; new one from
.have_last_cluster:
    test r14, r14
    jz .update_entry                    ; the top-up satisfied everything

    ; more data remains: allocate new clusters. If the file was
    ; NoFatChain, retrofit real FAT links across the existing run first
    ; (a new cluster has no reason to land contiguous with it) -- skip
    ; entirely if there was no existing run (ebx==0) or none is needed.
    test ebx, ebx
    jz .alloc_loop
    test r10d, r10d
    jz .alloc_loop                      ; already a real chain
    mov esi, ebx                        ; esi = retrofit walker
    mov rdx, [rel exfat_find_result+8]  ; original DataLength, full qword
    call exfat_data_length_to_clusters
    mov r9d, eax                        ; r9d = clusters in the old run
    dec r9d
    test r9d, r9d
    jz .retrofit_done                   ; only one cluster -- no links needed
.retrofit_loop:
    lea edx, [esi+1]                    ; edx = next cluster (link target)
    mov ecx, esi                        ; ecx = current cluster (link source)
    push rdx
    call exfat_fat_set_entry
    pop rdx
    test eax, eax
    jz .fail
    mov esi, edx
    dec r9d
    jnz .retrofit_loop
.retrofit_done:
    xor r10d, r10d                      ; no longer NoFatChain

.alloc_loop:
    test r14, r14
    jz .update_entry

    call exfat_alloc_cluster
    test eax, eax
    jz .fail
    mov r9d, eax                        ; r9d = this new cluster

    test edi, edi
    jz .first_new_cluster
    push r9
    mov ecx, edi
    mov edx, r9d
    call exfat_fat_set_entry
    pop r9
    test eax, eax
    jz .fail
    jmp .after_link
.first_new_cluster:
    mov ebx, r9d                        ; file had no clusters at all: this
                                         ; new one becomes FirstCluster
.after_link:
    mov edi, r9d

    mov r8d, r12d                       ; r8d = cluster size
    cmp r14, r8
    jbe .copylen_ok
    mov r11, r8
    jmp .copylen_set
.copylen_ok:
    mov r11, r14
.copylen_set:
    lea rdi, [rel exfat_write_buf]
    mov rsi, r13
    mov rcx, r11
    cld
    rep movsb

    mov rax, r8
    sub rax, r11
    jz .no_pad
    mov rcx, rax
    xor eax, eax
    cld
    rep stosb
.no_pad:
    mov edi, r9d                        ; restore edi = current cluster
                                         ; (rep stosb clobbered it)

    mov ecx, r9d
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_write_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    add r13, r11
    sub r14, r11
    jmp .alloc_loop

.update_entry:
    test ebx, ebx
    jz .append_done_no_data             ; nothing was ever allocated
    ; terminate the chain at the final cluster if it's a real FAT chain
    test r10d, r10d
    jnz .no_terminate
    mov ecx, edi
    mov edx, 0xFFFFFFFF
    call exfat_fat_set_entry
    test eax, eax
    jz .fail
.no_terminate:

    mov ecx, [rel exfat_find_result+24] ; EntryDirCluster
    lea r8, [rel exfat_dir_buf]
    call exfat_read_cluster
    test eax, eax
    jz .fail

    mov edx, [rel exfat_find_result+28] ; EntryOffset
    lea rsi, [rel exfat_dir_buf]
    add rsi, rdx                        ; rsi -> File entry (Stream
                                         ; Extension fields are at rsi+N,
                                         ; matching exfat_dir_write_entry_set)

    mov [rsi+52], ebx                   ; FirstCluster (may have just been set)
    mov rax, r15                        ; original DataLength
    add rax, rbp                        ; + the total length appended
    mov [rsi+40], rax                   ; ValidDataLength
    mov [rsi+56], rax                   ; DataLength

    movzx ecx, byte [rsi+33]            ; GeneralSecondaryFlags
    and ecx, 0xFD                       ; clear NoFatChain (bit1)
    test r10d, r10d
    jz .flags_set
    or ecx, 0x02
.flags_set:
    or ecx, 0x01                        ; AllocationPossible
    mov [rsi+33], cl

    movzx ecx, byte [rsi+1]
    inc ecx
    mov edx, ecx
    mov rcx, rsi
    call exfat_checksum_entries
    mov [rsi+2], ax

    mov ecx, [rel exfat_find_result+24]
    call exfat_cluster_to_lba
    mov rcx, rax
    mov edx, [rel exfat_sectors_per_cluster]
    lea r8, [rel exfat_dir_buf]
    call storage_write_sectors
    test eax, eax
    jz .fail

    mov eax, 1
    jmp .out
.append_done_no_data:
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbp
    pop rbx
    ret

; =========================================================================
; Data
; =========================================================================
exfat_base_lba:            dd 0
exfat_fat_offset:          dd 0
exfat_fat_length:          dd 0
exfat_cluster_heap_offset: dd 0
exfat_cluster_count:       dd 0
exfat_root_dir_cluster:    dd 0

; Current-directory state. exFAT has no on-disk "." / ".." entries, so
; OPEN/UP are backed entirely by this in-memory breadcrumb stack rather
; than anything read from the volume. exfat_cwd_cluster is what every
; directory-scanning primitive (exfat_dir_list_start, exfat_dir_find_slot,
; exfat_find_root_file) actually reads; it starts out equal to
; exfat_root_dir_cluster at mount and is the only thing OPEN/UP touch.
EXFAT_CWD_MAX_DEPTH equ 16
EXFAT_CWD_NAME_LEN  equ 64               ; matches EXFAT_MAX_NAME_LEN/shell_arg_buf
exfat_cwd_cluster:         dd 0
exfat_cwd_stack:           times EXFAT_CWD_MAX_DEPTH dd 0
exfat_cwd_depth:           dd 0
; exfat_cwd_name_stack[i] = the name OPEN was given to descend to depth
; i+1 (i.e. the name of the directory currently at depth i+1), used only
; to render the shell prompt's breadcrumb path -- not consulted by any
; exFAT traversal logic.
align 8
exfat_cwd_name_stack:      times (EXFAT_CWD_MAX_DEPTH * EXFAT_CWD_NAME_LEN) db 0

exfat_bytes_per_sector:    dd 0
exfat_sector_shift:        db 0
exfat_sector_mask:         dd 0
exfat_sectors_per_cluster: dd 0
exfat_gpt_entry_size:      dd 0

msg_exfat_sector_mismatch: db 'exFAT: volume sector size does not match device native sector size, refusing to mount', 13, 10, 0

exfat_list_cluster:           dd 0
exfat_list_offset:            dd 0

exfat_bitmap_cluster:         dd 0
exfat_bitmap_length:          dq 0
exfat_bitmap_cache_valid:     dd 0
exfat_bitmap_cache_chain_idx: dd 0
exfat_bitmap_cache_cluster:   dd 0

align 512
; Sized for the largest native device sector size these drivers report
; (4096, e.g. NVMe 4Kn or Advanced Format), not just the common 512 --
; each of these is filled by a single storage_read_sectors(count=1)
; call, which reads exactly one *device* sector, so a 512-byte buffer
; would overflow on hardware with a larger native sector size.
exfat_vbr_buf:        times 4096 db 0
exfat_fat_sector_buf: times 4096 db 0
exfat_gpt_buf:        times 4096 db 0

; NOTE: sized for our test volume's 4096-byte clusters. Real volumes can
; use larger clusters (up to 32MB per spec); once the heap allocator
; exists this should become a dynamically-sized, allocated buffer instead.
align 4096
exfat_dir_buf:    times 4096 db 0
exfat_bitmap_buf: times 4096 db 0
exfat_write_buf:  times 4096 db 0

align 8
exfat_wf_scratch: times 36 db 0

align 8
exfat_rmdir_name_buf: times 256 db 0
