#!/usr/bin/env python
"""Build a getTime()-driven ARBITRARY-AXIS rotation shader from a stock V203 RENDSHAD — the GENERAL V79 node-
ROTATION port (Outer Wilds Y-spin skybox/planets/Interloper AND Dragon Ball / Snake Way's tilted-axis King Kai
planet). Same proven getTime() toolchain as the erebor wispscale / flipbook.

The forward VERTEX stage rotates inPos about a fixed axis by angle = OMEGA*globalUniforms.time, via Rodrigues:
  v' = v*cos(a) + (axis x v)*sin(a) + axis*(axis.v)*(1-cos(a))
axis + OMEGA (signed rad/s) are baked as constants — the cooker generates one shader per (omega,axis,blend) ON
DEMAND, so ANY rotation works (no per-env pre-baked shaders). The cooker bakes the mesh RELATIVE to the rotation
pivot + puts the entity at the pivot, so a node spins in place and its children orbit it.

Usage: python make_rotate_shader.py <OMEGA_rad_s> <AX> <AY> <AZ> <SRC.bin> <OUT.bin>
REPACK identical to make_wisp_scale_shader.py (append grown vertex module at EOF, repoint the stage uoffset).
"""
import struct, sys, math

OMEGA = float(sys.argv[1]) if len(sys.argv) > 1 else (2*math.pi/333.333)
AX    = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
AY    = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
AZ    = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
SRC   = sys.argv[5] if len(sys.argv) > 5 else "cooker/nuxd_unlit_shader.bin"
OUT   = sys.argv[6] if len(sys.argv) > 6 else "cooker/rot_gen.surface.bin"
_l = math.sqrt(AX*AX+AY*AY+AZ*AZ) or 1.0
AX, AY, AZ = AX/_l, AY/_l, AZ/_l

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
    assert fwdIdx>=0 and nStages==2*nPasses
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
    assert slot
    spv=list(struct.unpack_from("<%dI"%(bc//4),d,sv+4))
    bound=spv[3]; insts=[]; i=5
    while i<len(spv):
        wc=spv[i]>>16; op=spv[i]&0xFFFF
        if wc==0: break
        insts.append([op,list(spv[i:i+wc])]); i+=wc
    def wstr(ws): return struct.pack("<%dI"%len(ws),*ws).split(b"\x00")[0]
    names={}; mnames={}; tFloat=tInt=tV3=glsl=None; gu=inpos=None; intc={}; fltc={}; ptr={}
    for o,w in insts:
        if o==5: names[w[1]]=wstr(w[2:]).decode('ascii','ignore')
        elif o==6: mnames[(w[1],w[2])]=wstr(w[3:]).decode('ascii','ignore')
        elif o==11 and b"GLSL" in wstr(w[2:]): glsl=w[1]
        elif o==22 and w[2]==32: tFloat=w[1]
        elif o==21 and w[2]==32: tInt=w[1]
    for o,w in insts:
        if o==23 and w[2]==tFloat and w[3]==3: tV3=w[1]
        elif o==32: ptr[(w[2],w[3])]=w[1]
    for o,w in insts:
        if o==43 and w[1]==tInt: intc[struct.unpack('<i',struct.pack('<I',w[3]))[0]]=w[2]
        elif o==43 and w[1]==tFloat: fltc[round(struct.unpack('<f',struct.pack('<I',w[3]))[0],6)]=w[2]
    for k,v in names.items():
        if v=='globalUniforms': gu=k
        elif v=='inPos': inpos=k
    time_idx=next((idx for (sid,idx),n in mnames.items() if n=='time'),None)
    print("ids tFloat=%s tInt=%s tV3=%s glsl=%s gu=%s inPos=%s time_idx=%s"%(tFloat,tInt,tV3,glsl,gu,inpos,time_idx))
    assert all(x is not None for x in [tFloat,tInt,tV3,glsl,gu,inpos,time_idx])
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
    c_om=fconst(OMEGA); c_one=fconst(1.0)
    cax=fconst(AX); cay=fconst(AY); caz=fconst(AZ)
    c_time=iconst(time_idx); pUf=ptr_of(2,tFloat)
    axisVec=nid(); new_consts.append([0x2c,[0,tV3,axisVec,cax,cay,caz]])   # OpConstantComposite axis
    GLSL_SIN=13; GLSL_COS=14; GLSL_CROSS=68
    pt=nid(); t=nid(); a=nid(); cs=nid(); sn=nid(); omc=nid()
    dotav=nid(); crossv=nid(); term1=nid(); term2=nid(); k=nid(); term3=nid(); tmp=nid(); rot=nid()
    body=[
        [65,[0,pUf,pt,gu,c_time]],
        [61,[0,tFloat,t,pt]],
        [133,[0,tFloat,a,t,c_om]],                  # a = time*OMEGA
        [12,[0,tFloat,cs,glsl,GLSL_COS,a]],         # c
        [12,[0,tFloat,sn,glsl,GLSL_SIN,a]],         # s
        [131,[0,tFloat,omc,c_one,cs]],              # 1-c
    ]
    load_idx=load_res=None
    for ki,(o,w) in enumerate(insts):
        if o==61 and len(w)>=4 and w[3]==inpos:
            load_idx=ki; load_res=w[2]; break
    assert load_res is not None
    body += [
        [148,[0,tFloat,dotav,axisVec,load_res]],            # dot(axis,v)         OpDot
        [12,[0,tV3,crossv,glsl,GLSL_CROSS,axisVec,load_res]], # cross(axis,v)
        [142,[0,tV3,term1,load_res,cs]],                    # v*c                 OpVectorTimesScalar
        [142,[0,tV3,term2,crossv,sn]],                      # cross*s
        [133,[0,tFloat,k,dotav,omc]],                       # (axis.v)*(1-c)
        [142,[0,tV3,term3,axisVec,k]],                      # axis*k
        [129,[0,tV3,tmp,term1,term2]],                      # term1+term2
        [129,[0,tV3,rot,tmp,term3]],                        # + term3 = rotated
    ]
    out=[]; inserted=False; injected=False
    for ki,(o,w) in enumerate(insts):
        if o==54 and not inserted:
            out.extend(new_types); out.extend(new_consts); inserted=True
        out.append([o,w])
        if ki==load_idx and not injected:
            out.extend(body); injected=True
    assert inserted and injected
    seen=False                                              # rename load_res -> rot after the final OpFAdd(rot)
    for ent in out:
        o,w=ent
        if o==129 and len(w)>=4 and w[2]==rot: seen=True; continue
        if seen:
            for j in range(1,len(w)):
                if w[j]==load_res: w[j]=rot
    words=[0x07230203, spv[1], spv[2], bound, 0]
    for o,w in out:
        w=list(w); w[0]=(len(w)<<16)|o; words.extend(w)
    mod=struct.pack("<%dI"%len(words),*words)
    out_d=bytearray(d)
    while len(out_d)%4: out_d.append(0)
    nv=len(out_d); out_d+=struct.pack("<I",len(mod))+mod
    struct.pack_into("<I",out_d,slot,nv-slot)
    open(OUT,"wb").write(out_d)
    print("wrote %s (omega=%.5f rad/s axis=(%.3f,%.3f,%.3f) period=%.1fs bound %d)"%(OUT,OMEGA,AX,AY,AZ,2*math.pi/abs(OMEGA) if OMEGA else 0,bound))
    return 0

if __name__=="__main__":
    sys.exit(main())
