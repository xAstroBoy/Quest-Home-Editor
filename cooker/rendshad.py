import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__)); import fb
# RENDSHAD .surface = "SHAD"@4 + FlatBuffer; renderer scans for SPIR-V modules (0x07230203) + pairs vert/frag.
# Minimal container: root table.field0 = vector of stage tables; each stage.field0 = spirv ubyte vector.
def encode_surface(spv_modules):   # spv_modules: list of bytes (each a full SPIR-V module)
    b=fb.Builder(sum(len(m) for m in spv_modules)+2048)
    stage_offs=[]
    for code in spv_modules:
        dv=b.CreateByteVector(code)
        b.StartObject(1); b.AddOffset(0,dv); stage_offs.append(b.EndObject())
    vec=b.CreateOffsetVector(stage_offs)
    b.StartObject(1); b.AddOffset(0,vec); root=b.EndObject()
    b.Finish(root, b'SHAD'); return b.Output()
if __name__=='__main__':
    v=open('cooker/shaders/myunlit.vert.spv','rb').read(); f=open('cooker/shaders/myunlit.frag.spv','rb').read()
    out=encode_surface([v,f]); open('cooker/out/_test.surface','wb').write(out)
    # verify the renderer's scan finds both modules
    offs=[i for i in range(0,len(out)-4,4) if struct.unpack_from('<I',out,i)[0]==0x07230203]
    print('SHAD built %d bytes, magic@4=%r, SPIR-V modules found=%d'%(len(out),out[4:8],len(offs)))
