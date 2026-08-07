; =============================================================================
; BASIX64 recursive-descent parser + single-pass code generator.
;
; Grammar (stage 2: sfloat/dfloat are now real, usable numeric types --
; see basix_combine_operands/basix_emit_store_var below for the int/float
; promotion rules):
;
;   program   := (statement NEWLINE)* EOF
;   statement := LET ident '=' expr
;              | ident '=' expr
;              | ident ':'                              (label)
;              | PRINT printitem (',' printitem)*
;              | IF condition THEN NEWLINE block (ELSE NEWLINE block)? ENDIF
;              | FOR ident '=' expr TO expr (STEP expr)? NEWLINE block NEXT ident?
;              | WHILE condition NEWLINE block WEND
;              | GOTO ident | GOSUB ident | RETURN
;              | DIM ident AS (INTEGER|SFLOAT|DFLOAT)
;              | END
;   printitem := expr | STRING
;   condition := expr relop expr                         (exactly one relop)
;   expr      := term (('+'|'-') term)*
;   term      := unary (('*'|'/'|MOD) unary)*
;   unary     := '-' unary | primary
;   primary   := INT | FLOAT | ident | '(' expr ')'
;
; No boolean-valued expressions: relops are only legal as the entire
; condition of IF/WHILE, compiled directly to a conditional jump rather
; than a materialized 0/1 value. No single-line IF, no AND/OR/NOT, no
; nested-FOR temp collision (each FOR nesting level gets its own hidden
; end/step slot, up to BASIX_MAX_FOR_DEPTH).
; =============================================================================

; -------------------------------------------------------------------------
; basix_expect: EAX = expected token type. If the current token matches,
; consumes it and returns EAX=1; otherwise flags a compile error (without
; consuming) and returns EAX=0.
; -------------------------------------------------------------------------
basix_expect:
    cmp [rel basix_tok_type], eax
    je .match
    mov dword [rel basix_compile_ok], 0
    xor eax, eax
    ret
.match:
    call basix_lex_next
    mov eax, 1
    ret

; -------------------------------------------------------------------------
; Value-type convention for every expression-producing routine below
; (basix_parse_primary/unary/term/arith/condition): the routine emits
; code that computes the value at runtime into RAX (int) or XMM0 (float
; -- sfloat widened to double via CVTSS2SD on load, narrowed back via
; CVTSD2SS only when stored into an sfloat variable, so there is exactly
; one float code path internally), and returns EAX_TYPE in R11D: 0=int,
; 2=float. (1, TOK type SFLOAT's value, is never returned by an
; expression -- it only appears as a variable's *declared* storage type.)
; -------------------------------------------------------------------------

; -------------------------------------------------------------------------
; basix_emit_push_operand: R10D = type of the value currently computed
; (RAX if int, XMM0 if float). Emits the matching push.
; -------------------------------------------------------------------------
basix_emit_push_operand:
    test r10d, r10d
    jnz .float
    jmp basix_emit_push_rax
.float:
    jmp basix_emit_push_xmm0

; -------------------------------------------------------------------------
; basix_combine_operands: R10D = left's type (left was already pushed via
; basix_emit_push_operand), R11D = right's type (right's value currently
; in RAX or XMM0, per R11D). Arranges the final operand pair for a binary
; op -- RAX=left/RCX=right if both int, else XMM0=left/XMM1=right (both
; widened to double) -- and returns EAX = the combined type.
; -------------------------------------------------------------------------
basix_combine_operands:
    test r10d, r10d
    jnz .left_float
    test r11d, r11d
    jnz .left_int_right_float
    call basix_emit_mov_rcx_rax
    call basix_emit_pop_rax
    xor eax, eax
    ret
.left_int_right_float:
    call basix_emit_mov_xmm1_xmm0
    call basix_emit_pop_rax
    call basix_emit_cvtsi2sd_x0_rax
    mov eax, 2
    ret
