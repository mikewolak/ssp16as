#!/usr/bin/env python3
"""mkmesh.py -- tables of the SVP mesh cartridge: an original low-poly figure (convex parts: ellipsoid head,
frustum limbs and torso, box hands and feet) standing at the centre of a grid room (floor and back wall drawn as
thin line quads in one green palette entry on black).

  mkmesh.py <out_dir> [--no-room] [--floor-infinite | --floor-room | --no-treadmill] [--floor N] [--wall-v N] [--wall-h N]
  --grad-pure: gradient colours within 1.2 levels of the floor hue (27 colours) instead of the 68-colour greens
  --no-raster-fade: no 68000 line gradient of the floor colour (default on for the infinite floor)
  --fade N: depth fade of the infinite floor in N bands (default 1 with the raster gradient, else 3) (default 3, at most the free palette entries + 1; 1 = off)
  room builds: --floor-infinite (default) treadmill ground plane to a horizon, no wall; --floor-room the back wall
  and bordered treadmill floor; --no-treadmill the back wall and the static floor grid
writes <out_dir>/tables.inc (ssp16as source), palette.h (68000 CRAM words), mesh.json (model data).

Coordinates: model units, y down, figure centre at the origin, camera on +z looking at -z.
Parts: static parts (the room) first, transformed at angle 0; then the figure's parts, rotated about Y and drawn
far to near by the depth of their rotated centre.  Face vertex order: the right-hand (Newell) normal points out of
the part; a face turned to the camera then projects with negative shoelace area, the cull test's front condition.
"""
import json, math, os, sys
import numpy as np

D, F, RSH = 4200, 900.0, 4          # camera distance, focal length (px), RECIP index shift
TILT = -14.0                        # rotation about X after the Y rotation: camera above the figure
LIGHT = (-0.45, -0.55, 0.70)
AMB = 0.30
CY = 100                            # screen row of the origin
FLOOR_Y = 330                       # floor plane (figure's feet)
WALL_Z = -1000                      # back wall plane
NEAR_Z = 700                        # floor front edge
LINE_PX = 1.6                       # projected line width, pixels
BG = (0, 0, 0)
GREEN = (0, 7, 2)                   # R 0, G 0.95, B 0.3 in Genesis 3-bit levels
# figure materials, Genesis 3-bit RGB at full light: suit, skin, accent
MATS = [(1, 2, 5), (6, 5, 4), (7, 3, 0)]
NSH = 4                             # shades per figure material

def q15(x): return int(max(-32767, min(32767, round(x * 32767))))
SIN = [q15(math.sin(2 * math.pi * i / 256)) for i in range(256)]
RECIP = [min(32767, round(F * 32768 / ((i << RSH) + (1 << (RSH - 1))))) for i in range(1024)]
ln = math.sqrt(sum(c * c for c in LIGHT)); LQ = [q15(c / ln) for c in LIGHT]
CT, ST = q15(math.cos(math.radians(TILT))), q15(math.sin(math.radians(TILT)))
def cram(r, g, b): return (b << 9) | (g << 5) | (r << 1)

# ------------------------------------------------------------------ geometry
def orient(verts, faces, point=None, direction=None):
    """faces as index lists; reorder each so its Newell normal points away from `point` or against `direction` (outward)"""
    out = []
    V = np.array(verts, float)
    for f in faces:
        p = V[f]; n = np.zeros(3)
        for k in range(len(f)): n += np.cross(p[k], p[(k + 1) % len(f)])
        ref = (point - p.mean(0)) if point is not None else direction
        if np.dot(n, ref) > 0: f = f[::-1]
        out.append(list(f))
    return out

def newell(p):
    n = np.zeros(3)
    for k in range(len(p)): n += np.cross(p[k], p[(k + 1) % len(p)])
    return n / np.linalg.norm(n)

