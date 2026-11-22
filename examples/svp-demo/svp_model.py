#!/usr/bin/env python3
"""SVP (SSP1601) bit-exact model of the Lara turntable, cart data export,
previews.

Fixed point (matches the SSP1601 multiplier, P = X * Y * 2, 32-bit
accumulator A, result taken from A's high word):
  vertices, face centres   int16, Q0, TR1 units (1/1024 of a sector), model centred
  sin/cos, normals, light  int16, Q15 (0x7FFF = +1.0)
  rotate:  xv = hi(2*x*c + 2*z*s), zv = hi(-2*x*s + 2*z*c)   (Q0 * Q15 * 2 -> Q16, hi -> Q0)
  project: R = recip[(D - zv) >> 3] (Q15 scale F/d), sx = 128 - hi(2*xv*R), sy = 112 + hi(2*y*R)
  light:   d = hi(2*nxv*Lx + 2*ny*Ly + 2*nzv*Lz) (Q15), level = clamp(d >> 12, 0, 7)
  colour:  pal = lut[material*8 + level]
  cull:    shoelace area of the projected polygon (integer), front when area < 0
  sort:    ascending zv of the rotated face centre, ties by face index (painter's)
  raster:  pixel centres, top-left fill rule, quads as fans (0,1,2)(0,2,3)
  frame:   256 x 224, 4 bpp, Genesis tile order (32 x 28 cells, row-major,
           32 bytes per cell, high nibble = left pixel)
"""
import sys, os, struct, math
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import lara

OUT = os.path.dirname(os.path.abspath(__file__))
W, H, NF, FPS = 256, 224, 64, 15
D = 2000                       # camera distance, TR units
F = 500.0                      # focal length, pixels
LIGHT = np.array([-0.45, -0.55, 0.70]); LIGHT /= np.linalg.norm(LIGHT)   # view space, from camera side, upper left
AMB = 0.30
BG = (0, 0, 2); HUD = (7, 7, 7)                                           # Genesis 3-bit RGB

def hi(acc):                   # 32-bit accumulator wrap, high word, arithmetic
    acc = ((acc + 2**31) % 2**32) - 2**31
    return acc >> 16

def q15(x): return int(max(-32767, min(32767, round(x * 32767))))

SIN = [q15(math.sin(2 * math.pi * i / 256)) for i in range(256)]
RECIP = [min(32767, round(F * 32768 / ((i << 3) + 4))) if i else 32767 for i in range(512)]
LQ = [q15(c) for c in LIGHT]

def g3(rgb3): return tuple(int(round(c * 255 / 7)) for c in rgb3)
def cram(rgb3): r, g, b = rgb3; return (b << 9) | (g << 5) | (r << 1)