.left_float:
    test r11d, r11d
    jnz .both_float
    call basix_emit_cvtsi2sd_x1_rax
    call basix_emit_pop_xmm0
    mov eax, 2
    ret
.both_float:
    call basix_emit_mov_xmm1_xmm0
    call basix_emit_pop_xmm0
    mov eax, 2
    ret

; -------------------------------------------------------------------------
; basix_ensure_int: R11D = a value's type. If float, truncates XMM0 into
; RAX (used where a construct is int-only in v1, e.g. FOR loop control).
; -------------------------------------------------------------------------
basix_ensure_int:
    cmp r11d, 0
    je .done
    jmp basix_emit_cvttsd2si_rax_x0
.done:
    ret

; -------------------------------------------------------------------------
; basix_emit_store_var: EAX = variable index, R11D = the just-evaluated
; expression's type (value in RAX or XMM0). Converts as needed for the
; variable's DECLARED type and emits the store.
; -------------------------------------------------------------------------
basix_emit_store_var:
    push r9
    push r10
    mov r10d, eax
    mov r9d, [rel basix_var_type + rax*4]

    cmp r9d, 0
    jne .target_float
    cmp r11d, 0
    je .store_int_direct
    call basix_emit_cvttsd2si_rax_x0
.store_int_direct:
    mov eax, r10d
    call basix_var_slot_addr
    call basix_emit_mov_moffs_rax
    jmp .out

.target_float:
    cmp r11d, 0
    jne .value_already_float
    call basix_emit_cvtsi2sd_x0_rax
.value_already_float:
    cmp r9d, 1
    jne .store_dfloat
    call basix_emit_cvtsd2ss_x0_x0
    mov eax, r10d
    call basix_var_slot_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movss_store_rax
    jmp .out
.store_dfloat:
    mov eax, r10d
    call basix_var_slot_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax
.out:
    pop r10
    pop r9
    ret

; -------------------------------------------------------------------------
; basix_parse_primary
; -------------------------------------------------------------------------
basix_parse_primary:
    mov eax, [rel basix_tok_type]
    cmp eax, TOK_INT
    je .int_lit
    cmp eax, TOK_FLOAT
    je .float_lit
    cmp eax, TOK_IDENT
    je .ident
    cmp eax, TOK_LPAREN
    je .paren

    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    xor r11d, r11d
    ret

.int_lit:
    mov rax, [rel basix_tok_ival]
    call basix_emit_mov_rax_imm64
    call basix_lex_next
    xor r11d, r11d
    ret

.float_lit:
    mov rax, [rel basix_tok_dval]        ; raw IEEE754 bits (compile-time)
    call basix_emit_mov_rax_imm64        ; runtime: mov rax, <those bits>
    call basix_emit_movq_xmm0_rax        ; runtime: movq xmm0, rax
    call basix_lex_next
    mov r11d, 2
    ret

.ident:
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r10d, eax
    mov r9d, [rel basix_var_type + r10*4]
    mov eax, r10d
    call basix_var_slot_addr

    cmp r9d, 0
    jne .ident_float
    call basix_emit_mov_rax_moffs
    call basix_lex_next
    xor r11d, r11d
    ret

.ident_float:
    call basix_emit_mov_rax_imm64        ; runtime: mov rax, <slot addr>
    cmp r9d, 1
    je .ident_sfloat
    call basix_emit_movsd_load_rax
    jmp .ident_float_done
.ident_sfloat:
    call basix_emit_movss_load_rax
    call basix_emit_cvtss2sd_x0_x0
.ident_float_done:
    call basix_lex_next
    mov r11d, 2
    ret

.paren:
    call basix_lex_next
    call basix_parse_arith
    mov eax, TOK_RPAREN
    call basix_expect
    ret

