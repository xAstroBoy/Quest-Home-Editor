#!/usr/bin/env python3
# v79_transpile.py — the 1:1 V79→HSL SHADER PORT (user goal: "ANY SHADER, ANY MATERIAL SETTING ...
# DECOMPILED AND PORTED TO HSL ... 1:1").
#
# For every material in a V79 env APK this reads the AUTHORED .mat.txt (Shader/AlphaTest/Transparent/
# Additive/DoubleSided + every uniform incl. alphatestthreshold/diffuse/alpha), picks the matching PROVEN
# V205 base surface (unlit / unlitblend / skinned / skinnedblend — their FlatBuffer containers carry the
# device-verified render state + descriptor layout), decompiles the base's FORWARD FRAG with
# --vulkan-semantics (explicit set/binding stay intact), injects the V79 math verbatim:
#     color = diffuseUniform × tex            (vertexColor0 carries diffuse×tint×lightmap — cook bakes it)
#     color *= pushConstants.color            (entity color)
#     if (color.a < AUTHORED alphatestthreshold) discard      [AlphaTest materials — POST-multiply alpha,
#                                                              exactly the ripped MeshShellEnv_runtime GLSL]
# recompiles with glslangValidator and swaps the module back into a COPY of the base container
# (same append+repoint scheme as shadergen::generate / glsl_to_surface.py).
#
# Output: cooker/v79gen/<variant>.surface.bin + cooker/v79gen/manifest.json
#         manifest maps material STEM -> { surface, cutoff, blend, additive, skinnedSurface }
# The cooker consumes the manifest (material stem match) and uses these surfaces INSTEAD of heuristics —
# the cooker only ADAPTS (bakes vertex colors, wires textures), it no longer decides shading.
#
# Usage: python tools/v79_transpile.py <env.apk> [--force]
import sys, os, io, re, json, struct, zipfile, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # hsr_renderer_cpp/
GLSLANG = os.path.join(HERE, 'glslangValidator.exe')
SPIRV_CROSS = os.path.join(HERE, 'spirv-cross.exe')
MAGIC = b'\x03\x02\x23\x07'

def u32(d, o): return struct.unpack_from('<I', d, o)[0]

# ── container plumbing (same logic as glsl_to_surface.py) ──────────────────────────────────────────────
def find_modules(data):
    mods, i = [], 0
    while True:
        p = data.find(MAGIC, i)
        if p < 0: break
        vv = p - 4
        if vv < 0: i = p + 4; continue
        L = u32(data, vv)
        if L >= 20 and L % 4 == 0 and vv + 4 + L <= len(data):
            mods.append((vv, p, L)); i = p + 4 + max(L, 4)
        else: i = p + 4
    return mods

def module_stage(spv):
    n = len(spv)//4
    i = 5
    while i < n:
        w = struct.unpack_from('<I', spv, i*4)[0]
        wc, op = w >> 16, w & 0xffff
        if wc == 0: break
        if op == 15:  # OpEntryPoint: word1 = execution model (0 VERT, 4 FRAG)
            return struct.unpack_from('<I', spv, (i+1)*4)[0]
        i += wc
    return -1

def forward_frag_index(data):
    # the FORWARD frag = the LARGEST FRAG module (depth/shadow frags are stubs) — matches shadergen's pick
    mods = find_modules(data)
    best, bestLen = -1, 0
    for k, (vv, p, L) in enumerate(mods):
        spv = data[p-0:p+L][0:L] if False else data[vv+4:vv+4+L]
        if module_stage(spv) == 4 and L > bestLen: best, bestLen = k, L
    return best, mods

def find_slot(data, vv):
    for sp in range(0, vv, 4):
        if sp + u32(data, sp) == vv: return sp
    return -1

def swap_module(base, mod_idx, new_spv):
    data = bytearray(base)
    mods = find_modules(bytes(data))
    vv, p, L = mods[mod_idx]
    sp = find_slot(bytes(data), vv)
    if sp < 0: raise RuntimeError('slot not found for module %d' % mod_idx)
    while len(data) % 4: data.append(0)
    nv = len(data)
    data += struct.pack('<I', len(new_spv)) + new_spv
    struct.pack_into('<I', data, sp, nv - sp)
    return bytes(data)

# ── GLSL of a module (vulkan semantics keeps set/binding) ──────────────────────────────────────────────
def decompile(spv_bytes):
    tmp = os.path.join(HERE, '_t.spv')
    open(tmp, 'wb').write(spv_bytes)
    r = subprocess.run([SPIRV_CROSS, tmp, '--vulkan-semantics', '--version', '450'],
                       capture_output=True, text=True)
    os.remove(tmp)
    if r.returncode != 0: raise RuntimeError('spirv-cross: ' + r.stderr)
    return r.stdout

def compile_frag(glsl):
    src = os.path.join(HERE, '_t.frag'); out = os.path.join(HERE, '_t.frag.spv')
    open(src, 'w').write(glsl)
    r = subprocess.run([GLSLANG, '-V', '--target-env', 'vulkan1.0', '-S', 'frag', src, '-o', out],
                       capture_output=True, text=True)
    os.remove(src)
    if r.returncode != 0 or not os.path.exists(out):
        raise RuntimeError('glslang: ' + r.stdout + r.stderr)
    b = open(out, 'rb').read(); os.remove(out)
    return b

