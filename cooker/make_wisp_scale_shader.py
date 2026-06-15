#!/usr/bin/env python
"""Build cooker/wispscale.surface.bin: the stock unlitblend V203 RENDSHAD with a getTime()-driven SCALE
BREATHING injected into the forward VERTEX stage — the FAITHFUL erebor wisp port.

The V79 wisps (Plane.001..012) are a 100-key node SCALE oscillation (~4.17s loop): each plane breathes
DOWN from its rest size and back, z compressing more than x/y (decoded from TheHobbit_EreborThrone.bin).
That is GEOMETRY scale, not a brightness/alpha pulse. VAT didn't render on device; ShellPoseAnimation is a
one-shot (no loop). So we reproduce the scale as a vertex-stage shader driven by globalUniforms.time (which
DOES reach shaders on device — the brightness pulse proved it) so it loops forever.

Injection: scale inPos in MODEL space right after it's loaded, BEFORE the existing worldFromModel transform.
The cooker bakes each wisp's geometry CENTERED on its centroid + puts the entity at the centroid, so scaling
model-space pos pivots in place. Per-wisp PHASE = worldFromModel translation.x (so they don't all sync).

  shrink = 0.5 - 0.5*cos(time*FREQ + tx*PHASE)     # 0..1
  s.xy   = 1 - AMP_XY*shrink ;  s.z = 1 - AMP_Z*shrink
  inPos *= s

REPACK identical to make_pulse_shader.py: grow the VERTEX module, append at EOF, patch that stage's uoffset.
"""
import struct, sys

SRC = "cooker/nuxd_unlitblend_shader.bin"
OUT = "cooker/wispscale.surface.bin"
FREQ   = 6.2831853/4.166667   # 2pi / loop seconds  -> one breathe per 4.17s (the V79 loop)
AMP_XY = 0.16                 # x/y shrink fraction (V79 dips ~0.83..1.0 of rest)
AMP_Z  = 0.24                 # z compresses more (V79 z dips lower)
PHASE  = 2.3                  # rad per world-metre of centroid.x -> de-syncs the 12 wisps