; -------------------------------------------------------------------------
; basix_parse_unary
; -------------------------------------------------------------------------
basix_parse_unary:
    mov eax, [rel basix_tok_type]
    cmp eax, TOK_MINUS
    jne basix_parse_primary
    call basix_lex_next
    call basix_parse_unary
    cmp r11d, 0
    je .int_neg
    jmp basix_emit_negsd
.int_neg:
    jmp basix_emit_neg_rax

; -------------------------------------------------------------------------
; basix_parse_term
; -------------------------------------------------------------------------
basix_parse_term:
    push r10
    push r12
    call basix_parse_unary
    mov r10d, r11d                      ; running left type
.loop:
    mov r12d, [rel basix_tok_type]
    cmp r12d, TOK_STAR
    je .op
    cmp r12d, TOK_SLASH
    je .op
    cmp r12d, TOK_MOD
    je .op
    jmp .done
.op:
    call basix_lex_next
    call basix_emit_push_operand
    call basix_parse_unary               ; r11d = right's type
    call basix_combine_operands          ; eax = combined type
    mov r10d, eax

    cmp r12d, TOK_MOD
    je .domod
    cmp r10d, 0
    jne .float_op
    cmp r12d, TOK_STAR
    je .imul
    call basix_emit_cqo
    call basix_emit_idiv_rcx
    jmp .loop
.imul:
    call basix_emit_imul_rax_rcx
    jmp .loop
.float_op:
    cmp r12d, TOK_STAR
    je .fmul
    call basix_emit_divsd
    jmp .loop
.fmul:
    call basix_emit_mulsd
    jmp .loop
.domod:
    cmp r10d, 0
    je .domod_int
    mov dword [rel basix_compile_ok], 0  ; MOD requires both operands int
    jmp .loop
.domod_int:
    call basix_emit_cqo
    call basix_emit_idiv_rcx
    call basix_emit_mov_rax_rdx
    jmp .loop
.done:
    mov r11d, r10d
    pop r12
    pop r10
    ret

; -------------------------------------------------------------------------
; basix_parse_arith
; -------------------------------------------------------------------------
basix_parse_arith:
    push r10
    push r12
    call basix_parse_term
    mov r10d, r11d
.loop:
    mov r12d, [rel basix_tok_type]
    cmp r12d, TOK_PLUS
    je .op
    cmp r12d, TOK_MINUS
    je .op
    jmp .done
.op:
    call basix_lex_next
    call basix_emit_push_operand
    call basix_parse_term                ; r11d = right's type
    call basix_combine_operands          ; eax = combined type
    mov r10d, eax

    cmp r10d, 0
    jne .float_op
    cmp r12d, TOK_PLUS
    je .iadd
    call basix_emit_sub_rax_rcx
    jmp .loop
.iadd:
    call basix_emit_add_rax_rcx
    jmp .loop
.float_op:
    cmp r12d, TOK_PLUS
    je .fadd
    call basix_emit_subsd
    jmp .loop
.fadd:
    call basix_emit_addsd
    jmp .loop
.done:
    mov r11d, r10d
    pop r12
    pop r10
    ret

; -------------------------------------------------------------------------
; basix_parse_condition: parses "arith relop arith", emits the comparison
; (integer CMP or float COMISD, per the combined operand type) and an
; inverted conditional jump (taken when the condition is FALSE). Returns
; RAX = absolute address of the jump's rel32 field, to be patched later
; with the false-branch target via basix_patch_rel32.
; -------------------------------------------------------------------------
basix_parse_condition:
    push r10
    push r12
    call basix_parse_arith
    mov r10d, r11d
    call basix_emit_push_operand

    mov r12d, [rel basix_tok_type]
    cmp r12d, TOK_EQ
    je .relop_ok
    cmp r12d, TOK_NE
    je .relop_ok
    cmp r12d, TOK_LT
    je .relop_ok
    cmp r12d, TOK_GT
    je .relop_ok
    cmp r12d, TOK_LE
    je .relop_ok
    cmp r12d, TOK_GE
    je .relop_ok
    mov dword [rel basix_compile_ok], 0
    jmp .after_relop
