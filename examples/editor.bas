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

' Syntax-highlighting state (see classify_char and friends, near the
' bottom of this file). in_comment persists across classify_char calls
' within one screen line (reset at every newline); hl_len/hl_color are
' classify_char's per-call outputs. The cc_* vars are private scratch
' for classify_char's own helper subroutines.
DIM in_comment AS INTEGER
DIM hl_len AS INTEGER
DIM hl_color AS INTEGER
DIM tki AS INTEGER
DIM cc_is_alpha AS INTEGER
DIM cc_prev_word AS INTEGER
DIM cc_pch AS INTEGER
DIM cc_wc_result AS INTEGER
DIM cc_bp AS INTEGER
DIM cc_bnd_ok AS INTEGER

DIM COL_WHITE AS INTEGER
DIM COL_GREEN AS INTEGER
DIM COL_BLUE AS INTEGER
DIM COL_YELLOW AS INTEGER
DIM COL_ORANGE AS INTEGER

LET cols = TEXTCOLS
LET rows = TEXTROWS
LET buflen = 0
LET cur = 0
LET scrolltop = 0
LET fnamelen = 0
LET modified = 0
LET exitreq = 0
LET in_comment = 0

LET COL_WHITE = 16777215
LET COL_GREEN = 52224
LET COL_BLUE = 5676246
LET COL_YELLOW = 16776960
LET COL_ORANGE = 14251863

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
' redraw: everything that isn't file content is pinned to the TOP of the
' screen now -- row 0 = the Load/Save/Save As/Exit menu bar (always
' fixed -- see do_menu, which still gates the actual keys behind ESC so
' L/S/A/X keep working as normal typing outside menu mode), row 1 =
' filename/modified status (always fixed), row 2 = key hints (always
' fixed), row 3 = a full-width '=' divider bar (TUI-style separator
' between the fixed header and the document), row 4 = a blank spacer
' row, rows 5..rows-1 = a SCROLLING window onto buffer content running
' all the way to the bottom of the screen. scrolltop is the buffer
' index of the first visible line's start; before drawing, it's
' adjusted just enough to keep the cursor's own line inside the visible
' window (scroll up instantly if the cursor moved above it, scroll down
' one line at a time if it moved below it) -- the same approach a
' simple pager uses. Content is scanned starting from scrolltop rather
' than 0, so rows 0-4 are never touched by file content regardless of
' file length. The cursor's own screen cell is overdrawn with an
' underscore as the last drawing step, since LOCATE alone (unlike real
' hardware text mode) has no visible caret of its own -- see
' shell_cursor_to's comment for the same convention used by the shell's
' own line editor.
' -----------------------------------------------------------------------
redraw:
GOSUB clear_all
' a previous redraw's content scan may have left the console color set
' to a syntax-highlight color -- the fixed header rows always draw white
COLOR COL_WHITE

LOCATE 0, 0
PRINT "ESC=Menu  L=Load  S=Save  A=Save As  X=Exit"

LOCATE 1, 0
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

LOCATE 2, 0
PRINT "Arrows=Move  Enter=NewLine  Backspace/Del=Delete"

LOCATE 3, 0
LET i = 0
WHILE i < cols
  PUTCHAR 61
  LET i = i + 1
WEND

LET lsq = cur
GOSUB find_line_start
LET ls = lsr
IF ls < scrolltop THEN LET scrolltop = ls

scroll_check:
LET wp = scrolltop
LET wi = 0
scroll_count_loop:
IF wi >= rows - 5 THEN GOTO scroll_count_done
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

LET row = 5
LET col = 0
LOCATE row, col
LET p = scrolltop
LET curow = 5
LET cucol = 0
' scrolltop always lands on a line start (see the comment above), so a
' fresh scan never begins mid-comment
LET in_comment = 0
redraw_scan:
IF p >= buflen THEN GOTO redraw_after_scan
LET ch = buf[p]
IF ch = 10 THEN
  IF p = cur THEN
    LET curow = row
    LET cucol = col
  ENDIF
  LET row = row + 1
  LET col = 0
  LET in_comment = 0
  IF row > rows - 1 THEN GOTO redraw_done
  LOCATE row, col
  LET p = p + 1
  GOTO redraw_scan
