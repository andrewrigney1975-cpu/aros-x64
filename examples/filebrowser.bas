' arOS-X64 file browser -- LAUNCHed by workbench.bas when the disk icon
' is double-clicked. A REAL compositor-managed window now (kernel-drawn
' chrome: title bar, border, close gadget, wm_tick drag+close) -- NOT
' hand-drawn the way this used to be as part of workbench.bas's own
' content. This file only draws its own CONTENT (the directory icon
' grid); the kernel handles everything chrome-related (see
' kernel/kernel.asm's compositor chrome_phase / wm_tick), the same way
' editor.bas/viewer.bas already do. workbench.bas used to hand-roll its
' own title-bar drag/close hit-testing and chrome drawing -- a second,
' independently-maintained implementation of the exact same thing
' LAUNCHed programs already got for free from the kernel, which drifted
' (close-button size/inset, colors) more than once before this split.
'
' Double-click a folder entry to drill in (same window, in place);
' double-click the ".." entry (shown whenever not at the disk's root)
' to go back up. Navigation shares exfat_cwd_cluster with the text
' shell's OPEN/UP/RUN/COMPILE and with any OTHER file-browser window --
' DIRCD/DIRUP are not a separate GUI-side concept, and this is a real,
' known limitation of allowing more than one browser window open at
' once (double-clicking the disk icon again always opens a new one,
' by design): two browser windows navigating at the same time will
' stomp each other's exfat_cwd_cluster state, since it's one global
' cursor shared by every task, not per-window. Not fixed here -- would
' need a real per-window cwd (a bigger change, out of scope for just
' turning this into a real window).
'
' Double-click a file entry to open it -- .BAS launches EDITOR.BAS,
' .PNG launches VIEWER.BAS (both passed this file's name as their
' LAUNCH argument, which they read via ARGLEN/ARGCHAR), .AXB launches
' straight into the compiled binary itself.
'
' WAIT 2 (~50fps cap) below is deliberate, not just pacing -- see
' workbench.bas's own identical comment on the PIC IRQ0 mitigation.

' DIM must come before anything that references these arrays in
' *source order* -- DIM's array-arena allocation happens at parse
' time (this is a single-pass compiler), not at runtime.
' entChars stores each entry's FULL name (up to 32 chars) even though
' only the first 12 are ever drawn -- truncating storage itself to the
' display cap would chop off longer filenames' extensions, breaking
' the double-click file-type check below, which looks at the last 4
' characters of the FULL name.
DIM entIsDir(40) AS INTEGER
DIM entChars(1280) AS INTEGER
DIM entNameLen(40) AS INTEGER
' entType: classified ONCE per entry, here in RESCAN -- the single
' source of truth for both icon selection (the render loop) and
' double-click file-type dispatch (the click handler), instead of
' each re-deriving it from the raw name characters independently (the
' exact "two places doing the same classification" pattern that's
' caused real drift bugs elsewhere in this codebase before).
' 0=other/unclassified file, 1=folder, 2=.BAS, 3=.PNG, 4=.AXB, 5=.TXT
DIM entType(40) AS INTEGER
DIM nameBuf(32) AS INTEGER
DIM editorProgName(16) AS INTEGER
DIM viewerProgName(16) AS INTEGER
DIM folderIconName(16) AS INTEGER
DIM fileIconName(16) AS INTEGER
DIM txtIconName(16) AS INTEGER
DIM basIconName(16) AS INTEGER
DIM pngIconName(16) AS INTEGER
DIM axbIconName(16) AS INTEGER

GOTO START

