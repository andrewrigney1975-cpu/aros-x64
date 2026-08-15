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
;              | IF condition THEN statement (ELSE statement)?  (single-line)
;              | FOR ident '=' expr TO expr (STEP expr)? NEWLINE block NEXT ident?
;              | WHILE condition NEWLINE block WEND
;              | GOTO ident | GOSUB ident | RETURN
;              | DIM ident AS (INTEGER|SFLOAT|DFLOAT|VECTOR2|VECTOR3|VECTOR4|MATRIX4)
;              | END
;              | VSET ident (',' expr)+
;              | (VADD|VSUB) ident ',' ident ',' ident
;              | VSCALE ident ',' ident ',' expr
;              | VDOT ident ',' ident ',' ident
;              | VCROSS ident ',' ident ',' ident           (VECTOR3 only)
;              | (VLEN|VNORM) ident ',' ident
;              | MIDENT ident                                (MATRIX4 only)
;              | MMUL ident ',' ident ',' ident               (MATRIX4 only)
;              | MVMUL ident ',' ident ',' ident      (MATRIX4 * VECTOR4)
;   printitem := expr | STRING
;   condition := bool_or
;   bool_or   := bool_and ('OR' bool_and)*
;   bool_and  := bool_unary ('AND' bool_unary)*
;   bool_unary:= 'NOT' bool_unary | comparison
;   comparison:= expr relop expr | expr     (bare expr: nonzero = true)
;   expr      := term (('+'|'-') term)*
;   term      := unary (('*'|'/'|MOD) unary)*
;   unary     := '-' unary | primary
;   primary   := INT | FLOAT | ident | '(' expr ')'
;
; Comparisons materialize a plain 0/1 int (SETcc+MOVZX) rather than
; jumping directly, so AND/OR/NOT compose with ordinary integer bitwise
; ops -- no short-circuit evaluation (every operand is always fully
; evaluated, which is fine as long as expressions have no side effects,
; true in this language as of stage 2). basix_parse_condition adds one
; final TEST+JE on top to get back to a single false-jump patch address
; for IF/WHILE. No nested-FOR temp collision (each FOR nesting level
; gets its own hidden end/step slot, up to BASIX_MAX_FOR_DEPTH).
;
; Vector/matrix values (VECTOR2/3/4, MATRIX4) are statement-level only --
; they don't flow through the general expr grammar as first-class values
; the way int/float do (that would need the type-tracking system to
; carry width and route through memory-resident temporaries everywhere,
; a much bigger redesign). Instead: DIM declares one, VSET/VADD/etc.
; operate on named vector/matrix variables directly, and ident[N] (N a
; compile-time integer literal, not a general expression -- v1 scope)
; reads/writes a single scalar component, which *does* fit the normal
; expr grammar since it's just a double. Every VSET/VADD/.../MMUL/MVMUL
; statement is unrolled entirely at compile time (component indices are
; always compile-time constants), so "looping over components" in this
; parser is the *parser* looping while compiling, not code generated to
; loop at runtime -- each component gets its own fixed-address load/op/
; store sequence emitted directly.
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
; basix_emit_store_var_indexed: EAX = variable index, R12 = component
; index (compile-time constant), R11D = the just-evaluated expression's
; type. Stores into that component of the variable's wide (vector/
; matrix) slot, converting int->double if needed (vector/matrix
; components are always doubles, regardless of the language's separate
; sfloat/dfloat distinction for scalars).
; -------------------------------------------------------------------------
basix_emit_store_var_indexed:
    push r10
    mov r10, r12
    call basix_var_wide_slot_addr
    imul r10, r10, 8
    add rax, r10
    push rax
    cmp r11d, 0
    jne .already_float
    call basix_emit_cvtsi2sd_x0_rax
.already_float:
    pop rax
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax
    pop r10
    ret

; =============================================================================
; Vector/matrix support (VECTOR2/3/4, MATRIX4). Every operation below is
; unrolled entirely at compile time -- component indices are always
; compile-time constants (v1 scope: no runtime-computed indices), so a
; "loop over components" in this parser is a loop that runs while
; *compiling*, emitting one fixed-address load/op/store sequence per
; component rather than a real runtime loop. Matrices are row-major,
; 16 doubles, M[row][col] at flat index row*4+col.
; =============================================================================

; -------------------------------------------------------------------------
; basix_var_component_count: EAX = a var_type value. Returns ECX = its
; component count (2/3/4/16), or 0 if it's not a vector/matrix type.
; -------------------------------------------------------------------------
basix_var_component_count:
    cmp eax, 3
    je .v2
    cmp eax, 4
    je .v3
    cmp eax, 5
    je .v4
    cmp eax, 6
    je .m4
    xor ecx, ecx
    ret
.v2:
    mov ecx, 2
    ret
.v3:
    mov ecx, 3
    ret
.v4:
    mov ecx, 4
    ret
.m4:
    mov ecx, 16
    ret

; -------------------------------------------------------------------------
; basix_wide_component_addr: EAX = variable index, ECX = component index
; (both compile-time constants). Returns RAX = absolute address of that
; double.
; -------------------------------------------------------------------------
basix_wide_component_addr:
    push rcx
    call basix_var_wide_slot_addr
    pop rcx
    imul ecx, ecx, 8
    add rax, rcx
    ret

; -------------------------------------------------------------------------
; basix_resolve_vec_ident: assumes the current token is the (not yet
; consumed) name of a vector/matrix variable. Creates it if new
; (defaulting to INTEGER, which will then fail the caller's component-
; count check -- variables used this way should always have been DIM'd
; explicitly first). Consumes the identifier. Returns EAX = var index,
; ECX = component count (0 if not actually a vector/matrix type).
; -------------------------------------------------------------------------
basix_resolve_vec_ident:
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .fail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    push rax
    mov ecx, [rel basix_var_type + rax*4]
    mov eax, ecx
    call basix_var_component_count
    pop rax
    call basix_lex_next
    ret
.fail:
    mov dword [rel basix_compile_ok], 0
    xor eax, eax
    xor ecx, ecx
    ret

; -------------------------------------------------------------------------
; basix_emit_cross_term: RCX = a_idx, RDX = b_idx, R8D = i, R9D = j.
; Emits code computing XMM0 = a[i]*b[j] - a[j]*b[i] (one component of a
; 3D cross product).
; -------------------------------------------------------------------------
basix_emit_cross_term:
    push rbx
    push r10
    push r11
    push r12
    mov ebx, ecx
    mov r10d, edx
    mov r11d, r8d
    mov r12d, r9d

    mov eax, ebx
    mov ecx, r11d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax       ; xmm0 = a[i]

    mov eax, r10d
    mov ecx, r12d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1    ; xmm1 = b[j]

    call basix_emit_mulsd                ; xmm0 = a[i]*b[j]
    call basix_emit_push_xmm0

    mov eax, ebx
    mov ecx, r12d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax       ; xmm0 = a[j]

    mov eax, r10d
    mov ecx, r11d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1    ; xmm1 = b[i]

    call basix_emit_mulsd                ; xmm0 = a[j]*b[i]
    call basix_emit_mov_xmm1_xmm0        ; xmm1 = a[j]*b[i]
    call basix_emit_pop_xmm0             ; xmm0 = a[i]*b[j]
    call basix_emit_subsd                ; xmm0 = a[i]*b[j] - a[j]*b[i]

    pop r12
    pop r11
    pop r10
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vset: "VSET target, v0, v1, ..." -- sets all of target's
; components. The value count must exactly match target's declared type.
; -------------------------------------------------------------------------
basix_parse_vset:
    push rbx
    push r13
    push r14

    call basix_resolve_vec_ident
    mov ebx, eax
    mov r13d, ecx
    test r13d, r13d
    jz .bail

    xor r14d, r14d
.loop:
    cmp r14d, r13d
    jge .check_extra
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_arith               ; r11d = expr type
    mov eax, ebx
    mov r12, r14
    call basix_emit_store_var_indexed
    inc r14d
    jmp .loop
.check_extra:
    cmp dword [rel basix_tok_type], TOK_COMMA
    jne .out
    mov dword [rel basix_compile_ok], 0  ; too many values supplied
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vadd / basix_parse_vsub: "VADD target, a, b" / "VSUB ...":
; target = a +/- b, component-wise. All three must share the same
; vector/matrix type.
; -------------------------------------------------------------------------
basix_parse_vadd:
    xor r12d, r12d
    jmp basix_parse_vaddsub_common
basix_parse_vsub:
    mov r12d, 1
    jmp basix_parse_vaddsub_common

basix_parse_vaddsub_common:
    push rbx
    push r12
    push r13
    push r14
    push r15

    call basix_resolve_vec_ident         ; target
    mov ebx, eax
    mov r13d, ecx
    test r13d, r13d
    jz .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident         ; a
    mov r14d, eax
    cmp ecx, r13d
    jne .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident         ; b
    mov r15d, eax
    cmp ecx, r13d
    jne .bail

    xor r10d, r10d
.loop:
    cmp r10d, r13d
    jge .done
    mov r11d, r10d
    shl r11, 3                           ; component byte offset

    mov eax, r14d
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax       ; xmm0 = a[i]

    mov eax, r15d
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1    ; xmm1 = b[i]

    test r12d, r12d
    jnz .do_sub
    call basix_emit_addsd
    jmp .store_result
.do_sub:
    call basix_emit_subsd
.store_result:
    mov eax, ebx
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .loop
.done:
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vscale: "VSCALE target, a, scalar" -- target = a * scalar
; (scalar may be any arithmetic expression, int or float).
; -------------------------------------------------------------------------
basix_parse_vscale:
    push rbx
    push r13
    push r14

    call basix_resolve_vec_ident         ; target
    mov ebx, eax
    mov r13d, ecx
    test r13d, r13d
    jz .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident         ; a
    mov r14d, eax
    cmp ecx, r13d
    jne .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_arith               ; scalar expr, r11d = type
    cmp r11d, 0
    jne .have_scalar_float
    call basix_emit_cvtsi2sd_x0_rax
.have_scalar_float:
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax      ; stash scalar

    xor r10d, r10d