ENDIF

' sets hl_len (>=1) / hl_color for buf[p], may set in_comment
GOSUB classify_char
COLOR hl_color
LET tki = 0
classify_print_loop:
IF tki >= hl_len THEN GOTO classify_print_done
IF p >= buflen THEN GOTO classify_print_done
IF p = cur THEN
  LET curow = row
  LET cucol = col
ENDIF
LET ch = buf[p]
IF col < cols THEN
  PUTCHAR ch
  LET col = col + 1
ENDIF
LET p = p + 1
LET tki = tki + 1
GOTO classify_print_loop
classify_print_done:
GOTO redraw_scan

redraw_after_scan:
IF p = cur THEN
  LET curow = row
  LET cucol = col
ENDIF
redraw_done:
' the cursor underscore itself is never syntax-colored
COLOR COL_WHITE
LOCATE curow, cucol
PUTCHAR 95
RETURN

' -----------------------------------------------------------------------
' classify_char: input p (buffer position, buf[p] must be valid, i.e.
' p < buflen). Sets hl_len (>=1, how many characters starting at p share
' one color) and hl_color (that color). Comments (from ' to end of line,
' tracked via in_comment across calls within one screen line) are green;
' DIM/LET are mid-blue; (), [] are yellow; the handful of BASIX64 data-
' type keywords are Claude-orange; everything else is COL_WHITE. Keyword
' matching only fires at a word boundary (start of buffer, or the
' preceding char isn't itself an identifier char) and requires the
' character immediately following the candidate word to not be an
' identifier char either -- so e.g. "DIMENSION" or "MYDIM" never get
' mistaken for the keyword DIM.
' -----------------------------------------------------------------------
classify_char:
LET hl_len = 1
LET hl_color = COL_WHITE

IF in_comment = 1 THEN
  LET hl_color = COL_GREEN
  RETURN
ENDIF

LET ch = buf[p]

IF ch = 39 THEN
  LET in_comment = 1
  LET hl_color = COL_GREEN
  RETURN
ENDIF

IF ch = 40 OR ch = 41 OR ch = 91 OR ch = 93 THEN
  LET hl_color = COL_YELLOW
  RETURN
ENDIF

LET cc_is_alpha = 0
IF ch >= 65 AND ch <= 90 THEN LET cc_is_alpha = 1
IF ch >= 97 AND ch <= 122 THEN LET cc_is_alpha = 1
IF cc_is_alpha = 0 THEN RETURN

LET cc_prev_word = 0
IF p > 0 THEN
  LET cc_pch = buf[p - 1]
  GOSUB is_wordchar
  LET cc_prev_word = cc_wc_result
ENDIF
IF cc_prev_word = 1 THEN RETURN

GOSUB try_match_keywords
RETURN

' -----------------------------------------------------------------------
' is_wordchar: input cc_pch (a char code), output cc_wc_result (1 if
' it's an identifier char -- alnum or underscore -- else 0).
' -----------------------------------------------------------------------
is_wordchar:
LET cc_wc_result = 0
IF cc_pch >= 48 AND cc_pch <= 57 THEN LET cc_wc_result = 1
IF cc_pch >= 65 AND cc_pch <= 90 THEN LET cc_wc_result = 1
IF cc_pch >= 97 AND cc_pch <= 122 THEN LET cc_wc_result = 1
IF cc_pch = 95 THEN LET cc_wc_result = 1
RETURN

' -----------------------------------------------------------------------
' check_boundary_after: input cc_bp (the buffer position right after a
' candidate keyword match), output cc_bnd_ok (1 if that position is NOT
' another identifier char, or is past the end of the buffer -- i.e. the
' candidate word actually ends there rather than continuing).
' -----------------------------------------------------------------------
check_boundary_after:
LET cc_bnd_ok = 1
IF cc_bp < buflen THEN
  LET cc_pch = buf[cc_bp]
  GOSUB is_wordchar
  IF cc_wc_result = 1 THEN LET cc_bnd_ok = 0
