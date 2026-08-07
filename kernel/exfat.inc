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

    xor edx, edx
    mov eax, 512
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

    movzx ecx, byte [rsi+108]           ; BytesPerSectorShift
    mov eax, 1
    shl eax, cl
    mov [rel exfat_bytes_per_sector], eax

    movzx ecx, byte [rsi+109]           ; SectorsPerClusterShift
    mov eax, 1
    shl eax, cl
    mov [rel exfat_sectors_per_cluster], eax

    mov eax, 1
    jmp .out
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
    ; byte offset within the FAT = cluster*4; sector = FatOffset + offset/512
    mov eax, ebx
    shl eax, 2
    mov edx, eax
    shr edx, 9                          ; edx = sector index within the FAT
    and eax, 0x1FF                      ; eax = byte offset within that sector

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
;         confirmed against a real Windows-written exFAT volume)
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

    mov ecx, [rel exfat_root_dir_cluster]
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
    lea rdx, [rsi+rdi+32]               ; rdx -> Stream Extension entry
    cmp edi, ebx                        ; guard: stream ext must still be in-buffer
    jae .skip_entry
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

; =========================================================================
; Data
; =========================================================================
exfat_base_lba:            dd 0
exfat_fat_offset:          dd 0
exfat_fat_length:          dd 0
exfat_cluster_heap_offset: dd 0
exfat_cluster_count:       dd 0
exfat_root_dir_cluster:    dd 0
exfat_bytes_per_sector:    dd 0
exfat_sectors_per_cluster: dd 0
exfat_gpt_entry_size:      dd 0

exfat_bitmap_cluster:         dd 0
exfat_bitmap_length:          dq 0
exfat_bitmap_cache_valid:     dd 0
exfat_bitmap_cache_chain_idx: dd 0
exfat_bitmap_cache_cluster:   dd 0

align 512
exfat_vbr_buf:        times 512 db 0
exfat_fat_sector_buf: times 512 db 0
exfat_gpt_buf:        times 512 db 0

; NOTE: sized for our test volume's 4096-byte clusters. Real volumes can
; use larger clusters (up to 32MB per spec); once the heap allocator
; exists this should become a dynamically-sized, allocated buffer instead.
align 4096
exfat_dir_buf:    times 4096 db 0
exfat_bitmap_buf: times 4096 db 0
