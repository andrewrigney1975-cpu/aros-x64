' PNG image viewer for arOS-X64/BASIX64.
'
' R3: rewritten to draw everything -- header text AND the image --
' through the same pixel back buffer (CLS/DRAWTEXT/DRAWCHAR/DRAWPNG/
' FLIP), instead of the old hybrid design (image via the back buffer +
' FLIP, header text via the text console straight to the real
' framebuffer, drawn AFTER FLIP on the assumption that FLIP presented
' synchronously). That assumption predates R1's compositor: FLIP no
' longer blits anything itself (see basix_rt_flip's own comment) --
' it's just a marker that presentation has decoupled from any one
' program's own execution. The header text, having its own draw with
' no dirty-rect tracking, could get silently erased whenever the
' compositor's still-pending blit of THIS program's own (image-only)
' back buffer landed after the header text was drawn -- the exact
' race documented in kernel/basix_rbx_clobber_bug.md's
' [[project_r3_gui_editor]] note (found there via editor.bas's own
' exit-cleanup attempt, but this hybrid design had the identical race
' built into EVERY normal redraw, not just on exit). Putting
' everything into one back buffer, presented with a single FLIP at
' the end of one redraw, is what removes the race -- same fix
' editor.bas already got.
'
' Same top-of-screen menuing style as editor.bas: a persistent ESC=Menu
' bar, a filename/status row, a hints row, and a full-width divider,
' all pinned to rows 0-4. The image itself is drawn below that (row 5's
' pixel Y, i.e. CONTENT_Y) via LOADPNG/DRAWPNG -- decoding and blitting
' both happen in the kernel (see kernel/png.inc), not in BASIX64 itself,
' since this language has no string type and per-pixel BASIC loops over
' a multi-megapixel image would be far too slow anyway.
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

' Pixel-cursor state (see editor.bas's identical header comment) --
' curX/curY are derived from row/col wherever those change, never
' advanced on their own; curcolor replaces the old ambient COLOR.
DIM curX AS INTEGER
DIM curY AS INTEGER
DIM curcolor AS INTEGER
DIM row AS INTEGER
DIM col AS INTEGER

' draw_int's own scratch (see below) -- DRAWTEXT only takes a literal
' string, unlike PRINT's mixed string/int concatenation, so showing
' "(1234 x 5678)" needs its own decimal-digit drawing loop.
DIM di_n AS INTEGER
DIM di_tmp AS INTEGER
DIM di_len AS INTEGER
DIM di_i AS INTEGER
DIM di_buf(12) AS INTEGER

DIM COL_WHITE AS INTEGER

LET cols = TEXTCOLS
LET rows = TEXTROWS
LET fnamelen = 0
LET loaded = 0
LET imgw = 0
LET imgh = 0
LET exitreq = 0
LET CHAR_H = 16
LET CONTENT_Y = 5 * CHAR_H
LET COL_WHITE = 16777215

' If launched with an argument (e.g. Workbench double-clicking a .PNG
' icon -- see LAUNCH/ARGLEN/ARGCHAR), treat it as a filename to open
' immediately, same LOADPNG call the L=Load menu action uses. ARGLEN
' is 0 when started via plain RUN, so this is a no-op then.
IF ARGLEN > 0 THEN
  LET fnamelen = ARGLEN
  IF fnamelen > 64 THEN LET fnamelen = 64
  LET i = 0
  WHILE i < fnamelen
    LET fname[i] = ARGCHAR(i)
    LET i = i + 1
  WEND
  LET loadok = LOADPNG(fname, fnamelen)
  IF loadok = 1 THEN
    LET loaded = 1
    LET imgw = PNGWIDTH
    LET imgh = PNGHEIGHT
  ENDIF
ENDIF

main_loop:
GOSUB redraw

' Poll WINCLOSE (the kernel-drawn close gadget) alongside KEYHIT
' instead of blocking straight on GETKEY -- see editor.bas's identical
' comment for why.
wait_key:
IF WINCLOSE THEN END
IF KEYHIT = 0 THEN
  WAIT 1
  GOTO wait_key
ENDIF
LET k = GETKEY

IF k = 136 THEN
  GOSUB do_menu
  IF exitreq = 1 THEN END
ENDIF
GOTO main_loop

END

' -----------------------------------------------------------------------
' putch: draws ch at the current pixel cursor (curX, curY) in
' curcolor, then advances col (and curX, derived from it) by one cell.
' -----------------------------------------------------------------------
putch:
DRAWCHAR curX, curY, ch, curcolor
LET col = col + 1
LET curX = col * 8
RETURN

