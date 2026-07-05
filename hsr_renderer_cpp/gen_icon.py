#!/usr/bin/env python3
# Generates app.ico (multi-size) for the editor exe — a dark tile with a green navmesh triangle + cyan spawn dot.
import struct, math, os

def render(size):
    px = bytearray(size*size*4)  # BGRA
    cx, cy = size/2.0, size/2.0
    R = size*0.46
    for y in range(size):
        for x in range(size):
            dx, dy = x+0.5-cx, y+0.5-cy
            # rounded-square mask
            q = max(abs(dx), abs(dy))
            inside = q < R
            # base dark bg
            b,g,r,a = 32, 34, 40, 0
            if inside:
                a = 255
                # subtle vertical gradient
                t = y/size
                b = int(28+10*t); g = int(30+12*t); r = int(36+14*t)
            # green triangle (navmesh motif), pointing up, centered
            tri = size*0.30
            ty = dy + size*0.10
            if inside and ty > -tri and ty < tri:
                w = (tri - ty) * 0.55
                if abs(dx) < w:
                    b,g,r = 70, 220, 90      # green
                    # edge brighten
                    if abs(abs(dx)-w) < size*0.05 or abs(ty-tri) < size*0.05:
                        b,g,r = 120, 255, 140
            # cyan spawn dot near top
            sdx, sdy = dx, dy + size*0.26
            if inside and (sdx*sdx+sdy*sdy) < (size*0.085)**2:
                b,g,r = 235, 200, 0          # cyan (BGR)
            o = (y*size+x)*4
            px[o]=b; px[o+1]=g; px[o+2]=r; px[o+3]=a
    return bytes(px)

def bmp_for_icon(size, bgra):
    # BITMAPINFOHEADER (height doubled for the AND mask), bottom-up
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size*2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(size-1, -1, -1):       # bottom-up
        rows.append(bgra[y*size*4:(y+1)*size*4])
    xor = b''.join(rows)
    and_stride = ((size+31)//32)*4
    andmask = b'\x00' * (and_stride*size)  # alpha used; AND mask all-0
    return hdr + xor + andmask

sizes = [16, 32, 48, 64]
imgs = [(s, bmp_for_icon(s, render(s))) for s in sizes]
# ICONDIR
out = struct.pack('<HHH', 0, 1, len(imgs))
offset = 6 + 16*len(imgs)
entries = b''; datas = b''
for s, data in imgs:
    w = s if s < 256 else 0
    entries += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(data), offset)
    datas += data; offset += len(data)
out += entries + datas
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.ico')
open(p,'wb').write(out)
print('wrote', p, len(out), 'bytes')
