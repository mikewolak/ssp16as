#!/usr/bin/env python3
"""mesh_model.py -- bit-exact model of the SSP mesh renderer (mesh.s), arithmetic of the single-object
renderer extended to parts:

  per frame k: a = 4k & 255; static parts use angle index 0 (c = SIN[64], s = SIN[0]), in table order;
  dynamic parts use angle a, ordered by zt of their transformed centre ascending (far first), ties by part index;
  R = RECIP[(D - zt) >> RSH]; sx = 128 - hi(2 xv R), sy = CY + hi(2 yt R);
  per face: cull when (x2 - x0)(y3 - y1) - (x3 - x1)(y2 - y0) >= 0; level = max(0, d >> 12);
  colour = LUT[mat8 + level] & 15; triangles (0,1,2) and, unless vertex 3 projects onto vertex 2, (0,2,3).
Frame buffer: 256 x 192, 4 bpp indices, row-major.

  mesh_model.py <mesh.json> <out.bin> [frames]    writes the frames' packed buffers (128 bytes a row) and counts
"""
import json, sys
import numpy as np

W, H = 256, 192

def hi(acc):
    acc &= 0xFFFFFFFF
    v = acc >> 16
    return v - 0x10000 if v & 0x8000 else v

def transform(c, s, ct, st, x, y, z):
    xv = hi(2 * x * c + 2 * z * s)
    z2 = hi(-2 * x * s + 2 * z * c)
    yt = hi(2 * y * ct - 2 * z2 * st)
    zt = hi(2 * y * st + 2 * z2 * ct)
    return xv, yt, zt

def tri_fill(fb, xs, ys, col):
    (x0, x1, x2), (y0, y1, y2) = xs, ys
    area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
    if area == 0: return 0
    assert 0 <= min(xs) and max(xs) <= 255, xs          # the cart's edge x are 8-bit pointer registers
    if area < 0: x1, x2, y1, y2 = x2, x1, y2, y1
    minx, maxx = min(x0, x1, x2), max(x0, x1, x2)
    miny, maxy = max(min(y0, y1, y2), 0), min(max(y0, y1, y2), H - 1)
    if miny > maxy: return 1
    px, py = np.meshgrid(np.arange(minx, maxx + 1), np.arange(miny, maxy + 1))
    cx, cy = 2 * px + 1, 2 * py + 1
    inside = np.ones(px.shape, bool)
    for (ax, ay), (bx, by) in (((x0, y0), (x1, y1)), ((x1, y1), (x2, y2)), ((x2, y2), (x0, y0))):
        dx, dy = bx - ax, by - ay
        e = dx * (cy - 2 * ay) - dy * (cx - 2 * ax)
        tl = dy < 0 or (dy == 0 and dx > 0)
        inside &= (e > 0) | ((e == 0) if tl else False)
    fb[miny:maxy + 1, minx:maxx + 1][inside] = col
    return 1

def frame(d, k, stats=None):
    a = (4 * k) & 255
    ct, st, R, L, RSH = d['CT'], d['ST'], d['RECIP'], d['LQ'], d['RSH']
    fb = np.zeros((H, W), np.uint8)
    nfront = ntri = 0
    def draw(p, c, s):
        nonlocal nfront, ntri
        sx, sy = [], []
        for x, y, z in p['V']:
            xv, yt, zt = transform(c, s, ct, st, x, y, z)
            r = R[(d['D'] - zt) >> RSH]
            sx.append(128 - hi(2 * xv * r)); sy.append(d['CY'] + hi(2 * yt * r))
        for vi, (nx, ny, nz), m in p['F']:
            xs = [sx[i] for i in vi]; ys = [sy[i] for i in vi]
            a2 = (xs[2] - xs[0]) * (ys[3] - ys[1]) - (xs[3] - xs[1]) * (ys[2] - ys[0])
            if a2 >= 0: continue
            nfront += 1
            nxv, nyt, nzt = transform(c, s, ct, st, nx, ny, nz)
            dd = hi(2 * nxv * L[0] + 2 * nyt * L[1] + 2 * nzt * L[2])
            col = d['LUT'][m * 8 + max(0, dd >> 12)] & 15
            ntri += tri_fill(fb, (xs[0], xs[1], xs[2]), (ys[0], ys[1], ys[2]), col)
            if (xs[3], ys[3]) != (xs[2], ys[2]):
                ntri += tri_fill(fb, (xs[0], xs[2], xs[3]), (ys[0], ys[2], ys[3]), col)
    S = d['SIN']
    for p in d['parts']:
        if p['static']: draw(p, S[64], S[0])
    if 'tread' in d: ntri += treadmill(d, fb, k, S[64], S[0])
    s, c = S[a], S[(a + 64) & 255]
    dyn = d['poses'][(k & 0x7FFF) % len(d['poses'])] if 'poses' in d else [p for p in d['parts'] if not p['static']]
    keys = [transform(c, s, ct, st, *p['C'])[2] for p in dyn]
    for i in sorted(range(len(dyn)), key=lambda i: (keys[i], i)):
        draw(dyn[i], c, s)
    if stats is not None: stats.update(front=nfront, tris=ntri)
    return fb

