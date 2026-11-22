#!/usr/bin/env python3
"""verify.py -- runs gen/ssp16sim for frames 1..64 of a cartridge and compares the rendered DRAM buffer with
mesh_model.py; prints instructions per frame.  With --gpgx <prefix>, compares lrshot captures (<prefix>_NNNN.dram,
both buffers) with the model frames instead.

  verify.py <cart.bin> <mesh.json> [--gpgx prefix]
"""
import glob, json, os, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesh_model as M

def unpack(dram, b):
    w = np.frombuffer(dram, '>u2').reshape(-1)
    rows = w[:M.H * 128].reshape(M.H, 128)[:, b * 64:b * 64 + 64]
    px = np.stack([(rows >> 12) & 15, (rows >> 8) & 15, (rows >> 4) & 15, rows & 15], 2).reshape(M.H, M.W)
    return px.astype(np.uint8)

rom, js = sys.argv[1], sys.argv[2]
d = json.load(open(js))
NM = 64 * len(d["poses"]) if "poses" in d else 64
model = {k: M.frame(d, k) for k in range(65)}
bad = 0
def screen(path):
    """the viewport of a captured screen as 3-bit RGB (rows vy0.. of the screen)"""
    raw = open(path, 'rb').read()
    hdr = raw.split(b'\n', 3); wd, ht = map(int, hdr[1].split())
    img = np.frombuffer(hdr[3], np.uint8)[:wd * ht * 3].reshape(ht, wd, 3) >> 5
    vy0 = (ht - 224) // 2 + 16
    return img[vy0:vy0 + M.H, :M.W].astype(np.uint8), vy0

def expect_rgb(fb, vy0):
    """model frame as 3-bit RGB: palette 0, entry 13 per screen line from d['GRAD'] (the 68000's H-interrupt)"""
    out = np.zeros((M.H, M.W, 3), np.uint8)
    for r in range(M.H):
        pal = list(d['PAL'])
        if 'GRAD' in d: pal[13] = d['GRAD'][vy0 + r]
        P = np.array([((c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7) for c in pal], np.uint8)
        out[r] = P[fb[r]]
    return out

FPS_BOX = (188, 4, 252, 12)        # main.c FPS_X, FPS_Y: 8 sprites of 8 x 8, screen coordinates (x0, y0, x1, y1)

def masked_equal(m, scr, vy0):
    """screen viewport equal to model frame m (as colours) outside the FPS box"""
    x0, y0, x1, y1 = FPS_BOX
    a, b = expect_rgb(m, vy0), scr.copy()
    r0, r1 = max(0, y0 - vy0), max(0, min(M.H, y1 - vy0))
    if r1 > r0: a[r0:r1, x0:x1] = 0; b[r0:r1, x0:x1] = 0
    return np.array_equal(a, b)

if '--gpgx' in sys.argv:
    pre = sys.argv[sys.argv.index('--gpgx') + 1]
    shown = []
    for f in sorted(glob.glob(pre + '_*.dram')):
        dram = open(f, 'rb').read()
        res = []
        for b in (0, 1):
            px = unpack(dram, b)
            ks = [k for k in range(64) if k % 2 == b and np.array_equal(model[k], px)]
            res.append(ks)
        scr, vy0 = screen(f[:-5] + '.ppm')
        sk = [k for k in range(64) if masked_equal(model[k], scr, vy0)]
        ok = bool(sk) and any(res)
        if not ok: bad += 1
        shown.append((int(f[-9:-5]), sk[0] if sk else None))
        print('%s screen: %s; buffers: %s, %s' % (os.path.basename(f[:-5]), 'model frame %s' % sk if sk else 'NO MATCH',
              res[0] or 'in progress', res[1] or 'in progress'))
    seq = [s for s in shown if s[0] >= 121]
    steps = [(b[1] - a[1]) % 64 for a, b in zip(seq, seq[1:]) if b[0] - a[0] == 4 and a[1] is not None and b[1] is not None]
    if steps: print('displayed frame advance per 4 emulator frames: %s' % sorted(set(steps)))
    print('%d of %d screens equal to a model frame' % (len(shown) - bad, len(shown)))
    sys.exit(1 if bad else 0)
sim = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen', 'ssp16sim')
counts = []
with tempfile.TemporaryDirectory() as t:
    for k in range(1, 65):
        o = os.path.join(t, 'd.bin')
        r = subprocess.run([sim, rom, str(k), '-o', o] if False else [sim, '-o', o, rom, str(k)], capture_output=True, text=True)
        n = int(r.stdout.split(':')[1].split()[0])
        px = unpack(open(o, 'rb').read(), k & 1)
        other = unpack(open(o, 'rb').read(), 1 - (k & 1))
        ok = np.array_equal(px, model[k]) and not other.any()
        counts.append(n)
        if not ok:
            bad += 1
            diff = (px != model[k]).sum()
            print('frame %d: MISMATCH %d pixels, rc %d %s' % (k, diff, r.returncode, r.stderr.strip()[:200]))
print('%s: %d of 64 frames equal to the model; instructions per frame min %d max %d mean %.0f'
      % (os.path.basename(rom), 64 - bad, min(counts), max(counts), sum(counts) / len(counts)))
sys.exit(1 if bad else 0)
