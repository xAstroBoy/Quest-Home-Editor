#!/usr/bin/env python
"""Build cooker/wisp_pulse.surface.bin: the stock unlitblend V203 RENDSHAD with a getTime()-driven
BRIGHTNESS PULSE injected into the forward fragment stage (flame-wisp animation).

erebor's wisps are flame billboards that pulsed via a V79 node SCALE track. VAT collapsed the geometry;
a skeleton crashed env-unload. The faithful, device-safe way is a tiny shader edit: multiply the frag
output by sin(time). unlitblend already exposes GlobalUniforms.time (member 12) + GLSL.std.450 and uses
the exact V203 descriptor interface, so we reuse it verbatim and only touch the forward frag SPIR-V.

REPACK: SPIR-V modules are byte-vectors in the SHAD FlatBuffer. We APPEND the grown module at EOF and
patch only that stage's uoffset slot + the vector count word (FlatBuffer offsets are relative → nothing
after the slot shifts; SPIR-V refs are by id → mid-module insertion is safe with a bumped id-bound).
"""
import struct, sys

SRC = "cooker/nuxd_unlitblend_shader.bin"
OUT = "cooker/wisp_pulse.surface.bin"
# pulse(time) = BIAS + AMP*sin(time*SPEED); brightness multiplier. [BIAS-AMP .. BIAS+AMP]
SPEED, AMP, BIAS = 2.2, 0.35, 0.7      # -> brightness in [0.35 .. 1.05]

