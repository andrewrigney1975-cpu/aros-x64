# arOS-X64

A small, multithreaded, UEFI-compatible x86-64 operating system, hand-written
in NASM assembly, targeting 8th-gen-or-later Intel x64 machines booting from
NVMe or AHCI. Ships its own UEFI bootloader (GOP graphics, no C/no linker —
the PE32+ header is emitted directly from NASM), a read/write exFAT user
filesystem, a preemptive scheduler, a keyboard-driven interactive shell, and
BASIX64: a mixed-case-keyword BASIC-inspired language with `sfloat`,
`dfloat`, matrices, and 3D/GUI-oriented math, compiled directly to native
machine code by a compiler running inside the kernel itself.

## Status: Phase 10 — process termination and kernel hardening

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
- **BASIX64 compiler** (`basix_lexer.inc`, `basix_codegen.inc`,
  `basix_symbols.inc`, `basix_runtime.inc`, `basix_parser.inc`) — a real
  single-pass compiler: a lexer (float literals parsed via genuine SSE2
  arithmetic at lex time), and a recursive-descent parser that emits native
  x64 machine code directly as it parses — no AST, no linker, no external
  assembler. Variable/label addresses are embedded as absolute 64-bit
  constants (`MOV RAX, moffs64`) since this kernel is a flat, fixed-address,
  non-relocatable binary. Language scope: `LET`, `PRINT`,
  `IF/THEN/ELSE/ENDIF` (block form), `FOR/NEXT`, `WHILE/WEND`,
  `GOTO/GOSUB/RETURN` with named labels, arithmetic (`+ - * / MOD`, unary
  minus, parens), and comparisons (valid only as an `IF`/`WHILE` condition,
  compiled directly to a conditional jump rather than a materialized
  boolean). Verified with real compiled-and-executed programs run through
  the shell's `RUN` command: arithmetic, loops, conditionals, and
  subroutines with both forward and backward label references all produce
  correct output.
- **Real `sfloat`/`dfloat` arithmetic** (stage 2) — every expression-
  producing codegen routine now tracks its result's type (int in RAX, or
  double in XMM0 — `sfloat` is transparently widened to double on load via
  `CVTSS2SD` and narrowed back via `CVTSD2SS` only when stored into an
  `sfloat` variable, so there's one internal float code path, not two).
  Binary ops promote int operands to double automatically when mixed with
  a float operand; float comparisons use `COMISD` with the unsigned-style
  jump-condition mapping float flags actually require (not the signed
  mapping integer `CMP` uses). `PRINT` of a float value goes through a new
  `basix_rt_print_double` runtime helper (sign, integer part, fixed 6
  fractional digits). Verified with real compiled-and-executed programs:
  float literals, arithmetic (`+ - * /`), int→float promotion on
  assignment, `sfloat` round-tripping, and float comparisons inside both
  `IF` and `WHILE` all produce correct output.

### Phase 9 — BASIX64 language polish and vector/matrix math
- **Language polish**: single-line `IF cond THEN stmt (ELSE stmt)?` alongside
  the block form; `AND`/`OR`/`NOT` (comparisons now materialize a plain 0/1
  int via `SETcc`+`MOVZX` instead of jumping directly, so boolean combinators
  compose with ordinary integer bitwise ops -- no short-circuit evaluation,
  fine since expressions have no side effects; a bare arithmetic expression
  is also now a valid condition, nonzero = true); `MOD` now works on floats
  too (`a - b*trunc(a/b)`, SSE2 has no direct remainder instruction); float
  literals accept an exponent suffix (`1.5e10`); `PRINT` of a float trims
  trailing fractional zeros (`3.5`, not `3.500000`) while always keeping at
  least one digit (`3.0`, not bare `3`).
- **Vector/matrix math** (`VECTOR2`/`VECTOR3`/`VECTOR4`/`MATRIX4`) — a new
  storage class (`basix_var_wide_slots`, a 128-byte slot per variable
  alongside the existing 8-byte scalar slot) plus statement-level operations:
  `VSET`, `VADD`, `VSUB`, `VSCALE`, `VDOT`, `VCROSS` (VECTOR3), `VLEN`,
  `VNORM`, `MIDENT`, `MMUL`, `MVMUL` (MATRIX4 x VECTOR4). Single-component
  access (`v[0]`, `v[1]`, ...) is a normal expression, both read and write,
  since one component is just a double -- but vectors/matrices themselves
  don't flow through the general expression grammar as first-class values
  the way int/float do (that would need the type system to carry width and
  route through memory-resident temporaries everywhere; a bigger redesign
  deferred for now). Every vector/matrix statement is unrolled entirely at
  compile time: component indices are always compile-time constants, so
  "looping over components" is the *parser* looping while compiling, not
  runtime-generated loop code. Verified with real compiled-and-executed
  programs: every operation above produces mathematically correct results,
  including `MIDENT` + `MVMUL` round-tripping a vector through an identity
  transform and `MMUL` composing two identity matrices.
- One more real bug found and fixed while testing this phase: a genuine
  bounds-check bug in `exfat_find_root_file`/`exfat_dir_list_next` (present
  since phase 4) where a File entry landing near a directory cluster's end
  could read its Stream Extension/FileName fields from one cluster too
  early -- the existing guard checked the same already-validated offset
  instead of whether the *rest of the entry set* still fit, so it never
  actually caught anything. Fixed, though it's not confirmed to be the root
  cause of a separate, still-unresolved anomaly where one specific file on
  the well-worn `exfat_test.vhd` test volume is invisible to lookups despite
  Windows confirming it exists with correct content -- noted for follow-up,
  not blocking (a fresh file under a different name works perfectly).