.loop:
    cmp r10d, r13d
    jge .done
    mov r11d, r10d
    shl r11, 3

    mov eax, r14d
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax       ; xmm0 = a[i]

    push r11
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1    ; xmm1 = scalar
    pop r11

    call basix_emit_mulsd

    mov eax, ebx
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .loop
.done:
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vdot: "VDOT target, a, b" -- target (a plain scalar
; variable) = dot(a, b).
; -------------------------------------------------------------------------
basix_parse_vdot:
    push rbx
    push r13
    push r14
    push r15

    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov ebx, eax                          ; target scalar var
    call basix_lex_next

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident          ; a
    mov r14d, eax
    mov r13d, ecx
    test r13d, r13d
    jz .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident          ; b
    mov r15d, eax
    cmp ecx, r13d
    jne .bail

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax       ; scalar_temp = 0.0

    xor r10d, r10d
.loop:
    cmp r10d, r13d
    jge .finish
    mov r11d, r10d
    shl r11, 3

    mov eax, r14d
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax        ; xmm0 = a[i]

    mov eax, r15d
    call basix_var_wide_slot_addr
    add rax, r11
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1     ; xmm1 = b[i]

    call basix_emit_mulsd                 ; xmm0 = a[i]*b[i]
    call basix_emit_mov_xmm1_xmm0

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_addsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .loop
.finish:
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    mov r11d, 2
    mov eax, ebx
    call basix_emit_store_var
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r15
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vcross: "VCROSS target, a, b" -- target = a x b. VECTOR3
; only.
; -------------------------------------------------------------------------
basix_parse_vcross:
    push rbx
    push r13
    push r14

    call basix_resolve_vec_ident         ; target
    mov ebx, eax
    cmp ecx, 3
    jne .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident         ; a
    mov r13d, eax
    cmp ecx, 3
    jne .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident         ; b
    mov r14d, eax
    cmp ecx, 3
    jne .bail

    mov ecx, r13d
    mov edx, r14d
    mov r8d, 1
    mov r9d, 2
    call basix_emit_cross_term
    mov eax, ebx
    mov ecx, 0
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    mov ecx, r13d
    mov edx, r14d
    mov r8d, 2
    mov r9d, 0
    call basix_emit_cross_term
    mov eax, ebx
    mov ecx, 1
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    mov ecx, r13d
    mov edx, r14d
    mov r8d, 0
    mov r9d, 1
    call basix_emit_cross_term
    mov eax, ebx
    mov ecx, 2
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vlen: "VLEN target, a" -- target (scalar) = |a|.
; -------------------------------------------------------------------------
basix_parse_vlen:
    push rbx
    push r13
    push r14

    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov ebx, eax
    call basix_lex_next

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident          ; a
    mov r13d, eax
    mov r14d, ecx
    test r14d, r14d
    jz .bail

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax

    xor r10d, r10d
.loop:
    cmp r10d, r14d
    jge .finish
    mov eax, r13d
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_mov_xmm1_xmm0
    call basix_emit_mulsd                 ; xmm0 = a[i]^2
    call basix_emit_mov_xmm1_xmm0

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_addsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .loop
.finish:
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_sqrtsd
    mov r11d, 2
    mov eax, ebx
    call basix_emit_store_var
    jmp .out
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_vnorm: "VNORM target, a" -- target = a / |a|.
; -------------------------------------------------------------------------
basix_parse_vnorm:
    push rbx
    push r13
    push r14

    call basix_resolve_vec_ident          ; target
    mov ebx, eax
    mov r14d, ecx
    test r14d, r14d
    jz .bail

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident          ; a
    mov r13d, eax
    cmp ecx, r14d
    jne .bail

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax

    xor r10d, r10d
.sumloop:
    cmp r10d, r14d
    jge .sumdone
    mov eax, r13d
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_mov_xmm1_xmm0
    call basix_emit_mulsd
    call basix_emit_mov_xmm1_xmm0
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_addsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax
    inc r10d
    jmp .sumloop
.sumdone:
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_sqrtsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax       ; scalar_temp = |a|

    xor r10d, r10d
.divloop:
    cmp r10d, r14d
    jge .out
    mov eax, r13d
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax        ; xmm0 = a[i]
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1     ; xmm1 = |a|
    call basix_emit_divsd
    mov eax, ebx
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax
    inc r10d
    jmp .divloop
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_mident: "MIDENT target" -- target (MATRIX4) = identity.
; -------------------------------------------------------------------------
basix_parse_mident:
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail_early
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    push rax
    call basix_lex_next

    xor r10d, r10d
.zero_loop:
    cmp r10d, 16
    jge .set_diag
    mov eax, [rsp]
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax
    inc r10d
    jmp .zero_loop
.set_diag:
    xor r10d, r10d
.diag_loop:
    cmp r10d, 4
    jge .done
    mov rax, 0x3FF0000000000000           ; IEEE754 bits for 1.0
    call basix_emit_mov_rax_imm64
    call basix_emit_movq_xmm0_rax         ; xmm0 = 1.0

    mov eax, [rsp]
    mov ecx, r10d
    imul ecx, ecx, 4
    add ecx, r10d                          ; index = i*4+i
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .diag_loop
.done:
    pop rax
    ret
.bail_early:
    mov dword [rel basix_compile_ok], 0
    ret

; -------------------------------------------------------------------------
; basix_parse_mmul: "MMUL target, a, b" -- target = a*b, standard 4x4
; matrix multiply. Writes through a scratch matrix first so it's safe
; even if target aliases a or b.
; -------------------------------------------------------------------------
basix_parse_mmul:
    push rbx
    push r13
    push r14

    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov ebx, eax                          ; target
    call basix_lex_next

    mov eax, TOK_COMMA
    call basix_expect
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r13d, eax                          ; a
    call basix_lex_next

    mov eax, TOK_COMMA
    call basix_expect
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r14d, eax                          ; b
    call basix_lex_next

    xor r10d, r10d                         ; row
.row_loop:
    cmp r10d, 4
    jge .copy_out
    xor r11d, r11d                         ; col
.col_loop:
    cmp r11d, 4
    jge .row_next

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax

    push r10
    push r11
    xor r12d, r12d                         ; k
.k_loop:
    cmp r12d, 4
    jge .k_done

    mov eax, r13d
    mov ecx, r10d
    imul ecx, ecx, 4
    add ecx, r12d                          ; a[row][k]
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, r14d
    mov ecx, r12d
    imul ecx, ecx, 4
    add ecx, r11d                          ; b[k][col]
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1

    call basix_emit_mulsd
    call basix_emit_mov_xmm1_xmm0
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_addsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r12d
    jmp .k_loop
.k_done:
    pop r11
    pop r10

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, r10d
    imul eax, eax, 4
    add eax, r11d
    imul eax, eax, 8
    lea rax, [rel basix_mat_scratch + rax]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r11d
    jmp .col_loop
.row_next:
    inc r10d
    jmp .row_loop

.copy_out:
    xor r10d, r10d
.copy_loop:
    cmp r10d, 16
    jge .out
    mov eax, r10d
    imul eax, eax, 8
    lea rax, [rel basix_mat_scratch + rax]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, ebx
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .copy_loop
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_mvmul: "MVMUL target, m, v" -- target = m*v. MATRIX4 times
; VECTOR4, giving a VECTOR4. Writes through a scratch vector first so
; it's safe even if target aliases v.
; -------------------------------------------------------------------------
basix_parse_mvmul:
    push rbx
    push r13
    push r14

    call basix_resolve_vec_ident          ; target
    mov ebx, eax
    cmp ecx, 4
    jne .bail

    mov eax, TOK_COMMA
    call basix_expect
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r13d, eax                          ; matrix
    call basix_lex_next

    mov eax, TOK_COMMA
    call basix_expect
    call basix_resolve_vec_ident           ; v
    mov r14d, eax
    cmp ecx, 4
    jne .bail

    xor r10d, r10d                         ; output component i
.loop:
    cmp r10d, 4
    jge .copy_out

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_pxor_xmm0_xmm0
    call basix_emit_movsd_store_rax

    push r10
    xor r11d, r11d                         ; k
.kloop:
    cmp r11d, 4
    jge .kdone

    mov eax, r13d
    mov ecx, r10d
    imul ecx, ecx, 4
    add ecx, r11d                          ; m[i][k]
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, r14d
    mov ecx, r11d                          ; v[k]
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax_x1

    call basix_emit_mulsd
    call basix_emit_mov_xmm1_xmm0
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    call basix_emit_addsd
    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r11d
    jmp .kloop
.kdone:
    pop r10

    lea rax, [rel basix_vec_scalar_temp]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, r10d
    imul eax, eax, 8
    lea rax, [rel basix_vec4_scratch + rax]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .loop

.copy_out:
    xor r10d, r10d
.copyloop:
    cmp r10d, 4
    jge .out
    mov eax, r10d
    imul eax, eax, 8
    lea rax, [rel basix_vec4_scratch + rax]
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax

    mov eax, ebx
    mov ecx, r10d
    call basix_wide_component_addr
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_store_rax

    inc r10d
    jmp .copyloop
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r14
    pop r13
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_int_arg_push: parses one arithmetic expression, converting
; a float result to a truncated int (CVTTSD2SI) so PSET/LINE's pixel-
; coordinate and color arguments are always plain integers by the time
; they're pushed, then emits code pushing that int result. Shared by
; basix_parse_pset/basix_parse_line so their argument lists don't repeat
; this "parse, convert-if-float, push" sequence at every comma.
; -------------------------------------------------------------------------
basix_parse_int_arg_push:
    call basix_parse_arith               ; r11d = expr type (0=int)
    test r11d, r11d
    jz .is_int
    call basix_emit_cvttsd2si_rax_x0
.is_int:
    jmp basix_emit_push_rax              ; tail call -- returns to our
                                          ; own caller directly

; -------------------------------------------------------------------------
; basix_parse_pset: "PSET x, y, color" -- sets one framebuffer pixel.
; Arguments are pushed left to right, then popped in reverse into the
; runtime call's registers (RCX=x, RDX=y, R8=color) so the last-pushed
; value (color) comes off the stack first.
; -------------------------------------------------------------------------
basix_parse_pset:
    call basix_parse_int_arg_push        ; x
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; y
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; color

    call basix_emit_pop_r8               ; color
    call basix_emit_pop_rdx              ; y
    call basix_emit_pop_rcx              ; x
    lea rcx, [rel basix_rt_pset]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_putchar: "PUTCHAR expr" -- prints one raw character (not
; its decimal digits) via basix_rt_print_char, for rendering a text
; buffer's stored char codes.
; -------------------------------------------------------------------------
basix_parse_putchar:
    call basix_parse_int_arg_push        ; char code
    call basix_emit_pop_rcx
    lea rcx, [rel basix_rt_print_char]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_color: "COLOR expr" -- sets the text console's current