' ---- RESCAN: reads the current directory (DIROPEN/DIRNEXT) into the
' entXxx arrays, capped at 40 entries. Called once at startup and again
' after every DIRCD/DIRUP. ----
RESCAN:
  DIROPEN
  LET entCount = 0
  WHILE DIRNEXT
    IF entCount < 40 THEN
      LET entIsDir[entCount] = DIRISDIR
      LET nlen = DIRNAMELEN
      IF nlen > 32 THEN LET nlen = 32
      LET j = 0
      WHILE j < nlen
        LET entChars[entCount * 32 + j] = DIRNAMECHAR(j)
        LET j = j + 1
      WEND
      LET entNameLen[entCount] = nlen

      ' Classify by extension (last 4 chars, case-insensitive) --
      ' see entType's own comment above.
      LET entType[entCount] = 0
      IF entIsDir[entCount] = 1 THEN
        LET entType[entCount] = 1
      ELSE
        IF nlen >= 4 THEN
          LET c0 = entChars[entCount * 32 + nlen - 4]
          LET c1 = entChars[entCount * 32 + nlen - 3]
          LET c2 = entChars[entCount * 32 + nlen - 2]
          LET c3 = entChars[entCount * 32 + nlen - 1]
          IF c1 >= 97 THEN
          IF c1 <= 122 THEN LET c1 = c1 - 32
          ENDIF
          IF c2 >= 97 THEN
          IF c2 <= 122 THEN LET c2 = c2 - 32
          ENDIF
          IF c3 >= 97 THEN
          IF c3 <= 122 THEN LET c3 = c3 - 32
          ENDIF
          IF c0 = 46 THEN
            IF c1 = 66 THEN
            IF c2 = 65 THEN
            IF c3 = 83 THEN LET entType[entCount] = 2
            ENDIF
            ENDIF
            IF c1 = 80 THEN
            IF c2 = 78 THEN
            IF c3 = 71 THEN LET entType[entCount] = 3
            ENDIF
            ENDIF
            IF c1 = 65 THEN
            IF c2 = 88 THEN
            IF c3 = 66 THEN LET entType[entCount] = 4
            ENDIF
            ENDIF
            IF c1 = 84 THEN
            IF c2 = 88 THEN
            IF c3 = 84 THEN LET entType[entCount] = 5
            ENDIF
            ENDIF
          ENDIF
        ENDIF
      ENDIF

      LET entCount = entCount + 1
    ENDIF
  WEND
  RETURN

START:

' "/EDITOR.BAS" and "/VIEWER.BAS" -- leading slash forces root-relative
' resolution (exfat_resolve_path) regardless of how deep DIRCD has
' navigated, since both programs live at the disk's root.
LET editorProgName[0] = 47
LET editorProgName[1] = 69
LET editorProgName[2] = 68
LET editorProgName[3] = 73
LET editorProgName[4] = 84
LET editorProgName[5] = 79
LET editorProgName[6] = 82
LET editorProgName[7] = 46
LET editorProgName[8] = 66
LET editorProgName[9] = 65
LET editorProgName[10] = 83

LET viewerProgName[0] = 47
LET viewerProgName[1] = 86
LET viewerProgName[2] = 73
LET viewerProgName[3] = 69
LET viewerProgName[4] = 87
LET viewerProgName[5] = 69
LET viewerProgName[6] = 82
LET viewerProgName[7] = 46
LET viewerProgName[8] = 66
LET viewerProgName[9] = 65
LET viewerProgName[10] = 83

' Per-file-type icon art (bare, cwd-relative -- see workbench.bas's
' own identical comment on why LOADPNG needs bare names, not root-
' relative ones, until basix_resolve_dir_path's leading-'/' bug is
' fixed everywhere it's used). Generated by scripts/gen_icons.py,
' same bevel-edged "chunky 3D" convention as every other beveled
' surface in this UI (light top/left, dark bottom/right) instead of a
' flat black outline.
LET folderIconName[0] = 73
LET folderIconName[1] = 67
LET folderIconName[2] = 79
LET folderIconName[3] = 78
LET folderIconName[4] = 70
LET folderIconName[5] = 79
LET folderIconName[6] = 76
LET folderIconName[7] = 68
LET folderIconName[8] = 69
LET folderIconName[9] = 82
LET folderIconName[10] = 46
LET folderIconName[11] = 80
LET folderIconName[12] = 78
LET folderIconName[13] = 71

LET fileIconName[0] = 73
LET fileIconName[1] = 67
LET fileIconName[2] = 79
LET fileIconName[3] = 78
LET fileIconName[4] = 70
LET fileIconName[5] = 73
LET fileIconName[6] = 76
LET fileIconName[7] = 69
LET fileIconName[8] = 46
LET fileIconName[9] = 80
LET fileIconName[10] = 78
LET fileIconName[11] = 71

LET txtIconName[0] = 73
LET txtIconName[1] = 67
LET txtIconName[2] = 79
LET txtIconName[3] = 78
LET txtIconName[4] = 84
LET txtIconName[5] = 88
LET txtIconName[6] = 84
LET txtIconName[7] = 46
LET txtIconName[8] = 80
LET txtIconName[9] = 78
LET txtIconName[10] = 71

LET basIconName[0] = 73
LET basIconName[1] = 67
LET basIconName[2] = 79
LET basIconName[3] = 78
LET basIconName[4] = 66
LET basIconName[5] = 65
LET basIconName[6] = 83
LET basIconName[7] = 46
LET basIconName[8] = 80
LET basIconName[9] = 78
LET basIconName[10] = 71

