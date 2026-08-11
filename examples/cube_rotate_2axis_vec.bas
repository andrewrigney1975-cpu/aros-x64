' cube_rotate_2axis_vec.bas -- same two-axis spinning wireframe cube as
' cube_rotate_2axis.bas, but the per-vertex transform's scalar temp
' variables (lx,ly,lz, mx,my,mz2, nx,ny,nz, rx,ry,rz,r1 in that
' version) are replaced by two VECTOR3s and two reusable rotation
' subroutines. Both the object's own 2-axis spin AND the camera's
' fixed 3/4-view rotation turn out to be the exact same operation --
' "rotate this vector about Y" / "rotate this vector about X" -- just
' called with different (cos,sin) pairs, so ROTATE_Y/ROTATE_X get
' called 4 times total instead of that logic being written out longhand
' twice.

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

' -- Camera setup (static 3/4 view -- cy/sy is the view's own Y-axis
' -- rotation, cx/sx its X-axis rotation, applied via ROTATE_Y/
' -- ROTATE_X below exactly like the object's spin is).
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
' -- from its own bit pattern (bit0=x, bit1=y, bit2=z). Arrays can't
' -- hold VECTOR3 elements (BASIX64 arrays are INTEGER/SFLOAT only),
' -- which is exactly why this stays a 2D SFLOAT array rather than an
' -- array of vectors -- the VECTOR3 replacement below is for the
' -- per-frame WORKING variables, not this storage.
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

' -- ROTATE_Y/ROTATE_X's shared in/out and (cos,sin) arguments -- see
' -- the subroutines themselves, after END.
DIM rotIn AS VECTOR3
DIM rotOut AS VECTOR3
DIM rotCos AS SFLOAT
DIM rotSin AS SFLOAT

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
        VSET rotIn, base[i, 0], base[i, 1], base[i, 2]

        ' object spin: about Y, then about X
        LET rotCos = cay
        LET rotSin = say
        GOSUB ROTATE_Y
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]

        LET rotCos = cax
        LET rotSin = sax
        GOSUB ROTATE_X
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]

        ' camera's fixed 3/4 view: about Y, then about X
        LET rotCos = cy
        LET rotSin = sy
        GOSUB ROTATE_Y
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]

        LET rotCos = cx
        LET rotSin = sx
        GOSUB ROTATE_X

        LET dn = d - rotOut[2]
        LET sc = f / dn
        LET proj[i, 0] = ex + (rotOut[0]*sc)
        LET proj[i, 1] = ey - (rotOut[1]*sc)
        LET proj[i, 2] = rotOut[2]
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

' rotOut = rotIn rotated about the Y axis by (rotCos, rotSin) --
' x'=x*cos+z*sin, y'=y (unchanged), z'=z*cos-x*sin.
ROTATE_Y:
VSET rotOut, rotIn[0]*rotCos + rotIn[2]*rotSin, rotIn[1], rotIn[2]*rotCos - rotIn[0]*rotSin
RETURN

' rotOut = rotIn rotated about the X axis by (rotCos, rotSin) --
' x'=x (unchanged), y'=y*cos-z*sin, z'=y*sin+z*cos.
ROTATE_X:
VSET rotOut, rotIn[0], rotIn[1]*rotCos - rotIn[2]*rotSin, rotIn[1]*rotSin + rotIn[2]*rotCos
RETURN
