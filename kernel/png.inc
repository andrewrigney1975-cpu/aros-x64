; =============================================================================
; Minimal PNG decoder for BASIX64's viewer.bas: enough DEFLATE (RFC 1951)
; and PNG (RFC 2083) to load an 8-bit-depth, non-interlaced PNG (grayscale,
; RGB, palette, grayscale+alpha, or RGBA) into a flat native-pixel-order
; buffer DRAWPNG can blit. No 16-bit-depth or Adam7-interlaced support --
; LOADPNG just fails (returns 0) on anything outside that subset.
;
; The Huffman construct()/decode() pair below is a direct port of Mark
; Adler's "puff.c" reference inflate (public domain) -- its count/first/
; index canonical-Huffman algorithm is the standard minimal approach, and
; porting a known-correct reference beats inventing another one from
; scratch for something this easy to get subtly wrong.
; =============================================================================

PNG_MAX_W equ 2048
PNG_MAX_H equ 2048
PNG_FILE_MAX  equ 8388608                        ; 8MB: raw file bytes
PNG_IDAT_MAX  equ 8388608                        ; 8MB: concatenated IDAT stream
PNG_INFL_MAX  equ (PNG_MAX_W*4+1)*PNG_MAX_H      ; filtered scanlines (worst case RGBA)
PNG_PIXELS_MAX equ PNG_MAX_W*PNG_MAX_H*4          ; final native-pixel buffer

PNG_MAXBITS equ 15

; -------------------------------------------------------------------------
; Bit-reader state (png_bit1/png_bits_getn) and inflate output-cursor
; state (png_out_*), both reset at the top of every png_inflate call.
; -------------------------------------------------------------------------
align 8
png_bits_ptr:   dq 0
png_bits_end:   dq 0
png_bit_buf:    dd 0
png_bit_count:  dd 0
png_out_ptr:    dq 0
png_out_end:    dq 0
png_out_base:   dq 0

; Huffman tree scratch -- count[len]/symbol[] pairs per puff.c's
; construct()/decode(), one set per tree currently in use. combined_len
; holds the dynamic block's decoded HLIT+HDIST code-length array (split
; by offset, not copied, when building the two real trees from it).
align 8
png_huff_offs:      times 16 dd 0
png_clc_lengths:    times 19 dd 0
png_clc_count:      times 16 dd 0
png_clc_symbol:     times 19 dd 0
png_combined_len:   times 320 dd 0
png_litlen_count:   times 16 dd 0
png_litlen_symbol:  times 288 dd 0
png_dist_count:     times 16 dd 0
png_dist_symbol:    times 32 dd 0
png_fixed_lit_len:  times 288 dd 0
png_fixed_dist_len: times 30 dd 0

; The tree currently feeding png_decode_block's symbol loop -- pointed at
; either the fixed tables above or the freshly-constructed dynamic ones.
align 8
png_cur_lit_count:   dq 0
png_cur_lit_symbol:  dq 0
png_cur_dist_count:  dq 0
png_cur_dist_symbol: dq 0

clc_order: dd 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15

length_base:  dd 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
length_extra: dd 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
dist_base:    dd 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
dist_extra:   dd 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13

; -------------------------------------------------------------------------
; PNG-level state: heap buffers (allocated once at boot, see
; png_heap_init, called from basix_heap_init), decoded image
; dimensions/format, and a pointer/length into the raw file buffer for
; PLTE (only valid during one decode call).
; -------------------------------------------------------------------------
align 8
png_file_buf_ptr: dq 0
png_idat_buf_ptr: dq 0
png_infl_buf_ptr: dq 0
png_pixels_ptr:   dq 0
png_width:  dd 0
png_height: dd 0
png_bitdepth: dd 0
png_colortype: dd 0
png_plte_ptr: dq 0
png_plte_len: dd 0
png_debug_code: dd 0   ; TEMP: which .fail site tripped, for bring-up debugging
png_debug_inflate_len: dd 0  ; TEMP: actual png_inflate byte count, never overwritten
png_debug_abc: times 4 dd 0   ; TEMP: captured a,b,c,predictor for one probe pixel
png_debug_gate: dd 0          ; TEMP: scratch flag for the above (avoids clobbering a live reg)
png_debug_gate2: dd 0         ; TEMP: second scratch flag, dedicated/unused elsewhere

; -------------------------------------------------------------------------
; png_heap_init: called once from basix_heap_init (kernel boot). Heap-
; allocates every PNG buffer via kmalloc_large -- same reasoning as
; basix_array_arena/basix_file_scratch: ~40MB of literal zero bytes would
; blow out KERNEL.BIN's size and break the bootloader's fixed-address
; AllocatePages call.
; -------------------------------------------------------------------------
png_heap_init:
    push rcx
    mov rcx, PNG_FILE_MAX
    call kmalloc_large
    mov [rel png_file_buf_ptr], rax

    mov rcx, PNG_IDAT_MAX
    call kmalloc_large
    mov [rel png_idat_buf_ptr], rax

    mov rcx, PNG_INFL_MAX
    call kmalloc_large
    mov [rel png_infl_buf_ptr], rax

    mov rcx, PNG_PIXELS_MAX
    call kmalloc_large
    mov [rel png_pixels_ptr], rax
    pop rcx
    ret

; =============================================================================
; Bit reader
; =============================================================================

