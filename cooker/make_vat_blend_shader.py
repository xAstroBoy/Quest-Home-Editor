#!/usr/bin/env python
"""Build cooker/vatunlitblend.surface.bin: the stock V203 `vatunlitbasecolor` VAT RENDSHAD made
TRANSPARENT (alpha-blend) + DOUBLE-SIDED, so an animated wisp keeps its VAT vertex morph but draws
like the V79 wisp's material (alphaMode=BLEND, doubleSided=true, baseColorTexture with alpha).

GROUND TRUTH (isolated from 5 shipped Meta `.surface` files, not guessed):
  Each RENDSHAD `passes[]` table (forward / forward_debug) carries the pipeline render-state as inline
  scalar fields. Comparing otherwise-identical shipped shaders pins three of them exactly:
    * vatunlitbasecolor (opaque, 1-sided): f2,f3,f4 all PRESENT (=0xffffffff)
    * vatunlitdoublesided (opaque, 2-sided): same but f4 ABSENT   -> f4 = cull/face state
    * vatlitbubble (transparent, 1-sided):   f2,f3 ABSENT          -> f2,f3 = opaque-depth/blend gate
    * renderer_module unlit -> unlitblend (alpha blend): also drops EXACTLY f2,f3
  i.e. dropping f2,f3 converts opaque->alpha-blend (proven by unlit->unlitblend, which are otherwise
  identical), and dropping f4 converts 1-sided->doubleSided (proven by basecolor->doublesided).

EDIT = make f2,f3,f4 ABSENT by zeroing their 2-byte vtable slots in BOTH passes (matches the shipped
blend shaders, which omit these fields). FlatBuffers: an absent field reads its schema default, so the
device's strict verifier accepts it (shipped unlitblend/bubble load on-device with these fields absent).
No data shifts -> all other offsets stay valid (same minimal-edit philosophy as make_pulse_shader.py).
The vtables were verified UNSHARED across the whole buffer, so zeroing slots is collateral-free.
"""
import struct, sys

SRC = "cooker/vat_shader.bin"               # vatunlitbasecolor (the cooker's VAT template)
OUT = "cooker/vatunlitblend.surface.bin"
# ⛔ DEVICE-PROVEN 2026-06-10: drop ONLY f2,f3 — this exactly reproduces the SHIPPED `vatlitbubble` pass
# state {f2 absent, f3 absent, f4 present, f5 present}, which renders transparently on device. ALSO dropping
# f4 (doubleSided) made a config NO shipped shader has (f2,f3,f4 all absent) and the wisp sparkles went
# INVISIBLE on the Quest (the pipeline broke). The wisp billboards face the player, so single-sided (f4 kept)
# is fine. Transparency = f2,f3 absent (proven by both vatlitbubble AND renderer_module unlit->unlitblend).
DROP_FIELDS = (2, 3)                          # f2,f3 = transparent/blend gate (keep f4 = single-sided, like vatlitbubble)

def main():
    d = bytearray(open(SRC, "rb").read()); N = len(d)
    u16 = lambda o: struct.unpack_from("<H", d, o)[0] if 0 <= o and o + 2 <= N else 0
    i32 = lambda o: struct.unpack_from("<i", d, o)[0] if 0 <= o and o + 4 <= N else 0
    u32 = lambda o: struct.unpack_from("<I", d, o)[0] if 0 <= o and o + 4 <= N else 0
    def nf(t):
        v = t - i32(t)
        if v < 0 or v + 4 > N: return 0
        return (u16(v) - 4) // 2
    def fo(t, fi):
        v = t - i32(t)
        if v < 0 or v + 4 > N: return 0
        vs = u16(v); sl = v + 4 + fi * 2
        return (t + u16(sl)) if sl + 2 <= v + vs and sl + 2 <= N and u16(sl) else 0
    def sat(p):
        if not p or p + 4 > N: return None
        s = p + u32(p)
        if s < 0 or s + 4 > N: return None
        ln = u32(s)
        if 0 < ln <= 64 and s + 4 + ln <= N:
            try: return d[s + 4:s + 4 + ln].decode("ascii")
            except: return None
        return None

    # ---- locate the passes vector + each pass table ----
    root = u32(0); passes = []
    for fi in range(nf(root)):
        fp = fo(root, fi)
        if not fp or not u32(fp): continue
        vec = fp + u32(fp)
        if vec + 4 > N: continue
        cnt = u32(vec)
        if not (0 < cnt <= 64): continue
        base = vec + 4
        if base + cnt * 4 > N: continue
        e0 = base + u32(base)
        if not any((sat(fo(e0, ef)) or "").startswith("forward") for ef in range(min(nf(e0), 6))): continue
        for pi in range(cnt):
            pt = base + pi * 4 + u32(base + pi * 4)
            nm = next((sat(fo(pt, ef)) for ef in range(nf(pt)) if (sat(fo(pt, ef)) or "").startswith("forward")), None)
            passes.append((nm, pt))
        break
    assert passes, "passes vector not found"

    # ---- zero the vtable slots for f2,f3,f4 in every pass (forward + forward_debug) ----
    for nm, pt in passes:
        vt = pt - i32(pt); vts = u16(vt)
        for f in DROP_FIELDS:
            sl = vt + 4 + f * 2
            if sl + 2 <= vt + vts:
                before = u16(sl)
                struct.pack_into("<H", d, sl, 0)          # field -> absent (default)
                print(f"  pass '{nm}' vtable@{vt}: f{f} slot@{sl} {before} -> 0 (absent)")
    open(OUT, "wb").write(d)
    print(f"wrote {OUT}: {len(d)} bytes")
    return 0

if __name__ == "__main__":
    sys.exit(main())
