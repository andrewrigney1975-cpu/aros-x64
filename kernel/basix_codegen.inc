; =============================================================================
; BASIX64 native x64 code emitter.
;
; Emits raw machine code bytes into basix_code_buf as the parser walks the
; source. Because this kernel is a flat, non-relocatable binary loaded at
; a fixed address, variable/label addresses are known absolute 64-bit
; constants at "compile" time, so all variable access uses the special
; no-ModRM "MOV RAX, moffs64" / "MOV moffs64, RAX" forms (opcodes A1/A3)
; -- there's no register allocator or stack frame to design for v1.
; =============================================================================

; Per-slot code buffer size (see basix_symbols.inc's R0 slot block --
; BASIX_SLOT_COUNT independent buffers this size, heap-allocated by
; basix_slot_init_all). 512KB, up from the original 64KB single
; buffer -- plenty of headroom for richer GUI-era programs, and
; trivial against available RAM even times 8 slots.
BASIX_CODE_BUF_SIZE equ 524288

; -------------------------------------------------------------------------
; basix_emit_u8: AL = byte to append.
; -------------------------------------------------------------------------
basix_emit_u8:
    push rbx
    push rcx
    mov ecx, [rel basix_code_pos]
    cmp ecx, BASIX_CODE_BUF_SIZE
    jae .overflow
    mov rbx, [rel basix_active_code_buf_ptr]
    mov [rbx+rcx], al
    inc dword [rel basix_code_pos]
    jmp .out
.overflow:
    mov dword [rel basix_compile_ok], 0
.out:
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_emit_u32: EAX = dword to append.
; -------------------------------------------------------------------------
basix_emit_u32:
    push rbx
    push rcx
    push rdx
    mov edx, eax
    mov ecx, [rel basix_code_pos]
    cmp ecx, BASIX_CODE_BUF_SIZE - 4
    ja .overflow
    mov rbx, [rel basix_active_code_buf_ptr]
    mov [rbx+rcx], edx
    add dword [rel basix_code_pos], 4
    jmp .out
.overflow:
    mov dword [rel basix_compile_ok], 0
.out:
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_emit_u64: RAX = qword to append.
; -------------------------------------------------------------------------
basix_emit_u64:
    push rbx
    push rcx
    push rdx
    mov rdx, rax
    mov ecx, [rel basix_code_pos]
    cmp ecx, BASIX_CODE_BUF_SIZE - 8
    ja .overflow
    mov rbx, [rel basix_active_code_buf_ptr]
    mov [rbx+rcx], rdx
    add dword [rel basix_code_pos], 8
    jmp .out
.overflow:
    mov dword [rel basix_compile_ok], 0
.out:
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_emit_bytes: RCX = ptr to bytes, EDX = count.
; -------------------------------------------------------------------------
basix_emit_bytes:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    mov rsi, rcx
    xor edi, edi
.loop:
    cmp edi, edx
    jge .done
    movzx eax, byte [rsi+rdi]
    call basix_emit_u8
    inc edi
    jmp .loop
.done:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_emit_here: returns RAX = absolute address of the current write
; position (basix_code_buf + basix_code_pos).
; -------------------------------------------------------------------------
basix_emit_here:
    push rbx
    push rcx
    mov ecx, [rel basix_code_pos]
    mov rbx, [rel basix_active_code_buf_ptr]
    lea rax, [rbx+rcx]
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_patch_rel32: RCX = absolute address of a rel32 field, RDX =
; absolute target address. Writes target-(patch_addr+4) at [RCX].
; -------------------------------------------------------------------------
basix_patch_rel32:
    push rax
    mov rax, rdx
    sub rax, rcx
    sub rax, 4
    mov [rcx], eax
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_emit_mov_rax_imm64: RAX = the immediate value. Emits 48 B8 <imm64>.
; -------------------------------------------------------------------------
basix_emit_mov_rax_imm64:
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xB8
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    ret

; -------------------------------------------------------------------------
; basix_emit_mov_rcx_imm64: RAX = the immediate value. Emits 48 B9 <imm64>.
; -------------------------------------------------------------------------
basix_emit_mov_rcx_imm64:
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xB9
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    ret

; -------------------------------------------------------------------------
; basix_emit_mov_rdx_imm64: RAX = the immediate value. Emits 48 BA <imm64>.
; -------------------------------------------------------------------------
basix_emit_mov_rdx_imm64:
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xBA
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    ret

