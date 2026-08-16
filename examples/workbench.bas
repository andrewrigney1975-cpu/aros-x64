' arOS-X64 "Workbench" desktop shell.
'
' The desktop background, top status bar, and disk icon -- unwindowed,
' drawn straight into main's own full-screen back buffer, same as
' always. Double-clicking the disk icon LAUNCHes filebrowser.bas, a
' REAL compositor-managed window (kernel-drawn chrome, wm_tick
' drag+close) -- see filebrowser.bas's own header comment for why this
' used to be hand-drawn here instead, and why that was a problem.
' Double-clicking again while a browser window is already open just
' opens another one (matches how double-clicking a file to LAUNCH
' editor.bas/viewer.bas already behaves -- real multi-window OS
' behavior, and this desktop layer has no way to know whether a
' previously-LAUNCHed child is still alive or already closed anyway).
'
' WAIT 2 (~50fps cap) below is deliberate, not just pacing: an
' uncapped CLS/RECT/DRAWTEXT/FLIP loop is the heaviest sustained
' interrupt/stack load anything in this codebase has produced so far,
' and it's been observed to eventually wedge the master PIC (a stuck
' IRQ0 in-service bit that blocks IRQ1/keyboard -- a real, still-
' unfixed kernel bug, not specific to this program). Capping the frame
' rate is a mitigation, not a fix -- remove it once the underlying
' interrupt-handling bug is actually root-caused.
'
' Redraw is also now gated on "did anything actually change" (see
' needsRedraw below), not unconditional every frame: the disk icon's
' LOADPNG never changes, so reloading and redrawing it 50x/sec was
' pure waste even before this mattered for correctness -- and once
' filebrowser.bas started doing its own PNGLOCK-held batched decodes,
' that waste became real contention: workbench's own per-frame LOADPNG
' was racing filebrowser's locked batch on every single frame, and
' under that contention filebrowser's redraw pass took long enough
' (many scheduler quantums of lock-wait) that the compositor would
' blit its back buffer mid-update again -- the exact tearing bug
' PNGLOCK was added to prevent, reopened by a slower path. Only
' redrawing (and thus only re-touching the shared PNG decode lock)
' when the desktop's own visible state actually changes cuts that
' contention from ~50/sec to near-zero after the first frame, without
' needing per-task decode buffers (a bigger fix, not done here). The
' compositor still repaints this window correctly on expose/z-order
' changes from its own last-committed back buffer -- it doesn't need
' this program to re-FLIP anything for that.

DIM filebrowserProgName(16) AS INTEGER
DIM diskIconName(16) AS INTEGER
DIM argbuf(1) AS INTEGER

' "/FILEBROWSER.BAS" -- leading slash forces root-relative resolution
' (exfat_resolve_path) regardless of how deep DIRCD navigation has
' gone inside some OTHER already-open browser window.
LET filebrowserProgName[0] = 47
LET filebrowserProgName[1] = 70
LET filebrowserProgName[2] = 73
LET filebrowserProgName[3] = 76
LET filebrowserProgName[4] = 69
LET filebrowserProgName[5] = 66
LET filebrowserProgName[6] = 82
LET filebrowserProgName[7] = 79
LET filebrowserProgName[8] = 87
LET filebrowserProgName[9] = 83
LET filebrowserProgName[10] = 69
LET filebrowserProgName[11] = 82
LET filebrowserProgName[12] = 46
LET filebrowserProgName[13] = 66
LET filebrowserProgName[14] = 65
LET filebrowserProgName[15] = 83

' "ICONDISK.PNG" -- icon art (scripts/gen_icons.py). Bare (cwd-
' relative), NOT root-relative like the program name above: LOADPNG
' resolves paths via basix_resolve_dir_path (basix_runtime.inc), a
' different/older resolver than RUN/COMPILE/LAUNCH's exfat_resolve_path
' -- it does NOT special-case a leading '/' as "start from root" (a
' real bug there, not yet fixed). Bare names work correctly at the
' disk's root, which is where this icon lives and where the desktop
' icon spends 100% of its time being drawn now that the file browser
' itself is a separate window (it used to also need to redraw a close
' icon while navigated into a deep folder -- no longer this program's
' problem at all).
LET diskIconName[0] = 73
LET diskIconName[1] = 67
LET diskIconName[2] = 79
LET diskIconName[3] = 78
LET diskIconName[4] = 68
LET diskIconName[5] = 73
LET diskIconName[6] = 83
LET diskIconName[7] = 75
LET diskIconName[8] = 46
LET diskIconName[9] = 80
LET diskIconName[10] = 78
LET diskIconName[11] = 71

LET sw = SCREENW
LET sh = SCREENH

LET iconX = 20
LET iconY = 40
LET iconW = 48
LET iconH = 32
LET selected = 0

LET prevBtn = 0
LET lastClickTime = 0
LET DBLCLICK_TICKS = 40
LET needsRedraw = 1

WHILE 1
  IF KEYHIT THEN
    LET k = GETKEY
    IF k = 136 THEN END
  ENDIF

  LET mx = MOUSEX
  LET my = MOUSEY
  LET btn = MOUSEBTN
  LET leftDown = 0
  LET leftJustDown = 0
  IF btn = 1 THEN LET leftDown = 1
  IF btn = 3 THEN LET leftDown = 1
  IF btn = 5 THEN LET leftDown = 1
  IF btn = 7 THEN LET leftDown = 1
  LET prevLeftDown = 0
  IF prevBtn = 1 THEN LET prevLeftDown = 1
  IF prevBtn = 3 THEN LET prevLeftDown = 1
  IF prevBtn = 5 THEN LET prevLeftDown = 1
  IF prevBtn = 7 THEN LET prevLeftDown = 1
  IF leftDown = 1 THEN
    IF prevLeftDown = 0 THEN LET leftJustDown = 1
  ENDIF

  IF leftJustDown = 1 THEN
    IF mx >= iconX THEN
    IF mx < iconX + iconW THEN
    IF my >= iconY THEN
    IF my < iconY + iconH THEN
      IF selected = 1 THEN
        IF TIMER - lastClickTime < DBLCLICK_TICKS THEN
          LAUNCH filebrowserProgName, 16, argbuf, 0
        ENDIF
      ENDIF
      LET selected = 1
      LET lastClickTime = TIMER
      LET needsRedraw = 1
    ENDIF
    ENDIF
    ENDIF
    ENDIF
  ENDIF

  LET prevBtn = btn

  ' ---- Redraw (only when something actually changed -- see the
  ' needsRedraw header comment above) ----
  IF needsRedraw = 1 THEN
    CLS WBGRAY

    RECT 0, 0, sw, 20, WBBLUE
    RECT 0, 20, sw, 2, WBDARK
    DRAWTEXT 6, 3, "arOS64X Desktop", 16777215

    LET diskLoaded = LOADPNG(diskIconName, 12)
    IF diskLoaded = 1 THEN
      DRAWPNG iconX, iconY
    ELSE
      ' Fallback if the icon art isn't staged on this VHD -- same plain
      ' box the original Workbench always drew, so a missing icon file
      ' degrades gracefully instead of leaving a blank hole.
      RECT iconX, iconY, iconW, iconH, 16777215
      RECT iconX, iconY, iconW, 2, 0
      RECT iconX, iconY + iconH - 2, iconW, 2, 0
      RECT iconX, iconY, 2, iconH, 0
      RECT iconX + iconW - 2, iconY, 2, iconH, 0
    ENDIF
    DRAWTEXT iconX - 8, iconY + iconH + 6, "AROS64X", 0
    IF selected = 1 THEN
      RECT iconX - 3, iconY - 3, iconW + 6, 2, 0
      RECT iconX - 3, iconY + iconH + 1, iconW + 6, 2, 0
      RECT iconX - 3, iconY - 3, 2, iconH + 6, 0
      RECT iconX + iconW + 1, iconY - 3, 2, iconH + 6, 0
    ENDIF

    ' The mouse cursor itself is drawn by the compositor now (see
    ' wm_fb_draw_cursor, kernel.asm) -- a real overlay drawn last,
    ' after every window's own chrome+content, so it's always on top
    ' regardless of which window the pointer happens to be over.

    FLIP
    LET needsRedraw = 0
  ENDIF
  WAIT 2
WEND