; -------------------------------------------------------------------------
; png_bit1: no args. Returns EAX = next bit (0/1) from the DEFLATE stream,
; consumed LSB-first within each byte (per RFC 1951). Pads with 0 bits
; past png_bits_end rather than faulting -- a malformed/truncated stream
; just decodes garbage, it doesn't crash the kernel.
; -------------------------------------------------------------------------
png_bit1:
    push rbx
    push rdx
    push rsi

    mov edx, [rel png_bit_count]
    test edx, edx
    jnz .have

    mov rsi, [rel png_bits_ptr]
    cmp rsi, [rel png_bits_end]
    jae .pad
    movzx ebx, byte [rsi]
    inc rsi
    mov [rel png_bits_ptr], rsi
    mov [rel png_bit_buf], ebx
    mov dword [rel png_bit_count], 8
    jmp .have
.pad:
    mov dword [rel png_bit_buf], 0
    mov dword [rel png_bit_count], 8
.have:
    mov eax, [rel png_bit_buf]
    and eax, 1
    mov ebx, [rel png_bit_buf]
    shr ebx, 1
    mov [rel png_bit_buf], ebx
    dec dword [rel png_bit_count]

    pop rsi
    pop rdx
    pop rbx
    ret

; -------------------------------------------------------------------------
; png_bits_getn: ECX = n (0..24). Returns EAX = an n-bit value assembled
; LSB-first (bit read first -> bit 0 of the result) -- the convention
; RFC 1951 uses for every multi-bit field except Huffman codes
; themselves. Built on png_bit1 one bit at a time (simple over fast;
; images here are small enough that this never matters).
; -------------------------------------------------------------------------
png_bits_getn:
    push rbx
    push rcx
    push rdx

    xor ebx, ebx
    xor edx, edx
.loop:
    cmp edx, ecx
    jge .done
    push rcx
    push rdx
    call png_bit1
    pop rdx
    pop rcx
    push rcx
    mov ecx, edx
    shl eax, cl
    pop rcx
    or ebx, eax
    inc edx
    jmp .loop
.done:
    mov eax, ebx

    pop rdx
    pop rcx
    pop rbx
    ret

; =============================================================================
; Canonical Huffman: construct() builds count[]/symbol[] from a per-symbol
; code-length array; decode() reads bits one at a time against them. Both
; ported from puff.c (Mark Adler, public domain).
; =============================================================================

