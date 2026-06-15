#!/usr/bin/env python
"""Build cooker/flipbook.surface.bin: the stock unlitblend V203 RENDSHAD with a getTime()-driven SPRITESHEET
FLIPBOOK injected into the forward VERTEX stage — the faithful V79 "Sprite_3x3" port.

V79 sonic_schoolhouse posters = 9 fully-overlapping quads, each UV-mapped to one cell of a 3x3 spritesheet,
each skinned to one bone; the armature toggles bone scale.Y 1->0 to show ONE frame per 0.2s (9 frames /
1.8s = 5fps). That is a classic flipbook. We collapse the 9 quads to ONE quad with UV 0..1 (cooker side) and
this shader offsets inUv to the current cell from globalUniforms.time:

  frame = mod(floor(time*FPS), NFRAMES)
  col   = mod(frame, NCOLS) ;  row = floor(frame / NCOLS)
  uvOut = inUv * (1/NCOLS, 1/NROWS) + (col/NCOLS, row/NROWS)

Alpha-blend base (unlitblend) so the transparent poster backgrounds composite over the wall. getTime() loops
forever (no skeleton, no per-frame mesh). REPACK identical to make_pulse/make_wisp_scale_shader.
"""
import struct, sys

SRC = "cooker/nuxd_unlitblend_shader.bin"
OUT = "cooker/flipbook.surface.bin"
NCOLS, NROWS, NFRAMES, FPS = 3, 3, 9, 5.0   # sonic_schoolhouse 3x3, 9 frames @ 5fps (0.2s/frame, 1.8s loop)

