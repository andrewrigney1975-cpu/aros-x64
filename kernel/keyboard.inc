; =============================================================================
; PS/2 keyboard driver (IRQ1, scancode set 1, US QWERTY).
;
; Scope: make/break codes for the standard alphanumeric block, shift and
; caps lock. The 0xE0 extended-key prefix (arrows, right-Ctrl/Alt, etc.)
; is recognized and its following byte is swallowed so it can't be
; misread as a base-set code, but extended keys themselves produce no
; character yet -- future work. Decoded ASCII lands in a small circular
; buffer that kbd_read_char blocks on (via HLT, so the CPU is genuinely
; idle between keystrokes, not spinning).
; =============================================================================

KBD_BUF_SIZE equ 256

; -------------------------------------------------------------------------
; kbd_irq_stub: IRQ1 handler. Reads the scancode, updates shift/caps
; state or decodes+buffers a character, then EOIs.
; -------------------------------------------------------------------------
kbd_irq_stub:
    push rax
    push rbx
    push rcx
    push rdx

    cmp byte [rel kbd_pending_ext], 0
    jne .swallow_ext_byte

    in al, 0x60
    mov bl, al

    cmp bl, 0xE0
    jne .not_ext
    mov byte [rel kbd_pending_ext], 1
    jmp .eoi
.not_ext:

    test bl, 0x80
    jnz .release

    cmp bl, 0x2A
    je .shift_down
    cmp bl, 0x36
    je .shift_down
    cmp bl, 0x3A
    je .capslock_toggle

    movzx ecx, bl
    cmp ecx, 0x80
    jae .eoi
    cmp byte [rel kbd_shift_active], 0
    je .use_unshifted
    lea rdx, [rel kbd_table_shifted]
    jmp .have_table
.use_unshifted:
    lea rdx, [rel kbd_table_unshifted]
.have_table:
    movzx eax, byte [rdx+rcx]
    test al, al
    jz .eoi

    cmp byte [rel kbd_capslock_active], 0
    je .push_char
    cmp al, 'a'
    jb .check_upper
    cmp al, 'z'
    ja .push_char
    sub al, 0x20                        ; caps: lower -> upper
    jmp .push_char
.check_upper:
    cmp al, 'A'
    jb .push_char
    cmp al, 'Z'
    ja .push_char
    add al, 0x20                        ; caps+shift: upper -> lower
.push_char:
    call kbd_buf_push
    jmp .eoi

.release:
    and bl, 0x7F
    cmp bl, 0x2A
    je .shift_up
    cmp bl, 0x36
    je .shift_up
    jmp .eoi

.shift_down:
    mov byte [rel kbd_shift_active], 1
    jmp .eoi
.shift_up:
    mov byte [rel kbd_shift_active], 0
    jmp .eoi
.capslock_toggle:
    xor byte [rel kbd_capslock_active], 1
    jmp .eoi

.swallow_ext_byte:
    in al, 0x60                         ; discard the byte following 0xE0
    mov byte [rel kbd_pending_ext], 0

.eoi:
    mov al, 0x20
    out 0x20, al

    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; -------------------------------------------------------------------------
; kbd_buf_push: AL = character to enqueue. Silently drops it if the
; buffer is full (a stuck/unread console is not worth crashing over).
; -------------------------------------------------------------------------
kbd_buf_push:
    push rbx
    push rcx
    push rdx

    movzx edx, byte [rel kbd_buf_head]
    mov bl, dl
    inc bl
    cmp bl, [rel kbd_buf_tail]
    je .full
    lea rcx, [rel kbd_buf]
    mov [rcx+rdx], al
    mov [rel kbd_buf_head], bl
.full:
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; kbd_has_char: returns EAX=1 if a decoded character is waiting, else 0.
; -------------------------------------------------------------------------
kbd_has_char:
    movzx eax, byte [rel kbd_buf_tail]
    cmp al, [rel kbd_buf_head]
    je .none
    mov eax, 1
    ret
.none:
    xor eax, eax
    ret

; -------------------------------------------------------------------------
; kbd_read_char: blocks (via HLT) until a character is available, then
; returns it in AL (zero-extended into EAX).
; -------------------------------------------------------------------------
kbd_read_char:
    push rbx
    push rcx
.wait:
    movzx ecx, byte [rel kbd_buf_tail]
    cmp cl, [rel kbd_buf_head]
    jne .have_char
    hlt
    jmp .wait
.have_char:
    lea rbx, [rel kbd_buf]
    movzx eax, byte [rbx+rcx]
    inc cl
    mov [rel kbd_buf_tail], cl
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; kbd_install: registers the IRQ1 handler and makes sure IRQ0 (timer),
; IRQ1 (keyboard) and IRQ2 (master->slave cascade) are unmasked on the
; master PIC, without assuming what pic_remap happened to leave there.
; -------------------------------------------------------------------------
kbd_install:
    push rax
    push rcx
    push rdx

    mov rcx, 33
    lea rdx, [rel kbd_irq_stub]
    call idt_set_gate

    in al, 0x21
    and al, 0xF8
    out 0x21, al

    pop rdx
    pop rcx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
kbd_shift_active:    db 0
kbd_capslock_active: db 0
kbd_pending_ext:      db 0

kbd_buf_head: db 0
kbd_buf_tail: db 0

; Scancode Set 1, unshifted. Index = make-code scancode (0x00-0x7F).
; Zero entries are keys with no printable ASCII (modifiers, F-keys, etc.)
; or are simply not covered by this v1 driver.
kbd_table_unshifted:
    db 0,    0,    '1',  '2',  '3',  '4',  '5',  '6'    ; 0x00-0x07
    db '7',  '8',  '9',  '0',  '-',  '=',  8,    9      ; 0x08-0x0F
    db 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i'    ; 0x10-0x17
    db 'o',  'p',  '[',  ']',  13,   0,    'a',  's'    ; 0x18-0x1F
    db 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';'    ; 0x20-0x27
    db 39,   '`',  0,    '\',  'z',  'x',  'c',  'v'    ; 0x28-0x2F
    db 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*'    ; 0x30-0x37
    db 0,    ' ',  0,    0,    0,    0,    0,    0      ; 0x38-0x3F
    times 64 db 0                                        ; 0x40-0x7F

kbd_table_shifted:
    db 0,    0,    '!',  '@',  '#',  '$',  '%',  '^'    ; 0x00-0x07
    db '&',  '*',  '(',  ')',  '_',  '+',  8,    9      ; 0x08-0x0F
    db 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I'    ; 0x10-0x17
    db 'O',  'P',  '{',  '}',  13,   0,    'A',  'S'    ; 0x18-0x1F
    db 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':'    ; 0x20-0x27
    db '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V'    ; 0x28-0x2F
    db 'B',  'N',  'M',  '<',  '>',  '?',  0,    '*'    ; 0x30-0x37
    db 0,    ' ',  0,    0,    0,    0,    0,    0      ; 0x38-0x3F
    times 64 db 0                                        ; 0x40-0x7F

align 4
kbd_buf: times KBD_BUF_SIZE db 0
