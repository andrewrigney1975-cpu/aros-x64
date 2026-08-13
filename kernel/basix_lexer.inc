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
TOK_LBRACKET equ 54
TOK_RBRACKET equ 55

; Vector/matrix types and statements (var_type encoding: 0=INT,1=SFLOAT,
; 2=DFLOAT,3=VECTOR2,4=VECTOR3,5=VECTOR4,6=MATRIX4)
TOK_VECTOR2 equ 60
TOK_VECTOR3 equ 61
TOK_VECTOR4 equ 62
TOK_MATRIX4 equ 63
TOK_VSET    equ 64
TOK_VADD    equ 65
TOK_VSUB    equ 66
TOK_VSCALE  equ 67
TOK_VDOT    equ 68
TOK_VCROSS  equ 69
TOK_VLEN    equ 70
TOK_VNORM   equ 71
TOK_MIDENT  equ 72
TOK_MMUL    equ 73
TOK_MVMUL   equ 74

; Graphics statements
TOK_PSET    equ 75
TOK_LINE    equ 76

; Trig functions (expression-level, IDENT-like: TOK_x LPAREN expr RPAREN)
TOK_SIN     equ 77
TOK_COS     equ 78
TOK_TAN     equ 79
TOK_ASIN    equ 80
TOK_ACOS    equ 81
TOK_ATAN    equ 82

TOK_TIMER   equ 83
TOK_WAIT    equ 84

TOK_CLS     equ 85
TOK_FLIP    equ 86
TOK_TRIFILL equ 87

; Screen-dimension queries (bare keyword, no parens/args -- same shape as TOK_TIMER)
TOK_SCREENW equ 88
TOK_SCREENH equ 89

; Text editor support: keyboard read (bare keyword, function),
; raw-character print and cursor positioning (statements)
TOK_GETKEY  equ 90
TOK_PUTCHAR equ 91
TOK_LOCATE  equ 92

; Text editor support: file I/O (functions, int result 1/0 or bytes-read)
TOK_FSAVE   equ 93
TOK_FLOAD   equ 94

