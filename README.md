# arOS-X64

A small, multithreaded, UEFI-compatible x86-64 operating system, hand-written
in NASM assembly, targeting 8th-gen-or-later Intel x64 machines booting from
NVMe or AHCI. Ships its own UEFI bootloader (GOP graphics, no C/no linker —
the PE32+ header is emitted directly from NASM), an exFAT user filesystem,
and BASIX64: a mixed-case-keyword BASIC-inspired language with `sfloat`,
`dfloat`, matrices, and 3D/GUI-oriented math, compiled to native machine code.

## Status: Phase 5 — hardened kernel core (exFAT, interrupts, real VMM)

**Bootloader** (`boot/bootloader.asm`) — hand-crafted PE32+ EFI_APPLICATION,
assembled directly by `nasm -f bin` (no linker). Boots under UEFI, locates
the volume it was loaded from, reads `\AROS\KERNEL.BIN` via
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, locates the GOP framebuffer, captures the
UEFI memory map, calls `ExitBootServices`, and jumps into the kernel with
`RCX = boot_info*` (framebuffer + memory-map descriptors).

**Kernel** (`kernel/kernel.asm` + `kernel/*.inc`) — flat binary loaded at a
fixed physical address, entirely off firmware from `ExitBootServices`
onward:

- Own flat GDT and a real IDT: all 32 CPU exception vectors dump
  vector/error-code/RIP/CR2 and halt instead of hanging silently — this has
  repeatedly turned "mystery hang" bugs into a one-line diagnosis.
- 8259 PIC remapped to vectors 32-47, PIT timer (~100Hz) confirmed firing.
- Real page-table-based VMM (`vmm.inc`) on top of a blanket identity map:
  4KB-granularity mapping in a dedicated virtual arena, used by `kmalloc` to
  stitch non-contiguous physical pages into contiguous multi-page
  allocations.
- Physical memory allocator (`pmm.inc`, bitmap-based, tracks 128GB) built
  from the real UEFI memory map, plus a `kmalloc`/`kfree` heap on top.
- AHCI and NVMe drivers (`ahci.inc`, `nvme.inc`) built from scratch against
  the AHCI/NVMe specs — PCI enumeration, command submission, and IDENTIFY
  (ATA IDENTIFY DEVICE / NVMe Identify Namespace) to discover real sector
  sizes rather than assuming 512 bytes. Dispatched through a generic
  `storage_read_sectors` (`storage.inc`) that prefers whichever driver found
  a working device.
- Read-only exFAT reader (`exfat.inc`) built against Microsoft's public
  exFAT specification: MBR-partition fallback, cluster addressing, FAT
  chain-following (including Windows' NoFatChain optimization), directory
  parsing, and file read — verified against a real Windows-formatted exFAT
  volume, not a hand-rolled one.

Current boot sequence (verified via serial log and QEMU screendumps):
GDT/IDT/PIC/timer → paging → PMM/VMM/heap self-tests → AHCI + NVMe device
bring-up and LBA0 read → exFAT mount, file lookup, and read-back.

## Toolchain

- **NASM** (`nasm -f bin`) — the only assembler/build tool needed; no
  linker, no C compiler. PE/COFF headers are hand-written in the source.
- **QEMU + TianoCore OVMF** (`edk2-x86_64-code.fd`, bundled with the QEMU
  Windows build) — the reference UEFI test platform. Emulates NVMe
  (`-device nvme`) and AHCI (`if=ide` on q35's built-in controller).

## Build & run

```sh
scripts/build.sh              # assembles boot/bootloader.asm + kernel/kernel.asm
                               # into disk/EFI/BOOT/BOOTX64.EFI and disk/AROS/KERNEL.BIN
scripts/run-qemu.sh           # graphical QEMU window
scripts/run-qemu.sh --headless -no-reboot   # serial-only, for automated checks
scripts/run-qemu.sh --snapshot -no-reboot   # headless + QEMU monitor on
                                             # 127.0.0.1:4444, for screendumps
```

`disk/` is exposed to QEMU as a virtual FAT volume (`fat:rw:`), so no disk
image tooling (mtools/dd) is needed for the ESP. `disk/startup.nsh` makes
headless boots deterministic by explicitly invoking
`FS0:\EFI\BOOT\BOOTX64.EFI` regardless of which default-boot path OVMF
picks on a given run. A separate real exFAT test volume lives under
`testdata/` (see `testdata/README.md`) and backs the NVMe-attached disk.

Real hardware / USB-image creation (GPT + FAT32 ESP) is a later-phase
concern once more of the kernel exists.

## Known limitations (tracked, not forgotten)

- Physical memory tracked up to 128GB; identity map covers up to 1TB.
- `kmalloc` doesn't split or coalesce free blocks yet.
- exFAT reader assumes the volume's sector size matches the underlying
  device's native sector size (true almost always in practice).
- No GPT partition-table support yet (MBR only).