; glyph foreground color (as a 0x00RRGGBB packed value) via
; basix_rt_set_text_color; applies to every PUTCHAR/PRINT call from here
; on, until the next COLOR.
; -------------------------------------------------------------------------
basix_parse_color:
    call basix_parse_int_arg_push        ; 0x00RRGGBB color
    call basix_emit_pop_rcx
    lea rcx, [rel basix_rt_set_text_color]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_locate: "LOCATE row, col" -- positions the text console
; cursor (console_row/console_col), so a program can redraw a specific
; screen line instead of only ever continuing linearly like PRINT.
; -------------------------------------------------------------------------
basix_parse_locate:
    call basix_parse_int_arg_push        ; row
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; col

    call basix_emit_pop_rdx              ; col
    call basix_emit_pop_rcx              ; row
    lea rcx, [rel basix_rt_locate]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_poke: "POKE addr, value" -- writes one byte (value & 0xFF)
; to raw memory at addr. Same comma-separated, no-parens statement shape
; as LOCATE/PSET.
; -------------------------------------------------------------------------
basix_parse_poke:
    call basix_parse_int_arg_push        ; addr
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; value

    call basix_emit_pop_rdx              ; value
    call basix_emit_pop_rcx              ; addr
    lea rcx, [rel basix_rt_poke]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_drawpng: "DRAWPNG x, y" -- blits the most recently LOADPNG'd
; image into the graphics back buffer at (x, y), clipped to the back
; buffer's own bounds (see basix_rt_drawpng). Same "comma-separated,
; no parens" statement shape as PSET/LOCATE.
; -------------------------------------------------------------------------
basix_parse_drawpng:
    call basix_parse_int_arg_push        ; x
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; y

    call basix_emit_pop_rdx              ; y
    call basix_emit_pop_rcx              ; x
    lea rcx, [rel basix_rt_drawpng]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_float_arg_store: RAX = a fixed (compile-time-constant)
; scratch address. Parses one arithmetic expression, coercing an int
; result to float, then emits code storing that float (XMM0) to the
; given address. Unlike basix_parse_int_arg_push, the value isn't
; pushed to the compiled program's runtime stack -- it's written
; straight to a named global -- since LINE's runtime routine
; (basix_rt_line_aa) needs sub-pixel-precision float endpoints for
; anti-aliasing, and there are more of them (four) than there are
; argument registers worth plumbing through push/pop.
;
; The scratch address is carried across the basix_parse_arith call on
; OUR OWN native return stack (push/pop), not in a register -- parsing
; the expression can recurse arbitrarily (nested calls, array index
; expressions, etc.) and may clobber any register, but a value pushed
; here is safe regardless of what runs in between.
; -------------------------------------------------------------------------
basix_parse_float_arg_store:
    push rax
    call basix_parse_arith                ; r11d = expr type (0=int)
    test r11d, r11d
    jnz .already_float
    call basix_emit_cvtsi2sd_x0_rax
.already_float:
    pop rax
    call basix_emit_mov_rax_imm64
    jmp basix_emit_movsd_store_rax        ; tail call -- returns to our
                                           ; own caller directly

; -------------------------------------------------------------------------
; basix_parse_int_arg_store: RAX = a fixed (compile-time-constant)
; scratch address. Parses one arithmetic expression, coercing a float
; result to a truncated int, then emits code storing that int to the
; given address. The int sibling of basix_parse_float_arg_store, for
; statements like RECT/DRAWTEXT that take more plain int arguments than
; fit in the four argument registers.
;
; basix_parse_arith leaves an int result in RAX itself (unlike float's
; XMM0), so unlike the float version this can't just overwrite RAX with
; the destination address once parsing is done -- the parsed value has
; to move out of RAX (into RCX, via the already-emitted mov_rcx_rax
; pattern) before RAX gets reused for the address.
; -------------------------------------------------------------------------
basix_parse_int_arg_store:
    push rax
    call basix_parse_arith                ; r11d = expr type (0=int)
    test r11d, r11d
    jz .already_int
    call basix_emit_cvttsd2si_rax_x0
.already_int:
    call basix_emit_mov_rcx_rax           ; emit: rcx := the parsed value
    pop rax
    call basix_emit_mov_rax_imm64         ; emit: rax := scratch address
    jmp basix_emit_mov_memrax_rcx         ; emit: [rax] := rcx -- tail call

; -------------------------------------------------------------------------
; basix_parse_line: "LINE x1, y1, x2, y2, color" -- draws an
; anti-aliased line (Wu's algorithm, basix_rt_line_aa) into the back
; buffer. Endpoints keep full float precision (stored to fixed globals
; basix_line_x1f/y1f/x2f/y2f -- see basix_parse_float_arg_store) rather
; than truncating to whole pixels the way PSET's integer coordinates
; do; only the trailing color argument is a plain int, passed in RCX
; like PSET/CLS.
; -------------------------------------------------------------------------
basix_parse_line:
    lea rax, [rel basix_line_x1f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_line_y1f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_line_x2f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_line_y2f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect

    call basix_parse_int_arg_push        ; color
    call basix_emit_pop_rcx              ; color
    lea rcx, [rel basix_rt_line_aa]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_trifill: "TRIFILL x0,y0, x1,y1, x2,y2, color" -- fills one
; solid-color triangle into the back buffer (basix_rt_fill_tri). Same
; float-endpoint marshaling as LINE (see basix_parse_float_arg_store),
; just three points instead of two.
; -------------------------------------------------------------------------
basix_parse_trifill:
    lea rax, [rel basix_tri_x0f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_tri_y0f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_tri_x1f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_tri_y1f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_tri_x2f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_tri_y2f]
    call basix_parse_float_arg_store
    mov eax, TOK_COMMA
    call basix_expect

    call basix_parse_int_arg_push        ; color
    call basix_emit_pop_rcx              ; color
    lea rcx, [rel basix_rt_fill_tri]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_rect: "RECT x, y, w, h, color" -- fills one solid-color
; rectangle into the back buffer (basix_rt_rect). Five int arguments,
; one more than fit in the argument registers alongside color, so x/y/
; w/h are stashed to fixed globals (basix_parse_int_arg_store) the
; same way LINE/TRIFILL stash their float endpoints; only the trailing
; color argument goes through a register.
; -------------------------------------------------------------------------
basix_parse_rect:
    lea rax, [rel basix_rect_x]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_rect_y]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_rect_w]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_rect_h]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect

    call basix_parse_int_arg_push        ; color
    call basix_emit_pop_rcx              ; color
    lea rcx, [rel basix_rt_rect]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_drawtext: "DRAWTEXT x, y, "string", color" -- draws a
; pixel-positioned text label into the back buffer (basix_rt_drawtext),
; unlike PRINT/PUTCHAR's character-cell/LOCATE-based console text. x/y
; are stashed to fixed globals like RECT's coordinates; the string
; literal is interned into the permanent pool exactly like PRINT's
; string items (see basix_parse_print_list), and its ptr/length plus
; the trailing color are passed through RCX/RDX/R8.
; -------------------------------------------------------------------------
basix_parse_drawtext:
    push r12
    push r13

    lea rax, [rel basix_drawtext_x]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_drawtext_y]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect

    cmp dword [rel basix_tok_type], TOK_STRING
    jne .bad
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_strlit_intern             ; rax=permanent ptr, ecx=length --
                                          ; both compile-time constants,
                                          ; carried across the further
                                          ; parsing below the same way
                                          ; basix_parse_print_list does
    mov r12, rax
    mov r13d, ecx
    call basix_lex_next
    mov eax, TOK_COMMA
    call basix_expect

    call basix_parse_int_arg_push        ; color
    call basix_emit_pop_r8               ; color

    mov rax, r12
    call basix_emit_mov_rcx_imm64        ; emit: rcx := string ptr
    mov eax, r13d
    call basix_emit_mov_rdx_imm64        ; emit: rdx := string length
    lea rcx, [rel basix_rt_drawtext]
    call basix_emit_call_to

    pop r13
    pop r12
    ret

.bad:
    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    pop r13
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_drawchar: "DRAWCHAR x, y, charcode, color" -- the single-
; glyph sibling of DRAWTEXT (basix_rt_drawchar), for text built up from
; runtime char codes (DIRNAMECHAR, ARGCHAR) rather than a compile-time
; string literal. x/y stash to the same basix_drawtext_x/y globals
; DRAWTEXT uses; charcode/color are plain int args, fitting in
; RCX/RDX with no register pressure.
; -------------------------------------------------------------------------
basix_parse_drawchar:
    lea rax, [rel basix_drawtext_x]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect
    lea rax, [rel basix_drawtext_y]
    call basix_parse_int_arg_store
    mov eax, TOK_COMMA
    call basix_expect

    call basix_parse_int_arg_push        ; charcode
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; color

    call basix_emit_pop_rdx              ; color
    call basix_emit_pop_rcx              ; charcode
    lea rcx, [rel basix_rt_drawchar]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_launch: "LAUNCH namearr, namelen, argarr, arglen" -- loads
; and jumps into another program the same way RUN does (deep paths and
; precompiled .axb included; see basix_rt_launch/basix_load_program),
; after stashing argarr/arglen where the launched program's ARGLEN/
; ARGCHAR can read it. namearr/argarr are bare int arrays of char codes
; (BASIX64 has no string type) -- same "bare array decays to its base
; pointer" argument shape as FSAVE/LOADPNG, so no special-casing is
; needed here beyond the ordinary four-int-argument plumbing.
; -------------------------------------------------------------------------
basix_parse_launch:
    call basix_parse_int_arg_push        ; namearr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; namelen
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; argarr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; arglen

    call basix_emit_pop_r9               ; arglen
    call basix_emit_pop_r8               ; argarr
    call basix_emit_pop_rdx              ; namelen
    call basix_emit_pop_rcx              ; namearr
    lea rcx, [rel basix_rt_launch]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_dircd: "DIRCD namearr, namelen" -- descends into a
