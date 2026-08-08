DIM lx AS DFLOAT
DIM ly AS DFLOAT
DIM lz AS DFLOAT
DIM rx AS DFLOAT
DIM ry AS DFLOAT
DIM rz AS DFLOAT
DIM r1 AS DFLOAT
DIM wx AS DFLOAT
DIM dn AS DFLOAT
DIM sc AS DFLOAT
DIM px AS DFLOAT
DIM py AS DFLOAT
DIM pz AS DFLOAT
DIM ox AS DFLOAT
DIM cy AS DFLOAT
DIM sy AS DFLOAT
DIM cx AS DFLOAT
DIM sx AS DFLOAT
DIM d AS DFLOAT
DIM f AS DFLOAT
DIM ex AS DFLOAT
DIM ey AS DFLOAT
DIM ang AS DFLOAT
DIM ca AS DFLOAT
DIM sa AS DFLOAT
DIM mz AS DFLOAT
DIM r AS DFLOAT
DIM astep AS DFLOAT
DIM i AS INTEGER
DIM j AS INTEGER
DIM k AS INTEGER
DIM n AS INTEGER
DIM cvx(8) AS DFLOAT
DIM cvy(8) AS DFLOAT
DIM cvz(8) AS DFLOAT
DIM spx(180) AS DFLOAT
DIM spy(180) AS DFLOAT
DIM spz(180) AS DFLOAT
DIM ax AS DFLOAT
DIM ay AS DFLOAT
DIM az AS DFLOAT
DIM cnx(60) AS DFLOAT
DIM cny(60) AS DFLOAT
DIM cnz(60) AS DFLOAT

LET cy = 0.819152
LET sy = 0.573576
LET cx = 0.939693
LET sx = 0.342020
LET d = 6
LET f = 700
LET ex = 640
LET ey = 400
LET n = 60
LET astep = 6.283185307 / n

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

LET ox = -3.2
LET lx = -1
LET ly = -1
LET lz = -1
GOSUB PROJECT
LET cvx[0] = px
LET cvy[0] = py
LET cvz[0] = pz
LET lx = 1
LET ly = -1
LET lz = -1
GOSUB PROJECT
LET cvx[1] = px
LET cvy[1] = py
LET cvz[1] = pz
LET lx = 1
LET ly = 1
LET lz = -1
GOSUB PROJECT
LET cvx[2] = px
LET cvy[2] = py
LET cvz[2] = pz
LET lx = -1
LET ly = 1
LET lz = -1
GOSUB PROJECT
LET cvx[3] = px
LET cvy[3] = py
LET cvz[3] = pz
LET lx = -1
LET ly = -1
LET lz = 1
GOSUB PROJECT
LET cvx[4] = px
LET cvy[4] = py
LET cvz[4] = pz
LET lx = 1
LET ly = -1
LET lz = 1
GOSUB PROJECT
LET cvx[5] = px
LET cvy[5] = py
LET cvz[5] = pz
LET lx = 1
LET ly = 1
LET lz = 1
GOSUB PROJECT
LET cvx[6] = px
LET cvy[6] = py
LET cvz[6] = pz
LET lx = -1
LET ly = 1
LET lz = 1
GOSUB PROJECT
LET cvx[7] = px
LET cvy[7] = py
LET cvz[7] = pz

LET mz = cvz[0] + cvz[1]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[0], cvy[0], cvx[1], cvy[1], k
LET mz = cvz[1] + cvz[2]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[1], cvy[1], cvx[2], cvy[2], k
LET mz = cvz[2] + cvz[3]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[2], cvy[2], cvx[3], cvy[3], k
LET mz = cvz[3] + cvz[0]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[3], cvy[3], cvx[0], cvy[0], k
LET mz = cvz[4] + cvz[5]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[4], cvy[4], cvx[5], cvy[5], k
LET mz = cvz[5] + cvz[6]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[5], cvy[5], cvx[6], cvy[6], k
LET mz = cvz[6] + cvz[7]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[6], cvy[6], cvx[7], cvy[7], k
LET mz = cvz[7] + cvz[4]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[7], cvy[7], cvx[4], cvy[4], k
LET mz = cvz[0] + cvz[4]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[0], cvy[0], cvx[4], cvy[4], k
LET mz = cvz[1] + cvz[5]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[1], cvy[1], cvx[5], cvy[5], k
LET mz = cvz[2] + cvz[6]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[2], cvy[2], cvx[6], cvy[6], k
LET mz = cvz[3] + cvz[7]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cvx[3], cvy[3], cvx[7], cvy[7], k

LET ox = 0
LET r = 1.3

LET i = 0
WHILE i < n
LET ang = i * astep
LET ca = COS(ang)
LET sa = SIN(ang)
LET lx = r * ca
LET ly = r * sa
LET lz = 0
GOSUB PROJECT
LET spx[i] = px
LET spy[i] = py
LET spz[i] = pz
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET ang = i * astep
LET ca = COS(ang)
LET sa = SIN(ang)
LET lx = r * ca
LET ly = 0
LET lz = r * sa
GOSUB PROJECT
LET spx[n+i] = px
LET spy[n+i] = py
LET spz[n+i] = pz
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET ang = i * astep
LET ca = COS(ang)
LET sa = SIN(ang)
LET lx = 0
LET ly = r * ca
LET lz = r * sa
GOSUB PROJECT
LET spx[2*n+i] = px
LET spy[2*n+i] = py
LET spz[2*n+i] = pz
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET j = i + 1
IF j >= n THEN LET j = 0
LET mz = spz[i] + spz[j]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE spx[i], spy[i], spx[j], spy[j], k
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET j = i + 1
IF j >= n THEN LET j = 0
LET mz = spz[n+i] + spz[n+j]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE spx[n+i], spy[n+i], spx[n+j], spy[n+j], k
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET j = i + 1
IF j >= n THEN LET j = 0
LET mz = spz[2*n+i] + spz[2*n+j]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE spx[2*n+i], spy[2*n+i], spx[2*n+j], spy[2*n+j], k
LET i = i + 1
WEND

LET ox = 3.2
LET lx = 0
LET ly = 1.0
LET lz = 0
GOSUB PROJECT
LET ax = px
LET ay = py
LET az = pz

LET r = 0.9
LET i = 0
WHILE i < n
LET ang = i * astep
LET ca = COS(ang)
LET sa = SIN(ang)
LET lx = r * ca
LET ly = -0.6
LET lz = r * sa
GOSUB PROJECT
LET cnx[i] = px
LET cny[i] = py
LET cnz[i] = pz
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET j = i + 1
IF j >= n THEN LET j = 0
LET mz = cnz[i] + cnz[j]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE cnx[i], cny[i], cnx[j], cny[j], k
LET i = i + 1
WEND

LET i = 0
WHILE i < n
LET mz = az + cnz[i]
IF mz > 0 THEN LET k = -1 ELSE LET k = 8421504
LINE ax, ay, cnx[i], cny[i], k
LET i = i + 1
WEND

END
