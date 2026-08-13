' PNG image viewer for arOS-X64/BASIX64.
'
' Same top-of-screen menuing style as editor.bas: a persistent ESC=Menu
' bar, a filename/status row, a hints row, and a full-width '=' divider,
' all pinned to rows 0-4. The image itself is drawn below that (row 5's
' pixel Y, i.e. CONTENT_Y) via LOADPNG/DRAWPNG -- decoding and blitting
' both happen in the kernel (see kernel/png.inc), not in BASIX64 itself,
' since this language has no string type and per-pixel BASIC loops over
' a multi-megapixel image would be far too slow anyway.
'
' Text (PUTCHAR/PRINT) draws straight onto the real, visible framebuffer.
' The image instead goes through the graphics back buffer (DRAWPNG write
' + FLIP to present, same as PSET/LINE), which is a *full-screen* blit --
' so every redraw must FLIP first and draw the header text second, or
' FLIP would erase it. See redraw below.
'
' If the loaded image is larger than the content area, it's simply
' clipped (DRAWPNG stops at the back buffer's own edges) -- no scrolling
' in this initial build.

DIM fname(64) AS INTEGER
DIM fnamelen AS INTEGER

DIM cols AS INTEGER
DIM rows AS INTEGER
DIM i AS INTEGER
DIM j AS INTEGER
DIM ch AS INTEGER
DIM k AS INTEGER
DIM k2 AS INTEGER

DIM loaded AS INTEGER
DIM imgw AS INTEGER
DIM imgh AS INTEGER
DIM loadok AS INTEGER
DIM pfcancel AS INTEGER
DIM exitreq AS INTEGER

DIM CHAR_H AS INTEGER
DIM CONTENT_Y AS INTEGER

LET cols = TEXTCOLS
LET rows = TEXTROWS
LET fnamelen = 0
LET loaded = 0
LET imgw = 0
LET imgh = 0
LET exitreq = 0
LET CHAR_H = 16
LET CONTENT_Y = 5 * CHAR_H

GOSUB clear_all

main_loop:
GOSUB redraw
LET k = GETKEY

IF k = 136 THEN
  GOSUB do_menu
  IF exitreq = 1 THEN
    GOSUB clear_all
    LOCATE 0, 0
    END
  ENDIF
ENDIF
GOTO main_loop

END

' -----------------------------------------------------------------------
' clear_all: blanks every text-console row.
' -----------------------------------------------------------------------
clear_all:
LET i = 0
WHILE i < rows
  LOCATE i, 0
  LET j = 0
  WHILE j < cols
    PUTCHAR 32
    LET j = j + 1
  WEND
  LET i = i + 1
WEND
RETURN

' -----------------------------------------------------------------------
' clear_header: blanks only rows 0-4 (the fixed header) -- unlike
' clear_all, this deliberately leaves the image content area (rows
' 5..rows-1) alone, since it was just FLIPped from the graphics back
' buffer and a PUTCHAR-drawn space there would blacken image pixels.
' -----------------------------------------------------------------------
clear_header:
LET i = 0
WHILE i < 5
  LOCATE i, 0
  LET j = 0
  WHILE j < cols
    PUTCHAR 32
    LET j = j + 1
  WEND
  LET i = i + 1
WEND
RETURN

' -----------------------------------------------------------------------
' redraw: CLS + (if loaded) DRAWPNG + FLIP presents the image (or a
' blank screen) first, since FLIP overwrites the whole visible
' framebuffer; the header text is drawn second, directly on top.
' -----------------------------------------------------------------------
redraw:
CLS 0
IF loaded = 1 THEN
  DRAWPNG 0, CONTENT_Y
ENDIF
FLIP

GOSUB clear_header

LOCATE 0, 0
PRINT "ESC=Menu  L=Load  X=Exit"

LOCATE 1, 0
LET i = 0
WHILE i < fnamelen
  LET ch = fname[i]
  PUTCHAR ch
  LET i = i + 1
WEND
IF fnamelen = 0 THEN
  PRINT "(no image loaded)"
ELSE
  IF loaded = 1 THEN
    PRINT "  (", imgw, " x ", imgh, ")"
  ELSE
    PRINT "  (failed to load -- not a supported PNG)"
  ENDIF
ENDIF

LOCATE 2, 0
PRINT "View-only -- larger-than-screen images are clipped, not scrolled"

LOCATE 3, 0
LET i = 0
WHILE i < cols
  PUTCHAR 61
  LET i = i + 1
WEND
RETURN

' -----------------------------------------------------------------------
' prompt_filename: reads a filename (typed on the top row) into
' fname/fnamelen. Sets pfcancel = 1 on ESC instead of Enter.
' -----------------------------------------------------------------------
prompt_filename:
LOCATE 0, 0
LET i = 0
WHILE i < cols
  PUTCHAR 32
  LET i = i + 1
WEND
LOCATE 0, 0
PRINT "Filename: "
LOCATE 0, 10
LET fnamelen = 0
LET pfcancel = 0
pf_loop:
LET k2 = GETKEY
IF k2 = 13 THEN GOTO pf_done
IF k2 = 136 THEN
  LET pfcancel = 1
  GOTO pf_done
ENDIF
IF k2 = 8 THEN
  IF fnamelen > 0 THEN
    LET fnamelen = fnamelen - 1
    LOCATE 0, 10 + fnamelen
    PUTCHAR 32
    LOCATE 0, 10 + fnamelen
  ENDIF
  GOTO pf_loop
ENDIF
IF k2 < 32 THEN GOTO pf_loop
IF k2 > 126 THEN GOTO pf_loop
IF fnamelen >= 63 THEN GOTO pf_loop
LET fname[fnamelen] = k2
LET fnamelen = fnamelen + 1
PUTCHAR k2
GOTO pf_loop
pf_done:
RETURN

' -----------------------------------------------------------------------
' do_menu: the ESC-triggered Load/Exit menu.
' -----------------------------------------------------------------------
do_menu:
LOCATE 0, 0
LET i = 0
WHILE i < cols
  PUTCHAR 32
  LET i = i + 1
WEND
LOCATE 0, 0
PRINT "L=Load  X=Exit  ESC=Cancel"
LET k2 = GETKEY

IF k2 = 136 THEN RETURN

IF k2 = 88 OR k2 = 120 THEN
  LET exitreq = 1
  RETURN
ENDIF

IF k2 = 76 OR k2 = 108 THEN
  GOSUB prompt_filename
  IF pfcancel = 0 THEN
    IF fnamelen > 0 THEN
      LET loadok = LOADPNG(fname, fnamelen)
      IF loadok = 1 THEN
        LET loaded = 1
        LET imgw = PNGWIDTH
        LET imgh = PNGHEIGHT
      ELSE
        LET loaded = 0
      ENDIF
    ENDIF
  ENDIF
  RETURN
ENDIF

RETURN
