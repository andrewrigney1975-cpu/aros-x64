; =============================================================================
; Preemptive round-robin scheduler.
;
; Design: a task's saved state is just an RSP pointing into its own kernel
; stack, at a spot laid out to look EXACTLY like what pic.inc's timer ISR
; produces after its GPR-save prologue -- 15 pushed GPRs immediately below
; a CPU interrupt frame (RIP/CS/RFLAGS/RSP/SS). That means the ISR's
; "resume a task" path (pop everything, iretq) works identically whether
; the task was really interrupted mid-execution or has never run at all;
; task_create just hand-builds that same layout once, pointed at the
; task's entry point instead of some mid-function RIP.
;
; Tasks are kernel-only (ring 0, flat GDT selectors already in use
; elsewhere) and still share ONE kernel address space for code, data, and
; the kmalloc heap -- there's no ring-3/syscall boundary here, so this
; was never going to be hard memory protection. What each task DOES get
; is its own private PML4 (TCB_CR3) whose only difference from every
; other task's is a single PML4 slot (TASK_STACK_VIRT_BASE, see below)
; mapping that task's own kernel stack and nobody else's -- every other
; slot (identity map, kmalloc heap arena) is a straight copy of the
; shared global `pml4`, so kernel code/data/heap access is unaffected.
; The payoff: the SAME virtual address means a DIFFERENT physical stack
; depending on which task's CR3 is active, so a stray/dangling pointer
; in task A that happens to guess task B's stack's virtual address reads
; or corrupts task A's OWN stack instead (or nothing, if A has no
; private stack) -- accidental cross-task stack aliasing is structurally
; impossible now, not just discouraged. This does NOT stop a task that
; deliberately reads/writes another task's stack via its raw PHYSICAL
; address through the (necessarily shared) identity map, or that reloads
; CR3 itself -- real isolation needs ring 3 + syscalls, out of scope
; here. See kernel.asm's Known Limitations section.
;
; Termination: a task can end itself (task_exit), be ended by the kernel
; (an unhandled CPU exception while the scheduler is armed marks the
; faulting task terminated instead of halting the whole system -- see
; idt.inc's isr_common), or be ended by a corrupted stack canary
; (task_create writes a known value at the lowest address of every
; task's kernel stack; sched_pick_next checks the outgoing task's canary
; on every scheduling decision and terminates it if a stack overflow has
; clobbered it). All three just set TCB_STATE and leave the task's own
; execution stopped (a terminated task's saved RSP is simply never
; chosen again); actually freeing its TCB+stack+page-tables happens
; later, from ordinary (non-interrupt) context, via sched_reap_zombies --
; freeing memory from inside an ISR/exception handler is avoidable
; complexity this design just sidesteps by deferring it. sched_pick_next
; is the one place that walks the ready ring looking for the next task
; to run; it's also where terminated tasks get spliced out and queued
; for reaping, so both the timer ISR and the exception handler share it
; instead of duplicating the skip logic.
;
; Internal calling convention for this file only: plain registers as
; documented per routine, not the MS x64 ABI.
; =============================================================================

TCB_RSP         equ 0   ; saved stack pointer (0 until this task's first save)
TCB_NEXT        equ 8   ; next task in the ready ring, or (once terminated)
                         ; the next zombie in sched_zombie_list -- the field
                         ; is repurposed since a terminated task is never
                         ; scheduled again, so its ready-ring link is dead.
TCB_CR3         equ 16  ; physical address of this task's private PML4, or
                         ; 0 for a task with no private paging (the main
                         ; task, which runs on the kernel's own boot stack
                         ; under the shared global `pml4` -- see
                         ; sched_switch_cr3)
TCB_STATE       equ 24  ; TCB_STATE_ALIVE / TCB_STATE_TERMINATED
TCB_STACK_PAGES equ 32  ; number of private stack pages to free on reap
TCB_FPU         equ 40  ; pointer to this task's 512-byte, 16-byte-aligned
                         ; FXSAVE area (irq0_stub saves/restores SSE/x87
                         ; state across a switch here -- the GPR save this
                         ; file's header describes doesn't cover XMM/FPU
                         ; registers at all, which BASIX64's float pipeline
                         ; lives in almost entirely; without this a task
                         ; preempted mid float computation could resume
                         ; with another task's XMM state)
TCB_BACKBUF_PTR equ 48  ; this task's own off-screen back buffer (see
                         ; basix_rt_ensure_backbuf, basix_runtime.inc) --
                         ; 0 until that task's first graphics call
                         ; allocates one. irq0_stub/isr_common mirror
                         ; TCB_BACKBUF_PTR/W/H into the global
                         ; basix_backbuf_ptr/w/h on every switch-IN, so
                         ; every basix_rt_* graphics routine keeps reading
                         ; through the same global as before and just
                         ; transparently sees "whichever task is running
                         ; right now"'s own buffer -- see R0's per-task
                         ; backbuffer work.
TCB_BACKBUF_W   equ 56
TCB_BACKBUF_H   equ 60
TCB_KBD_BUF_PTR equ 64  ; this task's own 256-byte keyboard ring buffer
                         ; (see keyboard.inc's kbd_has_char/kbd_read_char,
                         ; which now resolve through current_task instead
                         ; of one shared kbd_buf) -- allocated up front in
                         ; task_create, not lazily like the back buffer,
                         ; since 256 bytes/task is trivial and every task
                         ; that can call GETKEY/KEYHIT needs one from the
                         ; moment it starts.
TCB_KBD_HEAD    equ 72
TCB_KBD_TAIL    equ 76
TCB_LAUNCH_ARG_BUF_PTR equ 80  ; this task's own kmalloc'd copy of its
                                ; LAUNCH argument bytes (0/empty if it
                                ; wasn't started via LAUNCH -- e.g. RUN),
                                ; read by ARGLEN/ARGCHAR through
                                ; current_task. Freed by
                                ; basix_task_launch_wrapper once the
                                ; task's own compiled code returns. A
                                ; private per-task copy (not the old
                                ; single shared basix_launch_arg_buf) is
                                ; what makes LAUNCH safe to be genuinely
                                ; concurrent/non-blocking now -- see
                                ; basix_rt_launch.
TCB_LAUNCH_ARG_LEN     equ 88
; R1: this task's own dirty rect (bounding box of everything drawn
; into its back buffer since the compositor task last composited it --
; see basix_mark_dirty, basix_runtime.inc, and the compositor task).
; [X0,Y0)-[X1,Y1), exclusive upper bound. TCB_DIRTY_VALID==0 means
; nothing has been drawn since the last composite (or ever) -- the
; other four fields are meaningless until it's 1.
TCB_DIRTY_X0    equ 92
TCB_DIRTY_Y0    equ 96
TCB_DIRTY_X1    equ 100
TCB_DIRTY_Y1    equ 104
TCB_DIRTY_VALID equ 108
; R4: window chrome/positioning. TCB_WIN_X/Y is the CLIENT area's
; screen position (chrome -- title bar + border -- is drawn by the
; compositor OUTSIDE this, not inside the task's own back buffer);
; TCB_WIN_W/H is the client area's size, which is also what
; TCB_BACKBUF_W/H end up being for a windowed task (see
; basix_rt_ensure_backbuf) and what SCREENW/SCREENH report to that
; task -- a program never needs to know it's not drawing directly to
; the real screen. Only meaningful for LAUNCHed tasks (basix_rt_launch
; sets all of these right after task_create); main (the desktop/
; shell layer, z-order index 0) draws full-screen with no chrome and
; never touches them -- see compositor_task's own z-order-index-0
; special case. TCB_WIN_CLOSE_REQ is set by the compositor's own
; window-manager pass when the user clicks this window's close gadget
; -- cooperative, not a forced kill: see basix_rt_winclose, the
; BASIX64-visible poll a program checks (same style as KEYHIT/
; MOUSEBTN) to notice and act on it (typically END).
TCB_WIN_X          equ 112
TCB_WIN_Y           equ 116
TCB_WIN_W           equ 120
TCB_WIN_H           equ 124
TCB_WIN_TITLE_LEN   equ 128
TCB_WIN_TITLE       equ 132   ; 32 bytes, not NUL-terminated -- length
                               ; is TCB_WIN_TITLE_LEN, same convention
                               ; as every other BASIX64 name buffer
TCB_WIN_CLOSE_REQ   equ 164
TCB_SIZE            equ 168

TCB_STATE_ALIVE      equ 0
TCB_STATE_TERMINATED equ 1

SCHED_QUANTUM equ 2   ; timer ticks per task (~20ms at the 100Hz PIT rate)

SCHED_STACK_CANARY equ 0xDEADC0DEDEADC0DE

; A task's private stack lives at this fixed virtual address in every
; task's OWN page tables -- the same address in each, but resolving to
; different physical pages depending on which task's CR3 (and therefore
; which task's copy of this PML4 slot) is currently active. PML4 index 3
; (each slot covers 512GB): index 0-1 are the identity map, index 2 is
; the shared kmalloc heap arena (VMM_VIRT_BASE, vmm.inc), so 3 is the
; next free one.
TASK_STACK_VIRT_BASE equ 0x18000000000

; -------------------------------------------------------------------------
; task_create: RCX = entry point, RDX = argument (delivered in RDI when the
; task first runs), R8 = kernel stack size in bytes -- must be a multiple
; of 4096 (every call site uses 16384; the fake interrupt frame this
; builds is placed at the very end of the last page, which relies on
; that). Returns RAX = new TCB pointer, or 0 on failure. The new task is
; NOT linked into the ready ring or otherwise made runnable -- the
; caller does that (see kernel.asm).
; -------------------------------------------------------------------------
task_create:
    push rbx
    push rsi
    push rdi
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                        ; r12 = entry point
    mov r13, rdx                        ; r13 = argument
    mov r14, r8                         ; r14 = stack size

    mov rcx, TCB_SIZE
    call kmalloc
    test rax, rax
    jz .fail
    mov rbx, rax                        ; rbx = new TCB

    ; This task's own PML4: starts as a full copy of the shared global
    ; one (so kernel code/data/heap stay reachable), then gets a PRIVATE
    ; mapping for its stack below that no other task's page tables
    ; reference at all -- see the header comment for why.
    call pmm_alloc_page
    test rax, rax
    jz .fail_free_tcb
    mov r15, rax                        ; r15 = this task's PML4 (physical)
    push rcx
    push rsi
    push rdi
    lea rsi, [rel pml4]
    mov rdi, r15
    mov rcx, 512
    cld
    rep movsq
    pop rdi
    pop rsi
    pop rcx
    mov [rbx+TCB_CR3], r15

    mov rax, r14
    add rax, 4095
    shr rax, 12
    mov r10, rax                        ; r10 = stack page count
    mov [rbx+TCB_STACK_PAGES], r10

    xor r9, r9                          ; r9 = page index
    xor r11, r11                        ; r11 = last allocated page's
                                         ; physical address (needed after
                                         ; the loop to place the initial
                                         ; frame)
.stack_page_loop:
    cmp r9, r10
    jge .stack_pages_done
    call pmm_alloc_page
    test rax, rax
    jz .fail_free_stack
    mov r11, rax

    test r9, r9
    jnz .not_first_page
    mov rdx, SCHED_STACK_CANARY
    mov [rax], rdx                      ; canary at the stack's lowest
                                         ; address -- the first thing an
                                         ; overflow (stack grows down)
                                         ; would clobber
.not_first_page:

    mov rcx, TASK_STACK_VIRT_BASE
    mov rdx, r9
    shl rdx, 12
    add rcx, rdx                        ; rcx = virtual address for this page
    mov rdx, r11                        ; rdx = physical address
    mov r8, r15                         ; r8 = this task's PML4
    call vmm_map_page_at
    test eax, eax
    jz .fail_free_stack

    inc r9
    jmp .stack_page_loop
.stack_pages_done:

    ; The fake interrupt frame (20 qwords, 160 bytes) that irq0_stub will
    ; pop when this task is first switched to goes at the very end of
    ; the stack -- which, since stack size is always a multiple of 4096,
    ; is exactly the last 160 bytes of the last allocated page (r11).
    ; Written through r11's PHYSICAL address (valid under whatever CR3 is
    ; currently active, via the shared identity map) since the new
    ; task's PRIVATE virtual mapping only exists in ITS OWN page tables,
    ; not the caller's. Order matters: it must mirror irq0_stub's push
    ; order exactly (see pic.inc).
    lea rdi, [r11 + 4096 - 160]         ; rdi = physical write address
    mov rsi, TASK_STACK_VIRT_BASE
    add rsi, r14                        ; rsi = virtual stack top (what
                                         ; RSP will hold once this task
                                         ; is actually running)

    mov qword [rdi+0], 0                ; r15
    mov qword [rdi+8], 0                ; r14
    mov qword [rdi+16], 0               ; r13
    mov qword [rdi+24], 0               ; r12
    mov qword [rdi+32], 0               ; r11
    mov qword [rdi+40], 0               ; r10
    mov qword [rdi+48], 0               ; r9
    mov qword [rdi+56], 0               ; r8
    mov qword [rdi+64], 0               ; rbp
    mov [rdi+72], r13                   ; rdi (the saved GPR) = argument
    mov qword [rdi+80], 0               ; rsi
    mov qword [rdi+88], 0               ; rdx
    mov qword [rdi+96], 0               ; rcx
    mov qword [rdi+104], 0              ; rbx
    mov qword [rdi+112], 0              ; rax
    mov [rdi+120], r12                  ; RIP = entry point
    mov qword [rdi+128], 0x08           ; CS  = kernel code selector
    mov qword [rdi+136], 0x202          ; RFLAGS = IF set, reserved bit 1
    mov [rdi+144], rsi                  ; RSP = virtual stack top
    mov qword [rdi+152], 0x10           ; SS  = kernel data selector

    mov rax, TASK_STACK_VIRT_BASE
    add rax, r14
    sub rax, 160                        ; rax = virtual address of the
                                         ; frame itself (this task's
                                         ; initial RSP)
    mov [rbx+TCB_RSP], rax
    mov qword [rbx+TCB_NEXT], 0
    mov dword [rbx+TCB_STATE], TCB_STATE_ALIVE
    mov qword [rbx+TCB_BACKBUF_PTR], 0
    mov dword [rbx+TCB_BACKBUF_W], 0
    mov dword [rbx+TCB_BACKBUF_H], 0

    ; This task's own FXSAVE area (see TCB_FPU) -- captured from the
    ; CURRENT cpu's FPU/SSE state (kmalloc's 16-byte alignment satisfies
    ; FXSAVE/FXRSTOR's requirement) purely to seed a well-formed default;
    ; this task hasn't executed any FP instructions yet, so whose state
    ; it starts from doesn't matter, only that it's valid.
    mov rcx, 512
    call kmalloc
    test rax, rax
    jz .fail_free_stack
    fxsave [rax]
    mov [rbx+TCB_FPU], rax

    ; This task's own keyboard ring buffer (see TCB_KBD_BUF_PTR above).
    mov rcx, 256
    call kmalloc
    test rax, rax
    jz .fail_free_stack
    mov [rbx+TCB_KBD_BUF_PTR], rax
    mov dword [rbx+TCB_KBD_HEAD], 0
    mov dword [rbx+TCB_KBD_TAIL], 0

    mov qword [rbx+TCB_LAUNCH_ARG_BUF_PTR], 0
    mov dword [rbx+TCB_LAUNCH_ARG_LEN], 0
    mov dword [rbx+TCB_DIRTY_VALID], 0

    ; R4 window geometry/title -- left zeroed here; only basix_rt_launch
    ; (not every task_create caller, e.g. compositor_task itself) fills
    ; these in, right after this call returns.
    mov dword [rbx+TCB_WIN_X], 0
    mov dword [rbx+TCB_WIN_Y], 0
    mov dword [rbx+TCB_WIN_W], 0
    mov dword [rbx+TCB_WIN_H], 0
    mov dword [rbx+TCB_WIN_TITLE_LEN], 0
    mov dword [rbx+TCB_WIN_CLOSE_REQ], 0

    mov rax, rbx
    jmp .out
.fail_free_stack:
    ; Leaks whatever pages/PML4 were allocated so far on this rare path
    ; (matches kmalloc_large's own documented policy for the equivalent
    ; case) -- not worth the extra machinery to unwind cleanly.
.fail_free_tcb:
    mov rcx, rbx
    call kfree
.fail:
    xor eax, eax
.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop rdi
    pop rsi
    pop rbx
    ret

; -------------------------------------------------------------------------
; sched_pick_next: RAX = the outgoing task's TCB. Returns RCX = the next
; task to switch to. Walks the ready ring starting at the outgoing
; task's TCB_NEXT, splicing out and queueing (onto sched_zombie_list, via
; each zombie's own now-unused TCB_NEXT) any terminated task found along
; the way, so neither the timer ISR nor the exception handler ever
; switches to a task that no longer exists. Never calls kmalloc/kfree --
; safe to call from interrupt/exception context.
; -------------------------------------------------------------------------
sched_pick_next:
    push rax
    push rbx
    push rdx
    push r8

    ; Stack-overflow guard: verify the outgoing task's canary (written
    ; at the lowest address of its stack by task_create) is still
    ; intact. Skipped for tasks with no private stack of their own
    ; (TCB_CR3==0 -- the main task runs on the kernel's own boot stack
    ; under the shared global page tables, which don't have
    ; TASK_STACK_VIRT_BASE mapped at all). Read via that fixed virtual
    ; address, not a physical one: this runs BEFORE the CR3 switch below,
    ; so the outgoing task's own page tables (mapping it to ITS OWN
    ; physical stack) are still the active ones. A corrupted canary
    ; means that task overflowed its stack into adjacent heap memory at
    ; some point during its last time slice; terminate it now rather
    ; than let it keep running (and corrupting more) or resume it later.
    mov rdx, [rax+TCB_CR3]
    test rdx, rdx
    jz .no_canary_check
    mov rbx, SCHED_STACK_CANARY
    mov rdx, TASK_STACK_VIRT_BASE
    cmp qword [rdx], rbx
    je .no_canary_check
    mov dword [rax+TCB_STATE], TCB_STATE_TERMINATED
