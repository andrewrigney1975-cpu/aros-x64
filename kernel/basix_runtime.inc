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
; basix_rt_ensure_backbuf: lazily allocates (via kmalloc) and zeroes the
; back buffer -- a private, tightly-packed (stride == width, no GOP
; padding) copy of the frame that PSET/LINE/CLS draw into, sized off
; the real framebuffer's own dimensions (FB_WIDTH/FB_HEIGHT in
; boot_info*, kept in RBX globally -- see basix_rt_print_int's note on
; never clobbering it). Idempotent: every caller below runs this first
; and it's a single pointer check after the first successful call.
;
; Off-screen drawing is what actually fixes the flicker WAIT-paced
; animation used to show: previously PSET/LINE wrote straight to the
; real, currently-scanned-out framebuffer, so a viewer could catch a
; frame mid-redraw (old lines erased, new ones not yet drawn). Now a
; whole frame is composed here, invisibly, and FLIP (basix_rt_flip)
; presents it in one blit.
;
; On kmalloc failure, basix_backbuf_ptr is left at 0 and every caller
; below checks that and no-ops rather than dereferencing NULL -- a
; compiled program with no graphics simply draws nothing rather than
; crashing the kernel.
; -------------------------------------------------------------------------
basix_rt_ensure_backbuf:
    push rax
    push rcx
    push rdx
    push rdi

    cmp qword [rel basix_backbuf_ptr], 0
    jne .out

    mov eax, [rbx+FB_WIDTH]
    mov [rel basix_backbuf_w], eax
    mov eax, [rbx+FB_HEIGHT]
    mov [rel basix_backbuf_h], eax

    mov eax, [rel basix_backbuf_w]
    imul eax, [rel basix_backbuf_h]
    shl rax, 2                          ; * 4 bytes/pixel
    mov rcx, rax
    call kmalloc
    test rax, rax
    jz .out
    mov [rel basix_backbuf_ptr], rax

    mov rdi, rax
    mov eax, [rel basix_backbuf_w]
    imul eax, [rel basix_backbuf_h]
    mov ecx, eax
    xor eax, eax
    cld
    rep stosd
.out:
    pop rdi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_pset: RCX = x, RDX = y, R8 = color (packed 32-bit pixel,
; channel order doesn't matter -- see basix_rt_blend_pixel). Writes
; into the back buffer (not the visible framebuffer -- see FLIP), with
; the same bounds-check-and-ignore behavior as before.
; -------------------------------------------------------------------------
basix_rt_pset:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    cmp ecx, 0
    jl .out
    cmp edx, 0
    jl .out
    cmp ecx, [rel basix_backbuf_w]
    jge .out
    cmp edx, [rel basix_backbuf_h]
    jge .out

    mov eax, edx
    imul eax, [rel basix_backbuf_w]
    add eax, ecx                        ; eax = y*w + x (zero-extends rax)
    shl rax, 2                          ; * 4 bytes/pixel
    mov rdi, [rel basix_backbuf_ptr]
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
; basix_rt_cls: RCX = color. Fills the entire back buffer with it (a
; plain 32-bit-per-pixel rep stosd -- no blending, this is a hard
; clear).
; -------------------------------------------------------------------------
basix_rt_cls:
    push rax
    push rcx
    push rdi

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov eax, ecx
    mov rdi, [rel basix_backbuf_ptr]
    mov ecx, [rel basix_backbuf_w]
    imul ecx, [rel basix_backbuf_h]
    cld
    rep stosd
.out:
    pop rdi
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_rect: RCX = color. x/y/w/h are read from basix_rect_x/y/w/h
; (fixed globals the parser stores into via basix_parse_int_arg_store --
; RECT takes five int arguments, one more than fit in RCX/RDX/R8/R9, so
; it follows LINE/TRIFILL's convention of stashing all-but-the-last
; through globals instead of registers). Fills the (clipped) rectangle
; in the back buffer with a plain row-at-a-time rep stosd -- no
; blending, same hard-fill semantics as CLS.
; -------------------------------------------------------------------------
basix_rt_rect:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov r10d, ecx                        ; r10d = color
    mov ecx, [rel basix_rect_x]
    mov edx, [rel basix_rect_y]
    mov r8d, [rel basix_rect_w]
    mov r9d, [rel basix_rect_h]

    ; Clip x/w to [0, backbuf_w)
    test ecx, ecx
    jns .x_ok
    add r8d, ecx                         ; shrink w by how far x was negative
    xor ecx, ecx
.x_ok:
    mov eax, [rel basix_backbuf_w]
    sub eax, ecx
    cmp r8d, eax
    jle .w_ok
    mov r8d, eax
.w_ok:
    test r8d, r8d
    jle .out

    ; Clip y/h to [0, backbuf_h)
    test edx, edx
    jns .y_ok
    add r9d, edx
    xor edx, edx
.y_ok:
    mov eax, [rel basix_backbuf_h]
    sub eax, edx
    cmp r9d, eax
    jle .h_ok
    mov r9d, eax
.h_ok:
    test r9d, r9d
    jle .out

    mov ebx, [rel basix_backbuf_w]       ; ebx = stride (in pixels)
    xor r11d, r11d                       ; r11d = row counter
.row_loop:
    cmp r11d, r9d
    jge .out

    mov eax, edx
    add eax, r11d
    imul eax, ebx
    add eax, ecx                         ; eax = (y+row)*stride + x
    mov rdi, [rel basix_backbuf_ptr]
    lea rdi, [rdi + rax*4]

    mov eax, r10d                        ; color, for stosd
    push rcx
    mov ecx, r8d                         ; width = count of dwords this row
    cld
    rep stosd
    pop rcx

    inc r11d
    jmp .row_loop

.out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_rt_flip: no arguments. Blits the back buffer onto the real,
; visible framebuffer, one scanline at a time -- the two can't share a
; single rep movsd over the whole thing since the back buffer is
; tightly packed (stride == width) but the real framebuffer's stride
; (FB_STRIDE) may exceed its width (GOP padding), so each row needs its
; own copy with the two pointers re-synced afterward.
; -------------------------------------------------------------------------
basix_rt_flip:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov r8d, [rel basix_backbuf_h]
    test r8d, r8d
    jz .out
    xor edx, edx                        ; row index
    mov rsi, [rel basix_backbuf_ptr]
    mov rdi, [rbx+FB_BASE]
.rowloop:
    cmp edx, r8d
    jge .out
    mov ecx, [rel basix_backbuf_w]
    push rsi
    push rdi
    cld
    rep movsd
    pop rdi
    pop rsi
    mov eax, [rel basix_backbuf_w]
    shl rax, 2
    add rsi, rax
    mov eax, [rbx+FB_STRIDE]
    shl rax, 2
    add rdi, rax
    inc edx
    jmp .rowloop
.out:
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_dfloor: XMM0 = value. Returns EAX = floor(value) as int32;
; XMM0 is left unchanged (clobbers XMM1 only) so a caller can still
; compute fpart = value - floor afterward. cvttsd2si truncates toward
; zero, which already equals floor() for value >= 0 and for exact
; negative integers; the one case it gets wrong is a negative non-
; integer, where truncating rounds up (toward zero) instead of down --
; corrected here by decrementing when the truncated value overshoots.
; -------------------------------------------------------------------------
basix_rt_dfloor:
    cvttsd2si eax, xmm0
    cvtsi2sd xmm1, eax
    comisd xmm1, xmm0
    jbe .done
    dec eax
.done:
    ret

; -------------------------------------------------------------------------
; basix_rt_fill_tri: RCX = packed 32-bit color. Vertices are read from
; basix_tri_x0f/y0f/x1f/y1f/x2f/y2f (doubles -- basix_parse_trifill
; stores them there, see basix_parse_float_arg_store). Fills a solid
; triangle into the back buffer via the standard edge-function test:
; for a point P, w0/w1/w2 are the signed areas of the sub-triangles
; (v1,v2,P), (v2,v0,P), (v0,v1,P) -- P is inside iff all three carry
; the same sign as the whole triangle's own signed area (computed once
; up front, "area" below), which is what lets this work regardless of
; the triangle's winding order without a branch to detect it (just
; multiply each w by area and check the product is non-negative).
;
; This is a hard-edged fill (every covered pixel gets the flat color,
; no coverage blending at the boundary) -- deliberately simpler than
; basix_rt_line_aa's Wu's-algorithm approach, since a caller wanting
; smooth silhouettes on a filled polygon is expected to additionally
; stroke its boundary edges with LINE in the same color (which IS
; anti-aliased) rather than this routine anti-aliasing its own edges.
; -------------------------------------------------------------------------
basix_rt_fill_tri:
    push rax
    push rcx
    push rdx
    push rdi

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov [rel basix_tri_color], ecx

    ; area = (x1-x0)*(y2-y0) - (y1-y0)*(x2-x0)
    movsd xmm0, [rel basix_tri_x1f]
    subsd xmm0, [rel basix_tri_x0f]
    movsd xmm1, [rel basix_tri_y2f]
    subsd xmm1, [rel basix_tri_y0f]
    mulsd xmm0, xmm1
    movsd xmm1, [rel basix_tri_y1f]
    subsd xmm1, [rel basix_tri_y0f]
    movsd xmm2, [rel basix_tri_x2f]
    subsd xmm2, [rel basix_tri_x0f]
    mulsd xmm1, xmm2
    subsd xmm0, xmm1
    movsd [rel basix_tri_area], xmm0

    xorpd xmm1, xmm1
    comisd xmm0, xmm1
    je .out                              ; degenerate (zero-area) triangle

    ; bounding box, clamped to the back buffer
    movsd xmm0, [rel basix_tri_x0f]
    movsd xmm1, [rel basix_tri_x1f]
    minsd xmm0, xmm1
    movsd xmm1, [rel basix_tri_x2f]
    minsd xmm0, xmm1
    call basix_rt_dfloor
    cmp eax, 0
    jge .minx_ok
    xor eax, eax