# ── the V79 feature injection ──────────────────────────────────────────────────────────────────────────
def inject_cutoff(glsl, cutoff):
    # V79 (ripped MeshShellEnv_runtime.frag.glsl): the discard tests the POST-multiply alpha
    # (BaseColorFactor.a × tex.a). In the V205 template that is s.alpha after pushConstants.color.w —
    # the skinned bases access push constants through a named block (`_1012.pushConstants.color.w`),
    # so match the tail, not the exact expression.
    m = re.search(r'^([ \t]*)s\.alpha \*= [\w.]*pushConstants\.color\.w;', glsl, re.M)
    if not m: raise RuntimeError('anchor not found for cutoff injection')
    ins = m.group(0) + ('\n%sif (s.alpha < %.6f) { discard; }' % (m.group(1), cutoff))
    return glsl[:m.start()] + ins + glsl[m.end():]

# ── material table from the APK ────────────────────────────────────────────────────────────────────────
def read_materials(apk):
    z = zipfile.ZipFile(apk)
    sc = None
    for n in z.namelist():
        if n.endswith('scene.zip'): sc = zipfile.ZipFile(io.BytesIO(z.read(n))); break
    zz = sc if sc else z
    mats = {}
    for n in zz.namelist():
        if not n.endswith('.mat.txt'): continue
        t = zz.read(n).decode('utf-8', 'replace')
        stem = os.path.basename(n)[:-8].lower()           # strip ".mat.txt"
        def flag(k):
            m = re.search(k + r':\s*(\w+)', t); return (m.group(1) == 'true') if m else False
        def uni(k, dv):
            m = re.search(r'Name: ' + k + r'\s+Value:\s*(?:\[([^\]]*)\]|-\s*([\d.eE+-]+))', t)
            if not m: return dv
            if m.group(1) is not None:
                try: return float(m.group(1).split(',')[0])
                except: return dv
            try: return float(m.group(2))
            except: return dv
        mats[stem] = {
            'alphaTest':  flag('AlphaTest'),
            'transparent':flag('Transparent'),
            'additive':   flag('Additive'),
            'doubleSided':flag('DoubleSided'),
            'cutoff':     uni('alphatestthreshold', 0.5),
            'alpha':      uni('alpha', 1.0),
        }
    return mats

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    apk = sys.argv[1]
    # output where the COOK reads it: <repo>/cooker/v79gen (the cook's CWD-relative "cooker/"), not hsr_renderer_cpp/cooker
    outdir = os.path.join(os.path.dirname(ROOT), 'cooker', 'v79gen')
    os.makedirs(outdir, exist_ok=True)
    bases = {
        'static':      os.path.join(ROOT, 'cooker', 'nuxd_unlit_shader.bin'),
        'staticblend': os.path.join(ROOT, 'cooker', 'nuxd_unlitblend_shader.bin'),
        'skin':        os.path.join(ROOT, 'cooker', 'unlitdoublesidedskinned.bin'),
        'skinblend':   os.path.join(ROOT, 'cooker', 'unlitblendskinned.bin'),
    }
    for k, p in list(bases.items()):
        if not os.path.exists(p):
            alt = p.replace('hsr_renderer_cpp' + os.sep + 'cooker', 'Junk 2' + os.sep + 'cooker')
            if os.path.exists(alt): bases[k] = alt
    baseBytes = {k: open(p, 'rb').read() for k, p in bases.items()}
    baseGlsl, baseFwdIdx = {}, {}
    for k, b in baseBytes.items():
        idx, mods = forward_frag_index(b)
        vv, p, L = mods[idx]
        baseFwdIdx[k] = idx
        baseGlsl[k] = decompile(b[vv+4:vv+4+L])
        print('[base] %-11s fwd-frag module %d (%d bytes SPIR-V)' % (k, idx, L))

    mats = read_materials(apk)
    print('[mats] %d materials' % len(mats))
    manifest = {}
    made = {}
    for stem, m in sorted(mats.items()):
        # variant selection = EXACTLY what the authored flags say
        if m['alphaTest'] and not m['transparent']:
            variants = [('static', 'cut'), ('skin', 'cut')]     # masked: opaque pass + authored-cutoff discard
        else:
            continue   # blend/add/opaque: the proven bases already ARE the 1:1 V79 unlit math (vertexColor0×tex×color+fog)
        entry = {'cutoff': m['cutoff'], 'doubleSided': m['doubleSided']}
        for baseKey, kind in variants:
            cp = int(round(m['cutoff']*100))
            name = 'v79_%s_c%02d' % (baseKey, cp)
            fn = os.path.join(outdir, name + '.surface.bin')
            if name not in made:
                glsl = inject_cutoff(baseGlsl[baseKey], m['cutoff'])
                spv = compile_frag(glsl)
                out = swap_module(baseBytes[baseKey], baseFwdIdx[baseKey], spv)
                open(fn, 'wb').write(out)
                made[name] = True
                print('[gen ] %s  (cutoff %.2f)' % (name, m['cutoff']))
            entry['surface_' + baseKey] = name
        manifest[stem] = entry
    open(os.path.join(outdir, 'manifest.json'), 'w').write(json.dumps(manifest, indent=1))
    print('[done] %d variants, %d masked materials -> %s' % (len(made), len(manifest), outdir))

if __name__ == '__main__':
    main()
