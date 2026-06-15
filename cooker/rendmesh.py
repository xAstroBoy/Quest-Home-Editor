import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import fb

def f32_to_f16(f):  # numpy-free half encode
    import numpy as np; return int(np.float16(f).view(np.uint16))

def encode_rendmesh(positions, uvs, indices):
    # positions: [(x,y,z)], uvs: [(u,v)] same len, indices: [int]
    nv=len(positions); assert len(uvs)==nv
    stride=16  # pos f32x3 (12) + uv f16x2 (4) — matches renderer's parseRendMesh decode
    vb=bytearray()
    for (x,y,z),(u,v) in zip(positions,uvs):
        vb+=struct.pack('<fff',x,y,z); vb+=struct.pack('<HH',f32_to_f16(u),f32_to_f16(v))
    ib=b''.join(struct.pack('<H',i) for i in indices)
    attrs=[(0,0x32,0),(5,0x21,0)]   # POSITION f32x3, TEXCOORD0 f16x2
    attr_raw=b''.join(struct.pack('<BBBB',s,f,i,0) for (s,f,i) in attrs)
    b=fb.Builder(max(1024,len(vb)+len(ib)+512))
    vb_off=b.CreateByteVector(bytes(vb)); ib_off=b.CreateByteVector(ib)
    attr_off=b.CreateStructVector(attr_raw,4,len(attrs))
    b.StartObject(4); b.AddScalar('I',1,nv); b.AddOffset(2,vb_off); b.AddOffset(3,attr_off); vs=b.EndObject()
    vsvec=b.CreateOffsetVector([vs])
    b.StartObject(2); b.AddOffset(0,vsvec); b.AddOffset(1,ib_off); part=b.EndObject()
    partvec=b.CreateOffsetVector([part])
    b.StartObject(1); b.AddOffset(0,partvec); lod=b.EndObject()
    lodvec=b.CreateOffsetVector([lod])
    b.StartObject(2); b.AddOffset(1,lodvec); root=b.EndObject()
    b.Finish(root,b'MESH'); return b.Output()

# ---- round-trip via the renderer's parseRendMesh logic ----
def parse_rendmesh(d):
    u32=lambda o: struct.unpack_from('<I',d,o)[0]; i32=lambda o: struct.unpack_from('<i',d,o)[0]; u16=lambda o: struct.unpack_from('<H',d,o)[0]
    assert d[4:8]==b'MESH', d[4:8]
    def fieldFo(tp,fi):
        soff=i32(tp); vt=tp-soff; vtsz=u16(vt); idx=vt+4+fi*2
        return u16(idx) if idx+2<=vt+vtsz else 0
    def followVec(tp,fi):
        fo=fieldFo(tp,fi)
        if not fo: return None
        fs=tp+fo; uo=u32(fs); vp=fs+uo; return (vp+4,u32(vp))
    def followElem(eb,i): slot=eb+i*4; return slot+u32(slot)
    def followByteVec(tp,fi):
        r=followVec(tp,fi); return r
    root=u32(0)
    lodBase,lodCount=followVec(root,1); lod=followElem(lodBase,0)
    partBase,pc=followVec(lod,0); part=followElem(partBase,0)
    vsBase,vsc=followVec(part,0); vs=followElem(vsBase,0)
    ibStart,ibCount=followByteVec(part,1)
    vcfo=fieldFo(vs,1); nv=u32(vs+vcfo)
    vbStart,vbCount=followByteVec(vs,2); stride=vbCount//nv
    fb2,fc2=followVec(vs,3)
    pos=[struct.unpack_from('<fff',d,vbStart+i*stride) for i in range(nv)]
    nidx=ibCount//2; idx=[u16(ibStart+i*2) for i in range(nidx)]
    return dict(nv=nv,stride=stride,pos=pos,idx=idx,nattr=fc2,vbCount=vbCount,ibCount=ibCount)

if __name__=='__main__':
    P=[(-1,0,-1),(1,0,-1),(1,0,1),(-1,0,1)]; U=[(0,0),(1,0),(1,1),(0,1)]; I=[0,1,2,0,2,3]
    blob=encode_rendmesh(P,U,I); m=parse_rendmesh(blob)
    ok = m['nv']==4 and m['stride']==16 and m['idx']==I and all(abs(a-b)<1e-4 for pa,pb in zip(m['pos'],P) for a,b in zip(pa,pb)) and m['nattr']==2
    print('RENDMESH round-trip:', 'OK nv=%d stride=%d idx=%s nattr=%d (%d bytes)'%(m['nv'],m['stride'],m['idx'],m['nattr'],len(blob)) if ok else 'FAIL '+str(m))