; -------------------------------------------------------------------------
; png_construct: RCX = length[] (dwords, one per symbol, 0 = unused),
; RDX = n (symbol count), R8 = count[] out (16 dwords), R9 = symbol[] out
; (n dwords). Uses png_huff_offs as scratch. Doesn't return a validity
; code (puff.c's "left") -- we trust the encoder to emit well-formed
; trees rather than hardening against malformed ones.
; -------------------------------------------------------------------------
png_construct:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r10
    push r11
    push r12
    push r13

    mov r10, rcx
    mov r11d, edx
    mov r12, r8
    mov r13, r9

    xor eax, eax
    mov rdi, r12
    mov ecx, 16
    cld
    rep stosd

    xor esi, esi
.count_loop:
    cmp esi, r11d
    jge .count_done
    mov eax, [r10 + rsi*4]
    inc dword [r12 + rax*4]
    inc esi
    jmp .count_loop
.count_done:

    lea rbx, [rel png_huff_offs]
    mov dword [rbx + 4], 0
    mov ecx, 1
.offs_loop:
    cmp ecx, 14
    jg .offs_done
    mov eax, [rbx + rcx*4]
    mov edx, [r12 + rcx*4]
    add eax, edx
    mov [rbx + rcx*4 + 4], eax
    inc ecx
    jmp .offs_loop
.offs_done:

    xor esi, esi
.assign_loop:
    cmp esi, r11d
    jge .assign_done
    mov eax, [r10 + rsi*4]
    test eax, eax
    jz .assign_next
    mov edx, [rbx + rax*4]
    mov [r13 + rdx*4], esi
    inc edx
    mov [rbx + rax*4], edx
.assign_next:
    inc esi
    jmp .assign_loop
.assign_done:

    pop r13
    pop r12
    pop r11
    pop r10
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_decode: RCX = count[] (16 dwords), RDX = symbol[]. Returns
; EAX = decoded symbol, or -1 if no code matched within PNG_MAXBITS bits
; (malformed stream).
; -------------------------------------------------------------------------
png_decode:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14

    mov r12, rcx
    mov r13, rdx
    xor ebx, ebx
    xor esi, esi
    xor edi, edi
    mov r14d, 1
.loop:
    cmp r14d, PNG_MAXBITS
    jg .error

    push r12
    push r13
    push r14
    call png_bit1
    pop r14
    pop r13
    pop r12

    or ebx, eax
    mov edx, [r12 + r14*4]
    mov eax, ebx
    sub eax, esi
    cmp eax, edx
    jge .not_yet
    add eax, edi
    mov eax, [r13 + rax*4]
    jmp .out
.not_yet:
    add edi, edx
    add esi, edx
    shl esi, 1
    shl ebx, 1
    inc r14d
    jmp .loop
.error:
    mov eax, -1
.out:
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

; =============================================================================
; DEFLATE (RFC 1951)
; =============================================================================

; -------------------------------------------------------------------------
; png_build_fixed_tables: fills png_fixed_lit_len[288]/png_fixed_dist_len
; [30] with the RFC-1951-mandated fixed code lengths (symbols 0-143 -> 8,
; 144-255 -> 9, 256-279 -> 7, 280-287 -> 8; all 30 distance symbols -> 5).
; -------------------------------------------------------------------------
png_build_fixed_tables:
    push rax
    push rcx

    xor ecx, ecx
.lit_loop:
    cmp ecx, 288
    jge .lit_done
    mov eax, 8
    cmp ecx, 144
    jl .lit_store
    mov eax, 9
    cmp ecx, 256
    jl .lit_store
    mov eax, 7
    cmp ecx, 280
    jl .lit_store
    mov eax, 8
.lit_store:
    mov [rel png_fixed_lit_len + rcx*4], eax
    inc ecx
    jmp .lit_loop
.lit_done:

    xor ecx, ecx
.dist_loop:
    cmp ecx, 30
    jge .dist_done
    mov dword [rel png_fixed_dist_len + rcx*4], 5
    inc ecx
    jmp .dist_loop
.dist_done:

    pop rcx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_read_dynamic_header: reads a dynamic block's HLIT/HDIST/HCLEN
; header and code-length-code lengths, builds the code-length Huffman
; tree, decodes the HLIT+HDIST length array with it (handling the 16/17/
; 18 repeat symbols), and builds the final literal/length and distance
; trees from the result -- leaving them in png_litlen_count/symbol and
; png_dist_count/symbol. No return value; bails silently (leaving
; whatever's already in those tables) on a malformed header, since a
; well-formed PNG encoder never produces one.
; -------------------------------------------------------------------------
png_read_dynamic_header:
    push rax
    push rbx
    push rcx
    push rdx
    push r12
    push r13
    push r14

    mov ecx, 5
    call png_bits_getn
    add eax, 257
    mov r12d, eax                       ; r12d = HLIT+257 (nlit)

    mov ecx, 5
    call png_bits_getn
    inc eax
    mov r13d, eax                       ; r13d = HDIST+1 (ndist)

    mov ecx, 4
    call png_bits_getn
    add eax, 4
    mov r14d, eax                       ; r14d = HCLEN+4 (nclen)


    ; zero all 19 code-length-code lengths, then fill the first nclen of
    ; them (in clc_order permutation) from the stream
    xor ecx, ecx
.clc_zero:
    cmp ecx, 19
    jge .clc_zero_done
    mov dword [rel png_clc_lengths + rcx*4], 0
    inc ecx
    jmp .clc_zero
.clc_zero_done:

    xor ebx, ebx
.clc_read:
    cmp ebx, r14d
    jge .clc_read_done
    mov ecx, 3
    call png_bits_getn
    mov edx, [rel clc_order + rbx*4]
    mov [rel png_clc_lengths + rdx*4], eax
    inc ebx
    jmp .clc_read
.clc_read_done:

    lea rcx, [rel png_clc_lengths]
    mov edx, 19
    lea r8, [rel png_clc_count]
    lea r9, [rel png_clc_symbol]
    call png_construct

    ; decode nlit+ndist code lengths into png_combined_len, handling the
    ; 16 (repeat previous 3-6x) / 17 (repeat 0 3-10x) / 18 (repeat 0
    ; 11-138x) meta-symbols
    mov ebx, r12d
    add ebx, r13d                       ; ebx = total symbols to decode
    xor r14d, r14d                      ; r14d = write index
.dec_loop:
    cmp r14d, ebx
    jge .dec_done

    lea rcx, [rel png_clc_count]
    lea rdx, [rel png_clc_symbol]
    call png_decode                     ; eax = symbol (0..18)

    cmp eax, 16
    jge .dec_repeat

    mov [rel png_combined_len + r14*4], eax
    inc r14d
    jmp .dec_loop

.dec_repeat:
    cmp eax, 16
    jne .dec_17
    mov ecx, 2
    call png_bits_getn
    add eax, 3                          ; repeat count
    mov edx, [rel png_combined_len + r14*4 - 4]  ; previous length
    jmp .dec_fill
.dec_17:
    cmp eax, 17
    jne .dec_18
    mov ecx, 3
    call png_bits_getn
    add eax, 3
    xor edx, edx
    jmp .dec_fill
.dec_18:
    mov ecx, 7
    call png_bits_getn
    add eax, 11
    xor edx, edx
.dec_fill:
    mov ecx, eax                        ; ecx = repeat count
.dec_fill_loop:
    test ecx, ecx
    jz .dec_loop
    cmp r14d, ebx
    jge .dec_loop
    mov [rel png_combined_len + r14*4], edx
    inc r14d
    dec ecx
    jmp .dec_fill_loop

.dec_done:
    lea rcx, [rel png_combined_len]
    mov edx, r12d
    lea r8, [rel png_litlen_count]
    lea r9, [rel png_litlen_symbol]
    call png_construct


    lea rcx, [rel png_combined_len]
    mov eax, r12d
    lea rcx, [rcx + rax*4]
    mov edx, r13d
    lea r8, [rel png_dist_count]
    lea r9, [rel png_dist_symbol]
    call png_construct

    pop r14
    pop r13
    pop r12
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_out_byte: AL = byte to append to the inflate output. No-ops past
; png_out_end (truncates rather than overflowing the buffer).
; -------------------------------------------------------------------------
png_out_byte:
    push rax
    push rbx
    mov rbx, [rel png_out_ptr]
    cmp rbx, [rel png_out_end]
    jae .out
    mov [rbx], al
    inc rbx
    mov [rel png_out_ptr], rbx
.out:
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_decode_block: decodes one Huffman-coded symbol stream (fixed or
; dynamic -- caller has already pointed png_cur_lit_count/symbol and
; png_cur_dist_count/symbol at the right tables) until an end-of-block
; symbol (256).
; -------------------------------------------------------------------------
png_decode_block:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

.loop:
    mov rcx, [rel png_cur_lit_count]
    mov rdx, [rel png_cur_lit_symbol]
    call png_decode
    mov ebx, eax

    ; TEMP: capture the first symbol decoded, plus a sanity-check re-read
    ; of litlen_symbol[0..1] via the SAME pointers decode_block itself
    ; uses, to rule out png_cur_lit_symbol pointing somewhere unexpected
    cmp dword [rel png_debug_gate], 0
    jne .no_dbg4
    mov [rel png_debug_abc+0], ebx
    mov rcx, [rel png_cur_lit_symbol]
    mov eax, [rcx+0]
    mov [rel png_debug_abc+4], eax
    mov eax, [rcx+4]
    mov [rel png_debug_abc+8], eax
    mov dword [rel png_debug_gate], 1
.no_dbg4:

    cmp ebx, 256
    jl .literal
    je .eob
    jg .length

.literal:
    mov eax, ebx
    call png_out_byte

    ; TEMP: right after the FIRST literal write, check what's actually
    ; sitting at the buffer's first byte (should be the value just
    ; written, i.e. 1)
    cmp dword [rel png_debug_gate2], 0
    jne .no_dbg5
    mov rax, [rel png_out_base]
    movzx eax, byte [rax]
    mov [rel png_debug_abc+12], eax
    mov dword [rel png_debug_gate2], 1
.no_dbg5:

    jmp .loop

.eob:
    jmp .out

.length:
    mov esi, ebx
    sub esi, 257                        ; esi = length-code index (0..28)
    mov eax, [rel length_base + rsi*4]
    mov edi, eax                        ; edi = base length
    mov ecx, [rel length_extra + rsi*4]
    test ecx, ecx
    jz .have_length
    call png_bits_getn
    add edi, eax
.have_length:
    ; edi = final match length

    mov rcx, [rel png_cur_dist_count]
    mov rdx, [rel png_cur_dist_symbol]
    call png_decode
    mov esi, eax                        ; esi = distance-code index (0..29)
    mov eax, [rel dist_base + rsi*4]
    mov ebx, eax                        ; ebx = base distance
    mov ecx, [rel dist_extra + rsi*4]
    test ecx, ecx
    jz .have_dist
    call png_bits_getn
    add ebx, eax
.have_dist:
    ; ebx = final match distance, edi = match length -- copy byte by
    ; byte (never as a block copy: distance can be smaller than length,
    ; meaning the source range overlaps/extends into bytes this very
    ; copy is writing, which is how DEFLATE represents runs)
    mov rsi, [rel png_out_ptr]
    sub rsi, rbx                        ; source = out_ptr - distance
.copy_loop:
    test edi, edi
    jz .loop
    movzx eax, byte [rsi]
    call png_out_byte
    inc rsi
    dec edi
    jmp .copy_loop

.out:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_inflate: RCX = compressed data ptr (a zlib stream's DEFLATE payload,
; i.e. past the 2-byte zlib header), RDX = its length, R8 = output buffer,
; R9 = output buffer max size. Returns EAX = bytes written.
; -------------------------------------------------------------------------
png_inflate:
    push rbx
    push rcx
    push rdx
    push r12
    push r13

    mov r12, r8                         ; output base
    mov r13, r9                         ; output max

    mov [rel png_bits_ptr], rcx
    add rdx, rcx
    mov [rel png_bits_end], rdx
    mov dword [rel png_bit_buf], 0
    mov dword [rel png_bit_count], 0

    mov [rel png_out_ptr], r12
    mov [rel png_out_base], r12
    lea rax, [r12 + r13]
    mov [rel png_out_end], rax

.block_loop:
    mov ecx, 1
    call png_bits_getn
    mov ebx, eax                        ; ebx = BFINAL

    mov ecx, 2
    call png_bits_getn                  ; eax = BTYPE


    cmp eax, 0
    je .stored
    cmp eax, 1
    je .fixed
    cmp eax, 2
    je .dynamic
    jmp .done                           ; BTYPE==3: reserved/invalid, bail

.stored:
    mov dword [rel png_bit_buf], 0
    mov dword [rel png_bit_count], 0
    mov rcx, [rel png_bits_ptr]
    movzx eax, byte [rcx]
    movzx edx, byte [rcx+1]
    shl edx, 8
    or eax, edx                         ; eax = LEN (NLEN at [rcx+2..3] ignored)
    add rcx, 4
    mov [rel png_bits_ptr], rcx
.stored_copy:
    test eax, eax
    jz .stored_done
    movzx edx, byte [rcx]
    push rax
    mov al, dl
    call png_out_byte
    pop rax
    inc rcx
    dec eax
    jmp .stored_copy
.stored_done:
    mov [rel png_bits_ptr], rcx
    jmp .block_done

.fixed:
    call png_build_fixed_tables
    lea rcx, [rel png_fixed_lit_len]
    mov edx, 288
    lea r8, [rel png_litlen_count]
    lea r9, [rel png_litlen_symbol]
    call png_construct
    lea rcx, [rel png_fixed_dist_len]
    mov edx, 30
    lea r8, [rel png_dist_count]
    lea r9, [rel png_dist_symbol]
    call png_construct
    jmp .use_tables

.dynamic:
    call png_read_dynamic_header

.use_tables:
    lea rax, [rel png_litlen_count]
    mov [rel png_cur_lit_count], rax
    lea rax, [rel png_litlen_symbol]
    mov [rel png_cur_lit_symbol], rax
    lea rax, [rel png_dist_count]
    mov [rel png_cur_dist_count], rax
    lea rax, [rel png_dist_symbol]
    mov [rel png_cur_dist_symbol], rax
    call png_decode_block

.block_done:
    test ebx, ebx
    jnz .done
    jmp .block_loop

.done:
    ; TEMP: capture the final byte[0] value once inflate is fully done
    push rax
    movzx eax, byte [r12]
    mov [rel png_debug_abc+8], eax
    pop rax

    mov rax, [rel png_out_ptr]
    sub rax, r12

    pop r13
    pop r12
    pop rdx
    pop rcx
    pop rbx
    ret

; =============================================================================
; PNG container: chunk parsing, scanline defiltering, pixel conversion
; =============================================================================

; -------------------------------------------------------------------------
; png_paeth: RCX=a, RDX=b, R8=c (all byte values, zero/sign-extended into
; full registers). Returns EAX = the Paeth predictor of the three.
; -------------------------------------------------------------------------
png_paeth:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

    mov eax, ecx
    add eax, edx
    sub eax, r8d                        ; p = a+b-c
    mov ebx, eax
    sub ebx, ecx                        ; pa = p-a
    test ebx, ebx
    jns .pa_pos
    neg ebx
.pa_pos:
    mov esi, eax
    sub esi, edx                        ; pb = p-b
    test esi, esi
    jns .pb_pos
    neg esi
.pb_pos:
    mov edi, eax
    sub edi, r8d                        ; pc = p-c
    test edi, edi
    jns .pc_pos
    neg edi
.pc_pos:
    cmp ebx, esi
    jg .not_a
    cmp ebx, edi
    jg .not_a
    mov eax, ecx
    jmp .out
.not_a:
    cmp esi, edi
    jg .use_c
    mov eax, edx
    jmp .out
.use_c:
    mov eax, r8d
.out:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

; -------------------------------------------------------------------------
; png_unfilter: RCX = filtered-scanlines buffer (in place), EDX = width,
; R8D = height, R9D = bpp (bytes/pixel -- 1/2/3/4 per colortype, bitdepth
; 8 only). Each scanline is [1 filter-type byte][width*bpp data bytes];
; reconstructs every scanline's real pixel bytes in place, left to right,
; top to bottom, per RFC 2083 section 6.
; -------------------------------------------------------------------------
png_unfilter:
    push rax
    push rbx
    push rbp
    push rcx
    push rdx
    push rsi
    push rdi
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r10, rcx                        ; buffer base
    mov r11d, edx                       ; width
    mov r12d, r8d                       ; height
    mov r13d, r9d                       ; bpp

    mov eax, r11d
    imul eax, r13d
    mov r14d, eax                       ; rowbytes
    lea r15d, [eax + 1]                 ; stride (1 filter byte + rowbytes)

    ; (filter-byte capture removed this round -- reusing the slots for
    ; BFINAL/BTYPE/nlit/ndist in png_inflate/png_read_dynamic_header
    ; instead, which run earlier and would otherwise get overwritten)

    ; y lives in ebp, NOT ebx -- ebx gets clobbered every column as the
    ; filt(x) accumulator (see below), so the outer row counter can't
    ; share it or "inc ebx" at the end of a row would increment the last
    ; column's accumulator instead of the actual row index.
    xor ebp, ebp                        ; ebp = row index y
.row_loop:
    cmp ebp, r12d
    jge .row_done

    mov eax, ebp
    imul eax, r15d
    lea rdi, [r10 + rax]                ; rdi = this row's base (filter byte)
    movzx esi, byte [rdi]               ; esi = filter type
    inc rdi                             ; rdi = this row's data start

    ; (TEMP per-row filter-type capture removed -- reusing the slots
    ; above for width/bpp/rowbytes/stride instead this round)

    test ebp, ebp
    jz .no_prior
    mov rcx, rdi
    sub rcx, r15                        ; rcx = prior row's data start
    jmp .have_prior
.no_prior:
    xor ecx, ecx                        ; 0 = "no prior row" sentinel
.have_prior:

    xor edx, edx                        ; edx = x (byte offset within row)
.col_loop:
    cmp edx, r14d
    jge .row_next

    ; a = (x>=bpp) ? data[x-bpp] : 0
    xor eax, eax
    cmp edx, r13d
    jl .a_zero
    mov r8d, edx
    sub r8d, r13d
    movzx eax, byte [rdi + r8]
.a_zero:
    push rax                            ; save a

    ; b = prior ? prior[x] : 0
    xor eax, eax
    test rcx, rcx
    jz .b_zero
    movzx eax, byte [rcx + rdx]
.b_zero:
    push rax                            ; save b

    ; c = (prior && x>=bpp) ? prior[x-bpp] : 0
    xor eax, eax
    test rcx, rcx
    jz .c_zero
    cmp edx, r13d
    jl .c_zero
    mov r8d, edx
    sub r8d, r13d
    movzx eax, byte [rcx + r8]
.c_zero:
    mov r9d, eax                        ; r9d = c

    pop rax                             ; b (into eax for the moment)
    mov r8d, eax                        ; r8d = b
    pop rax                             ; a
    ; now: eax=a, r8d=b, r9d=c

    movzx ebx, byte [rdi + rdx]         ; ebx = filt(x), pure per-column
                                         ; scratch here -- y is in ebp
    cmp esi, 0
    je .f_none
    cmp esi, 1
    je .f_sub
    cmp esi, 2
    je .f_up
    cmp esi, 3
    je .f_avg
    ; filter 4: Paeth
    push rcx                            ; save prior-row pointer (png_paeth
                                         ; clobbers rcx with its own copy of
                                         ; the 'a' parameter, not ours)
    push rdx
    push rsi
    push rdi
    mov ecx, eax
    mov edx, r8d
    mov r8d, r9d
    call png_paeth
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    add ebx, eax
    jmp .store
.f_none:
    jmp .store
.f_sub:
    add ebx, eax
    jmp .store
.f_up:
    add ebx, r8d
    jmp .store
.f_avg:
    add eax, r8d
    shr eax, 1
    add ebx, eax
.store:
    mov [rdi + rdx], bl

    inc edx
    jmp .col_loop

.row_next:
    inc ebp
    jmp .row_loop

.row_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbp
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; png_pack_pixel: RCX=R, RDX=G, R8=B (byte values). Returns EAX = the
; 32-bit pixel packed into the framebuffer's actual native byte order
; (boot_info's FB_PIXFMT) -- same RGB/BGR conversion as
; basix_rt_set_text_color, reused here for image pixels.
; -------------------------------------------------------------------------
png_pack_pixel:
    push rdx
    push r8
    push r9

    ; TEMP: branch bodies deliberately swapped vs. the "obvious" RGB/BGR
    ; formula -- empirically confirmed via pixel-exact screendump
    ; sampling (see conversation) that the straightforward mapping came
    ; out R/B-swapped on this hardware/QEMU config, so this is the
    ; measured-correct assignment, not the theoretically-derived one.
    mov r9d, [rbx+FB_PIXFMT]
    cmp r9d, 1
    je .bgr

    mov eax, r8d
    mov r9d, edx
    shl r9d, 8
    or eax, r9d
    mov r9d, ecx
    shl r9d, 16
    or eax, r9d
    jmp .out
.bgr:
    mov eax, ecx
    mov r9d, edx
    shl r9d, 8
    or eax, r9d
    mov r9d, r8d
    shl r9d, 16
    or eax, r9d
.out:
    pop r9
    pop r8
    pop rdx
    ret

; -------------------------------------------------------------------------
; png_convert_pixels: converts the unfiltered scanline buffer
; (png_infl_buf_ptr, per png_width/png_height/png_colortype) into
; png_pixels_ptr (one native-order 32-bit pixel per source pixel, no
; alpha compositing -- DRAWPNG paints fully opaque). Palette lookups use
; png_plte_ptr/png_plte_len (colortype 3 only).
; -------------------------------------------------------------------------
png_convert_pixels:
    push rax
    push rbp
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

    mov r10d, [rel png_width]
    mov r11d, [rel png_height]
    mov r12d, [rel png_colortype]

    xor r13d, r13d                      ; channels, set below
    cmp r12d, 0
    jne .ct2
    mov r13d, 1
    jmp .ct_done
.ct2:
    cmp r12d, 2
    jne .ct3
    mov r13d, 3
    jmp .ct_done
.ct3:
    cmp r12d, 3
    jne .ct4
    mov r13d, 1
    jmp .ct_done
.ct4:
    cmp r12d, 4
    jne .ct6
    mov r13d, 2
    jmp .ct_done
.ct6:
    mov r13d, 4
.ct_done:

    mov eax, r10d
    imul eax, r13d
    mov r14d, eax                       ; rowbytes
    lea r15d, [eax + 1]                 ; stride

    mov rsi, [rel png_infl_buf_ptr]
    mov rdi, [rel png_pixels_ptr]

    xor ebp, ebp                        ; y
.row_loop:
    cmp ebp, r11d
    jge .done

    mov eax, ebp
    imul eax, r15d
    lea r9, [rsi + rax + 1]             ; r9 = this row's data (skip filter byte)

    ; x lives in r13d for the rest of this row -- channels (r13d's value
    ; up to here) is no longer needed once rowbytes/stride are computed,
    ; and R/G/B need rcx/rdx/r8 (png_pack_pixel's own argument
    ; registers), so x can't live in any of those either.
    xor r13d, r13d
.col_loop:
    cmp r13d, r10d
    jge .row_next

    cmp r12d, 0
    je .px_gray
    cmp r12d, 2
    je .px_rgb
    cmp r12d, 3
    je .px_pal
    cmp r12d, 4
    je .px_gray_alpha
    jmp .px_rgba

.px_gray:
    mov eax, r13d
    movzx ecx, byte [r9 + rax]
    mov rdx, rcx
    mov r8, rcx
    jmp .have_rgb
.px_gray_alpha:
    mov eax, r13d
    shl eax, 1
    movzx ecx, byte [r9 + rax]
    mov rdx, rcx
    mov r8, rcx
    jmp .have_rgb
.px_rgb:
    mov eax, r13d
    lea eax, [eax*2 + eax]              ; *3
    movzx ecx, byte [r9 + rax]
    movzx edx, byte [r9 + rax + 1]
    movzx r8, byte [r9 + rax + 2]
    jmp .have_rgb
.px_rgba:
    mov eax, r13d
    shl eax, 2
    movzx ecx, byte [r9 + rax]
    movzx edx, byte [r9 + rax + 1]
    movzx r8, byte [r9 + rax + 2]
    jmp .have_rgb
.px_pal:
    mov eax, r13d
    movzx eax, byte [r9 + rax]          ; palette index
    lea eax, [eax*2 + eax]              ; *3
    mov r8, [rel png_plte_ptr]
    movzx ecx, byte [r8 + rax]
    movzx edx, byte [r8 + rax + 1]
    movzx r8, byte [r8 + rax + 2]
.have_rgb:
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call png_pack_pixel
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9

    mov ecx, ebp
    imul ecx, r10d
    add ecx, r13d
    mov [rdi + rcx*4], eax

    inc r13d
    jmp .col_loop

.row_next:
    inc ebp
    jmp .row_loop

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
    pop rbp
    pop rax
    ret

; -------------------------------------------------------------------------
; png_be32: RCX = ptr to 4 big-endian bytes. Returns EAX = the value.
; -------------------------------------------------------------------------
png_be32:
    push rdx
    movzx eax, byte [rcx]
    movzx edx, byte [rcx+1]
    shl eax, 8
    or eax, edx
    movzx edx, byte [rcx+2]
    shl eax, 8
    or eax, edx
    movzx edx, byte [rcx+3]
    shl eax, 8
    or eax, edx
    pop rdx
    ret

; -------------------------------------------------------------------------
; png_decode_file: RCX = raw file bytes ptr, RDX = length. Parses the PNG
; container (signature + chunks), concatenates IDAT data, inflates it,
; defilters the scanlines, and converts to png_pixels_ptr. Returns
; EAX = 1 on success, 0 on any failure (bad signature, unsupported
; bit depth/interlace/colortype, dimensions over PNG_MAX_W/H, or a
; malformed chunk stream).
; -------------------------------------------------------------------------
png_decode_file:
    push rbx
    push rbp
    push rcx
    push rdx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                        ; file base
    mov r13, rdx                        ; file length
    lea r14, [r12 + r13]                ; file end

    mov dword [rel png_debug_code], 1
    cmp r13, 8
    jb .fail
    cmp byte [r12+0], 0x89
    jne .fail
    cmp byte [r12+1], 'P'
    jne .fail
    cmp byte [r12+2], 'N'
    jne .fail
    cmp byte [r12+3], 'G'
    jne .fail

    lea rsi, [r12 + 8]                  ; rsi = chunk cursor
    xor r15d, r15d                      ; r15d = bytes written into idat buf so far
    mov qword [rel png_plte_ptr], 0
    mov dword [rel png_plte_len], 0
    mov dword [rel png_width], 0

.chunk_loop:
    mov dword [rel png_debug_code], 2
    lea rax, [rsi + 8]
    cmp rax, r14
    ja .fail                            ; not even room for length+type

    mov rcx, rsi
    call png_be32
    mov ebx, eax                        ; ebx = chunk data length

    mov eax, [rsi+4]                    ; chunk type (4 ASCII bytes, LE dword)

    lea rdi, [rsi + 8]                  ; rdi = chunk data start
    mov rcx, rdi
    add rcx, rbx
    add rcx, 4                          ; + CRC
    mov dword [rel png_debug_code], 3
    cmp rcx, r14
    ja .fail                            ; chunk claims to run past EOF

    cmp eax, 'IHDR'
    jne .not_ihdr
    mov dword [rel png_debug_code], 4
    cmp ebx, 13
    jne .fail
    mov rcx, rdi
    call png_be32
    mov [rel png_width], eax
    lea rcx, [rdi+4]
    call png_be32
    mov [rel png_height], eax
    movzx eax, byte [rdi+8]
    mov [rel png_bitdepth], eax
    movzx eax, byte [rdi+9]
    mov [rel png_colortype], eax
    ; compression(10)/filter(11) must be 0, interlace(12) must be 0
    mov dword [rel png_debug_code], 5
    cmp byte [rdi+10], 0
    jne .fail
    mov dword [rel png_debug_code], 6
    cmp byte [rdi+11], 0
    jne .fail
    mov dword [rel png_debug_code], 7
    cmp byte [rdi+12], 0
    jne .fail
    mov dword [rel png_debug_code], 8
    cmp dword [rel png_bitdepth], 8
    jne .fail
    mov dword [rel png_debug_code], 9
    mov eax, [rel png_width]
    cmp eax, 0
    jle .fail
    cmp eax, PNG_MAX_W
    ja .fail
    mov dword [rel png_debug_code], 10
    mov eax, [rel png_height]
    cmp eax, 0
    jle .fail
    cmp eax, PNG_MAX_H
    ja .fail
    jmp .chunk_next

.not_ihdr:
    cmp eax, 'PLTE'
    jne .not_plte
    mov [rel png_plte_ptr], rdi
    mov [rel png_plte_len], ebx
    jmp .chunk_next

.not_plte:
    cmp eax, 'IDAT'
    jne .not_idat
    mov eax, r15d
    add eax, ebx
    mov dword [rel png_debug_code], 14
    cmp eax, PNG_IDAT_MAX
    ja .fail
    mov rdx, [rel png_idat_buf_ptr]
    add rdx, r15
    ; memcpy(rdx, rdi, ebx)
    push rcx
    push rsi
    push rdi
    mov rsi, rdi
    mov rdi, rdx
    mov ecx, ebx
    cld
    rep movsb
    pop rdi
    pop rsi
    pop rcx
    add r15d, ebx
    jmp .chunk_next

.not_idat:
    cmp eax, 'IEND'
    jne .chunk_next
    jmp .have_idat

.chunk_next:
    lea rsi, [rdi + rbx + 4]            ; skip data + CRC
    jmp .chunk_loop

.have_idat:
    mov dword [rel png_debug_code], 11
    cmp dword [rel png_width], 0
    je .fail
    mov dword [rel png_debug_code], 12
    test r15d, r15d
    jz .fail

    ; zlib stream: 2-byte header, then the DEFLATE payload; ignore the
    ; trailing 4-byte Adler32 (png_inflate stops at the final block
    ; regardless of what follows)
    mov dword [rel png_debug_code], 13
    cmp r15d, 2
    jb .fail
    mov rcx, [rel png_idat_buf_ptr]
    add rcx, 2
    mov edx, r15d
    sub edx, 2
    mov r8, [rel png_infl_buf_ptr]
    mov r9, PNG_INFL_MAX
    call png_inflate
    mov [rel png_debug_code], eax       ; TEMP: also doubles as inflate byte count on failure
    mov [rel png_debug_inflate_len], eax
    test eax, eax
    jz .fail

    ; NOTE: deliberately NOT using ebx here (unlike the chunk-parsing
    ; loop above) -- png_unfilter is fine either way, but
    ; png_convert_pixels calls png_pack_pixel, which reads [rbx+
    ; FB_PIXFMT] trusting the project-wide "rbx == boot_info*" global
    ; convention; clobbering rbx here (even though it gets restored by
    ; this function's own pop rbx at the very end) would corrupt that
    ; convention for the whole call chain those two calls make.
    mov ebp, [rel png_colortype]
    xor ecx, ecx
    cmp ebp, 0
    jne .bpp2
    mov ecx, 1
    jmp .bpp_done
.bpp2:
    cmp ebp, 2
    jne .bpp3
    mov ecx, 3
    jmp .bpp_done
.bpp3:
    cmp ebp, 3
    jne .bpp4
    mov ecx, 1
    jmp .bpp_done
.bpp4:
    cmp ebp, 4
    jne .bpp6
    mov ecx, 2
    jmp .bpp_done
.bpp6:
    cmp ebp, 6
    jne .fail
    mov ecx, 4
.bpp_done:
    mov dword [rel png_debug_code], 999  ; reached bpp_done successfully

    mov dword [rel png_debug_code], 5001
    mov rax, [rel png_infl_buf_ptr]
    mov edx, [rel png_width]
    mov r8d, [rel png_height]
    mov r9d, ecx
    mov rcx, rax
    call png_unfilter
    mov dword [rel png_debug_code], 5002

    call png_convert_pixels
    mov dword [rel png_debug_code], 5003

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbp
    pop rbx
    ret

; =============================================================================
; BASIX64 entry points
; =============================================================================

; -------------------------------------------------------------------------
; basix_rt_loadpng: RCX = namearr base ptr, RDX = namelen. Looks the file
; up on exFAT exactly like basix_rt_fload (path may contain '/'
; separators), reads it into png_file_buf_ptr, then decodes it. Returns
; EAX = 1/0.
; -------------------------------------------------------------------------
basix_rt_loadpng:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                        ; namearr ptr
    mov r13d, edx                       ; namelen

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
    mov r14, rax
    test r14, r14
    jz .fail

    mov rcx, r14
    lea r8, [rel exfat_find_result]
    call exfat_find_root_file
    test eax, eax
    jz .fail

    mov rax, [rel exfat_find_result+8]  ; DataLength
    mov r15, PNG_FILE_MAX
    cmp rax, r15
    jbe .len_ok
    mov rax, r15
.len_ok:
    mov r15, rax                        ; r15 = bytes to read

    mov ecx, [rel exfat_find_result+0]  ; FirstCluster
    mov rdx, r15
    mov r8, [rel png_file_buf_ptr]
    mov r9d, [rel exfat_find_result+16] ; NoFatChain
    call exfat_read_file
    test eax, eax
    jz .fail

    mov rcx, [rel png_file_buf_ptr]
    mov rdx, r15                        ; bytes actually in the buffer --
                                         ; exfat_read_file's EAX is a
                                         ; plain 1/0 success flag, not a
                                         ; byte count
    call png_decode_file
    test eax, eax
    jz .fail

    mov eax, [rel basix_path_saved_cwd]
    mov [rel exfat_cwd_cluster], eax
    mov eax, 1
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
; basix_rt_drawpng: RCX = x, RDX = y. Blits the most recently LOADPNG'd
; image into the graphics back buffer at (x, y), clipped to the back
; buffer's own bounds -- pixels (and whole rows) outside it are simply
; skipped, giving the "clip, don't scroll" behavior viewer.bas wants.
; -------------------------------------------------------------------------
basix_rt_drawpng:
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

    call basix_rt_ensure_backbuf
    cmp qword [rel basix_backbuf_ptr], 0
    je .out

    mov r12d, [rel png_width]
    mov r13d, [rel png_height]
    test r12d, r12d
    jz .out
    test r13d, r13d
    jz .out

    mov r10d, ecx                       ; x0
    mov r11d, edx                       ; y0

    xor esi, esi                        ; row y
.row_loop:
    cmp esi, r13d
    jge .out

    mov eax, r11d
    add eax, esi
    test eax, eax
    js .row_next
    cmp eax, [rel basix_backbuf_h]
    jge .row_next
    mov r8d, eax                        ; r8d = dest row

    xor edi, edi                        ; col x
.col_loop:
    cmp edi, r12d
    jge .row_next

    mov eax, r10d
    add eax, edi
    test eax, eax
    js .col_next
    cmp eax, [rel basix_backbuf_w]
    jge .col_next
    mov r9d, eax                        ; r9d = dest col

    mov eax, esi
    imul eax, r12d
    add eax, edi
    mov rbx, [rel png_pixels_ptr]
    mov eax, [rbx + rax*4]              ; source pixel

    mov ecx, r8d
    imul ecx, [rel basix_backbuf_w]
    add ecx, r9d
    mov rbx, [rel basix_backbuf_ptr]
    mov [rbx + rcx*4], eax

.col_next:
    inc edi
    jmp .col_loop

.row_next:
    inc esi
    jmp .row_loop

.out:
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
; basix_rt_png_width / basix_rt_png_height: no args. Return the most
; recently LOADPNG'd image's dimensions (0/0 if nothing's loaded yet).
; -------------------------------------------------------------------------
basix_rt_png_width:
    mov eax, [rel png_width]
    ret

basix_rt_png_height:
    mov eax, [rel png_height]
    ret

; TEMP debug helpers, bring-up only -- remove once LOADPNG is verified.
basix_rt_png_debug:
    mov eax, [rel png_debug_code]
    ret

basix_rt_png_debug2:
    mov eax, [rel png_debug_inflate_len]
    ret
