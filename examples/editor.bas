' Simple text editor for arOS-X64/BASIX64.
'
' R3: rewritten to draw through the pixel back buffer (CLS/DRAWTEXT/
' DRAWCHAR/RECT/FLIP) instead of the old text console (LOCATE/PUTCHAR/
' PRINT/COLOR). The text console writes straight to the real,
' currently-scanned-out framebuffer with no dirty-rect tracking at
' all -- fine when this ran alone (blocking RUN), but since LAUNCH
' (see workbench.bas) now runs this concurrently alongside Workbench's
' own continuously-redrawing compositor-driven window, anything drawn
' via the text console got overwritten by Workbench's very next frame
' almost immediately (a visible flicker, then gone). Going through the
' same back buffer + R1/R2 compositor path Workbench itself uses is
' what keeps this editor's own content actually visible while it has
' focus. See kernel/basix_rbx_clobber_bug.md's [[project_r2_zorder]]
' note for the diagnosis this fix came from.
'
' curX/curY (pixel coordinates, 8x16 per cell -- see basix_draw_glyph)
' replace the old LOCATE-set console cursor; curcolor replaces COLOR
' (DRAWCHAR/DRAWTEXT take an explicit color argument every call rather
' than an ambient console color, so curcolor is just tracked here the
' same way hl_color already was). Both are purely DERIVED from
' row/col wherever those already change -- row/col were already being
' hand-tracked for cursor bookkeeping before this rewrite, so keeping
' them as the single source of truth (rather than maintaining curX/
' curY as their own independent counters) avoids the two ever
' drifting out of sync.
'
' DRAWCHAR only paints a glyph's SET pixels (unlike the text console's
' fb_draw_char, which paints set AND clear pixels, replacing the whole
' cell -- see its own doc comment) -- so "erase by drawing a space"
' (the text-console idiom the old version used for the filename
' prompt's backspace) no longer erases anything; a space glyph has no
' set pixels to paint. Replaced with an explicit black RECT over that
' one cell instead (see prompt_filename's backspace handling).
'
' Load/Save/Save As/Exit are reached through an ESC-triggered modal
' menu (there's no mouse-driven menu here, and no other key the
' keyboard driver decodes safely without shadowing normal typing --
' see keyboard.inc). Normal typing inserts at the cursor; arrow keys,
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

' Pixel-cursor state (see header comment) -- curX/curY are re-derived
' from row/col every time either changes, never advanced on their own.
DIM curX AS INTEGER
DIM curY AS INTEGER
DIM curcolor AS INTEGER

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

' If launched with an argument (e.g. Workbench double-clicking a .BAS
' icon -- see LAUNCH/ARGLEN/ARGCHAR), treat it as a filename to open
' immediately, same FLOAD call the L=Load menu action uses. ARGLEN is 0
' when started via plain RUN, so this is a no-op then.
IF ARGLEN > 0 THEN
  LET fnamelen = ARGLEN
  IF fnamelen > 64 THEN LET fnamelen = 64
  LET i = 0
  WHILE i < fnamelen
    LET fname[i] = ARGCHAR(i)
    LET i = i + 1
  WEND
  LET readn = FLOAD(fname, fnamelen, buf, 1048576)
  LET buflen = readn
  LET cur = 0
  LET scrolltop = 0
  LET modified = 0
ENDIF

main_loop:
GOSUB redraw

' Poll WINCLOSE (the kernel-drawn close gadget, see kernel/kernel.asm's
' wm_tick) alongside KEYHIT instead of blocking straight on GETKEY --
' otherwise a click on the close gadget would only be noticed the next
' time the user pressed some other key anyway, since GETKEY's own wait
' has no way to also watch for a window-manager event. Once KEYHIT
' confirms a key really is waiting, GETKEY itself won't actually block
' (there's something there to read), so this doesn't change the
' keyboard side of the loop at all -- WAIT 1 here just keeps the idle
' poll from burning a full CPU share while nothing's happening, same
' reasoning as workbench.bas's own WAIT-paced main loop.
wait_key:
IF WINCLOSE THEN END
IF KEYHIT = 0 THEN
  WAIT 1
  GOTO wait_key
ENDIF
LET k = GETKEY

' NOT doing a CLS+FLIP cleanup pass here before END, on purpose: R1's
' compositor is a separate, asynchronously-scheduled task, so a CLS
' here only QUEUES a full-screen dirty mark rather than presenting it
' immediately -- tried it, and it actively raced the shell's own
' prompt text (drawn straight to the real framebuffer via the text
' console the instant this program's RUN returns): the compositor's
' still-pending blit of this now-stale all-black back buffer could
' land AFTER that prompt was drawn, silently erasing it. Leftover
' pixels from this program's last frame are a real cosmetic gap when
' run standalone via RUN (nothing else is guaranteed to redraw over
' that region afterward), but it's the same class of "what happens to
' a window's exposed leftover pixels when its owner exits" problem
' R4/R5's window chrome/positioning work will need to solve properly
' anyway -- not something safe to hack around per-program here. The
' intended real use (LAUNCHed from workbench.bas) doesn't hit this at
' all: workbench's own continuously-redrawing loop covers this
' program's last frame on its very next pass regardless.
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
' underscore as the last drawing step, since there's no visible caret
' of its own -- see shell_cursor_to's comment for the same convention
' used by the text shell's own line editor. Ends with FLIP to present
' the whole frame at once (CLS below marks it fully dirty either way,
' so this is really about not leaving a half-drawn frame the
' compositor could catch mid-update, same reasoning as workbench.bas's
' own redraw).
' -----------------------------------------------------------------------
redraw:
CLS 0
' a previous redraw's content scan may have left curcolor set to a
' syntax-highlight color -- the fixed header rows always draw white
LET curcolor = COL_WHITE

