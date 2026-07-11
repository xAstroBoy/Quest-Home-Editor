#!/usr/bin/env python3
"""Generate the self-contained V79 'unlit transparent' shader (vert+frag) as a C header.

WHY: raw V79 .gltf.ovrscene envs (Outer Wilds planet billboards, SpongeBob jellyfish,
foliage cutouts, smoke) carry REAL transparency in the base-color texture's ALPHA
channel. Borrowing a v200+ system shader (isotropictiled etc.) to draw them outputs
opacity = 1.0 (it treats baseColor.a as METALLIC), so SRC_ALPHA blending has nothing to
blend -> transparent backgrounds rendered as opaque black squares. libshell's V79 path
uses its OWN shader that outputs the sampled texture's rgb + a. We replicate that: a tiny
unlit textured shader whose fragment outputs vec4(tex.rgb, tex.a).

INTERFACE (must match vk_renderer.h introspection + binding):
  vertex:   in loc0 vec3 'position' (role pos), in loc1 vec2 'uv' (role uv)
            set0 bind0 UBO { mat4 mvp }  -- renderer fills it with clipFromWorld0 = P*V;
            V79 static meshes bake the world transform into positions and use an identity
            model, so gl_Position = mvp * vec4(position,1) is correct with NO push const.
            out loc0 vec2 vUV
  fragment: set0 bind1 sampler 'samp'                 (renderer's shared sampler)
            set2 bind1 texture2D 'baseColorTex'        (renderer binds the mesh texture)
            in loc0 vec2 vUV ; out loc0 vec4 outColor = texture(baseColorTex, vUV)

Both stages are hand-assembled here so every word count / id is under our control and
structurally validated. Single-view (no MultiView); the renderpass viewMask=0x1 resolves
to view 0. Semantic correctness is verified by running the renderer (pipeline create +
screenshot).
"""

import struct, os

# ── SPIR-V opcode constants ─────────────────────────────────────────────────────
OpCapability=17; OpMemoryModel=14; OpEntryPoint=15; OpExecutionMode=16
OpName=5; OpMemberName=6; OpDecorate=71; OpMemberDecorate=72
OpTypeVoid=19; OpTypeFunction=33; OpTypeFloat=22; OpTypeInt=21
OpTypeVector=23; OpTypeMatrix=24; OpTypeStruct=30
OpTypeImage=25; OpTypeSampler=26; OpTypeSampledImage=27; OpTypePointer=32
OpConstant=43; OpVariable=59
OpFunction=54; OpFunctionEnd=56; OpLabel=248; OpReturn=253
OpLoad=61; OpStore=62; OpAccessChain=65
OpCompositeExtract=81; OpCompositeConstruct=80
OpMatrixTimesVector=145; OpSampledImage=86; OpImageSampleImplicitLod=87
OpTypeBool=20; OpFOrdLessThan=184; OpSelectionMerge=247; OpBranchConditional=250; OpKill=252
OpFMul=133; OpFAdd=129; OpVectorTimesScalar=142

