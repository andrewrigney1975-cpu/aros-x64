; =============================================================================
; BASIX64 symbol tables: variables, labels, and the forward-reference
; fixup list used by GOTO/GOSUB targeting a label not yet seen.
;
; Variables are 8-byte slots in a flat static array (basix_var_slots),
; addressed directly since this kernel is a flat, non-relocatable binary
; -- there is no stack frame or register allocator for v1.
; =============================================================================

BASIX_MAX_VARS    equ 96
BASIX_MAX_LABELS  equ 64
BASIX_MAX_FIXUPS  equ 64
BASIX_MAX_FOR_DEPTH equ 8
BASIX_MAX_ARRAY_DIMS equ 4       ; DIM arr(d0,d1,...) -- up to 4 dimensions

; var_type encoding continued (0=INT,1=SFLOAT,2=DFLOAT,3=VECTOR2,
; 4=VECTOR3,5=VECTOR4,6=MATRIX4 -- see basix_parser.inc's DIM parsing):
; 1D arrays are 7/8. Both SFLOAT and DFLOAT array elements use
; BASIX_VARTYPE_ARRAY_FLOAT (8-byte double storage per element either
; way) -- unlike scalar SFLOAT, arrays don't get the 4-byte-per-element
; packing, a deliberate simplification since nothing here needs the
; memory savings and it avoids array code needing to track a second,
; smaller element size.
BASIX_VARTYPE_ARRAY_INT   equ 7
BASIX_VARTYPE_ARRAY_FLOAT equ 8

BASIX_ARRAY_ARENA_SIZE equ 16777216      ; 16MB, bump-allocated by DIM's
                                          ; array path. Grown from the
                                          ; original 256KB so a text editor
                                          ; program can DIM buf(1048576) AS
                                          ; INTEGER (~8MB, since arrays here
                                          ; are uniformly 8 bytes/element)
                                          ; to edit files up to 1MB, with
                                          ; headroom left for other arrays
                                          ; (e.g. a filename buffer).

; -------------------------------------------------------------------------
; basix_name_eq: RCX = candidate ptr, EDX = candidate len, R8 = stored
; name field (32 bytes, NUL-padded, already upper-case). Returns EAX=1/0,
; folding the candidate to upper-case for the comparison.
; -------------------------------------------------------------------------
basix_name_eq:
    push rcx
    push rdx
    push rsi
    push rdi
    push r9

    mov rsi, rcx
    mov rdi, r8
    mov r9d, edx
    cmp r9d, 31
    jbe .len_ok
    mov r9d, 31
.len_ok:
    xor ecx, ecx
.loop:
    cmp ecx, r9d
    jge .check_end
    mov al, [rsi+rcx]
    cmp al, 'a'
    jb .nofold
    cmp al, 'z'
    ja .nofold
    sub al, 0x20
.nofold:
    mov dl, [rdi+rcx]
    cmp al, dl
    jne .nomatch
    inc ecx
    jmp .loop
.check_end:
    cmp byte [rdi+rcx], 0
    jne .nomatch
    mov eax, 1
    jmp .out
.nomatch:
    xor eax, eax
.out:
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; basix_name_copy_upper: RCX = src ptr, EDX = src len (caller caps to 31),
; R8 = dest ptr (32-byte field, caller pre-zeroed). Copies up to EDX
; chars, upper-casing ASCII letters.
; -------------------------------------------------------------------------
basix_name_copy_upper:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r9

    mov rsi, rcx
    mov rdi, r8
    mov r9d, edx
    xor ecx, ecx
.loop:
    cmp ecx, r9d
    jge .done
    mov al, [rsi+rcx]
    cmp al, 'a'
    jb .nofold
    cmp al, 'z'
    ja .nofold
    sub al, 0x20
.nofold:
    mov [rdi+rcx], al
    inc ecx
    jmp .loop
.done:
    pop r9
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_save_ident: RCX = name ptr, EDX = len. Copies (clamped to 31
; bytes) into basix_saved_ident_buf/basix_saved_ident_len -- see the
; comment there for why this exists.
; -------------------------------------------------------------------------
basix_save_ident:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    mov eax, edx
    cmp eax, 31
    jbe .len_ok
    mov eax, 31
.len_ok:
    mov [rel basix_saved_ident_len], eax

    mov rsi, rcx                        ; source pointer
    lea rdi, [rel basix_saved_ident_buf]
    mov ecx, eax                        ; copy count
    cld
    rep movsb

    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_var_find_or_create: RCX = name ptr, EDX = len. Returns EAX =
; variable index, creating a new INTEGER-typed variable if not found.
; -------------------------------------------------------------------------
basix_var_find_or_create:
    push rbx
    push rcx
    push rdx
    push rdi
    push r8
    push r9

    mov r8, rcx
    mov r9d, edx

    xor ebx, ebx
.search:
    cmp ebx, [rel basix_var_count]
    jge .create
    mov eax, ebx
    imul eax, 32
    lea rdi, [rel basix_var_name]
    add rdi, rax

    mov rcx, r8
    mov edx, r9d
    push r8
    mov r8, rdi
    call basix_name_eq
    pop r8
    test eax, eax
    jnz .found
    inc ebx
    jmp .search

.found:
    mov eax, ebx
    jmp .out

.create:
    cmp dword [rel basix_var_count], BASIX_MAX_VARS
    jge .fail

    mov eax, ebx
    imul eax, 32
    lea rdi, [rel basix_var_name]
    add rdi, rax
    push rdi
    mov ecx, 32
    xor eax, eax
    cld
    rep stosb
    pop rdi

    mov ecx, r9d
    cmp ecx, 31
    jbe .clen_ok
    mov ecx, 31
.clen_ok:
    mov edx, ecx
    mov rcx, r8
    mov r8, rdi
    call basix_name_copy_upper

    mov dword [rel basix_var_type + ebx*4], 0
    mov eax, [rel basix_var_count]
    inc eax
    mov [rel basix_var_count], eax
    mov eax, ebx
    jmp .out

.fail:
    mov dword [rel basix_compile_ok], 0
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_var_slot_addr: EAX = variable index. Returns RAX = absolute
; address of its 8-byte slot.
; -------------------------------------------------------------------------
basix_var_slot_addr:
    imul eax, eax, 8
    lea rax, [rel basix_var_slots + rax]
    ret

; -------------------------------------------------------------------------
; basix_var_wide_slot_addr: EAX = variable index. Returns RAX = absolute
; address of its 128-byte wide slot (VECTOR2/3/4 and MATRIX4 storage --
; up to 16 doubles, row-major for MATRIX4). Every variable gets one of
; these regardless of declared type; scalars just never use theirs.
; -------------------------------------------------------------------------
basix_var_wide_slot_addr:
    imul eax, eax, 128
    lea rax, [rel basix_var_wide_slots + rax]
    ret

; -------------------------------------------------------------------------
; basix_label_find_or_create: RCX = name ptr, EDX = len. Returns EAX =
; label index, creating an as-yet-undefined entry if not found.
; -------------------------------------------------------------------------
basix_label_find_or_create:
    push rbx
    push rcx
    push rdx
    push rdi
    push r8
    push r9

    mov r8, rcx
    mov r9d, edx

    xor ebx, ebx
.search:
    cmp ebx, [rel basix_label_count]
    jge .create
    mov eax, ebx
    imul eax, 32
    lea rdi, [rel basix_label_name]
    add rdi, rax

    mov rcx, r8
    mov edx, r9d
    push r8
    mov r8, rdi
    call basix_name_eq
    pop r8
    test eax, eax
    jnz .found
    inc ebx
    jmp .search

.found:
    mov eax, ebx
    jmp .out

.create:
    cmp dword [rel basix_label_count], BASIX_MAX_LABELS
    jge .fail

    mov eax, ebx
    imul eax, 32
    lea rdi, [rel basix_label_name]
    add rdi, rax
    push rdi
    mov ecx, 32
    xor eax, eax
    cld
    rep stosb
    pop rdi

    mov ecx, r9d
    cmp ecx, 31
    jbe .clen_ok
    mov ecx, 31
.clen_ok:
    mov edx, ecx
    mov rcx, r8
    mov r8, rdi
    call basix_name_copy_upper

    mov dword [rel basix_label_defined + ebx*4], 0
    mov eax, [rel basix_label_count]
    inc eax
    mov [rel basix_label_count], eax
    mov eax, ebx
    jmp .out

.fail:
    mov dword [rel basix_compile_ok], 0
    xor eax, eax
.out:
    pop r9
    pop r8
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_fixup_add: RCX = absolute patch address, EDX = label index.
; -------------------------------------------------------------------------
basix_fixup_add:
    push rax
    push rcx
    push rdx

    mov eax, [rel basix_fixup_count]
    cmp eax, BASIX_MAX_FIXUPS
    jge .full
    mov [rel basix_fixup_patchaddr + rax*8], rcx
    mov [rel basix_fixup_labelidx + rax*4], edx
    inc eax
    mov [rel basix_fixup_count], eax
    jmp .out
.full:
    mov dword [rel basix_compile_ok], 0
.out:
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_label_define: EAX = label index. Marks it defined at the current
; emit position and patches any pending fixups referencing it.
; -------------------------------------------------------------------------
basix_label_define:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi

    mov ebx, eax
    call basix_emit_here
    mov [rel basix_label_addr + rbx*8], rax
    mov dword [rel basix_label_defined + rbx*4], 1

    xor ecx, ecx
.scan:
    cmp ecx, [rel basix_fixup_count]
    jge .out
    cmp [rel basix_fixup_labelidx + rcx*4], ebx
    jne .next
    mov rdx, rax
    mov rsi, [rel basix_fixup_patchaddr + rcx*8]
    push rax
    push rcx
    mov rcx, rsi
    call basix_patch_rel32
    pop rcx
    pop rax
    mov dword [rel basix_fixup_labelidx + rcx*4], 0xFFFFFFFF
.next:
    inc ecx
    jmp .scan
.out:
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_goto_or_gosub_emit: RCX = label name ptr, EDX = len, R8D =
; is_call (0 = GOTO/JMP, 1 = GOSUB/CALL).
; -------------------------------------------------------------------------
basix_goto_or_gosub_emit:
    push rbx
    push rcx
    push rdx
    push r9

    mov ebx, r8d
    call basix_label_find_or_create
    mov r9d, eax

    cmp dword [rel basix_label_defined + r9*4], 0
    jne .defined

    test ebx, ebx
    jnz .call_placeholder
    call basix_emit_jmp_placeholder
    jmp .add_fixup
.call_placeholder:
    mov al, 0xE8
    call basix_emit_u8
    call basix_emit_here
    push rax
    xor eax, eax
    call basix_emit_u32
    pop rax
.add_fixup:
    mov rcx, rax
    mov edx, r9d
    call basix_fixup_add
    jmp .out

.defined:
    mov rdx, [rel basix_label_addr + r9*8]
    mov rcx, rdx
    test ebx, ebx
    jnz .call_defined
    call basix_emit_jmp_to
    jmp .out
.call_defined:
    call basix_emit_call_to

.out:
    pop r9
    pop rdx
    pop rcx
    pop rbx
    ret

; =========================================================================
; Data
; =========================================================================
basix_compile_ok: dd 1

basix_var_count: dd 0
basix_var_type:  times BASIX_MAX_VARS dd 0
align 8
basix_var_name:  times (BASIX_MAX_VARS*32) db 0
basix_var_slots: times BASIX_MAX_VARS dq 0

align 8
basix_var_wide_slots: times (BASIX_MAX_VARS * 128) db 0

basix_var_arr_len: times BASIX_MAX_VARS dd 0   ; TOTAL element count
                                                ; (product of all dims),
                                                ; arrays only

; Per-dimension shape, arrays only. ndims=1 for an ordinary 1D array
; (DIM arr(N) ...) -- the read/write index paths in basix_parser.inc
; treat that as the trivial 1-dimension case of the same general
; multi-index code, not a separate path. dims[var*BASIX_MAX_ARRAY_DIMS
; + k] is dimension k's size (row-major: the LAST dimension is the one
; whose index moves fastest through memory, same convention as C).
basix_var_arr_ndims: times BASIX_MAX_VARS dd 0
basix_var_arr_dims:  times (BASIX_MAX_VARS * BASIX_MAX_ARRAY_DIMS) dd 0