.relop_ok:
    call basix_lex_next
.after_relop:

    call basix_parse_arith               ; r11d = right's type
    call basix_combine_operands          ; eax = combined type

    cmp eax, 0
    jne .float_cmp
    call basix_emit_cmp_rax_rcx
    jmp .pick_jcc_int
.float_cmp:
    call basix_emit_comisd

    cmp r12d, TOK_EQ
    je .finv_ne
    cmp r12d, TOK_NE
    je .finv_eq
    cmp r12d, TOK_LT
    je .finv_ge
    cmp r12d, TOK_GT
    je .finv_le
    cmp r12d, TOK_LE
    je .finv_gt
    mov al, 0x82                        ; TOK_GE -> JB
    jmp .emit_jcc
.finv_ne:
    mov al, 0x85                        ; JNE
    jmp .emit_jcc
.finv_eq:
    mov al, 0x84                        ; JE
    jmp .emit_jcc
.finv_ge:
    mov al, 0x83                        ; JAE
    jmp .emit_jcc
.finv_le:
    mov al, 0x86                        ; JBE
    jmp .emit_jcc
.finv_gt:
    mov al, 0x87                        ; JA
    jmp .emit_jcc

.pick_jcc_int:
    cmp r12d, TOK_EQ
    je .inv_ne
    cmp r12d, TOK_NE
    je .inv_eq
    cmp r12d, TOK_LT
    je .inv_ge
    cmp r12d, TOK_GT
    je .inv_le
    cmp r12d, TOK_LE
    je .inv_gt
    mov al, 0x8C                        ; TOK_GE -> JL
    jmp .emit_jcc
.inv_ne:
    mov al, 0x85                        ; JNE
    jmp .emit_jcc
.inv_eq:
    mov al, 0x84                        ; JE
    jmp .emit_jcc
.inv_ge:
    mov al, 0x8D                        ; JGE
    jmp .emit_jcc
.inv_le:
    mov al, 0x8E                        ; JLE
    jmp .emit_jcc
.inv_gt:
    mov al, 0x8F                        ; JG
.emit_jcc:
    call basix_emit_jcc_placeholder
    pop r12
    pop r10
    ret

; -------------------------------------------------------------------------
; basix_parse_block: EAX = terminator1, EDX = terminator2 (0xFFFFFFFF if
; unused). Parses statements until one of the terminators is the current
; token (left unconsumed for the caller).
; -------------------------------------------------------------------------
basix_parse_block:
    push r13
    push r14
    mov r13d, eax
    mov r14d, edx
.loop:
    mov eax, [rel basix_tok_type]
    cmp eax, r13d
    je .done
    cmp r14d, 0xFFFFFFFF
    je .not_term2
    cmp eax, r14d
    je .done
.not_term2:
    cmp eax, TOK_EOF
    je .unexpected_eof
    cmp eax, TOK_NEWLINE
    je .skip_blank
    call basix_parse_statement
    cmp dword [rel basix_tok_type], TOK_NEWLINE
    jne .after_stmt
    call basix_lex_next
.after_stmt:
    jmp .loop
.skip_blank:
    call basix_lex_next
    jmp .loop
.unexpected_eof:
    mov dword [rel basix_compile_ok], 0
.done:
    pop r14
    pop r13
    ret

; -------------------------------------------------------------------------
; basix_parse_assignment: current token is the (already-known) target
; identifier, NOT yet consumed. Parses "ident '=' expr" and emits the
; store.
; -------------------------------------------------------------------------
basix_parse_assignment:
    push rbx
    push rdx

    mov rbx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    push rdx
    mov rcx, rbx
    call basix_var_find_or_create
    pop rdx
    push rax

    call basix_lex_next
    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith              ; r11d = expr type

    pop rax
    call basix_emit_store_var

    pop rdx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_if: IF keyword already consumed.
