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

; -------------------------------------------------------------------------
; basix_rt_pset: RCX = x, RDX = y, R8 = color (a packed 32-bit pixel
; value written directly, channel order doesn't matter -- same as every
; other framebuffer write in this kernel, e.g. fb_draw_char). Bounds-
; checked against the real framebuffer dimensions (FB_WIDTH/FB_HEIGHT
; in boot_info*, kept in RBX globally); out-of-range coordinates are
; silently ignored rather than corrupting memory past the framebuffer,
; since a compiled BASIX64 program's own arithmetic is the only thing
; producing these coordinates and nothing here validates it in advance.
;
; NOTE: must NEVER use RBX as scratch -- see basix_rt_print_int's note;
; boot_info* lives there for the whole kernel, framebuffer fields read
; here (FB_BASE/FB_WIDTH/FB_HEIGHT/FB_STRIDE) come straight off it.
; -------------------------------------------------------------------------
basix_rt_pset:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    cmp ecx, 0
    jl .out
    cmp edx, 0
    jl .out
    cmp ecx, [rbx+FB_WIDTH]
    jge .out
    cmp edx, [rbx+FB_HEIGHT]
    jge .out

    mov eax, edx
    imul eax, dword [rbx+FB_STRIDE]
    add eax, ecx                        ; eax = y*stride + x (zero-extends rax)
    shl rax, 2                          ; * 4 bytes/pixel
    mov rdi, [rbx+FB_BASE]
    add rdi, rax
    mov eax, r8d
    mov [rdi], eax
.out:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_line: RCX = x1, RDX = y1, R8 = x2, R9 = y2, R10 = color.
; Draws a line via the standard integer Bresenham algorithm (all
; octants), plotting each pixel through basix_rt_pset (so the same
; bounds check applies per-pixel -- a line that runs off-screen just
; gets clipped one pixel at a time rather than needing its own clip
; test up front).
;
; Persistent loop state lives in r8/r9 (current x/y -- reusing the
; same registers pset's own x/y arguments go in, since pset preserves
; RCX/RDX across its own call but not R8/R9, see the restore below),
; r10=dx, r11=dy (stored negative, i.e. -abs(y2-y1) -- the classic
; formulation this algorithm uses), r12=sx, r13=sy, r14=err, r15=color.
; The target (x2,y2) only needs to survive setup, so it's spilled to
; the two scratch globals below rather than claiming a register.
; -------------------------------------------------------------------------
basix_rt_line:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov [rel basix_rt_line_x2], r8d
    mov [rel basix_rt_line_y2], r9d
    mov r15d, r10d                      ; color

    ; dx = abs(x2-x1), sx = x1<x2 ? 1 : -1
    mov eax, r8d
    sub eax, ecx
    mov r10d, eax
    test r10d, r10d
    jns .dx_abs_done
    neg r10d
.dx_abs_done:
    mov r12d, 1
    cmp ecx, r8d
    jl .sx_done
    mov r12d, -1
.sx_done:

    ; dy = -abs(y2-y1), sy = y1<y2 ? 1 : -1
    mov eax, r9d
    sub eax, edx
    mov r11d, eax
    test r11d, r11d
    jns .dy_abs_done
    neg r11d
.dy_abs_done:
    neg r11d                            ; r11d = -abs(y2-y1)
    mov r13d, 1
    cmp edx, r9d
    jl .sy_done
    mov r13d, -1
.sy_done:

    mov r14d, r10d
    add r14d, r11d                      ; err = dx+dy

    mov r8d, ecx                        ; cur_x = x1
    mov r9d, edx                        ; cur_y = y1

.loop:
    mov ecx, r8d
    mov edx, r9d
    mov r8d, r15d                       ; color (clobbers cur_x -- restored
                                         ; below from ECX, which pset
                                         ; preserves across its own call)
    call basix_rt_pset
    mov r8d, ecx                        ; restore cur_x

    cmp r8d, [rel basix_rt_line_x2]
    jne .continue
    cmp r9d, [rel basix_rt_line_y2]
    jne .continue
    jmp .done

.continue:
    mov eax, r14d
    add eax, eax                        ; e2 = 2*err
    cmp eax, r11d
    jl .no_x_step
    add r14d, r11d                      ; err += dy
    add r8d, r12d                       ; cur_x += sx
.no_x_step:
    cmp eax, r10d
    jg .no_y_step
    add r14d, r10d                      ; err += dx
    add r9d, r13d                       ; cur_y += sy
.no_y_step:
    jmp .loop

.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
basix_rt_digit_buf: times 20 db 0

align 8
basix_rt_ten: dq 10.0
basix_rt_frac_buf: times 6 db 0
basix_rt_line_x2: dd 0
basix_rt_line_y2: dd 0
