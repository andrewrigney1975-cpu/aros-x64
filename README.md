# arOS-X64

A small, multithreaded, UEFI-compatible x86-64 operating system, hand-written
in NASM assembly, targeting 8th-gen-or-later Intel x64 machines booting from
NVMe or AHCI. Ships its own UEFI bootloader (GOP graphics, no C/no linker —
the PE32+ header is emitted directly from NASM), an exFAT user filesystem,
and BASIX64: a mixed-case-keyword BASIC-inspired language with `sfloat`,
`dfloat`, matrices, and 3D/GUI-oriented math, compiled to native machine code.

## Status: Phase 1 — bootloader + kernel handoff

- `boot/bootloader.asm` — hand-crafted PE32+ EFI_APPLICATION, assembled
  directly by `nasm -f bin` (no linker). Boots under UEFI, locates the
  volume it was loaded from, reads `\AROS\KERNEL.BIN` via
  `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, loads it at a fixed physical address
  via `AllocatePages`, and jumps to it with `RCX=ImageHandle,
  RDX=SystemTable` (Boot Services stay active for now).
- `kernel/kernel.asm` — flat binary loaded at `0x200000`, prints
  `Hello, kernal!` via `ConOut` and COM1 serial, then halts.

## Toolchain

- **NASM** (`nasm -f bin`) — the only assembler/build tool needed; no
  linker, no C compiler. PE/COFF headers are hand-written in the source.
- **QEMU + TianoCore OVMF** (`edk2-x86_64-code.fd`, bundled with the QEMU
  Windows build) — the reference UEFI test platform. Emulates NVMe
  (`-device nvme`) and AHCI (`-device ahci`) for later-phase driver work.

## Build & run

```sh
scripts/build.sh              # assembles boot/bootloader.asm + kernel/kernel.asm
                               # into disk/EFI/BOOT/BOOTX64.EFI and disk/AROS/KERNEL.BIN
scripts/run-qemu.sh           # graphical QEMU window
scripts/run-qemu.sh --headless -no-reboot   # serial-only, for automated checks
```

`disk/` is exposed to QEMU as a virtual FAT volume (`fat:rw:`), so no disk
image tooling (mtools/dd) is needed during development. `disk/startup.nsh`
makes headless boots deterministic by explicitly invoking
`FS0:\EFI\BOOT\BOOTX64.EFI` regardless of which default-boot path OVMF
picks on a given run.

Real hardware / USB-image creation (GPT + FAT32 ESP) is a later-phase
concern once more of the kernel exists.