.no_canary_check:

    mov rcx, [rax+TCB_NEXT]
.check:
    cmp dword [rcx+TCB_STATE], TCB_STATE_ALIVE
    je .found
    mov rbx, rcx                        ; rbx = terminated task, splice out
    mov r8, [rbx+TCB_NEXT]              ; r8 = the real next task after it
    mov [rax+TCB_NEXT], r8
    mov rdx, [rel sched_zombie_list]
    mov [rbx+TCB_NEXT], rdx             ; repurpose TCB_NEXT as the zombie link
    mov [rel sched_zombie_list], rbx
    mov rcx, r8
    jmp .check
.found:
    pop r8
    pop rdx
    pop rbx
    pop rax
    ret

; -------------------------------------------------------------------------
; task_exit: called BY a task to terminate itself. Never returns -- marks
; its own TCB terminated, then halts forever waiting to be switched away
; from (which happens on the very next timer tick, same as any other
; preemption; sched_pick_next then splices it out for good the next time
; anything walks past it in the ring).
; -------------------------------------------------------------------------
task_exit:
    mov rax, [rel current_task]
    test rax, rax
    jz .hang                            ; scheduler not armed -- nothing else to run
    mov dword [rax+TCB_STATE], TCB_STATE_TERMINATED
.hang:
    hlt
    jmp .hang

