' arOS-X64 "Workbench" desktop shell.
' Phase 1: background, status bar, one disk icon, mouse cursor.
' Phase 2: click-to-select, double-click-to-open a window, window
' dragging by its title bar, click-to-close.
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

' Disk icon bounds
LET iconX = 20
LET iconY = 40
LET iconW = 48
LET iconH = 32
LET selected = 0

' Window state
LET winOpen = 0
LET winX = 120
LET winY = 60
LET winW = 320
LET winH = 220
LET titleH = 18
LET closeW = 14

' Drag state
LET dragging = 0
LET dragOffX = 0
LET dragOffY = 0

' Click/double-click tracking
LET prevBtn = 0
LET lastClickTime = 0
LET DBLCLICK_TICKS = 40   ' ~400ms at the 100Hz tick this kernel uses

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
  LET leftJustUp = 0
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
  IF leftDown = 0 THEN
    IF prevLeftDown = 1 THEN LET leftJustUp = 1
  ENDIF

  IF leftJustDown = 1 THEN
    ' Window title bar: start a drag
    IF winOpen = 1 THEN
      IF mx >= winX THEN
      IF mx < winX + winW THEN
      IF my >= winY THEN
      IF my < winY + titleH THEN
        IF mx >= winX + winW - closeW THEN
          LET winOpen = 0
        ELSE
          LET dragging = 1
          LET dragOffX = mx - winX
          LET dragOffY = my - winY
        ENDIF
      ENDIF
      ENDIF
      ENDIF
      ENDIF
    ENDIF

    ' Disk icon: select, or open on a double-click
    IF winOpen = 0 THEN
      IF mx >= iconX THEN
      IF mx < iconX + iconW THEN
      IF my >= iconY THEN
      IF my < iconY + iconH THEN
        IF selected = 1 THEN
          IF TIMER - lastClickTime < DBLCLICK_TICKS THEN
            LET winOpen = 1
          ENDIF
        ENDIF
        LET selected = 1
        LET lastClickTime = TIMER
      ENDIF
      ENDIF
      ENDIF
      ENDIF
    ENDIF
  ENDIF

  IF leftJustUp = 1 THEN LET dragging = 0
  IF dragging = 1 THEN
    LET winX = mx - dragOffX
    LET winY = my - dragOffY
  ENDIF

  LET prevBtn = btn

  ' ---- Redraw ----
  CLS 11184810

  RECT 0, 0, sw, 20, 8947660
  DRAWTEXT 6, 3, "arOS-X64 Workbench", 16777215

  RECT iconX, iconY, iconW, iconH, 16777215
  RECT iconX, iconY, iconW, 2, 0
  RECT iconX, iconY + iconH - 2, iconW, 2, 0
  RECT iconX, iconY, 2, iconH, 0
  RECT iconX + iconW - 2, iconY, 2, iconH, 0
  DRAWTEXT iconX - 8, iconY + iconH + 6, "AROSTEST", 0
  IF selected = 1 THEN
    IF winOpen = 0 THEN
      RECT iconX - 3, iconY - 3, iconW + 6, 2, 0
      RECT iconX - 3, iconY + iconH + 1, iconW + 6, 2, 0
      RECT iconX - 3, iconY - 3, 2, iconH + 6, 0
      RECT iconX + iconW + 1, iconY - 3, 2, iconH + 6, 0
    ENDIF
  ENDIF

  IF winOpen = 1 THEN
    RECT winX, winY, winW, winH, 11184810
    RECT winX, winY, winW, titleH, 8947660
    DRAWTEXT winX + 6, winY + 2, "AROSTEST", 16777215
    RECT winX + winW - closeW - 2, winY + 2, closeW, titleH - 4, 16777215
    RECT winX, winY, winW, 2, 0
    RECT winX, winY + winH - 2, winW, 2, 0
    RECT winX, winY, 2, winH, 0
    RECT winX + winW - 2, winY, 2, winH, 0
    DRAWTEXT winX + 8, winY + titleH + 8, "(folder browsing comes in Phase 3)", 0
  ENDIF

  LET curX = MOUSEX
  LET curY = MOUSEY
  RECT curX - 4, curY - 4, 8, 8, 0
  RECT curX - 3, curY - 3, 6, 6, 16777215

  FLIP
  WAIT 2
WEND