' -----------------------------------------------------------------------
' draw_int: input di_n (>= 0). Draws its decimal digits at the current
' pixel cursor in curcolor, advancing col/curX as it goes -- DRAWTEXT's
' replacement for PRINT's built-in int-to-text conversion, which it
' doesn't have (a literal string only).
' -----------------------------------------------------------------------
draw_int:
IF di_n = 0 THEN
  LET ch = 48
  GOSUB putch
  RETURN
ENDIF
LET di_len = 0
LET di_tmp = di_n
WHILE di_tmp > 0
  LET di_buf[di_len] = di_tmp MOD 10
  LET di_tmp = di_tmp / 10
  LET di_len = di_len + 1
WEND
LET di_i = di_len - 1
WHILE di_i >= 0
  LET ch = 48 + di_buf[di_i]
  GOSUB putch
  LET di_i = di_i - 1
WEND
RETURN

' -----------------------------------------------------------------------
' redraw: CLS, then (if loaded) DRAWPNG, then the header text on top --
' all into the same back buffer -- then one FLIP presents the whole
' frame at once. Order no longer matters for correctness the way it
' used to (draw image first or the old FLIP-then-text would erase the
' header) since nothing is presented until the single FLIP at the end;
' drawing the image first is kept anyway so the header always ends up
' visually on top of it, same as before.
' -----------------------------------------------------------------------
redraw:
CLS 0
IF loaded = 1 THEN
  DRAWPNG 0, CONTENT_Y
ENDIF

LET curcolor = COL_WHITE
LET row = 0
LET col = 0
LET curX = 0
LET curY = 0
DRAWTEXT curX, curY, "ESC=Menu  L=Load  X=Exit", curcolor

LET row = 1
LET col = 0
LET curX = 0
LET curY = 16
LET i = 0
WHILE i < fnamelen
  LET ch = fname[i]
  GOSUB putch
  LET i = i + 1
WEND
IF fnamelen = 0 THEN
  DRAWTEXT curX, curY, "(no image loaded)", curcolor
ELSE
  IF loaded = 1 THEN
    DRAWTEXT curX, curY, "  (", curcolor
    LET col = col + 3
    LET curX = col * 8
    LET di_n = imgw
    GOSUB draw_int
    DRAWTEXT curX, curY, " x ", curcolor
    LET col = col + 3
    LET curX = col * 8
    LET di_n = imgh
    GOSUB draw_int
    DRAWTEXT curX, curY, ")", curcolor
  ELSE
    DRAWTEXT curX, curY, "  (failed to load -- not a supported PNG)", curcolor
  ENDIF
ENDIF

LET row = 2
LET col = 0
LET curX = 0
LET curY = 32
DRAWTEXT curX, curY, "View-only -- larger-than-screen images are clipped, not scrolled", curcolor

RECT 0, 48, cols * 8, 2, COL_WHITE

FLIP
RETURN

' -----------------------------------------------------------------------
' prompt_filename: reads a filename (typed on the top row) into
' fname/fnamelen. Sets pfcancel = 1 on ESC instead of Enter. Draws
' incrementally (one FLIP per keystroke), same as editor.bas's version
' -- there's no outer redraw loop between GETKEY calls here to present
' each character/backspace on its own.
' -----------------------------------------------------------------------
prompt_filename:
RECT 0, 0, cols * 8, 16, 0
LET curcolor = COL_WHITE
LET curX = 0
LET curY = 0
DRAWTEXT curX, curY, "Filename: ", curcolor
LET col = 10
LET curX = 80
LET fnamelen = 0
LET pfcancel = 0
FLIP
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
    LET col = 10 + fnamelen
    LET curX = col * 8
    RECT curX, curY, 8, 16, 0
    FLIP
  ENDIF
  GOTO pf_loop
ENDIF
IF k2 < 32 THEN GOTO pf_loop
IF k2 > 126 THEN GOTO pf_loop
IF fnamelen >= 63 THEN GOTO pf_loop
LET fname[fnamelen] = k2
LET fnamelen = fnamelen + 1
LET ch = k2
LET col = 10 + fnamelen - 1
LET curX = col * 8
DRAWCHAR curX, curY, ch, curcolor
LET col = 10 + fnamelen
LET curX = col * 8
FLIP
GOTO pf_loop
pf_done:
RETURN

' -----------------------------------------------------------------------
' do_menu: the ESC-triggered Load/Exit menu.
' -----------------------------------------------------------------------
do_menu:
RECT 0, 0, cols * 8, 16, 0
LET curcolor = COL_WHITE
LET curX = 0
LET curY = 0
DRAWTEXT curX, curY, "L=Load  X=Exit  ESC=Cancel", curcolor
FLIP
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