; -------------------------------------------------------------------------
; sched_reap_zombies: frees the TCB+stack+page-tables of every task
; queued on sched_zombie_list. Calls kmalloc/kfree/pmm_free_page, so
; this must only ever be called from ordinary (non-interrupt) context --
; e.g. periodically from the shell's main loop.
;
; A zombie's physical stack pages aren't recorded directly (only a
; count, TCB_STACK_PAGES) -- they're found by translating
; TASK_STACK_VIRT_BASE+N*4096 through the zombie's OWN saved PML4
; (vmm_translate_at), since that's the one place that still knows where
; they are. The PML4 itself is freed too, but the intermediate PDPT/PD/
; PT pages task_create's vmm_map_page_at calls built for it are not --
; same "don't bother" precedent vmm_unmap_page already set for the
; shared heap arena; a handful of small per-task leaks, acceptable for
; now.
; -------------------------------------------------------------------------
sched_reap_zombies:
    push rcx
    push rdx
    push r8
    push r9
    push r12
    push r13
    push r14
    push r15
.loop:
    mov r12, [rel sched_zombie_list]
    test r12, r12
    jz .done
    mov rdx, [r12+TCB_NEXT]
    mov [rel sched_zombie_list], rdx

    mov r13, [r12+TCB_CR3]
    test r13, r13
    jz .free_tcb                        ; no private paging (shouldn't
                                         ; happen for a real zombie, but
                                         ; defensive)
    mov r14, [r12+TCB_STACK_PAGES]
    xor r15, r15
.free_page_loop:
    cmp r15, r14
    jge .free_pml4
    mov rcx, TASK_STACK_VIRT_BASE
    mov r9, r15
    shl r9, 12
    add rcx, r9
    mov r8, r13
    call vmm_translate_at
    test rax, rax
    jz .free_page_next
    mov rcx, rax
    call pmm_free_page
.free_page_next:
    inc r15
    jmp .free_page_loop
.free_pml4:
    mov rcx, r13
    call pmm_free_page

.free_tcb:
    mov rcx, r12
    call kfree
    jmp .loop
.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

; =========================================================================
; Data
; =========================================================================
sched_zombie_list: dq 0
current_task:          dq 0    ; NULL until the scheduler is armed; irq0_stub
                                ; leaves the timer as a plain tick counter
                                ; until this is set (see pic.inc)
sched_quantum_counter:  dd 0