; DIM's own scratch while parsing "(d0, d1, ...)" -- the sizes are
; known before the variable they belong to is (var creation happens
; after the trailing "AS type" clause), so they're staged here and
; copied into basix_var_arr_ndims/dims once the var index is known.
basix_dim_scratch_ndims: dd 0
basix_dim_scratch_dims:  times BASIX_MAX_ARRAY_DIMS dd 0

; basix_array_arena is heap-allocated at kernel init (basix_heap_init,
; kernel.asm) via kmalloc_large rather than baked into the kernel image
; as literal zero bytes -- at 16MB, embedding it directly would have
; bloated KERNEL.BIN from ~2MB to ~18MB, big enough to make the
; bootloader's fixed-address AllocatePages(KERNEL_LOAD_ADDR) call fail
; (not enough contiguous free physical memory at that fixed range).
basix_array_arena_pos: dd 0
align 8
basix_array_arena_ptr: dq 0

; Permanent home for PRINT's string-literal arguments (see
; basix_strlit_intern, basix_parser.inc) -- unlike basix_tok_text_buf,
; never reused within one compile, so a compiled program can safely
; read from it at runtime long after compilation has moved on to later
; tokens.
BASIX_STRLIT_POOL_SIZE equ 16384
basix_strlit_pool_pos: dd 0
align 8
basix_strlit_pool: times BASIX_STRLIT_POOL_SIZE db 0