; subdirectory of the current one (basix_rt_dircd/exfat_cd_into). Same
; bare-int-array shape as LAUNCH's namearr, just two args instead of
; four.
; -------------------------------------------------------------------------
basix_parse_dircd:
    call basix_parse_int_arg_push        ; namearr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; namelen

    call basix_emit_pop_rdx              ; namelen
    call basix_emit_pop_rcx              ; namearr
    lea rcx, [rel basix_rt_dircd]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_cls: "CLS color" -- fills the entire back buffer with one
; color. Single int argument, same plumbing as WAIT's.
; -------------------------------------------------------------------------
basix_parse_cls:
    call basix_parse_int_arg_push        ; color
    call basix_emit_pop_rcx              ; color
    lea rcx, [rel basix_rt_cls]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_flip: "FLIP" -- no arguments. Blits the back buffer
; (everything PSET/LINE/CLS have drawn since the last FLIP) onto the
; real, visible framebuffer in one shot, so a whole frame appears at
; once instead of being built up stroke-by-stroke on screen.
; -------------------------------------------------------------------------
basix_parse_flip:
    lea rcx, [rel basix_rt_flip]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_wait: "WAIT n" -- spins until n more PIT ticks (hundredths
; of a second, same units basix_rt_timer/TIMER reads) have elapsed.
; Single argument, same "parse, convert-if-float, push/pop" plumbing as
; PSET/LINE above.
; -------------------------------------------------------------------------
basix_parse_wait:
    call basix_parse_int_arg_push        ; ticks
    call basix_emit_pop_rcx              ; ticks
    lea rcx, [rel basix_rt_wait]
    call basix_emit_call_to
    ret

; -------------------------------------------------------------------------
; basix_parse_trig_arg: shared by SIN/COS/TAN/ASIN/ACOS/ATAN -- parses
; "(expr)" (the function keyword itself already consumed by the
; caller), coercing an int result to float (XMM0) since every one of
; these runtime routines expects its argument there, matching the
; convention basix_emit_addsd/mulsd/etc. already use throughout this
; file ("everything stays in XMM0", basix_codegen.inc's float-op
; block comment).
; -------------------------------------------------------------------------
basix_parse_trig_arg:
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_arith               ; r11d = expr type
    test r11d, r11d
    jnz .already_float
    call basix_emit_cvtsi2sd_x0_rax
.already_float:
    mov eax, TOK_RPAREN
    call basix_expect
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
    cmp eax, TOK_SIN
    je .do_sin
    cmp eax, TOK_COS
    je .do_cos
    cmp eax, TOK_TAN
    je .do_tan
    cmp eax, TOK_ASIN
    je .do_asin
    cmp eax, TOK_ACOS
    je .do_acos
    cmp eax, TOK_ATAN
    je .do_atan
    cmp eax, TOK_TIMER
    je .do_timer
    cmp eax, TOK_SCREENW
    je .do_screenw
    cmp eax, TOK_SCREENH
    je .do_screenh
    cmp eax, TOK_GETKEY
    je .do_getkey
    cmp eax, TOK_FSAVE
    je .do_fsave
    cmp eax, TOK_FLOAD
    je .do_fload
    cmp eax, TOK_TEXTCOLS
    je .do_textcols
    cmp eax, TOK_TEXTROWS
    je .do_textrows
    cmp eax, TOK_LOADPNG
    je .do_loadpng
    cmp eax, TOK_PNGWIDTH
    je .do_pngwidth
    cmp eax, TOK_PNGHEIGHT
    je .do_pngheight
    cmp eax, TOK_PNGDEBUG
    je .do_pngdebug
    cmp eax, TOK_PNGDEBUG2
    je .do_pngdebug2
    cmp eax, TOK_PEEK
    je .do_peek
    cmp eax, TOK_MOUSEX
    je .do_mousex
    cmp eax, TOK_MOUSEY
    je .do_mousey
    cmp eax, TOK_MOUSEBTN
    je .do_mousebtn
    cmp eax, TOK_ARGLEN
    je .do_arglen
    cmp eax, TOK_ARGCHAR
    je .do_argchar
    cmp eax, TOK_KEYHIT
    je .do_keyhit
    cmp eax, TOK_DIRNEXT
    je .do_dirnext
    cmp eax, TOK_DIRISDIR
    je .do_dirisdir
    cmp eax, TOK_DIRNAMELEN
    je .do_dirnamelen
    cmp eax, TOK_DIRNAMECHAR
    je .do_dirnamechar
    cmp eax, TOK_WINCLOSE
    je .do_winclose
    cmp eax, TOK_WBBLUE
    je .do_wbblue
    cmp eax, TOK_WBGRAY
    je .do_wbgray
    cmp eax, TOK_WBLIGHT
    je .do_wblight
    cmp eax, TOK_WBDARK
    je .do_wbdark
    cmp eax, TOK_WBTITLEH
    je .do_wbtitleh
    cmp eax, TOK_WBCLOSESZ
    je .do_wbclosesz
    cmp eax, TOK_WINX
    je .do_winx
    cmp eax, TOK_WINY
    je .do_winy

    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    xor r11d, r11d
    ret

.do_sin:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_sin]
    call basix_emit_call_to
    mov r11d, 2
    ret
.do_cos:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_cos]
    call basix_emit_call_to
    mov r11d, 2
    ret
.do_tan:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_tan]
    call basix_emit_call_to
    mov r11d, 2
    ret
.do_asin:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_asin]
    call basix_emit_call_to
    mov r11d, 2
    ret
.do_acos:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_acos]
    call basix_emit_call_to
    mov r11d, 2
    ret
.do_atan:
    call basix_lex_next
    call basix_parse_trig_arg
    lea rcx, [rel basix_rt_atan]
    call basix_emit_call_to
    mov r11d, 2
    ret

.do_timer:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_timer]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_screenw:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_screenw]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_screenh:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_screenh]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_getkey:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_getkey]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_textcols:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_textcols]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_textrows:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_textrows]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_pngwidth:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_png_width]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_pngheight:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_png_height]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_pngdebug:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_png_debug]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_pngdebug2:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_png_debug2]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

; -------------------------------------------------------------------------
; PEEK(addr) -- reads one byte from raw memory at addr (any 64-bit
; address; caller's responsibility that it's mapped). One-arg,
; parenthesized function shape.
; -------------------------------------------------------------------------
.do_peek:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; addr
    mov eax, TOK_RPAREN
    call basix_expect
    call basix_emit_pop_rcx              ; addr
    lea rcx, [rel basix_rt_peek]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_mousex:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_mousex]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_mousey:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_mousey]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_mousebtn:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_mousebtn]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_winclose:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_winclose]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wbblue:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wbblue]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wbgray:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wbgray]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wblight:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wblight]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wbdark:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wbdark]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wbtitleh:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wbtitleh]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_wbclosesz:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_wbclosesz]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_winx:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_winx]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_winy:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_winy]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_arglen:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_arglen]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_argchar:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; index
    mov eax, TOK_RPAREN
    call basix_expect
    call basix_emit_pop_rcx              ; index
    lea rcx, [rel basix_rt_argchar]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_keyhit:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_keyhit]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_dirnext:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_dirnext]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_dirisdir:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_dirisdir]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_dirnamelen:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_dirnamelen]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret
.do_dirnamechar:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; index
    mov eax, TOK_RPAREN
    call basix_expect
    call basix_emit_pop_rcx              ; index
    lea rcx, [rel basix_rt_dirnamechar]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

; -------------------------------------------------------------------------
; LOADPNG(namearr, namelen) -- decodes a PNG file straight from exFAT into
; the kernel-owned pixel buffer DRAWPNG later blits from. Returns 1/0.
; Two plain int args, same shape as FSAVE/FLOAD.
; -------------------------------------------------------------------------
.do_loadpng:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; namearr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; namelen
    mov eax, TOK_RPAREN
    call basix_expect

    call basix_emit_pop_rdx              ; namelen
    call basix_emit_pop_rcx              ; namearr
    lea rcx, [rel basix_rt_loadpng]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

