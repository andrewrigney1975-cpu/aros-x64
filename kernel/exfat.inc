; =============================================================================
; Minimal exFAT reader, built against Microsoft's public exFAT specification.
;
; Scope: read-only, single FAT. exfat_mount looks for the exFAT signature
; at LBA 0 first (whole-disk/superfloppy volume); if that's not it, it
; falls back to reading LBA 0 as an MBR and mounting the first partition
; with a nonzero type byte -- which is what Windows' own diskpart/format
; actually produces (confirmed against a real diskpart-created exFAT VHD:
; LBA 0 turned out to be MBR boot code, not the VBR). No GPT support yet.
; All LBAs this file computes are relative to exfat_base_lba, the start of
; whichever of those two the volume turned out to be at.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

; -------------------------------------------------------------------------
; exfat_mount: locates and validates the exFAT VBR, and stores the volume
; geometry. Returns EAX=1/0.
; -------------------------------------------------------------------------
exfat_mount:
    push rcx
    push rdx
    push r8
    push rsi

    xor ecx, ecx
    mov edx, 1
    lea r8, [rel exfat_vbr_buf]
    call storage_read_sectors
    test eax, eax
    jz .fail

    lea rsi, [rel exfat_vbr_buf]
    mov eax, [rsi+3]                    ; FileSystemName[0:4] = "EXFA"
    cmp eax, 0x41465845
    jne .try_mbr
    cmp byte [rsi+7], 'T'               ; FileSystemName[4] = "T"
    jne .try_mbr

    mov dword [rel exfat_base_lba], 0
    jmp .parse_vbr

.try_mbr:
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
    mov eax, [rsi+3]
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
    pop rsi
    pop r8
    pop rdx
    pop rcx
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
    push rcx
    mov eax, ecx
    call dbg_hex64
    pop rcx
    mov edx, 1
    lea r8, [rel exfat_fat_sector_buf]
    call storage_read_sectors
    push rax
    call dbg_hex64
    pop rax
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

align 512
exfat_vbr_buf:        times 512 db 0
exfat_fat_sector_buf: times 512 db 0

; NOTE: sized for our test volume's 4096-byte clusters. Real volumes can
; use larger clusters (up to 32MB per spec); once the heap allocator
; exists this should become a dynamically-sized, allocated buffer instead.
align 4096
exfat_dir_buf: times 4096 db 0