def main():
    d = bytearray(open(SRC, "rb").read()); N = len(d)
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

    # ---- locate forward VERTEX stage byte-vector (stage si = 2*fwdIdx + 0) ----
    root=u32(0); passesBase=stagesBase=nPasses=nStages=0; fwdIdx=-1
    for fi in range(vt_nf(root)):
        fp=vt_field(root,fi)
        if not fp or not u32(fp): continue
        vec=fp+u32(fp)
        if vec+4>N: continue
        cnt=u32(vec)
        if not (0<cnt<=64): continue
        base=vec+4
        if base+cnt*4>N: continue
        e0=base+u32(base)
        for ef in range(min(vt_nf(e0),4)):
            if str_at(vt_field(e0,ef)).startswith("forward"):
                passesBase,nPasses=base,cnt
                for pi in range(cnt):
                    pt=base+pi*4+u32(base+pi*4)
                    for pf in range(min(vt_nf(pt),4)):
                        if str_at(vt_field(pt,pf))=="forward": fwdIdx=pi
        for ef in range(min(vt_nf(e0),4)):
            sp=vt_field(e0,ef)
            if sp:
                sv=sp+u32(sp)
                if sv+4<=N and 500<u32(sv)<2000000 and sv+4+u32(sv)<=N:
                    stagesBase,nStages=base,cnt
    assert fwdIdx>=0 and nStages==2*nPasses, "forward pass not found"
    # pick whichever of the two forward stages is the VERTEX module (exec model 0)
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
    for si in (2*fwdIdx, 2*fwdIdx+1):
        sl2,v2,b2=stage_spv(si)
        spv2=struct.unpack_from("<%dI"%(b2//4),d,v2+4)
        # exec model
        em=-1; i=5
        while i<len(spv2):
            op=spv2[i]&0xffff; wc=spv2[i]>>16
            if wc==0: break
            if op==15: em=spv2[i+1]; break
            i+=wc
        if em==0: slot,sv,bc=sl2,v2,b2; break
    assert slot, "forward vertex spirv not found"
    print("forward vertex: uoff_slot@%d bytevec@%d count=%d"%(slot,sv,bc))
    spv=list(struct.unpack_from("<%dI"%(bc//4),d,sv+4))

    # ---- parse ----
    bound=spv[3]; insts=[]; i=5
    while i<len(spv):
        wc=spv[i]>>16; op=spv[i]&0xFFFF
        if wc==0: break
        insts.append([op,list(spv[i:i+wc])]); i+=wc
    def wstr(ws): return struct.pack("<%dI"%len(ws),*ws).split(b"\x00")[0]
    names={}; mnames={}; tFloat=tInt=tV3=glsl=None
    gu=pc=inpos=None; intc={}; fltc={}; ptr={}
    for o,w in insts:
        if o==5: names[w[1]]=wstr(w[2:]).decode('ascii','ignore')
        elif o==6: mnames[(w[1],w[2])]=wstr(w[3:]).decode('ascii','ignore')
        elif o==11 and b"GLSL" in wstr(w[2:]): glsl=w[1]
        elif o==22 and w[2]==32: tFloat=w[1]
        elif o==21 and w[2]==32: tInt=w[1]
    for o,w in insts:
        if o==23 and w[2]==tFloat and w[3]==3: tV3=w[1]
        elif o==32: ptr[(w[2],w[3])]=w[1]            # (storage,pointee)->id
    for o,w in insts:
        if o==43 and w[1]==tInt: intc[struct.unpack('<i',struct.pack('<I',w[3]))[0]]=w[2]
        elif o==43 and w[1]==tFloat: fltc[round(struct.unpack('<f',struct.pack('<I',w[3]))[0],6)]=w[2]
    for k,v in names.items():
        if v=='globalUniforms': gu=k
        elif v=='pushConstants': pc=k
        elif v=='inPos': inpos=k
    # member indices by name
    def member_idx(nm):
        for (sid,idx),n in mnames.items():
            if n==nm: return idx
        return None
    time_idx=member_idx('time'); wfm_idx=member_idx('worldFromModel')
    print("ids: tFloat=%s tInt=%s tV3=%s glsl=%s gu=%s pc=%s inPos=%s time_idx=%s wfm_idx=%s"%(
        tFloat,tInt,tV3,glsl,gu,pc,inpos,time_idx,wfm_idx))
    assert all(x is not None for x in [tFloat,tInt,tV3,glsl,gu,inpos,time_idx]), "missing core id"

    def nid():
        nonlocal bound; r=bound; bound+=1; return r
    fb=lambda f: struct.unpack("<I",struct.pack("<f",f))[0]
    new_consts=[]
    def fconst(val):
        v=round(val,6)
        if v in fltc: return fltc[v]
        cid=nid(); new_consts.append([43,[0,tFloat,cid,fb(val)]]); fltc[v]=cid; return cid
    def iconst(val):
        if val in intc: return intc[val]
        cid=nid(); new_consts.append([43,[0,tInt,cid,val&0xffffffff]]); intc[val]=cid; return cid
    new_types=[]
    def ptr_of(storage,pointee):
        if (storage,pointee) in ptr: return ptr[(storage,pointee)]
        pid=nid(); new_types.append([32,[0,pid,storage,pointee]]); ptr[(storage,pointee)]=pid; return pid

    c_one=fconst(1.0); c_half=fconst(0.5); c_freq=fconst(FREQ)
    c_axy=fconst(AMP_XY); c_az=fconst(AMP_Z); c_phase=fconst(PHASE)
    c_time=iconst(time_idx); c0=iconst(0)
    pUf=ptr_of(2,tFloat)        # Uniform float
    use_phase = (pc is not None and wfm_idx is not None)
    if use_phase:
        c3=iconst(3); pPCf=ptr_of(9,tFloat); c_wfm=iconst(wfm_idx)

    GLSL_COS=14
    # ids for the body
    pt_t=nid(); t=nid(); wt=nid()
    if use_phase:
        pt_tx=nid(); tx=nid(); ph=nid(); th0=nid(); th=nid()
    else:
        th=nid()
    cs=nid(); hc=nid(); shr=nid(); axys=nid(); sxy=nid(); azs=nid(); sz=nid(); svec=nid(); scaled=nid()
    body=[
        [65,[0,pUf,pt_t,gu,c_time]],          # &globalUniforms.time
        [61,[0,tFloat,t,pt_t]],               # time
        [133,[0,tFloat,wt,t,c_freq]],         # time*FREQ
    ]
    if use_phase:
        body += [
            [65,[0,pPCf,pt_tx,pc,c_wfm,c3,c0]],  # &worldFromModel[3].x  (member wfm, col3, row0)
            [61,[0,tFloat,tx,pt_tx]],            # tx
            [133,[0,tFloat,ph,tx,c_phase]],      # tx*PHASE
            [129,[0,tFloat,th,wt,ph]],           # theta = wt + phase
        ]
    else:
        body += [[129,[0,tFloat,th,wt,wt]]] if False else []
        th=wt
    body += [
        [12,[0,tFloat,cs,glsl,GLSL_COS,th]],     # cos(theta)
        [133,[0,tFloat,hc,cs,c_half]],           # 0.5*cos
        [131,[0,tFloat,shr,c_half,hc]],          # shrink = 0.5 - 0.5cos   (0..1)
        [133,[0,tFloat,axys,c_axy,shr]],         # AMP_XY*shrink
        [131,[0,tFloat,sxy,c_one,axys]],         # s.xy = 1 - AMP_XY*shrink
        [133,[0,tFloat,azs,c_az,shr]],           # AMP_Z*shrink
        [131,[0,tFloat,sz,c_one,azs]],           # s.z  = 1 - AMP_Z*shrink
        [80,[0,tV3,svec,sxy,sxy,sz]],            # vec3 s
        [133,[0,tV3,scaled,0,svec]],             # placeholder: inPos*s  (operand[3] filled below)
    ]
    # the inPos load result id we will multiply + rename downstream
    # find first OpLoad of inPos
    load_idx=load_res=None
    for k,(o,w) in enumerate(insts):
        if o==61 and len(w)>=4 and w[3]==inpos:
            load_idx=k; load_res=w[2]; break
    assert load_res is not None, "inPos load not found"
    body[-1][1][3]=load_res                       # scaled = OpFMul tV3 load_res svec  (set src vec)
    # OpFMul operands order: result-type, result-id, op1, op2  -> [tV3, scaled, load_res, svec]
    body[-1][1]=[0,tV3,scaled,load_res,svec]

    # ---- splice: new types+consts before first OpFunction(54); body right AFTER inPos load; rename downstream ----
    out=[]; inserted=False; injected=False
    for k,(o,w) in enumerate(insts):
        if o==54 and not inserted:
            out.extend(new_types); out.extend(new_consts); inserted=True
        out.append([o,w])
        if k==load_idx and not injected:
            out.extend(body); injected=True
    assert inserted and injected, "splice failed"
    # rename load_res -> scaled in every instruction AFTER the injected body
    seen_inject=False
    for ent in out:
        o,w=ent
        if o==80 and len(w)>=4 and w[2]==scaled:   # our CompositeConstruct of svec? no-op guard
            pass
        if o==133 and len(w)>=5 and w[2]==scaled:   # the OpFMul producing scaled = marker
            seen_inject=True; continue
        if seen_inject:
            for j in range(1,len(w)):
                if w[j]==load_res: w[j]=scaled

    # ---- re-serialize ----
    words=[0x07230203, spv[1], spv[2], bound, 0]
    for o,w in out:
        w=list(w); w[0]=(len(w)<<16)|o; words.extend(w)
    mod=struct.pack("<%dI"%len(words), *words)
    out_d=bytearray(d)
    while len(out_d)%4: out_d.append(0)
    new_vec=len(out_d); out_d += struct.pack("<I",len(mod))+mod
    struct.pack_into("<I",out_d,slot,new_vec-slot)
    open(OUT,"wb").write(out_d)
    print("wrote %s: %d bytes (orig vert %dB @%d orphaned; new %dB @%d, bound %d, phase=%s)"%(
        OUT,len(out_d),bc,sv,len(mod),new_vec,bound,use_phase))
    return 0

if __name__=="__main__":
    sys.exit(main())
