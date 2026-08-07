# arOS-X64

A small, multithreaded, UEFI-compatible x86-64 operating system, hand-written
in NASM assembly, targeting 8th-gen-or-later Intel x64 machines booting from
NVMe or AHCI. Ships its own UEFI bootloader (GOP graphics, no C/no linker —
the PE32+ header is emitted directly from NASM), a read/write exFAT user
filesystem, a preemptive scheduler, a keyboard-driven interactive shell, and
BASIX64: a mixed-case-keyword BASIC-inspired language with `sfloat`,
`dfloat`, matrices, and 3D/GUI-oriented math, compiled directly to native
machine code by a compiler running inside the kernel itself.

## Status: Phase 8 — keyboard, shell, and a working BASIX64 compiler (v1 core)

This is an as-built log: each phase below is implemented, tested (either via
the boot-time regression suite or interactively through QEMU), and merged.

### Phase 1-2 — bootloader and console
**Bootloader** (`boot/bootloader.asm`) — hand-crafted PE32+ EFI_APPLICATION,
assembled directly by `nasm -f bin` (no linker). Boots under UEFI, locates
the volume it was loaded from, reads `\AROS\KERNEL.BIN` via
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, locates the GOP framebuffer, captures the
UEFI memory map, calls `ExitBootServices`, and jumps into the kernel with
`RCX = boot_info*` (framebuffer + memory-map descriptors). The kernel owns
its own flat GDT and draws an 8x16 bitmap-font text console directly into
the GOP framebuffer, plus a COM1 serial mirror for headless/automated
verification.

### Phase 3 — PCI, AHCI, NVMe, paging
PCI enumeration (brute-force bus/device/function scan over ports
0xCF8/0xCFC), an AHCI driver and an NVMe driver built from scratch against
the AHCI/NVMe specs (command submission, IDENTIFY for real sector sizes
rather than assuming 512 bytes), and a kernel-owned identity page-table
mapping so the kernel isn't riding on firmware-established paging.

### Phase 4 — IDT, exFAT (read), interrupts, memory allocators
A real IDT: all 32 CPU exception vectors dump vector/error-code/RIP/CR2 and
halt instead of hanging silently — this has repeatedly turned "mystery
hang" bugs into a one-line diagnosis. 8259 PIC remapped to vectors 32-47,
PIT timer (~100Hz) confirmed firing. A read-only exFAT reader (`exfat.inc`)
built against Microsoft's public exFAT specification: MBR-partition
fallback, cluster addressing, FAT chain-following (including Windows'
NoFatChain optimization), directory parsing, and file read — verified
against a real Windows-formatted exFAT volume, not a hand-rolled one.
Storage access goes through a generic `storage_read_sectors` (`storage.inc`)
that prefers whichever driver (AHCI or NVMe) found a working device.
Physical memory allocator (`pmm.inc`, bitmap-based) and a `kmalloc`/`kfree`
heap on top.

### Phase 5 — hardening
Real page-table-based VMM (`vmm.inc`) on top of a blanket identity map:
4KB-granularity mapping in a dedicated virtual arena, used by `kmalloc` to
stitch non-contiguous physical pages into contiguous multi-page
allocations. Physical memory tracking extended to 128GB. Sector sizes
discovered via IDENTIFY rather than assumed.

### Phase 6 — preemptive scheduler
A round-robin preemptive scheduler (`sched.inc`) built directly on the PIT
timer interrupt: a task's saved state is just an RSP pointing into its own
kernel stack, laid out identically to what the timer ISR's own GPR-save
prologue produces, so resuming a never-run task and resuming a genuinely
interrupted one are the same code path. Verified with two demo tasks
looping forever and interleaving their output, proving real preemption
(not cooperative scheduling).

### Phase 7 — GPT support and exFAT write
`exfat_mount` now tries a GPT partition table (LBA1, preferring the
Microsoft Basic Data Partition type GUID) before falling back to MBR.
Full exFAT write support: sector-level writes for both AHCI and NVMe,
allocation-bitmap discovery and cluster alloc/free, FAT chain writing,
spec-correct directory entry construction (checksums, name hashing), and
`exfat_write_file` (allocate, FAT-chain, write data, create the directory
entry). Verified against both a real Windows-formatted MBR volume and a
real GPT volume, including full round-trip interop: files created by the
kernel are read back correctly by Windows' own exFAT driver.