; FSAVE/FLOAD (basix_rt_fsave/basix_rt_fload, basix_runtime.inc) scratch:
; BASIX64 has no string type, so a filename/file-content array arrives as
; one int (8 bytes) per character/byte -- these buffers hold the packed,
; byte-per-char/byte form that exfat_write_file/exfat_read_file actually
; want. Not reentrant (fine: FSAVE/FLOAD are never called concurrently
; with themselves, same convention as exfat_dir_buf/exfat_wf_scratch).
BASIX_FSAVE_NAME_BUF_SIZE equ 65         ; EXFAT_MAX_NAME_LEN + NUL
BASIX_FILE_SCRATCH_SIZE   equ 1048576    ; matches editor.bas's 1MB cap
basix_fsave_name_buf: times BASIX_FSAVE_NAME_BUF_SIZE db 0
; Also heap-allocated at kernel init (see basix_array_arena_ptr above) --
; 1MB of literal zero bytes is a smaller offender but the same fix.
align 8
basix_file_scratch_ptr: dq 0

; basix_resolve_dir_path (basix_runtime.inc) walks basix_fsave_name_buf's
; '/'-separated directory components by temporarily repointing
; exfat_cwd_cluster, exactly like the shell's OPEN command does one
; level at a time -- this holds the original cwd so basix_rt_fsave/
; basix_rt_fload can put it back once the file op is done, leaving the
; shell's own OPEN/UP navigation state untouched by a FSAVE/FLOAD call.
basix_path_saved_cwd: dd 0