.minx_ok:
    mov [rel basix_tri_minx], eax

    movsd xmm0, [rel basix_tri_x0f]
    movsd xmm1, [rel basix_tri_x1f]
    maxsd xmm0, xmm1
    movsd xmm1, [rel basix_tri_x2f]
    maxsd xmm0, xmm1
    call basix_rt_dfloor
    inc eax
    mov ecx, [rel basix_backbuf_w]
    dec ecx
    cmp eax, ecx
    jle .maxx_ok
    mov eax, ecx
.maxx_ok:
    mov [rel basix_tri_maxx], eax

    movsd xmm0, [rel basix_tri_y0f]
    movsd xmm1, [rel basix_tri_y1f]
    minsd xmm0, xmm1
    movsd xmm1, [rel basix_tri_y2f]
    minsd xmm0, xmm1
    call basix_rt_dfloor
    cmp eax, 0
    jge .miny_ok
    xor eax, eax
.miny_ok:
    mov [rel basix_tri_miny], eax

    movsd xmm0, [rel basix_tri_y0f]
    movsd xmm1, [rel basix_tri_y1f]
    maxsd xmm0, xmm1
    movsd xmm1, [rel basix_tri_y2f]
    maxsd xmm0, xmm1
    call basix_rt_dfloor
    inc eax
    mov ecx, [rel basix_backbuf_h]
    dec ecx
    cmp eax, ecx
    jle .maxy_ok
    mov eax, ecx
.maxy_ok:
    mov [rel basix_tri_maxy], eax

    ; Per-edge coefficients so each pixel's edge test is just
    ; A*sx+B*sy+C (2 muls, 2 adds) instead of recomputing every
    ; subtraction from the raw vertices -- and each is pre-scaled by
    ; "area" too, folding the separate "multiply by area" step (see
    ; the header comment) into these constants instead of repeating it
    ; per pixel. w_i(x,y) = A_i*x + B_i*y + C_i is the standard linear
    ; form of the edge function E(a,b,P) = (b.x-a.x)*(P.y-a.y) -
    ; (b.y-a.y)*(P.x-a.x), with A=a.y-b.y, B=b.x-a.x, C=-A*a.x-B*a.y.
    ; This all runs ONCE per triangle (outside the pixel loop below) --
    ; the loop it feeds is the actual hot path, since it runs once per
    ; covered pixel, easily tens of thousands of times for one cube
    ; face at this resolution.
    movsd xmm7, [rel basix_tri_area]

    movsd xmm0, [rel basix_tri_y1f]
    subsd xmm0, [rel basix_tri_y2f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_a0], xmm0
    movsd xmm0, [rel basix_tri_x2f]
    subsd xmm0, [rel basix_tri_x1f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_b0], xmm0
    movsd xmm0, [rel basix_tri_x1f]
    mulsd xmm0, [rel basix_tri_y2f]
    movsd xmm1, [rel basix_tri_x2f]
    mulsd xmm1, [rel basix_tri_y1f]
    subsd xmm0, xmm1
    mulsd xmm0, xmm7
    movsd [rel basix_tri_c0], xmm0

    movsd xmm0, [rel basix_tri_y2f]
    subsd xmm0, [rel basix_tri_y0f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_a1], xmm0
    movsd xmm0, [rel basix_tri_x0f]
    subsd xmm0, [rel basix_tri_x2f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_b1], xmm0
    movsd xmm0, [rel basix_tri_x2f]
    mulsd xmm0, [rel basix_tri_y0f]
    movsd xmm1, [rel basix_tri_x0f]
    mulsd xmm1, [rel basix_tri_y2f]
    subsd xmm0, xmm1
    mulsd xmm0, xmm7
    movsd [rel basix_tri_c1], xmm0

    movsd xmm0, [rel basix_tri_y0f]
    subsd xmm0, [rel basix_tri_y1f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_a2], xmm0
    movsd xmm0, [rel basix_tri_x1f]
    subsd xmm0, [rel basix_tri_x0f]
    mulsd xmm0, xmm7
    movsd [rel basix_tri_b2], xmm0
    movsd xmm0, [rel basix_tri_x0f]
    mulsd xmm0, [rel basix_tri_y1f]
    movsd xmm1, [rel basix_tri_x1f]
    mulsd xmm1, [rel basix_tri_y0f]
    subsd xmm0, xmm1
    mulsd xmm0, xmm7
    movsd [rel basix_tri_c2], xmm0

    ; Registers held resident for the whole pixel loop below (never
    ; reloaded from memory per-pixel): xmm8/9/10 = A0/B0/C0, xmm11/12/13
    ; = A1/B1/C1, xmm14 = A2 -- B2/C2 stay in memory (only so many XMM
    ; registers to go around), plus xmm15 = 0.0 for the inside tests.
    movsd xmm8,  [rel basix_tri_a0]
    movsd xmm9,  [rel basix_tri_b0]
    movsd xmm10, [rel basix_tri_c0]
    movsd xmm11, [rel basix_tri_a1]
    movsd xmm12, [rel basix_tri_b1]
    movsd xmm13, [rel basix_tri_c1]
    movsd xmm14, [rel basix_tri_a2]
    xorpd xmm15, xmm15

    mov eax, [rel basix_tri_miny]
    mov [rel basix_tri_py], eax
.yloop:
    mov eax, [rel basix_tri_py]
    cmp eax, [rel basix_tri_maxy]
    jg .out

    mov eax, [rel basix_tri_minx]
    mov [rel basix_tri_px], eax
