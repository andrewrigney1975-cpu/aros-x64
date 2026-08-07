; =============================================================================
; BASIX64 lexer.
;
; Pull-based: basix_lex_next() reads from basix_cursor (a raw pointer into
; the NUL-terminated source buffer set by the caller), advances it past
; one token, and fills basix_tok_type/ival/dval/text/len. Keywords are
; matched case-insensitively against plain identifiers, so PRINT, Print
; and print are all the same token.
;
; Float literals (digits '.' digits, no exponent notation yet -- future
; work) are parsed into a double by running real SSE2 arithmetic here in
; the lexer itself (this code runs on the host CPU like anything else in
; the kernel; it isn't part of what's being compiled), which sidesteps
; needing a hand-rolled decimal-to-IEEE754 conversion routine.
; =============================================================================

TOK_EOF     equ 0
TOK_NEWLINE equ 1
TOK_IDENT   equ 2
TOK_INT     equ 4
TOK_FLOAT   equ 5
TOK_STRING  equ 6

TOK_LET     equ 10
TOK_PRINT   equ 11
TOK_IF      equ 12
TOK_THEN    equ 13
TOK_ELSE    equ 14
TOK_ENDIF   equ 15
TOK_FOR     equ 16
TOK_TO      equ 17
TOK_STEP    equ 18
TOK_NEXT    equ 19
TOK_WHILE   equ 20
TOK_WEND    equ 21
TOK_GOTO    equ 22
TOK_GOSUB   equ 23
TOK_RETURN  equ 24
TOK_DIM     equ 25
TOK_AS      equ 26
TOK_INTEGER equ 27
TOK_SFLOAT  equ 28
TOK_DFLOAT  equ 29
TOK_END     equ 30
TOK_MOD     equ 31
TOK_AND     equ 32
TOK_OR      equ 33
TOK_NOT     equ 34

TOK_PLUS    equ 40
TOK_MINUS   equ 41
TOK_STAR    equ 42
TOK_SLASH   equ 43
TOK_EQ      equ 44
TOK_NE      equ 45
TOK_LT      equ 46
TOK_GT      equ 47
TOK_LE      equ 48
TOK_GE      equ 49
TOK_LPAREN  equ 50
TOK_RPAREN  equ 51
TOK_COMMA   equ 52
TOK_COLON   equ 53

; -------------------------------------------------------------------------
; basix_is_digit: AL = char. Returns EAX=1/0.
; -------------------------------------------------------------------------
basix_is_digit:
    cmp al, '0'
    jb .no
    cmp al, '9'
    ja .no
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; -------------------------------------------------------------------------
; basix_is_ident_start: AL = char. Returns EAX=1/0 (letters and underscore).
; -------------------------------------------------------------------------
basix_is_ident_start:
    cmp al, 'a'
    jb .check_upper
    cmp al, 'z'
    ja .check_underscore
    mov eax, 1
    ret
.check_upper:
    cmp al, 'A'
    jb .check_underscore
    cmp al, 'Z'
    ja .check_underscore
    mov eax, 1
    ret
.check_underscore:
    cmp al, '_'
    jne .no
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; -------------------------------------------------------------------------
; basix_is_ident_char: AL = char. Returns EAX=1/0 (ident_start or digit).
; -------------------------------------------------------------------------
; NOTE: is_ident_start and is_digit both return their result in EAX,
; which overwrites AL -- the very character byte this function needs to
; hand to the *second* check. Must re-fetch AL from the stack (not just
; re-use the original register) between the two calls, or a digit right
; after the identifier's first character (e.g. the '1' in "sub1") gets
; silently dropped, truncating the identifier one character short.
basix_is_ident_char:
    push rax
    call basix_is_ident_start
    test eax, eax
    jnz .yes
    mov al, [rsp]
    call basix_is_digit
    test eax, eax
    jnz .yes
    pop rax
    xor eax, eax
    ret
.yes:
    pop rax
    mov eax, 1
    ret

; -------------------------------------------------------------------------
; basix_lex_init: RCX = pointer to a NUL-terminated source buffer.
; -------------------------------------------------------------------------
basix_lex_init:
    mov [rel basix_cursor], rcx
    ret

; -------------------------------------------------------------------------
; basix_keyword_lookup: RCX = ptr to identifier text, EDX = length.
; Returns EAX = the keyword token type, or TOK_IDENT if it isn't one.
; -------------------------------------------------------------------------
basix_keyword_lookup:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov rbx, rcx                        ; rbx = ident ptr
    lea rsi, [rel basix_keywords]