; -------------------------------------------------------------------------
; basix_emit_mov_rax_moffs: RAX = absolute address. Emits 48 A1 <addr64>
; (MOV RAX, [addr]).
; -------------------------------------------------------------------------
basix_emit_mov_rax_moffs:
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xA1
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    ret

; -------------------------------------------------------------------------
; basix_emit_mov_moffs_rax: RAX = absolute address. Emits 48 A3 <addr64>
; (MOV [addr], RAX).
; -------------------------------------------------------------------------
basix_emit_mov_moffs_rax:
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xA3
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    ret

; -------------------------------------------------------------------------
; basix_emit_jmp_to: RCX = absolute target address. Emits an unconditional
; near JMP to it.
; -------------------------------------------------------------------------
basix_emit_jmp_to:
    push rcx
    mov al, 0xE9
    call basix_emit_u8
    call basix_emit_here
    mov r8, rax
    xor eax, eax
    call basix_emit_u32
    pop rdx
    mov rcx, r8
    call basix_patch_rel32
    ret

; -------------------------------------------------------------------------
; basix_emit_call_to: RCX = absolute target address. Emits MOV RAX,imm64
; + CALL RAX (48 B8 <imm64> FF D0) -- an absolute indirect call, not a
; rel32 near CALL. This matters now that a program's code buffer is a
; per-slot heap allocation (see basix_symbols.inc's R0 slot block)
; potentially tens of terabytes of virtual address space away from the
; kernel image basix_rt_* runtime helpers live in: a rel32 displacement
; only reaches +/-2GB, which silently truncates/wraps for a target that
; far away and jumps into garbage. Used for BOTH calls out to basix_rt_*
; helpers and GOSUBs to an already-defined BASIX label (see
; basix_goto_or_gosub_emit's .call_defined -- a same-buffer label call
; would fit in rel32 today, but there's no reason to keep two encodings
; when the target is always known immediately here, unlike the
; forward-reference GOSUB path below, which still uses a rel32
; placeholder + basix_patch_rel32 since that one genuinely only ever
; targets a same-buffer label).
; -------------------------------------------------------------------------
basix_emit_call_to:
    push rax
    mov rax, rcx
    push rax
    mov al, 0x48
    call basix_emit_u8
    mov al, 0xB8
    call basix_emit_u8
    pop rax
    call basix_emit_u64
    mov al, 0xFF
    call basix_emit_u8
    mov al, 0xD0
    call basix_emit_u8
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_emit_jmp_placeholder: emits E9 <rel32=0>. Returns RAX = absolute
; address of the rel32 field, for a later basix_patch_rel32 call.
; -------------------------------------------------------------------------
basix_emit_jmp_placeholder:
    mov al, 0xE9
    call basix_emit_u8
    call basix_emit_here
    push rax
    xor eax, eax
    call basix_emit_u32
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_emit_jcc_placeholder: AL = second opcode byte (e.g. 0x84 for JE).
; Emits 0F <cc> <rel32=0>. Returns RAX = absolute address of the rel32
; field.
; -------------------------------------------------------------------------
basix_emit_jcc_placeholder:
    push rbx
    mov bl, al
    mov al, 0x0F
    call basix_emit_u8
    mov al, bl
    call basix_emit_u8
    call basix_emit_here
    push rax
    xor eax, eax
    call basix_emit_u32
    pop rax
    pop rbx
    ret

; =========================================================================
; Fixed instruction byte patterns (no operand encoding needed -- always
; operate on RAX/RCX/RDX).
; =========================================================================
basix_i_push_rax:    db 0x50
basix_i_pop_rax:     db 0x58
basix_i_pop_rcx:     db 0x59
basix_i_pop_rdx:     db 0x5A
basix_i_pop_r8:      db 0x41,0x58
basix_i_pop_r9:      db 0x41,0x59
basix_i_pop_r10:     db 0x41,0x5A
basix_i_mov_rcx_rax: db 0x48,0x89,0xC1
basix_i_mov_rax_rdx: db 0x48,0x89,0xD0
basix_i_add_rax_rcx: db 0x48,0x01,0xC8
basix_i_sub_rax_rcx: db 0x48,0x29,0xC8
basix_i_imul_rax_rcx: db 0x48,0x0F,0xAF,0xC1
basix_i_cqo:         db 0x48,0x99
basix_i_idiv_rcx:    db 0x48,0xF7,0xF9
basix_i_cmp_rax_rcx: db 0x48,0x39,0xC8
basix_i_neg_rax:     db 0x48,0xF7,0xD8
basix_i_ret:         db 0xC3

; Array addressing -- RDX-based variants of imul/add/cmp exist purely so
; an indexed INT array store can keep its value in RCX (untouched by
; any of these) while the index-in-RAX becomes an address using RDX as
; the scratch operand instead. Float array stores don't need these
; (the value lives in XMM0, never in a GP register the address math
; would otherwise fight over), but the array-read paths use them too
; for consistency -- one address-computation shape for every array
; access rather than two.
basix_i_imul_rax_rdx: db 0x48,0x0F,0xAF,0xC2
basix_i_add_rax_rdx:  db 0x48,0x01,0xD0
basix_i_cmp_rax_rdx:  db 0x48,0x39,0xD0
basix_i_mov_rax_memrax: db 0x48,0x8B,0x00       ; mov rax,[rax]
basix_i_mov_memrax_rcx: db 0x48,0x89,0x08       ; mov [rax],rcx

; =========================================================================
; Thin wrappers around basix_emit_bytes for each fixed pattern above --
; tail-called (jmp, not call) since they have nothing left to do after.
; =========================================================================
basix_emit_push_rax:
    lea rcx, [rel basix_i_push_rax]
    mov edx, 1
    jmp basix_emit_bytes
basix_emit_pop_rax:
    lea rcx, [rel basix_i_pop_rax]
    mov edx, 1
    jmp basix_emit_bytes
basix_emit_pop_rcx:
    lea rcx, [rel basix_i_pop_rcx]
    mov edx, 1
    jmp basix_emit_bytes
basix_emit_pop_rdx:
    lea rcx, [rel basix_i_pop_rdx]
    mov edx, 1
    jmp basix_emit_bytes
basix_emit_pop_r8:
    lea rcx, [rel basix_i_pop_r8]
    mov edx, 2
    jmp basix_emit_bytes
basix_emit_pop_r9:
    lea rcx, [rel basix_i_pop_r9]
    mov edx, 2
    jmp basix_emit_bytes
basix_emit_pop_r10:
    lea rcx, [rel basix_i_pop_r10]
    mov edx, 2
    jmp basix_emit_bytes
basix_emit_mov_rcx_rax:
    lea rcx, [rel basix_i_mov_rcx_rax]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_mov_rax_rdx:
    lea rcx, [rel basix_i_mov_rax_rdx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_add_rax_rcx:
    lea rcx, [rel basix_i_add_rax_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_sub_rax_rcx:
    lea rcx, [rel basix_i_sub_rax_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_imul_rax_rcx:
    lea rcx, [rel basix_i_imul_rax_rcx]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_cqo:
    lea rcx, [rel basix_i_cqo]
    mov edx, 2
    jmp basix_emit_bytes
basix_emit_idiv_rcx:
    lea rcx, [rel basix_i_idiv_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_cmp_rax_rcx:
    lea rcx, [rel basix_i_cmp_rax_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_neg_rax:
    lea rcx, [rel basix_i_neg_rax]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_ret_instr:
    lea rcx, [rel basix_i_ret]
    mov edx, 1
    jmp basix_emit_bytes
basix_emit_imul_rax_rdx:
    lea rcx, [rel basix_i_imul_rax_rdx]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_add_rax_rdx:
    lea rcx, [rel basix_i_add_rax_rdx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_cmp_rax_rdx:
    lea rcx, [rel basix_i_cmp_rax_rdx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_mov_rax_memrax:
    lea rcx, [rel basix_i_mov_rax_memrax]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_mov_memrax_rcx:
    lea rcx, [rel basix_i_mov_memrax_rcx]
    mov edx, 3
    jmp basix_emit_bytes

; =========================================================================
; Float (SSE2) instruction patterns. Internally, every float value is
; carried as a double in XMM0 (sfloat values get widened via CVTSS2SD on
; load and narrowed via CVTSD2SS on store) -- one code path instead of
; two. XMM registers have no "moffs" load/store form like RAX does, so
; float variable access is always two steps: load the address into RAX
; (MOV RAX, imm64), then MOVSD/MOVSS through [RAX].
; =========================================================================
basix_i_push_xmm0:    db 0x48,0x83,0xEC,0x08, 0xF2,0x0F,0x11,0x04,0x24   ; sub rsp,8; movsd [rsp],xmm0
basix_i_pop_xmm0:     db 0xF2,0x0F,0x10,0x04,0x24, 0x48,0x83,0xC4,0x08   ; movsd xmm0,[rsp]; add rsp,8
basix_i_mov_xmm1_xmm0: db 0xF2,0x0F,0x10,0xC8                            ; movsd xmm1,xmm0
basix_i_addsd:        db 0xF2,0x0F,0x58,0xC1                             ; addsd xmm0,xmm1
basix_i_subsd:        db 0xF2,0x0F,0x5C,0xC1                             ; subsd xmm0,xmm1
basix_i_mulsd:        db 0xF2,0x0F,0x59,0xC1                             ; mulsd xmm0,xmm1
basix_i_divsd:        db 0xF2,0x0F,0x5E,0xC1                             ; divsd xmm0,xmm1
basix_i_comisd:       db 0x66,0x0F,0x2F,0xC1                             ; comisd xmm0,xmm1
basix_i_negsd:        db 0x66,0x0F,0xEF,0xC9, 0xF2,0x0F,0x5C,0xC8, 0xF2,0x0F,0x10,0xC1 ; pxor xmm1,xmm1; subsd xmm1,xmm0; movsd xmm0,xmm1
basix_i_cvtsi2sd_x0_rax: db 0xF2,0x48,0x0F,0x2A,0xC0                     ; cvtsi2sd xmm0,rax
basix_i_cvtsi2sd_x1_rax: db 0xF2,0x48,0x0F,0x2A,0xC8                     ; cvtsi2sd xmm1,rax
basix_i_cvttsd2si_rax_x0: db 0xF2,0x48,0x0F,0x2C,0xC0                    ; cvttsd2si rax,xmm0
basix_i_cvtsd2ss_x0_x0:  db 0xF2,0x0F,0x5A,0xC0                          ; cvtsd2ss xmm0,xmm0
basix_i_cvtss2sd_x0_x0:  db 0xF3,0x0F,0x5A,0xC0                          ; cvtss2sd xmm0,xmm0
basix_i_movsd_load_rax:  db 0xF2,0x0F,0x10,0x00                          ; movsd xmm0,[rax]
basix_i_movsd_store_rax: db 0xF2,0x0F,0x11,0x00                          ; movsd [rax],xmm0
basix_i_movss_load_rax:  db 0xF3,0x0F,0x10,0x00                          ; movss xmm0,[rax]
basix_i_movss_store_rax: db 0xF3,0x0F,0x11,0x00                          ; movss [rax],xmm0
basix_i_movq_xmm0_rax:   db 0x66,0x48,0x0F,0x6E,0xC0                     ; movq xmm0,rax

basix_emit_push_xmm0:
    lea rcx, [rel basix_i_push_xmm0]
    mov edx, 9
    jmp basix_emit_bytes
basix_emit_pop_xmm0:
    lea rcx, [rel basix_i_pop_xmm0]
    mov edx, 9
    jmp basix_emit_bytes
basix_emit_mov_xmm1_xmm0:
    lea rcx, [rel basix_i_mov_xmm1_xmm0]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_addsd:
    lea rcx, [rel basix_i_addsd]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_subsd:
    lea rcx, [rel basix_i_subsd]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_mulsd:
    lea rcx, [rel basix_i_mulsd]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_divsd:
    lea rcx, [rel basix_i_divsd]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_comisd:
    lea rcx, [rel basix_i_comisd]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_negsd:
    lea rcx, [rel basix_i_negsd]
    mov edx, 12
    jmp basix_emit_bytes
basix_emit_cvtsi2sd_x0_rax:
    lea rcx, [rel basix_i_cvtsi2sd_x0_rax]
    mov edx, 5
    jmp basix_emit_bytes
basix_emit_cvtsi2sd_x1_rax:
    lea rcx, [rel basix_i_cvtsi2sd_x1_rax]
    mov edx, 5
    jmp basix_emit_bytes
basix_emit_cvttsd2si_rax_x0:
    lea rcx, [rel basix_i_cvttsd2si_rax_x0]
    mov edx, 5
    jmp basix_emit_bytes
basix_emit_cvtsd2ss_x0_x0:
    lea rcx, [rel basix_i_cvtsd2ss_x0_x0]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_cvtss2sd_x0_x0:
    lea rcx, [rel basix_i_cvtss2sd_x0_x0]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movsd_load_rax:
    lea rcx, [rel basix_i_movsd_load_rax]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movsd_store_rax:
    lea rcx, [rel basix_i_movsd_store_rax]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movss_load_rax:
    lea rcx, [rel basix_i_movss_load_rax]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movss_store_rax:
    lea rcx, [rel basix_i_movss_store_rax]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movq_xmm0_rax:
    lea rcx, [rel basix_i_movq_xmm0_rax]
    mov edx, 5
    jmp basix_emit_bytes

basix_i_movsd_xmm2_xmm0: db 0xF2,0x0F,0x10,0xD0                          ; movsd xmm2,xmm0
basix_i_movsd_xmm0_xmm2: db 0xF2,0x0F,0x10,0xC2                          ; movsd xmm0,xmm2

basix_emit_movsd_xmm2_xmm0:
    lea rcx, [rel basix_i_movsd_xmm2_xmm0]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movsd_xmm0_xmm2:
    lea rcx, [rel basix_i_movsd_xmm0_xmm2]
    mov edx, 4
    jmp basix_emit_bytes

; -------------------------------------------------------------------------
; basix_emit_fmod: emits code computing XMM0 = fmod(XMM0, XMM1) --
; XMM0 - XMM1*trunc(XMM0/XMM1) -- since SSE2 has no direct remainder
; instruction. Clobbers RAX and XMM2 at runtime.
; -------------------------------------------------------------------------
basix_emit_fmod:
    call basix_emit_movsd_xmm2_xmm0     ; xmm2 = a (dividend, saved)
    call basix_emit_divsd               ; xmm0 = a/b
    call basix_emit_cvttsd2si_rax_x0    ; rax = trunc(a/b)
    call basix_emit_cvtsi2sd_x0_rax     ; xmm0 = trunc(a/b) as double
    call basix_emit_mulsd               ; xmm0 = b*trunc(a/b)
    call basix_emit_mov_xmm1_xmm0       ; xmm1 = b*trunc(a/b)
    call basix_emit_movsd_xmm0_xmm2     ; xmm0 = a
    jmp basix_emit_subsd                ; xmm0 = a - b*trunc(a/b)

; =========================================================================
; Boolean materialization patterns (AND/OR/NOT and bare-value-as-
; condition support): every comparison/logical-combination now leaves a
; plain 0/1 in RAX rather than jumping directly, so AND/OR/NOT compose
; via ordinary integer AND/OR/XOR -- no short-circuit control flow, no
; duplicated jump-patch plumbing. IF/WHILE still get a single false-jump
; patch address, via a final TEST RAX,RAX + JE tacked on top.
; =========================================================================
basix_i_test_rax_rax: db 0x48,0x85,0xC0             ; test rax,rax
basix_i_xor_eax_1:    db 0x83,0xF0,0x01             ; xor eax,1
basix_i_and_rax_rcx:  db 0x48,0x21,0xC8             ; and rax,rcx
basix_i_or_rax_rcx:   db 0x48,0x09,0xC8             ; or rax,rcx
basix_i_movzx_eax_al: db 0x0F,0xB6,0xC0              ; movzx eax,al
basix_i_pxor_xmm1_xmm1: db 0x66,0x0F,0xEF,0xC9       ; pxor xmm1,xmm1
basix_i_pxor_xmm0_xmm0: db 0x66,0x0F,0xEF,0xC0       ; pxor xmm0,xmm0
basix_i_movsd_load_rax_x1: db 0xF2,0x0F,0x10,0x08    ; movsd xmm1,[rax]
basix_i_sqrtsd: db 0xF2,0x0F,0x51,0xC0               ; sqrtsd xmm0,xmm0

basix_emit_pxor_xmm0_xmm0:
    lea rcx, [rel basix_i_pxor_xmm0_xmm0]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_movsd_load_rax_x1:
    lea rcx, [rel basix_i_movsd_load_rax_x1]
    mov edx, 4
    jmp basix_emit_bytes
basix_emit_sqrtsd:
    lea rcx, [rel basix_i_sqrtsd]
    mov edx, 4
    jmp basix_emit_bytes

basix_i_sete:  db 0x0F,0x94,0xC0                     ; sete al
basix_i_setne: db 0x0F,0x95,0xC0                     ; setne al
basix_i_setl:  db 0x0F,0x9C,0xC0                     ; setl al
basix_i_setge: db 0x0F,0x9D,0xC0                     ; setge al
basix_i_setle: db 0x0F,0x9E,0xC0                     ; setle al
basix_i_setg:  db 0x0F,0x9F,0xC0                     ; setg al
basix_i_setb:  db 0x0F,0x92,0xC0                     ; setb al
basix_i_setae: db 0x0F,0x93,0xC0                     ; setae al
basix_i_setbe: db 0x0F,0x96,0xC0                     ; setbe al
basix_i_seta:  db 0x0F,0x97,0xC0                     ; seta al

basix_emit_test_rax_rax:
    lea rcx, [rel basix_i_test_rax_rax]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_xor_eax_1:
    lea rcx, [rel basix_i_xor_eax_1]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_and_rax_rcx:
    lea rcx, [rel basix_i_and_rax_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_or_rax_rcx:
    lea rcx, [rel basix_i_or_rax_rcx]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_movzx_eax_al:
    lea rcx, [rel basix_i_movzx_eax_al]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_pxor_xmm1_xmm1:
    lea rcx, [rel basix_i_pxor_xmm1_xmm1]
    mov edx, 4
    jmp basix_emit_bytes

basix_emit_sete:
    lea rcx, [rel basix_i_sete]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setne:
    lea rcx, [rel basix_i_setne]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setl:
    lea rcx, [rel basix_i_setl]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setge:
    lea rcx, [rel basix_i_setge]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setle:
    lea rcx, [rel basix_i_setle]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setg:
    lea rcx, [rel basix_i_setg]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setb:
    lea rcx, [rel basix_i_setb]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setae:
    lea rcx, [rel basix_i_setae]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_setbe:
    lea rcx, [rel basix_i_setbe]
    mov edx, 3
    jmp basix_emit_bytes
basix_emit_seta:
    lea rcx, [rel basix_i_seta]
    mov edx, 3
    jmp basix_emit_bytes

; =========================================================================
; Data
; =========================================================================
basix_code_pos: dd 0

; =========================================================================
; .AXB (arOS eXecutable Binary): COMPILE's output format and RUN's fast
; -load path for it. Each slot's code buffer is a fixed, non-relocatable
; heap address for THIS boot (see this file's top comment and
; basix_symbols.inc's slot block) -- basix_emit_call_to patches CALLs to
; basix_rt_* runtime helpers as rel32 offsets computed against wherever
; the code actually ends up in the active slot's buffer, so an .AXB is
; only ever valid when copied back into a slot buffer under the exact
; same kernel build (kmalloc_large addresses are stable across runs of
; the same boot image, but not across rebuilds/reboots in general --
; BASIX_AXB_KERNEL_ID guards the build-mismatch case as before; a stale
; buffer address from a previous boot is not otherwise possible since
; .AXBs are always re-sniffed and re-loaded fresh by basix_load_program,
; never cached across boots). There is no relocation table (a real one
; would need codegen to record every such patch site instead of just
; applying it -- not built here). BASIX_AXB_KERNEL_ID is the guard
; against a stale/cross-build load -- RUN/LAUNCH refuse to load an
; .AXB whose stamped ID doesn't match this build's, rather than
; jumping into addresses that may no longer mean what they meant at
; compile time. scripts/build.sh
; passes a fresh value (`date +%s`) on every build via `-D
; BASIX_AXB_KERNEL_ID=...`, so ANY kernel rebuild automatically
; invalidates old .AXBs -- this used to be a hand-maintained constant
; someone had to remember to bump whenever basix_rt_* functions moved,
; and predictably went un-bumped across many rebuilds in one dev
; session: an hours-old .AXB loaded fine (kernel_id "matched" the
; never-changed default) and froze the moment it called a runtime
; function whose address had since shifted. The %ifndef below is only
; a fallback for assembling outside scripts/build.sh (e.g. by hand).
;
; Layout (16-byte header + raw code bytes, code_size of them):
;   +0  magic       db 'AXB1'
;   +4  kernel_id   dd  BASIX_AXB_KERNEL_ID
;   +8  code_size   dd  bytes following the header
;   +12 reserved    dd  0
; =========================================================================
%ifndef BASIX_AXB_KERNEL_ID
BASIX_AXB_KERNEL_ID equ 1
%endif

basix_axb_header_buf:
    db 'AXB1'
    dd BASIX_AXB_KERNEL_ID
    dd 0                                  ; code_size -- patched per COMPILE
    dd 0                                  ; reserved