def frustum(c0, c1, r0, r1, n, rot=0.0):
    """convex prism/frustum along c0->c1; r0, r1 = (rx, rz) radii at each end; n sides; caps fanned to triangles"""
    c0, c1 = np.array(c0, float), np.array(c1, float)
    ax = c1 - c0; ax /= np.linalg.norm(ax)
    u = np.array([1.0, 0, 0]) - ax * ax[0]; u /= np.linalg.norm(u)
    w = np.cross(ax, u)
    V = []
    for c, (rx, rz) in ((c0, r0), (c1, r1)):
        for i in range(n):
            t = 2 * math.pi * i / n + rot
            V.append(c + u * rx * math.cos(t) + w * rz * math.sin(t))
    Fc = [[i, (i + 1) % n, n + (i + 1) % n, n + i] for i in range(n)]
    Fc += [[0, i, i + 1] for i in range(1, n - 1)]
    Fc += [[n, n + i, n + i + 1] for i in range(1, n - 1)]
    return V, Fc

def box(c, h):
    c = np.array(c, float); h = np.array(h, float)
    V = [c + h * np.array([sx, sy, sz]) for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
    Fc = [[0, 1, 3, 2], [4, 5, 7, 6], [0, 1, 5, 4], [2, 3, 7, 6], [0, 2, 6, 4], [1, 3, 7, 5]]
    return V, Fc

def ellipsoid(c, r, nlon=8, nlat=5):
    """latitude-longitude ellipsoid of revolution about y (r[0] = r[2]): planar quads, triangle caps"""
    c = np.array(c, float)
    V = [c + np.array([0, -r[1], 0])]
    for j in range(1, nlat):
        ph = math.pi * j / nlat
        for i in range(nlon):
            t = 2 * math.pi * i / nlon
            V.append(c + np.array([r[0] * math.sin(ph) * math.cos(t), -r[1] * math.cos(ph), r[0] * math.sin(ph) * math.sin(t)]))
    V.append(c + np.array([0, r[1], 0]))
    bot = len(V) - 1
    Fc = [[0, 1 + i, 1 + (i + 1) % nlon] for i in range(nlon)]
    for j in range(nlat - 2):
        a, b = 1 + j * nlon, 1 + (j + 1) * nlon
        Fc += [[a + i, b + i, b + (i + 1) % nlon, a + (i + 1) % nlon] for i in range(nlon)]
    a = 1 + (nlat - 2) * nlon
    Fc += [[bot, a + (i + 1) % nlon, a + i] for i in range(nlon)]
    return V, Fc

def figure():
    """original mannequin, A-pose, about 660 units tall; returns [(name, verts, faces, material)]"""
    P = []
    P.append(('head', *ellipsoid((0, -265, 0), (54, 62, 54), 8, 5), 1))
    P.append(('visor', *box((0, -272, 50), (40, 10, 8)), 2))
    P.append(('pack', *box((0, -115, -62), (55, 50, 18)), 2))
    P.append(('neck', *frustum((0, -205, 0), (0, -180, 0), (22, 22), (26, 26), 6), 1))
    P.append(('chest', *frustum((0, -180, 0), (0, -70, 0), (95, 52), (72, 44), 8), 0))
    P.append(('belt', *frustum((0, -70, 0), (0, -40, 0), (74, 45), (78, 47), 8), 2))
    P.append(('hips', *frustum((0, -40, 0), (0, 15, 0), (80, 48), (62, 42), 8), 0))
    for s in (-1, 1):
        sh = (s * 110, -165, 0); el = (s * 150, -55, 0); wr = (s * 175, 45, 0)
        P.append(('uarm', *frustum(sh, el, (24, 24), (20, 20), 6), 0))
        P.append(('farm', *frustum(el, wr, (19, 19), (15, 15), 6), 1))
        P.append(('hand', *box((s * 182, 70, 0), (12, 24, 16)), 2))
        hp = (s * 42, 15, 0); kn = (s * 52, 170, 0); an = (s * 56, 300, 0)
        P.append(('thigh', *frustum(hp, kn, (34, 36), (24, 26), 8), 0))
        P.append(('shin', *frustum(kn, an, (23, 25), (16, 18), 6), 0))
        P.append(('foot', *box((s * 56, 315, 20), (20, 15, 42)), 2))
    out = []
    for name, V, Fc, m in P:
        c = np.mean(V, 0)
        out.append((name, V, orient(V, Fc, point=c), m))
    return out

# ------------------------------------------------------------------ room (float projection used only to size lines)
def thick(z, px=None):
    """depth half thickness of a floor line at z giving px (default LINE_PX) rows"""
    px = LINE_PX if px is None else px
    lo, hi_ = 0.1, 400.0
    for _ in range(50):
        m = (lo + hi_) / 2
        dy = fproj((0, FLOOR_Y, z + m))[1] - fproj((0, FLOOR_Y, z - m))[1]
        (lo, hi_) = (lo, m) if dy > px else (m, hi_)
    return lo

TM_CELL = 160                       # treadmill grid cell, model units (square); --cell N
TM_V = 21                           # floor speed, units a frame: one cell in 7.6 frames (0.51 s at 15 frames a second); --cell sets TM_V = N * 2 / 15
TM_SH = 5                           # floor table index: (z - far z) >> 5, |x| >> 5
SCR_MARGIN = 124                    # largest |sx - 128| a floor vertex may project to (float), pointer registers 0..255
HOR = (0, 3, 1)                     # horizon line, dim green (no fade)
FADE_FAR = (0, 2, 1)                # infinite floor fade: colour of the farthest band (3-bit RGB)
CROSS_PX = 1.2                      # cross-line thickness, rows (infinite floor)
THIN_PX = 5.0                       # infinite floor: beyond the z where cross-lines are closer than this, every second line

def fproj(p):
    x, y, z = p
    ct, st = math.cos(math.radians(TILT)), math.sin(math.radians(TILT))
    yt = y * ct - z * st; zt = y * st + z * ct
    d = D - zt
    return 128 - F * x / d, CY + F * yt / d, d

def sx_off(x, z): return abs(fproj((x, FLOOR_Y, z))[0] - 128)

def treadmill(mode, WF=None, hw_at=None):
    """Treadmill floor tables (mesh.s floor, mesh_model.treadmill).  The figure faces its local +z; at frame k
    (theta = 2 pi (4k & 255) / 256) its heading is (sin theta, cos theta) and the floor moves at -TM_V along it; the
    floor offset is the integral P(k) = R (cos theta, -sin theta), R = TM_V / (2 pi / 64), periodic in 64 frames.
    Lines front to back at x = P_x (mod TM_CELL), cross-lines at z = P_z (mod TM_CELL).
      mode 'infinite': ground plane from the screen bottom to the far z where the cross-line spacing falls to 2 px
        (the horizon, drawn in dim green); front-to-back lines converge, each cut at the near z where it would leave
        the screen (ZCT by |x|); cross-lines cut to the screen width (XWT by z).
      mode 'room': the bordered floor between the back wall and NEAR_Z (border as a static part).
    Per frame (TMTAB, 8 words): first x-line, x-line count, first far cross-line (every second line beyond ZTHIN),
    far count, first near cross-line, near count, 0, 0.
    Per z bucket: HWT (half width of a front-to-back line), THK (half thickness of a cross-line, >= 1 px rows),
    XWT (cross-line half extent)."""
    FY = FLOOR_Y
    if mode == 'infinite':
        z = 0
        while fproj((0, FY, z))[1] - fproj((0, FY, z - TM_CELL))[1] >= 2.0: z -= 16
        ZF = z
        z = 2000
        while fproj((0, FY, z))[1] > 190.0: z -= 4
        NZ = z
    else:
        ZF, NZ = WALL_Z, NEAR_Z
    n = ((NZ - ZF) >> TM_SH) + 2
    zb = [ZF + (i << TM_SH) for i in range(n)]                                    # bucket start
    def hw(z): return LINE_PX / 2 * fproj((0, FY, z))[2] / F
    HWT = [max(1, int(round(hw(z + (1 << TM_SH) - 1)))) for z in zb]
    THK = [max(1, int(math.ceil(thick(z + (1 << TM_SH) - 1, CROSS_PX if mode == 'infinite' else None)))) for z in zb]
    border = []
    if mode == 'infinite':
        # cross-line half extent: the screen width at the bucket's nearest z, less the margin
        XWT = [int(SCR_MARGIN * fproj((0, FY, min(z + (1 << TM_SH) + THK[i], NZ + THK[-1])))[2] / F) for i, z in enumerate(zb)]
        XMAX = XWT[0] - 2 * HWT[0]
        ZCT = []
        for b in range((XMAX >> TM_SH) + 1):
            xm = ((b + 1) << TM_SH)                                               # largest |x| in the bucket, + 1
            z = NZ
            while z > ZF and sx_off(xm + hw(z) + 1, z) > SCR_MARGIN: z -= 4
            ZCT.append(z)
        HOR_ON = 1
    else:
        HN = HWT[-1]; TN = THK[-1]
        XWT = [WF] * n
        XMAX = WF - 3 * max(HWT)
        z = NZ
        while fproj((0, FY, z))[1] > 190.0: z -= 4                         # one-pixel lines: rows inside 0..191
        ZCT = [z] * ((XMAX >> TM_SH) + 1)
        border = [[(-WF, FY, ZF), (-WF + 2 * HWT[0], FY, ZF), (-WF + 2 * HN, FY, NZ), (-WF, FY, NZ)],
                  [(WF - 2 * HWT[0], FY, ZF), (WF, FY, ZF), (WF, FY, NZ), (WF - 2 * HN, FY, NZ)],
                  [(-WF, FY, NZ - 2 * TN), (WF, FY, NZ - 2 * TN), (WF, FY, NZ), (-WF, FY, NZ)]]
        HOR_ON = 0
    ZTHIN = ZF
    if mode == 'infinite':
        z = NZ
        while z > ZF and fproj((0, FY, z))[1] - fproj((0, FY, z - TM_CELL))[1] >= THIN_PX: z -= 16
        ZTHIN = z
    R = TM_V / (2 * math.pi / 64)
    tab = []
    for k in range(64):
        th = 2 * math.pi * ((4 * k) & 255) / 256
        px, pz = int(round(R * math.cos(th))), int(round(-R * math.sin(th)))
        x0 = -XMAX + ((px + XMAX) % TM_CELL)
        nx = (XMAX - x0) // TM_CELL + 1
        def zlines(step, lo, hi_):
            out = []; z = ZF + ((pz - ZF) % step)
            while z <= NZ:
                t = THK[(z - ZF) >> TM_SH]
                if z - t > ZF + THK[0] and z + t < NZ - 2 * THK[-1] and lo <= z < hi_: out.append(z)
                z += step
            return out
        far = zlines(2 * TM_CELL, ZF, ZTHIN); near = zlines(TM_CELL, ZTHIN, NZ + 1)
        tab += [x0, nx, far[0] if far else 0, len(far), near[0] if near else 0, len(near), 0, 0]
    return {'border': border, 'HF': HWT[0], 'FY': FY, 'ZF': ZF, 'NZ': NZ, 'CELL': TM_CELL, 'SH': TM_SH,
            'HWT': HWT, 'THK': THK, 'XWT': XWT, 'ZCT': ZCT, 'TMTAB': tab, 'HOR': HOR_ON, 'XMAX': XMAX, 'ZTHIN': ZTHIN}

def room(nfloor, nwv, nwh, tread=False):
    # floor half width: the front corners stay inside x 4..251
    lo, hi_ = 1.0, 3000.0
    for _ in range(60):
        m = (lo + hi_) / 2
        (lo, hi_) = (m, hi_) if fproj((m, FLOOR_Y, NEAR_Z))[0] > 4 else (lo, m)
    WF = math.floor(lo)
    # wall top: as high as the viewport allows (row 2)
    lo, hi_ = 0.0, 4000.0
    for _ in range(60):
        m = (lo + hi_) / 2
        (lo, hi_) = (m, hi_) if fproj((0, FLOOR_Y - m, WALL_Z))[1] > 2 else (lo, m)
    WH = math.floor(lo)
    def hw_at(p): return LINE_PX / 2 * fproj(p)[2] / F           # half width giving LINE_PX on screen
    floorq, wallq = [], []
    # floor: lines along z at x_i, clipped at the wall; lines along x at z_j
    for i in range(nfloor):
        x = -WF + 2 * WF * i / (nfloor - 1)
        hf, hn = hw_at((x, FLOOR_Y, WALL_Z)), hw_at((x, FLOOR_Y, NEAR_Z))
        floorq.append([(x - hf, FLOOR_Y, WALL_Z), (x + hf, FLOOR_Y, WALL_Z), (x + hn, FLOOR_Y, NEAR_Z), (x - hn, FLOOR_Y, NEAR_Z)])
    for j in range(1, nfloor):
        z = WALL_Z + (NEAR_Z - WALL_Z) * j / (nfloor - 1)
        t = thick(z)
        z0, z1 = max(WALL_Z, z - t), z + t
        if j == nfloor - 1: z0, z1 = z - 2 * t, z
        floorq.append([(-WF, FLOOR_Y, z0), (WF, FLOOR_Y, z0), (WF, FLOOR_Y, z1), (-WF, FLOOR_Y, z1)])
    # wall: vertical lines at x_i, horizontal at y_j
    hw = hw_at((0, FLOOR_Y, WALL_Z))
    for i in range(nwv):
        x = -WF + 2 * WF * i / (nwv - 1)
        x0, x1 = x - hw, x + hw
        if i == 0: x0, x1 = x, x + 2 * hw
        if i == nwv - 1: x0, x1 = x - 2 * hw, x
        wallq.append([(x0, FLOOR_Y - WH, WALL_Z), (x1, FLOOR_Y - WH, WALL_Z), (x1, FLOOR_Y, WALL_Z), (x0, FLOOR_Y, WALL_Z)])
    for j in range(nwh):
        y = FLOOR_Y - WH * j / (nwh - 1)
        y0, y1 = y - hw, y + hw
        if j == 0: y0, y1 = y - 2 * hw, y
        if j == nwh - 1: y0, y1 = y, y + 2 * hw
        wallq.append([(-WF, y0, WALL_Z), (WF, y0, WALL_Z), (WF, y1, WALL_Z), (-WF, y1, WALL_Z)])
    T = None
    if tread:
        T = treadmill('room', WF, hw_at)
        floorq = T.pop('border')
    parts = []
    for name, qs, inward in (('wall', wallq, np.array([0, 0, -1.0])), ('floor', floorq, np.array([0, 1.0, 0]))):
        V, Fc = [], []
        for q in qs:
            Fc.append(list(range(len(V), len(V) + 4))); V += [np.array(p) for p in q]
        parts.append((name, V, orient(V, Fc, direction=inward), 3))
    return parts, WF, WH, T

# ------------------------------------------------------------------ tables
LARA_PHD = os.environ.get('LARA_PHD', '')

def lara_figure(height=640.0, anim_rel=11, fidx=0, norm=None, lab=None):
    """the posed TR1 model from the level file named by LARA_PHD (extract.py / svp_model.build), one part per mesh, faces
    wound with the outward Newell normal; 3 materials from a k-means fit of the faces' texture colours."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import svp_model
    ctr = None if norm is None else norm['ctr']
    _, Vc, _, faces, _, _ = svp_model.build(LARA_PHD, anim_rel, fidx, ctr)
    if norm is None:
        lo, hi = Vc.min(0), Vc.max(0)
        sc = height / (hi[1] - lo[1])
        off = np.array([(lo[0] + hi[0]) / 2, 0, (lo[2] + hi[2]) / 2])
        Vs = (Vc - off) * sc
        norm = dict(ctr=None, sc=sc, off=off, ysh=315 - Vs[:, 1].max())
        import svp_model as SM
        L0, V0, _, _ = SM.lara(LARA_PHD, anim_rel, fidx)
        norm['ctr'] = (V0.min(0) + V0.max(0)) / 2
    Vs = (Vc - norm['off']) * norm['sc']
    Vs[:, 1] += norm['ysh']                                             # feet on the floor line (pose 0)
    rgb = np.array([f['rgb'] for f in faces], float)
    w = np.array([np.linalg.norm(f['n']) for f in faces]) + 1
    C = rgb[np.argsort(rgb.sum(1))[[len(rgb) // 6, len(rgb) // 2, 5 * len(rgb) // 6]]].copy()
    for _ in range(0 if lab is not None else 30):
        lab = np.argmin(((rgb[:, None, :] - C[None]) ** 2).sum(2), 1)
        for k in range(3):
            if (lab == k).any(): C[k] = np.average(rgb[lab == k], 0, w[lab == k])
    if lab is None: lab = np.argmin(((rgb[:, None, :] - C[None]) ** 2).sum(2), 1)
    mats = [tuple(int(min(7, round(c * 7 / 255 * 1.25))) for c in C[k]) for k in range(3)]
    parts = {}
    for f, m in zip(faces, lab):
        parts.setdefault(f['bone'], []).append((f['idx'], int(m)))
    out = []
    for b in sorted(parts):
        used = sorted({k for idx, _ in parts[b] for k in idx})
        remap = {k: i for i, k in enumerate(used)}
        V = [Vs[k] for k in used]
        Fc = [[remap[k] for k in idx] for idx, _ in parts[b]]
        mm = [m for _, m in parts[b]]
        mvals = max(set(mm), key=mm.count)
        out.append(('mesh%d' % b, V, Fc, mvals, mm))
    return out, mats, norm, lab

def raster_gradient(T, near, pure=False):
    """CRAM entry 13 per screen line 0..209 for the 68000's H-interrupt: black above the horizon row, then a ramp
    to the floor colour at the last viewport line (207), through every 3-bit colour with R <= min(2, G / 2) and
    B <= min(5, G / 2 + 2) (greens with a little red and blue) in order of luminance, one luminance step per colour."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mesh_model as M
    xv, yt, zt = M.transform(SIN[64], SIN[0], CT, ST, 0, T['FY'], T['ZF'])
    hline = 16 + CY + M.hi(2 * yt * RECIP[(D - zt) >> RSH])
    Y = lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
    cand = [(r, g, b) for r in range(3) for g in range(8) for b in range(6) if b <= min(5, g // 2 + 2) and r <= min(2, g // 2)]
    if pure:                                                         # --grad-pure: within 1.2 levels of the floor hue
        dv = np.array([(near >> 1) & 7, (near >> 5) & 7, (near >> 9) & 7], float); dv /= np.linalg.norm(dv)
        cand = [c for c in [(r, g, b) for r in range(8) for g in range(8) for b in range(8)]
                if np.linalg.norm(np.array(c) - np.dot(c, dv) * dv) <= 1.2 and np.dot(c, dv) >= 0]
    cand.sort(key=lambda c: (Y(c), c))
    seq = []
    for c in cand:                                                   # strictly rising luminance
        if not seq or Y(c) > Y(seq[-1]) + 1e-9: seq.append(c)
    top = [(near >> 1) & 7, (near >> 5) & 7, (near >> 9) & 7]
    seq = [c for c in seq if Y(c) <= Y(top) + 1e-9]
    if tuple(top) not in seq: seq.append(tuple(top))
    grad = []
    for L in range(210):
        if L < hline: grad.append(0); continue
        t = min(1.0, (L - hline) / (207 - hline))
        grad.append(cram(*seq[int(round(t * (len(seq) - 1)))]))
    return grad, len(set(grad[hline:208]))

def fade_tables(T, FADE):
    """Depth fade of the infinite floor: len(FADE) bands of equal z from the horizon (T['ZF']) to the near edge,
    band b (0 = nearest) in palette index FADE[b].  CLT: colour word of a cross-line by its z bucket ((z - ZF) >> SH).
    FBAND: for the front-to-back lines (rows from the horizon row down), per band far to near: colour word, rows in
    the band (the last 0x7FFF); band rows from the fixed-point projection of the band boundary z (angle 0)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mesh_model as M
    ZF, NZ, SH, FY = T['ZF'], T['NZ'], T['SH'], T['FY']
    n = len(FADE)
    bz = [ZF + (((NZ - ZF) * i // n) >> SH << SH) for i in range(1, n)]      # boundaries far to near, bucket-aligned
    def row(z):
        xv, yt, zt = M.transform(SIN[64], SIN[0], CT, ST, 0, FY, z)
        return CY + M.hi(2 * yt * RECIP[(D - zt) >> RSH])
    far_to_near = FADE[::-1]
    T['CLT'] = [far_to_near[sum(1 for b in bz if ZF + (i << SH) >= b)] * 0x1111 for i in range(len(T['THK']))]
    rows = [row(ZF)] + [row(b) for b in bz]
    T['FBAND'] = []
    for i, c in enumerate(far_to_near):
        T['FBAND'] += [c * 0x1111, rows[i + 1] - rows[i] if i + 1 < len(rows) else 0x7FFF]
    T['FADE'] = FADE; T['BZ'] = bz

def build(argv):
    out = argv[0]
    noroom = '--no-room' in argv
    def opt(k, d): return int(argv[argv.index(k) + 1]) if k in argv else d
    nfl, nwv, nwh = opt('--floor', 9), opt('--wall-v', 9), opt('--wall-h', 6)
    global MATS
    POSES = []
    if '--lara-run' in argv:
        nfr = opt('--frames', 22); step = opt('--step', 2)
        lf, MATS, norm, lab = lara_figure(anim_rel=0, fidx=0)
        for j in range(0, nfr, step):
            lj = lf if j == 0 else lara_figure(anim_rel=0, fidx=j, norm=norm, lab=lab)[0]
            POSES.append([(n, V, Fc, m) for n, V, Fc, m, _ in lj])
        fig = POSES[0]
        FMAT = {n: mm for n, _, _, _, mm in lf}
    elif '--lara' in argv:
        lf, MATS, _, _ = lara_figure()
        fig = [(n, V, Fc, m) for n, V, Fc, m, _ in lf]
        FMAT = {n: mm for n, _, _, _, mm in lf}
    else:
        fig = figure(); FMAT = {}
    if not POSES: POSES = [fig]
    global TM_CELL, TM_V
    TM_CELL = opt('--cell', TM_CELL); TM_V = int(round(TM_CELL * 2 / 15))
    tread = not noroom and '--no-treadmill' not in argv
    infinite = tread and '--floor-room' not in argv                      # --floor-infinite: the default for room builds
    rparts, WF, WH, TM = room(nfl, nwv, nwh, tread and not infinite)
    static = [] if noroom else rparts
    if infinite:
        TM = treadmill('infinite'); TM.pop('border'); static = []
    parts = static + [p for pose in POSES for p in pose]
    pal = [cram(*BG)]
    for m in MATS:
        for i in range(NSH):
            f = AMB + (1 - AMB) * (i + 1) / NSH
            pal.append(cram(*[min(7, round(c * f)) for c in m]))
    pal.append(cram(*GREEN))                                         # index 13: floor, nearest band
    free = 16 - len(pal)                                             # entries after the figure's and the floor's
    raster = infinite and '--no-raster-fade' not in argv                # 68000 per-line CRAM gradient of entry 13
    nfade = min(opt('--fade', 1 if raster else 3), free + 1) if infinite else 1
    FADE = [13]                                                      # band palette indices, near to far
    for b in range(1, nfade):
        f = b / (nfade - 1)
        pal.append(cram(*[int(round(g + (r - g) * f)) for g, r in zip(GREEN, FADE_FAR)])); FADE.append(len(pal) - 1)
    if nfade == 1: pal.append(cram(*HOR))                            # index 14: horizon
    while len(pal) < 16: pal.append(cram(0, 2, 1))
    if infinite: fade_tables(TM, FADE)
    LUT = []
    for m in range(3):
        for lev in range(8): LUT.append((1 + m * NSH + (lev * NSH) // 8) * 0x1111)
    LUT += [13 * 0x1111] * 8
    VERTS, FACES, PREC, J = [], [], [], {'parts': []}
    nfaces = nverts = 0
    for pi, (name, V, Fc, m) in enumerate(parts):
        Vi = [[int(round(c)) for c in v] for v in V]
        assert len(Vi) <= 80, (name, len(Vi))
        cen = [int(round(c)) for c in np.mean(np.array(Vi, float), 0)]
        frec = []
        for fi, f in enumerate(Fc):
            idx = f + [f[2]] if len(f) == 3 else f
            n = newell(np.array([Vi[k] for k in f], float))             # outward Newell normal
            frec.append((idx, [q15(c) for c in n], FMAT[name][fi] if name in FMAT else m))
        PREC.append((len(VERTS), len(Vi), len(FACES), len(frec), cen, pi < len(static)))
        rec = {'name': name, 'V': Vi, 'F': [[i, n, mm] for i, n, mm in frec], 'C': cen, 'static': pi < len(static)}
        if pi < len(static) + len(POSES[0]): J['parts'].append(rec)
        if pi >= len(static):
            q = (pi - len(static)) // len(POSES[0])
            J.setdefault('poses', [[] for _ in POSES])[q].append(rec)
        VERTS += [c for v in Vi for c in v]
        for idx, n, mm in frec: FACES += [2 * i for i in idx] + n + [mm * 8]
        nfaces += len(frec); nverts += len(Vi)
    os.makedirs(out, exist_ok=True)
    w = lambda v: '0x%04X' % (v & 0xFFFF)
    with open(os.path.join(out, 'tables.inc'), 'w') as f:
        f.write('; generated by mkmesh.py -- do not edit\n')
        f.write('NPART equ %d\nNSTATIC equ %d\nNDYN equ %d\nCAM_D equ %d\nSCR_CY equ %d\nTILT_C equ %s\nTILT_S equ %s\n'
                % (len(parts), len(static), len(POSES[0]), D, CY, w(CT), w(ST)))
        f.write('NPOSE equ %d\nPOSEBIG equ %d\n' % (len(POSES), len(POSES) * 64))
        f.write('LIGHT_X equ %s\nLIGHT_Y equ %s\nLIGHT_Z equ %s\n' % tuple(w(c) for c in LQ))
        def table(name, vals, per=8):
            f.write('%s:\n' % name)
            for i in range(0, len(vals), per): f.write('\tdw ' + ', '.join(w(v) for v in vals[i:i + per]) + '\n')
        table('SIN', SIN); table('RECIP', RECIP); table('LUT', LUT)
        if TM and 'CLT' not in TM:
            TM['CLT'] = [LUT[3 * 8]] * len(TM['THK']); TM['FBAND'] = [LUT[3 * 8], 0x7FFF]
        if TM:
            f.write('TREAD equ 1\nTM_HF equ %d\nTM_FY equ %d\nTM_ZF equ %s\nTM_CELL equ %d\nTM_HOR equ %d\n'
                    % (TM['HF'], TM['FY'], w(TM['ZF']), TM['CELL'], TM['HOR']))
            table('TMTAB', TM['TMTAB'], 8); table('HWT', TM['HWT']); table('THK', TM['THK'])
            table('XWT', TM['XWT']); table('ZCT', TM['ZCT']); table('CLT', TM['CLT']); table('FBAND', TM['FBAND'], 2)
        else:
            f.write('TREAD equ 0\nTM_HF equ 0\nTM_FY equ 0\nTM_ZF equ 0\nTM_CELL equ 0\nTM_HOR equ 0\nTMTAB:\nHWT:\nTHK:\nXWT:\nZCT:\nCLT:\nFBAND:\n')
        f.write('HORW equ 0x%04X\n' % (14 * 0x1111))
        f.write('FLOORW equ 0x%04X\n' % LUT[3 * 8])
        f.write('POSEBASE:\n')
        for q in range(len(POSES)):
            f.write('\tdw PARTS + %d\n' % (8 * len(static) + 8 * len(POSES[0]) * q))
        # part record: VERTS address, nv - 1, FACES address, nf - 1, cx, cy, cz, 0
        f.write('PARTS:\n')
        for va, nv, fa, nf, c, st in PREC:
            f.write('\tdw VERTS + %d, %d, FACES + %d, %d, %s, %s, %s, 0\n' % (va, nv - 1, fa, nf - 1, *[w(x) for x in c]))
        table('VERTS', VERTS, 3)
        table('FACES', FACES, 8)
    grad, ncol = raster_gradient(TM, pal[13], '--grad-pure' in argv) if raster else ([pal[13]] * 210, 1)
    with open(os.path.join(out, 'palette.h'), 'w') as f:
        f.write('/* generated by mkmesh.py -- palette 0, CRAM words; floor_grad: CRAM entry 13 per screen line (main.c) */\n')
        f.write('static const unsigned short palette0[16] = {\n    ' + ', '.join('0x%04X' % p for p in pal) + '\n};\n')
        f.write('const unsigned short floor_grad[210] = {\n')
        for i in range(0, 210, 12): f.write('    ' + ', '.join('0x%04X' % g for g in grad[i:i + 12]) + ',\n')
        f.write('};\n')
    J['GRAD'] = grad
    if raster: print('raster gradient: %d distinct colours' % ncol)
    if TM: J['tread'] = TM
    J.update({'SIN': SIN, 'RECIP': RECIP, 'RSH': RSH, 'LQ': LQ, 'CT': CT, 'ST': ST, 'D': D, 'CY': CY, 'LUT': LUT, 'PAL': pal})
    with open(os.path.join(out, 'mesh.json'), 'w') as f: json.dump(J, f)
    print('%s: parts %d (static %d), vertices %d, faces %d (figure %d), floor half width %d, wall height %d, data words %d'
          % (out, len(parts), len(static), nverts, nfaces, sum(len(p[2]) for p in fig), WF, WH,
             len(VERTS) + len(FACES) + 8 * len(PREC)))

if __name__ == '__main__':
    build(sys.argv[1:])
