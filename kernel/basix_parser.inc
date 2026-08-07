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
    push r12
    mov rcx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    call basix_var_find_or_create
    mov r10d, eax
    mov r9d, [rel basix_var_type + r10*4]
    call basix_lex_next                   ; consume the identifier

    cmp dword [rel basix_tok_type], TOK_LBRACKET
    jne .ident_no_index

    ; indexed access: v[N] -- N must be an integer literal (v1 scope;
    ; no runtime-computed indices).
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
    call basix_parse_arith               ; r11d = right's type
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

    mov rbx, [rel basix_tok_text]
    mov edx, [rel basix_tok_len]
    push rdx
    mov rcx, rbx
    call basix_var_find_or_create
    pop rdx
    push rax

    call basix_lex_next

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

.done:
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
    pop r12
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
