' Simple text editor for arOS-X64/BASIX64.
'
' Load/Save/Save As/Exit are reached through an ESC-triggered modal
' menu (there's no mouse on this OS, and no other key the keyboard
' driver decodes safely without shadowing normal typing -- see
' keyboard.inc). Normal typing inserts at the cursor; arrow keys,
' Home/End, Backspace and Delete navigate/edit exactly like a
' conventional single-line-at-a-time text editor.
'
' The whole file lives as one flat int array (one BASIX64 "char" per
' array element, since this language has no string type), with
' embedded newline bytes (10) marking line breaks -- the same layout
' FSAVE/FLOAD round-trip to/from the exFAT volume as plain bytes.
' Editing up to a 1MB file is supported (buf's declared size).

DIM buf(1048576) AS INTEGER
DIM fname(64) AS INTEGER

DIM cols AS INTEGER
DIM rows AS INTEGER
DIM buflen AS INTEGER
DIM cur AS INTEGER
DIM fnamelen AS INTEGER
DIM modified AS INTEGER
DIM exitreq AS INTEGER

DIM k AS INTEGER
DIM k2 AS INTEGER
DIM ch AS INTEGER
DIM p AS INTEGER
DIM row AS INTEGER
DIM col AS INTEGER
DIM curow AS INTEGER
DIM cucol AS INTEGER
DIM i AS INTEGER
DIM j AS INTEGER

DIM ls AS INTEGER
DIM ps AS INTEGER
DIM ns AS INTEGER
DIM ne AS INTEGER
DIM le AS INTEGER
DIM curcol AS INTEGER
DIM plen AS INTEGER
DIM newcol AS INTEGER
DIM lsq AS INTEGER
DIM lsr AS INTEGER
DIM leq AS INTEGER
DIM ler AS INTEGER

DIM scrolltop AS INTEGER
DIM wp AS INTEGER
DIM wi AS INTEGER

DIM pfcancel AS INTEGER
DIM saveok AS INTEGER
DIM readn AS INTEGER

LET cols = TEXTCOLS
LET rows = TEXTROWS
LET buflen = 0
LET cur = 0
LET scrolltop = 0
LET fnamelen = 0
LET modified = 0
LET exitreq = 0

GOSUB clear_all

main_loop:
GOSUB redraw
LET k = GETKEY

IF k = 136 THEN
  GOSUB do_menu
  IF exitreq = 1 THEN END
  GOTO main_loop
ENDIF

IF k = 129 THEN
  IF cur > 0 THEN LET cur = cur - 1
  GOTO main_loop
ENDIF
IF k = 130 THEN
  IF cur < buflen THEN LET cur = cur + 1
  GOTO main_loop
ENDIF
IF k = 131 THEN
  GOSUB move_up
  GOTO main_loop
ENDIF
IF k = 132 THEN
  GOSUB move_down
  GOTO main_loop
ENDIF
IF k = 133 THEN
  LET lsq = cur
  GOSUB find_line_start
  LET cur = lsr
  GOTO main_loop
ENDIF
IF k = 134 THEN
  LET leq = cur
  GOSUB find_line_end
  LET cur = ler
  GOTO main_loop
ENDIF
IF k = 135 THEN
  IF cur < buflen THEN GOSUB delete_at_cursor
  GOTO main_loop
ENDIF
IF k = 8 THEN
  IF cur > 0 THEN
    LET cur = cur - 1
    GOSUB delete_at_cursor
  ENDIF
  GOTO main_loop
ENDIF
IF k = 13 THEN
  LET ch = 10
  GOSUB insert_char
  GOTO main_loop
ENDIF
IF k >= 32 AND k <= 126 THEN
  LET ch = k
  GOSUB insert_char
ENDIF
GOTO main_loop

END