align 8
basix_vec_scalar_temp: dq 0              ; accumulator for dot/length/matmul
basix_mat_scratch:     times 16 dq 0     ; MMUL result staging (alias safety)
basix_vec4_scratch:    times 4 dq 0      ; MVMUL result staging (alias safety)

basix_label_count:   dd 0
basix_label_defined: times BASIX_MAX_LABELS dd 0
align 8
basix_label_name: times (BASIX_MAX_LABELS*32) db 0
basix_label_addr: times BASIX_MAX_LABELS dq 0

basix_fixup_count: dd 0
align 8
basix_fixup_patchaddr: times BASIX_MAX_FIXUPS dq 0
basix_fixup_labelidx:  times BASIX_MAX_FIXUPS dd 0

basix_for_depth: dd 0
align 8
basix_for_end_temps:  times BASIX_MAX_FOR_DEPTH dq 0
basix_for_step_temps: times BASIX_MAX_FOR_DEPTH dq 0

; Shared scratch for any parser call site that needs an identifier's
; text to survive a later basix_lex_next call (basix_parse_dim's own
; name across the "AS"/type-keyword tokens; GOTO/GOSUB's target name
; across the lookahead lex_next that follows it) -- copied immediately
; via basix_save_ident rather than trusting the lexer's shared
; tok_text_buf to survive whatever gets lexed next, which silently
; overwrites it if that next token is itself an identifier/keyword.
; Never used by two call sites at once (DIM/GOTO/GOSUB parsing never
; nests), so one shared buffer is enough.
basix_saved_ident_buf: times 32 db 0
basix_saved_ident_len: dd 0