; -------------------------------------------------------------------------
basix_parse_if:
    push r12
    call basix_parse_condition
    mov r12, rax

    mov eax, TOK_THEN
    call basix_expect
    mov eax, TOK_NEWLINE
    call basix_expect

    mov eax, TOK_ELSE
    mov edx, TOK_ENDIF
    call basix_parse_block

    cmp dword [rel basix_tok_type], TOK_ELSE
    jne .no_else

    call basix_emit_jmp_placeholder
    push rax

    call basix_emit_here
    mov rcx, r12
    mov rdx, rax
    call basix_patch_rel32

    call basix_lex_next
    mov eax, TOK_NEWLINE
    call basix_expect

    mov eax, TOK_ENDIF
    mov edx, 0xFFFFFFFF
    call basix_parse_block

    pop rcx
    call basix_emit_here
    mov rdx, rax
    call basix_patch_rel32
    jmp .expect_endif

.no_else:
    call basix_emit_here
    mov rcx, r12
    mov rdx, rax
    call basix_patch_rel32

.expect_endif:
    mov eax, TOK_ENDIF
    call basix_expect
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_for: FOR keyword already consumed.
; -------------------------------------------------------------------------
basix_parse_for:
    push r12
    push r13
    push r14
    push r15

    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail

    mov r12, [rel basix_tok_text]
    mov r13d, [rel basix_tok_len]
    call basix_lex_next

    push r12
    push r13
    mov rcx, r12
    mov edx, r13d
    call basix_var_find_or_create
    pop r13
    pop r12
    call basix_var_slot_addr
    mov r14, rax                        ; r14 = loop var slot addr

    mov eax, [rel basix_for_depth]
    cmp eax, BASIX_MAX_FOR_DEPTH
    jge .bail
    mov r15d, eax
    inc dword [rel basix_for_depth]

    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith              ; FOR control values are int-only
    call basix_ensure_int               ; in v1 -- truncate if a float
    mov rax, r14                        ; expression sneaks in here.
    call basix_emit_mov_moffs_rax

    mov eax, TOK_TO
    call basix_expect
    call basix_parse_arith
    call basix_ensure_int
    mov eax, r15d
    imul eax, eax, 8
    lea rax, [rel basix_for_end_temps + rax]
    call basix_emit_mov_moffs_rax

    cmp dword [rel basix_tok_type], TOK_STEP
    jne .default_step
    call basix_lex_next
    call basix_parse_arith
    call basix_ensure_int
    jmp .store_step
.default_step:
    mov rax, 1
    call basix_emit_mov_rax_imm64
.store_step:
    mov eax, r15d
    imul eax, eax, 8
    lea rax, [rel basix_for_step_temps + rax]
    call basix_emit_mov_moffs_rax

    mov eax, TOK_NEWLINE
    call basix_expect

    call basix_emit_here
    mov r12, rax                        ; r12 = loop_top

    mov rax, r14
    call basix_emit_mov_rax_moffs
    call basix_emit_push_rax
    mov eax, r15d
    imul eax, eax, 8
    lea rax, [rel basix_for_end_temps + rax]
    call basix_emit_mov_rax_moffs
    call basix_emit_mov_rcx_rax
    call basix_emit_pop_rax
    call basix_emit_cmp_rax_rcx
    mov al, 0x8F                        ; JG -- exit when var > end
    call basix_emit_jcc_placeholder
    push rax                            ; save exit patch addr (r13 needed below)

    mov eax, TOK_NEXT
    mov edx, 0xFFFFFFFF
    call basix_parse_block

    mov rax, r14
    call basix_emit_mov_rax_moffs
    call basix_emit_push_rax
    mov eax, r15d
    imul eax, eax, 8
    lea rax, [rel basix_for_step_temps + rax]
    call basix_emit_mov_rax_moffs
    call basix_emit_mov_rcx_rax
    call basix_emit_pop_rax
    call basix_emit_add_rax_rcx
    mov rax, r14
    call basix_emit_mov_moffs_rax

    mov rcx, r12
    call basix_emit_jmp_to

    pop rcx                             ; exit patch addr
    call basix_emit_here
    mov rdx, rax
    call basix_patch_rel32

    dec dword [rel basix_for_depth]

    mov eax, TOK_NEXT
    call basix_expect
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .out
    call basix_lex_next
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_while: WHILE keyword already consumed.
; -------------------------------------------------------------------------
basix_parse_while:
    push r12
    push r13

    call basix_emit_here
    mov r12, rax

    call basix_parse_condition
    mov r13, rax

    mov eax, TOK_NEWLINE
    call basix_expect

    mov eax, TOK_WEND
    mov edx, 0xFFFFFFFF
    call basix_parse_block

    mov rcx, r12
    call basix_emit_jmp_to

    call basix_emit_here
    mov rcx, r13
    mov rdx, rax
    call basix_patch_rel32

    mov eax, TOK_WEND
    call basix_expect

    pop r13
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_print_list: PRINT keyword already consumed.
; -------------------------------------------------------------------------
basix_parse_print_list:
    push r12
    push r13