.xloop:
    mov eax, [rel basix_tri_px]
    cmp eax, [rel basix_tri_maxx]
    jg .ynext

    mov eax, [rel basix_tri_px]
    cvtsi2sd xmm4, eax
    addsd xmm4, [rel basix_rt_half]      ; xmm4 = sample x (px+0.5)
    mov eax, [rel basix_tri_py]
    cvtsi2sd xmm5, eax
    addsd xmm5, [rel basix_rt_half]      ; xmm5 = sample y (py+0.5)

    ; w0 = A0*sx + B0*sy + C0
    movsd xmm0, xmm8
    mulsd xmm0, xmm4
    movsd xmm1, xmm9
    mulsd xmm1, xmm5
    addsd xmm0, xmm1
    addsd xmm0, xmm10
    comisd xmm0, xmm15
    jb .not_inside

    ; w1 = A1*sx + B1*sy + C1
    movsd xmm0, xmm11
    mulsd xmm0, xmm4
    movsd xmm1, xmm12
    mulsd xmm1, xmm5
    addsd xmm0, xmm1
    addsd xmm0, xmm13
    comisd xmm0, xmm15
    jb .not_inside

    ; w2 = A2*sx + B2*sy + C2
    movsd xmm0, xmm14
    mulsd xmm0, xmm4
    movsd xmm1, [rel basix_tri_b2]
    mulsd xmm1, xmm5
    addsd xmm0, xmm1
    addsd xmm0, [rel basix_tri_c2]
    comisd xmm0, xmm15
    jb .not_inside

    mov edx, [rel basix_tri_py]
    mov ecx, [rel basix_tri_px]
    cmp ecx, 0
    jl .not_inside
    cmp edx, 0
    jl .not_inside
    cmp ecx, [rel basix_backbuf_w]
    jge .not_inside
    cmp edx, [rel basix_backbuf_h]
    jge .not_inside
    mov eax, edx
    imul eax, [rel basix_backbuf_w]
    add eax, ecx
    shl rax, 2
    mov rdi, [rel basix_backbuf_ptr]
    add rdi, rax
    mov eax, [rel basix_tri_color]
    mov [rdi], eax

.not_inside:
    mov eax, [rel basix_tri_px]
    inc eax
    mov [rel basix_tri_px], eax
    jmp .xloop
.ynext:
    mov eax, [rel basix_tri_py]
    inc eax
    mov [rel basix_tri_py], eax
    jmp .yloop

.out:
    pop rdi
    pop rdx
    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_blend_pixel: ECX = x, EDX = y, R8D = packed 32-bit color,
; XMM0 = coverage (0.0..1.0, clamped here). Alpha-blends color into the
; back buffer at (x,y): new = bg + (fg-bg)*coverage, done independently
; per byte (not per RGB channel with any assumed order) -- since a
; linear blend of each byte is identical regardless of which byte is
; red/green/blue/reserved, this needs no knowledge of the GOP's actual
; pixel format, matching the "channel order doesn't matter" convention
; basix_rt_pset already relies on. Bounds-checked the same as PSET.
; -------------------------------------------------------------------------
basix_rt_blend_pixel:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    cmp qword [rel basix_backbuf_ptr], 0
    je .out
    cmp ecx, 0
    jl .out
    cmp edx, 0
    jl .out
    cmp ecx, [rel basix_backbuf_w]
    jge .out
    cmp edx, [rel basix_backbuf_h]
    jge .out

    ; clamp coverage to [0,1]
    xorpd xmm1, xmm1
    comisd xmm0, xmm1
    jae .cov_not_neg
    xorpd xmm0, xmm0
.cov_not_neg:
    movsd xmm1, [rel basix_rt_trig_one]
    comisd xmm0, xmm1
    jbe .cov_not_over
    movsd xmm0, xmm1
.cov_not_over:

    mov eax, edx
    imul eax, [rel basix_backbuf_w]
    add eax, ecx
    mov rdi, [rel basix_backbuf_ptr]
    lea rdi, [rdi + rax*4]

    mov r10d, [rdi]                     ; bg dword
    mov r11d, r8d                       ; fg dword (color)
    xor ebx, ebx                        ; result accumulator
    xor r9d, r9d                        ; channel index 0..3
.chan_loop:
    mov ecx, r9d
    shl ecx, 3                          ; shift amount = channel*8

    mov eax, r10d
    shr eax, cl
    and eax, 0xFF                       ; eax = bg byte
    mov edx, r11d
    shr edx, cl
    and edx, 0xFF                       ; edx = fg byte

    cvtsi2sd xmm2, eax
    cvtsi2sd xmm3, edx
    subsd xmm3, xmm2                    ; fg-bg
    mulsd xmm3, xmm0                    ; (fg-bg)*coverage
    addsd xmm3, xmm2                    ; bg + (fg-bg)*coverage
    addsd xmm3, [rel basix_rt_half]     ; round to nearest on truncation
    cvttsd2si eax, xmm3
    cmp eax, 0
    jge .no_clamp_low
    xor eax, eax
.no_clamp_low:
    cmp eax, 255
    jle .no_clamp_high
    mov eax, 255
.no_clamp_high:
    shl eax, cl
    or ebx, eax

    inc r9d
    cmp r9d, 4
    jl .chan_loop

    mov [rdi], ebx

.out:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_line_aa: RCX = packed 32-bit color. Endpoints are read from
; basix_line_x1f/y1f/x2f/y2f (doubles -- basix_parse_line stores them
; there before this call, see basix_parse_float_arg_store). Draws an
; anti-aliased line into the back buffer via Xiaolin Wu's algorithm:
; every line is walked along its dominant axis (whichever of dx/dy is
; larger -- the "steep" flag below swaps x/y roles so the walk is
; always one pixel per step along that axis) and each step touches TWO
; adjacent pixels, weighted by how far the true (sub-pixel) line
; passes between them, rather than one hard pixel per Bresenham step.
; That fractional weighting is exactly what basix_rt_blend_pixel's
; coverage parameter is for.
; -------------------------------------------------------------------------
basix_rt_line_aa:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .ret_out

    mov [rel basix_wu_color], ecx

    movsd xmm0, [rel basix_line_x1f]
    movsd [rel basix_wu_x0], xmm0
    movsd xmm0, [rel basix_line_y1f]
    movsd [rel basix_wu_y0], xmm0
    movsd xmm0, [rel basix_line_x2f]
    movsd [rel basix_wu_x1], xmm0
    movsd xmm0, [rel basix_line_y2f]
    movsd [rel basix_wu_y1], xmm0

    ; steep = |y1-y0| > |x1-x0|
    movsd xmm0, [rel basix_wu_y1]
    subsd xmm0, [rel basix_wu_y0]
    xorpd xmm2, xmm2
    comisd xmm0, xmm2
    jae .absdy_ok
    xorpd xmm3, xmm3
    subsd xmm3, xmm0
    movsd xmm0, xmm3
.absdy_ok:
    movsd [rel basix_wu_t], xmm0        ; |dy|

    movsd xmm0, [rel basix_wu_x1]
    subsd xmm0, [rel basix_wu_x0]
    xorpd xmm2, xmm2
    comisd xmm0, xmm2
    jae .absdx_ok
    xorpd xmm3, xmm3
    subsd xmm3, xmm0
    movsd xmm0, xmm3
.absdx_ok:
    movsd xmm1, [rel basix_wu_t]        ; |dy|
    comisd xmm1, xmm0                   ; |dy| vs |dx|
    mov dword [rel basix_wu_steep], 0
    jbe .not_steep
    mov dword [rel basix_wu_steep], 1
    movsd xmm0, [rel basix_wu_x0]
    movsd xmm1, [rel basix_wu_y0]
    movsd [rel basix_wu_x0], xmm1
    movsd [rel basix_wu_y0], xmm0
    movsd xmm0, [rel basix_wu_x1]
    movsd xmm1, [rel basix_wu_y1]
    movsd [rel basix_wu_x1], xmm1
    movsd [rel basix_wu_y1], xmm0
.not_steep:

    ; if x0 > x1: swap endpoints so the walk always goes left to right
    movsd xmm0, [rel basix_wu_x0]
    movsd xmm1, [rel basix_wu_x1]
    comisd xmm0, xmm1
    jbe .no_endswap
    movsd [rel basix_wu_x0], xmm1
    movsd [rel basix_wu_x1], xmm0
    movsd xmm0, [rel basix_wu_y0]
    movsd xmm1, [rel basix_wu_y1]
    movsd [rel basix_wu_y0], xmm1
    movsd [rel basix_wu_y1], xmm0