; Text console dimensions in character cells (distinct from SCREENW/
; SCREENH, which are the framebuffer's PIXEL dimensions) -- same bare-
; keyword shape as SCREENW/SCREENH.
TOK_TEXTCOLS equ 95
TOK_TEXTROWS equ 96
TOK_COLOR    equ 97

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
; basix_lex_init: RCX = pointer to a NUL-terminated source buffer
; already fully resident in memory (the non-streaming path -- e.g. the
; boot-time BASIX64 smoke test compiling a fixed string literal).
; Explicitly clears basix_lex_streaming so state from an earlier
; streaming compile (see basix_lex_init_stream) can't leak in.
; -------------------------------------------------------------------------
basix_lex_init:
    mov dword [rel basix_lex_streaming], 0
    mov [rel basix_cursor], rcx
    ret

; -------------------------------------------------------------------------
; basix_lex_init_stream: ECX = first cluster, RDX = file length (bytes),
; R8D = NoFatChain flag. Sets up streaming mode and performs the
; initial window fill (up to BASIX_LEX_WINDOW_CHUNK bytes) via
; exfat_read_file_at -- used by RUN to compile straight from disk
; without first loading the whole program into one buffer (see
; basix_lex_next's refill check for the other half of this).
; -------------------------------------------------------------------------
basix_lex_init_stream:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10

    mov dword [rel basix_lex_streaming], 1
    mov [rel basix_lex_file_first_cluster], ecx
    mov [rel basix_lex_file_nofatchain], r8d
    mov [rel basix_lex_file_length], rdx
    mov qword [rel basix_lex_file_pos], 0
    mov dword [rel basix_lex_window_len], 0

    mov rax, rdx
    cmp rax, BASIX_LEX_WINDOW_CHUNK
    jbe .chunk_ok
    mov rax, BASIX_LEX_WINDOW_CHUNK
.chunk_ok:
    test rax, rax
    jz .place_nul                        ; zero-length source

    push rax                             ; chunk size
    mov ecx, [rel basix_lex_file_first_cluster]
    xor edx, edx
    mov r8, rax
    lea r9, [rel basix_lex_window]
    mov r10d, [rel basix_lex_file_nofatchain]
    call exfat_read_file_at
    pop rcx                              ; rcx = chunk size
    test eax, eax
    jz .fail

    mov [rel basix_lex_window_len], ecx
    mov [rel basix_lex_file_pos], rcx

.place_nul:
    mov rax, [rel basix_lex_file_pos]
    cmp rax, [rel basix_lex_file_length]
    jb .not_eof_yet
    mov ecx, [rel basix_lex_window_len]
    lea rdx, [rel basix_lex_window]
    mov byte [rdx+rcx], 0
.not_eof_yet:
    lea rax, [rel basix_lex_window]
    mov [rel basix_cursor], rax
    jmp .out

.fail:
    mov dword [rel basix_compile_ok], 0
    mov dword [rel basix_lex_window_len], 0
    lea rax, [rel basix_lex_window]
    mov byte [rax], 0
    mov [rel basix_cursor], rax

.out:
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_lex_copy_tok_text: RCX = ptr to token text (identifier or
; string-literal content), EDX = length. Copies into the fixed
; basix_tok_text_buf scratch buffer (clamped to
; BASIX_TOK_TEXT_BUF_SIZE-1 bytes -- identifiers are matched against
; keywords/created as variables using at most 31 chars elsewhere in
; this compiler anyway, per basix_name_eq's own cap, so this only
; bites unusually long string literals) and points basix_tok_text/
; basix_tok_len at the copy. See the comment on basix_tok_text_buf for
; why this indirection exists.
; -------------------------------------------------------------------------
basix_lex_copy_tok_text:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    mov edx, edx                        ; zero-extend (EDX write already
                                         ; clears the upper 32 bits of RDX)
    cmp edx, BASIX_TOK_TEXT_BUF_SIZE - 1
    jbe .len_ok
    mov edx, BASIX_TOK_TEXT_BUF_SIZE - 1
.len_ok:
    mov rsi, rcx
    lea rdi, [rel basix_tok_text_buf]
    mov ecx, edx
    cld
    rep movsb
    mov byte [rdi], 0                   ; harmless extra NUL past the
                                         ; copied bytes; tok_len is what
                                         ; every caller actually uses

    lea rax, [rel basix_tok_text_buf]
    mov [rel basix_tok_text], rax
    mov [rel basix_tok_len], edx

    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
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

    ; Streaming refill: if compiling straight from disk (see
    ; basix_lex_init_stream) and fewer than BASIX_LEX_REFILL_THRESHOLD
    ; bytes remain ahead of the cursor in the window, pull the next
    ; chunk from exFAT before tokenizing. The threshold is comfortably
    ; larger than any realistic single token (identifiers are capped
    ; at 31 chars) plus the 1-2 byte lookahead a couple of two-char
    ; operators need -- a single token/string literal longer than the
    ; threshold is the one case this doesn't handle, a deliberate
    ; scope limit rather than full arbitrary-length mid-token
    ; streaming, which would need touching every scan loop in this
    ; function instead of just this one entry-point check.
    cmp dword [rel basix_lex_streaming], 0
    je .no_refill

    lea rdi, [rel basix_lex_window]
    mov rax, rsi
    sub rax, rdi                         ; cursor offset within window
    mov ecx, [rel basix_lex_window_len]
    sub ecx, eax                         ; bytes remaining ahead of cursor
    cmp ecx, BASIX_LEX_REFILL_THRESHOLD
    jae .no_refill

    mov rdx, [rel basix_lex_file_pos]
    cmp rdx, [rel basix_lex_file_length]
    jae .no_refill                       ; nothing left on disk to fetch

    ; Retain the unconsumed tail: memmove [rsi..rsi+ecx) down to the
    ; window's base (rdi). Safe as a forward rep movsb since dest<=src.
    push rcx                             ; tail length
    mov r8d, ecx
    push rsi
    push rdi
    mov rcx, r8
    cld
    rep movsb
    pop rdi
    pop rsi
    pop rcx                              ; rcx = tail length again

    mov rax, [rel basix_lex_file_length]
    sub rax, [rel basix_lex_file_pos]
    cmp rax, BASIX_LEX_WINDOW_CHUNK
    jbe .chunk_ok
    mov rax, BASIX_LEX_WINDOW_CHUNK
.chunk_ok:
    push rcx                             ; tail length
    push rax                             ; chunk size
    mov r9, rdi
    add r9, rcx                          ; append right after the tail
    mov ecx, [rel basix_lex_file_first_cluster]
    mov rdx, [rel basix_lex_file_pos]
    mov r8, rax
    mov r10d, [rel basix_lex_file_nofatchain]
    call exfat_read_file_at
    test eax, eax
    jnz .refill_read_ok
    mov dword [rel basix_compile_ok], 0
.refill_read_ok:
    pop rax                              ; chunk size
    pop rcx                              ; tail length

    add [rel basix_lex_file_pos], rax
    add ecx, eax                         ; new window length
    mov [rel basix_lex_window_len], ecx

    mov rdx, [rel basix_lex_file_pos]
    cmp rdx, [rel basix_lex_file_length]
    jb .no_nul_yet
    lea rdi, [rel basix_lex_window]
    mov byte [rdi+rcx], 0                ; true EOF: NUL right after data
.no_nul_yet:

    lea rsi, [rel basix_lex_window]      ; rebase cursor to window start
.no_refill:

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
    cmp al, '['
    je .op1_lbracket
    cmp al, ']'
    je .op1_rbracket
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
.op1_lbracket:
    mov dword [rel basix_tok_type], TOK_LBRACKET
    jmp .adv1
.op1_rbracket:
    mov dword [rel basix_tok_type], TOK_RBRACKET
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
    mov rbx, rsi                        ; rbx = start of string content,
                                         ; survives the scan loop below
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
    mov dword [rel basix_tok_type], TOK_STRING
    push rcx
    push rdx
    mov rcx, rbx
    call basix_lex_copy_tok_text        ; sets tok_text/tok_len (copies
                                         ; into a fixed scratch buffer
                                         ; rather than pointing straight
                                         ; into the window -- see the
                                         ; comment on basix_tok_text_buf)
    pop rdx
    pop rcx
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
    mov rbx, rsi                        ; rbx = start of identifier text,
                                         ; survives the scan loop below
    xor edx, edx
.id_loop:
    movzx eax, byte [rsi+rdx]
    call basix_is_ident_char
    test eax, eax
    jz .id_done
    inc edx
    jmp .id_loop
.id_done:
    push rcx
    push rdx
    mov rcx, rbx
    call basix_lex_copy_tok_text        ; sets tok_text/tok_len (see the
                                         ; comment on basix_tok_text_buf)
    pop rdx
    pop rcx
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

; -------------------------------------------------------------------------
; Streaming-compile state (see basix_lex_init_stream / basix_lex_next's
; refill check). basix_lex_streaming=0 means basix_cursor points
; directly into a fully-resident caller buffer (the original, still-
; supported non-streaming path) -- everything below is only touched
; when it's 1.
; -------------------------------------------------------------------------
BASIX_LEX_WINDOW_CHUNK     equ 4096
BASIX_LEX_REFILL_THRESHOLD equ 1024
BASIX_LEX_WINDOW_SIZE      equ 5376      ; chunk + threshold + NUL margin

basix_lex_streaming:           dd 0
basix_lex_file_first_cluster:  dd 0
basix_lex_file_nofatchain:     dd 0
align 8
basix_lex_file_length:         dq 0
basix_lex_file_pos:            dq 0
basix_lex_window_len:          dd 0
align 16
basix_lex_window: times BASIX_LEX_WINDOW_SIZE db 0

; basix_tok_text points directly into basix_lex_window for identifiers/
; strings by default -- fine for the non-streaming path (the window IS
; the whole fixed caller buffer there, never moves), but a streaming
; refill's memmove can relocate/overwrite bytes a previously-lexed
; token's basix_tok_text still points at if a parser call site holds
; that pointer across a later basix_lex_next call (basix_parse_dim and
; basix_parse_assignment both do, saving an identifier's tok_text
; across the lex_next that consumes it). basix_lex_copy_tok_text (used
; by the identifier and string-literal paths below) copies into this
; fixed scratch buffer instead, so tok_text stays valid regardless of
; any later window movement -- decouples every such parser call site
; from needing to know or care about the lexer's internal buffering
; strategy, present or future.
BASIX_TOK_TEXT_BUF_SIZE equ 256
basix_tok_text_buf: times BASIX_TOK_TEXT_BUF_SIZE db 0

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

    db 7
    dd TOK_VECTOR2
    db 'VECTOR2', 0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_VECTOR3
    db 'VECTOR3', 0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_VECTOR4
    db 'VECTOR4', 0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_MATRIX4
    db 'MATRIX4', 0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_VSET
    db 'VSET', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_VADD
    db 'VADD', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_VSUB
    db 'VSUB', 0,0,0,0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_VSCALE
    db 'VSCALE', 0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_VDOT
    db 'VDOT', 0,0,0,0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_VCROSS
    db 'VCROSS', 0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_VLEN
    db 'VLEN', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_VNORM
    db 'VNORM', 0,0,0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_MIDENT
    db 'MIDENT', 0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_MMUL
    db 'MMUL', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_MVMUL
    db 'MVMUL', 0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_PSET
    db 'PSET', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_LINE
    db 'LINE', 0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_SIN
    db 'SIN', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_COS
    db 'COS', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_TAN
    db 'TAN', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_ASIN
    db 'ASIN', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_ACOS
    db 'ACOS', 0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_ATAN
    db 'ATAN', 0,0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_TIMER
    db 'TIMER', 0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_WAIT
    db 'WAIT', 0,0,0,0,0,0,0,0,0,0,0,0

    db 3
    dd TOK_CLS
    db 'CLS', 0,0,0,0,0,0,0,0,0,0,0,0,0

    db 4
    dd TOK_FLIP
    db 'FLIP', 0,0,0,0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_TRIFILL
    db 'TRIFILL', 0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_SCREENW
    db 'SCREENW', 0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_SCREENH
    db 'SCREENH', 0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_GETKEY
    db 'GETKEY', 0,0,0,0,0,0,0,0,0,0

    db 7
    dd TOK_PUTCHAR
    db 'PUTCHAR', 0,0,0,0,0,0,0,0,0

    db 6
    dd TOK_LOCATE
    db 'LOCATE', 0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_FSAVE
    db 'FSAVE', 0,0,0,0,0,0,0,0,0,0,0

    db 5
    dd TOK_FLOAD
    db 'FLOAD', 0,0,0,0,0,0,0,0,0,0,0

    db 8
    dd TOK_TEXTCOLS
    db 'TEXTCOLS', 0,0,0,0,0,0,0,0

    db 8
    dd TOK_TEXTROWS
    db 'TEXTROWS', 0,0,0,0,0,0,0,0

    db 5
    dd TOK_COLOR
    db 'COLOR', 0,0,0,0,0,0,0,0,0,0,0

    db 0                                 ; end of table
