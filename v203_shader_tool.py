#!/usr/bin/env python3
"""V203/HSL shader tool.

The V203 "horizon shared shaders" ship PRE-COMPILED as SPIR-V inside the env apk:
  <env>.apk -> assets/scene.zip -> content/meta/.../<name>.surface/shader_t2w9
Each shader_t2w9 is a binary "SHAD" container holding MANY SPIR-V blobs (the feature
variants, vert+frag each). This tool extracts them and reflects each one's interface
(descriptor set/binding names, I/O) so the renderer can vkCreateShaderModule them and
build a matching pipeline, and so the V79->HSR cooker can target the same format.

Usage: python v203_shader_tool.py <env.apk> [out_dir]
Writes out_dir/<surface>/<idx>_<stage>.spv  + out_dir/manifest.json
"""
import zipfile, io, struct, os, sys, json

SPV_MAGIC = b'\x03\x02\x23\x07'

def spv_blob_len(d, off):
    """Length in bytes of the SPIR-V module starting at off (walk instructions)."""
    i = off + 20                                  # past 5-word header
    n = len(d)
    while i + 4 <= n:
        ins = struct.unpack_from('<I', d, i)[0]
        wc = ins >> 16; op = ins & 0xffff
        if wc == 0 or op > 600:                   # invalid -> end of module
            break
        i += wc * 4
    return i - off

def _wstr(words):
    b = b''.join(struct.pack('<I', w) for w in words)
    return b.split(b'\x00')[0].decode('ascii', 'replace')

def reflect(spv):
    """Return {stage, inputs[], outputs[], bindings[{set,binding,name}]}."""
    n = len(spv) // 4
    w = struct.unpack('<%dI' % n, spv[:n*4])
    if w[0] != 0x07230203:
        return None
    names = {}; deco = {}; stage = None; storage = {}
    i = 5
    while i < n:
        ins = w[i]; wc = ins >> 16; op = ins & 0xffff; o = w[i+1:i+wc]
        if op == 15:    # OpEntryPoint: model, id, name..., interface ids
            stage = {0: 'vert', 4: 'frag', 5: 'geom', 3: 'tese', 2: 'tesc', 1: 'tesc'}.get(o[0], str(o[0]))
        elif op == 5:   # OpName id, "str"
            names[o[0]] = _wstr(o[1:])
        elif op == 71:  # OpDecorate id, deco, [operand]
            deco.setdefault(o[0], {})[o[1]] = (o[2] if len(o) > 2 else None)
        elif op == 59:  # OpVariable result_type, result_id, storage_class
            storage[o[1]] = o[2]                   # 1=Input 3=Output 2=Uniform 0=UniformConstant 12=StorageBuffer
        i += wc
    DSET, BIND, LOC = 34, 33, 30
    binds, ins_, outs = [], [], []
    for vid, dd in deco.items():
        nm = names.get(vid, '?')
        if DSET in dd or BIND in dd:
            binds.append({'set': dd.get(DSET), 'binding': dd.get(BIND), 'name': nm})
        elif LOC in dd:
            sc = storage.get(vid)
            (ins_ if sc == 1 else outs).append({'loc': dd.get(LOC), 'name': nm})
    binds.sort(key=lambda x: (x['set'] or 0, x['binding'] or 0))
    return {'stage': stage, 'inputs': ins_, 'outputs': outs, 'bindings': binds}

def extract_shad(data):
    """Yield (idx, spirv_bytes) for every SPIR-V blob in a SHAD container."""
    offs = [i for i in range(0, len(data) - 4) if data[i:i+4] == SPV_MAGIC]
    for k, o in enumerate(offs):
        yield k, data[o:o + spv_blob_len(data, o)]

def process_apk(apk, out):
    z = zipfile.ZipFile(apk)
    sz = zipfile.ZipFile(io.BytesIO(z.read('assets/scene.zip')))
    manifest = {}
    for entry in sz.namelist():
        if not entry.endswith('.surface/shader_t2w9'):
            continue
        surf = entry.split('/shaders/')[-1].split('.surface')[0] if '/shaders/' in entry else entry
        surf = surf.replace('/', '_')
        data = sz.read(entry)
        if data[4:8] != b'SHAD':
            continue
        sd = os.path.join(out, surf); os.makedirs(sd, exist_ok=True)
        blobs = []
        for k, spv in extract_shad(data):
            ref = reflect(spv) or {}
            st = ref.get('stage', 'unk')
            fn = os.path.join(sd, '%02d_%s.spv' % (k, st))
            with open(fn, 'wb') as f: f.write(spv)
            blobs.append({'idx': k, 'stage': st, 'bytes': len(spv),
                          'bindings': ref.get('bindings', []),
                          'inputs': ref.get('inputs', []), 'outputs': ref.get('outputs', [])})
        manifest[entry] = {'surface': surf, 'blobs': blobs}
        print('%-60s %d blobs' % (surf, len(blobs)))
    with open(os.path.join(out, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print('\nmanifest -> %s/manifest.json  (%d surfaces)' % (out, len(manifest)))

if __name__ == '__main__':
    apk = sys.argv[1] if len(sys.argv) > 1 else 'Envs To check/v203 Ufficial Envs/haven2025.apk'
    out = sys.argv[2] if len(sys.argv) > 2 else 'v203_shaders_out'
    process_apk(apk, out)