.no_endswap:

    movsd xmm0, [rel basix_wu_x1]
    subsd xmm0, [rel basix_wu_x0]
    movsd [rel basix_wu_dx], xmm0
    movsd xmm0, [rel basix_wu_y1]
    subsd xmm0, [rel basix_wu_y0]
    movsd [rel basix_wu_dy], xmm0

    ; gradient = dx==0 ? 1.0 : dy/dx
    movsd xmm0, [rel basix_wu_dx]
    xorpd xmm1, xmm1
    comisd xmm0, xmm1
    jne .grad_divide
    movsd xmm0, [rel basix_rt_trig_one]
    movsd [rel basix_wu_gradient], xmm0
    jmp .grad_done
.grad_divide:
    movsd xmm0, [rel basix_wu_dy]
    divsd xmm0, [rel basix_wu_dx]
    movsd [rel basix_wu_gradient], xmm0
.grad_done:

    ; ---- endpoint 1 ----
    movsd xmm0, [rel basix_wu_x0]
    addsd xmm0, [rel basix_rt_half]
    call basix_rt_dfloor                ; eax = round(x0)
    mov [rel basix_wu_xpxl1], eax
    cvtsi2sd xmm1, eax
    movsd [rel basix_wu_xend], xmm1

    movsd xmm0, xmm1
    subsd xmm0, [rel basix_wu_x0]
    mulsd xmm0, [rel basix_wu_gradient]
    addsd xmm0, [rel basix_wu_y0]
    movsd [rel basix_wu_yend], xmm0

    movsd xmm0, [rel basix_wu_x0]       ; xgap = 1 - fpart(x0+0.5)
    addsd xmm0, [rel basix_rt_half]
    subsd xmm0, [rel basix_wu_xend]
    movsd xmm1, [rel basix_rt_trig_one]
    subsd xmm1, xmm0
    movsd [rel basix_wu_xgap], xmm1

    movsd xmm0, [rel basix_wu_yend]
    call basix_rt_dfloor
    mov [rel basix_wu_ypxl1], eax
    cvtsi2sd xmm1, eax
    movsd xmm2, xmm0
    subsd xmm2, xmm1                    ; fpart(yend)
    movsd [rel basix_wu_t], xmm2
    movsd xmm3, [rel basix_rt_trig_one]
    subsd xmm3, xmm2                    ; rfpart(yend)
    movsd [rel basix_wu_t2], xmm3

    movsd xmm0, [rel basix_wu_t2]
    mulsd xmm0, [rel basix_wu_xgap]
    movsd [rel basix_wu_cov_a], xmm0
    movsd xmm0, [rel basix_wu_t]
    mulsd xmm0, [rel basix_wu_xgap]
    movsd [rel basix_wu_cov_b], xmm0

    mov eax, [rel basix_wu_steep]
    test eax, eax
    jz .ep1_notsteep
    mov ecx, [rel basix_wu_ypxl1]
    mov edx, [rel basix_wu_xpxl1]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_a]
    call basix_rt_blend_pixel
    mov ecx, [rel basix_wu_ypxl1]
    inc ecx
    mov edx, [rel basix_wu_xpxl1]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_b]
    call basix_rt_blend_pixel
    jmp .ep1_done
.ep1_notsteep:
    mov ecx, [rel basix_wu_xpxl1]
    mov edx, [rel basix_wu_ypxl1]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_a]
    call basix_rt_blend_pixel
    mov ecx, [rel basix_wu_xpxl1]
    mov edx, [rel basix_wu_ypxl1]
    inc edx
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_b]
    call basix_rt_blend_pixel
.ep1_done:

    movsd xmm0, [rel basix_wu_yend]
    addsd xmm0, [rel basix_wu_gradient]
    movsd [rel basix_wu_intery], xmm0

    ; ---- endpoint 2 ----
    movsd xmm0, [rel basix_wu_x1]
    addsd xmm0, [rel basix_rt_half]
    call basix_rt_dfloor
    mov [rel basix_wu_xpxl2], eax
    cvtsi2sd xmm1, eax
    movsd [rel basix_wu_xend], xmm1

    movsd xmm0, xmm1
    subsd xmm0, [rel basix_wu_x1]
    mulsd xmm0, [rel basix_wu_gradient]
    addsd xmm0, [rel basix_wu_y1]
    movsd [rel basix_wu_yend], xmm0

    movsd xmm0, [rel basix_wu_x1]       ; xgap = fpart(x1+0.5)
    addsd xmm0, [rel basix_rt_half]
    subsd xmm0, [rel basix_wu_xend]
    movsd [rel basix_wu_xgap], xmm0

    movsd xmm0, [rel basix_wu_yend]
    call basix_rt_dfloor
    mov [rel basix_wu_ypxl2], eax
    cvtsi2sd xmm1, eax
    movsd xmm2, xmm0
    subsd xmm2, xmm1
    movsd [rel basix_wu_t], xmm2
    movsd xmm3, [rel basix_rt_trig_one]
    subsd xmm3, xmm2
    movsd [rel basix_wu_t2], xmm3

    movsd xmm0, [rel basix_wu_t2]
    mulsd xmm0, [rel basix_wu_xgap]
    movsd [rel basix_wu_cov_a], xmm0
    movsd xmm0, [rel basix_wu_t]
    mulsd xmm0, [rel basix_wu_xgap]
    movsd [rel basix_wu_cov_b], xmm0

    mov eax, [rel basix_wu_steep]
    test eax, eax
    jz .ep2_notsteep
    mov ecx, [rel basix_wu_ypxl2]
    mov edx, [rel basix_wu_xpxl2]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_a]
    call basix_rt_blend_pixel
    mov ecx, [rel basix_wu_ypxl2]
    inc ecx
    mov edx, [rel basix_wu_xpxl2]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_b]
    call basix_rt_blend_pixel
    jmp .ep2_done
.ep2_notsteep:
    mov ecx, [rel basix_wu_xpxl2]
    mov edx, [rel basix_wu_ypxl2]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_a]
    call basix_rt_blend_pixel
    mov ecx, [rel basix_wu_xpxl2]
    mov edx, [rel basix_wu_ypxl2]
    inc edx
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_cov_b]
    call basix_rt_blend_pixel
.ep2_done:

    ; ---- main loop: x from xpxl1+1 to xpxl2-1 ----
    mov eax, [rel basix_wu_xpxl1]
    inc eax
    mov [rel basix_wu_loopx], eax
.mainloop:
    mov eax, [rel basix_wu_loopx]
    cmp eax, [rel basix_wu_xpxl2]
    jge .mainloop_done

    movsd xmm0, [rel basix_wu_intery]
    call basix_rt_dfloor
    mov r9d, eax                        ; ipart(intery)
    cvtsi2sd xmm1, eax
    movsd xmm2, xmm0
    subsd xmm2, xmm1                    ; fpart(intery)
    movsd [rel basix_wu_t], xmm2
    movsd xmm3, [rel basix_rt_trig_one]
    subsd xmm3, xmm2                    ; rfpart(intery)
    movsd [rel basix_wu_t2], xmm3

    mov eax, [rel basix_wu_steep]
    test eax, eax
    jz .ml_notsteep
    mov ecx, r9d
    mov edx, [rel basix_wu_loopx]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_t2]
    call basix_rt_blend_pixel
    mov ecx, r9d
    inc ecx
    mov edx, [rel basix_wu_loopx]
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_t]
    call basix_rt_blend_pixel
    jmp .ml_stepnext
.ml_notsteep:
    mov ecx, [rel basix_wu_loopx]
    mov edx, r9d
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_t2]
    call basix_rt_blend_pixel
    mov ecx, [rel basix_wu_loopx]
    mov edx, r9d
    inc edx
    mov r8d, [rel basix_wu_color]
    movsd xmm0, [rel basix_wu_t]
    call basix_rt_blend_pixel
.ml_stepnext:
    movsd xmm0, [rel basix_wu_intery]
    addsd xmm0, [rel basix_wu_gradient]
    movsd [rel basix_wu_intery], xmm0

    mov eax, [rel basix_wu_loopx]
    inc eax
    mov [rel basix_wu_loopx], eax
    jmp .mainloop
