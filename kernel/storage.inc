; =============================================================================
; Generic block-device dispatcher over whichever driver (AHCI or NVMe)
; found a working device during boot. storage_active_driver is set by
; storage_init_and_test / nvme_init_and_test in kernel.asm on success;
; NVMe is preferred when both are present (matches modern hardware, and
; kernel.asm probes NVMe after AHCI so a later NVMe success overrides it).
; =============================================================================

STORAGE_NONE equ 0
STORAGE_AHCI equ 1
STORAGE_NVME equ 2

; -------------------------------------------------------------------------
; storage_read_sectors: RCX = LBA, RDX = sector count, R8 = buffer.
; Returns EAX = 1/0. Fails immediately (EAX=0) if no driver is active.
; -------------------------------------------------------------------------
storage_read_sectors:
    cmp dword [rel storage_active_driver], STORAGE_NVME
    je .use_nvme
    cmp dword [rel storage_active_driver], STORAGE_AHCI
    je .use_ahci
    xor eax, eax
    ret
.use_nvme:
    jmp nvme_read_sectors
.use_ahci:
    jmp ahci_read_sectors

; -------------------------------------------------------------------------
; storage_write_sectors: RCX = LBA, RDX = sector count, R8 = buffer.
; Returns EAX = 1/0. Fails immediately (EAX=0) if no driver is active.
; -------------------------------------------------------------------------
storage_write_sectors:
    cmp dword [rel storage_active_driver], STORAGE_NVME
    je .use_nvme
    cmp dword [rel storage_active_driver], STORAGE_AHCI
    je .use_ahci
    xor eax, eax
    ret
.use_nvme:
    jmp nvme_write_sectors
.use_ahci:
    jmp ahci_write_sectors

storage_active_driver: dd STORAGE_NONE