LET pngIconName[0] = 73
LET pngIconName[1] = 67
LET pngIconName[2] = 79
LET pngIconName[3] = 78
LET pngIconName[4] = 80
LET pngIconName[5] = 78
LET pngIconName[6] = 71
LET pngIconName[7] = 46
LET pngIconName[8] = 80
LET pngIconName[9] = 78
LET pngIconName[10] = 71

LET axbIconName[0] = 73
LET axbIconName[1] = 67
LET axbIconName[2] = 79
LET axbIconName[3] = 78
LET axbIconName[4] = 65
LET axbIconName[5] = 88
LET axbIconName[6] = 66
LET axbIconName[7] = 46
LET axbIconName[8] = 80
LET axbIconName[9] = 78
LET axbIconName[10] = 71

' Own client-area size (SCREENW/SCREENH already return a windowed
' task's own client size, not the real screen's -- see basix_rt_screenw
' /basix_rt_screenh, basix_runtime.inc).
LET winW = SCREENW
LET winH = SCREENH

LET gridCols = 6
LET cellW = 95
LET cellH = 58
LET iconBoxW = 40
LET iconBoxH = 28
LET maxRows = winH / cellH
LET maxSlots = gridCols * maxRows

LET navDepth = 0
LET entCount = 0

LET prevBtn = 0
LET winLastClickTime = 0
LET winLastClickIdx = -1
LET DBLCLICK_TICKS = 40

GOSUB RESCAN