### Phase 8 — keyboard, shell, BASIX64 compiler (v1 core)
- **PS/2 keyboard driver** (`keyboard.inc`) — IRQ1 handler decoding
  scancode-set-1 into ASCII (shift, caps lock, backspace/tab/enter) into a
  circular buffer; verified interactively against QEMU's PS/2 emulation.
- **Interactive shell** — a scrolling 8x16-cell text console (real
  framebuffer scroll, not just a screen clear) with a small line-edited
  REPL: `HELP`, `DIR`, `TYPE <file>`, `WRITE <file> <text>`,
  `RUN <file.bas>`, `CLEAR`.
- **BASIX64 compiler v1** (`basix_lexer.inc`, `basix_codegen.inc`,
  `basix_symbols.inc`, `basix_runtime.inc`, `basix_parser.inc`) — a real
  single-pass compiler: a lexer (float literals parsed via genuine SSE2
  arithmetic at lex time), and a recursive-descent parser that emits native
  x64 machine code directly as it parses — no AST, no linker, no external
  assembler. Variable/label addresses are embedded as absolute 64-bit
  constants (`MOV RAX, moffs64`) since this kernel is a flat, fixed-address,
  non-relocatable binary. Current scope (integers only — `sfloat`/`dfloat`
  parse and `DIM` but aren't yet usable in expressions): `LET`, `PRINT`,
  `IF/THEN/ELSE/ENDIF` (block form), `FOR/NEXT`, `WHILE/WEND`,
  `GOTO/GOSUB/RETURN` with named labels, arithmetic (`+ - * / MOD`, unary
  minus, parens), and comparisons (valid only as an `IF`/`WHILE` condition,
  compiled directly to a conditional jump rather than a materialized
  boolean). Verified with real compiled-and-executed programs run through
  the shell's `RUN` command: arithmetic, loops, conditionals, and
  subroutines with both forward and backward label references all produce
  correct output.

Current boot sequence (verified via serial log and QEMU screendumps):
GDT/IDT/PIC/timer → paging → PMM/VMM/heap self-tests → AHCI + NVMe device
bring-up and LBA0 read → exFAT mount (GPT or MBR), file lookup, read-back,
write-path self-tests (bitmap/FAT-chain/directory/file write) → scheduler
bring-up and preemption proof → BASIX64 compile-and-run smoke test → drops
into the interactive shell.

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
                                             # and sending keystrokes (sendkey)
```

`disk/` is exposed to QEMU as a virtual FAT volume (`fat:rw:`), so no disk
image tooling (mtools/dd) is needed for the ESP. `disk/startup.nsh` makes
headless boots deterministic by explicitly invoking
`FS0:\EFI\BOOT\BOOTX64.EFI` regardless of which default-boot path OVMF
picks on a given run. Real exFAT test volumes (one MBR, one GPT) live under
`testdata/` and back the NVMe-attached disk; the `attach_*`/`detach_*`
diskpart scripts there let Windows mount them read/write for inspection
without ever re-running the destructive `make_*` (format) scripts against
an already-populated volume.

Real hardware / USB-image creation (GPT + FAT32 ESP) is a later-phase
concern once more of the kernel exists.

## Known limitations (tracked, not forgotten)

- Physical memory tracked up to 128GB; identity map covers up to 1TB.
- `kmalloc` doesn't split or coalesce free blocks yet.
- exFAT reader/writer assumes the volume's sector size matches the
  underlying device's native sector size (true almost always in practice).
- exFAT write path: file names capped at 15 ASCII characters (one
  FileName directory entry); no file delete/rename/truncate/append yet.
- Keyboard driver: no extended-key (arrow keys, right-Ctrl/Alt) support
  yet — the 0xE0 prefix is recognized and safely discarded.
- Shell: single-line command input only, no history/tab-completion.
- BASIX64: integers only (`sfloat`/`dfloat` are lexed and can be `DIM`'d
  but aren't usable in expressions yet), no matrices/vectors, no 3D/GUI
  math, no single-line `IF`, no `AND`/`OR`/`NOT`, names limited to 15
  ASCII characters, no nested nested-FOR beyond 8 levels deep. These are
  the explicitly deferred "stage 2" scope, not oversights.