# ---------------------------------------------------------------- model
def build(phd, anim_rel=11, frame=0, ctr=None):
    L, V, FACES, info = lara(phd, anim_rel, frame)
    lo, hi_ = V.min(0), V.max(0)
    if ctr is None: ctr = (lo + hi_) / 2
    Vc = V - ctr
    Vi = np.round(Vc).astype(np.int64)
    # dedupe posed vertices
    uniq, inv = np.unique(Vi, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    pal = L['pal']; tiles = L['tiles']; otex = L['otex']
    faces = []
    vol = 0.0
    for idx, tex, t, bone in FACES:
        p = Vc[idx]
        n = np.zeros(3)
        for k in range(len(p)):
            a, b = p[k], p[(k + 1) % len(p)]
            n += np.cross(a, b)
        vol += sum(np.dot(p[0], np.cross(p[k], p[k + 1])) for k in range(1, len(p) - 1))
        faces.append(dict(idx=idx, tex=tex, t=t, bone=bone, n=n))
    flip = vol < 0             # make sum(cross) point outward
    for f in faces:
        if flip:
            f['idx'] = f['idx'][::-1]; f['n'] = -f['n']
        nn = np.linalg.norm(f['n']); f['n'] = f['n'] / nn if nn else np.array([0, 0, 1.0])
        f['vi'] = [int(inv[k]) for k in f['idx']]
        f['c'] = np.round(Vc[f['idx']].mean(0)).astype(int)
        if f['tex']:
            attr, tile, uv = otex[f['t'] & 0x7FFF]
            uvs = uv[:len(f['idx'])]
            if flip: uvs = uvs[::-1]
            f['uv'] = uvs; f['tile'] = tile
            m = Image.new('L', (256, 256)); ImageDraw.Draw(m).polygon(uvs, fill=1, outline=1)
            m = np.array(m, bool); px = tiles[tile][m]; px = px[px != 0]
            f['rgb'] = pal[px].mean(0) if len(px) else np.array([128, 128, 128.])
        else:
            f['rgb'] = pal[f['t'] & 0xFF].astype(float)
    return L, Vc, uniq, faces, info, flip

def palette_and_lut(faces):
    mats = sorted({tuple(np.round(f['rgb']).astype(int)) for f in faces})
    mid = {m: i for i, m in enumerate(mats)}
    for f in faces: f['mat'] = mid[tuple(np.round(f['rgb']).astype(int))]
    w = np.bincount([f['mat'] for f in faces], minlength=len(mats)).astype(float)
    samples, sw = [], []
    for i, m in enumerate(mats):
        for l in range(8):
            samples.append(np.array(m) * (AMB + (1 - AMB) * l / 7)); sw.append(w[i])
    S = np.array(samples); SW = np.array(sw)
    # weighted k-means to 14 colours, deterministic init (farthest point)
    for K in range(14, 40):
        P3 = kmeans_pal(S, SW, K)
        if len(P3) >= 14: break
    P3 = [BG] + P3[:14] + [HUD]
    return finish(mats, P3)

def kmeans_pal(S, SW, K):
    C = [S[np.argmax(SW)]]
    while len(C) < K:
        d = np.min([((S - c) ** 2).sum(1) for c in C], 0) * SW
        C.append(S[np.argmax(d)])
    C = np.array(C)
    for _ in range(50):
        a = np.argmin(((S[:, None] - C[None]) ** 2).sum(2), 1)
        for k in range(len(C)):
            if (a == k).any(): C[k] = (S[a == k] * SW[a == k, None]).sum(0) / SW[a == k].sum()
    cnt = {}
    a = np.argmin(((S[:, None] - C[None]) ** 2).sum(2), 1)
    for k, c in enumerate(C):
        key = tuple(int(v) for v in np.clip(np.round(c * 7 / 255), 0, 7))
        if key in (BG, HUD): continue
        cnt[key] = cnt.get(key, 0) + SW[a == k].sum()
    return sorted(sorted(cnt, key=lambda q: -cnt[q])[:14])

def finish(mats, P3):
    prgb = np.array([g3(c) for c in P3], float)
    lut = np.zeros((len(mats), 8), int)
    for i, m in enumerate(mats):
        for l in range(8):
            c = np.array(m) * (AMB + (1 - AMB) * l / 7)
            lut[i, l] = 1 + int(np.argmin(((prgb[1:15] - c) ** 2).sum(1)))
    return mats, P3, prgb, lut

# ---------------------------------------------------------------- raster
def tri_fill(fb, xs, ys, col, zb=None, zfun=None):
    (x0, x1, x2), (y0, y1, y2) = xs, ys
    area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
    if area == 0: return
    if area < 0: x1, x2, y1, y2 = x2, x1, y2, y1
    minx, maxx = max(min(x0, x1, x2), 0), min(max(x0, x1, x2), W - 1)
    miny, maxy = max(min(y0, y1, y2), 0), min(max(y0, y1, y2), H - 1)
    if minx > maxx or miny > maxy: return
    px, py = np.meshgrid(np.arange(minx, maxx + 1), np.arange(miny, maxy + 1))
    cx, cy = 2 * px + 1, 2 * py + 1
    inside = np.ones(px.shape, bool)
    for (ax, ay), (bx, by) in (((x0, y0), (x1, y1)), ((x1, y1), (x2, y2)), ((x2, y2), (x0, y0))):
        dx, dy = bx - ax, by - ay
        e = dx * (cy - 2 * ay) - dy * (cx - 2 * ax)
        tl = dy < 0 or (dy == 0 and dx > 0)
        inside &= (e > 0) | ((e == 0) if tl else False)
    if zb is None:
        fb[miny:maxy + 1, minx:maxx + 1][inside] = col
    else:
        col(fb, zb, px[inside], py[inside])

# ---------------------------------------------------------------- font (3x5)
GLY = {'0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111',
       '4': '101101111001001', '5': '111100111001111', '6': '111100111101111', '7': '111001001010010',
       '8': '111101111101111', '9': '111101111001111', 'P': '111101111100100', 'O': '111101101101111',
       'L': '100100100100111', 'Y': '101101010010010', 'S': '111100111001111', ' ': '0' * 15}
def text(fb, s, x, y, col, sc=2):
    for ch in s:
        g = GLY[ch]
        for i, b in enumerate(g):
            if b == '1':
                fb[y + (i // 3) * sc:y + (i // 3) * sc + sc, x + (i % 3) * sc:x + (i % 3) * sc + sc] = col
        x += 4 * sc

def tiles4(fb):
    out = bytearray()
    for ty in range(H // 8):
        for tx in range(W // 8):
            c = fb[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8]
            out += bytes(((c[:, 0::2] << 4) | c[:, 1::2]).astype(np.uint8).ravel())
    return bytes(out)

def untile(buf):
    fb = np.zeros((H, W), np.uint8); a = np.frombuffer(buf, np.uint8)
    for t in range(32 * 28):
        cell = a[t * 32:(t + 1) * 32].reshape(8, 4)
        ty, tx = divmod(t, 32)
        fb[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8:2] = cell >> 4
        fb[ty * 8:ty * 8 + 8, tx * 8 + 1:tx * 8 + 8:2] = cell & 15
    return fb

# ---------------------------------------------------------------- SVP frame
def svp_frame(k, verts, faces, lut, front_sign):
    a = (k * 256 // NF) & 255
    s, c = SIN[a], SIN[(a + 64) & 255]
    sx, sy, zv = [], [], []
    for x, y, z in verts:
        xv = hi(2 * x * c + 2 * z * s); z2 = hi(-2 * x * s + 2 * z * c)
        R = RECIP[(D - z2) >> 3]
        sx.append(128 - hi(2 * xv * R)); sy.append(112 + hi(2 * int(y) * R)); zv.append(z2)
    vis = []
    for i, f in enumerate(faces):
        vi = f['vi']; xs = [sx[j] for j in vi]; ys = [sy[j] for j in vi]
        ar = sum(xs[j] * ys[(j + 1) % len(vi)] - xs[(j + 1) % len(vi)] * ys[j] for j in range(len(vi)))
        if ar * front_sign <= 0: continue
        nx, ny, nz = f['nq']; cx, cy, cz = f['cq']
        nxv = hi(2 * nx * c + 2 * nz * s); nzv = hi(-2 * nx * s + 2 * nz * c)
        d = hi(2 * nxv * LQ[0] + 2 * ny * LQ[1] + 2 * nzv * LQ[2])
        lev = max(0, min(7, d >> 12))
        key = hi(-2 * cx * s + 2 * cz * c)
        vis.append((key, i, xs, ys, lut[f['mat'], lev]))
    vis.sort(key=lambda v: (v[0], v[1]))
    fb = np.zeros((H, W), np.uint8)
    for key, i, xs, ys, col in vis:
        for t in range(1, len(xs) - 1):
            tri_fill(fb, (xs[0], xs[t], xs[t + 1]), (ys[0], ys[t], ys[t + 1]), col)
    text(fb, 'POLYS %d' % len(vis), 6, 6, 15)
    return fb, len(vis)

# ---------------------------------------------------------------- reference (float, textured, z-buffer)
def ref_frame(k, Vc, faces, L):
    ang = 2 * math.pi * k / NF
    s, c = math.sin(ang), math.cos(ang)
    Ry = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    P = Vc @ Ry.T
    d = D - P[:, 2]
    sx = 128 - F * P[:, 0] / d; sy = 112 + F * P[:, 1] / d
    img = np.zeros((H, W, 3)); img[:] = g3(BG); zb = np.full((H, W), 1e9)
    tiles, pal = L['tiles'], L['pal']
    for f in faces:
        n = Ry @ f['n']; pc = P[f['idx']].mean(0)
        if np.dot(n, np.array([0, 0, D]) - pc) <= 0: continue
        sh = AMB + (1 - AMB) * max(0.0, float(np.dot(n, LIGHT)))
        idx = f['idx']
        for t in range(1, len(idx) - 1):
            tri = [0, t, t + 1]
            X = np.array([sx[idx[j]] for j in tri]); Y = np.array([sy[idx[j]] for j in tri]); Z = np.array([d[idx[j]] for j in tri])
            area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0])
            if abs(area) < 1e-9: continue
            x0, x1 = max(int(X.min()), 0), min(int(X.max()) + 1, W - 1)
            y0, y1 = max(int(Y.min()), 0), min(int(Y.max()) + 1, H - 1)
            if x0 > x1 or y0 > y1: continue
            px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((X[1] - px) * (Y[2] - py) - (Y[1] - py) * (X[2] - px)) / area
            w1 = ((X[2] - px) * (Y[0] - py) - (Y[2] - py) * (X[0] - px)) / area
            w2 = 1 - w0 - w1
            m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not m.any(): continue
            iz = w0 / Z[0] + w1 / Z[1] + w2 / Z[2]
            z = 1 / iz
            ix = (px[m] - 0.5).astype(int); iy = (py[m] - 0.5).astype(int)
            zz = z[m]; ok = zz < zb[iy, ix]
            if not ok.any(): continue
            ix, iy, zz = ix[ok], iy[ok], zz[ok]
            if f['tex']:
                uv = np.array([f['uv'][j] for j in tri], float)
                ww = np.stack([w0[m][ok] / Z[0], w1[m][ok] / Z[1], w2[m][ok] / Z[2]], 1) * zz[:, None]
                u = np.clip((ww @ uv[:, 0]).astype(int), 0, 255); v = np.clip((ww @ uv[:, 1]).astype(int), 0, 255)
                ti = tiles[f['tile']][v, u]
                col = pal[ti].astype(float)
                col[ti == 0] = f['rgb']
            else:
                col = np.tile(f['rgb'], (len(ix), 1))
            img[iy, ix] = np.clip(col * sh, 0, 255); zb[iy, ix] = zz
    return img.astype(np.uint8)

# ---------------------------------------------------------------- export
def export(verts, faces, P3, lut, front_sign):
    def words(a): return struct.pack('>%dh' % len(a), *a)
    V = [int(v) for p in verts for v in p]
    Fw = []
    for f in faces:
        vi = f['vi'] + [-1] * (4 - len(f['vi']))
        Fw += vi + list(f['nq']) + list(f['cq']) + [f['mat'], int(lut[f['mat'], 7])]
    CR = [cram(c) for c in P3]
    LUT = [int(x) for x in lut.ravel()]
    secs = [('verts', V), ('faces', Fw), ('cram', CR), ('lut', LUT), ('sin', SIN), ('recip', RECIP), ('light', LQ)]
    hdr = [len(verts), len(faces), len(lut), NF, D, front_sign, 0, 0]
    body = b''; offs = []
    for n, a in secs:
        offs.append(16 + len(secs) + len(body) // 2)   # word offsets from file start
        body += words(a)
    hdrw = hdr + [0] * (16 - len(hdr))
    blob = words(hdrw) + words(offs) + body
    open(os.path.join(OUT, 'lara_svp_data.bin'), 'wb').write(blob)
    with open(os.path.join(OUT, 'lara_svp_data.h'), 'w') as h, open(os.path.join(OUT, 'lara_svp_data.s'), 'w') as s:
        h.write('/* lara_svp_data.h: posed TR1 Lara mesh for the SSP1601 renderer. Data derived from the\n'
                '   the user\'s TR1 PC game files; local only, not for distribution. Layout: see svp_model.py. */\n'
                '#include <stdint.h>\n#define LARA_NVERTS %d\n#define LARA_NFACES %d\n#define LARA_NMATS %d\n'
                '#define LARA_NFRAMES %d\n#define LARA_CAM_D %d\n#define LARA_FRONT_SIGN %d\n'
                '/* face: v0 v1 v2 v3(-1 = triangle) nx ny nz (Q15) cx cy cz (Q0) material palidx_full */\n'
                % (len(verts), len(faces), len(lut), NF, D, front_sign))
        s.write('; lara_svp_data.s: posed TR1 Lara mesh for the SSP1601 renderer (big-endian words).\n'
                '; Data derived from the the user\'s TR1 PC game files; local only, not for distribution.\n')
        for n, a in secs:
            h.write('static const int16_t lara_%s[%d] = {\n' % (n, len(a)))
            for i in range(0, len(a), 12):
                h.write('  ' + ', '.join(str(x) for x in a[i:i + 12]) + ',\n')
            h.write('};\n')
            s.write('lara_%s:\n' % n)
            for i in range(0, len(a), 12):
                s.write('\tdc.w\t' + ','.join('$%04X' % (x & 0xFFFF) for x in a[i:i + 12]) + '\n')
    return {n: 2 * len(a) for n, a in secs}, len(blob)

def main(phd):
    L, Vc, verts, faces, info, flip = build(phd)
    mats, P3, prgb, lut = palette_and_lut(faces)
    for f in faces:
        f['nq'] = [q15(x) for x in f['n']]; f['cq'] = [int(x) for x in f['c']]
    # front sign: compare float normal test with integer screen area at frame 0
    svp_tmp = []
    a, s, c = 0, SIN[0], SIN[64]
    votes = 0
    for f in faces:
        pc = Vc[f['idx']].mean(0)
        facing = np.dot(f['n'], np.array([0, 0, D]) - pc) > 0
        xs = []; ys = []
        for j in f['vi']:
            x, y, z = verts[j]; R = RECIP[(D - z) >> 3]
            xs.append(128 - hi(2 * int(x) * R)); ys.append(112 + hi(2 * int(y) * R))
        ar = sum(xs[j] * ys[(j + 1) % len(xs)] - xs[(j + 1) % len(xs)] * ys[j] for j in range(len(xs)))
        if ar: votes += (1 if (ar > 0) == facing else -1)
    front_sign = 1 if votes > 0 else -1
    print('faces', len(faces), 'verts(unique)', len(verts), 'materials', len(mats), 'flip', flip,
          'front_sign', front_sign, 'votes', votes, info)
    svp, ref, counts, raw = [], [], [], bytearray()
    for k in range(NF):
        fb, n = svp_frame(k, verts, faces, lut, front_sign)
        tb = tiles4(fb); assert (untile(tb) == fb).all(); raw += tb
        counts.append(n)
        svp.append(Image.fromarray(prgb.astype(np.uint8)[fb]))
        ref.append(Image.fromarray(ref_frame(k, Vc, faces, L)))
    open(os.path.join(OUT, 'lara_svp_frames.bin'), 'wb').write(bytes(raw))
    sizes, total = export(verts, faces, P3, lut, front_sign)
    ms = int(round(1000 / FPS))
    def gif(frames, name, scale=1):
        fr = [im.resize((im.width * scale, im.height * scale), Image.NEAREST) for im in frames]
        fr[0].save(os.path.join(OUT, name), save_all=True, append_images=fr[1:], duration=ms, loop=0)
    gif(svp, 'lara_svp.gif'); gif(svp, 'lara_svp_3x.gif', 3)
    gif(ref, 'lara_ref.gif'); gif(ref, 'lara_ref_3x.gif', 3)
    comp = []
    for a_, b_ in zip(svp, ref):
        im = Image.new('RGB', (W * 2 + 8, H), (40, 40, 40)); im.paste(a_, (0, 0)); im.paste(b_, (W + 8, 0)); comp.append(im)
    gif(comp, 'lara_compare.gif'); gif(comp, 'lara_compare_3x.gif', 3)
    sheet = Image.new('RGB', (W * 4, H * 4), (40, 40, 40))
    for i in range(8):
        k = i * NF // 8
        sheet.paste(svp[k], ((i % 4) * W, (i // 4) * 2 * H)); sheet.paste(ref[k], ((i % 4) * W, (i // 4) * 2 * H + H))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(os.path.join(OUT, 'lara_contact.png'))
    nq = sum(len(f['vi']) == 4 for f in faces)
    print('quads', nq, 'tris', len(faces) - nq, 'tri-equiv', len(faces) + nq)
    print('front-facing per frame: min %d max %d avg %.1f' % (min(counts), max(counts), sum(counts) / len(counts)))
    print('palette CRAM', ['%03X' % cram(c) for c in P3])
    print('sizes', sizes, 'bin total', total, 'frames.bin', len(raw))

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LARA_PHD', 'LEVEL1.PHD'))
