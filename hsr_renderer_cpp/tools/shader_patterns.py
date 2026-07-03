#!/usr/bin/env python3
# shader_patterns.py — STUDY decompiled shaders and emit a structured "program signature" per archetype, so the
# cooker can (a) recognize which official technique a material needs and (b) drive shadergen to reproduce it 1:1.
#
# It parses the GLSL SPIRV-Cross produced (see shader_decompile.py) and extracts the PROGRAM STRUCTURE — the
# matParams UBO fields, the getTime()/UV/alpha math signatures (scroll, flipbook cell-step, radial falloff, tint
# fade, noise layers, distance fade, rotation, VAT) — the exact knobs shadergen needs to emit the same program.
#
# Usage:
#   python tools/shader_patterns.py <dir-of-decompiled-glsl>      # catalog: one signature line per archetype
#   python tools/shader_patterns.py <one.glsl> --full             # full signature for a single shader
import sys, os, re, glob, json

def read(p):
    try: return open(p, 'r', encoding='utf-8', errors='ignore').read()
    except Exception: return ''

def matparams(src):
    # the per-material UBO: `... uniform matParamsTag { <fields> } matParams;`
    m = re.search(r'uniform\s+matParamsTag\s*\{([^}]*)\}', src, re.S)
    if not m: return []
    fields = []
    for ln in m.group(1).splitlines():
        fm = re.search(r'\b(\w+)\s*;', ln.strip())
        if fm and fm.group(1) not in ('float','int','vec2','vec3','vec4','mat4','highp','mediump','lowp'):
            fields.append(fm.group(1))
    return fields

# feature detectors: (name, predicate over the source). Order = reporting order.
def features(src):
    f = {}
    f['getTime']       = bool(re.search(r'\bgetTime\s*\(|globalUniforms\.time', src))
    f['uvScroll']      = bool(re.search(r'(texcoord\w*|uv\w*|noiseUV\w*)\s*\+?=.*\*\s*time|\*\s*time\b', src))
    f['flipbookCell']  = bool(re.search(r'floor\s*\(.*time.*\)|mod\s*\(.*(cols|rows|frames|numSprites)', src, re.I)) \
                         or bool(re.search(r'\b(numCols|numRows|numSprites|spriteCount|cellsX|cellsY)\b', src, re.I))
    f['radialFalloff'] = bool(re.search(r'-\s*vec2\(0\.5\)|texcoord0?\s*-\s*vec2\(0?\.5', src))
    f['tintFade']      = bool(re.search(r'alpha\s*\*=.*(color\.w|pushConstants\.color\.w|opacity)', src, re.I))
    f['noiseLayers']   = len(re.findall(r'noiseTexture\d*|noiseSpeed|noiseScale', src, re.I)) and \
                         len(set(re.findall(r'noiseTexture\d+', src)))
    f['distanceFade']  = bool(re.search(r'approximateDistanceFromCamera|distanceFade|fadeStart|fadeEnd', src, re.I))
    f['rotation']      = bool(re.search(r'(sin|cos)\s*\(.*time|mat2\(.*cos.*sin|rotate', src, re.I))
    f['vat']           = bool(re.search(r'vatData|vatTexture|vertexAnim|VAT', src, re.I))
    f['distortion']    = bool(re.search(r'distort|refract|screenSpaceUv.*\+', src, re.I))
    f['additive']      = bool(re.search(r'One,\s*One|additive|BlendAdd', src, re.I))
    f['lightmap']      = bool(re.search(r'lightmap|Modmap|uv1|texcoord1', src, re.I))
    f['skinned']       = bool(re.search(r'boneWeights|jointMatrices|skinning|Skinned', src, re.I))
    return f

def uv_math(src):
    # capture the exact UV-animation lines (the thing shadergen must reproduce)
    lines = []
    for ln in src.splitlines():
        s = ln.strip()
        if re.search(r'(noiseUV|texcoord\w*|uv\w*)\s*[+*]?=.*(time|Speed|Offset|Scale)', s) and 'struct' not in s:
            lines.append(s)
        if re.search(r'alpha\s*[*]?=|finalOpacity|s\.alpha', s):
            lines.append(s)
    return lines[:14]

def signature(path):
    src = read(path)
    return { 'file': os.path.basename(path), 'matParams': matparams(src),
             'features': {k:v for k,v in features(src).items() if v}, 'uvMath': uv_math(src) }

def archetype_name(fn):
    return re.sub(r'\.(FRAG|VERT|COMPUTE|unk|both)\.m\d+\.glsl$','', re.sub(r'.*shaders_','', fn))

if __name__ == '__main__':
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    tgt = sys.argv[1]
    if os.path.isfile(tgt):
        print(json.dumps(signature(tgt), indent=2)); sys.exit(0)
    # directory: one concise line per FRAG archetype (frag carries the anim/fade)
    rows = {}
    for p in sorted(glob.glob(os.path.join(tgt,'*.FRAG.*.glsl'))):
        sig = signature(p); name = archetype_name(sig['file'])
        if name in rows and not sig['features']: continue   # prefer the variant with detected features
        rows[name] = sig
    print("%-58s %-40s %s" % ('ARCHETYPE','FEATURES','matParams'))
    for name in sorted(rows):
        sig = rows[name]
        feats = ','.join(sig['features'].keys()) or '-'
        mp = ','.join(sig['matParams'][:8])
        print("%-58s %-40s %s" % (name[:57], feats[:39], mp))