def main():
    d = bytearray(open(SRC, "rb").read()); N = len(d)
    u16 = lambda o: struct.unpack_from("<H", d, o)[0] if 0 <= o and o + 2 <= N else 0
    i32 = lambda o: struct.unpack_from("<i", d, o)[0] if 0 <= o and o + 4 <= N else 0
    u32 = lambda o: struct.unpack_from("<I", d, o)[0] if 0 <= o and o + 4 <= N else 0
    def vt_field(tbl, fi):
        so = i32(tbl); vt = tbl - so
        if vt < 0 or vt + 4 > N: return 0
        vs = u16(vt); sl = vt + 4 + fi * 2
        if sl + 2 > vt + vs: return 0
        fo = u16(sl); return tbl + fo if fo else 0
    def vt_nf(tbl):
        so = i32(tbl); vt = tbl - so
        if vt < 0 or vt + 4 > N: return 0
        vs = u16(vt); return (vs - 4) // 2 if vs >= 4 else 0
    def str_at(p):
        if not p or p + 4 > N: return ""
        s = p + u32(p); ln = u32(s)
        return d[s+4:s+4+ln].decode("ascii","ignore") if 0 < ln <= 256 and s+4+ln <= N else ""

    # ---- locate forward frag stage byte-vector ----
    root = u32(0); passesBase=stagesBase=nPasses=nStages=0; fwdIdx=-1
    for fi in range(vt_nf(root)):
        fp = vt_field(root, fi)
        if not fp or not u32(fp): continue
        vec = fp + u32(fp)
        if vec+4>N: continue
        cnt = u32(vec)
        if not (0 < cnt <= 64): continue
        base = vec+4
        if base+cnt*4>N: continue
        e0 = base + u32(base)
        for ef in range(min(vt_nf(e0),4)):
            if str_at(vt_field(e0,ef)).startswith("forward"):
                passesBase,nPasses = base,cnt
                for pi in range(cnt):
                    pt = base+pi*4 + u32(base+pi*4)
                    for pf in range(min(vt_nf(pt),4)):
                        if str_at(vt_field(pt,pf))=="forward": fwdIdx=pi
        for ef in range(min(vt_nf(e0),4)):
            sp = vt_field(e0,ef)
            if sp:
                sv = sp+u32(sp)
                if sv+4<=N and 500<u32(sv)<2000000 and sv+4+u32(sv)<=N:
                    stagesBase,nStages = base,cnt
    assert fwdIdx>=0 and nStages==2*nPasses, "forward pass not found"
    si = 2*fwdIdx + 1                                   # frag stage
    se = stagesBase+si*4; st = se+u32(se)
    slot = sv = bc = 0
    for ef in range(min(vt_nf(st),6)):
        sp = vt_field(st,ef)
        if not sp: continue
        v = sp+u32(sp)
        if v+4<=N and 500<u32(v) and v+4+u32(v)<=N and u32(v)%4==0 and u32(v+4)==0x07230203:
            slot,sv,bc = sp, v, u32(v); break
    assert slot, "frag spirv not found"
    print(f"forward frag: uoff_slot@{slot} bytevec@{sv} count={bc}")
    spv = list(struct.unpack_from("<%dI"%(bc//4), d, sv+4))

    # ---- parse the frag SPIR-V into instructions ----
    bound = spv[3]
    insts = []; i = 5
    while i < len(spv):
        wc = spv[i] >> 16; op = spv[i] & 0xFFFF
        if wc == 0: break
        insts.append([op, list(spv[i:i+wc])]); i += wc
    def words_to_str(ws):
        return struct.pack("<%dI"%len(ws), *ws).split(b"\x00")[0]
    glsl=tfloat=tint=tv4=ptr_u_float=out_var=gu_struct=entry_fn=None
    decos={}
    for o,w in insts:
        if o==11 and b"GLSL.std.450" in words_to_str(w[2:]): glsl=w[1]
        elif o==15: entry_fn=w[2]
        elif o==22 and w[2]==32: tfloat=w[1]
        elif o==21 and w[2]==32: tint=w[1]
        elif o==6 and w[2]==12 and words_to_str(w[3:])==b"time": gu_struct=w[1]
        elif o==71: decos.setdefault(w[1],{})[w[2]] = (w[3] if len(w)>3 else None)
    for o,w in insts:
        if o==23 and w[2]==tfloat and w[3]==4: tv4=w[1]
        elif o==32 and w[2]==2 and w[3]==tfloat: ptr_u_float=w[1]
    # GlobalUniforms var: variable whose type is ptr(Uniform) -> gu_struct
    gu_ptr=None
    for o,w in insts:
        if o==32 and w[2]==2 and w[3]==gu_struct: gu_ptr=w[1]
    gu_var=None
    for o,w in insts:
        if o==59 and w[1]==gu_ptr and w[3]==2: gu_var=w[2]
    # output color var: OpVariable Output (sc=3) decorated Location 0
    for o,w in insts:
        if o==59 and w[3]==3 and decos.get(w[2],{}).get(30)==0: out_var=w[2]
    # existing int const 12?
    c12=None
    for o,w in insts:
        if o==43 and w[1]==tint and w[3]==12: c12=w[2]
    print(f"glsl={glsl} tfloat={tfloat} tint={tint} tv4={tv4} ptr_u_float={ptr_u_float} gu_struct={gu_struct} gu_var={gu_var} out_var={out_var} c12={c12} bound={bound}")
    assert all(x is not None for x in [glsl,tfloat,tint,tv4,ptr_u_float,gu_var,out_var,entry_fn]), "missing id"

    # ---- allocate new ids + constants ----
    def nid():
        nonlocal bound; r=bound; bound+=1; return r
    fbits = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]
    # NOTE: each new instruction's word list starts with a 0 PLACEHOLDER for word0 (the serializer
    # overwrites index 0 with (wordCount<<16)|opcode), so operand[0] must NOT be a real operand.
    new_consts=[]
    if c12 is None:
        c12=nid(); new_consts.append([43,[0,tint,c12,12]])
    cSpeed=nid(); new_consts.append([43,[0,tfloat,cSpeed,fbits(SPEED)]])
    cAmp=nid();   new_consts.append([43,[0,tfloat,cAmp,fbits(AMP)]])
    cBias=nid();  new_consts.append([43,[0,tfloat,cBias,fbits(BIAS)]])
    # pulse body (inserted before main's OpReturn): outColor *= BIAS+AMP*sin(time*SPEED)
    tptr=nid(); t=nid(); wt=nid(); sn=nid(); a=nid(); p=nid(); v=nid(); sv2=nid()
    GLSL_SIN=13
    body=[
        [65,[0,ptr_u_float,tptr,gu_var,c12]],      # OpAccessChain -> &GlobalUniforms.time
        [61,[0,tfloat,t,tptr]],                    # OpLoad time
        [133,[0,tfloat,wt,t,cSpeed]],              # time*SPEED
        [12,[0,tfloat,sn,glsl,GLSL_SIN,wt]],       # sin(...)
        [133,[0,tfloat,a,sn,cAmp]],                # *AMP
        [129,[0,tfloat,p,a,cBias]],                # +BIAS
        [61,[0,tv4,v,out_var]],                    # load current outColor
        [142,[0,tv4,sv2,v,p]],                     # *= pulse
        [62,[0,out_var,sv2]],                      # store back
    ]

    # ---- splice: constants before first OpFunction(54); body before main's OpReturn(253) ----
    out=[]
    inserted_c=False; in_main=False; done_body=False
    for k,(o,w) in enumerate(insts):
        if o==54 and not inserted_c:                    # first OpFunction
            out.extend(new_consts); inserted_c=True
        if o==54 and w[2]==entry_fn: in_main=True
        if o==56: in_main=False                          # OpFunctionEnd
        if in_main and o in (253,254) and not done_body: # OpReturn/Value in main
            out.extend(body); done_body=True
        out.append([o,w])
    assert inserted_c and done_body, "splice failed"

    # ---- re-serialize module ----
    words=[0x07230203, spv[1], spv[2], bound, 0]
    for o,w in out:
        w=list(w); w[0]=(len(w)<<16)|o; words.extend(w)
    mod=struct.pack("<%dI"%len(words), *words)

    # ---- append at EOF (4-aligned) as [count][module]; patch slot uoffset + nothing else shifts ----
    out_d=bytearray(d)
    while len(out_d)%4: out_d.append(0)
    new_vec=len(out_d)
    out_d += struct.pack("<I", len(mod)) + mod
    struct.pack_into("<I", out_d, slot, new_vec - slot)   # repoint stage frag uoffset
    open(OUT,"wb").write(out_d)
    print(f"wrote {OUT}: {len(out_d)} bytes (orig frag {bc}B @{sv} orphaned; new {len(mod)}B @{new_vec}, bound {bound})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