### Phase 10 — process termination and kernel hardening
- **Process termination** (`sched.inc`) — a `TCB_STATE` field
  (alive/terminated) added to every task. A task can end itself
  (`task_exit`, called voluntarily) or be ended by the kernel; either way
  this just flags the TCB and stops scheduling it. `sched_pick_next` is now
  the single shared routine both the timer ISR and the exception handler
  call to find the next task to run — it's also where a terminated task
  gets spliced out of the ready ring and queued onto `sched_zombie_list`
  (reusing its now-dead `TCB_NEXT` field as the zombie-list link). It never
  calls `kmalloc`/`kfree`, so it's safe from interrupt/exception context;
  the actual `kfree` of a zombie's TCB+stack happens later, from ordinary
  context, via `sched_reap_zombies` (wired into the shell's prompt loop).
- **Exception-triggered task isolation** (`idt.inc`) — `isr_common`, after
  dumping diagnostics, now checks whether the scheduler is armed. If so, an
  unhandled CPU exception (divide-by-zero, GPF, page fault, ...) takes down
  only the ONE faulting task — mark it terminated and switch to the next
  task via `sched_pick_next` — instead of halting the whole system. This is
  valid because a fault lands on the faulting task's own kernel stack
  (IST=0, no alternate exception stack) with a GPR frame built identically
  to the timer ISR's, so "switch to a different task's saved RSP and
  pop+iretq" is exactly as sound here as on a normal preemption. Falls back
  to the original halt-the-system behavior only if the scheduler isn't
  armed yet (a fault during early boot still needs to be fatal and loud).
- **Stack canary guard** (`sched.inc`) — `task_create` writes a known
  sentinel at the lowest address of every task's kmalloc'd kernel stack;
  `sched_pick_next` checks the outgoing task's canary on every scheduling
  decision (skipped for the main task, which runs on the kernel's own boot
  stack rather than a kmalloc'd one) and terminates it — same path as an
  unhandled exception — if a stack overflow has clobbered it.
- **`kmalloc`/`kfree` splitting and coalescing** (`kheap.inc`) — `kmalloc`'s
  small-block path now splits a found block down to the requested size
  when the leftover is worth keeping as its own free block, instead of
  always handing out the whole thing (previously a single 32-byte
  allocation would permanently consume an entire 4080-byte free block).
  `kfree` now scans for address-adjacent free neighbors and merges with
  them, so a page carved into many small pieces can still reassemble once
  everything on it is freed again.
- All four verified with dedicated demo/regression tasks in the boot-time
  self-test (`test_task_exit`, `test_task_crash`, `test_task_overflow`
  alongside the original preemption pair) and an extended `kmalloc`/`kfree`
  self-test: counters that must settle at an exact value and never advance
  again, A/B tasks that must keep advancing throughout (proving no
  termination path takes the rest of the system down with it), a zombie
  list that must end up empty after reaping, and a coalesced-block
  allocation that must return the exact address of the earlier of the two
  merged blocks. Confirmed stable across multiple consecutive full boots.

Current boot sequence (verified via serial log and QEMU screendumps):
GDT/IDT/PIC/timer → paging → PMM/VMM/heap self-tests (including
split/coalesce) → AHCI + NVMe device bring-up and LBA0 read → exFAT mount
(GPT or MBR), file lookup, read-back, write-path self-tests
(bitmap/FAT-chain/directory/file write) → scheduler bring-up, preemption
proof, and process-termination/canary-guard verification → BASIX64
compile-and-run smoke test → drops into the interactive shell.

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
- Tasks are kernel-only (ring 0) and share one address space — no per-task
  CR3/page tables yet, so isolation is cooperative (a fault or bad canary
  ends a task) rather than memory-protected (nothing stops one task's code
  from directly reading/writing another's memory).
- exFAT reader/writer assumes the volume's sector size matches the
  underlying device's native sector size (true almost always in practice).
- exFAT write path: file names capped at 15 ASCII characters (one
  FileName directory entry); no file delete/rename/truncate/append yet.
- Keyboard driver: no extended-key (arrow keys, right-Ctrl/Alt) support
  yet — the 0xE0 prefix is recognized and safely discarded.
- Shell: single-line command input only, no history/tab-completion.
- BASIX64: `FOR` loop control values are always truncated to int, names
  limited to 15 ASCII characters, no nested-FOR beyond 8 levels deep, no
  float literal scientific-notation *display* (only accepted on input), no
  general 3D/GUI pipeline (perspective divide, quaternions, transform
  stacks) beyond the raw vector/matrix primitives. Vector/matrix component
  indices must be compile-time integer literals (`v[0]`, not `v[i]`);
  vectors/matrices aren't first-class expression values (no `v3 = v1 + v2`
  -- see the design note atop `basix_parser.inc`); parenthesizing just one
  side of a top-level comparison doesn't work (`(x+1) = 5` -- wrap the whole
  comparison instead, `((x+1) = 5)`). These are explicitly deferred scope
  or documented parser limitations, not oversights.
- One unresolved anomaly: a specific file on the long-reused
  `testdata/exfat_test.vhd` test volume became invisible to the kernel's
  exFAT lookups despite Windows confirming it existed with correct content;
  root cause unconfirmed (a related bounds-check bug was found and fixed,
  but didn't resolve this specific case). Not reproduced under a different
  filename/fresh write.
