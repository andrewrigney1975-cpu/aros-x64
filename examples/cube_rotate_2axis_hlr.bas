' cube_rotate_2axis_hlr.bas -- cube_rotate_2axis_vec.bas with hidden
' line removal: an edge is only drawn if at least one of its two
' adjacent faces is facing the camera. For a CONVEX solid like a cube
' this is exact (no partial occlusion/clipping needed) -- an edge
' bordered by two back-facing faces is always fully hidden behind the
' solid itself.
'
' Face visibility reuses the exact same ROTATE_Y/ROTATE_X pipeline the
' vertex transform uses: each face's constant object-space normal is
' rotated through the same 4 stages (object spin, then camera view),
' and the face is front-facing if the rotated normal's Z ends up
' positive (this camera setup treats +Z as "toward the camera", same
' convention the dn = d - z perspective divide already relies on).
' Still anti-aliased (LINE is unchanged) -- HLR only decides WHICH
' edges get drawn, not how each one is drawn.

DIM DURATION_SEC AS INTEGER
DIM TARGET_FPS AS INTEGER
DIM TICKS_PER_SEC AS INTEGER
DIM FRAME_COUNT AS INTEGER
DIM TOTAL_TICKS AS INTEGER
DIM ANGLE_Y_TOTAL AS SFLOAT
DIM ANGLE_X_TOTAL AS SFLOAT

LET DURATION_SEC = 10
LET TARGET_FPS = 50
LET TICKS_PER_SEC = 100
LET FRAME_COUNT = DURATION_SEC * TARGET_FPS
LET TOTAL_TICKS = DURATION_SEC * TICKS_PER_SEC
LET ANGLE_Y_TOTAL = ACOS(0.0)        ' 90 degrees, in radians
LET ANGLE_X_TOTAL = ACOS(0.0) * 2    ' 180 degrees

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

' -- Cube geometry: base(8,3), one row per corner (columns x/y/z),
' -- corner i's signs from its own bit pattern (bit0=x,bit1=y,bit2=z).
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

' -- Faces 0..5 = x-,x+,y-,y+,z-,z+. Constant object-space normals...
DIM fnx(6) AS SFLOAT
DIM fny(6) AS SFLOAT
DIM fnz(6) AS SFLOAT
LET fnx[0] = -1
LET fny[0] = 0
LET fnz[0] = 0
LET fnx[1] = 1
LET fny[1] = 0
LET fnz[1] = 0
LET fnx[2] = 0
LET fny[2] = -1
LET fnz[2] = 0
LET fnx[3] = 0
LET fny[3] = 1
LET fnz[3] = 0
LET fnx[4] = 0
LET fny[4] = 0
LET fnz[4] = -1
LET fnx[5] = 0
LET fny[5] = 0
LET fnz[5] = 1

' ...and, for each of the 12 edges, the two faces it borders (an edge
' is the intersection of exactly two of the six faces above).
DIM efa(12) AS INTEGER
DIM efb(12) AS INTEGER
LET efa[0] = 2
LET efb[0] = 4
LET efa[1] = 1
LET efb[1] = 4
LET efa[2] = 3
LET efb[2] = 4
LET efa[3] = 0
LET efb[3] = 4
LET efa[4] = 2
LET efb[4] = 5
LET efa[5] = 1
LET efb[5] = 5
LET efa[6] = 3
LET efb[6] = 5
LET efa[7] = 0
LET efb[7] = 5
LET efa[8] = 0
LET efb[8] = 2
LET efa[9] = 1
LET efb[9] = 2
LET efa[10] = 0
LET efb[10] = 3
LET efa[11] = 1
LET efb[11] = 3

DIM proj(8, 3) AS SFLOAT
DIM faceVis(6) AS INTEGER

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
DIM fa AS INTEGER
DIM fb AS INTEGER
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

        LET rotCos = cay
        LET rotSin = say
        GOSUB ROTATE_Y
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]
        LET rotCos = cax
        LET rotSin = sax
        GOSUB ROTATE_X
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]
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

    FOR i = 0 TO 5
        VSET rotIn, fnx[i], fny[i], fnz[i]

        LET rotCos = cay
        LET rotSin = say
        GOSUB ROTATE_Y
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]
        LET rotCos = cax
        LET rotSin = sax
        GOSUB ROTATE_X
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]
        LET rotCos = cy
        LET rotSin = sy
        GOSUB ROTATE_Y
        VSET rotIn, rotOut[0], rotOut[1], rotOut[2]
        LET rotCos = cx
        LET rotSin = sx
        GOSUB ROTATE_X

        IF rotOut[2] > 0 THEN LET faceVis[i] = 1 ELSE LET faceVis[i] = 0
    NEXT i

    CLS 0

    FOR j = 0 TO 11
        LET va = ea[j]
        LET vb = eb[j]
        LET fa = efa[j]
        LET fb = efb[j]
        IF faceVis[fa] = 1 OR faceVis[fb] = 1 THEN LINE proj[va, 0], proj[va, 1], proj[vb, 0], proj[vb, 1], -1
    NEXT j

    FLIP

    LET target_tick = start_tick + (TOTAL_TICKS * frame) / FRAME_COUNT
    LET wait_ticks = target_tick - TIMER
    WAIT wait_ticks
NEXT frame

END

' rotOut = rotIn rotated about the Y axis by (rotCos, rotSin).
ROTATE_Y:
VSET rotOut, rotIn[0]*rotCos + rotIn[2]*rotSin, rotIn[1], rotIn[2]*rotCos - rotIn[0]*rotSin
RETURN

' rotOut = rotIn rotated about the X axis by (rotCos, rotSin).
ROTATE_X:
VSET rotOut, rotIn[0], rotIn[1]*rotCos - rotIn[2]*rotSin, rotIn[1]*rotSin + rotIn[2]*rotCos
RETURN
