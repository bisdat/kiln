# SPDX-License-Identifier: MIT
# Reference model of SF texture codecs inferred from black-box oracle observations only
# (no GPL source). gen_fixtures.py uses parse(); the Rust codec is eml/texture.rs.
import struct
def q(x, bits):
    if bits == 4: return x >> 4
    return (x * ((1<<bits)-1) + 127) // 255
PACK = {
 # name: (bytes, [(channel, bits, shift)])
 'R4G4_unorm_pack8': (1, [(0,4,4),(1,4,0)]),
 'R4G4B4A4_unorm_pack16': (2, [(0,4,12),(1,4,8),(2,4,4),(3,4,0)]),
 'B4G4R4A4_unorm_pack16': (2, [(2,4,12),(1,4,8),(0,4,4),(3,4,0)]),
 'R5G6B5_unorm_pack16': (2, [(0,5,11),(1,6,5),(2,5,0)]),
 'B5G6R5_unorm_pack16': (2, [(2,5,11),(1,6,5),(0,5,0)]),
 'R5G5B5A1_unorm_pack16': (2, [(0,5,11),(1,5,6),(2,5,1),(3,1,0)]),
 'B5G5R5A1_unorm_pack16': (2, [(2,5,11),(1,5,6),(0,5,1),(3,1,0)]),
 'A1R5G5B5_unorm_pack16': (2, [(3,1,15),(0,5,10),(1,5,5),(2,5,0)]),
}
ORDER = {'R':[0],'RG':[0,1],'RGB':[0,1,2],'BGR':[2,1,0],'RGBA':[0,1,2,3],'BGRA':[2,1,0,3],'ABGR':[3,2,1,0]}
def parse(n):
    import re
    m = re.match(r'^([RGBA])(\d+)(?:([RGBA])\d+)?(?:([RGBA])\d+)?(?:([RGBA])\d+)?_([a-z]+)(_pack32)?$', n)
    if not m: return None
    chans = ''.join(c for c in m.group(1,3,4,5) if c)
    return ORDER.get(chans), int(m.group(2)), m.group(6)
def encode_texel(n, p):
    if n in PACK:
        nb, f = PACK[n]; v = 0
        for c,bits,sh in f: v |= q(p[c],bits) << sh
        return v.to_bytes(nb,'little')
    pr = parse(n)
    order, size, kind = pr
    S = size // 8
    out = bytearray(S*len(order))
    if S == 1:
        for i,c in enumerate(order):
            out[i] = p[c] ^ 0x80 if kind in ('snorm','sscaled','sint') else p[c]
        return bytes(out)
    for i,c in enumerate(order):
        rep = p[c] * int('01'*S, 16)
        out[i:i+S] = rep.to_bytes(S,'little')
    return bytes(out[:S*len(order)])

def expand(v, bits, mode):
    m = (1<<bits)-1
    if mode == 'rep':
        if bits == 1: return 255 if v else 0
        out = v << (8-bits); s = bits
        while s < 8:
            out |= (out >> s); s *= 2
        return out & 255
    if mode == 'round': return (v*255 + m//2)//m
    if mode == 'floor': return (v*255)//m
def f2u8(f):
    import math
    x = f * 255.0
    if x != x: return 0
    if x <= 0: return 0
    if x >= 255: return 255
    return int(x)
def decode_texel(n, t, mode='rep'):
    import struct
    if n in PACK:
        nb, f = PACK[n]; v = int.from_bytes(t[:nb],'little'); p=[0,0,0,255]
        if n == 'B5G6R5_unorm_pack16': p[3] = 0
        for c,bits,sh in f: p[c] = expand((v>>sh)&((1<<bits)-1), bits, mode)
        return bytes(p)
    order, size, kind = parse(n)
    S = size//8; p=[0,0,0,255]
    for i,c in enumerate(order):
        comp = t[i*S:(i+1)*S]
        if kind == 'sfloat':
            fv = struct.unpack({2:'<e',4:'<f',8:'<d'}[S], comp)[0]
            if S==2: fv = struct.unpack('<f',struct.pack('<f',fv))[0]
            p[c] = f2u8(fv)
        else:
            hi = comp[-1]
            p[c] = hi ^ 0x80 if kind in ('snorm','sscaled','sint') else hi
    return bytes(p)

def uf(v, ebits, mbits):
    e = v >> mbits; m = v & ((1<<mbits)-1)
    if e == (1<<ebits)-1: return float('inf') if m == 0 else float('nan')
    if e == 0: return m / (1<<mbits) * 2.0**(-14)
    return (1 + m/(1<<mbits)) * 2.0**(e-15)
def decode_packed_float(n, t):
    import struct
    v = int.from_bytes(t[:4],'little'); p=[0,0,0,255]
    if n.startswith('B10'):
        r = uf(v & 0x7ff, 5, 6); g = uf((v>>11)&0x7ff, 5, 6); b = uf((v>>22)&0x3ff, 5, 5)
    else:
        e = v >> 27; s = 2.0**(e-15-9)
        r = (v & 0x1ff)*s; g = ((v>>9)&0x1ff)*s; b = ((v>>18)&0x1ff)*s
    f32 = lambda x: struct.unpack('<f',struct.pack('<f',x))[0] if x==x and abs(x)!=float('inf') else x
    p[0],p[1],p[2] = (f2u8(f32(x)) for x in (r,g,b))
    return bytes(p)