def edge_line(fb, ux, uy, lx, ly, col):
    """col: a palette index, or a list of (index, rows) runs from the top row (mesh.s FBAND)"""
    if not isinstance(col, list): col = [(col, 0x7FFF)]
    runs = []
    for c, r in col: runs += [c] * min(r, max(0, ly - uy - len(runs)))
    """one pixel a row at the left-edge position of the edge U-L, rows uy .. ly - 1 (mesh.s xline: setl and the
    left-edge step of seg)"""
    h, w = ly - uy, lx - ux
    sl, rl, e, x = 2 * h, -2 * w, h + w, ux
    for y in range(uy, ly):
        a = e + rl
        if a < 0:
            while True:
                a += sl; x += 1
                if a >= 0: break
        else:
            while a - sl >= 0:
                a -= sl; x -= 1
        e = a
        assert 0 <= y < H and 0 <= x < W, (x, y)
        fb[y, x] = runs[y - uy]
    return 1

def treadmill(d, fb, k, c, s):
    """floor of frame k (mesh.s floor): horizon (dim, when HOR), front-to-back lines (one pixel a row), far cross-lines (step 2 cells),
    near cross-lines (step 1 cell); quads (0,1,2),
    (0,2,3), no cull, no light"""
    T = d['tread']; ct, st, R, RSH = d['CT'], d['ST'], d['RECIP'], d['RSH']
    x0, nx, zf0, nzf, zn0, nzn = T['TMTAB'][8 * (k & 63):8 * (k & 63) + 6]
    FY, ZF, HF, SH, CELL = T['FY'], T['ZF'], T['HF'], T['SH'], T['CELL']
    def proj(x, y, z):
        xv, yt, zt = transform(c, s, ct, st, x, y, z)
        r = R[(d['D'] - zt) >> RSH]
        return 128 - hi(2 * xv * r), d['CY'] + hi(2 * yt * r)
    n = 0
    def quad(q, col):
        nonlocal n
        p = [proj(*v) for v in q]
        n += tri_fill(fb, (p[0][0], p[1][0], p[2][0]), (p[0][1], p[1][1], p[2][1]), col)
        n += tri_fill(fb, (p[0][0], p[2][0], p[3][0]), (p[0][1], p[2][1], p[3][1]), col)
    def zline(z, col):
        i = (z - ZF) >> SH; t = T['THK'][i]; w = T['XWT'][i]
        if 'CLT' in T: col = T['CLT'][i] & 15
        quad([(-w, FY, z - t), (w, FY, z - t), (w, FY, z + t), (-w, FY, z + t)], col)
    if T['HOR']: zline(ZF, 14)
    col = d['LUT'][24] & 15
    for i in range(nx):
        x = x0 + i * CELL
        zc = T['ZCT'][abs(x) >> SH]
        (ux, uy), (lx, ly) = proj(x, FY, ZF), proj(x, FY, zc)
        fband = [(T['FBAND'][2 * b] & 15, T['FBAND'][2 * b + 1]) for b in range(len(T['FBAND']) // 2)] if 'FBAND' in T else col
        n += edge_line(fb, ux, uy, lx, ly, fband)
    for j in range(nzf):
        zline(zf0 + 2 * j * CELL, col)
    for j in range(nzn):
        zline(zn0 + j * CELL, col)
    return n

def pack(fb):
    return bytes(((fb[:, 0::2] << 4) | fb[:, 1::2]).astype(np.uint8).ravel())

if __name__ == '__main__':
    d = json.load(open(sys.argv[1]))
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    out = bytearray()
    for k in range(n):
        st = {}
        out += pack(frame(d, k, st))
        print('frame %2d front faces %3d triangles %3d' % (k, st['front'], st['tris']))
    open(sys.argv[2], 'wb').write(bytes(out))
