#!/usr/bin/env python3
"""
Generates kernel/logo.inc from assets/arOS_logo.txt (the boot-splash ASCII
art). Run this after editing the art; kernel/logo.inc is generated output,
not meant to be hand-edited.

Bytes are emitted as explicit `db <values>` rather than NASM string
literals because the art freely mixes backslashes, single quotes, double
quotes, and backticks -- exactly the characters NASM's three string forms
use for escaping/delimiting, so no single quoting choice is safe for every
line. Numeric byte lists sidestep the whole problem.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "arOS_logo.txt")
DST = os.path.join(ROOT, "kernel", "logo.inc")

with open(SRC, "rb") as f:
    data = f.read()
lines = [l.rstrip(b"\r\n").rstrip() for l in data.split(b"\n")]
while lines and lines[-1] == b"":
    lines.pop()

out = []
out.append("; Auto-generated from assets/arOS_logo.txt (boot-splash ASCII art).")
out.append("; Do not hand-edit -- regenerate via scripts/gen_logo.py.")
out.append("")
out.append(f"LOGO_LINE_COUNT equ {len(lines)}")
out.append("")

for i, line in enumerate(lines):
    bytevals = ", ".join(str(b) for b in line) if line else ""
    if bytevals:
        out.append(f"logo_line_{i}: db {bytevals}, 0")
    else:
        out.append(f"logo_line_{i}: db 0")

out.append("")
out.append("logo_lines:")
for i in range(len(lines)):
    out.append(f"    dq logo_line_{i}")

with open(DST, "w", newline="\n") as f:
    f.write("\n".join(out) + "\n")

print(f"wrote {DST}: {len(lines)} lines")