.mainloop_done:

.ret_out:
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

; -------------------------------------------------------------------------
; basix_rt_sin / basix_rt_cos / basix_rt_tan / basix_rt_asin /
; basix_rt_acos / basix_rt_atan: XMM0 = argument (radians), returns
; XMM0 = result. Bridges to the x87 FPU for the actual transcendental
; instructions (FSIN/FCOS/FPTAN/FPATAN) -- SSE2 has no equivalent, and
; hand-rolling correctly-range-reduced polynomial approximations from
; scratch is real work this kernel doesn't need to take on when the
; CPU already does it in hardware. x87 is otherwise completely unused
; in this kernel (pure SSE2 throughout), but CR0.EM is already clear
; and CR4.OSFXSR already set -- required for the SSE2 this kernel uses
; everywhere else to work at all -- so x87 opcodes execute with no
; further CR0/CR4 changes; basix_fpu_init (called once at boot, see
; kernel.asm) just runs FNINIT to get a sane control/tag word, since
; UEFI makes no promise about x87 state the way it does about CR0/CR4.
;
; ASIN/ACOS have no direct x87 opcode; they're derived from FPATAN
; (which computes a true atan2, not a bare atan) plus a square root:
;   asin(v) = atan2(v, sqrt(1-v^2))
;   acos(v) = atan2(sqrt(1-v^2), v)
; The sqrt(1-v^2) term is computed in SSE2 (mulsd/subsd/sqrtsd, all
; already used elsewhere in this file) since that part needs no x87 at
; all; only the final FPATAN crosses over.
;
; None of these are reentrant (shared scratch qwords, no locking) --
; fine here since BASIX64 programs run single-threaded and
; synchronously, same assumption every other runtime helper in this
; file already makes.
; -------------------------------------------------------------------------
basix_rt_sin:
    push rax
    movsd [rel basix_rt_trig_a], xmm0
    fld qword [rel basix_rt_trig_a]
    fsin
    fstp qword [rel basix_rt_trig_a]
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

basix_rt_cos:
    push rax
    movsd [rel basix_rt_trig_a], xmm0
    fld qword [rel basix_rt_trig_a]
    fcos
    fstp qword [rel basix_rt_trig_a]
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

basix_rt_tan:
    push rax
    movsd [rel basix_rt_trig_a], xmm0
    fld qword [rel basix_rt_trig_a]
    fptan                                ; ST0=1.0 (pushed), ST1=tan(x)
    fstp st0                             ; discard the pushed 1.0
    fstp qword [rel basix_rt_trig_a]     ; store tan(x), pop
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

