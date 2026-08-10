' cube_rotate_90.bas -- wireframe cube spinning 90 degrees about the Y
' axis over DURATION_SEC seconds at TARGET_FPS frames/second. Frame
' count, per-frame angle step, and frame pacing are all derived from
' those two parameters (plus the PIT's known 100Hz tick rate) rather
' than hardcoded -- change either one and the animation retimes itself.

DIM DURATION_SEC AS INTEGER
DIM TARGET_FPS AS INTEGER
DIM TICKS_PER_SEC AS INTEGER
DIM FRAME_COUNT AS INTEGER
DIM TOTAL_TICKS AS INTEGER
DIM TOTAL_ANGLE AS SFLOAT

LET DURATION_SEC = 10
LET TARGET_FPS = 50
LET TICKS_PER_SEC = 100          ' PIT rate this kernel programs (pic.inc, PIT_HZ) --
                                  ' TIMER/WAIT both count in these ticks
LET FRAME_COUNT = DURATION_SEC * TARGET_FPS
LET TOTAL_TICKS = DURATION_SEC * TICKS_PER_SEC
LET TOTAL_ANGLE = ACOS(0.0)      ' 90 degrees, in radians -- exact, no pi literal

' -- Camera setup (static 3/4 view, same two-axis rotate+perspective
' -- convention as examples/cube_sphere_cone.bas's PROJECT routine).
DIM cy AS SFLOAT
DIM sy AS SFLOAT
DIM cx AS SFLOAT
DIM sx AS SFLOAT
DIM d AS SFLOAT
DIM f AS SFLOAT
DIM ex AS SFLOAT
DIM ey AS SFLOAT
LET cy = 0.8
LET sy = 0.6
LET cx = 0.9
LET sx = 0.3
LET d = 6
LET f = 700
LET ex = 640
LET ey = 400

' -- Cube geometry: 8 corners of a unit cube (base/object-space, never
' -- rotated in place -- each frame re-spins from these), 12 edges as
' -- vertex-index pairs. Corner i's signs come from its own bit pattern
' -- (bit0=x, bit1=y, bit2=z) rather than 24 hand-picked +/-1 literals.
DIM bx(8) AS SFLOAT
DIM by(8) AS SFLOAT
DIM bz(8) AS SFLOAT
DIM HS AS SFLOAT
LET HS = 1.0

DIM i AS INTEGER
DIM xb AS INTEGER
DIM yb AS INTEGER
DIM zb AS INTEGER
FOR i = 0 TO 7
    LET xb = i MOD 2
    LET yb = (i / 2) MOD 2
    LET zb = (i / 4) MOD 2
    LET bx[i] = (xb*2 - 1) * HS
    LET by[i] = (yb*2 - 1) * HS
    LET bz[i] = (zb*2 - 1) * HS
NEXT i

DIM ea(12) AS INTEGER
DIM eb(12) AS INTEGER
' bottom face
LET ea[0] = 0
LET eb[0] = 1
LET ea[1] = 1
LET eb[1] = 3
LET ea[2] = 3
LET eb[2] = 2
LET ea[3] = 2
LET eb[3] = 0
' top face
LET ea[4] = 4
LET eb[4] = 5
LET ea[5] = 5
LET eb[5] = 7
LET ea[6] = 7
LET eb[6] = 6
LET ea[7] = 6
LET eb[7] = 4
' verticals
LET ea[8] = 0
LET eb[8] = 4
LET ea[9] = 1
LET eb[9] = 5
LET ea[10] = 2
LET eb[10] = 6
LET ea[11] = 3
LET eb[11] = 7

' -- Per-frame projected screen position/depth, and the previous
' -- frame's screen position (drawn in black first each frame to erase
' -- it -- this kernel's LINE has no back buffer to flip).
DIM px_arr(8) AS SFLOAT
DIM py_arr(8) AS SFLOAT
DIM pz_arr(8) AS SFLOAT
DIM prevx(8) AS SFLOAT
DIM prevy(8) AS SFLOAT
DIM have_prev AS INTEGER
LET have_prev = 0

DIM lx AS SFLOAT
DIM ly AS SFLOAT
DIM lz AS SFLOAT
DIM mx AS SFLOAT
DIM mz2 AS SFLOAT
DIM rx AS SFLOAT
DIM ry AS SFLOAT
DIM rz AS SFLOAT
DIM r1 AS SFLOAT
DIM dn AS SFLOAT
DIM sc AS SFLOAT
DIM ca AS SFLOAT
DIM sa AS SFLOAT
DIM angle AS SFLOAT
DIM frame AS INTEGER
DIM j AS INTEGER
DIM va AS INTEGER
DIM vb AS INTEGER
DIM mzsum AS SFLOAT
DIM k AS INTEGER
DIM start_tick AS INTEGER
DIM target_tick AS INTEGER
DIM wait_ticks AS INTEGER

LET start_tick = TIMER

FOR frame = 1 TO FRAME_COUNT
    LET angle = (TOTAL_ANGLE * frame) / FRAME_COUNT
    LET ca = COS(angle)
    LET sa = SIN(angle)

    FOR i = 0 TO 7
        LET mx = bx[i]*ca + bz[i]*sa
        LET mz2 = bz[i]*ca - bx[i]*sa
        LET lx = mx
        LET ly = by[i]
        LET lz = mz2

        LET rx = lx*cy + lz*sy
        LET r1 = ((0-lx)*sy) + lz*cy
        LET ry = (ly*cx) - (r1*sx)
        LET rz = (ly*sx) + (r1*cx)
        LET dn = d - rz
        LET sc = f / dn
        LET px_arr[i] = ex + (rx*sc)
        LET py_arr[i] = ey - (ry*sc)
        LET pz_arr[i] = rz
    NEXT i

    IF have_prev = 1 THEN
        FOR j = 0 TO 11
            LET va = ea[j]
            LET vb = eb[j]
            LINE prevx[va], prevy[va], prevx[vb], prevy[vb], 0
        NEXT j
    ENDIF

    FOR j = 0 TO 11
        LET va = ea[j]
        LET vb = eb[j]
        LET mzsum = pz_arr[va] + pz_arr[vb]
        IF mzsum > 0 THEN LET k = -1 ELSE LET k = 8421504
        LINE px_arr[va], py_arr[va], px_arr[vb], py_arr[vb], k
    NEXT j

    FOR i = 0 TO 7
        LET prevx[i] = px_arr[i]
        LET prevy[i] = py_arr[i]
    NEXT i
    LET have_prev = 1

    LET target_tick = start_tick + (TOTAL_TICKS * frame) / FRAME_COUNT
    LET wait_ticks = target_tick - TIMER
    WAIT wait_ticks
NEXT frame

END
