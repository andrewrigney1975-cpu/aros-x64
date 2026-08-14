' arOS-X64 "Workbench" desktop shell.
' Phase 1: background, status bar, one disk icon, mouse cursor.
' Phase 2: click-to-select, double-click-to-open a window, window
' dragging by its title bar, click-to-close.
' Phase 3: the window shows the real exFAT directory it represents;
' double-click a folder entry to drill in (same window, in place);
' double-click the ".." entry (shown whenever not at the disk's root)
' to go back up. Navigation shares exfat_cwd_cluster with the text
' shell's OPEN/UP/RUN/COMPILE -- DIRCD/DIRUP are not a separate GUI-
' side concept.
'
' WAIT 2 (~50fps cap) below is deliberate, not just pacing: an
' uncapped CLS/RECT/DRAWTEXT/FLIP loop is the heaviest sustained
' interrupt/stack load anything in this codebase has produced so far,
' and it's been observed to eventually wedge the master PIC (a stuck
' IRQ0 in-service bit that blocks IRQ1/keyboard -- a real, still-
' unfixed kernel bug, not specific to this program). Capping the frame
' rate is a mitigation, not a fix -- remove it once the underlying
' interrupt-handling bug is actually root-caused.

' DIM must come before anything that references these arrays in
' *source order* -- DIM's array-arena allocation happens at parse
' time (this is a single-pass compiler), not at runtime, so a GOTO
' skipping past RESCAN below does NOT help if RESCAN's body were
' parsed before these DIMs. Keep all DIMs first.
DIM entIsDir(40) AS INTEGER
DIM entChars(480) AS INTEGER
DIM entNameLen(40) AS INTEGER
DIM navName(16) AS INTEGER

GOTO START

' ---- RESCAN: reads the current directory (DIROPEN/DIRNEXT) into the
' entXxx arrays, capped at MAXENT entries. Called once when the window
' opens and again after every DIRCD/DIRUP. ----
RESCAN:
  DIROPEN
  LET entCount = 0
  WHILE DIRNEXT
    IF entCount < 40 THEN
      LET entIsDir[entCount] = DIRISDIR
      LET nlen = DIRNAMELEN
      IF nlen > 12 THEN LET nlen = 12
      LET j = 0
      WHILE j < nlen
        LET entChars[entCount * 12 + j] = DIRNAMECHAR(j)
        LET j = j + 1
      WEND
      LET entNameLen[entCount] = nlen
      LET entCount = entCount + 1
    ENDIF
  WEND
  RETURN

START:

LET sw = SCREENW
LET sh = SCREENH

LET iconX = 20
LET iconY = 40
LET iconW = 48
LET iconH = 32
LET selected = 0

LET winOpen = 0
LET winX = 100
LET winY = 50
LET winW = 420
LET winH = 300
LET titleH = 18
LET closeW = 14

LET gridCols = 4
LET cellW = 95
LET cellH = 58
LET iconBoxW = 40
LET iconBoxH = 28
LET maxRows = (winH - titleH - 18) / cellH
LET maxSlots = gridCols * maxRows

LET navDepth = 0
LET entCount = 0

LET dragging = 0
LET dragOffX = 0
LET dragOffY = 0

LET prevBtn = 0
LET lastClickTime = 0
LET winLastClickTime = 0
LET winLastClickIdx = -1
LET DBLCLICK_TICKS = 40

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
    IF winOpen = 1 THEN
      ' Title bar: drag, or close
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

      ' Content area: hit-test the icon grid
      LET gx = winX + 10
      LET gy = winY + titleH + 8
      IF mx >= gx THEN
      IF my >= gy THEN
      IF mx < winX + winW - 10 THEN
      IF my < winY + winH - 10 THEN
        LET ccol = (mx - gx) / cellW
        LET crow = (my - gy) / cellH
        LET cidx = crow * gridCols + ccol
        LET first = 0
        IF navDepth > 0 THEN LET first = 1
        LET clickedSomething = 0
        IF cidx < maxSlots THEN
          IF first = 1 THEN
            IF cidx = 0 THEN LET clickedSomething = 1
          ENDIF
          IF cidx >= first THEN
            IF cidx - first < entCount THEN LET clickedSomething = 1
          ENDIF
        ENDIF

        IF clickedSomething = 1 THEN
          IF winLastClickIdx = cidx THEN
            IF TIMER - winLastClickTime < DBLCLICK_TICKS THEN
              IF first = 1 THEN
                IF cidx = 0 THEN
                  DIRUP
                  LET navDepth = navDepth - 1
                  GOSUB RESCAN
                ENDIF
              ENDIF
              IF cidx >= first THEN
                LET ei = cidx - first
                IF ei < entCount THEN
                  IF entIsDir[ei] = 1 THEN
                    LET p = 0
                    WHILE p < entNameLen[ei]
                      LET navName[p] = entChars[ei * 12 + p]
                      LET p = p + 1
                    WEND
                    DIRCD navName, entNameLen[ei]
                    LET navDepth = navDepth + 1
                    GOSUB RESCAN
                  ENDIF
                ENDIF
              ENDIF
            ENDIF
          ENDIF
          LET winLastClickIdx = cidx
          LET winLastClickTime = TIMER
        ENDIF
      ENDIF
      ENDIF
      ENDIF
      ENDIF
    ENDIF

    IF winOpen = 0 THEN
      IF mx >= iconX THEN
      IF mx < iconX + iconW THEN
      IF my >= iconY THEN
      IF my < iconY + iconH THEN
        IF selected = 1 THEN
          IF TIMER - lastClickTime < DBLCLICK_TICKS THEN
            LET winOpen = 1
            LET navDepth = 0
            GOSUB RESCAN
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

    LET gx = winX + 10
    LET gy = winY + titleH + 8
    LET first = 0
    IF navDepth > 0 THEN LET first = 1

    LET drawIdx = 0
    IF first = 1 THEN
      LET ux = gx + 0 * cellW
      LET uy = gy + 0 * cellH
      RECT ux, uy, iconBoxW, iconBoxH, 16777215
      RECT ux, uy, iconBoxW, 2, 0
      RECT ux, uy + iconBoxH - 2, iconBoxW, 2, 0
      RECT ux, uy, 2, iconBoxH, 0
      RECT ux + iconBoxW - 2, uy, 2, iconBoxH, 0
      DRAWCHAR ux + 4, uy + iconBoxH + 4, 46, 0
      DRAWCHAR ux + 11, uy + iconBoxH + 4, 46, 0
      LET drawIdx = 1
    ENDIF

    LET ei = 0
    WHILE ei < entCount
      LET didx = drawIdx + ei
      IF didx < maxSlots THEN
        LET dcol = didx MOD gridCols
        LET drow = didx / gridCols
        LET ex = gx + dcol * cellW
        LET ey = gy + drow * cellH
        LET ifill = 16777215
        IF entIsDir[ei] = 1 THEN LET ifill = 11184810
        RECT ex, ey, iconBoxW, iconBoxH, ifill
        RECT ex, ey, iconBoxW, 2, 0
        RECT ex, ey + iconBoxH - 2, iconBoxW, 2, 0
        RECT ex, ey, 2, iconBoxH, 0
        RECT ex + iconBoxW - 2, ey, 2, iconBoxH, 0

        LET nk = 0
        WHILE nk < entNameLen[ei]
          DRAWCHAR ex + nk * 6, ey + iconBoxH + 4, entChars[ei * 12 + nk], 0
          LET nk = nk + 1
        WEND
      ENDIF

      LET ei = ei + 1
    WEND

    RECT winX, winY, winW, 2, 0
    RECT winX, winY + winH - 2, winW, 2, 0
    RECT winX, winY, 2, winH, 0
    RECT winX + winW - 2, winY, 2, winH, 0
  ENDIF

  LET curX = MOUSEX
  LET curY = MOUSEY
  RECT curX - 4, curY - 4, 8, 8, 0
  RECT curX - 3, curY - 3, 6, 6, 16777215

  FLIP
  WAIT 2
WEND