basix_rt_asin:
    push rax
    movsd [rel basix_rt_trig_a], xmm0    ; a = v (this call's y)
    movsd xmm1, xmm0
    mulsd xmm1, xmm1                     ; xmm1 = v*v
    movsd xmm2, [rel basix_rt_trig_one]
    subsd xmm2, xmm1                     ; xmm2 = 1 - v*v
    sqrtsd xmm2, xmm2                    ; xmm2 = sqrt(1-v*v) = this call's x
    movsd [rel basix_rt_trig_b], xmm2
    fld qword [rel basix_rt_trig_a]      ; ST0 = y = v
    fld qword [rel basix_rt_trig_b]      ; ST0 = x, ST1 = y
    fpatan                               ; ST0 = atan(y/x) = asin(v)
    fstp qword [rel basix_rt_trig_a]
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

basix_rt_acos:
    push rax
    movsd [rel basix_rt_trig_a], xmm0    ; a = v (this call's x)
    movsd xmm1, xmm0
    mulsd xmm1, xmm1
    movsd xmm2, [rel basix_rt_trig_one]
    subsd xmm2, xmm1
    sqrtsd xmm2, xmm2                    ; xmm2 = sqrt(1-v*v) = this call's y
    movsd [rel basix_rt_trig_b], xmm2
    fld qword [rel basix_rt_trig_b]      ; ST0 = y = sqrt(1-v*v)
    fld qword [rel basix_rt_trig_a]      ; ST0 = x = v, ST1 = y
    fpatan                               ; ST0 = atan(y/x) = acos(v)
    fstp qword [rel basix_rt_trig_a]
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

basix_rt_atan:
    push rax
    movsd [rel basix_rt_trig_a], xmm0    ; a = y = x (this call's argument)
    fld qword [rel basix_rt_trig_a]      ; ST0 = y
    fld1                                  ; ST0 = 1.0 = x, ST1 = y
    fpatan                                ; ST0 = atan(y/1) = atan(x)
    fstp qword [rel basix_rt_trig_a]
    movsd xmm0, [rel basix_rt_trig_a]
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_fpu_init: FNINIT, called once at boot before any BASIX64
; program can run -- establishes a sane x87 control/tag word (UEFI
; guarantees CR0/CR4 are already right for SSE2, which is why the rest
; of this kernel's float code needs no init call of its own, but makes
; no equivalent promise about x87 register/control state).
; -------------------------------------------------------------------------
basix_fpu_init:
    fninit
    ret

; -------------------------------------------------------------------------
; basix_rt_timer: no arguments. Returns RAX = timer_ticks, the free-
; running PIT tick counter irq0_stub increments at PIT_HZ (100Hz, see
; pic.inc) -- i.e. hundredths of a second since boot. A BASIX64 program
; reads this to compute elapsed/delta time for animation.
; -------------------------------------------------------------------------
basix_rt_timer:
    mov rax, [rel timer_ticks]
    ret

; -------------------------------------------------------------------------
; basix_rt_screenw / basix_rt_screenh: no arguments. Return RAX = the
; real framebuffer width/height (boot_info's FB_WIDTH/FB_HEIGHT, chosen
; at boot by the bootloader's video-mode picker), so a compiled program
; can size/center itself instead of assuming a fixed resolution. RBX is
; the kernel-wide boot_info* convention (see the note on
; basix_rt_print_int above -- never repurposed as scratch).
; -------------------------------------------------------------------------
basix_rt_screenw:
    mov eax, [rbx+FB_WIDTH]
    ret

basix_rt_screenh:
    mov eax, [rbx+FB_HEIGHT]
    ret

; -------------------------------------------------------------------------
; basix_rt_textcols / basix_rt_textrows: no arguments. Return RAX = the
; text console's width/height in character cells (console_cols/
; console_rows, kernel.asm -- computed at boot from the framebuffer and
; font metrics), distinct from SCREENW/SCREENH's PIXEL dimensions above.
; Lets a program like the text editor lay out status/text/hint rows by
; character cell instead of guessing a font size.
; -------------------------------------------------------------------------
basix_rt_textcols:
    mov eax, [rel console_cols]
    ret

basix_rt_textrows:
    mov eax, [rel console_rows]
    ret

; -------------------------------------------------------------------------
; basix_rt_getkey: no arguments. Returns RAX = the next keystroke's code
; (blocks until one is available -- see kbd_read_char, keyboard.inc):
; printable ASCII (32-126), Backspace (8), Enter (13), or one of the
; KBD_KEY_* virtual codes (0x81-0x88: Left/Right/Up/Down/Home/End/
; Delete/Escape). A compiled program branches on this value directly
; (e.g. `IF k = 8 THEN ...` for backspace) -- BASIX64 has no named
; constant mechanism, so callers hardcode these literals.
; -------------------------------------------------------------------------
basix_rt_getkey:
    call kbd_read_char
    ret

; -------------------------------------------------------------------------
; basix_rt_mousex/mousey/mousebtn: no arguments. Poll the PS/2 mouse
; driver's live state (mouse.inc) -- unlike GETKEY, never blocks, so a
; GUI main loop calls these every frame. mousebtn's bits match
; MOUSE_PKT_LBTN/RBTN/MBTN (0x01/0x02/0x04).
; -------------------------------------------------------------------------
basix_rt_mousex:
    mov eax, [rel mouse_x]
    ret

basix_rt_mousey:
    mov eax, [rel mouse_y]
    ret

basix_rt_mousebtn:
    mov eax, [rel mouse_btn]
    ret

; -------------------------------------------------------------------------
; basix_rt_keyhit: no arguments. Returns 1 if a decoded keystroke is
; waiting (kbd_has_char, keyboard.inc), else 0 -- never blocks, unlike
; GETKEY, so a GUI main loop can check for an exit key without stalling
; mouse polling/redraw. Safe to follow with an unconditional GETKEY once
; this returns 1: that GETKEY is then guaranteed not to block either.
; -------------------------------------------------------------------------
basix_rt_keyhit:
    call kbd_has_char
    ret

; TEMP debug accessors for mouse.inc's bring-up counters -- remove once
; real mouse input is verified working end to end.
basix_rt_mirqcnt:
    mov eax, [rel mouse_irq_count]
    ret
basix_rt_mpktcnt:
    mov eax, [rel mouse_packet_count]
    ret
basix_rt_mdropcnt:
    mov eax, [rel mouse_resync_drops]
    ret

; -------------------------------------------------------------------------
; basix_rt_drawtext: RCX = string ptr, RDX = string length, R8 = color
; (packed 32-bit pixel). x/y come from basix_drawtext_x/y (parser-
; stashed globals -- DRAWTEXT takes four values plus the string, one
; more than fits in RCX/RDX/R8/R9 alongside it, same reasoning as
; RECT). Draws each character's 8x16 glyph (font8x16, the same table
; the text console uses) into the back buffer at pixel coordinates,
; left to right. Unlike fb_draw_char's console-cell text (which paints
; both the glyph's set AND clear pixels, replacing the whole cell),
; this only paints set pixels and leaves everything else alone, so a
; label drawn over a colored background/icon doesn't punch an opaque
; box around itself.
; -------------------------------------------------------------------------
basix_rt_drawtext:
    push rax
    push rbx
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

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov r13, rcx                         ; r13 = string ptr
    mov r14d, edx                        ; r14d = string length
    mov r11d, r8d                        ; r11d = color
    mov r9d, [rel basix_drawtext_x]      ; r9d = glyph x origin (advances)
    mov r10d, [rel basix_drawtext_y]     ; r10d = y origin (constant)

    xor r12d, r12d                       ; r12d = char index
.char_loop:
    cmp r12d, r14d
    jge .out

    movzx eax, byte [r13 + r12]
    sub eax, 32
    cmp eax, 95                          ; 95 printable glyphs (32..126)
    jae .next_char                       ; unprintable -- skip, no box drawn
    shl eax, 4                           ; * 16 bytes per glyph
    lea rsi, [rel font8x16]
    add rsi, rax                         ; rsi = this glyph's 16 scanline bytes

    xor ecx, ecx                         ; ecx = glyph row 0..15
.row_loop:
    cmp ecx, 16
    jge .next_char
    movzx ebx, byte [rsi + rcx]          ; row bits, bit7 = leftmost pixel
    mov edi, r10d
    add edi, ecx                         ; edi = pixel y
    cmp edi, 0
    jl .row_skip
    cmp edi, [rel basix_backbuf_h]
    jge .row_skip

    mov eax, edi
    imul eax, [rel basix_backbuf_w]
    mov r15d, eax                        ; r15d = rowbase = y*stride

    xor edx, edx                         ; edx = column 0..7
.col_loop:
    cmp edx, 8
    jge .row_skip
    test ebx, 0x80
    jz .col_skip
    mov eax, r9d
    add eax, edx                         ; eax = pixel x
    cmp eax, 0
    jl .col_skip
    cmp eax, [rel basix_backbuf_w]
    jge .col_skip
    add eax, r15d                        ; eax = rowbase + x
    mov r8, [rel basix_backbuf_ptr]
    lea r8, [r8 + rax*4]
    mov [r8], r11d
.col_skip:
    shl ebx, 1
    inc edx
    jmp .col_loop

.row_skip:
    inc ecx
    jmp .row_loop

.next_char:
    add r9d, 8                           ; advance to the next glyph column
    inc r12d
    jmp .char_loop

.out:
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
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_print_char: RCX = character code. Prints it as one raw byte
; via console_putc -- unlike PRINT (always decimal digits or a string
; literal), this is how a program renders an int-array-of-char-codes
; text buffer as actual text.
; -------------------------------------------------------------------------
basix_rt_print_char:
    push rax
    movzx eax, cl
    call console_putc
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_rt_locate: RCX = row, RDX = col. Sets the text console's cursor
; position directly, so a program can redraw one line at a time instead
; of only ever continuing linearly the way PRINT does.
; -------------------------------------------------------------------------
basix_rt_locate:
    mov [rel console_row], ecx
    mov [rel console_col], edx
    ret

; -------------------------------------------------------------------------
; basix_rt_peek: RCX = address. Returns the byte at that address (0-255)
; zero-extended in EAX. No bounds checking -- a BASIX64 program can fault
; the kernel with a bad address, same as real PEEK on classic 8-bit BASIC.
; -------------------------------------------------------------------------
basix_rt_peek:
    movzx eax, byte [rcx]
    ret

; -------------------------------------------------------------------------
; basix_rt_poke: RCX = address, RDX = value. Writes value & 0xFF to that
; address. No bounds checking, same caveat as basix_rt_peek.
; -------------------------------------------------------------------------
basix_rt_poke:
    mov [rcx], dl
    ret

; -------------------------------------------------------------------------
; basix_rt_set_text_color: RCX = color as a standard 0x00RRGGBB packed
; value (bits 16-23=R, 8-15=G, 0-7=B), unlike PSET/LINE's raw
; native-pixel-order color args -- text is drawn one glyph at a time via
; fb_draw_char (see fb_text_color), not blended pixel-by-pixel, so there's
; no existing native-order convention to match here, and RRGGBB is what a
; BASIX64 program will naturally write literal color constants as. Reorders
; into the framebuffer's actual byte layout (boot_info's FB_PIXFMT: 0 =
; RGBX8888, 1 = BGRX8888) so PUTCHAR/PRINT text colors come out correct
; regardless of which byte order this GOP mode happens to use.
; -------------------------------------------------------------------------
basix_rt_set_text_color:
    push rax
    push rdx
    push r8
    push r10
    push r11
    push r12

    mov r8d, ecx

    mov eax, r8d
    shr eax, 16
    and eax, 0xFF
    mov r10d, eax                       ; r10d = R

    mov eax, r8d
    shr eax, 8
    and eax, 0xFF
    mov r11d, eax                       ; r11d = G

    mov eax, r8d
    and eax, 0xFF
    mov r12d, eax                       ; r12d = B

    mov edx, [rbx+FB_PIXFMT]
    cmp edx, 1
    je .bgr

    ; RGBX8888: byte0=R, byte1=G, byte2=B -> dword = R | (G<<8) | (B<<16)
    mov eax, r10d
    mov edx, r11d
    shl edx, 8
    or eax, edx
    mov edx, r12d
    shl edx, 16
    or eax, edx
    jmp .store

.bgr:
    ; BGRX8888: byte0=B, byte1=G, byte2=R -> dword = B | (G<<8) | (R<<16)
    mov eax, r12d
    mov edx, r11d
    shl edx, 8
    or eax, edx
    mov edx, r10d
    shl edx, 16
    or eax, edx

.store:
    mov [rel fb_text_color], eax

    pop r12
    pop r11
    pop r10
    pop r8
    pop rdx
    pop rax
    ret

; -------------------------------------------------------------------------
; basix_resolve_dir_path: RCX = ptr to a NUL-terminated path inside
; basix_fsave_name_buf that may contain '/'-separated directory
; components (e.g. "SUB/DEEPER/FILE.TXT"). Mutates the buffer in place,
; overwriting each '/' with a NUL to split it into components, then
; walks every component except the last as a subdirectory -- exactly
; the shell's OPEN, minus the exfat_cwd_stack/exfat_cwd_depth breadcrumb
; bookkeeping OPEN also does for its own UP command, since this is a
; transient lookup scoped to one FSAVE/FLOAD call, not a persistent
; shell navigation. Advances exfat_cwd_cluster one level per component;
; the CALLER is responsible for saving it beforehand and restoring it
; afterward (basix_path_saved_cwd), since this leaves cwd pointing at
; the final directory on success, ready for the file op that follows.
; Returns RAX = pointer to the final component (the filename) on
; success, or 0 if some intermediate component doesn't exist or isn't a
; directory (cwd may have advanced partway before the failure -- still
; the caller's job to restore it).
; -------------------------------------------------------------------------
basix_resolve_dir_path:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8

    mov rbx, rcx                         ; rbx = cursor into the path
.next_component:
    mov rsi, rbx                         ; rsi = start of this component
.scan:
    mov al, [rbx]
    test al, al
    jz .last_component                   ; end of string -- rsi is final
    cmp al, '/'
    je .found_sep
    inc rbx
    jmp .scan
.found_sep:
    mov byte [rbx], 0                    ; split the path here
    inc rbx                              ; rbx -> next component's start

    mov rcx, rsi
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail
    test byte [rel exfat_find_result+32], ATTR_DIRECTORY
    jz .fail
    mov eax, [rel exfat_find_result+0]
    mov [rel exfat_cwd_cluster], eax
    jmp .next_component

.last_component:
    mov rax, rsi
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_rt_fsave: RCX = namearr base ptr (int array, one char code per
; 8-byte element), RDX = namelen, R8 = dataarr base ptr (int array, one
; byte value per 8-byte element), R9 = datalen. BASIX64 has no string
; type, so both arrays arrive this way -- pack each down into a plain
; byte buffer (basix_fsave_name_buf/basix_file_scratch, basix_symbols.inc)
; before handing off to the exfat_* layer, which only speaks bytes. The
; packed name may contain '/' separators (see basix_resolve_dir_path)
; to reach a file in a subdirectory relative to the current directory.
; Overwrite semantics (unlike exfat_write_file's own create-only
; behavior): if a file of this name already exists, delete it first.
; Returns EAX=1/0.
; -------------------------------------------------------------------------
basix_rt_fsave:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                         ; namearr ptr
    mov r13d, edx                        ; namelen
    mov r14, r8                          ; dataarr ptr
    mov r15d, r9d                        ; datalen

    cmp r13d, EXFAT_MAX_NAME_LEN
    jbe .name_len_ok
    mov r13d, EXFAT_MAX_NAME_LEN
.name_len_ok:
    lea rdi, [rel basix_fsave_name_buf]
    xor ecx, ecx
.name_copy:
    cmp ecx, r13d
    jge .name_copy_done
    mov rax, [r12 + rcx*8]
    mov [rdi+rcx], al
    inc ecx
    jmp .name_copy
.name_copy_done:
    mov byte [rdi+rcx], 0                ; NUL-terminate

    mov eax, [rel exfat_cwd_cluster]
    mov [rel basix_path_saved_cwd], eax
    lea rcx, [rel basix_fsave_name_buf]
    call basix_resolve_dir_path
    mov r13, rax                         ; r13 = filename component ptr (0 = bad path)
    test r13, r13
    jz .fail

    mov eax, BASIX_FILE_SCRATCH_SIZE
    cmp r15d, eax
    jbe .data_len_ok
    mov r15d, eax
.data_len_ok:
    mov rdi, [rel basix_file_scratch_ptr]
    xor ecx, ecx
.data_copy:
    cmp ecx, r15d
    jge .data_copy_done
    mov rax, [r14 + rcx*8]
    mov [rdi+rcx], al
    inc ecx
    jmp .data_copy
.data_copy_done:

    mov rcx, r13
    call exfat_delete_file               ; overwrite semantics; result
                                          ; ignored -- fine if it didn't
                                          ; already exist

    mov rcx, r13
    mov r8, [rel basix_file_scratch_ptr]
    mov r9d, r15d
    call exfat_write_file
    mov r15d, eax                        ; save result across the cwd restore

    mov eax, [rel basix_path_saved_cwd]
    mov [rel exfat_cwd_cluster], eax
    mov eax, r15d
    jmp .out

.fail:
    mov eax, [rel basix_path_saved_cwd]
    mov [rel exfat_cwd_cluster], eax
    xor eax, eax

.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_rt_fload: RCX = namearr base ptr, RDX = namelen, R8 = dataarr
; base ptr, R9 = maxlen. Looks the file up (path may contain '/'
; separators, see basix_resolve_dir_path), reads up to maxlen bytes of
; its content into basix_file_scratch, then expands each byte back out
; into one 8-byte int-array element (the reverse of basix_rt_fsave's
; packing). Returns EAX = bytes read (0 if not found or on failure).
; -------------------------------------------------------------------------
basix_rt_fload:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                         ; namearr ptr
    mov r13d, edx                        ; namelen
    mov r14, r8                          ; dataarr ptr
    mov r15d, r9d                        ; maxlen

    cmp r13d, EXFAT_MAX_NAME_LEN
    jbe .name_len_ok
    mov r13d, EXFAT_MAX_NAME_LEN
.name_len_ok:
    lea rdi, [rel basix_fsave_name_buf]
    xor ecx, ecx
.name_copy:
    cmp ecx, r13d
    jge .name_copy_done
    mov rax, [r12 + rcx*8]
    mov [rdi+rcx], al
    inc ecx
    jmp .name_copy
.name_copy_done:
    mov byte [rdi+rcx], 0

    mov eax, [rel exfat_cwd_cluster]
    mov [rel basix_path_saved_cwd], eax
    lea rcx, [rel basix_fsave_name_buf]
    call basix_resolve_dir_path
    mov r13, rax                         ; r13 = filename component ptr (0 = bad path)
    test r13, r13
    jz .fail

    mov rcx, r13
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail

    mov eax, BASIX_FILE_SCRATCH_SIZE
    cmp r15d, eax
    jbe .maxlen_ok
    mov r15d, eax
.maxlen_ok:
    mov rax, [rel exfat_find_result+8]   ; DataLength
    cmp rax, r15
    jbe .len_ok
    mov rax, r15
.len_ok:
    mov r15, rax                         ; r15 = actual bytes to read

    mov ecx, [rel exfat_find_result+0]   ; FirstCluster
    mov rdx, r15                         ; bytes to read
    mov r8, [rel basix_file_scratch_ptr]
    mov r9d, [rel exfat_find_result+16]  ; NoFatChain
    call exfat_read_file
    test eax, eax
    jz .fail

    mov rsi, [rel basix_file_scratch_ptr]
    xor ecx, ecx
.data_expand:
    cmp rcx, r15
    jge .done
    movzx eax, byte [rsi+rcx]
    mov [r14 + rcx*8], rax
    inc rcx
    jmp .data_expand
.done:
    mov rbx, r15                          ; save result across the cwd restore
    mov eax, [rel basix_path_saved_cwd]
    mov [rel exfat_cwd_cluster], eax
    mov rax, rbx
    jmp .out
.fail:
    mov eax, [rel basix_path_saved_cwd]
    mov [rel exfat_cwd_cluster], eax
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

; -------------------------------------------------------------------------
; basix_rt_wait: RCX = number of PIT ticks (hundredths of a second) to
; wait. Busy-spins (PAUSE-throttled) until timer_ticks has advanced by
; at least that many -- the same counter basix_rt_timer reads -- so a
; BASIX64 program can pace an animation loop to a fixed frame rate
; without a real sleep/yield primitive. Fine for a single-threaded
; compiled program; not reentrant-safe to call concurrently, same
; caveat as the trig helpers above.
; -------------------------------------------------------------------------
basix_rt_wait:
    push rax
    push rdx

    mov rax, [rel timer_ticks]
    add rax, rcx                         ; rax = target tick count
.spin:
    pause
    mov rdx, [rel timer_ticks]
    cmp rdx, rax
    jl .spin

    pop rdx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
basix_rt_digit_buf: times 20 db 0

align 8
basix_rt_ten: dq 10.0
basix_rt_frac_buf: times 6 db 0

align 8
basix_rt_trig_a:   dq 0
basix_rt_trig_b:   dq 0
basix_rt_trig_one: dq 1.0
basix_rt_half:     dq 0.5

; Back buffer (basix_rt_ensure_backbuf/basix_rt_flip): a kmalloc'd,
; tightly-packed copy of the frame PSET/LINE/CLS draw into. Zero
; means "not yet allocated" -- every drawing routine lazily inits it
; on first use.
align 8
basix_backbuf_ptr: dq 0
basix_backbuf_w:   dd 0
basix_backbuf_h:   dd 0

; LINE's endpoints (basix_parse_line stores these via
; basix_parse_float_arg_store; basix_rt_line_aa reads them).
align 8
basix_line_x1f: dq 0
basix_line_y1f: dq 0
basix_line_x2f: dq 0
basix_line_y2f: dq 0

; basix_rt_line_aa's (Wu's algorithm) working state -- not reentrant,
; same caveat as the trig helpers above (fine: BASIX64 programs run
; single-threaded and synchronously).
align 8
basix_wu_x0:       dq 0
basix_wu_y0:       dq 0
basix_wu_x1:       dq 0
basix_wu_y1:       dq 0
basix_wu_dx:       dq 0
basix_wu_dy:       dq 0
basix_wu_gradient: dq 0
basix_wu_intery:   dq 0
basix_wu_xend:     dq 0
basix_wu_yend:     dq 0
basix_wu_xgap:     dq 0
basix_wu_t:        dq 0
basix_wu_t2:       dq 0
basix_wu_cov_a:    dq 0
basix_wu_cov_b:    dq 0
basix_wu_steep:    dd 0
basix_wu_color:    dd 0
basix_wu_xpxl1:    dd 0
basix_wu_ypxl1:    dd 0
basix_wu_xpxl2:    dd 0
basix_wu_ypxl2:    dd 0
basix_wu_loopx:    dd 0

