' arOS-X64 Terminal -- a real windowed command-line program (LAUNCHed
' like editor.bas/filebrowser.bas, kernel-drawn chrome), NOT the boot
' shell (shell_main/console_*, kernel.asm) running inside a window --
' that shell draws straight to the real screen framebuffer and predates
' the whole windowing system; making IT windowed would be a much bigger,
' riskier change to code that's been stable a long time. This is a
' fresh, smaller command set built on the same BASIX64 primitives every
' other GUI program here already uses (DIROPEN/DIRNEXT, DIRCD/DIRUP,
' LAUNCH) -- RUN/DIR/CD/CLS/HELP/EXIT, not full parity with the boot
' shell's own RUN/DIR/TYPE/DEL/RENAME/APPEND/TRUNCATE/MKDIR/COMPILE set.
'
' No native string type in BASIX64, and no way to pass an array BY
' REFERENCE into a GOSUB (every array is always accessed through its
' own DIM'd name directly, never aliased into another variable) -- so
' every "print a line" call site here writes its text straight into
' the single shared pendingLine staging array before GOSUB PUTLINE,
' rather than each message having its own separately-DIM'd array. Text
' itself is char-code arrays throughout (same convention as
' filebrowser.bas's entChars/DIRNAMECHAR), drawn one glyph at a time
' via DRAWCHAR (DRAWTEXT only takes string LITERALS).
'
' DIM must come before anything that references these arrays in
' *source order* -- DIM's array-arena allocation happens at parse
' time (this is a single-pass compiler), and every dimension size must
' be a literal integer, not a variable -- so everything is DIM'd here,
' at the top, before GOTO START, with literal sizes.
DIM outChars(1480) AS INTEGER
DIM outLen(20) AS INTEGER
DIM inBuf(74) AS INTEGER
DIM cmdBuf(74) AS INTEGER
DIM argBuf(74) AS INTEGER
DIM runProgName(74) AS INTEGER
DIM pendingLine(74) AS INTEGER
DIM emptyArgBuf(1) AS INTEGER

LET outCount = 0
LET inLen = 0
LET rowH = 16
LET marginX = 6
LET marginY = 6

GOTO START

' ---- PUTLINE: appends pendingLine[0..pendingLen) as one new
' scrollback row, scrolling the whole buffer up by one if already at
' 20 (MAXLINES) rows. Every call site fills pendingLine/pendingLen
' itself first -- see this file's own header comment on why there's
' no "pass the text in" parameter instead. ----
PUTLINE:
  IF outCount < 20 THEN
    LET plRow = outCount
    LET outCount = outCount + 1
  ELSE
    LET plI = 0
    WHILE plI < 19
      LET outLen[plI] = outLen[plI + 1]
      LET plJ = 0
      WHILE plJ < 74
        LET outChars[plI * 74 + plJ] = outChars[(plI + 1) * 74 + plJ]
        LET plJ = plJ + 1
      WEND
      LET plI = plI + 1
    WEND
    LET plRow = 19
  ENDIF
  LET plLen = pendingLen
  IF plLen > 74 THEN LET plLen = 74
  LET outLen[plRow] = plLen
  LET plI = 0
  WHILE plI < plLen
    LET outChars[plRow * 74 + plI] = pendingLine[plI]
    LET plI = plI + 1
  WEND
  RETURN

START:

' Initial banner line.
LET pendingLine[0] = 84
LET pendingLine[1] = 121
LET pendingLine[2] = 112
LET pendingLine[3] = 101
LET pendingLine[4] = 32
LET pendingLine[5] = 72
LET pendingLine[6] = 69
LET pendingLine[7] = 76
LET pendingLine[8] = 80
LET pendingLine[9] = 32
LET pendingLine[10] = 102
LET pendingLine[11] = 111
LET pendingLine[12] = 114
LET pendingLine[13] = 32
LET pendingLine[14] = 99
LET pendingLine[15] = 111
LET pendingLine[16] = 109
LET pendingLine[17] = 109
LET pendingLine[18] = 97
LET pendingLine[19] = 110
LET pendingLine[20] = 100
LET pendingLine[21] = 115
LET pendingLen = 22
GOSUB PUTLINE

main_loop:
GOSUB redraw

wait_key:
IF WINCLOSE THEN END
IF KEYHIT = 0 THEN
  WAIT 1
  GOTO wait_key
ENDIF
LET k = GETKEY

IF k = 8 THEN
  IF inLen > 0 THEN LET inLen = inLen - 1
  GOTO main_loop
ENDIF

IF k = 13 THEN
  GOSUB run_command
  LET inLen = 0
  GOTO main_loop
ENDIF

