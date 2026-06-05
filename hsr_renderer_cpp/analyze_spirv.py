#!/usr/bin/env python3
"""Analyze SPIRV shaders from APK to find bindings, vertex inputs, etc."""
import zipfile, io, struct, os

APK = 'D:/Quest Stuff/Restore Old Envs/Working (Current Env)/com_meta_environment_prod_nuxd.apk'
SHADER_NAME = 'content/meta/renderer_module/shaders/unlit.surface/shader'

def read_apk_shader(apk_path, shader_path):
    with zipfile.ZipFile(apk_path) as z:
        sz = z.read('assets/scene.zip')
    with zipfile.ZipFile(io.BytesIO(sz)) as sz2:
        return sz2.read(shader_path)

def find_spirv_blobs(data):
    """Find all SPIRV version 1.0 blobs in binary data."""
    magic = b'\x03\x02\x23\x07'
    blobs = []
    pos = 0
    while pos + 20 <= len(data):
        pos = data.find(magic, pos)
        if pos < 0:
            break
        version = struct.unpack_from('<I', data, pos + 4)[0]
        if version == 0x00010000:
            blobs.append(pos)
            pos += 4
        else:
            pos += 1
    return blobs

def read_words(data, blob_start, blob_end):
    """Read SPIRV words from blob range."""
    words = []
    for off in range(blob_start, blob_end, 4):
        if off + 4 <= len(data):
            words.append(struct.unpack_from('<I', data, off)[0])
    return words

def analyze_spirv(words, label):
    """Analyze a SPIRV blob and return bindings, inputs, outputs."""
    if len(words) < 6:
        return None
    
    info = {
        'label': label,
        'exec_model': None,
        'exec_name': None,
        'entry_name': None,
        'bindings': {},    # id -> {set, binding}
        'inputs': {},      # id -> {location, name}
        'outputs': {},     # id -> {location, name}
        'names': {},       # id -> name
        'decorations': {}, # id -> [(decoration, params)]
    }
    
    i = 5  # skip header (magic, version, generator, bound, schema)
    while i < len(words):
        w = words[i]
        op = w & 0xFFFF
        wc = w >> 16
        if wc == 0:
            break
        
        if op == 15:  # OpEntryPoint
            info['exec_model'] = words[i + 1]
            em_names = {0: 'Vertex', 1: 'TessControl', 2: 'TessEval', 3: 'Geometry', 4: 'Fragment', 5: 'Compute', 6: 'RayGen', 7: 'Intersect', 8: 'AnyHit', 9: 'ClosestHit', 10: 'Miss', 11: 'Callable', 12: 'Task', 13: 'Mesh'}
            info['exec_name'] = em_names.get(info['exec_model'], f'Unknown({info["exec_model"]})')
            # Read name
            name_start = i + 3
            name_words = words[name_start:name_start + (wc - 3)]
            name_bytes = b''
            for nw in name_words:
                name_bytes += struct.pack('<I', nw)
            info['entry_name'] = name_bytes.split(b'\x00')[0].decode('ascii', errors='replace')
            # Record interface variables
            for j in range(3, wc):
                if i + j < len(words):
                    info['inputs'][words[i + j]] = {'location': None}
                    info['outputs'][words[i + j]] = {'location': None}
        
        elif op == 71:  # OpDecorate
            tid, dec = words[i + 1], words[i + 2]
            params = list(words[i + 3 : i + wc])
            if tid not in info['decorations']:
                info['decorations'][tid] = []
            info['decorations'][tid].append((dec, params))
        
        elif op == 5:  # OpName
            tid = words[i + 1]
            name_bytes = b''
            for j in range(2, wc):
                name_bytes += struct.pack('<I', words[i + j])
            info['names'][tid] = name_bytes.split(b'\x00')[0].decode('ascii', errors='replace')
        
        i += wc
    
    # Process decorations for bindings
    for tid, decs in info['decorations'].items():
        ds_info = {'set': 0, 'binding': None, 'location': None}
        for dec, params in decs:
            if dec == 34 and params:  # DescriptorSet
                ds_info['set'] = params[0]
            elif dec == 33 and params:  # Binding
                ds_info['binding'] = params[0]
            elif dec == 30 and params:  # Location
                ds_info['location'] = params[0]
        if ds_info['binding'] is not None:
            info['bindings'][tid] = ds_info
        if ds_info['location'] is not None:
            if tid in info['inputs']:
                info['inputs'][tid]['location'] = ds_info['location']
            if tid in info['outputs']:
                info['outputs'][tid]['location'] = ds_info['location']
    
    # Find actual input variables (OpVariable with Input storage class)
    # Also find their types
    info['input_vars'] = {}
    info['output_vars'] = {}
    info['ubo_vars'] = {}
    info['sampler_vars'] = {}
    
    i = 5
    while i < len(words):
        w = words[i]
        op = w & 0xFFFF
        wc = w >> 16
        if wc == 0: break
        
        if op == 59 and wc >= 4:  # OpVariable
            result_type = words[i + 1]
            result_id = words[i + 2]
            storage_class = words[i + 3]
            name = info['names'].get(result_id, f'%{result_id}')
            
            if storage_class == 1:  # Input
                loc = info['inputs'].get(result_id, {}).get('location', '?')
                info['input_vars'][result_id] = {'type': result_type, 'name': name, 'location': loc}
            elif storage_class == 3:  # Output
                loc = info['outputs'].get(result_id, {}).get('location', '?')
                info['output_vars'][result_id] = {'type': result_type, 'name': name, 'location': loc}
            elif storage_class == 2:  # Uniform
                bnd = info['bindings'].get(result_id, {})
                info['ubo_vars'][result_id] = {'type': result_type, 'name': name, 'set': bnd.get('set', 0), 'binding': bnd.get('binding', '?')}
            elif storage_class == 0:  # UniformConstant (samplers/textures)
                bnd = info['bindings'].get(result_id, {})
                info['sampler_vars'][result_id] = {'type': result_type, 'name': name, 'set': bnd.get('set', 0), 'binding': bnd.get('binding', '?')}
        
        i += wc
    
    return info

