; =============================================================================
; Minimal NVMe driver: bring up a controller's admin queue, create one I/O
; queue pair, and read LBA 0. Same scope note as ahci.inc -- this proves the
; kernel can drive an NVMe controller directly; general multi-sector I/O and
; real namespace/LBA-format discovery (via IDENTIFY) are follow-up work once
; the exFAT layer needs them. We assume NSID=1 and 512-byte logical blocks,
; which matches an unconfigured QEMU `-device nvme` namespace.
;
; All buffers are statically reserved and page-aligned; "physical address"
; and "virtual address" are still the same number here (see the note at the
; top of ahci.inc about riding the firmware's identity map).
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

NVME_ADMIN_QSIZE equ 16                 ; entries
NVME_IO_QSIZE    equ 8                  ; entries

; -------------------------------------------------------------------------
; nvme_do_cmd: submit a pre-built 64-byte SQE to a queue pair and wait for
; its completion.
;   RCX = pointer to the 64-byte SQE
;   RDX = pointer to a queue-pair struct (48 bytes):
;         +0  sq_base (dq)   +8  cq_base (dq)
;         +16 sqdb (dq)      +24 cqdb (dq)      (doorbell MMIO addresses)
;         +32 sq_tail (dd)   +36 cq_head (dd)
;         +40 cq_phase (dd)  +44 qsize (dd)
; Returns EAX=1 on success, EAX=0 on a timeout or non-zero completion status.
; -------------------------------------------------------------------------
nvme_do_cmd:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10

    mov r10, rdx                        ; r10 = qpair
    mov rsi, rcx                        ; rsi = source SQE

    mov r8d, [r10+32]                   ; sq_tail
    mov rdi, [r10+0]                    ; sq_base
    mov eax, r8d
    shl eax, 6                          ; * 64 bytes/entry
    add rdi, rax

    mov ecx, 8
    cld
    rep movsq                           ; copy 64-byte SQE into the slot

    mov r9d, [r10+44]                   ; qsize
    inc r8d
    cmp r8d, r9d
    jl .no_wrap_sq
    xor r8d, r8d
.no_wrap_sq:
    mov [r10+32], r8d
    mov rbx, [r10+16]                   ; sqdb
    mov [rbx], r8d

    mov r8d, [r10+36]                   ; cq_head
    mov rdi, [r10+8]                    ; cq_base
    mov eax, r8d
    shl eax, 4                          ; * 16 bytes/entry
    add rdi, rax

    mov edx, [r10+40]                   ; expected cq_phase
    mov r9d, 0x7FFFFFFF
.poll:
    mov eax, [rdi+12]                   ; completion entry DW3
    mov ebx, eax
    shr ebx, 16
    and ebx, 1                          ; phase tag
    cmp ebx, edx
    je .got
    dec r9d
    jnz .poll
    xor eax, eax                        ; timed out
    jmp .out

.got:
    mov eax, [rdi+12]
    mov ebx, eax
    shr ebx, 17
    and ebx, 0x7FFF                     ; status field (0 = success)

    mov r9d, [r10+44]                   ; qsize
    inc r8d
    cmp r8d, r9d
    jl .no_wrap_cq
    xor r8d, r8d
    xor dword [r10+40], 1               ; wrapped: toggle expected phase
.no_wrap_cq:
    mov [r10+36], r8d
    mov rdx, [r10+24]                   ; cqdb
    mov [rdx], r8d

    test ebx, ebx
    jnz .fail
    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
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
; nvme_init: RCX = controller MMIO base (from BAR0/BAR1). Resets the
; controller if it was already enabled, programs the admin queue, and
; enables it. Returns EAX=1 on success, EAX=0 on failure/timeout.
; -------------------------------------------------------------------------
nvme_init:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9

    mov [rel nvme_base], rcx
    mov rbx, rcx

    mov eax, [rbx+0x04]                 ; CAP[63:32]
    and eax, 0x0F                       ; DSTRD (bits 32:35 of CAP)
    mov ecx, eax
    mov eax, 4
    shl eax, cl                         ; stride = 4 << DSTRD bytes
    mov [rel nvme_dbstride], eax

    mov eax, [rbx+0x1C]                 ; CSTS
    test al, 1
    jz .not_ready
    mov eax, [rbx+0x14]                 ; CC
    and eax, ~1
    mov [rbx+0x14], eax
    mov r9d, 20000000
.wait_disable:
    mov eax, [rbx+0x1C]
    test al, 1
    jz .not_ready
    dec r9d
    jnz .wait_disable
    xor eax, eax
    jmp .out
.not_ready:

    mov eax, (NVME_ADMIN_QSIZE-1) | ((NVME_ADMIN_QSIZE-1) << 16)
    mov [rbx+0x24], eax                 ; AQA

    lea rax, [rel nvme_asq]
    mov [rbx+0x28], rax                 ; ASQ
    lea rax, [rel nvme_acq]
    mov [rbx+0x30], rax                 ; ACQ

    mov eax, (6 << 16) | (4 << 20) | 1  ; IOSQES=6,IOCQES=4,MPS=0,CSS=0,EN=1
    mov [rbx+0x14], eax                 ; CC

    mov r9d, 20000000
.wait_ready:
    mov eax, [rbx+0x1C]
    test al, 1
    jnz .ready
    dec r9d
    jnz .wait_ready
    xor eax, eax
    jmp .out
.ready:
    lea rax, [rel nvme_asq]
    mov [rel nvme_admin_qpair+0], rax
    lea rax, [rel nvme_acq]
    mov [rel nvme_admin_qpair+8], rax
    mov rax, rbx
    add rax, 0x1000
    mov [rel nvme_admin_qpair+16], rax  ; SQ0TDBL
    mov ecx, [rel nvme_dbstride]
    add rax, rcx
    mov [rel nvme_admin_qpair+24], rax  ; CQ0HDBL
    mov dword [rel nvme_admin_qpair+32], 0
    mov dword [rel nvme_admin_qpair+36], 0
    mov dword [rel nvme_admin_qpair+40], 1
    mov dword [rel nvme_admin_qpair+44], NVME_ADMIN_QSIZE

    mov eax, 1
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
; nvme_create_io_queues: creates I/O Completion Queue 1 then I/O Submission
; Queue 1 (CQ must exist before the SQ that references it) via admin
; commands, and populates nvme_io_qpair. Returns EAX=1/0.
; -------------------------------------------------------------------------
nvme_create_io_queues:
    push rcx
    push rdx
    push rdi

    lea rdi, [rel nvme_sqe_scratch]
    push rdi
    xor eax, eax
    mov ecx, 8
    cld
    rep stosq
    pop rdi

    mov dword [rdi+0], 0x00000005       ; CDW0: opcode = CREATE_IO_CQ
    lea rax, [rel nvme_iocq]
    mov [rdi+24], rax                   ; PRP1
    mov eax, 1 | ((NVME_IO_QSIZE-1) << 16)
    mov [rdi+40], eax                   ; CDW10: QID=1, QSIZE-1
    mov dword [rdi+44], 1               ; CDW11: PC=1, IEN=0

    lea rcx, [rel nvme_sqe_scratch]
    lea rdx, [rel nvme_admin_qpair]
    call nvme_do_cmd
    test eax, eax
    jz .fail

    lea rdi, [rel nvme_sqe_scratch]
    push rdi
    xor eax, eax
    mov ecx, 8
    cld
    rep stosq
    pop rdi

    mov dword [rdi+0], 0x00000001       ; CDW0: opcode = CREATE_IO_SQ
    lea rax, [rel nvme_iosq]
    mov [rdi+24], rax                   ; PRP1
    mov eax, 1 | ((NVME_IO_QSIZE-1) << 16)
    mov [rdi+40], eax                   ; CDW10: QID=1, QSIZE-1
    mov eax, 1 | (1 << 16)
    mov [rdi+44], eax                   ; CDW11: PC=1, CQID=1

    lea rcx, [rel nvme_sqe_scratch]
    lea rdx, [rel nvme_admin_qpair]
    call nvme_do_cmd
    test eax, eax
    jz .fail

    lea rax, [rel nvme_iosq]
    mov [rel nvme_io_qpair+0], rax
    lea rax, [rel nvme_iocq]
    mov [rel nvme_io_qpair+8], rax
    mov rax, [rel nvme_base]
    add rax, 0x1000
    mov ecx, [rel nvme_dbstride]
    add rax, rcx
    add rax, rcx                        ; +2*stride = SQ1TDBL
    mov [rel nvme_io_qpair+16], rax
    add rax, rcx                        ; +3*stride = CQ1HDBL
    mov [rel nvme_io_qpair+24], rax
    mov dword [rel nvme_io_qpair+32], 0
    mov dword [rel nvme_io_qpair+36], 0
    mov dword [rel nvme_io_qpair+40], 1
    mov dword [rel nvme_io_qpair+44], NVME_IO_QSIZE

    mov eax, 1
    jmp .out
.fail:
    xor eax, eax
.out:
    pop rdi
    pop rdx
    pop rcx
    ret

; -------------------------------------------------------------------------
; nvme_read_sectors: RCX = LBA (64-bit), RDX = sector count (1-65536),
; R8 = destination buffer (contiguous, >= count*512 bytes, page-aligned
; recommended so a single PRP1 covers it without needing PRP2/a PRP list).
; Issues an NVM READ for NSID=1. Returns EAX=1/0.
; -------------------------------------------------------------------------
nvme_read_sectors:
    push rcx
    push rdx
    push rdi
    push rsi
    push r10

    mov rsi, r8                         ; rsi = destination buffer
    mov r10, rcx                        ; r10 = LBA

    lea rdi, [rel nvme_sqe_scratch]
    push rdi
    xor eax, eax
    mov ecx, 8
    cld
    rep stosq
    pop rdi

    mov dword [rdi+0], 0x00000002       ; CDW0: opcode = READ
    mov dword [rdi+4], 1                ; NSID = 1
    mov [rdi+24], rsi                   ; PRP1 = destination buffer
    mov eax, r10d
    mov [rdi+40], eax                   ; CDW10: SLBA[31:0]
    mov rax, r10
    shr rax, 32
    mov [rdi+44], eax                   ; CDW11: SLBA[63:32]
    mov eax, edx
    dec eax                             ; CDW12: NLB (0's-based)
    mov [rdi+48], eax

    lea rcx, [rel nvme_sqe_scratch]
    lea rdx, [rel nvme_io_qpair]
    call nvme_do_cmd

    pop r10
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    ret

; =========================================================================
; Data
; =========================================================================
nvme_base:      dq 0
nvme_dbstride:  dd 0

align 4096
nvme_asq: times (NVME_ADMIN_QSIZE*64) db 0
align 4096
nvme_acq: times (NVME_ADMIN_QSIZE*16) db 0
align 4096
nvme_iosq: times (NVME_IO_QSIZE*64) db 0
align 4096
nvme_iocq: times (NVME_IO_QSIZE*16) db 0
align 4096
nvme_databuf: times 512 db 0

nvme_sqe_scratch: times 64 db 0
nvme_admin_qpair: times 48 db 0
nvme_io_qpair:    times 48 db 0