.next_kw:
    movzx eax, byte [rsi]               ; keyword length (0 = end of table)
    test al, al
    jz .no_match
    cmp eax, edx
    jne .kw_skip
    lea rdi, [rsi+5]                    ; keyword text starts 5 bytes in
    mov ecx, eax                        ; ecx = length to compare
    xor r8d, r8d
.cmp_loop:
    cmp r8d, ecx
    jge .kw_match
    mov al, [rbx+r8]
    cmp al, 'a'
    jb .no_fold
    cmp al, 'z'
    ja .no_fold
    sub al, 0x20
.no_fold:
    mov r9b, [rdi+r8]
    cmp al, r9b
    jne .kw_nomatch
    inc r8d
    jmp .cmp_loop
.kw_match:
    mov eax, [rsi+1]                    ; token type dword
    jmp .out
.kw_nomatch:
.kw_skip:
    add rsi, 5+16                       ; length byte + type dword + 16-byte text slot
    jmp .next_kw
.no_match:
    mov eax, TOK_IDENT
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
; basix_lex_next: advances basix_cursor past one token, filling the
; basix_tok_* globals. Unrecognized characters are silently skipped (v1
; has no fine-grained lex diagnostics).
; -------------------------------------------------------------------------
basix_lex_next:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov rsi, [rel basix_cursor]

.top:
.skip_ws:
    movzx eax, byte [rsi]
    cmp al, ' '
    je .ws_next
    cmp al, 9
    je .ws_next
    cmp al, 13
    je .ws_next
    cmp al, 39                          ; '
    je .skip_comment
    jmp .check_rem
.ws_next:
    inc rsi
    jmp .skip_ws

.skip_comment:
    inc rsi
.comment_loop:
    movzx eax, byte [rsi]
    test al, al
    jz .at_end
    cmp al, 10
    je .skip_ws
    inc rsi
    jmp .comment_loop

.check_rem:
    cmp al, 'R'
    je .maybe_rem
    cmp al, 'r'
    jne .not_rem
.maybe_rem:
    mov dl, [rsi+1]
    or dl, 0x20
    cmp dl, 'e'
    jne .not_rem
    mov dl, [rsi+2]
    or dl, 0x20
    cmp dl, 'm'
    jne .not_rem
    mov dl, [rsi+3]
    mov al, dl
    call basix_is_ident_char
    test eax, eax
    jnz .not_rem
    add rsi, 3
    jmp .comment_loop
.not_rem:

    movzx eax, byte [rsi]
    test al, al
    jz .at_end
    cmp al, 10
    je .newline_tok
    cmp al, '"'
    je .string_lit

    call basix_is_digit
    test eax, eax
    jnz .number_lit

    movzx eax, byte [rsi]
    call basix_is_ident_start
    test eax, eax
    jnz .ident_or_kw

    movzx eax, byte [rsi]
    cmp al, '+'
    je .op1_plus
    cmp al, '-'
    je .op1_minus
    cmp al, '*'
    je .op1_star
    cmp al, '/'
    je .op1_slash
    cmp al, '='
    je .op1_eq
    cmp al, '('
    je .op1_lparen
    cmp al, ')'
    je .op1_rparen
    cmp al, ','
    je .op1_comma
    cmp al, ':'
    je .op1_colon
    cmp al, '<'
    je .op_lt_family
    cmp al, '>'
    je .op_gt_family

    inc rsi                             ; unrecognized char -- skip and retry
    mov [rel basix_cursor], rsi
    jmp .top

.op1_plus:
    mov dword [rel basix_tok_type], TOK_PLUS
    jmp .adv1
.op1_minus:
    mov dword [rel basix_tok_type], TOK_MINUS
    jmp .adv1
.op1_star:
    mov dword [rel basix_tok_type], TOK_STAR
    jmp .adv1
.op1_slash:
    mov dword [rel basix_tok_type], TOK_SLASH
    jmp .adv1
.op1_eq:
    mov dword [rel basix_tok_type], TOK_EQ
    jmp .adv1
.op1_lparen:
    mov dword [rel basix_tok_type], TOK_LPAREN
    jmp .adv1
.op1_rparen:
    mov dword [rel basix_tok_type], TOK_RPAREN
    jmp .adv1
.op1_comma:
    mov dword [rel basix_tok_type], TOK_COMMA
    jmp .adv1
