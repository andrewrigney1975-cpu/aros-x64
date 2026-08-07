#!/usr/bin/env bash
# Boots the arOS-X64 dev disk (disk/) under QEMU + TianoCore OVMF.
# Usage: scripts/run-qemu.sh [extra qemu args...]
#   Add nothing for an interactive graphical window.
#   Add --headless for a serial-only headless run (useful for CI / automated checks).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
OVMF="/c/Program Files/qemu/share/edk2-x86_64-code.fd"
DISK="$(cygpath -w "$ROOT/disk")"

ARGS=(
  -machine q35
  -cpu qemu64,pdpe1gb=on
  -m 256M
  -drive if=pflash,format=raw,readonly=on,file="$OVMF"
  -drive file=fat:rw:"$DISK",format=raw,if=virtio
  -drive file=fat:rw:"$DISK",format=raw,if=ide
  -drive if=none,id=nvmedisk,file=fat:rw:"$DISK",format=raw
  -device nvme,drive=nvmedisk,serial=deadbeef01
  -net none
)

if [[ "${1:-}" == "--headless" ]]; then
  shift
  ARGS+=(-display none -serial stdio)
elif [[ "${1:-}" == "--snapshot" ]]; then
  shift
  ARGS+=(-display none -serial file:"$(cygpath -w "$ROOT/build/serial.log")" -monitor tcp:127.0.0.1:4444,server,nowait)
else
  ARGS+=(-serial stdio)
fi

"$QEMU" "${ARGS[@]}" "$@"