def main():
    print(f'Loading shader: {SHADER_NAME}')
    data = read_apk_shader(APK, SHADER_NAME)
    print(f'Shader file size: {len(data)} bytes')
    
    blobs = find_spirv_blobs(data)
    print(f'Found {len(blobs)} SPIRV blobs\n')
    
    for bi in range(len(blobs)):
        start = blobs[bi]
        end = blobs[bi + 1] if bi + 1 < len(blobs) else len(data)
        size = end - start
        words = read_words(data, start, end)
        
        info = analyze_spirv(words, f'Blob {bi}')
        if info is None:
            continue
        
        print(f'{"="*60}')
        print(f'Blob {bi}: {size} bytes, {len(words)} words')
        print(f'  Entry: {info["entry_name"]} ({info["exec_name"]})')
        
        if info['input_vars']:
            print(f'  Vertex Inputs:')
            for vid, v in sorted(info['input_vars'].items()):
                print(f'    location={v["location"]}  {v["name"]}')
        
        if info['output_vars']:
            print(f'  Shader Outputs:')
            for vid, v in sorted(info['output_vars'].items()):
                print(f'    location={v["location"]}  {v["name"]}')
        
        if info['ubo_vars']:
            print(f'  Uniform Buffers (UBO):')
            for vid, v in sorted(info['ubo_vars'].items()):
                print(f'    set={v["set"]} binding={v["binding"]}  {v["name"]}')
        
        if info['sampler_vars']:
            print(f'  Samplers/Textures:')
            for vid, v in sorted(info['sampler_vars'].items()):
                print(f'    set={v["set"]} binding={v["binding"]}  {v["name"]}')
        
        if info['bindings']:
            print(f'  All Bindings:')
            for tid, b in sorted(info['bindings'].items()):
                name = info['names'].get(tid, f'%{tid}')
                print(f'    set={b["set"]} binding={b["binding"]}  {name}')
        
        print()

if __name__ == '__main__':
    main()