' -----------------------------------------------------------------------
' clear_all: blanks every text-console row (there's no CLS for the text
' console -- CLS only clears the pixel back buffer, see LOCATE's own
' notes), so a full redraw never leaves stale characters on screen.
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
' redraw: row 0 = filename/modified status (always fixed), rows
' 1..rows-2 = a SCROLLING window onto buffer content, row rows-1 = key
' hints (always fixed). scrolltop is the buffer index of the first
' visible line's start; before drawing, it's adjusted just enough to
' keep the cursor's own line inside the visible window (scroll up
' instantly if the cursor moved above it, scroll down one line at a
' time if it moved below it) -- the same approach a simple pager uses.
' Content is scanned starting from scrolltop rather than 0, so rows
' 0 and rows-1 are never touched by file content regardless of file
' length. The cursor's own screen cell is overdrawn with an underscore
' as the last drawing step, since LOCATE alone (unlike real hardware
' text mode) has no visible caret of its own -- see shell_cursor_to's
' comment for the same convention used by the shell's own line editor.
' -----------------------------------------------------------------------
redraw:
GOSUB clear_all

LOCATE 0, 0
LET i = 0
WHILE i < fnamelen
  LET ch = fname[i]
  PUTCHAR ch
  LET i = i + 1
WEND
IF fnamelen = 0 THEN
  PRINT "(untitled)"
ELSE
  IF modified = 1 THEN
    PUTCHAR 32
    PUTCHAR 42
  ENDIF
ENDIF

LOCATE rows - 1, 0
PRINT "ESC=Menu  Arrows=Move  Enter=NewLine  Backspace/Del=Delete"

LET lsq = cur
GOSUB find_line_start
LET ls = lsr
IF ls < scrolltop THEN LET scrolltop = ls

scroll_check:
LET wp = scrolltop
LET wi = 0
scroll_count_loop:
IF wi >= rows - 2 THEN GOTO scroll_count_done
LET leq = wp
GOSUB find_line_end
IF ler >= buflen THEN GOTO scroll_count_done
LET wp = ler + 1
LET wi = wi + 1
GOTO scroll_count_loop
scroll_count_done:
IF ls < wp THEN GOTO scroll_done
LET leq = scrolltop
GOSUB find_line_end
LET scrolltop = ler + 1
GOTO scroll_check
scroll_done:

LET row = 1
LET col = 0
LOCATE row, col
LET p = scrolltop
LET curow = 1
LET cucol = 0
redraw_scan:
IF p >= buflen THEN GOTO redraw_after_scan
IF p = cur THEN
  LET curow = row
  LET cucol = col
ENDIF
LET ch = buf[p]
IF ch = 10 THEN
  LET row = row + 1
  LET col = 0
  IF row > rows - 2 THEN GOTO redraw_done
  LOCATE row, col
ELSE
  IF col < cols THEN
    PUTCHAR ch
    LET col = col + 1
  ENDIF
ENDIF
LET p = p + 1
GOTO redraw_scan
redraw_after_scan:
IF p = cur THEN
  LET curow = row
  LET cucol = col
ENDIF
redraw_done:
LOCATE curow, cucol
PUTCHAR 95
RETURN

' -----------------------------------------------------------------------
' find_line_start: input lsq (a position in buf), output lsr = the
' index of the start of the line containing lsq.
' -----------------------------------------------------------------------
find_line_start:
LET lsr = lsq
fls_loop:
IF lsr <= 0 THEN GOTO fls_done
IF buf[lsr - 1] = 10 THEN GOTO fls_done
LET lsr = lsr - 1
GOTO fls_loop
fls_done:
RETURN

' -----------------------------------------------------------------------
' find_line_end: input leq (a position in buf), output ler = the index
' of the newline terminating that line, or buflen if it's the last
' (unterminated) line.
' -----------------------------------------------------------------------
find_line_end:
LET ler = leq
fle_loop:
IF ler >= buflen THEN GOTO fle_done
IF buf[ler] = 10 THEN GOTO fle_done
LET ler = ler + 1
GOTO fle_loop
fle_done:
RETURN

' -----------------------------------------------------------------------
' move_up / move_down: preserve the cursor's column across the line
' change as best they can, clipping to the target line's own length --
' the usual "ragged" arrow-key-vertical-move behavior.
' -----------------------------------------------------------------------
move_up:
LET lsq = cur
GOSUB find_line_start
LET ls = lsr
LET curcol = cur - ls
IF ls = 0 THEN RETURN
LET lsq = ls - 1
GOSUB find_line_start
LET ps = lsr
LET plen = (ls - 1) - ps
LET newcol = curcol
IF newcol > plen THEN LET newcol = plen
LET cur = ps + newcol
RETURN

move_down:
LET lsq = cur
GOSUB find_line_start
LET ls = lsr
LET curcol = cur - ls
LET leq = ls
GOSUB find_line_end
LET le = ler
IF le >= buflen THEN RETURN
LET ns = le + 1
LET leq = ns
GOSUB find_line_end
LET ne = ler
LET plen = ne - ns
LET newcol = curcol
IF newcol > plen THEN LET newcol = plen
LET cur = ns + newcol
RETURN

' -----------------------------------------------------------------------
' insert_char: inserts the value in ch at the cursor, shifting
' everything from cur..buflen-1 up by one element first.
' -----------------------------------------------------------------------
insert_char:
IF buflen >= 1048576 THEN RETURN
LET j = buflen
ins_loop:
IF j <= cur THEN GOTO ins_done
LET buf[j] = buf[j - 1]
LET j = j - 1
GOTO ins_loop
ins_done:
LET buf[cur] = ch
LET buflen = buflen + 1
LET cur = cur + 1
LET modified = 1
RETURN

' -----------------------------------------------------------------------
' delete_at_cursor: removes the element at cur, shifting everything
' after it down by one. Caller must ensure cur < buflen first.
' -----------------------------------------------------------------------
delete_at_cursor:
LET j = cur
del_loop:
IF j >= buflen - 1 THEN GOTO del_done
LET buf[j] = buf[j + 1]
LET j = j + 1
GOTO del_loop
del_done:
LET buflen = buflen - 1
LET modified = 1
RETURN

' -----------------------------------------------------------------------
' prompt_filename: reads a filename (typed on the hint row) into
' fname/fnamelen. Sets pfcancel = 1 if the user pressed ESC instead of
' Enter, leaving fname/fnamelen untouched by the caller's own logic
' (the caller is expected to check pfcancel before using the result).
' -----------------------------------------------------------------------
prompt_filename:
LOCATE rows - 1, 0
LET i = 0
WHILE i < cols
  PUTCHAR 32
  LET i = i + 1
WEND
LOCATE rows - 1, 0
PRINT "Filename: "
LOCATE rows - 1, 10
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
    LOCATE rows - 1, 10 + fnamelen
    PUTCHAR 32
    LOCATE rows - 1, 10 + fnamelen
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
' do_menu: the ESC-triggered Load/Save/Save As/Exit menu. Sets exitreq
' = 1 on Exit rather than using END directly -- END compiles to a bare
' RET, which from inside a GOSUB'd subroutine would only unwind this
' call, not actually end the program; the real END has to run from the
' top-level main_loop once do_menu has returned.
' -----------------------------------------------------------------------
do_menu:
LOCATE rows - 1, 0
LET i = 0
WHILE i < cols
  PUTCHAR 32
  LET i = i + 1
WEND
LOCATE rows - 1, 0
PRINT "L=Load  S=Save  A=Save As  X=Exit  ESC=Cancel"
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
      LET readn = FLOAD(fname, fnamelen, buf, 1048576)
      LET buflen = readn
      LET cur = 0
      LET scrolltop = 0
      LET modified = 0
    ENDIF
  ENDIF
  RETURN
ENDIF

IF k2 = 65 OR k2 = 97 THEN
  GOSUB prompt_filename
  IF pfcancel = 0 THEN
    IF fnamelen > 0 THEN
      LET saveok = FSAVE(fname, fnamelen, buf, buflen)
      LET modified = 0
    ENDIF
  ENDIF
  RETURN
ENDIF

IF k2 = 83 OR k2 = 115 THEN
  IF fnamelen > 0 THEN
    LET saveok = FSAVE(fname, fnamelen, buf, buflen)
    LET modified = 0
  ELSE
    GOSUB prompt_filename
    IF pfcancel = 0 THEN
      IF fnamelen > 0 THEN
        LET saveok = FSAVE(fname, fnamelen, buf, buflen)
        LET modified = 0
      ENDIF
    ENDIF
  ENDIF
  RETURN
ENDIF

RETURN