LET row = 0
LET col = 0
LET curX = 0
LET curY = 0
DRAWTEXT curX, curY, "ESC=Menu  L=Load  S=Save  A=Save As  X=Exit", curcolor

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
  DRAWTEXT curX, curY, "(untitled)", curcolor
ELSE
  IF modified = 1 THEN
    LET ch = 32
    GOSUB putch_skip_space
    LET ch = 42
    GOSUB putch_skip_space
  ENDIF
ENDIF

LET row = 2
LET col = 0
LET curX = 0
LET curY = 32
DRAWTEXT curX, curY, "Arrows=Move  Enter=NewLine  Backspace/Del=Delete", curcolor

RECT 0, 48, cols * 8, 2, COL_WHITE

LET lsq = cur
GOSUB find_line_start
LET ls = lsr
IF ls < scrolltop THEN LET scrolltop = ls

' scroll_count_loop can exit two different ways, and they mean
' different things: hitting rows-5 lines counted (wi >= rows - 5)
' means the display filled up before reaching the cursor's line, so
' scroll_count_done below needs to check whether ls was actually
' covered. Running out of buffer content first (ler >= buflen, the
' probed line has no trailing newline -- it's the last line in the
' file) means there's nothing left to scroll to at all, so the
' cursor's line (always <= buflen) is trivially already visible --
' jump straight to scroll_done rather than falling into the "not
' visible yet, scroll down more" check, which would otherwise keep
' advancing scrolltop PAST the buffer's only content forever (this bit
' real: a brand new/short document's redraw_scan starts at a scrolltop
' beyond buflen and draws nothing at all).
scroll_check:
LET wp = scrolltop
LET wi = 0
scroll_count_loop:
IF wi >= rows - 5 THEN GOTO scroll_count_done
LET leq = wp
GOSUB find_line_end
IF ler >= buflen THEN GOTO scroll_done
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
LET curX = 0
LET curY = 80
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
  LET curX = 0
  LET curY = row * 16
  LET in_comment = 0
  IF row > rows - 1 THEN GOTO redraw_done
  LET p = p + 1
  GOTO redraw_scan
ENDIF

' sets hl_len (>=1) / hl_color for buf[p], may set in_comment
GOSUB classify_char
LET curcolor = hl_color
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
  GOSUB putch_skip_space
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
DRAWCHAR cucol * 8, curow * 16, 95, COL_WHITE
FLIP
RETURN

' -----------------------------------------------------------------------
' putch: draws buf/menu character ch at the current pixel cursor
' (curX, curY) in curcolor, then advances col (and curX, derived from
' it) by one cell -- the pixel-drawing replacement for the old text
' console's PUTCHAR, which advanced its own implicit cursor the same
' way.
' -----------------------------------------------------------------------
putch:
DRAWCHAR curX, curY, ch, curcolor
LET col = col + 1
LET curX = col * 8
RETURN

' -----------------------------------------------------------------------
' putch_skip_space: same as putch, but skips the actual DRAWCHAR call
' for a space (code 32) -- DRAWCHAR only paints a glyph's SET pixels
' (see this file's header comment), and a space glyph has none, so
' calling it would be a pure no-op draw anyway. Used in redraw, where
' the destination is already black from this frame's own CLS, so
' skipping is purely an optimization, not a correctness fix (unlike
' prompt_filename's backspace handling below, which genuinely needs an
' explicit RECT erase since it does NOT redraw from a fresh CLS).
' -----------------------------------------------------------------------
putch_skip_space:
IF ch = 32 THEN
  LET col = col + 1
  LET curX = col * 8
  RETURN
ENDIF
GOSUB putch
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
' Draws incrementally (one FLIP per keystroke) rather than through the
' main redraw, so the compositor sees each character/backspace as it
' happens instead of only once the whole prompt is done.
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
' do_menu: the ESC-triggered Load/Save/Save As/Exit menu. Sets exitreq
' = 1 on Exit -- the real END has to run from the top-level main_loop
' once do_menu has returned, not here (a GOSUB'd END would only unwind
' this call, not actually end the program).
' -----------------------------------------------------------------------
do_menu:
RECT 0, 0, cols * 8, 16, 0
LET curcolor = COL_WHITE
LET curX = 0
LET curY = 0
DRAWTEXT curX, curY, "L=Load  S=Save  A=Save As  X=Exit  ESC=Cancel", curcolor
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
