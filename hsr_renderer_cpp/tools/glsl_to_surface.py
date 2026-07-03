#!/usr/bin/env python3
# glsl_to_surface.py — the WRITE half of the round-trip: recompile an (edited) GLSL stage and swap it back into a
# libshell RENDSHAD *.surface container, so shadergen/the cook can TRANSFORM ANY SHADER (not just hand-patch SPIR-V).
#
# Pipeline: shader_decompile.py gives you the stage GLSL -> edit it (add getTime fade / scroll / whatever) ->
# this compiles it with the bundled glslangValidator and repoints the container's SPIR-V slot to the new module
# (append-at-EOF + rewrite the FlatBuffer offset field, exactly like shadergen::generate does in C++).
#
# Usage:
#   python tools/glsl_to_surface.py <base.surface> <moduleIndex> <edited.frag|.vert|.spv> <out.surface> [--stage frag|vert]
#   (moduleIndex = the index printed by shader_decompile.py, e.g. the forward FRAG module)
import sys, os, struct, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
GLSLANG = os.path.join(HERE, 'glslangValidator.exe')
MAGIC = b'\x03\x02\x23\x07'

def u32(d, o): return struct.unpack_from('<I', d, o)[0]

def find_modules(data):
    # returns list of (blobStart vv, spvOff vv+4, spvLen L) for each embedded SPIR-V, where L = u32(vv)
    mods, i = [], 0
    while True:
        p = data.find(MAGIC, i)
        if p < 0: break
        vv = p - 4
        if vv < 0: i = p + 4; continue
        L = u32(data, vv)
        # validate: L is the module byte length, 4-aligned, fits, and the magic sits at vv+4
        if L >= 20 and L % 4 == 0 and vv + 4 + L <= len(data) and (L == (len(data)-(vv+4)) or True):
            # walk instructions to get the real module length (don't trust L blindly for the LAST module)
            n = L // 4
            mods.append((vv, p, L))
            i = p + 4 + max(L, 4)
        else:
            i = p + 4
    return mods

def find_slot(data, vv):
    # the FlatBuffer offset field `sp` (4-aligned) with sp + u32(sp) == vv  (the pointer to this blob)
    cands = []
    for sp in range(0, vv, 4):
        if sp + u32(data, sp) == vv:
            cands.append(sp)
    return cands

def compile_glsl(path, stage):
    spv = path + '.spv'
    st = stage or ('frag' if path.endswith('.frag') or 'frag' in path.lower() else 'vert')
    r = subprocess.run([GLSLANG, '-V', '--target-env', 'vulkan1.0', '-S', st, path, '-o', spv],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(spv):
        print('glslang FAILED:\n' + r.stdout + r.stderr); return None
    b = open(spv, 'rb').read(); os.remove(spv); return b

def swap(base_path, mod_idx, new_stage_path, out_path, stage=None):
    data = bytearray(open(base_path, 'rb').read())
    mods = find_modules(data)
    if mod_idx < 0 or mod_idx >= len(mods):
        print('module %d out of range (found %d)' % (mod_idx, len(mods))); return False
    vv, spvOff, L = mods[mod_idx]
    slots = find_slot(data, vv)
    if not slots:
        print('no FlatBuffer slot points to blob@%d — cannot repoint' % vv); return False
    slot = slots[-1]   # the nearest/last pointer is the stage field
    # get the new SPIR-V
    if new_stage_path.endswith('.spv'):
        newspv = open(new_stage_path, 'rb').read()
    else:
        newspv = compile_glsl(new_stage_path, stage)
        if newspv is None: return False
    # append [len][spv] at 4-aligned EOF, repoint slot
    while len(data) % 4: data.append(0)
    newBlob = len(data)
    data += struct.pack('<I', len(newspv)) + newspv
    struct.pack_into('<I', data, slot, newBlob - slot)
    open(out_path, 'wb').write(data)
    print('swapped module %d (was %d B @%d) -> new %d B @%d, slot@%d repointed. out=%s (%d B)'
          % (mod_idx, L, spvOff, len(newspv), newBlob+4, slot, out_path, len(data)))
    return True

if __name__ == '__main__':
    if len(sys.argv) < 5: print(__doc__); sys.exit(1)
    stage = None
    if '--stage' in sys.argv: stage = sys.argv[sys.argv.index('--stage')+1]
    swap(sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], stage)