.item:
    cmp dword [rel basix_tok_type], TOK_STRING
    je .str_item
    call basix_parse_arith
    cmp r11d, 0
    jne .print_float
    call basix_emit_mov_rcx_rax
    lea rcx, [rel basix_rt_print_int]
    call basix_emit_call_to
    jmp .after_item
.print_float:
    lea rcx, [rel basix_rt_print_double]
    call basix_emit_call_to
    jmp .after_item
.str_item:
    mov r12, [rel basix_tok_text]
    mov r13d, [rel basix_tok_len]
    mov rax, r12
    call basix_emit_mov_rcx_imm64
    mov eax, r13d
    call basix_emit_mov_rdx_imm64
    lea rcx, [rel basix_rt_print_str]
    call basix_emit_call_to
    call basix_lex_next
.after_item:
    cmp dword [rel basix_tok_type], TOK_COMMA
    jne .done_items
    call basix_lex_next
    jmp .item
.done_items:
    lea rcx, [rel basix_rt_newline]
    call basix_emit_call_to
    pop r13
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_dim: DIM keyword already consumed.
; -------------------------------------------------------------------------
basix_parse_dim:
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail

    push qword [rel basix_tok_text]
    push qword [rel basix_tok_len]
    call basix_lex_next

    mov eax, TOK_AS
    call basix_expect

    xor r8d, r8d
    cmp dword [rel basix_tok_type], TOK_INTEGER
    je .type_ok
    cmp dword [rel basix_tok_type], TOK_SFLOAT
    jne .check_dfloat
    mov r8d, 1
    jmp .type_ok
.check_dfloat:
    cmp dword [rel basix_tok_type], TOK_DFLOAT
    jne .bail_after_push
    mov r8d, 2
.type_ok:
    call basix_lex_next

    pop rdx
    pop rcx
    push r8
    call basix_var_find_or_create
    pop r8
    mov [rel basix_var_type + rax*4], r8d
    ret

.bail_after_push:
    pop rdx
    pop rcx
.bail:
    mov dword [rel basix_compile_ok], 0
    ret

; -------------------------------------------------------------------------
; basix_parse_statement: parses exactly one statement (not its trailing
; NEWLINE).
; -------------------------------------------------------------------------
basix_parse_statement:
    push rbx
    push r13
    push r15

    mov r15d, [rel basix_tok_type]

    cmp r15d, TOK_LET
    je .do_let
    cmp r15d, TOK_PRINT
    je .do_print
    cmp r15d, TOK_IF
    je .do_if
    cmp r15d, TOK_FOR
    je .do_for
    cmp r15d, TOK_WHILE
    je .do_while
    cmp r15d, TOK_GOTO
    je .do_goto
    cmp r15d, TOK_GOSUB
    je .do_gosub
    cmp r15d, TOK_RETURN
    je .do_return
    cmp r15d, TOK_DIM
    je .do_dim
    cmp r15d, TOK_END
    je .do_end
    cmp r15d, TOK_IDENT
    je .do_ident_stmt

    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    jmp .out