def _strw(s):
    sb=s.encode('utf-8')+b'\x00'
    while len(sb)%4: sb+=b'\x00'
    return list(struct.unpack('<%dI'%(len(sb)//4), sb))

def _f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]

class Mod:
    def __init__(self, bound):
        self.w=[0x07230203, 0x00010000, 0x00000000, bound, 0]
    def ins(self, op, *operands):
        flat=[]
        for o in operands: flat.extend(o) if isinstance(o,(list,tuple)) else flat.append(o)
        self.w.append(((len(flat)+1)<<16)|op); self.w.extend(flat)
    def words(self): return self.w

# Decoration / builtin enums
DEC_BLOCK=2; DEC_COLMAJOR=5; DEC_MATSTRIDE=7; DEC_BUILTIN=11; DEC_LOCATION=30
DEC_BINDING=33; DEC_DESCSET=34; DEC_OFFSET=35
BUILTIN_POSITION=0
SC_UNIFORMCONSTANT=0; SC_INPUT=1; SC_UNIFORM=2; SC_OUTPUT=3; SC_PUSHCONSTANT=9

def build_vert(flip_v=False):
    # ids
    MAIN=1; VOID=2; FNVOID=3; FLOAT=4; V2=5; V3=6; V4=7; MAT4=8; UINT=9
    UBOSTR=10; P_U_UBO=11; P_U_MAT4=12; P_IN_V3=13; P_IN_V2=14; P_OUT_V2=15; P_OUT_V4=16
    vUBO=17; vPOS=18; vUVIN=19; vUVOUT=20; vGLPOS=21
    C1=22; C0U=23
    LBL=24; ldPOS=25; mvpPtr=26; ldMVP=27; px=28; py=29; pz=30; pos4=31; clip=32; ldUV=33
    # push-constant model matrix @byte0 (the editor's per-object transform; identity at rest, so
    # un-edited world-baked meshes are UNCHANGED). gl_Position = mvp * model * pos -> gizmo edits
    # (move/rotate/scale) actually move the rendered object.
    PCV=34; P_PC_PCV=35; P_PC_MAT4=36; vPCv=37; modelPtr=38; ldMODEL=39; worldPos=40
    # per-vertex COLOR (loc2) -> varying vColor (loc1). Carries the CPU-baked diffuse-IBL irradiance
    # for SpecIbl meshes (white for everything else, so it's a no-op there). The frag multiplies it in.
    vCOLIN=41; vCOLOUT=42; P_IN_V4=43; ldCOL=44
    # per-vertex NORMAL (loc3) -> varying vNormal (loc2). The frag samples the specular reflection
    # cube at this direction (a matcap-style env reflection) for SpecIbl metal/glass. Pass-through:
    # OPA static meshes bake world-space normals, so no model transform needed for the reflection dir.
    vNRMIN=45; vNRMOUT=46; P_OUT_V3=47; ldNRM=48
    # per-pixel LIGHTMAP uv1: vertex input 'lightmapUv' (renderer role 3) -> varying 'vUV1' (loc3).
    # The frag samples lightmapTex at vUV1 and multiplies (libshell ShellEnv = diffuse·lightmap·lightmappower).
    vUV1IN=49; vUV1OUT=50; ldUV1=51
    BOUND=52
    m=Mod(BOUND)
    m.ins(OpCapability, 1)                                   # Shader
    m.ins(OpMemoryModel, 0, 1)                               # Logical GLSL450
    m.ins(OpEntryPoint, 0, MAIN, _strw("main"), vPOS, vUVIN, vCOLIN, vNRMIN, vUV1IN, vUVOUT, vCOLOUT, vNRMOUT, vUV1OUT, vGLPOS)  # Vertex
    # names
    m.ins(OpName, MAIN, _strw("main"))
    m.ins(OpName, vPOS, _strw("position"))
    m.ins(OpName, vUVIN, _strw("uv"))
    m.ins(OpName, vCOLIN, _strw("vertColor"))
    m.ins(OpName, vNRMIN, _strw("normal"))
    m.ins(OpName, vUVOUT, _strw("vUV"))
    m.ins(OpName, vCOLOUT, _strw("vColor"))
    m.ins(OpName, vNRMOUT, _strw("vNormal"))
    m.ins(OpName, vUV1IN, _strw("lightmapUv"))
    m.ins(OpName, vUV1OUT, _strw("vUV1"))
    m.ins(OpName, vUBO, _strw("ubo"))
    # decorations
    m.ins(OpDecorate, vPOS, DEC_LOCATION, 0)
    m.ins(OpDecorate, vUVIN, DEC_LOCATION, 1)
    m.ins(OpDecorate, vCOLIN, DEC_LOCATION, 2)
    m.ins(OpDecorate, vNRMIN, DEC_LOCATION, 3)
    m.ins(OpDecorate, vUVOUT, DEC_LOCATION, 0)
    m.ins(OpDecorate, vCOLOUT, DEC_LOCATION, 1)
    m.ins(OpDecorate, vNRMOUT, DEC_LOCATION, 2)
    m.ins(OpDecorate, vUV1IN, DEC_LOCATION, 4)
    m.ins(OpDecorate, vUV1OUT, DEC_LOCATION, 3)
    m.ins(OpDecorate, vGLPOS, DEC_BUILTIN, BUILTIN_POSITION)
    m.ins(OpMemberDecorate, UBOSTR, 0, DEC_OFFSET, 0)
    m.ins(OpMemberDecorate, UBOSTR, 0, DEC_COLMAJOR)
    m.ins(OpMemberDecorate, UBOSTR, 0, DEC_MATSTRIDE, 16)
    m.ins(OpDecorate, UBOSTR, DEC_BLOCK)
    m.ins(OpDecorate, vUBO, DEC_DESCSET, 0)
    m.ins(OpDecorate, vUBO, DEC_BINDING, 0)
    # push-constant block { mat4 model; } @byte offset 0
    m.ins(OpMemberDecorate, PCV, 0, DEC_OFFSET, 0)
    m.ins(OpMemberDecorate, PCV, 0, DEC_COLMAJOR)
    m.ins(OpMemberDecorate, PCV, 0, DEC_MATSTRIDE, 16)
    m.ins(OpDecorate, PCV, DEC_BLOCK)
    # types
    m.ins(OpTypeVoid, VOID)
    m.ins(OpTypeFunction, FNVOID, VOID)
    m.ins(OpTypeFloat, FLOAT, 32)
    m.ins(OpTypeVector, V2, FLOAT, 2)
    m.ins(OpTypeVector, V3, FLOAT, 3)
    m.ins(OpTypeVector, V4, FLOAT, 4)
    m.ins(OpTypeMatrix, MAT4, V4, 4)
    m.ins(OpTypeInt, UINT, 32, 0)
    m.ins(OpTypeStruct, UBOSTR, MAT4)
    m.ins(OpTypeStruct, PCV, MAT4)                          # push-constant struct { mat4 model; }
    m.ins(OpTypePointer, P_U_UBO, SC_UNIFORM, UBOSTR)
    m.ins(OpTypePointer, P_U_MAT4, SC_UNIFORM, MAT4)
    m.ins(OpTypePointer, P_PC_PCV, SC_PUSHCONSTANT, PCV)
    m.ins(OpTypePointer, P_PC_MAT4, SC_PUSHCONSTANT, MAT4)
    m.ins(OpTypePointer, P_IN_V3, SC_INPUT, V3)
    m.ins(OpTypePointer, P_IN_V2, SC_INPUT, V2)
    m.ins(OpTypePointer, P_IN_V4, SC_INPUT, V4)
    m.ins(OpTypePointer, P_OUT_V2, SC_OUTPUT, V2)
    m.ins(OpTypePointer, P_OUT_V4, SC_OUTPUT, V4)
    m.ins(OpTypePointer, P_OUT_V3, SC_OUTPUT, V3)
    # constants
    m.ins(OpConstant, FLOAT, C1, _f32(1.0))
    m.ins(OpConstant, UINT, C0U, 0)
    # variables
    m.ins(OpVariable, P_U_UBO, vUBO, SC_UNIFORM)
    m.ins(OpVariable, P_PC_PCV, vPCv, SC_PUSHCONSTANT)
    m.ins(OpVariable, P_IN_V3, vPOS, SC_INPUT)
    m.ins(OpVariable, P_IN_V2, vUVIN, SC_INPUT)
    m.ins(OpVariable, P_IN_V4, vCOLIN, SC_INPUT)
    m.ins(OpVariable, P_IN_V3, vNRMIN, SC_INPUT)
    m.ins(OpVariable, P_OUT_V2, vUVOUT, SC_OUTPUT)
    m.ins(OpVariable, P_OUT_V4, vCOLOUT, SC_OUTPUT)
    m.ins(OpVariable, P_OUT_V3, vNRMOUT, SC_OUTPUT)
    m.ins(OpVariable, P_IN_V2, vUV1IN, SC_INPUT)
    m.ins(OpVariable, P_OUT_V2, vUV1OUT, SC_OUTPUT)
    m.ins(OpVariable, P_OUT_V4, vGLPOS, SC_OUTPUT)
    # function
    m.ins(OpFunction, VOID, MAIN, 0, FNVOID)
    m.ins(OpLabel, LBL)
    m.ins(OpLoad, V3, ldPOS, vPOS)
    m.ins(OpAccessChain, P_U_MAT4, mvpPtr, vUBO, C0U)
    m.ins(OpLoad, MAT4, ldMVP, mvpPtr)
    m.ins(OpCompositeExtract, FLOAT, px, ldPOS, 0)
    m.ins(OpCompositeExtract, FLOAT, py, ldPOS, 1)
    m.ins(OpCompositeExtract, FLOAT, pz, ldPOS, 2)
    m.ins(OpCompositeConstruct, V4, pos4, px, py, pz, C1)
    # worldPos = model * pos ; gl_Position = mvp * worldPos  (model=identity at rest)
    m.ins(OpAccessChain, P_PC_MAT4, modelPtr, vPCv, C0U)
    m.ins(OpLoad, MAT4, ldMODEL, modelPtr)
    m.ins(OpMatrixTimesVector, V4, worldPos, ldMODEL, pos4)
    m.ins(OpMatrixTimesVector, V4, clip, ldMVP, worldPos)
    m.ins(OpStore, vGLPOS, clip)
    m.ins(OpLoad, V2, ldUV, vUVIN)
    m.ins(OpStore, vUVOUT, ldUV)
    m.ins(OpLoad, V4, ldCOL, vCOLIN)        # pass per-vertex color (diffuse-IBL) to the fragment
    m.ins(OpStore, vCOLOUT, ldCOL)
    m.ins(OpLoad, V3, ldNRM, vNRMIN)        # pass world normal (specular reflection direction)
    m.ins(OpStore, vNRMOUT, ldNRM)
    m.ins(OpLoad, V2, ldUV1, vUV1IN)        # pass lightmap UV (uv1) to the fragment
    m.ins(OpStore, vUV1OUT, ldUV1)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()

def build_frag(alpha_test=False, threshold=0.5):
    # alpha_test=True adds a `if (texAlpha < threshold) discard;` (OpKill) so hard-edged
    # cutouts (flags/foliage/animals — materials with AlphaTest:true) can render in the
    # OPAQUE pass writing depth, matching how libshell.so handles them (depth-test stays on,
    # depth-write on, transparent texels killed) instead of the no-depth-write blend pass.
    MAIN=1; VOID=2; FNVOID=3; FLOAT=4; V2=5; V4=6; IMG=7; SAMP=8; SI=9
    P_IN_V2=10; P_OUT_V4=11; P_UC_SAMP=12; P_UC_IMG=13
    VUV=14; OUTCOL=15; vSAMP=16; vTEX=17
    LBL=18; ldTEX=19; ldSAMP=20; siID=21; uvID=22; cID=23
    # push-constant tint: the renderer pushes mat4 model @byte0 + vec4 UniformColor @byte64.
    # We read the tint at offset 64 and multiply: outColor = texture * tint. Faithful to the
    # V79 ShellEnv shader (frag = texture * UniformColor); UniformColor carries the mat.sanim
    # MaterialTint (the per-frame fog/dust/flicker OPACITY animation that keeps fog faint).
    UINT=24; C0U=25; PC_STRUCT=26; P_PC_STRUCT=27; P_PC_V4=28; vPC=29
    tintPtr=30; ldTINT=31; tinted=32
    BOOL=33; C_THRESH=34; aID=35; ltID=36; KILL_LBL=37; MERGE_LBL=38
    # per-vertex vColor (loc1) = the diffuse-IBL irradiance (white for non-SpecIbl meshes)
    vCOL=39; P_IN_V4f=40; ldVCOL=41; diffuse4=42
    # SPECULAR reflection: sample the env cube (set2 b2) at the world-normal varying (loc2), add it
    # scaled by vColor.a (=1 only for SpecIbl metal/glass; 0 elsewhere -> no reflection). Output alpha
    # stays texture.a*tint.a (NOT *vColor.a) so opaque meshes don't go transparent.
    vNRM=43; P_IN_V3f=44; IMGC=45; P_UC_IMGC=46; SIC=47; vCUBE=48
    ldCUBE=49; ldNRMf=50; sicID=51; cubeSamp=52; aVCOL=53; specV4=54; sum4=55
    cr=56; cg=57; cb=58; ta=59; finalv=60; V3F=61
    # per-pixel LIGHTMAP: sampler 'lightmap' (set2 bind3) sampled at vUV1 (loc3); diffuse4 *= lightmap.
    # lightmappower is pre-baked into the lightmap texture on the CPU; non-lightmapped meshes bind a
    # 1x1 WHITE lightmap so the multiply is a no-op. (libshell ShellEnv = diffuse·lightmap·lightmappower.)
    vUV1=62; vLM=63; ldUV1=64; ldLM=65; slmID=66; lmSamp=67; litDiffuse=68
    BOUND=69
    m=Mod(BOUND)
    m.ins(OpCapability, 1)                                   # Shader
    m.ins(OpMemoryModel, 0, 1)                               # Logical GLSL450
    m.ins(OpEntryPoint, 4, MAIN, _strw("main"), VUV, vCOL, vNRM, vUV1, OUTCOL) # Fragment
    m.ins(OpExecutionMode, MAIN, 7)                          # OriginUpperLeft
    m.ins(OpName, MAIN, _strw("main"))
    m.ins(OpName, vTEX, _strw("baseColorTex"))
    m.ins(OpName, vSAMP, _strw("samp"))
    m.ins(OpName, vCUBE, _strw("specularCube"))
    m.ins(OpName, vLM, _strw("lightmap"))
    m.ins(OpName, vUV1, _strw("vUV1"))
    m.ins(OpName, VUV, _strw("vUV"))
    m.ins(OpName, vCOL, _strw("vColor"))
    m.ins(OpName, vNRM, _strw("vNormal"))
    m.ins(OpName, OUTCOL, _strw("outColor"))
    m.ins(OpDecorate, VUV, DEC_LOCATION, 0)
    m.ins(OpDecorate, vCOL, DEC_LOCATION, 1)
    m.ins(OpDecorate, vNRM, DEC_LOCATION, 2)
    m.ins(OpDecorate, vUV1, DEC_LOCATION, 3)
    m.ins(OpDecorate, vLM, DEC_DESCSET, 2)
    m.ins(OpDecorate, vLM, DEC_BINDING, 3)
    m.ins(OpDecorate, OUTCOL, DEC_LOCATION, 0)
    m.ins(OpDecorate, vCUBE, DEC_DESCSET, 2)
    m.ins(OpDecorate, vCUBE, DEC_BINDING, 2)
    m.ins(OpDecorate, vSAMP, DEC_DESCSET, 0)
    m.ins(OpDecorate, vSAMP, DEC_BINDING, 1)
    m.ins(OpDecorate, vTEX, DEC_DESCSET, 2)
    m.ins(OpDecorate, vTEX, DEC_BINDING, 1)
    # push-constant block: one vec4 member at BYTE OFFSET 64 (after the mat4 model)
    m.ins(OpMemberDecorate, PC_STRUCT, 0, DEC_OFFSET, 64)
    m.ins(OpDecorate, PC_STRUCT, DEC_BLOCK)
    m.ins(OpTypeVoid, VOID)
    m.ins(OpTypeFunction, FNVOID, VOID)
    m.ins(OpTypeFloat, FLOAT, 32)
    m.ins(OpTypeVector, V2, FLOAT, 2)
    m.ins(OpTypeVector, V4, FLOAT, 4)
    m.ins(OpTypeVector, V3F, FLOAT, 3)                       # vec3 (world normal varying)
    # OpTypeImage: sampledType Dim(2D=1) Depth=0 Arrayed=0 MS=0 Sampled=1 Format=Unknown(0)
    m.ins(OpTypeImage, IMG, FLOAT, 1, 0, 0, 0, 1, 0)
    m.ins(OpTypeImage, IMGC, FLOAT, 3, 0, 0, 0, 1, 0)        # Dim=Cube(3) -> the reflection cubemap
    m.ins(OpTypeSampler, SAMP)
    m.ins(OpTypeSampledImage, SI, IMG)
    m.ins(OpTypeSampledImage, SIC, IMGC)
    m.ins(OpTypeInt, UINT, 32, 0)
    m.ins(OpConstant, UINT, C0U, 0)
    m.ins(OpTypeStruct, PC_STRUCT, V4)                       # struct { vec4 tint; } @offset 64
    m.ins(OpTypePointer, P_PC_STRUCT, SC_PUSHCONSTANT, PC_STRUCT)
    m.ins(OpTypePointer, P_PC_V4, SC_PUSHCONSTANT, V4)
    if alpha_test:
        m.ins(OpTypeBool, BOOL)
        m.ins(OpConstant, FLOAT, C_THRESH, _f32(threshold))
    m.ins(OpTypePointer, P_IN_V2, SC_INPUT, V2)
    m.ins(OpTypePointer, P_IN_V3f, SC_INPUT, V3F)
    m.ins(OpTypePointer, P_IN_V4f, SC_INPUT, V4)
    m.ins(OpTypePointer, P_OUT_V4, SC_OUTPUT, V4)
    m.ins(OpTypePointer, P_UC_IMGC, SC_UNIFORMCONSTANT, IMGC)
    m.ins(OpTypePointer, P_UC_SAMP, SC_UNIFORMCONSTANT, SAMP)
    m.ins(OpTypePointer, P_UC_IMG, SC_UNIFORMCONSTANT, IMG)
    m.ins(OpVariable, P_IN_V2, VUV, SC_INPUT)
    m.ins(OpVariable, P_IN_V4f, vCOL, SC_INPUT)
    m.ins(OpVariable, P_IN_V3f, vNRM, SC_INPUT)
    m.ins(OpVariable, P_OUT_V4, OUTCOL, SC_OUTPUT)
    m.ins(OpVariable, P_UC_SAMP, vSAMP, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, P_UC_IMG, vTEX, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, P_UC_IMGC, vCUBE, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, P_IN_V2, vUV1, SC_INPUT)
    m.ins(OpVariable, P_UC_IMG, vLM, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, P_PC_STRUCT, vPC, SC_PUSHCONSTANT)
    m.ins(OpFunction, VOID, MAIN, 0, FNVOID)
    m.ins(OpLabel, LBL)
    m.ins(OpLoad, IMG, ldTEX, vTEX)
    m.ins(OpLoad, SAMP, ldSAMP, vSAMP)
    m.ins(OpSampledImage, SI, siID, ldTEX, ldSAMP)
    m.ins(OpLoad, V2, uvID, VUV)
    m.ins(OpImageSampleImplicitLod, V4, cID, siID, uvID)
    # diffuse4 = sampled * UniformColor(tint @offset 64) * vColor(per-vertex diffuse-IBL)
    m.ins(OpAccessChain, P_PC_V4, tintPtr, vPC, C0U)
    m.ins(OpLoad, V4, ldTINT, tintPtr)
    m.ins(OpFMul, V4, tinted, cID, ldTINT)
    m.ins(OpLoad, V4, ldVCOL, vCOL)
    m.ins(OpFMul, V4, diffuse4, tinted, ldVCOL)
    # per-pixel LIGHTMAP: litDiffuse = diffuse4 * texture(lightmap, vUV1). lightmappower pre-baked into the
    # lightmap texture; non-lightmapped meshes bind a 1x1 WHITE lightmap so this is a no-op.
    m.ins(OpLoad, IMG, ldLM, vLM)
    m.ins(OpSampledImage, SI, slmID, ldLM, ldSAMP)
    m.ins(OpLoad, V2, ldUV1, vUV1)
    m.ins(OpImageSampleImplicitLod, V4, lmSamp, slmID, ldUV1)
    m.ins(OpFMul, V4, litDiffuse, diffuse4, lmSamp)
    # specular: sample the env cube at the world normal, scale by vColor.a (spec weight), ADD it
    m.ins(OpLoad, IMGC, ldCUBE, vCUBE)
    m.ins(OpLoad, V3F, ldNRMf, vNRM)
    m.ins(OpSampledImage, SIC, sicID, ldCUBE, ldSAMP)
    m.ins(OpImageSampleImplicitLod, V4, cubeSamp, sicID, ldNRMf)
    m.ins(OpCompositeExtract, FLOAT, aVCOL, ldVCOL, 3)        # vColor.a = spec weight
    m.ins(OpVectorTimesScalar, V4, specV4, cubeSamp, aVCOL)
    m.ins(OpFAdd, V4, sum4, litDiffuse, specV4)
    # final = (sum4.rgb, tinted.a)  -- keep the opaque alpha, don't let vColor.a make it transparent
    m.ins(OpCompositeExtract, FLOAT, cr, sum4, 0)
    m.ins(OpCompositeExtract, FLOAT, cg, sum4, 1)
    m.ins(OpCompositeExtract, FLOAT, cb, sum4, 2)
    m.ins(OpCompositeExtract, FLOAT, ta, tinted, 3)
    m.ins(OpCompositeConstruct, V4, finalv, cr, cg, cb, ta)
    if alpha_test:
        m.ins(OpFOrdLessThan, BOOL, ltID, ta, C_THRESH)       # alpha < threshold
        m.ins(OpSelectionMerge, MERGE_LBL, 0)
        m.ins(OpBranchConditional, ltID, KILL_LBL, MERGE_LBL)
        m.ins(OpLabel, KILL_LBL)
        m.ins(OpKill)                                          # discard transparent texel
        m.ins(OpLabel, MERGE_LBL)
    m.ins(OpStore, OUTCOL, finalv)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()

def validate(words, name):
    assert words[0]==0x07230203, f"{name}: bad magic"
    bound=words[3]; i=5; n=len(words); maxid=0
    while i<n:
        wc=words[i]>>16; op=words[i]&0xFFFF
        assert wc>0, f"{name}: zero word count at {i}"
        assert i+wc<=n, f"{name}: insn overruns at {i} (wc={wc})"
        i+=wc
    assert i==n, f"{name}: trailing words ({i}!={n})"
    print(f"  {name}: {n} words, bound={bound}, OK")

def main():
    vert=build_vert(); frag=build_frag(); fragAlpha=build_frag(alpha_test=True)
    validate(vert,"vert"); validate(frag,"frag"); validate(fragAlpha,"fragAlpha")
    out=os.path.join(os.path.dirname(__file__),"src","v79_shader.h")
    with open(out,"w") as f:
        f.write("// AUTO-GENERATED by gen_v79_shader.py - do not edit by hand.\n")
        f.write("// Self-contained V79 unlit-transparent shader (outputs base texture rgb + ALPHA).\n")
        f.write("// See gen_v79_shader.py for the interface contract with vk_renderer.h.\n")
        f.write("#pragma once\n#include <cstdint>\n#include <cstddef>\n\n")
        def arr(nm, words):
            f.write(f"static const uint32_t {nm}[] = {{\n")
            for i in range(0,len(words),8):
                f.write("    "+",".join("0x%08x"%x for x in words[i:i+8])+",\n")
            f.write("};\n")
            f.write(f"static const size_t {nm}Size = sizeof({nm});\n\n")
        arr("kV79VertSpirv", vert)
        arr("kV79FragSpirv", frag)
        arr("kV79FragAlphaSpirv", fragAlpha)
    print("wrote", out)

if __name__=="__main__":
    main()
