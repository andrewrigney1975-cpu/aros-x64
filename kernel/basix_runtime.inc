; =============================================================================
; BASIX64 runtime helpers -- real kernel functions that compiled BASIX64
; programs CALL into directly (fixed addresses, plain internal calling
; convention, same as everything else in this kernel).
; =============================================================================

; -------------------------------------------------------------------------
; basix_rt_print_int: RCX = signed int64 value. Prints its decimal
; representation via console_putc (no trailing newline/space).
; -------------------------------------------------------------------------
; NOTE: this function must NEVER use RBX as scratch -- it's a global
; convention throughout the kernel holding boot_info* (see kernel.asm's
; entry:), and fb_draw_char/console_scroll_up (reached via the
; console_putc calls below) dereference it. Using r11 instead cost a
; real bug here: rbx=10 (the divisor) got read as boot_info* mid-loop,
; producing a #GP the moment a digit print touched the framebuffer.
basix_rt_print_int:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r11

    mov rax, rcx
    test rax, rax
    jns .positive
    push rax
    mov al, '-'
    call console_putc
    pop rax
    neg rax
.positive:
    lea rdi, [rel basix_rt_digit_buf + 20]
    mov r11, 10
    xor ecx, ecx
    test rax, rax
    jnz .digit_loop
    dec rdi
    mov byte [rdi], '0'
    inc ecx
    jmp .print_digits
.digit_loop:
    test rax, rax
    jz .print_digits
    xor edx, edx
    div r11
    add dl, '0'
    dec rdi
    mov [rdi], dl
    inc ecx
    jmp .digit_loop
.print_digits:
    xor esi, esi
.print_loop:
    cmp esi, ecx
    jge .done
    movzx eax, byte [rdi+rsi]
    call console_putc
    inc esi
    jmp .print_loop
.done:
    pop r11
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_print_str: RCX = ptr, RDX = length (not NUL-terminated -- these
; come straight from string literal tokens, which point into source text).
; -------------------------------------------------------------------------
basix_rt_print_str:
    push rax
    push rcx
    push rdx
    push rsi

    mov rsi, rcx
    xor ecx, ecx
.loop:
    cmp ecx, edx
    jge .done
    movzx eax, byte [rsi+rcx]
    call console_putc
    inc ecx
    jmp .loop
.done:
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_print_double: XMM0 = double value. Prints sign, integer part
; (reusing basix_rt_print_int), a decimal point, and up to 6 fractional
; digits with trailing zeros trimmed (but always at least one digit, so
; a whole value like 3.0 still reads as a float, not "3"). Fixed 6-digit
; precision, no scientific notation -- v1 simplification, not a general-
; purpose float formatter.
; -------------------------------------------------------------------------
basix_rt_print_double:
    push rax
    push rcx
    push rsi
    push r8

    xorpd xmm1, xmm1
    comisd xmm0, xmm1
    jae .positive
    mov al, '-'
    call console_putc
    xorpd xmm1, xmm1
    subsd xmm1, xmm0
    movsd xmm0, xmm1
.positive:
    cvttsd2si rcx, xmm0
    call basix_rt_print_int

    mov al, '.'
    call console_putc

    cvtsi2sd xmm1, rcx
    subsd xmm0, xmm1            ; xmm0 = fractional part, 0 <= frac < 1

    lea rsi, [rel basix_rt_frac_buf]
    mov r8d, 6
.digit_loop:
    test r8d, r8d
    jz .digits_done
    mulsd xmm0, [rel basix_rt_ten]
    cvttsd2si rcx, xmm0
    mov eax, ecx
    add al, '0'
    mov [rsi], al
    inc rsi
    cvtsi2sd xmm1, rcx
    subsd xmm0, xmm1
    dec r8d
    jmp .digit_loop
.digits_done:
    ; trim trailing '0's, but keep at least one digit
    lea rax, [rel basix_rt_frac_buf]
    dec rsi
.trim_loop:
    cmp rsi, rax
    jle .print_frac
    cmp byte [rsi], '0'
    jne .print_frac
    dec rsi
    jmp .trim_loop
.print_frac:
    lea rax, [rel basix_rt_frac_buf]
    sub rsi, rax
    inc rsi                     ; rsi = number of digits to print
    xor ecx, ecx
.print_loop:
    cmp ecx, esi
    jge .done
    movzx eax, byte [rel basix_rt_frac_buf + rcx]
    call console_putc
    inc ecx
    jmp .print_loop
.done:
    pop r8
    pop rsi
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_newline: prints CRLF.
; -------------------------------------------------------------------------
basix_rt_newline:
    push rax
    mov al, 13
    call console_putc
    mov al, 10
    call console_putc
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
basix_rt_digit_buf: times 20 db 0

align 8
basix_rt_ten: dq 10.0
basix_rt_frac_buf: times 6 db 0