.op1_colon:
    mov dword [rel basix_tok_type], TOK_COLON
    jmp .adv1
.adv1:
    inc rsi
    mov [rel basix_cursor], rsi
    jmp .out

.op_lt_family:
    mov al, [rsi+1]
    cmp al, '>'
    je .lt_ne
    cmp al, '='
    je .lt_le
    mov dword [rel basix_tok_type], TOK_LT
    inc rsi
    mov [rel basix_cursor], rsi
    jmp .out
.lt_ne:
    mov dword [rel basix_tok_type], TOK_NE
    add rsi, 2
    mov [rel basix_cursor], rsi
    jmp .out
.lt_le:
    mov dword [rel basix_tok_type], TOK_LE
    add rsi, 2
    mov [rel basix_cursor], rsi
    jmp .out

.op_gt_family:
    mov al, [rsi+1]
    cmp al, '='
    je .gt_ge
    mov dword [rel basix_tok_type], TOK_GT
    inc rsi
    mov [rel basix_cursor], rsi
    jmp .out
.gt_ge:
    mov dword [rel basix_tok_type], TOK_GE
    add rsi, 2
    mov [rel basix_cursor], rsi
    jmp .out

.newline_tok:
    mov dword [rel basix_tok_type], TOK_NEWLINE
.skip_more_newlines:
    inc rsi
    movzx eax, byte [rsi]
    cmp al, 10
    je .skip_more_newlines
    cmp al, 13
    je .skip_more_newlines
    cmp al, ' '
    je .skip_more_newlines
    cmp al, 9
    je .skip_more_newlines
    mov [rel basix_cursor], rsi
    jmp .out

.at_end:
    mov dword [rel basix_tok_type], TOK_EOF
    mov [rel basix_cursor], rsi
    jmp .out

.string_lit:
    inc rsi
    mov [rel basix_tok_text], rsi
    xor edx, edx
.str_loop:
    movzx eax, byte [rsi+rdx]
    test al, al
    jz .str_done
    cmp al, '"'
    je .str_done
    inc edx
    jmp .str_loop
.str_done:
    mov [rel basix_tok_len], edx
    mov dword [rel basix_tok_type], TOK_STRING
    add rsi, rdx
    cmp byte [rsi], '"'
    jne .str_noquote
    inc rsi
.str_noquote:
    mov [rel basix_cursor], rsi
    jmp .out

.number_lit:
    xor r8, r8                          ; r8 = accumulated integer value
    pxor xmm0, xmm0                     ; xmm0 = accumulated double value (mantissa)
    xor r9d, r9d                        ; r9d = is_float flag (0/1)
.int_digits:
    movzx eax, byte [rsi]
    call basix_is_digit
    test eax, eax
    jz .after_int
    movzx eax, byte [rsi]
    sub eax, '0'
    ; r8 = r8*10 + digit
    imul r8, r8, 10
    add r8, rax
    ; xmm0 = xmm0*10.0 + digit
    cvtsi2sd xmm1, eax
    mulsd xmm0, [rel basix_const_10]
    addsd xmm0, xmm1
    inc rsi
    jmp .int_digits
.after_int:
    cmp byte [rsi], '.'
    jne .check_exp
    movzx eax, byte [rsi+1]
    call basix_is_digit
    test eax, eax
    jz .check_exp                       ; a lone '.' isn't part of the number
    mov r9d, 1                          ; has a fractional part -> float
    inc rsi                             ; consume '.'
    pxor xmm2, xmm2                     ; xmm2 = fractional value
    movsd xmm3, [rel basix_const_1]     ; xmm3 = scale
.frac_digits:
    movzx eax, byte [rsi]
    call basix_is_digit
    test eax, eax
    jz .frac_done
    movzx eax, byte [rsi]
    sub eax, '0'
    cvtsi2sd xmm1, eax
    mulsd xmm2, [rel basix_const_10]
    addsd xmm2, xmm1
    mulsd xmm3, [rel basix_const_10]
    inc rsi
    jmp .frac_digits
.frac_done:
    divsd xmm2, xmm3
    addsd xmm0, xmm2

.check_exp:
    ; Optional exponent suffix: [eE][+-]?digits. Only consumed if a real
    ; digit follows the optional sign -- otherwise leave the source
    ; alone (e.g. a bare identifier starting with 'e' right after a
    ; number, however unlikely, must not be swallowed).
    movzx eax, byte [rsi]
    cmp al, 'e'
    je .has_exp_char
    cmp al, 'E'
    jne .finish_number
