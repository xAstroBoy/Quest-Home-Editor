import struct
# ASMH assets.manifest writer/reader — EXACT match to libshell LocalAssetManifest.cpp deserializer
# (sub_20520D4) + HsrAssetLoader createFromManifestPath. Verified field-for-field against Nuxd's manifest.
#
# File: [header 20B: "ASMH", u32 ver=2, u64 pad, u32 root_off(@16; rootTable = root_off+16)]
# Root FlatBuffer table: CONTENT vector exposed at **vtable field index 1** (field 0 absent).
# Each CONTENT element = a FlatBuffer TABLE with 4 fields (Nuxd-exact byte layout):
#   field0 @off4  = AssetReference inline struct {pkg u64@4, ing u64@12, tgt u32@20}  (20B; @24-31 pad)
#   field1 @off32 = path  (uoffset -> fb string)
#   field2 @off36 = type FourCC u32  (category: 'REND' for MESH/TXTR/SHAD, 'MATL' for materials, 'HRZN' for hstf)
#   field3 @off44 = asset byte size u32   (@40-43 pad)
# Entry vtable: vtsize=12, tablesize=48, field offsets [4,32,36,44]. Entries aligned so tbl%8==4 (AssetRef u64s 8-aligned).

def _fourcc(v):
    if isinstance(v, int): return v & 0xffffffff
    b = (v.encode() if isinstance(v, str) else v)[:4].ljust(4, b'\0')
    return struct.unpack('<I', b)[0]

def build_asmh(entries):
    # entries: list of (pkg, ing, tgt, path, fourcc, size)
    H = 20
    pos = {}; p = H
    pos['root'] = p; p += 8                          # root table: soffset@0, uoffset@4 (CONTENT vec)
    if p % 2: p += 1
    pos['rvt'] = p; p += 10                           # root vtable: vtsize,tablesize,field0,field1 (4 u16 + 1 pad slot)
    while p % 4: p += 1
    pos['vec'] = p; p += 4 + 4 * len(entries)         # CONTENT vector: count + i32 per element
    if p % 2: p += 1
    pos['evt'] = p; p += 4 + 2 * 4                    # shared entry vtable: vtsize=12 -> 4 fields
    elem = []
    for _ in entries:
        while p % 8 != 4: p += 1                       # tbl%8==4 so AssetRef u64s land 8-aligned
        elem.append(p); p += 48
    spos = {}
    for e in entries:
        path = e[3]
        if path in spos: continue
        while p % 4: p += 1
        spos[path] = p; b = path.encode('utf-8'); p += 4 + len(b) + 1
    total = p
    buf = bytearray(total)
    buf[0:4] = b'ASMH'; struct.pack_into('<I', buf, 4, 2); struct.pack_into('<Q', buf, 8, 0)
    struct.pack_into('<I', buf, 16, pos['root'] - 16)
    rt = pos['root']; struct.pack_into('<i', buf, rt, rt - pos['rvt']); struct.pack_into('<I', buf, rt + 4, pos['vec'] - (rt + 4))
    rvt = pos['rvt']                                   # CONTENT at field index 1 (f0 absent)
    struct.pack_into('<H', buf, rvt, 10); struct.pack_into('<H', buf, rvt + 2, 8)
    struct.pack_into('<H', buf, rvt + 4, 0); struct.pack_into('<H', buf, rvt + 6, 4)
    vc = pos['vec']; struct.pack_into('<I', buf, vc, len(entries))
    for i, ep in enumerate(elem):
        slot = vc + 4 + 4 * i; struct.pack_into('<i', buf, slot, ep - slot)
    evt = pos['evt']; struct.pack_into('<H', buf, evt, 12); struct.pack_into('<H', buf, evt + 2, 48)
    for k, fo in enumerate([4, 32, 36, 44]): struct.pack_into('<H', buf, evt + 4 + 2 * k, fo)
    for (pkg, ing, tgt, path, fourcc, size), ep in zip(entries, elem):
        struct.pack_into('<i', buf, ep, ep - evt)     # soffset -> shared entry vtable
        struct.pack_into('<Q', buf, ep + 4, pkg & ((1 << 64) - 1))
        struct.pack_into('<Q', buf, ep + 12, ing & ((1 << 64) - 1))
        struct.pack_into('<I', buf, ep + 20, tgt & 0xffffffff)
        struct.pack_into('<i', buf, ep + 32, spos[path] - (ep + 32))   # path uoffset (fwd)
        struct.pack_into('<I', buf, ep + 36, _fourcc(fourcc))          # type FourCC
        struct.pack_into('<I', buf, ep + 44, size & 0xffffffff)        # byte size
    for path, sp in spos.items():
        b = path.encode('utf-8'); struct.pack_into('<I', buf, sp, len(b)); buf[sp + 4:sp + 4 + len(b)] = b
    return bytes(buf)

