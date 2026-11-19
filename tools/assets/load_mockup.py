#!/usr/bin/env python3
"""load_mockup.py <packs> <out.png> [percent] [step] -- the load progress
screen (docs/concept-load-progress.md) drawn from the game's own art: font19
(the original lettering, recoloured to the grey ramp every original full
screen holds) and green_building (the forts' countdown digits), on the
black 640x480 screen with the 320x200 picture's 2x layout of zod_schirm.

Reads the .zpk archives directly (tools/assets/pack.py's format); writes the
PNG itself (no Pillow needed). A mockup for review, not the game's code.
"""
import struct
import sys
import zlib


FMT_INDEXED, FMT_RGB565, FMT_PCM8, FMT_SHARED8 = 1, 2, 3, 4

def read_pack(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'ZPK1', path
    n, names_off, names_len = struct.unpack('>III', d[4:16])
    out = {}
    for i in range(n):
        e = d[16 + 24 * i: 16 + 24 * (i + 1)]
        noff, nlen, w, h, fmt, trans, ncol, off, blen, extra = struct.unpack('>IHHHBBHIIH', e)
        name = d[names_off + noff: names_off + noff + nlen].decode('latin1')
        out[name] = (fmt, w, h, trans, d[off: off + ncol * 3], d[off + ncol * 3: off + blen])
    return out




class Canvas:
    def __init__(self, w, h, rgb=(0, 0, 0)):
        self.w, self.h = w, h
        self.px = [list(rgb) * w for _ in range(h)]
    def put(self, x, y, rgb):
        if 0 <= x < self.w and 0 <= y < self.h:
            r = self.px[y]; r[x*3:x*3+3] = rgb
    def get(self, x, y):
        r = self.px[y]; return tuple(r[x*3:x*3+3])
    def rect(self, x, y, w, h, rgb):
        for yy in range(max(0, y), min(self.h, y + h)):
            for xx in range(max(0, x), min(self.w, x + w)):
                self.put(xx, yy, rgb)
    def blit(self, sprite, x, y, scale=1):
        """sprite: (w, h, pixels) with pixels[y][x] = rgb or None"""
        w, h, p = sprite
        for yy in range(h):
            for xx in range(w):
                c = p[yy][xx]
                if c is None: continue
                for sy in range(scale):
                    for sx in range(scale):
                        self.put(x + xx * scale + sx, y + yy * scale + sy, c)
    def save(self, path):
        raw = b''.join(b'\x00' + bytes(r) for r in self.px)
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
        open(path, 'wb').write(png)

def sprite(e, base):
    fmt, w, h, trans, pal, data = e
    p = []
    for y in range(h):
        row = []
        for x in range(w):
            if fmt == FMT_SHARED8:
                i = data[y * w + x]; row.append(None if i == 0 else base[i])
            elif fmt == FMT_INDEXED:
                i = data[y * w + x]; row.append(None if i == trans else tuple(pal[i*3:i*3+3]))
            else:
                v = struct.unpack('>H', data[(y*w+x)*2:(y*w+x)*2+2])[0]
                row.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
        p.append(row)
    return (w, h, p)



packs = sys.argv[1]
out = sys.argv[2]
pct = int(sys.argv[3]) if len(sys.argv) > 3 else 45
step = sys.argv[4] if len(sys.argv) > 4 else 'UNITS AND BUILDINGS'

d = open(packs + '/palette.zpl', 'rb').read()
base = [tuple(d[12 + i * 3: 15 + i * 3]) for i in range(256)]

P = {}
for f in ('zod_fonts.zpk', 'zod_engine.zpk'):
    P.update(read_pack(packs + '/' + f))

SATZ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?!*:-"
BROWN = [(63, 31, 0), (95, 63, 31), (127, 95, 63), (63, 63, 95)]
VGA = [(120, 120, 143), (156, 166, 176), (255, 255, 255), (10, 10, 12)]   # slots 1..4 of every loading picture
DIM = [(10, 10, 12), (120, 120, 143), (156, 166, 176), (10, 10, 12)]       # one step down: "dimmed"

def glyph19(ch, basis, dim=False):
    k = SATZ.find(ch)
    if k < 0: return None
    e = P.get('assets/fonts/font19/SPRITES_%d.png' % (basis + k))
    if not e: return None
    w, h, p = sprite(e, base)
    lut = DIM if dim else VGA
    q = []
    for row in p:
        r = []
        for c in row:
            if c is None: r.append(None); continue
            best = min(range(4), key=lambda i: sum((c[j] - BROWN[i][j]) ** 2 for j in range(3)))
            r.append(lut[best])
        q.append(r)
    return (w, h, q)

def text19(c, x, y, s, basis, gap, scale, dim=False, measure=False):
    xx = x
    for ch in s.upper():
        g = glyph19(ch, basis, dim)
        if g is None:
            xx += 8 * scale; continue
        if not measure: c.blit(g, xx, y, scale)
        xx += (g[0] + gap) * scale
    return xx - x

def led(ch):
    e = P.get('assets/fonts/green_building/char_%03d.png' % ord(ch))
    return sprite(e, base) if e else None

S2 = 2                      # the 320x200 picture at 2x
OX, OY = 0, 40              # centred on 640x480
c = Canvas(640, 480, (0, 0, 0))

# "LOADING" as zod_schirm draws it now (set "laden": font19 small, no gap), picture y 131
wl = text19(c, 0, 0, 'LOADING', 6512, 0, S2, measure=True)
text19(c, OX + (640 - wl) // 2, OY + 120 * S2, 'LOADING', 6512, 0, S2)

# the bar: a bevelled plate (the picture's own greys), 36 LED segments
bx, by, bw, bh = 70, 140, 180, 10              # picture coordinates
X, Y, W, H = OX + bx * S2, OY + by * S2, bw * S2, bh * S2
c.rect(X - 2, Y - 2, W + 4, H + 4, VGA[1])     # outer light edge
c.rect(X, Y, W + 2, H + 2, VGA[3])             # shadow bottom-right
c.rect(X, Y, W, H, (24, 26, 30))               # recess
seg_n = 44
segw = (bw - 4) // seg_n
lit = seg_n * pct // 100
GREEN_ON, GREEN_DIM, GREEN_OFF = (90, 230, 70), (40, 150, 40), (20, 52, 22)
for i in range(seg_n):
    sx = X + (2 + i * segw) * S2
    col = GREEN_ON if i < lit else GREEN_OFF
    c.rect(sx, Y + 2 * S2, (segw - 1) * S2, (bh - 4) * S2, col)
    if i < lit:
        c.rect(sx, Y + 2 * S2, (segw - 1) * S2, S2, (170, 255, 150))      # a highlight line
        c.rect(sx, Y + (bh - 3) * S2, (segw - 1) * S2, S2, GREEN_DIM)

# the share as the forts' green countdown digits, in a small recessed box right of the bar
digits = '%d' % pct
gl = [led(ch) for ch in digits]
dw = sum(g[0] + 1 for g in gl if g) * S2
DX, DY = X + W + 8 * S2, Y - 1 * S2
c.rect(DX - 3 * S2, DY - 1 * S2, dw + 5 * S2, 12 * S2, VGA[1])
c.rect(DX - 2 * S2, DY, dw + 3 * S2, 10 * S2, (12, 20, 12))
xx = DX
for g in gl:
    if g: c.blit(g, xx, DY + 1 * S2, S2); xx += (g[0] + 1) * S2

# the step: font19 small, dimmed (the original's grey), centred under the bar
ws = text19(c, 0, 0, step, 6512, 0, 1, measure=True)
text19(c, OX + (640 - ws) // 2, OY + 158 * S2, step, 6512, 0, 1, dim=True)

c.save(out)
print('saved', out, 'lit', lit, 'of', seg_n)