; TRIFILL's vertices (basix_parse_trifill stores them; basix_rt_fill_tri
; reads them) and its own working state.
align 8
basix_tri_x0f:   dq 0
basix_tri_y0f:   dq 0
basix_tri_x1f:   dq 0
basix_tri_y1f:   dq 0
basix_tri_x2f:   dq 0
basix_tri_y2f:   dq 0
basix_tri_area:  dq 0
basix_tri_a0:    dq 0
basix_tri_b0:    dq 0
basix_tri_c0:    dq 0
basix_tri_a1:    dq 0
basix_tri_b1:    dq 0
basix_tri_c1:    dq 0
basix_tri_a2:    dq 0
basix_tri_b2:    dq 0
basix_tri_c2:    dq 0
basix_tri_minx:  dd 0
basix_tri_maxx:  dd 0
basix_tri_miny:  dd 0
basix_tri_maxy:  dd 0
basix_tri_px:    dd 0
basix_tri_py:    dd 0
basix_tri_color: dd 0

; RECT's x/y/w/h (basix_parse_rect stores them via
; basix_parse_int_arg_store; basix_rt_rect reads them) -- RECT takes
; five int arguments, one more than fit in RCX/RDX/R8/R9 alongside the
; trailing color, same reasoning as TRIFILL's vertices above.
align 8
basix_rect_x: dq 0
basix_rect_y: dq 0
basix_rect_w: dq 0
basix_rect_h: dq 0