ENDIF
RETURN

' -----------------------------------------------------------------------
' try_match_keywords: called only when buf[p] starts a word boundary.
' Tries each reserved keyword this editor colors; on the first exact
' match (respecting the trailing word boundary too), sets hl_len/
' hl_color and returns. Leaves hl_len/hl_color at classify_char's
' default (1 / COL_WHITE) if nothing matches.
' -----------------------------------------------------------------------
try_match_keywords:
IF buflen >= p + 3 THEN
  IF buf[p] = 68 AND buf[p+1] = 73 AND buf[p+2] = 77 THEN
    LET cc_bp = p + 3
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 3
      LET hl_color = COL_BLUE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 3 THEN
  IF buf[p] = 76 AND buf[p+1] = 69 AND buf[p+2] = 84 THEN
    LET cc_bp = p + 3
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 3
      LET hl_color = COL_BLUE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 7 THEN
  IF buf[p] = 73 AND buf[p+1] = 78 AND buf[p+2] = 84 AND buf[p+3] = 69 AND buf[p+4] = 71 AND buf[p+5] = 69 AND buf[p+6] = 82 THEN
    LET cc_bp = p + 7
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 7
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 6 THEN
  IF buf[p] = 83 AND buf[p+1] = 70 AND buf[p+2] = 76 AND buf[p+3] = 79 AND buf[p+4] = 65 AND buf[p+5] = 84 THEN
    LET cc_bp = p + 6
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 6
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 6 THEN
  IF buf[p] = 68 AND buf[p+1] = 70 AND buf[p+2] = 76 AND buf[p+3] = 79 AND buf[p+4] = 65 AND buf[p+5] = 84 THEN
    LET cc_bp = p + 6
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 6
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 7 THEN
  IF buf[p] = 86 AND buf[p+1] = 69 AND buf[p+2] = 67 AND buf[p+3] = 84 AND buf[p+4] = 79 AND buf[p+5] = 82 AND buf[p+6] = 50 THEN
    LET cc_bp = p + 7
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 7
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 7 THEN
  IF buf[p] = 86 AND buf[p+1] = 69 AND buf[p+2] = 67 AND buf[p+3] = 84 AND buf[p+4] = 79 AND buf[p+5] = 82 AND buf[p+6] = 51 THEN
    LET cc_bp = p + 7
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 7
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 7 THEN
  IF buf[p] = 86 AND buf[p+1] = 69 AND buf[p+2] = 67 AND buf[p+3] = 84 AND buf[p+4] = 79 AND buf[p+5] = 82 AND buf[p+6] = 52 THEN
    LET cc_bp = p + 7
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 7
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

IF buflen >= p + 7 THEN
  IF buf[p] = 77 AND buf[p+1] = 65 AND buf[p+2] = 84 AND buf[p+3] = 82 AND buf[p+4] = 73 AND buf[p+5] = 88 AND buf[p+6] = 52 THEN
    LET cc_bp = p + 7
    GOSUB check_boundary_after
    IF cc_bnd_ok = 1 THEN
      LET hl_len = 7
      LET hl_color = COL_ORANGE
      RETURN
    ENDIF
  ENDIF
ENDIF

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
' do_menu: the ESC-triggered Load/Save/Save As/Exit menu. Sets exitreq
' = 1 on Exit rather than using END directly -- END compiles to a bare
' RET, which from inside a GOSUB'd subroutine would only unwind this
' call, not actually end the program; the real END has to run from the
' top-level main_loop once do_menu has returned.
' -----------------------------------------------------------------------
do_menu:
LOCATE 0, 0
LET i = 0
WHILE i < cols
  PUTCHAR 32
  LET i = i + 1
WEND
LOCATE 0, 0
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