.do_let:
    call basix_lex_next
    call basix_parse_assignment
    jmp .out

.do_ident_stmt:
    mov rbx, [rel basix_tok_text]
    mov r13d, [rel basix_tok_len]
    call basix_lex_next
    cmp dword [rel basix_tok_type], TOK_COLON
    je .label_def

    mov eax, TOK_EQ
    call basix_expect
    push rbx
    mov rcx, rbx
    mov edx, r13d
    call basix_var_find_or_create
    pop rbx
    push rax
    call basix_parse_arith              ; r11d = expr type
    pop rax
    call basix_emit_store_var
    jmp .out

.label_def:
    mov rcx, rbx
    mov edx, r13d
    call basix_label_find_or_create
    call basix_label_define
    call basix_lex_next
    jmp .out

.do_print:
    call basix_lex_next
    call basix_parse_print_list
    jmp .out

.do_if:
    call basix_lex_next
    call basix_parse_if
    jmp .out

.do_for:
    call basix_lex_next
    call basix_parse_for
    jmp .out

.do_while:
    call basix_lex_next
    call basix_parse_while
    jmp .out

.do_goto:
    call basix_lex_next
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bad_target
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    push rcx
    push rdx
    call basix_lex_next
    pop rdx
    pop rcx
    xor r8d, r8d
    call basix_goto_or_gosub_emit
    jmp .out

.do_gosub:
    call basix_lex_next
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bad_target
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    push rcx
    push rdx
    call basix_lex_next
    pop rdx
    pop rcx
    mov r8d, 1
    call basix_goto_or_gosub_emit
    jmp .out

.bad_target:
    mov dword [rel basix_compile_ok], 0
    jmp .out

.do_return:
    call basix_lex_next
    call basix_emit_ret_instr
    jmp .out

.do_dim:
    call basix_lex_next
    call basix_parse_dim
    jmp .out

.do_end:
    call basix_lex_next
    call basix_emit_ret_instr
    jmp .out

.out:
    pop r15
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_compile: RCX = ptr to a NUL-terminated BASIX64 source buffer.
; Compiles it into basix_code_buf (callable at that address on success).
; Returns EAX=1/0.
; -------------------------------------------------------------------------
basix_compile:
    push rbx

    mov dword [rel basix_compile_ok], 1
    mov dword [rel basix_code_pos], 0
    mov dword [rel basix_var_count], 0
    mov dword [rel basix_label_count], 0
    mov dword [rel basix_fixup_count], 0
    mov dword [rel basix_for_depth], 0

    call basix_lex_init
    call basix_lex_next

.loop:
    cmp dword [rel basix_tok_type], TOK_EOF
    je .done
    cmp dword [rel basix_tok_type], TOK_NEWLINE
    je .skip_blank
    call basix_parse_statement
    cmp dword [rel basix_tok_type], TOK_NEWLINE
    jne .maybe_eof
    call basix_lex_next
    jmp .loop
.maybe_eof:
    cmp dword [rel basix_tok_type], TOK_EOF
    je .done
    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    jmp .loop
.skip_blank:
    call basix_lex_next
    jmp .loop

.done:
    call basix_emit_ret_instr

    xor ebx, ebx
.check_labels:
    cmp ebx, [rel basix_label_count]
    jge .labels_ok
    cmp dword [rel basix_label_defined + rbx*4], 0
    jne .next_label
    mov dword [rel basix_compile_ok], 0
.next_label:
    inc ebx
    jmp .check_labels
.labels_ok:

    mov eax, [rel basix_compile_ok]
    pop rbx
    ret
