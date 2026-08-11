; =============================================================================
; PS/2 keyboard driver (IRQ1, scancode set 1, US QWERTY).
;
; Scope: make/break codes for the standard alphanumeric block, shift and
; caps lock, plus the 0xE0-prefixed extended set: arrow keys, Home/End/
; Delete/Insert/PgUp/PgDn, and right-Ctrl/right-Alt (tracked as modifier
; state for future use -- nothing consumes them yet). Since the buffer
; kbd_read_char returns from is just a byte stream, arrows/Home/End/etc.
; are encoded as values in the 0x80-0x8F range (KBD_KEY_* below) that
; can never collide with a real decoded ASCII character (the printable
; tables only ever produce 0x20-0x7E plus backspace/tab/enter). Decoded
; bytes land in a small circular buffer that kbd_read_char blocks on (via
; HLT, so the CPU is genuinely idle between keystrokes, not spinning).
; =============================================================================

KBD_BUF_SIZE equ 256

; Virtual key codes for keys with no ASCII representation, pushed into
; the same byte stream as ordinary characters (see the header comment).
KBD_KEY_LEFT  equ 0x81
KBD_KEY_RIGHT equ 0x82
KBD_KEY_UP    equ 0x83
KBD_KEY_DOWN  equ 0x84
KBD_KEY_HOME  equ 0x85
KBD_KEY_END   equ 0x86
KBD_KEY_DELETE equ 0x87
KBD_KEY_ESCAPE equ 0x88

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
    jne .ext_byte

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
    cmp bl, 0x01
    je .escape_down

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
.escape_down:
    mov al, KBD_KEY_ESCAPE
    call kbd_buf_push
    jmp .eoi

.ext_byte:
    in al, 0x60
    mov bl, al
    mov byte [rel kbd_pending_ext], 0

    test bl, 0x80
    jnz .ext_release

    cmp bl, 0x4B
    je .ext_left
    cmp bl, 0x4D
    je .ext_right
    cmp bl, 0x48
    je .ext_up
    cmp bl, 0x50
    je .ext_down
    cmp bl, 0x47
    je .ext_home
    cmp bl, 0x4F
    je .ext_end
    cmp bl, 0x53
    je .ext_delete
    cmp bl, 0x1D
    je .ext_rctrl_down
    cmp bl, 0x38
    je .ext_ralt_down
    jmp .eoi                            ; unhandled extended key: ignore

.ext_left:
    mov al, KBD_KEY_LEFT
    jmp .ext_push
.ext_right:
    mov al, KBD_KEY_RIGHT
    jmp .ext_push
.ext_up:
    mov al, KBD_KEY_UP
    jmp .ext_push
.ext_down:
    mov al, KBD_KEY_DOWN
    jmp .ext_push
.ext_home:
    mov al, KBD_KEY_HOME
    jmp .ext_push
.ext_end:
    mov al, KBD_KEY_END
    jmp .ext_push
.ext_delete:
    mov al, KBD_KEY_DELETE
.ext_push:
    call kbd_buf_push
    jmp .eoi

.ext_rctrl_down:
    mov byte [rel kbd_rctrl_active], 1
    jmp .eoi
.ext_ralt_down:
    mov byte [rel kbd_ralt_active], 1
    jmp .eoi

.ext_release:
    and bl, 0x7F
    cmp bl, 0x1D
    je .ext_rctrl_up
    cmp bl, 0x38
    je .ext_ralt_up
    jmp .eoi
.ext_rctrl_up:
    mov byte [rel kbd_rctrl_active], 0
    jmp .eoi
.ext_ralt_up:
    mov byte [rel kbd_ralt_active], 0

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
kbd_rctrl_active:     db 0
kbd_ralt_active:      db 0

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