; DRAWTEXT's x/y (basix_parse_drawtext stores them; basix_rt_drawtext
; reads them).
basix_drawtext_x: dq 0
basix_drawtext_y: dq 0

; -------------------------------------------------------------------------
; LAUNCH's byte buffers (basix_rt_launch fills both from the int-array-
; of-char-codes arguments it's given) and the stashed argument
; ARGLEN/ARGCHAR read. basix_launch_arg_len is 0 whenever the current
; program was started via RUN rather than LAUNCH -- never reset
; mid-program, so it stays valid for that program's whole lifetime.
; -------------------------------------------------------------------------
BASIX_LAUNCH_NAME_BUF_SIZE equ 128
BASIX_LAUNCH_ARG_BUF_SIZE  equ 256

basix_launch_name_buf: times BASIX_LAUNCH_NAME_BUF_SIZE db 0
basix_launch_arg_buf:  times BASIX_LAUNCH_ARG_BUF_SIZE db 0
basix_launch_arg_len:  dd 0

; -------------------------------------------------------------------------
; basix_rt_launch: RCX = program-name array base ptr (int, char codes),
; RDX = name length, R8 = argument array base ptr (int, char codes),
; R9 = argument length. Converts both int-array-of-char-codes arguments
; into plain byte buffers (BASIX64 has no string type -- see the
; comment on basix_launch_name_buf/basix_launch_arg_buf), stashes the
; argument for the launched program's ARGLEN/ARGCHAR to read, then
; loads the target exactly like RUN (basix_load_program: resolves the
; path, deep paths included, and magic-sniffs for a precompiled .axb).
;
; On success this NEVER RETURNS to its caller: it discards its own
; return address and jumps straight into the freshly loaded
; basix_code_buf, so the launched program's own trailing RET pops the
; *original* top-level invocation's return address (the shell's `call
; basix_code_buf` for RUN, or an outer LAUNCH's) instead of unwinding
; back into the code that just called LAUNCH -- which basix_load_program
; has already overwritten (basix_code_buf is one shared, fixed buffer;
; see basix_codegen.inc's top comment). That overwrite is safe because
; by the time execution ever runs from basix_code_buf again, this
; function -- which lives in the kernel's own separate code, never in
; basix_code_buf -- has already finished writing it. On failure (bad
; path/compile error/stale .axb) it returns normally instead, and the
; calling program continues past the LAUNCH statement untouched.
; -------------------------------------------------------------------------
basix_rt_launch:
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                        ; namearr ptr
    mov r13d, edx                       ; namelen
    mov r14, r8                         ; argarr ptr
    mov r15d, r9d                       ; arglen

    cmp r13d, BASIX_LAUNCH_NAME_BUF_SIZE - 1
    jbe .namelen_ok
    mov r13d, BASIX_LAUNCH_NAME_BUF_SIZE - 1
.namelen_ok:
    lea rdi, [rel basix_launch_name_buf]
    xor esi, esi
.name_copy:
    cmp esi, r13d
    jge .name_done
    mov rax, [r12 + rsi*8]
    mov [rdi + rsi], al
    inc esi
    jmp .name_copy
.name_done:
    mov byte [rdi + rsi], 0

    cmp r15d, BASIX_LAUNCH_ARG_BUF_SIZE
    jbe .arglen_ok
    mov r15d, BASIX_LAUNCH_ARG_BUF_SIZE
.arglen_ok:
    lea rdi, [rel basix_launch_arg_buf]
    xor esi, esi
.arg_copy:
    cmp esi, r15d
    jge .arg_done
    mov rax, [r14 + rsi*8]
    mov [rdi + rsi], al
    inc esi
    jmp .arg_copy
.arg_done:
    mov [rel basix_launch_arg_len], r15d

    lea rcx, [rel basix_launch_name_buf]
    call basix_load_program
    test eax, eax
    jnz .fail

    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rax                             ; discard our own return address --
                                         ; see the header comment above:
                                         ; this never returns to its caller
    jmp basix_code_buf

.fail:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    ret

; -------------------------------------------------------------------------
; basix_rt_arglen: no arguments. Returns the length (in bytes) of the
; argument the current program was LAUNCHed with, or 0 if it wasn't
; (e.g. started via RUN).
; -------------------------------------------------------------------------
basix_rt_arglen:
    mov eax, [rel basix_launch_arg_len]
    ret

; -------------------------------------------------------------------------
; basix_rt_argchar: RCX = index. Returns the index'th byte (as an int
; char code) of the current LAUNCH argument, or 0 if out of range.
; -------------------------------------------------------------------------
basix_rt_argchar:
    cmp ecx, [rel basix_launch_arg_len]
    jae .oob
    lea rax, [rel basix_launch_arg_buf]
    movzx eax, byte [rax+rcx]
    ret
.oob:
    xor eax, eax
    ret
