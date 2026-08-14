' arOS-X64 "Workbench" desktop shell -- Phase 1: background, status bar,
' one disk icon, and a mouse cursor that tracks MOUSEX/MOUSEY. Whole
' desktop is redrawn every frame (no dirty-rect tracking yet -- fine at
' this resolution/complexity; revisit if it's ever too slow). Press
' Escape to exit back to the text shell.
'
' WAIT 2 (~50fps cap) below is deliberate, not just pacing: an
' uncapped CLS/RECT/DRAWTEXT/FLIP loop is the heaviest sustained
' interrupt/stack load anything in this codebase has produced so far,
' and it's been observed to eventually wedge the master PIC (a stuck
' IRQ0 in-service bit that blocks IRQ1/keyboard -- a real, still-
' unfixed kernel bug, not specific to this program). Capping the frame
' rate is a mitigation, not a fix -- remove it once the underlying
' interrupt-handling bug is actually root-caused.

LET sw = SCREENW
LET sh = SCREENH

WHILE 1
  IF KEYHIT THEN
    LET k = GETKEY
    IF k = 136 THEN END
  ENDIF

  ' Desktop background
  CLS 11184810

  ' Top status bar
  RECT 0, 0, sw, 20, 8947660
  DRAWTEXT 6, 3, "arOS-X64 Workbench", 16777215

  ' One disk icon (placeholder box + label -- real icon art is a later
  ' phase)
  RECT 20, 40, 48, 32, 16777215
  RECT 20, 40, 48, 2, 0
  RECT 20, 70, 48, 2, 0
  RECT 20, 40, 2, 32, 0
  RECT 66, 40, 2, 32, 0
  DRAWTEXT 12, 76, "AROSTEST", 0

  ' Mouse cursor: a small filled square with a dark border, centered on
  ' MOUSEX/MOUSEY
  LET mx = MOUSEX
  LET my = MOUSEY
  RECT mx - 4, my - 4, 8, 8, 0
  RECT mx - 3, my - 3, 6, 6, 16777215

  FLIP
  WAIT 2
WEND
