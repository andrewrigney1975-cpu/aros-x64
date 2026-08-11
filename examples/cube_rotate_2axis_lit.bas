' cube_rotate_2axis_lit.bas -- cube_rotate_2axis_vec.bas with filled,
' flat-shaded faces under a single directional light positioned above
' and to the right of the camera, instead of a wireframe.
'
' Each face's constant object-space normal is rotated through the same
' ROTATE_Y/ROTATE_X pipeline the vertices use (object spin, then
' camera view) to get it into camera space. A face is only drawn at
' all if that rotated normal's Z is positive (front-facing -- for a
' CONVEX solid like a cube, backface culling alone renders correctly
' with no depth buffer or draw-order sorting needed: front-facing
' faces of a convex shape never overlap each other on screen). Its
' shade is diffuse lighting: intensity = max(0, dot(normal, lightDir))
' plus a small ambient term so faces never go fully black, turned into
' a flat grayscale fill color.
'
' TRIFILL rasterizes each face (as two triangles) with a hard edge --
' no coverage blending -- so every visible face additionally has its 4
' boundary edges stroked with LINE in the same color afterward, which
' IS anti-aliased (Wu's algorithm), to soften the silhouette. That's
' the "using anti-aliasing" half of this version; TRIFILL's own fill
' is deliberately simple.

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
LET ex = SCREENW / 2
LET ey = SCREENH / 2

' -- Light: positioned above and to the right of the camera (+X right,
' -- +Y up in camera space -- see basix_rt_pset's projection convention
' -- this file's ex+wx*sc / ey-ry*sc formulas already rely on), and
' -- somewhat toward the camera (+Z) so front-facing surfaces actually
' -- catch some of it.
DIM lightDir AS VECTOR3
DIM lightDirN AS VECTOR3
VSET lightDir, 0.4, 0.5, 0.75
VNORM lightDirN, lightDir

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

' -- Faces 0..5 = x-,x+,y-,y+,z-,z+: fv(6,4) is each face's 4 corner
' -- indices IN ORDER around its boundary (so consecutive pairs, and
' -- the wrap from column 3 back to column 0, are the face's real
' -- edges), fnx/fny/fnz(6) its constant object-space normal.
DIM fv(6, 4) AS INTEGER
LET fv[0, 0] = 0
LET fv[0, 1] = 2
LET fv[0, 2] = 6
LET fv[0, 3] = 4
LET fv[1, 0] = 1
LET fv[1, 1] = 3
LET fv[1, 2] = 7
LET fv[1, 3] = 5
LET fv[2, 0] = 0
LET fv[2, 1] = 1
LET fv[2, 2] = 5
LET fv[2, 3] = 4
LET fv[3, 0] = 2
LET fv[3, 1] = 3
LET fv[3, 2] = 7
LET fv[3, 3] = 6
LET fv[4, 0] = 0
LET fv[4, 1] = 1
LET fv[4, 2] = 3
LET fv[4, 3] = 2
LET fv[5, 0] = 4
LET fv[5, 1] = 5
LET fv[5, 2] = 7
LET fv[5, 3] = 6

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

DIM proj(8, 3) AS SFLOAT

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
DIM intensity AS SFLOAT
DIM g AS INTEGER
DIM litColor AS INTEGER
DIM v0 AS INTEGER
DIM v1 AS INTEGER
DIM v2 AS INTEGER
DIM v3 AS INTEGER
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

    CLS 0

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

        IF rotOut[2] > 0 THEN
            VDOT intensity, rotOut, lightDirN
            IF intensity < 0 THEN LET intensity = 0
            LET intensity = 0.25 + (0.75 * intensity)
            LET g = intensity * 255
            LET litColor = g + (g*256) + (g*65536)

            LET v0 = fv[i, 0]
            LET v1 = fv[i, 1]
            LET v2 = fv[i, 2]
            LET v3 = fv[i, 3]

            TRIFILL proj[v0,0], proj[v0,1], proj[v1,0], proj[v1,1], proj[v2,0], proj[v2,1], litColor
            TRIFILL proj[v0,0], proj[v0,1], proj[v2,0], proj[v2,1], proj[v3,0], proj[v3,1], litColor

            LINE proj[v0,0], proj[v0,1], proj[v1,0], proj[v1,1], litColor
            LINE proj[v1,0], proj[v1,1], proj[v2,0], proj[v2,1], litColor
            LINE proj[v2,0], proj[v2,1], proj[v3,0], proj[v3,1], litColor
            LINE proj[v3,0], proj[v3,1], proj[v0,0], proj[v0,1], litColor
        ENDIF
    NEXT i

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
