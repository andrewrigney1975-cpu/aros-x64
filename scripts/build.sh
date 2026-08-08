#!/usr/bin/env bash
# Assembles the bootloader and kernel and stages them into disk/ for QEMU.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NASM="/c/Program Files/NASM/nasm.exe"

mkdir -p "$ROOT/build" "$ROOT/disk/EFI/BOOT" "$ROOT/disk/AROS"

"$NASM" -f bin "$ROOT/boot/bootloader.asm" -o "$ROOT/build/BOOTX64.EFI"
cp "$ROOT/build/BOOTX64.EFI" "$ROOT/disk/EFI/BOOT/BOOTX64.EFI"

if [[ -f "$ROOT/kernel/kernel.asm" ]]; then
  "$NASM" -f bin -I "$ROOT/kernel/" "$ROOT/kernel/kernel.asm" -o "$ROOT/build/KERNEL.BIN"
  cp "$ROOT/build/KERNEL.BIN" "$ROOT/disk/AROS/KERNEL.BIN"
fi

echo "Build OK -> $ROOT/disk"

PYTHON="$(command -v python3 || command -v python)"
"$PYTHON" "$ROOT/scripts/make_image.py"
