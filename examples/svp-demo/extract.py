#!/usr/bin/env python3
"""TR1 PC (.PHD, version 0x20) reader: Lara (moveable type 0) posed with the
first frame of animation 11 (stand), one combined mesh.

Structures, read order, frame angle decoding and the Y-X-Z joint rotation order follow the TR1 PC level
format (README, Provenance).
Output: lara_posed.npz (vertices float, faces, per-face colour, per-face
texture info) and the 8-bit tiles + palette for the reference renderer.
"""
import struct, sys, math
import numpy as np

class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def i16(s): v = struct.unpack_from('<h', s.b, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v
    def i32(s): v = struct.unpack_from('<i', s.b, s.p)[0]; s.p += 4; return v
    def arr(s, dt, n):
        a = np.frombuffer(s.b, dtype=dt, count=n, offset=s.p); s.p += a.nbytes; return a
    def skip(s, n): s.p += n

def load(path):
    r = R(open(path, 'rb').read())
    assert r.u32() == 0x20
    ntiles = r.u32()
    tiles = r.arr(np.uint8, ntiles * 65536).reshape(ntiles, 256, 256)
    r.skip(4)
    nrooms = r.u16()
    for _ in range(nrooms):
        r.skip(16)
        r.skip(2 * r.u32())
        r.skip(32 * r.u16())
        nz, nx = r.u16(), r.u16(); r.skip(8 * nz * nx)
        r.skip(2)
        r.skip(18 * r.u16())
        r.skip(18 * r.u16())
        r.skip(4)
    r.skip(2 * r.u32())                       # floor data
    nmd = r.u32(); meshdata = bytes(r.b[r.p:r.p + 2 * nmd]); r.skip(2 * nmd)
    meshptr = r.arr('<u4', r.u32())
    nanims = r.u32(); anims = []
    for _ in range(nanims):
        a = struct.unpack_from('<IBBHiiHHHHHHHH', r.b, r.p); r.skip(32)
        anims.append(a)                       # a[0] frameOffset (bytes), a[2] frameSize
    r.skip(6 * r.u32()); r.skip(8 * r.u32()); r.skip(2 * r.u32())
    nodes = r.arr('<i4', r.u32())
    frames = r.arr('<u2', r.u32())
    models = []
    for _ in range(r.u32()):
        models.append(struct.unpack_from('<IHHIIH', r.b, r.p)); r.skip(18)
    r.skip(32 * r.u32())                      # static meshes
    otex = []
    for _ in range(r.u32()):
        attr, tile = r.u16(), r.u16()
        uv = [(r.u16() >> 8, r.u16() >> 8) for _ in range(4)]
        otex.append((attr, tile & 0x7FFF, uv))
    r.skip(16 * r.u32()); r.skip(8 * r.u32()); r.skip(16 * r.u32()); r.skip(16 * r.u32())
    nboxes = r.u32(); r.skip(20 * nboxes)
    r.skip(2 * r.u32())
    r.skip(2 * 6 * nboxes)
    r.skip(2 * r.u32())
    r.skip(22 * r.u32())
    r.skip(8192)
    pal = (r.arr(np.uint8, 768).reshape(256, 3).astype(np.int32) * 4).clip(0, 255)
    return dict(tiles=tiles, meshdata=meshdata, meshptr=meshptr, anims=anims,
                nodes=nodes, frames=frames, models=models, otex=otex, pal=pal)

def mesh(md, off):
    r = R(md); r.p = off
    r.skip(6 + 4)
    nv = r.i16(); v = r.arr('<i2', 3 * nv).reshape(nv, 3).astype(np.float64)
    nn = r.i16(); r.skip(6 * nn if nn > 0 else -2 * nn)
    faces = []   # (indices, textured, tex)
    for n, tex in ((4, True), (3, True), (4, False), (3, False)):
        for _ in range(r.i16()):
            idx = [r.u16() for _ in range(n)]; t = r.u16()
            faces.append((idx, tex, t))
    return v, faces

def rot_yxz(ax, ay, az):
    def Rx(a): c, s = math.cos(a), math.sin(a); return np.array([[1,0,0],[0,c,-s],[0,s,c]])
    def Ry(a): c, s = math.cos(a), math.sin(a); return np.array([[c,0,s],[0,1,0],[-s,0,c]])
    def Rz(a): c, s = math.cos(a), math.sin(a); return np.array([[c,-s,0],[s,c,0],[0,0,1]])
    return Ry(ay) @ Rx(ax) @ Rz(az)

def lara(path, anim_rel=11, fidx=0):
    L = load(path)
    m = [x for x in L['models'] if x[0] == 0][0]
    _, mcount, mstart, node, frame, anim0 = m
    an = L['anims'][anim0 + anim_rel]
    fr = L['frames'][an[0] // 2:]
    fr = fr[fidx * (10 + 2 * int(fr[9])):]            # TR1: fixed 10 + 2 * numangles words a frame
    pos = np.array([np.int16(fr[6]), np.int16(fr[7]), np.int16(fr[8])], float)
    nodes = L['nodes'][node:node + 4 * (mcount - 1)].reshape(-1, 4)
    sc = 2 * math.pi / 1024
    R0, T0 = np.eye(3), pos.copy()
    stack, V, F = [], [], []
    for i in range(mcount):
        if i > 0:
            fl, x, y, z = nodes[i - 1]
            if fl & 1: R0, T0 = stack.pop()
            if fl & 2: stack.append((R0.copy(), T0.copy()))
            T0 = T0 + R0 @ np.array([x, y, z], float)
        b, a = int(fr[10 + 2 * i]), int(fr[10 + 2 * i + 1])   # TR1: angles[0] = count
        ax = ((a & 0x3FF0) >> 4) * sc
        ay = (((a & 0xF) << 6) | ((b & 0xFC00) >> 10)) * sc
        az = (b & 0x3FF) * sc
        R0 = R0 @ rot_yxz(ax, ay, az)
        v, f = mesh(L['meshdata'], int(L['meshptr'][mstart + i]))
        base = sum(len(x) for x in V)
        V.append((R0 @ v.T).T + T0)
        for idx, tex, t in f:
            F.append(([base + k for k in idx], tex, t, i))
    return L, np.vstack(V), F, dict(mcount=mcount, anim=anim0 + anim_rel, numangles=int(fr[9]))

if __name__ == '__main__':
    L, V, F, info = lara(sys.argv[1])
    print(info, 'verts', len(V), 'faces', len(F),
          'quads', sum(len(f[0]) == 4 for f in F), 'tris', sum(len(f[0]) == 3 for f in F))
    print('bbox', V.min(0), V.max(0))