def asset_fourcc(data):
    # category FourCC for a cooked asset, derived from its file magic@4 (matches libshell's manifest).
    m = bytes(data[4:8]) if len(data) >= 8 else b''
    if m == b'MATL': return b'MATL'
    if m in (b'MESH', b'TXTR', b'SHAD'): return b'REND'
    return b'HRZN'   # hstf/template/json + anything else

# ---- reader: read CONTENT (root field 1) + each entry's AssetRef(field0)+path(field1), EXACT vtable walk ----
def parse_asmh(d):
    if d[:4] != b'ASMH': raise ValueError('not ASMH')
    SZ = len(d)
    def u16(o): return struct.unpack_from('<H', d, o)[0] if 0 <= o and o + 2 <= SZ else 0
    def u32(o): return struct.unpack_from('<I', d, o)[0] if 0 <= o and o + 4 <= SZ else 0
    def i32(o): return struct.unpack_from('<i', d, o)[0] if 0 <= o and o + 4 <= SZ else 0
    def u64(o): return struct.unpack_from('<Q', d, o)[0] if 0 <= o and o + 8 <= SZ else 0
    def fbstr(o):
        if o < 0 or o + 4 > SZ: return ''
        ln = u32(o)
        if ln == 0 or ln > 1024 or o + 4 + ln > SZ: return ''
        return d[o + 4:o + 4 + ln].decode('utf-8', 'replace')
    def field_off(tbl, fi):  # absolute pos of table field fi (0 if absent)
        vt = tbl - i32(tbl)
        if vt < 0 or vt + 4 > SZ: return 0
        if 4 + fi * 2 + 2 > u16(vt): return 0
        fo = u16(vt + 4 + fi * 2)
        return tbl + fo if fo else 0
    rootTbl = u32(16) + 16
    # libshell (sub_20520D4) picks the CONTENT entry vector by schema variant:
    #   vtsize>=0x13 AND field 7 present -> field 7 (full schema: vista/haven)
    #   else                            -> field 1 (reduced schema: nuxd-style / our cooker)
    vt = rootTbl - i32(rootTbl); vtsize = u16(vt)
    cfield = 7 if (vtsize >= 0x13 and field_off(rootTbl, 7)) else 1
    cf = field_off(rootTbl, cfield)
    if not cf: return {}
    vec = cf + u32(cf); cnt = u32(vec)
    out = {}
    if cnt == 0 or cnt > 100000: return out
    for i in range(cnt):
        ep = vec + 4 + i * 4; tbl = ep + i32(ep)
        a = field_off(tbl, 0)                # AssetReference struct
        pf = field_off(tbl, 1)               # path
        if not a or not pf: continue
        pkg, ing, tgt = u64(a), u64(a + 8), u32(a + 16)
        path = fbstr(pf + i32(pf))
        if ing < 0x100000000 or not path: continue
        out[(pkg, ing, tgt)] = path
    return out

if __name__ == '__main__':
    test = [(0x1234, 0xABCDEF0123456789, 0x6E4CC522, 'meta/x/y.rendmesh/mesh', b'REND', 208),
            (0, 0xDEAD12345678, 0x6E4CC522, 'meta/a/b.png/tex', b'REND', 4096),
            (0x9, 0xCAFEBABE1234, 0x095BD446, 'meta/m/c.material/material', b'MATL', 176)]
    blob = build_asmh(test)
    got = parse_asmh(blob)
    ok = all(got.get((p, i, t)) == path for (p, i, t, path, _, _) in test)
    print('ASMH round-trip (4-field entries):', 'OK %d entries %dB' % (len(got), len(blob)) if ok else 'FAIL %s' % got)