WHILE 1
  IF WINCLOSE THEN
    ' Bring the REAL exFAT cwd back to root before closing -- DIRCD
    ' pushed onto exfat_cwd_cluster/exfat_cwd_stack each level down;
    ' leaving those untouched would desync the next RESCAN (in this or
    ' any other still-open browser window) from where DIROPEN/DIRNEXT
    ' actually reads.
    WHILE navDepth > 0
      DIRUP
      LET navDepth = navDepth - 1
    WEND
    END
  ENDIF

  ' MOUSEX/MOUSEY are always SCREEN coordinates -- WINX/WINY (this
  ' window's own screen offset) converts them into the LOCAL
  ' coordinates everything below is drawn in, same as any other
  ' RECT/DRAWTEXT/etc call in this file already is.
  LET mx = MOUSEX - WINX
  LET my = MOUSEY - WINY
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
    LET gx = 10
    LET gy = 8
    IF mx >= gx THEN
    IF my >= gy THEN
    IF mx < winW - 10 THEN
    IF my < winH - 10 THEN
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
                LET p = 0
                WHILE p < entNameLen[ei]
                  LET nameBuf[p] = entChars[ei * 32 + p]
                  LET p = p + 1
                WEND

                ' entType[ei] was already classified once, in RESCAN
                ' (single source of truth -- see its own comment) --
                ' LAUNCH the right viewer, passing this file's own
                ' name as its argument (both resolve relative to the
                ' current directory, which LAUNCH never changes).
                IF entType[ei] = 1 THEN
                  DIRCD nameBuf, entNameLen[ei]
                  LET navDepth = navDepth + 1
                  GOSUB RESCAN
                ENDIF
                IF entType[ei] = 2 THEN LAUNCH editorProgName, 11, nameBuf, entNameLen[ei]
                IF entType[ei] = 3 THEN LAUNCH viewerProgName, 11, nameBuf, entNameLen[ei]
                IF entType[ei] = 4 THEN LAUNCH nameBuf, entNameLen[ei], nameBuf, 0
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

  LET prevBtn = btn

  ' ---- Redraw ----
  CLS WBGRAY

  LET gx = 10
  LET gy = 8
  LET first = 0
  IF navDepth > 0 THEN LET first = 1

  LET drawIdx = 0
  IF first = 1 THEN
    LET ux = gx + 0 * cellW
    LET uy = gy + 0 * cellH
    LET iconLoaded = LOADPNG(folderIconName, 14)
    IF iconLoaded = 1 THEN
      DRAWPNG ux, uy
    ELSE
      RECT ux, uy, iconBoxW, iconBoxH, 16777215
      RECT ux, uy, iconBoxW, 2, 0
      RECT ux, uy + iconBoxH - 2, iconBoxW, 2, 0
      RECT ux, uy, 2, iconBoxH, 0
      RECT ux + iconBoxW - 2, uy, 2, iconBoxH, 0
    ENDIF
    DRAWCHAR ux + 4, uy + iconBoxH + 4, 46, 0
    DRAWCHAR ux + 11, uy + iconBoxH + 4, 46, 0
    LET drawIdx = 1
  ENDIF

  ' Grouped by TYPE (one LOADPNG, then a DRAWPNG per matching entry --
  ' at most 6 decodes/frame instead of up to 40), with the WHOLE batch
  ' held under PNGLOCK/PNGUNLOCK so another task's own concurrent
  ' LOADPNG (workbench.bas redraws its own desktop icon every ~20ms)
  ' can't swap out the shared decoded-image slot mid-batch -- see
  ' basix_rt_pnglock's own comment (kernel/png.inc).
  '
  ' This went through TWO other versions first, both wrong in
  ' different ways -- worth remembering if this ever needs touching
  ' again: (1) originally grouped by type with NO lock, which decoded
  ' fast but let workbench.bas's own concurrent LOADPNG corrupt the
  ' shared slot mid-batch (wrong icons drawn); (2) switched to one
  ' LOADPNG immediately followed by its own DRAWPNG per entry (up to
  ' 40 decodes/frame, no batching at all) to dodge that, which fixed
  ' the wrong-icon corruption but made this window's own single
  ' redraw pass take long enough (many scheduler quantums) that the
  ' COMPOSITOR itself would blit this window's back buffer mid-
  ' redraw whenever workbench.bas's own independent FLIP triggered a
  ' pass -- showing whatever partial subset of icons happened to be
  ' drawn so far, changing every time (a much stranger bug: neither
  ' this program nor its own dirty-rect tracking did anything wrong,
  ' the buffer really was mid-update when something ELSE'S dirty
  ' notification caused it to get read). Both looked completely
  ' stable in headless tests that only ever ran this file ALONE, with
  ' no concurrent workbench.bas also redrawing -- and both flickered
  ' for real once actually launched from it interactively. This
  ' PNGLOCK-held, type-grouped version is short (fast redraw, low
  ' tear-window) AND safe (lock-protected against cross-task
  ' corruption) at the same time.
  LET passType = 1
  LET passNum = 0
  WHILE passNum < 6
    PNGLOCK
    LET iconLoaded = 0
    IF passType = 1 THEN LET iconLoaded = LOADPNG(folderIconName, 14)
    IF passType = 2 THEN LET iconLoaded = LOADPNG(basIconName, 11)
    IF passType = 3 THEN LET iconLoaded = LOADPNG(pngIconName, 11)
    IF passType = 4 THEN LET iconLoaded = LOADPNG(axbIconName, 11)
    IF passType = 5 THEN LET iconLoaded = LOADPNG(txtIconName, 11)
    IF passType = 0 THEN LET iconLoaded = LOADPNG(fileIconName, 12)

    LET ei = 0
    WHILE ei < entCount
      IF entType[ei] = passType THEN
        LET didx = drawIdx + ei
        IF didx < maxSlots THEN
          LET dcol = didx MOD gridCols
          LET drow = didx / gridCols
          LET ex = gx + dcol * cellW
          LET ey = gy + drow * cellH

          ' Same plain-RECT fallback the desktop disk icon already
          ' uses if the art isn't staged on this VHD, so a missing
          ' icon file degrades gracefully instead of showing nothing.
          IF iconLoaded = 1 THEN
            DRAWPNG ex, ey
          ELSE
            LET ifill = 16777215
            IF passType = 1 THEN LET ifill = 11184810
            RECT ex, ey, iconBoxW, iconBoxH, ifill
            RECT ex, ey, iconBoxW, 2, 0
            RECT ex, ey + iconBoxH - 2, iconBoxW, 2, 0
            RECT ex, ey, 2, iconBoxH, 0
            RECT ex + iconBoxW - 2, ey, 2, iconBoxH, 0
          ENDIF

          LET dispLen = entNameLen[ei]
          IF dispLen > 12 THEN LET dispLen = 12
          LET nk = 0
          WHILE nk < dispLen
            DRAWCHAR ex + nk * 6, ey + iconBoxH + 4, entChars[ei * 32 + nk], 0
            LET nk = nk + 1
          WEND
        ENDIF
      ENDIF
      LET ei = ei + 1
    WEND
    PNGUNLOCK

    LET passType = passType + 1
    IF passType > 5 THEN LET passType = 0
    LET passNum = passNum + 1
  WEND

  FLIP
  WAIT 2
WEND