IF k >= 32 THEN
IF k <= 126 THEN
  IF inLen < 72 THEN
    LET inBuf[inLen] = k
    LET inLen = inLen + 1
  ENDIF
ENDIF
ENDIF
GOTO main_loop

END

' ---- run_command: echoes the typed line into the scrollback (prefixed
' "> "), splits it into a command word + trailing argument (first run
' of non-space chars, then the rest with leading spaces trimmed), and
' dispatches. Unrecognized commands get an error line -- typing a bare
' program name does NOT implicitly launch it (unlike a real Unix
' shell); RUN is required, matching the boot shell's own convention. ----
run_command:
  LET pendingLine[0] = 62
  LET pendingLine[1] = 32
  LET rcI = 0
  WHILE rcI < inLen
    IF rcI < 72 THEN LET pendingLine[rcI + 2] = inBuf[rcI]
    LET rcI = rcI + 1
  WEND
  LET pendingLen = inLen + 2
  GOSUB PUTLINE

  IF inLen = 0 THEN RETURN

  LET cmdLen = 0
  LET rcI = 0
  WHILE rcI < inLen
    IF inBuf[rcI] = 32 THEN GOTO cmd_split_done
    IF cmdLen < 74 THEN LET cmdBuf[cmdLen] = inBuf[rcI]
    LET cmdLen = cmdLen + 1
    LET rcI = rcI + 1
  WEND
cmd_split_done:
  WHILE rcI < inLen
    IF inBuf[rcI] <> 32 THEN GOTO arg_start_found
    LET rcI = rcI + 1
  WEND
arg_start_found:
  LET argLen = 0
  WHILE rcI < inLen
    IF argLen < 74 THEN LET argBuf[argLen] = inBuf[rcI]
    LET argLen = argLen + 1
    LET rcI = rcI + 1
  WEND

  ' Uppercase-fold cmdBuf for case-insensitive command matching (RUN,
  ' run, Run all work the same way).
  LET rcI = 0
  WHILE rcI < cmdLen
    IF cmdBuf[rcI] >= 97 THEN
    IF cmdBuf[rcI] <= 122 THEN LET cmdBuf[rcI] = cmdBuf[rcI] - 32
    ENDIF
    LET rcI = rcI + 1
  WEND

  IF cmdLen = 4 THEN
    IF cmdBuf[0] = 72 THEN
    IF cmdBuf[1] = 69 THEN
    IF cmdBuf[2] = 76 THEN
    IF cmdBuf[3] = 80 THEN
      GOSUB cmd_help
      RETURN
    ENDIF
    ENDIF
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 3 THEN
    IF cmdBuf[0] = 68 THEN
    IF cmdBuf[1] = 73 THEN
    IF cmdBuf[2] = 82 THEN
      GOSUB cmd_dir
      RETURN
    ENDIF
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 2 THEN
    IF cmdBuf[0] = 67 THEN
    IF cmdBuf[1] = 68 THEN
      GOSUB cmd_cd
      RETURN
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 3 THEN
    IF cmdBuf[0] = 67 THEN
    IF cmdBuf[1] = 76 THEN
    IF cmdBuf[2] = 83 THEN
      LET outCount = 0
      RETURN
    ENDIF
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 5 THEN
    IF cmdBuf[0] = 67 THEN
    IF cmdBuf[1] = 76 THEN
    IF cmdBuf[2] = 69 THEN
    IF cmdBuf[3] = 65 THEN
    IF cmdBuf[4] = 82 THEN
      LET outCount = 0
      RETURN
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 3 THEN
    IF cmdBuf[0] = 82 THEN
    IF cmdBuf[1] = 85 THEN
    IF cmdBuf[2] = 78 THEN
      GOSUB cmd_run
      RETURN
    ENDIF
    ENDIF
    ENDIF
  ENDIF
  IF cmdLen = 4 THEN
    IF cmdBuf[0] = 69 THEN
    IF cmdBuf[1] = 88 THEN
    IF cmdBuf[2] = 73 THEN
    IF cmdBuf[3] = 84 THEN END
    ENDIF
    ENDIF
    ENDIF
  ENDIF

  LET pendingLine[0] = 85
  LET pendingLine[1] = 110
  LET pendingLine[2] = 107
  LET pendingLine[3] = 110
  LET pendingLine[4] = 111
  LET pendingLine[5] = 119
  LET pendingLine[6] = 110
  LET pendingLine[7] = 32
  LET pendingLine[8] = 99
  LET pendingLine[9] = 111
  LET pendingLine[10] = 109
  LET pendingLine[11] = 109
  LET pendingLine[12] = 97
  LET pendingLine[13] = 110
  LET pendingLine[14] = 100
  LET pendingLine[15] = 46
  LET pendingLine[16] = 32
  LET pendingLine[17] = 84
  LET pendingLine[18] = 114
  LET pendingLine[19] = 121
  LET pendingLine[20] = 32
  LET pendingLine[21] = 72
  LET pendingLine[22] = 69
  LET pendingLine[23] = 76
  LET pendingLine[24] = 80
  LET pendingLen = 25
  GOSUB PUTLINE
  RETURN