def main():
    d=bytearray(open(SRC,"rb").read()); N=len(d)
    u16=lambda o: struct.unpack_from("<H",d,o)[0] if 0<=o and o+2<=N else 0
    i32=lambda o: struct.unpack_from("<i",d,o)[0] if 0<=o and o+4<=N else 0
    u32=lambda o: struct.unpack_from("<I",d,o)[0] if 0<=o and o+4<=N else 0
    def vt_field(tbl,fi):
        so=i32(tbl); vt=tbl-so
        if vt<0 or vt+4>N: return 0
        vs=u16(vt); sl=vt+4+fi*2
        if sl+2>vt+vs: return 0
        fo=u16(sl); return tbl+fo if fo else 0
    def vt_nf(tbl):
        so=i32(tbl); vt=tbl-so
        if vt<0 or vt+4>N: return 0
        vs=u16(vt); return (vs-4)//2 if vs>=4 else 0
    def str_at(p):
        if not p or p+4>N: return ""
        s=p+u32(p); ln=u32(s)
        return d[s+4:s+4+ln].decode("ascii","ignore") if 0<ln<=256 and s+4+ln<=N else ""
    # locate forward pass + stages
    root=u32(0); stagesBase=nPasses=nStages=0; fwdIdx=-1
    for fi in range(vt_nf(root)):
        fp=vt_field(root,fi)
        if not fp or not u32(fp): continue
        vec=fp+u32(fp)
        if vec+4>N: continue
        cnt=u32(vec)
        if not(0<cnt<=64): continue
        base=vec+4
        if base+cnt*4>N: continue
        e0=base+u32(base)
        for ef in range(min(vt_nf(e0),4)):
            if str_at(vt_field(e0,ef)).startswith("forward"):
                nPasses=cnt
                for pi in range(cnt):
                    pt=base+pi*4+u32(base+pi*4)
                    for pf in range(min(vt_nf(pt),4)):
                        if str_at(vt_field(pt,pf))=="forward": fwdIdx=pi
        for ef in range(min(vt_nf(e0),4)):
            sp=vt_field(e0,ef)
            if sp:
                sv=sp+u32(sp)
                if sv+4<=N and 500<u32(sv)<2000000 and sv+4+u32(sv)<=N:
                    stagesBase=base; nStages=cnt
    assert fwdIdx>=0 and nStages==2*nPasses, "forward pass not found"
    def stage_spv(si):
        se=stagesBase+si*4; st=se+u32(se)
        for ef in range(min(vt_nf(st),6)):
            sp=vt_field(st,ef)
            if not sp: continue
            v=sp+u32(sp)
            if v+4<=N and 500<u32(v) and v+4+u32(v)<=N and u32(v)%4==0 and u32(v+4)==0x07230203:
                return sp,v,u32(v)
        return 0,0,0
    slot=sv=bc=0
    for si in (2*fwdIdx,2*fwdIdx+1):
        sl2,v2,b2=stage_spv(si)
        spv2=struct.unpack_from("<%dI"%(b2//4),d,v2+4); em=-1; i=5
        while i<len(spv2):
            op=spv2[i]&0xffff; wc=spv2[i]>>16
            if wc==0: break
            if op==15: em=spv2[i+1]; break
            i+=wc
        if em==0: slot,sv,bc=sl2,v2,b2; break
    assert slot, "forward vertex not found"
    spv=list(struct.unpack_from("<%dI"%(bc//4),d,sv+4))
    bound=spv[3]; insts=[]; i=5
    while i<len(spv):
        wc=spv[i]>>16; op=spv[i]&0xFFFF
        if wc==0: break
        insts.append([op,list(spv[i:i+wc])]); i+=wc
    def wstr(ws): return struct.pack("<%dI"%len(ws),*ws).split(b"\x00")[0]
    names={}; mnames={}; tFloat=tInt=tV2=glsl=None; gu=inuv=None; intc={}; fltc={}; ptr={}
    for o,w in insts:
        if o==5: names[w[1]]=wstr(w[2:]).decode('ascii','ignore')
        elif o==6: mnames[(w[1],w[2])]=wstr(w[3:]).decode('ascii','ignore')
        elif o==11 and b"GLSL" in wstr(w[2:]): glsl=w[1]
        elif o==22 and w[2]==32: tFloat=w[1]
        elif o==21 and w[2]==32: tInt=w[1]
    for o,w in insts:
        if o==23 and w[2]==tFloat and w[3]==2: tV2=w[1]
        elif o==32: ptr[(w[2],w[3])]=w[1]
    for o,w in insts:
        if o==43 and w[1]==tInt: intc[struct.unpack('<i',struct.pack('<I',w[3]))[0]]=w[2]
        elif o==43 and w[1]==tFloat: fltc[round(struct.unpack('<f',struct.pack('<I',w[3]))[0],6)]=w[2]
    for k,v in names.items():
        if v=='globalUniforms': gu=k
        elif v=='inUv': inuv=k
    time_idx=next((idx for (sid,idx),n in mnames.items() if n=='time'),None)
    print("ids: tFloat=%s tInt=%s tV2=%s glsl=%s gu=%s inUv=%s time_idx=%s"%(tFloat,tInt,tV2,glsl,gu,inuv,time_idx))
    assert all(x is not None for x in [tFloat,tInt,tV2,glsl,gu,inuv,time_idx]), "missing id"

    def nid():
        nonlocal bound; r=bound; bound+=1; return r
    fb=lambda f: struct.unpack("<I",struct.pack("<f",f))[0]
    new_consts=[]; new_types=[]
    def fconst(val):
        v=round(val,6)
        if v in fltc: return fltc[v]
        cid=nid(); new_consts.append([43,[0,tFloat,cid,fb(val)]]); fltc[v]=cid; return cid
    def iconst(val):
        if val in intc: return intc[val]
        cid=nid(); new_consts.append([43,[0,tInt,cid,val&0xffffffff]]); intc[val]=cid; return cid
    def ptr_of(st,po):
        if (st,po) in ptr: return ptr[(st,po)]
        pid=nid(); new_types.append([32,[0,pid,st,po]]); ptr[(st,po)]=pid; return pid

    c_fps=fconst(FPS); c_nf=fconst(float(NFRAMES)); c_nc=fconst(float(NCOLS))
    c_invc=fconst(1.0/NCOLS); c_invr=fconst(1.0/NROWS)
    c_time=iconst(time_idx); pUf=ptr_of(2,tFloat)
    GLSL_FLOOR=8
    # body ids
    pt=nid(); t=nid(); tf=nid(); fr=nid(); fm=nid(); col=nid(); rdiv=nid(); row=nid()
    uo=nid(); vo=nid(); cell=nid(); off=nid(); scaled=nid(); uvout=nid()
    body=[
        [65,[0,pUf,pt,gu,c_time]],
        [61,[0,tFloat,t,pt]],
        [133,[0,tFloat,tf,t,c_fps]],            # time*FPS
        [12,[0,tFloat,fr,glsl,GLSL_FLOOR,tf]],  # floor()
        [141,[0,tFloat,fm,fr,c_nf]],            # OpFMod frame, NFRAMES
        [141,[0,tFloat,col,fm,c_nc]],           # col = mod(fm, NCOLS)
        [133,[0,tFloat,rdiv,fm,c_invc]],        # fm/NCOLS
        [12,[0,tFloat,row,glsl,GLSL_FLOOR,rdiv]], # row = floor(fm/NCOLS)
        [133,[0,tFloat,uo,col,c_invc]],         # uOff = col/NCOLS
        [133,[0,tFloat,vo,row,c_invr]],         # vOff = row/NROWS
        [80,[0,tV2,cell,c_invc,c_invr]],        # vec2(1/NCOLS,1/NROWS)
        [80,[0,tV2,off,uo,vo]],                 # vec2(uOff,vOff)
        [133,[0,tV2,scaled,0,cell]],            # inUv*cell  (src filled below)
        [129,[0,tV2,uvout,scaled,off]],         # + off
    ]
    # find first OpLoad inUv
    load_idx=load_res=None
    for k,(o,w) in enumerate(insts):
        if o==61 and len(w)>=4 and w[3]==inuv:
            load_idx=k; load_res=w[2]; break
    assert load_res is not None, "inUv load not found"
    body[-2][1]=[0,tV2,scaled,load_res,cell]    # scaled = inUv * cell
    out=[]; inserted=False; injected=False
    for k,(o,w) in enumerate(insts):
        if o==54 and not inserted:
            out.extend(new_types); out.extend(new_consts); inserted=True
        out.append([o,w])
        if k==load_idx and not injected:
            out.extend(body); injected=True
    assert inserted and injected
    # rename load_res -> uvout AFTER the injected body (marker = the OpFAdd producing uvout)
    seen=False
    for ent in out:
        o,w=ent
        if o==129 and len(w)>=4 and w[2]==uvout:
            seen=True; continue
        if seen:
            for j in range(1,len(w)):
                if w[j]==load_res: w[j]=uvout
    words=[0x07230203, spv[1], spv[2], bound, 0]
    for o,w in out:
        w=list(w); w[0]=(len(w)<<16)|o; words.extend(w)
    mod=struct.pack("<%dI"%len(words),*words)
    out_d=bytearray(d)
    while len(out_d)%4: out_d.append(0)
    nv=len(out_d); out_d+=struct.pack("<I",len(mod))+mod
    struct.pack_into("<I",out_d,slot,nv-slot)
    open(OUT,"wb").write(out_d)
    print("wrote %s: %d bytes (orig vert %dB@%d orphaned; new %dB@%d bound %d) grid %dx%d %dframes @%gfps"%(
        OUT,len(out_d),bc,sv,len(mod),nv,bound,NCOLS,NROWS,NFRAMES,FPS))
    return 0

if __name__=="__main__":
    sys.exit(main())