; -------------------------------------------------------------------------
; FSAVE(namearr, namelen, dataarr, datalen) / FLOAD(namearr, namelen,
; dataarr, maxlen) -- four plain int-valued arguments each. A bare array
; name used as an argument (not indexed) now evaluates to its compile-
; time-known base address as an int (see .ident_array's no-'['  path
; above), so basix_parse_int_arg_push handles namearr/dataarr exactly
; like any other int expression -- no special array-argument parsing
; needed beyond that.
; -------------------------------------------------------------------------
.do_fsave:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; namearr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; namelen
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; dataarr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; datalen
    mov eax, TOK_RPAREN
    call basix_expect

    call basix_emit_pop_r9               ; datalen
    call basix_emit_pop_r8               ; dataarr
    call basix_emit_pop_rdx              ; namelen
    call basix_emit_pop_rcx              ; namearr
    lea rcx, [rel basix_rt_fsave]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
    ret

.do_fload:
    call basix_lex_next
    mov eax, TOK_LPAREN
    call basix_expect
    call basix_parse_int_arg_push        ; namearr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; namelen
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; dataarr (base ptr)
    mov eax, TOK_COMMA
    call basix_expect
    call basix_parse_int_arg_push        ; maxlen
    mov eax, TOK_RPAREN
    call basix_expect

    call basix_emit_pop_r9               ; maxlen
    call basix_emit_pop_r8               ; dataarr
    call basix_emit_pop_rdx              ; namelen
    call basix_emit_pop_rcx              ; namearr
    lea rcx, [rel basix_rt_fload]
    call basix_emit_call_to
    xor r11d, r11d                       ; result type = int
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
    push r12
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r10d, eax
    mov r9d, [rel basix_var_type + r10*4]
    call basix_lex_next                   ; consume the identifier

    cmp r9d, BASIX_VARTYPE_ARRAY_INT
    je .ident_array
    cmp r9d, BASIX_VARTYPE_ARRAY_FLOAT
    je .ident_array

    cmp dword [rel basix_tok_type], TOK_LBRACKET
    jne .ident_no_index

    ; indexed access: v[N] -- N must be an integer literal (v1 scope;
    ; no runtime-computed indices). Real 1D arrays (BASIX_VARTYPE_
    ; ARRAY_INT/ARRAY_FLOAT) are handled separately below in
    ; .ident_array, which DOES support a runtime-computed index --
    ; this compile-time-constant-only path stays exactly as it was for
    ; VECTOR2/3/4/MATRIX4 component access, since there's no reason to
    ; add runtime-index overhead there.
    call basix_lex_next                   ; consume '['
    cmp dword [rel basix_tok_type], TOK_INT
    jne .index_bad
    mov r12, [rel basix_tok_ival]
    call basix_lex_next                   ; consume the index literal
    mov eax, TOK_RBRACKET
    call basix_expect

    mov eax, r10d
    call basix_var_wide_slot_addr
    imul r12, r12, 8
    add rax, r12
    call basix_emit_mov_rax_imm64
    call basix_emit_movsd_load_rax
    mov r11d, 2
    pop r12
    ret
.index_bad:
    mov dword [rel basix_compile_ok], 0
    mov r11d, 2
    pop r12
    ret

; -------------------------------------------------------------------------
; .ident_array: arr[i] or arr[i,j,...] read, each index a general
; runtime expression (not just a literal). N indices (N =
; basix_var_arr_ndims for this variable) are parsed left to right and
; combined via Horner's method into one linear offset --
; acc = acc*dims[k] + index_k for each dimension k after the first --
; entirely with EMITTED instructions (the compiled program's own
; RAX/RCX/RDX and its own runtime stack, not this compiler's), since
; only the per-dimension SIZES are known at compile time; the index
; VALUES are only known once the compiled program actually runs.
; ndims==1 (an ordinary 1D array) falls out of this as the trivial
; case: the loop body runs once, the "combine with the previous
; dimension" step never triggers, and the parsed index is used as-is.
;
; Only the FINAL combined offset is bounds-checked (against the
; array's total element count) before computing base+offset*8 and
; loading; an individual out-of-range index that still lands inside
; the flattened array by coincidence isn't caught -- consistent with
; every other silent-clip/best-effort check in this compiler (e.g.
; basix_rt_pset's off-screen coordinates), and catching it properly
; would need a bounds check (and a shared not-taken jump target) per
; dimension instead of one at the end; not worth the complexity here.
; -------------------------------------------------------------------------
.ident_array:
    push r13
    push r14
    push r15

    cmp dword [rel basix_tok_type], TOK_LBRACKET
    je .ident_array_indexed

    ; Bare array name used as an argument (not arr[i]): emit its
    ; compile-time-known base address as a plain int result, so it can
    ; be passed to FSAVE/FLOAD etc. exactly like any other int expr
    ; (see basix_parse_int_arg_push). DIM already wrote this array's
    ; base pointer into its slot earlier in this same compile pass.
    mov eax, r10d
    call basix_var_slot_addr
    mov rax, [rax]
    call basix_emit_mov_rax_imm64
    pop r15
    pop r14
    pop r13
    xor r11d, r11d
    pop r12
    ret

.ident_array_indexed:
    call basix_lex_next                   ; consume '['

    mov r14d, [rel basix_var_arr_ndims + r10*4]   ; how many indices expected
    xor r15d, r15d                                ; which index we're on (0-based)
.arr_idx_loop:
    push r9                               ; preserve this array's own
    push r10                              ; var_type/var_index (and our
    push r14                              ; own ndims/position) across
    push r15                              ; the index expression -- it
                                           ; may itself reference another
                                           ; variable, recursing back
                                           ; into .ident and clobbering
                                           ; these as THAT call's own
                                           ; scratch (real bug, caught
                                           ; via "nums[i]" printing 0.0
                                           ; instead of nums[i]'s value)
    call basix_parse_arith                ; index expr; r11d = type
    pop r15
    pop r14
    pop r10
    pop r9
    test r11d, r11d
    jz .arr_idx_int
    call basix_emit_cvttsd2si_rax_x0
.arr_idx_int:
    test r15d, r15d
    jz .arr_idx_first
    ; combine: acc (pushed at the end of the previous iteration) with
    ; this index, weighted by the size of THIS dimension.
    call basix_emit_push_rax              ; push this index
    call basix_emit_pop_rdx               ; rdx = this index
    call basix_emit_pop_rax               ; rax = running acc
    mov eax, r10d
    imul eax, BASIX_MAX_ARRAY_DIMS
    add eax, r15d
    mov eax, [rel basix_var_arr_dims + rax*4]   ; this dimension's size
    call basix_emit_mov_rcx_imm64
    call basix_emit_imul_rax_rcx          ; rax = acc * dim_size
    call basix_emit_add_rax_rdx           ; rax = acc*dim_size + index
.arr_idx_first:
    inc r15d
    cmp r15d, r14d
    jge .arr_idx_done
    mov eax, TOK_COMMA
    call basix_expect
    call basix_emit_push_rax              ; save running acc for the next index
    jmp .arr_idx_loop
.arr_idx_done:
    mov eax, TOK_RBRACKET
    call basix_expect
    ; rax (emitted/runtime) = final combined linear offset

    mov eax, [rel basix_var_arr_len + r10*4]
    call basix_emit_mov_rcx_imm64
    call basix_emit_cmp_rax_rcx
    mov al, 0x83                          ; JAE (unsigned >=)
    call basix_emit_jcc_placeholder
    mov r12, rax                          ; out-of-range patch address

    mov eax, 8
    call basix_emit_mov_rcx_imm64
    call basix_emit_imul_rax_rcx          ; rax = index*8
    mov eax, r10d
    call basix_var_slot_addr
    mov rax, [rax]                        ; this array's base address
                                           ; (a compile-time-known value:
                                           ; DIM already wrote it into
                                           ; the var's slot earlier in
                                           ; this same compile pass)
    call basix_emit_mov_rcx_imm64
    call basix_emit_add_rax_rcx           ; rax = base + index*8

    cmp r9d, BASIX_VARTYPE_ARRAY_INT
    je .arr_read_int_ok
    call basix_emit_movsd_load_rax
    jmp .arr_read_jmp_done
.arr_read_int_ok:
    call basix_emit_mov_rax_memrax
.arr_read_jmp_done:
    call basix_emit_jmp_placeholder
    mov r13, rax                          ; skip-the-zero-path patch addr

    call basix_emit_here                  ; out-of-range lands here
    mov rcx, r12
    mov rdx, rax
    call basix_patch_rel32

    cmp r9d, BASIX_VARTYPE_ARRAY_INT
    je .arr_read_zero_int
    call basix_emit_pxor_xmm0_xmm0
    jmp .arr_read_zero_done
.arr_read_zero_int:
    xor eax, eax
    call basix_emit_mov_rax_imm64
.arr_read_zero_done:

    call basix_emit_here
    mov rcx, r13
    mov rdx, rax
    call basix_patch_rel32

    pop r15
    pop r14
    pop r13
    cmp r9d, BASIX_VARTYPE_ARRAY_INT
    je .arr_read_result_int
    mov r11d, 2
    pop r12
    ret
.arr_read_result_int:
    xor r11d, r11d
    pop r12
    ret

.index_bad_arr:
    pop r15
    pop r14
    pop r13
    mov dword [rel basix_compile_ok], 0
    mov r11d, 2
    pop r12
    ret

.ident_no_index:
    mov eax, r10d
    call basix_var_slot_addr

    cmp r9d, 0
    jne .ident_float
    call basix_emit_mov_rax_moffs
    xor r11d, r11d
    pop r12
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
    mov r11d, 2
    pop r12
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
    push r10                             ; preserve left's type -- parsing
                                          ; the right operand may reference
                                          ; a variable, which recurses
                                          ; through basix_parse_primary's
                                          ; .ident and unconditionally
                                          ; clobbers r10d with that
                                          ; variable's own index as its own
                                          ; scratch (real bug caught here:
                                          ; "float / int-variable" silently
                                          ; became integer division
                                          ; whenever the variable happened
                                          ; to be the very first one
                                          ; declared, since combine_operands
                                          ; only checks r10d zero-vs-nonzero
                                          ; and a corrupted-to-zero value
                                          ; reads as "left is int")
    call basix_parse_unary               ; r11d = right's type
    pop r10
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
    call basix_emit_fmod
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
    push r10                             ; preserve left's type across the
                                          ; right operand's own parse --
                                          ; see the matching comment in
                                          ; basix_parse_term
    call basix_parse_term                ; r11d = right's type
    pop r10
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
; basix_parse_comparison: parses "arith relop arith", or a bare arith
; expression used as a boolean (nonzero/true). Leaves a plain 0/1 in RAX
; (via SETcc + MOVZX, not a jump -- AND/OR/NOT compose on top of this
; with ordinary integer bitwise ops, so there's no short-circuit control
; flow to thread through here). R11D is always 0 on return (booleans are
; plain ints).
; -------------------------------------------------------------------------
basix_parse_comparison:
    push r10
    push r12
    call basix_parse_arith
    mov r10d, r11d

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
    jmp .bare_value              ; no relop -- the value itself is the boolean

.relop_ok:
    call basix_emit_push_operand
    call basix_lex_next
    push r10                             ; preserve left's type across the
                                          ; right operand's own parse --
                                          ; see the matching comment in
                                          ; basix_parse_term
    call basix_parse_arith               ; r11d = right's type
    pop r10
    call basix_combine_operands          ; eax = combined type

    cmp eax, 0
    jne .float_cmp
    call basix_emit_cmp_rax_rcx
    jmp .pick_setcc_int
.float_cmp:
    call basix_emit_comisd
    cmp r12d, TOK_EQ
    je .fs_eq
    cmp r12d, TOK_NE
    je .fs_ne
    cmp r12d, TOK_LT
    je .fs_lt
    cmp r12d, TOK_GT
    je .fs_gt
    cmp r12d, TOK_LE
    je .fs_le
    call basix_emit_setae                ; TOK_GE
    jmp .finish_set
.fs_eq:
    call basix_emit_sete
    jmp .finish_set
.fs_ne:
    call basix_emit_setne
    jmp .finish_set
.fs_lt:
    call basix_emit_setb
    jmp .finish_set
.fs_gt:
    call basix_emit_seta
    jmp .finish_set
.fs_le:
    call basix_emit_setbe
    jmp .finish_set

.pick_setcc_int:
    cmp r12d, TOK_EQ
    je .is_eq
    cmp r12d, TOK_NE
    je .is_ne
    cmp r12d, TOK_LT
    je .is_lt
    cmp r12d, TOK_GT
    je .is_gt
    cmp r12d, TOK_LE
    je .is_le
    call basix_emit_setge                ; TOK_GE
    jmp .finish_set
.is_eq:
    call basix_emit_sete
    jmp .finish_set
.is_ne:
    call basix_emit_setne
    jmp .finish_set
.is_lt:
    call basix_emit_setl
    jmp .finish_set
.is_gt:
    call basix_emit_setg
    jmp .finish_set
.is_le:
    call basix_emit_setle

.finish_set:
    call basix_emit_movzx_eax_al
    xor r11d, r11d
    pop r12
    pop r10
    ret

.bare_value:
    cmp r10d, 0
    jne .bare_float
    call basix_emit_test_rax_rax
    call basix_emit_setne
    jmp .bare_finish
.bare_float:
    call basix_emit_pxor_xmm1_xmm1
    call basix_emit_comisd
    call basix_emit_setne
.bare_finish:
    call basix_emit_movzx_eax_al
    xor r11d, r11d
    pop r12
    pop r10
    ret

; -------------------------------------------------------------------------
; basix_parse_bool_unary: 'NOT' bool_unary | '(' condition ')' | comparison
;
; A leading '(' here is always treated as wrapping a full boolean
; sub-expression (so "NOT (a < b)" and "(a < b) AND (c < d)" work). The
; one known gap: parenthesizing just *one side* of a comparison at the
; outermost condition position -- e.g. "(x+1) = 5" -- doesn't work,
; since the paren content gets evaluated and materialized as a boolean
; (nonzero-is-true) before the trailing "= 5" is ever seen; resolving
; that in general needs backtracking this parser doesn't have. Wrap the
; *whole* comparison instead: "((x+1) = 5)" works fine, since by the
; time the outer '(' is seen here, the inner one is handled by
; basix_parse_primary's ordinary arithmetic-paren case instead.
; -------------------------------------------------------------------------
basix_parse_bool_unary:
    cmp dword [rel basix_tok_type], TOK_NOT
    jne .check_paren
    call basix_lex_next
    call basix_parse_bool_unary
    jmp basix_emit_xor_eax_1
.check_paren:
    cmp dword [rel basix_tok_type], TOK_LPAREN
    jne basix_parse_comparison
    call basix_lex_next
    call basix_parse_bool_or
    mov eax, TOK_RPAREN
    call basix_expect
    ret

; -------------------------------------------------------------------------
; basix_parse_bool_and: bool_unary ('AND' bool_unary)*
; -------------------------------------------------------------------------
basix_parse_bool_and:
    call basix_parse_bool_unary
.loop:
    cmp dword [rel basix_tok_type], TOK_AND
    jne .done
    call basix_emit_push_rax
    call basix_lex_next
    call basix_parse_bool_unary
    call basix_emit_mov_rcx_rax
    call basix_emit_pop_rax
    call basix_emit_and_rax_rcx
    jmp .loop
.done:
    ret

; -------------------------------------------------------------------------
; basix_parse_bool_or: bool_and ('OR' bool_and)*
; -------------------------------------------------------------------------
basix_parse_bool_or:
    call basix_parse_bool_and
.loop:
    cmp dword [rel basix_tok_type], TOK_OR
    jne .done
    call basix_emit_push_rax
    call basix_lex_next
    call basix_parse_bool_and
    call basix_emit_mov_rcx_rax
    call basix_emit_pop_rax
    call basix_emit_or_rax_rcx
    jmp .loop
.done:
    ret

; -------------------------------------------------------------------------
; basix_parse_condition: parses a full boolean expression (comparisons
; combined with AND/OR/NOT) and emits a single conditional jump, taken
; when the condition is FALSE. Returns RAX = absolute address of the
; jump's rel32 field, to be patched later with the false-branch target
; via basix_patch_rel32.
; -------------------------------------------------------------------------
basix_parse_condition:
    call basix_parse_bool_or             ; leaves a final 0/1 in RAX
    call basix_emit_test_rax_rax
    mov al, 0x84                         ; JE -- jump when RAX==0 (false)
    jmp basix_emit_jcc_placeholder

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
    push r12
    push r13

    mov rbx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    push rdx
    mov rcx, rbx
    call basix_var_find_or_create
    pop rdx
    push rax

    mov r13d, eax                        ; r13d = var index (also needed
                                          ; by the array-write path)
    mov r12d, [rel basix_var_type + r13*4]

    call basix_lex_next

    cmp r12d, BASIX_VARTYPE_ARRAY_INT
    je .assign_array
    cmp r12d, BASIX_VARTYPE_ARRAY_FLOAT
    je .assign_array

    cmp dword [rel basix_tok_type], TOK_LBRACKET
    jne .no_index

    call basix_lex_next                 ; consume '['
    cmp dword [rel basix_tok_type], TOK_INT
    jne .index_bad
    mov r12, [rel basix_tok_ival]
    call basix_lex_next
    mov eax, TOK_RBRACKET
    call basix_expect
    jmp .have_index
.index_bad:
    mov dword [rel basix_compile_ok], 0
    xor r12, r12
.have_index:
    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith              ; r11d = expr type
    pop rax
    call basix_emit_store_var_indexed
    jmp .done

.no_index:
    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith              ; r11d = expr type
    pop rax
    call basix_emit_store_var
    jmp .done

; -------------------------------------------------------------------------
; .assign_array: arr[i] = expr, i a general runtime expression. The
; index is parsed (and pushed, via EMITTED code -- this is the
; compiled program's own runtime stack, not this parser's) before the
; value expression, since parsing the value clobbers RAX/XMM0 exactly
; like parsing the index did; popping the index back afterward is
; what lets both survive to the final address computation.
;
; INT and FLOAT arrays take genuinely different register shapes: a
; float value lives in XMM0, never contending with the RAX/RCX the
; index+address math uses, so it needs no extra care. An int value
; DOES live in a GP register, so the index+address math is done
; through RAX/RDX instead of the usual RAX/RCX specifically to leave
; RCX free to hold the value untouched across it (see
; basix_i_imul_rax_rdx/add_rax_rdx/cmp_rax_rdx in basix_codegen.inc).
; -------------------------------------------------------------------------
.assign_array:
    pop rax                              ; drop the pushed var index --
                                          ; already have it in r13d
    cmp dword [rel basix_tok_type], TOK_LBRACKET
    jne .assign_array_bad
    call basix_lex_next                  ; consume '['

    push r14
    push r15
    mov r14d, [rel basix_var_arr_ndims + r13*4]   ; how many indices expected
    xor r15d, r15d                                ; which index we're on (0-based)
.aidx_loop:
    push r12                             ; preserve array kind/var index
    push r13                             ; (and our own ndims/position)
    push r14                             ; across the index expression --
    push r15                             ; it may itself reference another
                                          ; array's element, recursing back
                                          ; in here and clobbering these as
                                          ; THAT call's own scratch (same
                                          ; hazard the read path's .ident
                                          ; recursion comment describes)
    call basix_parse_arith               ; index expr; r11d = type
    pop r15
    pop r14
    pop r13
    pop r12
    test r11d, r11d
    jz .aidx_int
    call basix_emit_cvttsd2si_rax_x0
.aidx_int:
    test r15d, r15d
    jz .aidx_first
    call basix_emit_push_rax             ; push this index
    call basix_emit_pop_rdx              ; rdx = this index
    call basix_emit_pop_rax              ; rax = running acc
    mov eax, r13d
    imul eax, BASIX_MAX_ARRAY_DIMS
    add eax, r15d
    mov eax, [rel basix_var_arr_dims + rax*4]     ; this dimension's size
    call basix_emit_mov_rcx_imm64
    call basix_emit_imul_rax_rcx         ; rax = acc * dim_size
    call basix_emit_add_rax_rdx          ; rax = acc*dim_size + index
.aidx_first:
    inc r15d
    cmp r15d, r14d
    jge .aidx_done
    mov eax, TOK_COMMA
    call basix_expect
    call basix_emit_push_rax             ; save running acc for the next index
    jmp .aidx_loop
.aidx_done:
    pop r15
    pop r14
    mov eax, TOK_RBRACKET
    call basix_expect
    call basix_emit_push_rax             ; save final combined offset (emitted code)

    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith               ; value expr; r11d = type

    cmp r12d, BASIX_VARTYPE_ARRAY_INT
    je .aval_int

    ; FLOAT array: coerce the value to float (XMM0), then recover the
    ; index into RAX -- XMM0 survives basix_emit_pop_rax untouched.
    test r11d, r11d
    jnz .aval_float_ready
    call basix_emit_cvtsi2sd_x0_rax
.aval_float_ready:
    call basix_emit_pop_rax              ; rax = index, xmm0 = value

    mov eax, [rel basix_var_arr_len + r13*4]
    call basix_emit_mov_rcx_imm64
    call basix_emit_cmp_rax_rcx
    mov al, 0x83                          ; JAE
    call basix_emit_jcc_placeholder
    push rax                              ; skip-store patch addr

    mov eax, 8
    call basix_emit_mov_rcx_imm64
    call basix_emit_imul_rax_rcx
    mov eax, r13d
    call basix_var_slot_addr
    mov rax, [rax]
    call basix_emit_mov_rcx_imm64
    call basix_emit_add_rax_rcx
    call basix_emit_movsd_store_rax

    call basix_emit_here
    pop rcx
    mov rdx, rax
    call basix_patch_rel32
    jmp .done

.aval_int:
    ; INT array: coerce the value to int (RAX), then push it (so both
    ; index and value ride the emitted stack) and pop them back out in
    ; RCX (value) / RAX (index) -- freeing RAX for the address math
    ; while RCX holds the value untouched throughout.
    test r11d, r11d
    jz .aval_int_ready
    call basix_emit_cvttsd2si_rax_x0
.aval_int_ready:
    call basix_emit_push_rax             ; save value
    call basix_emit_pop_rcx              ; rcx = value
    call basix_emit_pop_rax              ; rax = index

    mov eax, [rel basix_var_arr_len + r13*4]
    call basix_emit_mov_rdx_imm64
    call basix_emit_cmp_rax_rdx
    mov al, 0x83                          ; JAE
    call basix_emit_jcc_placeholder
    push rax                              ; skip-store patch addr

    mov eax, 8
    call basix_emit_mov_rdx_imm64
    call basix_emit_imul_rax_rdx
    mov eax, r13d
    call basix_var_slot_addr
    mov rax, [rax]
    call basix_emit_mov_rdx_imm64
    call basix_emit_add_rax_rdx
    call basix_emit_mov_memrax_rcx

    call basix_emit_here
    pop rcx
    mov rdx, rax
    call basix_patch_rel32
    jmp .done

.assign_array_bad:
    mov dword [rel basix_compile_ok], 0

.done:
    pop r13
    pop r12
    pop rdx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_parse_if: IF keyword already consumed. Supports both forms:
;   block form:       IF cond THEN NEWLINE block (ELSE NEWLINE block)? ENDIF
;   single-line form:  IF cond THEN stmt (ELSE stmt)?
; distinguished by whether a NEWLINE immediately follows THEN.
; -------------------------------------------------------------------------
basix_parse_if:
    push r12
    call basix_parse_condition
    mov r12, rax

    mov eax, TOK_THEN
    call basix_expect

    cmp dword [rel basix_tok_type], TOK_NEWLINE
    jne .single_line

    call basix_lex_next                 ; consume NEWLINE

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

.single_line:
    call basix_parse_statement
    cmp dword [rel basix_tok_type], TOK_ELSE
    jne .single_no_else

    call basix_emit_jmp_placeholder
    push rax

    call basix_emit_here
    mov rcx, r12
    mov rdx, rax
    call basix_patch_rel32

    call basix_lex_next                 ; consume ELSE
    call basix_parse_statement

    pop rcx
    call basix_emit_here
    mov rdx, rax
    call basix_patch_rel32
    jmp .single_out

.single_no_else:
    call basix_emit_here
    mov rcx, r12
    mov rdx, rax
    call basix_patch_rel32

.single_out:
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
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_strlit_intern            ; rax=permanent ptr, ecx=length --
                                         ; basix_tok_text points into the
                                         ; lexer's shared, reused scratch
                                         ; buffer (valid only during
                                         ; compilation, see the comment on
                                         ; basix_tok_text_buf), but this
                                         ; pointer gets baked into the
                                         ; compiled program as an immediate
                                         ; for basix_rt_print_str to read
                                         ; at RUNTIME -- long after later
                                         ; tokens have overwritten that
                                         ; scratch buffer's contents. A
                                         ; permanent copy is required.
    mov r12, rax
    mov r13d, ecx
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
    push r12
    push r13

    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bail

    ; Copy the identifier's name out immediately (basix_save_ident) --
    ; NOT the lexer's shared basix_tok_text_buf, which the "AS"
    ; keyword and the type keyword a few lines below both also lex
    ; (through the very same identifier/keyword scan path) before this
    ; name is finally used, silently overwriting it if we only held
    ; onto the pointer. Caught via a large streamed program with many
    ; DIM statements compiling "successfully" but rendering wrong --
    ; every declared name had been quietly clobbered by whatever type
    ; keyword followed it.
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_save_ident
    push qword [rel basix_saved_ident_len]
    call basix_lex_next

    ; Optional "(d0[, d1[, d2[, d3]]])" -- an array declaration, one to
    ; BASIX_MAX_ARRAY_DIMS comma-separated compile-time-constant
    ; dimension sizes (row-major -- the last dimension is contiguous,
    ; same convention as C). r12d = TOTAL element count (product of
    ; every dimension), 0 meaning "not an array" (the ordinary
    ; scalar/vector/matrix path, unchanged from before arrays existed).
    ; The individual sizes are staged in basix_dim_scratch_dims/ndims
    ; until the variable's own index is known, a few lines further
    ; down past the "AS type" clause.
    xor r12d, r12d
    mov dword [rel basix_dim_scratch_ndims], 0
    cmp dword [rel basix_tok_type], TOK_LPAREN
    jne .no_arrsize
    call basix_lex_next                  ; consume '('
    mov r12, 1                           ; running product of dims seen so far
.dimloop:
    cmp dword [rel basix_tok_type], TOK_INT
    jne .bail_after_push
    mov rax, [rel basix_tok_ival]
    cmp rax, 1
    jl .bail_after_push                  ; zero/negative-sized dimension
    mov ecx, [rel basix_dim_scratch_ndims]
    cmp ecx, BASIX_MAX_ARRAY_DIMS
    jge .bail_after_push
    mov [rel basix_dim_scratch_dims + rcx*4], eax
    inc dword [rel basix_dim_scratch_ndims]
    imul r12, rax
    call basix_lex_next                  ; consume the size literal
    cmp dword [rel basix_tok_type], TOK_COMMA
    jne .dims_done
    call basix_lex_next                  ; consume ','
    jmp .dimloop
.dims_done:
    mov eax, TOK_RPAREN
    call basix_expect
.no_arrsize:

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
    jne .check_vec2
    mov r8d, 2
    jmp .type_ok
.check_vec2:
    cmp dword [rel basix_tok_type], TOK_VECTOR2
    jne .check_vec3
    mov r8d, 3
    jmp .type_ok
.check_vec3:
    cmp dword [rel basix_tok_type], TOK_VECTOR3
    jne .check_vec4
    mov r8d, 4
    jmp .type_ok
.check_vec4:
    cmp dword [rel basix_tok_type], TOK_VECTOR4
    jne .check_mat4
    mov r8d, 5
    jmp .type_ok
.check_mat4:
    cmp dword [rel basix_tok_type], TOK_MATRIX4
    jne .bail_after_push
    mov r8d, 6
.type_ok:
    call basix_lex_next

    test r12d, r12d
    jz .not_array
    cmp r8d, 2
    ja .bail_after_push                  ; array of VECTOR*/MATRIX4: no
    mov r13d, BASIX_VARTYPE_ARRAY_FLOAT
    test r8d, r8d
    jnz .arr_kind_set
    mov r13d, BASIX_VARTYPE_ARRAY_INT
.arr_kind_set:
    mov r8d, r13d
.not_array:

    pop rdx
    lea rcx, [rel basix_saved_ident_buf]
    push r12
    push r8
    call basix_var_find_or_create
    pop r8
    pop r12
    mov [rel basix_var_type + rax*4], r8d

    test r12d, r12d
    jz .out                              ; scalar/vector/matrix: done

    ; Array: bump-allocate r12*8 bytes from the array arena, record the
    ; base address in this variable's (otherwise-unused-by-arrays) 8-
    ; byte scalar slot and the element count in basix_var_arr_len.
    mov r13d, eax                        ; r13d = this variable's index
    mov ecx, r12d
    shl ecx, 3                           ; bytes needed = size * 8
    mov edx, [rel basix_array_arena_pos]
    add edx, ecx
    cmp edx, BASIX_SLOT_ARENA_SIZE
    ja .arena_overflow

    mov eax, [rel basix_array_arena_pos]
    mov rcx, [rel basix_array_arena_ptr]
    add rcx, rax                         ; rcx = this array's base address
    mov [rel basix_array_arena_pos], edx

    mov eax, r13d
    call basix_var_slot_addr
    mov [rax], rcx
    mov eax, r13d
    mov [rel basix_var_arr_len + rax*4], r12d

    ; Copy this array's per-dimension sizes out of DIM's scratch and
    ; into its own permanent slot (basix_var_arr_ndims/dims), indexed
    ; by variable so the read/write index paths can look them up by
    ; var index alone.
    mov ecx, [rel basix_dim_scratch_ndims]
    mov [rel basix_var_arr_ndims + r13*4], ecx
    xor edx, edx
.copy_dims_loop:
    cmp edx, ecx
    jge .copy_dims_done
    mov r9d, [rel basix_dim_scratch_dims + rdx*4]
    mov r10d, r13d
    imul r10d, BASIX_MAX_ARRAY_DIMS
    add r10d, edx
    mov [rel basix_var_arr_dims + r10*4], r9d
    inc edx
    jmp .copy_dims_loop
.copy_dims_done:
    jmp .out

.arena_overflow:
    mov dword [rel basix_compile_ok], 0
    jmp .out

.bail_after_push:
    pop rdx
.bail:
    mov dword [rel basix_compile_ok], 0
.out:
    pop r13
    pop r12
    ret

; -------------------------------------------------------------------------
; basix_parse_statement: parses exactly one statement (not its trailing
; NEWLINE).
; -------------------------------------------------------------------------
basix_parse_statement:
    push rbx
    push r12
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
    cmp r15d, TOK_VSET
    je .do_vset
    cmp r15d, TOK_VADD
    je .do_vadd
    cmp r15d, TOK_VSUB
    je .do_vsub
    cmp r15d, TOK_VSCALE
    je .do_vscale
    cmp r15d, TOK_VDOT
    je .do_vdot
    cmp r15d, TOK_VCROSS
    je .do_vcross
    cmp r15d, TOK_VLEN
    je .do_vlen
    cmp r15d, TOK_VNORM
    je .do_vnorm
    cmp r15d, TOK_MIDENT
    je .do_mident
    cmp r15d, TOK_MMUL
    je .do_mmul
    cmp r15d, TOK_MVMUL
    je .do_mvmul
    cmp r15d, TOK_PSET
    je .do_pset
    cmp r15d, TOK_LINE
    je .do_line
    cmp r15d, TOK_WAIT
    je .do_wait
    cmp r15d, TOK_CLS
    je .do_cls
    cmp r15d, TOK_FLIP
    je .do_flip
    cmp r15d, TOK_PNGLOCK
    je .do_pnglock
    cmp r15d, TOK_PNGUNLOCK
    je .do_pngunlock
    cmp r15d, TOK_TRIFILL
    je .do_trifill
    cmp r15d, TOK_PUTCHAR
    je .do_putchar
    cmp r15d, TOK_LOCATE
    je .do_locate
    cmp r15d, TOK_POKE
    je .do_poke
    cmp r15d, TOK_COLOR
    je .do_color
    cmp r15d, TOK_DRAWPNG
    je .do_drawpng
    cmp r15d, TOK_RECT
    je .do_rect
    cmp r15d, TOK_DRAWTEXT
    je .do_drawtext
    cmp r15d, TOK_LAUNCH
    je .do_launch
    cmp r15d, TOK_DIROPEN
    je .do_diropen
    cmp r15d, TOK_DIRCD
    je .do_dircd
    cmp r15d, TOK_DIRUP
    je .do_dirup
    cmp r15d, TOK_DRAWCHAR
    je .do_drawchar
    cmp r15d, TOK_IDENT
    je .do_ident_stmt

    mov dword [rel basix_compile_ok], 0
    call basix_lex_next
    jmp .out

.do_let:
    call basix_lex_next
    call basix_parse_assignment
    jmp .out

.do_vset:
    call basix_lex_next
    call basix_parse_vset
    jmp .out
.do_vadd:
    call basix_lex_next
    call basix_parse_vadd
    jmp .out
.do_vsub:
    call basix_lex_next
    call basix_parse_vsub
    jmp .out
.do_vscale:
    call basix_lex_next
    call basix_parse_vscale
    jmp .out
.do_vdot:
    call basix_lex_next
    call basix_parse_vdot
    jmp .out
.do_vcross:
    call basix_lex_next
    call basix_parse_vcross
    jmp .out
.do_vlen:
    call basix_lex_next
    call basix_parse_vlen
    jmp .out
.do_vnorm:
    call basix_lex_next
    call basix_parse_vnorm
    jmp .out
.do_mident:
    call basix_lex_next
    call basix_parse_mident
    jmp .out
.do_mmul:
    call basix_lex_next
    call basix_parse_mmul
    jmp .out
.do_mvmul:
    call basix_lex_next
    call basix_parse_mvmul
    jmp .out
.do_pset:
    call basix_lex_next
    call basix_parse_pset
    jmp .out
.do_line:
    call basix_lex_next
    call basix_parse_line
    jmp .out
.do_wait:
    call basix_lex_next
    call basix_parse_wait
    jmp .out
.do_cls:
    call basix_lex_next
    call basix_parse_cls
    jmp .out
.do_flip:
    call basix_lex_next
    call basix_parse_flip
    jmp .out
.do_pnglock:
    call basix_lex_next
    lea rcx, [rel basix_rt_pnglock]
    call basix_emit_call_to
    jmp .out
.do_pngunlock:
    call basix_lex_next
    lea rcx, [rel basix_rt_pngunlock]
    call basix_emit_call_to
    jmp .out
.do_trifill:
    call basix_lex_next
    call basix_parse_trifill
    jmp .out
.do_putchar:
    call basix_lex_next
    call basix_parse_putchar
    jmp .out
.do_locate:
    call basix_lex_next
    call basix_parse_locate
    jmp .out
.do_poke:
    call basix_lex_next
    call basix_parse_poke
    jmp .out
.do_color:
    call basix_lex_next
    call basix_parse_color
    jmp .out
.do_drawpng:
    call basix_lex_next
    call basix_parse_drawpng
    jmp .out

.do_rect:
    call basix_lex_next
    call basix_parse_rect
    jmp .out
.do_drawtext:
    call basix_lex_next
    call basix_parse_drawtext
    jmp .out
.do_launch:
    call basix_lex_next
    call basix_parse_launch
    jmp .out

.do_diropen:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_diropen]
    call basix_emit_call_to
    jmp .out
.do_dircd:
    call basix_lex_next
    call basix_parse_dircd
    jmp .out
.do_dirup:
    call basix_lex_next                  ; bare keyword, no parens/args
    lea rcx, [rel basix_rt_dirup]
    call basix_emit_call_to
    jmp .out
.do_drawchar:
    call basix_lex_next
    call basix_parse_drawchar
    jmp .out

.do_ident_stmt:
    mov rbx, [rel basix_tok_text]
    mov r13d, [rel basix_tok_len]
    call basix_lex_next
    cmp dword [rel basix_tok_type], TOK_COLON
    je .label_def

    push rbx
    mov rcx, rbx
    mov edx, r13d
    call basix_var_find_or_create
    pop rbx
    push rax

    cmp dword [rel basix_tok_type], TOK_LBRACKET
    jne .assign_no_index

    call basix_lex_next                 ; consume '['
    cmp dword [rel basix_tok_type], TOK_INT
    jne .assign_index_bad
    mov r12, [rel basix_tok_ival]
    call basix_lex_next
    mov eax, TOK_RBRACKET
    call basix_expect
    jmp .assign_have_index
.assign_index_bad:
    mov dword [rel basix_compile_ok], 0
    xor r12, r12
.assign_have_index:
    mov eax, TOK_EQ
    call basix_expect
    call basix_parse_arith              ; r11d = expr type
    pop rax
    call basix_emit_store_var_indexed
    jmp .out

.assign_no_index:
    mov eax, TOK_EQ
    call basix_expect
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
    call basix_save_ident               ; see basix_saved_ident_buf's
                                         ; comment -- the lookahead
                                         ; lex_next below could lex
                                         ; another identifier/keyword
                                         ; and clobber a merely-saved
                                         ; pointer
    call basix_lex_next
    lea rcx, [rel basix_saved_ident_buf]
    mov edx, [rel basix_saved_ident_len]
    xor r8d, r8d
    call basix_goto_or_gosub_emit
    jmp .out

.do_gosub:
    call basix_lex_next
    cmp dword [rel basix_tok_type], TOK_IDENT
    jne .bad_target
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_save_ident
    call basix_lex_next
    lea rcx, [rel basix_saved_ident_buf]
    mov edx, [rel basix_saved_ident_len]
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
    pop r12
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_strlit_intern: RCX = ptr to text, RDX = length. Copies into the
; permanent basix_strlit_pool at the current bump position and advances
; it, so the copy survives long after the lexer's own shared scratch
; buffer (basix_tok_text_buf) gets reused by later tokens -- unlike
; that buffer, this pool is never reused within one compile, so a
; pointer into it baked into the compiled program as an immediate stays
; valid at runtime. Clamped to the pool's remaining space; on overflow,
; fails closed (basix_compile_ok=0, returns a zero-length copy) rather
; than corrupt memory, matching this compiler's established convention
; for every other fixed-size resource (variables, labels, code buffer).
; Returns RAX = pool address of the copy, ECX = length actually copied.
; -------------------------------------------------------------------------
basix_strlit_intern:
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    mov r8, rcx                         ; r8 = source ptr
    mov r9d, edx                        ; r9d = requested length
    mov r10d, [rel basix_strlit_pool_pos] ; r10d = start of this copy

    mov eax, BASIX_SLOT_STRLIT_SIZE
    sub eax, r10d                       ; eax = remaining space
    cmp r9d, eax
    jbe .len_ok
    mov dword [rel basix_compile_ok], 0
    xor r9d, r9d
.len_ok:
    mov rdi, [rel basix_strlit_pool_ptr]
    add rdi, r10
    mov rsi, r8
    mov ecx, r9d
    cld
    rep movsb

    mov eax, r10d
    add eax, r9d
    mov [rel basix_strlit_pool_pos], eax

    mov rax, [rel basix_strlit_pool_ptr]
    add rax, r10                        ; rax = address of this copy
    mov ecx, r9d                        ; ecx = length copied

    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    ret

; -------------------------------------------------------------------------
; basix_heap_init: called once from kernel.asm's boot sequence, right
; after pmm_init, and before anything might compile/run a BASIX64
; program. Heap-allocates basix_file_scratch (via kmalloc_large)
; instead of embedding it as literal zero bytes in the kernel image
; (see basix_symbols.inc's comment on basix_file_scratch_ptr). The
; per-slot code/arena/strlit/var buffers (basix_array_arena_ptr's own
; backing memory included) are allocated separately by
; basix_slot_init_all, called right alongside this from kernel.asm.
; -------------------------------------------------------------------------
basix_heap_init:
    push rcx
    mov rcx, BASIX_FILE_SCRATCH_SIZE
    call kmalloc_large
    mov [rel basix_file_scratch_ptr], rax

    call png_heap_init
    pop rcx
    ret

; -------------------------------------------------------------------------
; basix_runtime_reset_state: clears every piece of per-run state that
; isn't the emitted code itself or the ok flag -- variables, labels,
; fixups, FOR nesting, the array arena, the string-literal pool. Split
; out of basix_compile_reset_state so RUN's AXB fast-load path (which
; supplies basix_code_buf/basix_code_pos itself, by copying a
; precompiled binary straight off disk instead of running the
; lexer/parser/codegen) can still get a clean runtime without re-doing
; compile-only bookkeeping. Touches no registers beyond EAX-free
; `mov [mem], imm` forms.
; -------------------------------------------------------------------------
basix_runtime_reset_state:
    mov dword [rel basix_var_count], 0
    mov dword [rel basix_label_count], 0
    mov dword [rel basix_fixup_count], 0
    mov dword [rel basix_for_depth], 0
    mov dword [rel basix_array_arena_pos], 0
    mov dword [rel basix_strlit_pool_pos], 0
    ret

; -------------------------------------------------------------------------
; basix_compile_reset_state: shared by basix_compile/basix_compile_file
; -- on top of basix_runtime_reset_state, also resets the emitted-code
; position and the overall ok flag before a fresh compile begins. Safe
; to call before either entry point has finished unpacking its own
; incoming arguments out of RCX/RDX/R8.
; -------------------------------------------------------------------------
basix_compile_reset_state:
    mov dword [rel basix_compile_ok], 1
    mov dword [rel basix_code_pos], 0
    jmp basix_runtime_reset_state

; -------------------------------------------------------------------------
; basix_compile: RCX = ptr to a NUL-terminated BASIX64 source buffer
; already fully resident in memory. Compiles it into basix_code_buf
; (callable at that address on success). Returns EAX=1/0.
; -------------------------------------------------------------------------
basix_compile:
    push rbx
    call basix_compile_reset_state
    call basix_lex_init
    call basix_lex_next
    jmp basix_compile_tail

; -------------------------------------------------------------------------
; basix_compile_file: ECX = first cluster, RDX = file length (bytes),
; R8D = NoFatChain flag. Compiles a BASIX64 program straight from
; exFAT via the streaming lexer (basix_lex_init_stream/basix_lex_next's
; refill check) instead of requiring the whole source resident in one
; buffer first -- what RUN uses. Returns EAX=1/0.
; -------------------------------------------------------------------------
basix_compile_file:
    push rbx
    call basix_compile_reset_state
    call basix_lex_init_stream
    call basix_lex_next
    jmp basix_compile_tail

; -------------------------------------------------------------------------
; basix_compile_tail: the parse loop and finalization shared by both
; entry points above -- identical either way once the source is
; flowing through basix_tok_*, regardless of whether it's backed by a
; fully-resident buffer or the streaming window.
; -------------------------------------------------------------------------
basix_compile_tail:
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
