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

BASIX_CODE_BUF_SIZE equ 65536

; -------------------------------------------------------------------------
; basix_emit_u8: AL = byte to append.
; -------------------------------------------------------------------------
basix_emit_u8:
    push rbx
    push rcx
    mov ecx, [rel basix_code_pos]
    lea rbx, [rel basix_code_buf]
    mov [rbx+rcx], al
    inc dword [rel basix_code_pos]
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
    lea rbx, [rel basix_code_buf]
    mov [rbx+rcx], edx
    add dword [rel basix_code_pos], 4
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
    lea rbx, [rel basix_code_buf]
    mov [rbx+rcx], rdx
    add dword [rel basix_code_pos], 8
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
    lea rbx, [rel basix_code_buf]
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
; basix_emit_call_to: RCX = absolute target address. Emits a near CALL.
; -------------------------------------------------------------------------
basix_emit_call_to:
    push rcx
    mov al, 0xE8
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

; =========================================================================
; Data
; =========================================================================
basix_code_pos: dd 0

align 16
basix_code_buf: times BASIX_CODE_BUF_SIZE db 0
