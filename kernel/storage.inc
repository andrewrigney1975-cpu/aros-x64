; =============================================================================
; Generic block-device dispatcher over whichever driver (AHCI, NVMe, or
; USB Mass Storage) found a working device during boot.
; storage_active_driver is set by storage_init_and_test /
; nvme_init_and_test / the USB enumeration sequence in kernel.asm on
; success. NVMe is preferred over AHCI when both are present (matches
; modern hardware, and kernel.asm probes NVMe after AHCI so a later
; NVMe success overrides it); USB is only selected when neither AHCI
; nor NVMe found a device -- the fallback of last resort, matching this
; project's eventual goal of booting from a USB drive on hardware with
; no other storage.
; =============================================================================

STORAGE_NONE equ 0
STORAGE_AHCI equ 1
STORAGE_NVME equ 2
STORAGE_USB  equ 3

; -------------------------------------------------------------------------
; storage_read_sectors: RCX = LBA, RDX = sector count, R8 = buffer.
; Returns EAX = 1/0. Fails immediately (EAX=0) if no driver is active.
; -------------------------------------------------------------------------
storage_read_sectors:
    cmp dword [rel storage_active_driver], STORAGE_NVME
    je .use_nvme
    cmp dword [rel storage_active_driver], STORAGE_AHCI
    je .use_ahci
    cmp dword [rel storage_active_driver], STORAGE_USB
    je .use_usb
    xor eax, eax
    ret
.use_nvme:
    jmp nvme_read_sectors
.use_ahci:
    jmp ahci_read_sectors
.use_usb:
    jmp usb_msd_read_sectors

; -------------------------------------------------------------------------
; storage_write_sectors: RCX = LBA, RDX = sector count, R8 = buffer.
; Returns EAX = 1/0. Fails immediately (EAX=0) if no driver is active.
; -------------------------------------------------------------------------
storage_write_sectors:
    cmp dword [rel storage_active_driver], STORAGE_NVME
    je .use_nvme
    cmp dword [rel storage_active_driver], STORAGE_AHCI
    je .use_ahci
    cmp dword [rel storage_active_driver], STORAGE_USB
    je .use_usb
    xor eax, eax
    ret
.use_nvme:
    jmp nvme_write_sectors
.use_ahci:
    jmp ahci_write_sectors
.use_usb:
    jmp usb_msd_write_sectors

; -------------------------------------------------------------------------
; storage_sector_size: returns EAX = the active driver's native sector
; size in bytes (as reported by IDENTIFY/READ CAPACITY), or 0 if no
; driver is active.
; -------------------------------------------------------------------------
storage_sector_size:
    cmp dword [rel storage_active_driver], STORAGE_NVME
    je .use_nvme
    cmp dword [rel storage_active_driver], STORAGE_AHCI
    je .use_ahci
    cmp dword [rel storage_active_driver], STORAGE_USB
    je .use_usb
    xor eax, eax
    ret
.use_nvme:
    mov eax, [rel nvme_sector_size]
    ret
.use_ahci:
    mov eax, [rel ahci_sector_size]
    ret
.use_usb:
    mov eax, [rel xhci_msd_sector_size]
    ret

storage_active_driver: dd STORAGE_NONE
