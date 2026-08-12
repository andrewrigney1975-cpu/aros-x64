; =============================================================================
; Minimal identity-mapped paging, built and owned by the kernel.
;
; Until now we've been riding whatever page tables UEFI left behind, which
; only cover the low, conventional address space it actually uses itself.
; NVMe's 64-bit BAR gets placed by firmware way out in the high MMIO
; aperture (hundreds of GB up), which firmware's own tables never bothered
; to map -- touching it page-faults with no handler installed and silently
; hangs. So: build our own identity map, covering 0..1TB with 1GB pages
; (cheap: one PML4 table + two PDPT tables, no PDs/PTs needed at all), and
; switch CR3 to it. We're already in long mode with PAE/LME set by
; firmware; swapping CR3 to another valid 4-level table is safe and normal.
;
; The 0xC0000000-0xFFFFFFFF 1GB entry (the standard sub-4GB PCI/firmware
; MMIO hole -- local APIC at 0xFEE00000, IOAPIC at 0xFEC00000, PCI BARs,
; flash, never real RAM) is special-cased: it gets its own page directory
; of 2MB leaves (PCD set) instead of one 1GB huge leaf like every other
; entry. A plain cacheable 1GB leaf is harmless under QEMU/TCG, which
; traps every MMIO access regardless of the guest's own page attributes,
; but a Hyper-V/WHvp-backed hypervisor (e.g. VirtualBox on a Windows host
; with Hyper-V active) apparently can't intercept an MMIO access whose
; guest-side translation bottoms out in a 1GB leaf -- the very first LAPIC
; MMIO read after enabling it triple-faults the guest with no exception
; ever reaching our own IDT (confirmed: nothing after "Hello, kernal!" on
; serial). Splitting just this one GB down to 2MB granularity (still PS=1,
; just one level down) fixed it.
; -------------------------------------------------------------------------

; -------------------------------------------------------------------------
; paging_init: builds pml4/pdpt_low/pdpt_high and loads CR3.
; -------------------------------------------------------------------------
paging_init:
    push rax
    push rcx
    push rdx
    push rdi

    lea rdi, [rel pdpt_low]
    xor ecx, ecx
    xor rdx, rdx                        ; rdx = 0 (first 1GB-aligned addr)
.low_loop:
    mov rax, rdx
    or rax, 0x83                        ; present | write | PS(1GB page)
    cmp edx, 0xC0000000                 ; the MMIO-hole GB gets a real PD
    jne .low_store                      ; (see .mmio_pd below) instead of
                                         ; a 1GB leaf; 32-bit compare since
                                         ; cmp r64,imm32 sign-extends the
                                         ; immediate and would never match
    lea rax, [rel pd_mmio]
    or rax, 0x03                        ; present | write (PDE, not PS)
.low_store:
    mov [rdi], rax
    add rdi, 8
    add rdx, 0x40000000                 ; + 1GB
    inc ecx
    cmp ecx, 512
    jl .low_loop

    ; pd_mmio: 512 * 2MB PCD leaves covering 0xC0000000-0xFFFFFFFF.
    lea rdi, [rel pd_mmio]
    xor ecx, ecx
    mov rdx, 0xC0000000
.mmio_loop:
    mov rax, rdx
    or rax, 0x93                        ; present | write | PCD | PS(2MB page)
    mov [rdi], rax
    add rdi, 8
    add rdx, 0x200000                   ; + 2MB
    inc ecx
    cmp ecx, 512
    jl .mmio_loop

    lea rdi, [rel pdpt_high]
    xor ecx, ecx
    mov rdx, 0x8000000000               ; 512GB
.high_loop:
    mov rax, rdx
    or rax, 0x83
    mov [rdi], rax
    add rdi, 8
    add rdx, 0x40000000
    inc ecx
    cmp ecx, 512
    jl .high_loop

    lea rdi, [rel pml4]
    lea rax, [rel pdpt_low]
    or rax, 0x03                        ; present | write
    mov [rdi+0], rax                    ; covers 0..512GB
    lea rax, [rel pdpt_high]
    or rax, 0x03
    mov [rdi+8], rax                    ; covers 512GB..1TB

    lea rax, [rel pml4]
    mov cr3, rax

    pop rdi
    pop rdx
    pop rcx
    pop rax
    ret

; =========================================================================
; Data
; =========================================================================
align 4096
pml4: times 4096 db 0
align 4096
pdpt_low: times 4096 db 0
align 4096
pdpt_high: times 4096 db 0
align 4096
pd_mmio: times 4096 db 0
