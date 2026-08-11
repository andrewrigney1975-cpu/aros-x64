' cube_rotate_2axis.bas -- wireframe cube spinning about BOTH the X and
' Y axes at once (X completing a half turn, Y a quarter turn, over the
' same DURATION_SEC/TARGET_FPS-derived timeline as cube_rotate_90.bas)
' -- rewritten around genuine multi-dimensional arrays instead of one
' parallel 1D array per coordinate: base(8,3) holds each of the 8 cube
' corners' (x,y,z) as one row, proj(8,3) the same for each frame's
' projected (screenX, screenY, cameraZ). Composed off-screen (CLS/LINE
' into the back buffer) and presented with FLIP, same as before.

DIM DURATION_SEC AS INTEGER
DIM TARGET_FPS AS INTEGER
DIM TICKS_PER_SEC AS INTEGER
DIM FRAME_COUNT AS INTEGER
DIM TOTAL_TICKS AS INTEGER
DIM ANGLE_Y_TOTAL AS SFLOAT
DIM ANGLE_X_TOTAL AS SFLOAT

LET DURATION_SEC = 10
LET TARGET_FPS = 50
LET TICKS_PER_SEC = 100          ' PIT rate this kernel programs (pic.inc, PIT_HZ) --
                                  ' TIMER/WAIT both count in these ticks
LET FRAME_COUNT = DURATION_SEC * TARGET_FPS
LET TOTAL_TICKS = DURATION_SEC * TICKS_PER_SEC
LET ANGLE_Y_TOTAL = ACOS(0.0)        ' 90 degrees, in radians -- exact, no pi literal
LET ANGLE_X_TOTAL = ACOS(0.0) * 2    ' 180 degrees -- same trick, doubled

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
LET ex = SCREENW / 2
LET ey = SCREENH / 2

' -- Cube geometry: base(8,3) is 8 corners of a unit cube, one row per
' -- corner, columns 0/1/2 = x/y/z (object-space, never rotated in
' -- place -- each frame re-spins from these). Corner i's signs come
' -- from its own bit pattern (bit0=x, bit1=y, bit2=z), same trick
' -- cube_rotate_90.bas used, just written into a 2D array now.
DIM base(8, 3) AS SFLOAT
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
    LET base[i, 0] = (xb*2 - 1) * HS
    LET base[i, 1] = (yb*2 - 1) * HS
    LET base[i, 2] = (zb*2 - 1) * HS
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

' -- Per-frame projected screen position/depth: proj(8,3), one row per
' -- corner, columns 0/1/2 = screenX/screenY/cameraZ.
DIM proj(8, 3) AS SFLOAT

DIM lx AS SFLOAT
DIM ly AS SFLOAT
DIM lz AS SFLOAT
DIM mx AS SFLOAT
DIM my AS SFLOAT
DIM mz2 AS SFLOAT
DIM nx AS SFLOAT
DIM ny AS SFLOAT
DIM nz AS SFLOAT
DIM rx AS SFLOAT
DIM ry AS SFLOAT
DIM rz AS SFLOAT
DIM r1 AS SFLOAT
DIM dn AS SFLOAT
DIM sc AS SFLOAT
DIM cay AS SFLOAT
DIM say AS SFLOAT
DIM cax AS SFLOAT
DIM sax AS SFLOAT
DIM angleY AS SFLOAT
DIM angleX AS SFLOAT
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
    LET angleY = (ANGLE_Y_TOTAL * frame) / FRAME_COUNT
    LET cay = COS(angleY)
    LET say = SIN(angleY)
    LET angleX = (ANGLE_X_TOTAL * frame) / FRAME_COUNT
    LET cax = COS(angleX)
    LET sax = SIN(angleX)

    FOR i = 0 TO 7
        ' spin about Y first: x' = x*cos+z*sin, z' = z*cos-x*sin, y'=y
        LET mx = base[i, 0]*cay + base[i, 2]*say
        LET my = base[i, 1]
        LET mz2 = base[i, 2]*cay - base[i, 0]*say

        ' then about X: y''=y'*cos-z'*sin, z''=y'*sin+z'*cos, x''=x'
        LET nx = mx
        LET ny = my*cax - mz2*sax
        LET nz = my*sax + mz2*cax

        LET lx = nx
        LET ly = ny
        LET lz = nz

        LET rx = lx*cy + lz*sy
        LET r1 = ((0-lx)*sy) + lz*cy
        LET ry = (ly*cx) - (r1*sx)
        LET rz = (ly*sx) + (r1*cx)
        LET dn = d - rz
        LET sc = f / dn
        LET proj[i, 0] = ex + (rx*sc)
        LET proj[i, 1] = ey - (ry*sc)
        LET proj[i, 2] = rz
    NEXT i

    CLS 0

    FOR j = 0 TO 11
        LET va = ea[j]
        LET vb = eb[j]
        LET mzsum = proj[va, 2] + proj[vb, 2]
        IF mzsum > 0 THEN LET k = -1 ELSE LET k = 8421504
        LINE proj[va, 0], proj[va, 1], proj[vb, 0], proj[vb, 1], k
    NEXT j

    FLIP

    LET target_tick = start_tick + (TOTAL_TICKS * frame) / FRAME_COUNT
    LET wait_ticks = target_tick - TIMER
    WAIT wait_ticks
NEXT frame

END