cmd_help:
  LET pendingLine[0] = 82
  LET pendingLine[1] = 85
  LET pendingLine[2] = 78
  LET pendingLine[3] = 32
  LET pendingLine[4] = 60
  LET pendingLine[5] = 110
  LET pendingLine[6] = 97
  LET pendingLine[7] = 109
  LET pendingLine[8] = 101
  LET pendingLine[9] = 62
  LET pendingLine[10] = 32
  LET pendingLine[11] = 45
  LET pendingLine[12] = 32
  LET pendingLine[13] = 108
  LET pendingLine[14] = 97
  LET pendingLine[15] = 117
  LET pendingLine[16] = 110
  LET pendingLine[17] = 99
  LET pendingLine[18] = 104
  LET pendingLine[19] = 32
  LET pendingLine[20] = 97
  LET pendingLine[21] = 32
  LET pendingLine[22] = 112
  LET pendingLine[23] = 114
  LET pendingLine[24] = 111
  LET pendingLine[25] = 103
  LET pendingLine[26] = 114
  LET pendingLine[27] = 97
  LET pendingLine[28] = 109
  LET pendingLen = 29
  GOSUB PUTLINE

  LET pendingLine[0] = 68
  LET pendingLine[1] = 73
  LET pendingLine[2] = 82
  LET pendingLine[3] = 32
  LET pendingLine[4] = 45
  LET pendingLine[5] = 32
  LET pendingLine[6] = 108
  LET pendingLine[7] = 105
  LET pendingLine[8] = 115
  LET pendingLine[9] = 116
  LET pendingLine[10] = 32
  LET pendingLine[11] = 116
  LET pendingLine[12] = 104
  LET pendingLine[13] = 101
  LET pendingLine[14] = 32
  LET pendingLine[15] = 99
  LET pendingLine[16] = 117
  LET pendingLine[17] = 114
  LET pendingLine[18] = 114
  LET pendingLine[19] = 101
  LET pendingLine[20] = 110
  LET pendingLine[21] = 116
  LET pendingLine[22] = 32
  LET pendingLine[23] = 100
  LET pendingLine[24] = 105
  LET pendingLine[25] = 114
  LET pendingLine[26] = 101
  LET pendingLine[27] = 99
  LET pendingLine[28] = 116
  LET pendingLine[29] = 111
  LET pendingLine[30] = 114
  LET pendingLine[31] = 121
  LET pendingLen = 32
  GOSUB PUTLINE

  LET pendingLine[0] = 67
  LET pendingLine[1] = 68
  LET pendingLine[2] = 32
  LET pendingLine[3] = 60
  LET pendingLine[4] = 110
  LET pendingLine[5] = 97
  LET pendingLine[6] = 109
  LET pendingLine[7] = 101
  LET pendingLine[8] = 62
  LET pendingLine[9] = 32
  LET pendingLine[10] = 47
  LET pendingLine[11] = 32
  LET pendingLine[12] = 67
  LET pendingLine[13] = 68
  LET pendingLine[14] = 32
  LET pendingLine[15] = 46
  LET pendingLine[16] = 46
  LET pendingLine[17] = 32
  LET pendingLine[18] = 45
  LET pendingLine[19] = 32
  LET pendingLine[20] = 99
  LET pendingLine[21] = 104
  LET pendingLine[22] = 97
  LET pendingLine[23] = 110
  LET pendingLine[24] = 103
  LET pendingLine[25] = 101
  LET pendingLine[26] = 32
  LET pendingLine[27] = 100
  LET pendingLine[28] = 105
  LET pendingLine[29] = 114
  LET pendingLine[30] = 101
  LET pendingLine[31] = 99
  LET pendingLine[32] = 116
  LET pendingLine[33] = 111
  LET pendingLine[34] = 114
  LET pendingLine[35] = 121
  LET pendingLen = 36
  GOSUB PUTLINE

  LET pendingLine[0] = 67
  LET pendingLine[1] = 76
  LET pendingLine[2] = 83
  LET pendingLine[3] = 32
  LET pendingLine[4] = 45
  LET pendingLine[5] = 32
  LET pendingLine[6] = 99
  LET pendingLine[7] = 108
  LET pendingLine[8] = 101
  LET pendingLine[9] = 97
  LET pendingLine[10] = 114
  LET pendingLine[11] = 32
  LET pendingLine[12] = 115
  LET pendingLine[13] = 99
  LET pendingLine[14] = 114
  LET pendingLine[15] = 111
  LET pendingLine[16] = 108
  LET pendingLine[17] = 108
  LET pendingLine[18] = 98
  LET pendingLine[19] = 97
  LET pendingLine[20] = 99
  LET pendingLine[21] = 107
  LET pendingLen = 22
  GOSUB PUTLINE

  LET pendingLine[0] = 69
  LET pendingLine[1] = 88
  LET pendingLine[2] = 73
  LET pendingLine[3] = 84
  LET pendingLine[4] = 32
  LET pendingLine[5] = 45
  LET pendingLine[6] = 32
  LET pendingLine[7] = 99
  LET pendingLine[8] = 108
  LET pendingLine[9] = 111
  LET pendingLine[10] = 115
  LET pendingLine[11] = 101
  LET pendingLine[12] = 32
  LET pendingLine[13] = 116
  LET pendingLine[14] = 104
  LET pendingLine[15] = 105
  LET pendingLine[16] = 115
  LET pendingLine[17] = 32
  LET pendingLine[18] = 119
  LET pendingLine[19] = 105
  LET pendingLine[20] = 110
  LET pendingLine[21] = 100
  LET pendingLine[22] = 111
  LET pendingLine[23] = 119
  LET pendingLen = 24
  GOSUB PUTLINE
  RETURN

