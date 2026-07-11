#!/usr/bin/env python3
"""Check SPIRV for RowMajor/ColMajor matrix decorations on types and members."""
import zipfile, io, struct

APK = 'D:/Quest Stuff/Restore Old Envs/Working (Current Env)/com_meta_environment_prod_nuxd.apk'
SHADER = 'content/meta/renderer_module/shaders/unlit.surface/shader'

def get_words(data, start, end):
    return [struct.unpack_from('<I', data, start+i*4)[0] for i in range(min(8000, (end-start)//4))]

with zipfile.ZipFile(APK) as z:
    sz = z.read('assets/scene.zip')
with zipfile.ZipFile(io.BytesIO(sz)) as sz2:
    data = sz2.read(SHADER)

magic = b'\x03\x02\x23\x07'
poss = []
p = 0
while p + 20 <= len(data):
    p = data.find(magic, p)
    if p < 0: break
    if struct.unpack_from('<I', data, p+4)[0] == 0x00010000:
        poss.append(p)
        p += 4
    else:
        p += 1

labels = ['Fragment', 'Vertex', 'Fragment2', 'Vertex2']

for bi in range(len(poss)):
    start = poss[bi]
    end = poss[bi+1] if bi+1 < len(poss) else len(data)
    words = get_words(data, start, end)
    print(f'\n===== {labels[bi]} ({len(words)} words) =====')
    
    # Track all decorations
    type_decos = {}    # type_id -> {decoration_type: value}
    member_decos = {}  # struct_id -> {member_idx -> {decoration_type: value}}
    type_names = {}
    current_type = None
    
    i = 5
    while i < len(words):
        w = words[i]
        op = w & 0xFFFF
        wc = w >> 16
        if wc == 0: break
        
        if op == 5:  # OpName
            tid = words[i+1]
            nb = b''.join(struct.pack('<I', words[i+j]) for j in range(2, min(wc, 40)))
            nb = nb.split(b'\x00')[0]
            try:
                type_names[tid] = nb.decode('ascii', errors='replace')
            except:
                pass
        
        elif op == 71:  # OpDecorate
            tid = words[i+1]
            dec = words[i+2]
            if dec not in type_decos:
                type_decos[tid] = {}
            if dec == 39:  # RowMajor
                type_decos[tid]['RowMajor'] = True
            elif dec == 40:  # ColMajor
                type_decos[tid]['ColMajor'] = True
            elif dec == 33:  # Binding
                type_decos[tid]['Binding'] = words[i+3]
            elif dec == 34:  # DescriptorSet
                type_decos[tid]['DescriptorSet'] = words[i+3]
            elif dec in (35, 36, 37, 38, 39, 40):
                type_decos[tid][f'Dec{dec}'] = words[i+3] if i+3 < len(words) else True
        
        elif op == 72:  # OpMemberDecorate
            sid = words[i+1]
            midx = words[i+2]
            dec = words[i+3]
            if sid not in member_decos:
                member_decos[sid] = {}
            if midx not in member_decos[sid]:
                member_decos[sid][midx] = {}
            if dec == 35:  # Offset
                member_decos[sid][midx]['offset'] = words[i+4]
            elif dec == 38:  # MatrixStride
                member_decos[sid][midx]['stride'] = words[i+4]
            elif dec == 39:  # RowMajor
                member_decos[sid][midx]['rowMajor'] = True
            elif dec == 40:  # ColMajor
                member_decos[sid][midx]['colMajor'] = True
        
        i += wc
    
    # Print all type decorations related to matrices
    print('\n  Matrix type decorations (RowMajor/ColMajor on types):')
    found_matrix_deco = False
    for tid, deco in sorted(type_decos.items()):
        if 'RowMajor' in deco or 'ColMajor' in deco:
            name = type_names.get(tid, f'%{tid}')
            rm = 'RowMajor' if deco.get('RowMajor') else ''
            cm = 'ColMajor' if deco.get('ColMajor') else ''
            print(f'    %{tid:4d} ({name}): {rm} {cm}')
            found_matrix_deco = True
    if not found_matrix_deco:
        print('    (none found — no explicit RowMajor/ColMajor at type level)')
    
    # Print member decorations for RowMajor/ColMajor
    print('\n  Struct member decorations (RowMajor/ColMajor on members):')
    found_member = False
    for sid, members in sorted(member_decos.items()):
        sname = type_names.get(sid, f'%{sid}')
        for midx, deco in sorted(members.items()):
            if 'rowMajor' in deco or 'colMajor' in deco:
                rm = 'RowMajor' if deco.get('rowMajor') else ''
                cm = 'ColMajor' if deco.get('colMajor') else ''
                off = deco.get('offset', '?')
                stride = deco.get('stride', '?')
                print(f'    %{sid} ({sname})[{midx}] offset={off} stride={stride}: {rm} {cm}')
                found_member = True
    if not found_member:
        print('    (none found — no explicit RowMajor/ColMajor at member level)')
    
    # Print ALL decorations on types named GlobalUniforms or matParamsTag
    print('\n  All decorations on GlobalUniforms/matParamsTag types:')
    for tid, name in type_names.items():
        if 'GlobalUniform' in name or 'matParams' in name or 'pushConstants' in name:
            print(f'\n    %{tid}: "{name}"')
            if tid in type_decos:
                for k, v in type_decos[tid].items():
                    print(f'      TypeDeco: {k} = {v}')
            if tid in member_decos:
                for midx, deco in sorted(member_decos[tid].items()):
                    print(f'      Member[{midx}]: {deco}')