.has_exp_char:
    xor ebx, ebx                        ; ebx = offset of first digit to check
    inc ebx
    movzx eax, byte [rsi+1]
    cmp al, '+'
    je .exp_has_sign
    cmp al, '-'
    jne .exp_check_digit
.exp_has_sign:
    mov ebx, 2
.exp_check_digit:
    movzx eax, byte [rsi+rbx]
    call basix_is_digit
    test eax, eax
    jz .finish_number                   ; not a real exponent -- don't consume

    mov r9d, 1                          ; an exponent always makes this a float
    inc rsi                             ; consume 'e'/'E'
    xor ebx, ebx                        ; ebx = exponent sign (0=+, 1=-)
    cmp byte [rsi], '+'
    je .exp_skip_sign
    cmp byte [rsi], '-'
    jne .exp_digits
    mov ebx, 1
.exp_skip_sign:
    inc rsi
.exp_digits:
    xor edi, edi                        ; edi = exponent magnitude
.exp_digit_loop:
    movzx eax, byte [rsi]
    call basix_is_digit
    test eax, eax
    jz .exp_apply
    movzx eax, byte [rsi]
    sub eax, '0'
    imul edi, edi, 10
    add edi, eax
    inc rsi
    jmp .exp_digit_loop
.exp_apply:
    test edi, edi
    jz .finish_number
.exp_scale_loop:
    test edi, edi
    jz .finish_number
    test ebx, ebx
    jnz .exp_div
    mulsd xmm0, [rel basix_const_10]
    jmp .exp_scale_next
.exp_div:
    divsd xmm0, [rel basix_const_10]
.exp_scale_next:
    dec edi
    jmp .exp_scale_loop

.finish_number:
    test r9d, r9d
    jnz .is_float
    mov [rel basix_tok_ival], r8
    mov dword [rel basix_tok_type], TOK_INT
    mov [rel basix_cursor], rsi
    jmp .out
.is_float:
    movsd [rel basix_tok_dval], xmm0
    mov dword [rel basix_tok_type], TOK_FLOAT
    mov [rel basix_cursor], rsi
    jmp .out

.ident_or_kw:
    mov [rel basix_tok_text], rsi
    xor edx, edx
.id_loop:
    movzx eax, byte [rsi+rdx]
    call basix_is_ident_char
    test eax, eax
    jz .id_done
    inc edx
    jmp .id_loop
.id_done:
    mov [rel basix_tok_len], edx
    add rsi, rdx
    mov [rel basix_cursor], rsi

    mov rcx, [rel basix_tok_text]
    call basix_keyword_lookup
    mov [rel basix_tok_type], eax
    jmp .out

.out:
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
basix_cursor:   dq 0
basix_tok_type: dd 0
basix_tok_ival: dq 0
basix_tok_dval: dq 0
basix_tok_text: dq 0
basix_tok_len:  dd 0

align 8
basix_const_10: dq 10.0
basix_const_1:  dq 1.0

; Keyword table: each entry is {length: db, token_type: dd, text: 16 bytes
; uppercase, NUL-padded}. Terminated by a zero length byte.
align 4
basix_keywords:
    db 3
    dd TOK_LET
    db 'LET', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_PRINT
    db 'PRINT', 0,0,0,0,0,0,0,0,0,0,0

    db 2
    dd TOK_IF
    db 'IF', 0,0,0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_THEN
    db 'THEN', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_ELSE
    db 'ELSE', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_ENDIF
    db 'ENDIF', 0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_FOR
    db 'FOR', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 2
    dd TOK_TO
    db 'TO', 0,0,0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_STEP
    db 'STEP', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_NEXT
    db 'NEXT', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_WHILE
    db 'WHILE', 0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_WEND
    db 'WEND', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_GOTO
    db 'GOTO', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_GOSUB
    db 'GOSUB', 0,0,0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_RETURN
    db 'RETURN', 0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_DIM
    db 'DIM', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 2
    dd TOK_AS
    db 'AS', 0,0,0,0,0,0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_INTEGER
    db 'INTEGER', 0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_SFLOAT
    db 'SFLOAT', 0,0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_DFLOAT
    db 'DFLOAT', 0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_END
    db 'END', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_MOD
    db 'MOD', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_AND
    db 'AND', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 2
    dd TOK_OR
    db 'OR', 0,0,0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_NOT
    db 'NOT', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 0                                 ; end of table