cmd_dir:
  DIROPEN
  WHILE DIRNEXT
    LET dnLen = DIRNAMELEN
    IF dnLen > 68 THEN LET dnLen = 68
    LET dlI = 0
    IF DIRISDIR THEN
      LET pendingLine[0] = 91
      LET pendingLine[1] = 68
      LET pendingLine[2] = 93
      LET pendingLine[3] = 32
      LET dlI = 4
    ENDIF
    LET dlJ = 0
    WHILE dlJ < dnLen
      LET pendingLine[dlI + dlJ] = DIRNAMECHAR(dlJ)
      LET dlJ = dlJ + 1
    WEND
    LET pendingLen = dlI + dnLen
    GOSUB PUTLINE
  WEND
  RETURN

cmd_cd:
  IF argLen = 0 THEN RETURN
  IF argLen = 2 THEN
    IF argBuf[0] = 46 THEN
    IF argBuf[1] = 46 THEN
      DIRUP
      RETURN
    ENDIF
    ENDIF
  ENDIF
  LET rcI = 0
  WHILE rcI < argLen
    LET runProgName[rcI] = argBuf[rcI]
    LET rcI = rcI + 1
  WEND
  DIRCD runProgName, argLen
  RETURN

cmd_run:
  IF argLen = 0 THEN
    LET pendingLine[0] = 82
    LET pendingLine[1] = 85
    LET pendingLine[2] = 78
    LET pendingLine[3] = 32
    LET pendingLine[4] = 110
    LET pendingLine[5] = 101
    LET pendingLine[6] = 101
    LET pendingLine[7] = 100
    LET pendingLine[8] = 115
    LET pendingLine[9] = 32
    LET pendingLine[10] = 97
    LET pendingLine[11] = 32
    LET pendingLine[12] = 102
    LET pendingLine[13] = 105
    LET pendingLine[14] = 108
    LET pendingLine[15] = 101
    LET pendingLine[16] = 110
    LET pendingLine[17] = 97
    LET pendingLine[18] = 109
    LET pendingLine[19] = 101
    LET pendingLen = 20
    GOSUB PUTLINE
    RETURN
  ENDIF
  LET rcI = 0
  WHILE rcI < argLen
    LET runProgName[rcI] = argBuf[rcI]
    LET rcI = rcI + 1
  WEND
  LAUNCH runProgName, argLen, emptyArgBuf, 0
  RETURN

redraw:
  CLS WBGRAY
  LET rdY = marginY
  LET rdI = 0
  WHILE rdI < outCount
    LET rdJ = 0
    WHILE rdJ < outLen[rdI]
      DRAWCHAR marginX + rdJ * 8, rdY, outChars[rdI * 74 + rdJ], 0
      LET rdJ = rdJ + 1
    WEND
    LET rdY = rdY + rowH
    LET rdI = rdI + 1
  WEND

  DRAWCHAR marginX, rdY, 62, 16711680
  DRAWCHAR marginX + 8, rdY, 32, 0
  LET rdJ = 0
  WHILE rdJ < inLen
    DRAWCHAR marginX + 16 + rdJ * 8, rdY, inBuf[rdJ], 0
    LET rdJ = rdJ + 1
  WEND
  RECT marginX + 16 + inLen * 8, rdY, 8, 16, 11184810

  FLIP
  RETURN
