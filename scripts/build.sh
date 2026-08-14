#!/usr/bin/env bash
# Assembles the bootloader and kernel and stages them into disk/ for QEMU.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NASM="/c/Program Files/NASM/nasm.exe"

mkdir -p "$ROOT/build" "$ROOT/disk/EFI/BOOT" "$ROOT/disk/AROS"

"$NASM" -f bin "$ROOT/boot/bootloader.asm" -o "$ROOT/build/BOOTX64.EFI"
cp "$ROOT/build/BOOTX64.EFI" "$ROOT/disk/EFI/BOOT/BOOTX64.EFI"

if [[ -f "$ROOT/kernel/kernel.asm" ]]; then
  # BASIX_AXB_KERNEL_ID: stamped into every .AXB COMPILE writes (see
  # basix_codegen.inc) and checked by RUN/LAUNCH before trusting one --
  # an .AXB's CALLs to basix_rt_* are absolute addresses baked in at
  # compile time, valid only for the exact kernel build that compiled
  # them (see basix_codegen.inc's .AXB format comment). A fresh value
  # every build (rather than a hand-maintained constant someone has to
  # remember to bump) means ANY kernel rebuild automatically
  # invalidates old .AXBs instead of silently letting a stale one jump
  # into whatever now lives at those addresses -- exactly what
  # happened before this was wired up: the constant was never bumped
  # across many rebuilds in one dev session, so an hours-old .AXB
  # loaded fine (kernel_id "matched") and froze/crashed the moment it
  # called a runtime function whose address had since shifted.
  AXB_KERNEL_ID="$(date +%s)"
  "$NASM" -f bin -I "$ROOT/kernel/" -D BASIX_AXB_KERNEL_ID="$AXB_KERNEL_ID" "$ROOT/kernel/kernel.asm" -o "$ROOT/build/KERNEL.BIN"
  cp "$ROOT/build/KERNEL.BIN" "$ROOT/disk/AROS/KERNEL.BIN"
fi

echo "Build OK -> $ROOT/disk"

PYTHON="$(command -v python3 || command -v python)"
"$PYTHON" "$ROOT/scripts/make_image.py"
