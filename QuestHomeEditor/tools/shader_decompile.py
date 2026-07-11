#!/usr/bin/env python3
# shader_decompile.py — INTEGRATED shader inspector for the HSR renderer/cooker.
#
# Extracts every embedded SPIR-V module from a libshell RENDSHAD *.surface(.bin) container and decompiles
# each stage back to readable GLSL via the bundled SPIRV-Cross (tools/spirv-cross.exe). Works on:
#   * a single cooked   *.surface.bin              (what shadergen just produced — verify the fade/flipbook/translate)
#   * a directory        (recurses for *.surface*)
#   * an env .apk        (pulls surfaces out of assets/scene.zip)
#   * a raw .spv         (disassemble directly)
#
# Labels each module by SPIR-V execution model (VERT / FRAG) from OpEntryPoint, so you can see exactly which
# stage carries the getTime() animation. NO guessing — this is the real SPIR-V the device runs.
#
# Usage:
#   python tools/shader_decompile.py <file.surface.bin | dir | env.apk | file.spv> [outdir]
#   python tools/shader_decompile.py <apk> _spv_glsl --grep fog        # only surfaces whose name matches
import sys, os, struct, subprocess, glob, io, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b'\x03\x02\x23\x07'   # SPIR-V magic 0x07230203 little-endian

def find_spirv_cross():
    cands = [
        os.path.join(HERE, 'spirv-cross.exe'),
        os.path.join(HERE, 'spirv-cross'),
        os.path.join(HERE, '..', '..', 'Junk 2', '_tools', 'SPIRV-Cross', 'build', 'spirv-cross.exe'),
    ]
    for c in cands:
        if os.path.exists(c): return c
    # PATH
    from shutil import which
    return which('spirv-cross')

SC = find_spirv_cross()

EXEC_MODEL = {0: 'VERT', 1: 'TESSC', 2: 'TESSE', 3: 'GEOM', 4: 'FRAG', 5: 'COMPUTE'}

def module_stage(spv):
    # walk to the first OpEntryPoint (op 15); word[1] = execution model
    try:
        n = len(spv) // 4
        off = 5
        while off < n:
            w0 = struct.unpack_from('<I', spv, off*4)[0]
            wc = w0 >> 16; op = w0 & 0xffff
            if wc == 0: break
            if op == 15:
                model = struct.unpack_from('<I', spv, (off+1)*4)[0]
                return EXEC_MODEL.get(model, 'stage%d' % model)
            off += wc
    except Exception:
        pass
    return 'unk'

def find_spirv_modules(data):
    mods, i = [], 0
    while True:
        p = data.find(MAGIC, i)
        if p < 0: break
        if p + 20 > len(data): break
        n = (len(data) - p) // 4
        off = 5; ok_end = 5
        while off < n:
            w0 = struct.unpack_from('<I', data, p + off*4)[0]
            wc = w0 >> 16; op = w0 & 0xffff
            if wc == 0 or op > 5000:      # invalid opcode -> module ended
                break
            if p + (off+wc)*4 > len(data):
                break
            off += wc
            if op == 56:                  # OpFunctionEnd — candidate end; peek next word for sanity
                ok_end = off
                if off < n:
                    nxt = struct.unpack_from('<I', data, p + off*4)[0]
                    nop = nxt & 0xffff; nwc = nxt >> 16
                    if nwc == 0 or nop == 0 or (nop > 400 and nop != 5632):
                        break
        end = off if off >= 6 else ok_end
        blob = data[p : p + end*4]
        if end >= 6:
            mods.append(blob)
        i = p + max(end, 1)*4
    return mods

VULKAN = False   # --vulkan: emit Vulkan-semantics GLSL (layout(binding) samplers) that ROUND-TRIPS through glslang
                 # for glsl_to_surface.py. Default off = GL-style GLSL, more readable for inspection.

def decompile(spv, tag):
    if not SC:
        # no spirv-cross: leave the raw .spv for an external tool
        open(tag + '.spv', 'wb').write(spv)
        return '// spirv-cross not found; wrote raw %s.spv (%d bytes)\n' % (tag, len(spv))
    tmp = tag + '.spv'; open(tmp, 'wb').write(spv)
    out = None
    vk = ['--vulkan-semantics'] if VULKAN else []
    # --vulkan (round-trip) prefers desktop 450: multiview forward frags use textureQueryLOD which ES glslang rejects,
    # and --vulkan-semantics is REQUIRED for multiview (GL path throws "ovr_multiview_view_count"). GLSL version is just
    # the intermediate — the device consumes SPIR-V — so 450 is safe and round-trips every stage incl. multiview.
    versions = ([['--version','450'], ['--version','460']] if VULKAN
                else [['--version','310','--es'], ['--version','450'], ['--version','320','--es']])
    for a in versions:
        r = subprocess.run([SC] + a + vk + [tmp], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            out = r.stdout; break
    if os.path.exists(tmp): os.remove(tmp)
    if out is None:
        return '// spirv-cross failed:\n// ' + (r.stderr.strip().replace('\n','\n// '))
    return out

def process(path, outdir):
    data = open(path,'rb').read()
    mods = find_spirv_modules(data)
    base = os.path.splitext(os.path.basename(path))[0]
    os.makedirs(outdir, exist_ok=True)
    stages = []
    for k, m in enumerate(mods):
        st = module_stage(m); stages.append(st)
        tag = os.path.join(outdir, '%s.%s.m%d' % (base, st, k))
        glsl = decompile(m, tag)
        open(tag + '.glsl','w').write(glsl)
    print('%-52s %2d module(s) [%s] -> %s' % (os.path.basename(path), len(mods), ','.join(stages), outdir))
    return len(mods)

def gather(target, grep):
    if os.path.isdir(target):
        return glob.glob(os.path.join(target,'**','*.surface*'), recursive=True)
    if target.endswith('.apk'):
        z = zipfile.ZipFile(target)
        try: sz = zipfile.ZipFile(io.BytesIO(z.read('assets/scene.zip')))
        except KeyError:
            sz = z
        os.makedirs('_apk_surfaces', exist_ok=True); files=[]
        for nm in sz.namelist():
            low = nm.lower()
            if 'surface' in low or low.endswith('.shader') or '/shaders/' in low:
                if grep and grep.lower() not in low: continue
                p = os.path.join('_apk_surfaces', nm.replace('/','_'))
                open(p,'wb').write(sz.read(nm)); files.append(p)
        return files
    return [target]

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    grep = None
    if '--grep' in sys.argv:
        gi = sys.argv.index('--grep'); grep = sys.argv[gi+1]; args = [a for a in args if a != grep]
    if '--vulkan' in sys.argv:
        globals()['VULKAN'] = True
    target = args[0]
    outdir = args[1] if len(args) > 1 else '_spv_glsl'
    print('spirv-cross: %s' % (SC or 'NOT FOUND (raw .spv only)'))
    files = gather(target, grep)
    tot = sum(process(f, outdir) for f in files)
    print('TOTAL %d modules from %d files -> %s/' % (tot, len(files), outdir))
