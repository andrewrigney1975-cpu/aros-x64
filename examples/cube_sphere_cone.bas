DIM lx AS SFLOAT
DIM ly AS SFLOAT
DIM lz AS SFLOAT
DIM rx AS SFLOAT
DIM ry AS SFLOAT
DIM rz AS SFLOAT
DIM r1 AS SFLOAT
DIM wx AS SFLOAT
DIM dn AS SFLOAT
DIM sc AS SFLOAT
DIM px AS SFLOAT
DIM py AS SFLOAT
DIM pz AS SFLOAT
DIM ox AS SFLOAT
DIM mz AS SFLOAT
DIM cy AS SFLOAT
DIM sy AS SFLOAT
DIM cx AS SFLOAT
DIM sx AS SFLOAT
DIM d AS SFLOAT
DIM f AS SFLOAT
DIM ex AS SFLOAT
DIM ey AS SFLOAT
DIM x0 AS SFLOAT
DIM x1 AS SFLOAT
DIM x2 AS SFLOAT
DIM x3 AS SFLOAT
DIM x4 AS SFLOAT
DIM x5 AS SFLOAT
DIM x6 AS SFLOAT
DIM x7 AS SFLOAT
DIM y0 AS SFLOAT
DIM y1 AS SFLOAT
DIM y2 AS SFLOAT
DIM y3 AS SFLOAT
DIM y4 AS SFLOAT
DIM y5 AS SFLOAT
DIM y6 AS SFLOAT
DIM y7 AS SFLOAT
DIM z0 AS SFLOAT
DIM z1 AS SFLOAT
DIM z2 AS SFLOAT
DIM z3 AS SFLOAT
DIM z4 AS SFLOAT
DIM z5 AS SFLOAT
DIM z6 AS SFLOAT
DIM z7 AS SFLOAT
DIM ax AS SFLOAT
DIM ay AS SFLOAT
DIM az AS SFLOAT
LET cy = 0.8
LET sy = 0.6
LET cx = 0.9
LET sx = 0.3
LET d = 6
LET f = 700
LET ex = 640
LET ey = 400
GOTO MAIN
PROJECT:
LET rx = lx*cy + lz*sy
LET r1 = ((0-lx)*sy) + lz*cy
LET ry = (ly*cx) - (r1*sx)
LET rz = (ly*sx) + (r1*cx)
LET wx = rx + ox
LET dn = d - rz
LET sc = f / dn
LET px = ex + (wx*sc)
LET py = ey - (ry*sc)
LET pz = rz
RETURN
MAIN:
LET ox = -2.8
LET lx = -1.0
LET ly = -1.0
LET lz = -1.0
GOSUB PROJECT
LET x0 = px
LET y0 = py
LET z0 = pz
LET lx = 1.0
LET ly = -1.0
LET lz = -1.0
GOSUB PROJECT
LET x1 = px
LET y1 = py
LET z1 = pz
LET lx = 1.0
LET ly = 1.0
LET lz = -1.0
GOSUB PROJECT
LET x2 = px
LET y2 = py
LET z2 = pz
LET lx = -1.0
LET ly = 1.0
LET lz = -1.0
GOSUB PROJECT
LET x3 = px
LET y3 = py
LET z3 = pz
LET lx = -1.0
LET ly = -1.0
LET lz = 1.0
GOSUB PROJECT
LET x4 = px
LET y4 = py
LET z4 = pz
LET lx = 1.0
LET ly = -1.0
LET lz = 1.0
GOSUB PROJECT
LET x5 = px
LET y5 = py
LET z5 = pz
LET lx = 1.0
LET ly = 1.0
LET lz = 1.0
GOSUB PROJECT
LET x6 = px
LET y6 = py
LET z6 = pz
LET lx = -1.0
LET ly = 1.0
LET lz = 1.0
GOSUB PROJECT
LET x7 = px
LET y7 = py
LET z7 = pz
LET mz = z0 + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x1, y1, k
LET mz = z1 + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x2, y2, k
LET mz = z2 + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x3, y3, k
LET mz = z3 + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x0, y0, k
LET mz = z4 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x4, y4, x5, y5, k
LET mz = z5 + z6
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x5, y5, x6, y6, k
LET mz = z6 + z7
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x6, y6, x7, y7, k
LET mz = z7 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x7, y7, x4, y4, k
LET mz = z0 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x4, y4, k
LET mz = z1 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x5, y5, k
LET mz = z2 + z6
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x6, y6, k
LET mz = z3 + z7
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x7, y7, k
LET ox = 0
LET lx = 1.3
LET ly = 0.0
LET lz = 0.0
GOSUB PROJECT
LET x0 = px
LET y0 = py
LET z0 = pz
LET lx = 0.7
LET ly = 1.1
LET lz = 0.0
GOSUB PROJECT
LET x1 = px
LET y1 = py
LET z1 = pz
LET lx = -0.6
LET ly = 1.1
LET lz = 0.0
GOSUB PROJECT
LET x2 = px
LET y2 = py
LET z2 = pz
LET lx = -1.3
LET ly = 0.0
LET lz = 0.0
GOSUB PROJECT
LET x3 = px
LET y3 = py
LET z3 = pz
LET lx = -0.7
LET ly = -1.1
LET lz = 0.0
GOSUB PROJECT
LET x4 = px
LET y4 = py
LET z4 = pz
LET lx = 0.7
LET ly = -1.1
LET lz = 0.0
GOSUB PROJECT
LET x5 = px
LET y5 = py
LET z5 = pz
LET mz = z0 + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x1, y1, k
LET mz = z1 + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x2, y2, k
LET mz = z2 + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x3, y3, k
LET mz = z3 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x4, y4, k
LET mz = z4 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x4, y4, x5, y5, k
LET mz = z5 + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x5, y5, x0, y0, k
LET lx = 1.3
LET ly = 0.0
LET lz = 0.0
GOSUB PROJECT
LET x0 = px
LET y0 = py
LET z0 = pz
LET lx = 0.7
LET ly = 0.0
LET lz = 1.1
GOSUB PROJECT
LET x1 = px
LET y1 = py
LET z1 = pz
LET lx = -0.6
LET ly = 0.0
LET lz = 1.1
GOSUB PROJECT
LET x2 = px
LET y2 = py
LET z2 = pz
LET lx = -1.3
LET ly = 0.0
LET lz = 0.0
GOSUB PROJECT
LET x3 = px
LET y3 = py
LET z3 = pz
LET lx = -0.7
LET ly = 0.0
LET lz = -1.1
GOSUB PROJECT
LET x4 = px
LET y4 = py
LET z4 = pz
LET lx = 0.7
LET ly = 0.0
LET lz = -1.1
GOSUB PROJECT
LET x5 = px
LET y5 = py
LET z5 = pz
LET mz = z0 + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x1, y1, k
LET mz = z1 + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x2, y2, k
LET mz = z2 + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x3, y3, k
LET mz = z3 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x4, y4, k
LET mz = z4 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x4, y4, x5, y5, k
LET mz = z5 + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x5, y5, x0, y0, k
LET lx = 0.0
LET ly = 1.3
LET lz = 0.0
GOSUB PROJECT
LET x0 = px
LET y0 = py
LET z0 = pz
LET lx = 0.0
LET ly = 0.7
LET lz = 1.1
GOSUB PROJECT
LET x1 = px
LET y1 = py
LET z1 = pz
LET lx = 0.0
LET ly = -0.6
LET lz = 1.1
GOSUB PROJECT
LET x2 = px
LET y2 = py
LET z2 = pz
LET lx = 0.0
LET ly = -1.3
LET lz = 0.0
GOSUB PROJECT
LET x3 = px
LET y3 = py
LET z3 = pz
LET lx = 0.0
LET ly = -0.7
LET lz = -1.1
GOSUB PROJECT
LET x4 = px
LET y4 = py
LET z4 = pz
LET lx = 0.0
LET ly = 0.7
LET lz = -1.1
GOSUB PROJECT
LET x5 = px
LET y5 = py
LET z5 = pz
LET mz = z0 + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x1, y1, k
LET mz = z1 + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x2, y2, k
LET mz = z2 + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x3, y3, k
LET mz = z3 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x4, y4, k
LET mz = z4 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x4, y4, x5, y5, k
LET mz = z5 + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x5, y5, x0, y0, k
LET ox = 2.8
LET lx = 0.0
LET ly = 1.0
LET lz = 0.0
GOSUB PROJECT
LET ax = px
LET ay = py
LET az = pz
LET lx = 0.9
LET ly = -0.6
LET lz = 0.0
GOSUB PROJECT
LET x0 = px
LET y0 = py
LET z0 = pz
LET lx = 0.5
LET ly = -0.6
LET lz = 0.8
GOSUB PROJECT
LET x1 = px
LET y1 = py
LET z1 = pz
LET lx = -0.4
LET ly = -0.6
LET lz = 0.8
GOSUB PROJECT
LET x2 = px
LET y2 = py
LET z2 = pz
LET lx = -0.9
LET ly = -0.6
LET lz = 0.0
GOSUB PROJECT
LET x3 = px
LET y3 = py
LET z3 = pz
LET lx = -0.5
LET ly = -0.6
LET lz = -0.8
GOSUB PROJECT
LET x4 = px
LET y4 = py
LET z4 = pz
LET lx = 0.5
LET ly = -0.6
LET lz = -0.8
GOSUB PROJECT
LET x5 = px
LET y5 = py
LET z5 = pz
LET mz = z0 + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x0, y0, x1, y1, k
LET mz = z1 + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x1, y1, x2, y2, k
LET mz = z2 + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x2, y2, x3, y3, k
LET mz = z3 + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x3, y3, x4, y4, k
LET mz = z4 + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x4, y4, x5, y5, k
LET mz = z5 + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE x5, y5, x0, y0, k
LET mz = az + z0
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x0, y0, k
LET mz = az + z1
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x1, y1, k
LET mz = az + z2
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x2, y2, k
LET mz = az + z3
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x3, y3, k
LET mz = az + z4
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x4, y4, k
LET mz = az + z5
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, x5, y5, k
END
