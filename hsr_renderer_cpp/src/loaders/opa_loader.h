#pragma once
// V79 ".opa" loader — the OLD OFFICIAL Meta-home cooked-asset format (e.g. spacestation:
// com.oculus.environment.prod.spacestation__base.apk). Reverse-engineered FAITHFULLY from
// V79 libshell.so (Meta MHE's own core/reflection serializer — "VertexTypeSystem"/
// "dataformat"), NOT guessed. Produces the renderer's MeshData[] (positions/uvs/indices,
// per-node world transform baked into positions), the source-format side of the V79 -> new
// HSR env porter — same role as gltf_loader.h but for cooked .opa homes.
//
// Container nesting: APK -> assets/scene.zip -> cache/android/<name>.fbx.opa (geometry).
//
// .opa MODEL SCHEMA (decompiled: sub_AF0678 MeshData reader + sub_AECEF0/DC74A8/AF0384/...):
//   [OPAA hdr 48B] then payload:
//     u32 version (0x405)        string typeName ("MeshData")
//     NODES:    u32 count; each = [8B lead][string name][i32 parent][T f32x3][R quat f32x4][S f32x3]
//     MATERIALS u32 count; each = reflection obj: {[u8 tag !=0xC8][string field][value]}* [0xC8 end]
//               fields: "Id"=u64, "Path"=string(.mat.asset)
//     MESHES:   u32 count; each entry =
//               string posFmt("RigidPos")  string dataFmt("StdData")
//               u32 listCount(usually 0)
//               u32 submeshCount; each submesh = [u32 ?][u32 firstIndex][u32 indexCount]
//                     [u32 matIndex][AABB min f32x3][AABB max f32x3][u8 bool]
//                     (version>=0x407 inserts 2 extra u32 @ +16/+20)
//               AABB min f32x3 / max f32x3   (whole-mesh)
//               u32 vertCount
//               u32 posBytes;  posBytes of RigidPos  (pos f32x3, stride 12)
//               u32 stdBytes;  stdBytes of StdData   (stride 20, uv f16x4 @ off 8)
//               u32 idxCount
//               string idxType("kUnsignedShort")
//               u32 idxBytes;  idxBytes of u16 indices
//               u16 tail (version>=0x404)
//   String codec (sub_AEF1EC): u16 marker; ==0xFFFF -> [u16 len][bytes] (len==0xFFFF ->
//   [u32 len][bytes]); else marker is an interned-table index (not used by these .opa).
//   Submesh draws indices [firstIndex, firstIndex+indexCount) with material[matIndex];
//   mesh entry[i] uses node[i]'s world transform.

#include "core/types.h"
#include "miniz.h"
#include "loaders/rendtxtr_parser.h"   // astc::decodeASTC (KTX/ASTC -> RGBA)
#include "render/ibl.h"               // SpecIbl diffuse irradiance cubemap (RGBA16F KTX)
#include "cook/node_rot_fit.h"      // shared spin/sway fitter (V79->V203 cook) — same core the glTF loader uses
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

class OpaLoader {
public:
    std::vector<MeshData> meshes;
    bool verbose = true;

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list a; va_start(a, fmt); fprintf(stderr, "[OPA] "); vfprintf(stderr, fmt, a);
        fprintf(stderr, "\n"); va_end(a);
    }

    // IEEE half -> float
    static float h2f(uint16_t h) {
        uint32_t s = (h & 0x8000u) << 16;
        uint32_t e = (h >> 10) & 0x1F;
        uint32_t m = h & 0x3FF;
        uint32_t bits;
        if (e == 0) {
            if (m == 0) { bits = s; }
            else { // subnormal
                e = 127 - 15 + 1;
                while (!(m & 0x400)) { m <<= 1; --e; }
                m &= 0x3FF;
                bits = s | (e << 23) | (m << 13);
            }
        } else if (e == 0x1F) {
            bits = s | 0x7F800000u | (m << 13);
        } else {
            bits = s | ((e - 15 + 127) << 23) | (m << 13);
        }
        float f; memcpy(&f, &bits, 4); return f;
    }

    // ── sequential cursor over the payload (libshell sub_AEF07C semantics) ──
    struct Cur {
        const uint8_t* d = nullptr; size_t n = 0; size_t p = 0; bool ok = true;
        bool avail(size_t k) const { return p + k <= n; }
        uint8_t  u8()  { if (!avail(1)) { ok = false; return 0; } return d[p++]; }
        uint16_t u16() { if (!avail(2)) { ok = false; return 0; } uint16_t v; memcpy(&v, d+p, 2); p += 2; return v; }
        uint32_t u32() { if (!avail(4)) { ok = false; return 0; } uint32_t v; memcpy(&v, d+p, 4); p += 4; return v; }
        int32_t  i32() { return (int32_t)u32(); }
        uint64_t u64() { if (!avail(8)) { ok = false; return 0; } uint64_t v; memcpy(&v, d+p, 8); p += 8; return v; }
        float    f32() { if (!avail(4)) { ok = false; return 0; } float v; memcpy(&v, d+p, 4); p += 4; return v; }
        void     skip(size_t k) { if (!avail(k)) { ok = false; p = n; } else p += k; }
        const uint8_t* at(size_t off) const { return d + off; }
        // sub_AEF1EC string record
        std::string str() {
            uint16_t m = u16();
            if (m != 0xFFFF) { ok = false; return {}; } // interned index — not present in these .opa
            uint32_t len = u16();
            if (len == 0xFFFF) len = u32();
            if (!avail(len)) { ok = false; return {}; }
            std::string s((const char*)d + p, len);
            p += len; return s;
        }
    };

    // ── minimal 4x4 (column-major) for baking node world transforms ──
    struct Mat4 { float m[16]; };
    static Mat4 identity() { Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.0f; return r; }
    static Mat4 mul(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k*4+row] * b.m[c*4+k];
                r.m[c*4+row] = s;
            }
        return r;
    }
    // column-major 4x4 helpers on raw float[16] (for skeletal skinning matrices)
    static void mat4mul(const float* A, const float* B, float* C) {
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) { float s=0; for(int k=0;k<4;++k) s+=A[k*4+r]*B[c*4+k]; C[c*4+r]=s; }
    }
    static void mat4affineInverse(const float* M, float* out) {
        float a0=M[0],a1=M[1],a2=M[2], a3=M[4],a4=M[5],a5=M[6], a6=M[8],a7=M[9],a8=M[10]; // 3x3 col-major
        float det = a0*(a4*a8-a5*a7) - a3*(a1*a8-a2*a7) + a6*(a1*a5-a2*a4);
        if (det > -1e-12f && det < 1e-12f) { for(int i=0;i<16;++i) out[i]=(i%5==0)?1.f:0.f; return; }
        float id = 1.0f/det;
        float b0=(a4*a8-a5*a7)*id, b1=(a2*a7-a1*a8)*id, b2=(a1*a5-a2*a4)*id;     // inv col-major
        float b3=(a5*a6-a3*a8)*id, b4=(a0*a8-a2*a6)*id, b5=(a2*a3-a0*a5)*id;
        float b6=(a3*a7-a4*a6)*id, b7=(a1*a6-a0*a7)*id, b8=(a0*a4-a1*a3)*id;
        float tx=M[12],ty=M[13],tz=M[14];
        out[0]=b0; out[1]=b1; out[2]=b2; out[3]=0;
        out[4]=b3; out[5]=b4; out[6]=b5; out[7]=0;
        out[8]=b6; out[9]=b7; out[10]=b8; out[11]=0;
        out[12]=-(b0*tx+b3*ty+b6*tz); out[13]=-(b1*tx+b4*ty+b7*tz); out[14]=-(b2*tx+b5*ty+b8*tz); out[15]=1;
    }
    static Mat4 trs(const float t[3], const float q[4], const float s[3]) {
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
        Mat4 r = identity();
        r.m[0]=(1-2*(yy+zz))*s[0]; r.m[1]=(2*(xy+wz))*s[0];   r.m[2]=(2*(xz-wy))*s[0];
        r.m[4]=(2*(xy-wz))*s[1];   r.m[5]=(1-2*(xx+zz))*s[1]; r.m[6]=(2*(yz+wx))*s[1];
        r.m[8]=(2*(xz+wy))*s[2];   r.m[9]=(2*(yz-wx))*s[2];   r.m[10]=(1-2*(xx+yy))*s[2];
        r.m[12]=t[0]; r.m[13]=t[1]; r.m[14]=t[2];
        return r;
    }
    static void xform(const Mat4& M, float x, float y, float z, float out[3]) {
        out[0] = M.m[0]*x + M.m[4]*y + M.m[8]*z + M.m[12];
        out[1] = M.m[1]*x + M.m[5]*y + M.m[9]*z + M.m[13];
        out[2] = M.m[2]*x + M.m[6]*y + M.m[10]*z + M.m[14];
    }

    struct Node { std::string name; int parent=-1; float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; };
    std::vector<Node> nodes;
    std::vector<Mat4> nodeWorld;
    struct Mat { uint64_t id=0; std::string path; };
    std::vector<Mat> materials;

    // Decoded *.png.opa textures (OPAA container -> KTX/ASTC -> RGBA), keyed by their cooked
    // basename (filename minus ".png.opa", lowercased) so ANY home's naming resolves — not
    // just spacestation's "tx_" convention.
    struct Tex { std::string key; std::vector<uint8_t> rgba; uint32_t w=1, h=1; bool hasAlpha=false; };
    std::vector<Tex> textures;

    // Faithful material metadata, read from the cooked *.mat.txt (libshell's own material
    // descriptions): the home TELLS us blend mode + diffuse texture, so we don't guess.
    // Keyed by the material's lowercased stem (its .mat.asset basename minus ".mat.*").
    struct MatProps {
        bool transparent=false, additive=false, alphaTest=false, doubleSided=false, unlit=true;
        uint64_t diffuseId=0;        // diffuse texture AssetId (-> assetIdToTexBase)
        uint64_t lightmapId=0;       // BAKED lightmap texture AssetId. The interior SHELL meshes
                                     // (helmet/gem/octo/table/floor...) bake their lighting+detail into
                                     // this; without it a no-albedo shell renders as a flat blob.
        std::string diffuseBase;     // OR diffuse texture basename from a Path field (lowercased)
        float diffuseColor[3]={1,1,1}; // 'diffuse' basecolor UNIFORM (flat color when no texture; tint when textured)
        float alpha=1.0f;            // 'alpha' UNIFORM — libshell scales fragment alpha by it. Transparent
                                     // effects (forge flicker=0.27, fog, dust) use <1 to be FAINT overlays;
                                     // ignoring it = full-opacity dark box that occludes everything behind.
        // PBR / SpecIbl uniforms (read verbatim from the cooked .mat.txt) — drive the split-sum IBL of
        // no-albedo metallic/gem shells (divingHelmet metallic=1, rubyGem metallic=0 rough=0, etc).
        float metallic=0.0f, roughness=1.0f;
        float speciblDiffScale=1.0f, speciblSpecScale=1.0f;
        float lightmapPower[3]={1.0f,1.0f,1.0f};  // per-channel lightmap HDR boost (the neon/glow tint)
        bool  isSpecibl=false;       // Shader: SpecIbl
        bool found=false;
    };
    std::unordered_map<std::string, MatProps> matProps;          // material stem -> props
    std::unordered_map<uint64_t, std::string> assetIdToTexBase;  // texture AssetId -> tex basename

    static std::string lc(std::string s){ for(auto&c:s)c=(char)tolower((unsigned char)c); return s; }
    // basename(path) with a trailing ".mat.asset"/".mat.opa"/".mat.txt" (or any ext run) stripped.
    static std::string matStem(const std::string& path) {
        size_t sl = path.find_last_of("/\\");
        std::string b = (sl==std::string::npos)?path:path.substr(sl+1);
        size_t m = b.find(".mat."); if (m!=std::string::npos) b=b.substr(0,m);
        return lc(b);
    }
    const Tex* texByBase(const std::string& base) const {
        std::string b=lc(base);
        for (auto& t : textures) if (t.key==b) return &t;
        return nullptr;
    }

    // KTX1 (ASTC) base mip -> RGBA (same decode the glTF loader uses)
    static bool decodeKtxBaseMip(const uint8_t* ktx, size_t n, std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH) {
        if (n < 64) return false;
        static const uint8_t id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        if (memcmp(ktx, id, 12) != 0) return false;
        auto u32a = [&](size_t o){ uint32_t v; memcpy(&v, ktx+o, 4); return v; };
        uint32_t glInternalFormat = u32a(28);
        uint32_t w = u32a(36), h = u32a(40);
        uint32_t bytesOfKeyValueData = u32a(60);
        size_t off = 64 + bytesOfKeyValueData;
        if (off + 4 > n) return false;
        uint32_t imageSize = u32a(off); off += 4;
        if (off + imageSize > n) imageSize = (uint32_t)(n - off);
        // Full ASTC footprint table (linear 0x93B0..0x93BD, sRGB 0x93D0..0x93DD, same
        // order of 14 footprints) — libshell reads the footprint from glInternalFormat,
        // so every non-square one (e.g. 8x6 = 0x93B6) must map correctly; defaulting to
        // 8x8 mis-strides the block grid and scrambles the texture.
        static const uint8_t kFootprints[14][2] = {
            {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
            {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
        };
        uint32_t bw = 8, bh = 8;
        int fidx = -1;
        if (glInternalFormat >= 0x93B0 && glInternalFormat <= 0x93BD) fidx = (int)(glInternalFormat - 0x93B0);
        else if (glInternalFormat >= 0x93D0 && glInternalFormat <= 0x93DD) fidx = (int)(glInternalFormat - 0x93D0);
        if (fidx >= 0) { bw = kFootprints[fidx][0]; bh = kFootprints[fidx][1]; }
        if (!astc::decodeASTC(ktx+off, imageSize, w, h, bw, bh, rgba)) return false;
        outW = w; outH = h; return true;
    }

    // ── sanim node-TRS animation (the looping ui_ring / wire motion) ──────────────────
    // sanim.opa (same OPAA reflection format) stores per-node TRS tracks: for each
    // (node, channel in {Translation,Rotation,Scale}): [u32 flag][u32 nKeys][u32 nVals]
    // [nVals f32]. nFrames = nKeys+1, comps = nVals/nFrames (3 for T/S, 4 for R quat).
    // No explicit per-key times -> dense per-frame sampling, so we loop at a fixed fps.
    struct Track { int comps = 0; int nFrames = 0; int effFrames = 0; std::vector<float> v; };
    struct NodeTracks { Track t, r, s; };
    std::unordered_map<std::string, NodeTracks> nodeAnim;
    struct AnimRec { size_t meshIdx; uint32_t nodeIdx; Mat4 parentWorld; std::vector<float> basePos; };
    std::vector<AnimRec> animRecs;
    std::vector<Mat4> nodeWorldAnim;   // per-frame scratch: animated world transform of every node (reused)
    std::vector<Node> animNodes;       // GLOBAL persistent node list — every .fbx.opa appended (parents offset).
                                       // animRec.nodeIdx indexes THIS, NOT the per-mesh `nodes` that parseModel
                                       // CLEARS each call (which made the bird's wing read the waterfall's node).
    int   animMaxFrames = 0;
    float animFps = 30.0f;

    // ── Skeletal animation (*.skel/*.anim.opa) for skinned meshes ──────────────────────────
    // An .anim clip stores, per frame, one 4x4 SKINNING matrix per joint (frame0 ~= identity ->
    // they're pre-multiplied jointWorld*inverseBind). vertex_world(t) = jointMat[t][bone]*bindVertex.
    struct AnimClip { std::vector<std::string> joints; std::vector<float> mats;   // mats = per-frame LOCAL 4x4
                      std::vector<int> parents; std::vector<float> invBind;       // from .skel (parents + inverse bind WORLD)
                      int numJoints=0, numFrames=0; };
    std::vector<AnimClip> clips;
    std::unordered_map<std::string,std::pair<int,int>> jointToClip;  // jointName -> (clipIdx, jointIdx)
    // .skel.opa bind data, keyed by basename (owl_offset.skel <-> owl_offset.anim): parents + bind LOCAL 4x4.
    std::unordered_map<std::string, std::pair<std::vector<int>, std::vector<float>>> skelData;
    // Per skinned mesh: bind verts + per-vertex 4 (skin-bone, weight) for linear blend skinning,
    // plus the skin's per-bone INVERSE-BIND (from the .skin.opa) and bone->clip-joint map.
    // skinMat[bone] = clipJointWorld[boneClip[bone]] * invBind[bone].
    struct SkinRec { size_t meshIdx; int clipIdx=-1; int nJoints=0; std::vector<float> basePos; // nv*3
                     std::vector<int> jidx; std::vector<float> jw;        // nv*4 each (jidx = SKIN bone idx)
                     std::vector<int> boneClip; std::vector<float> invBind; }; // nJoints, nJoints*16
    std::vector<SkinRec> skinRecs;
    std::vector<std::vector<float>> _clipSkin;   // per-frame scratch (reused -> no heap churn/stutter)
    std::vector<float> _sm;                       // per-skin scratch (reused)

    // mat.sanim "UVTransform" = per-mesh animated 2x3 UV matrix (flipbook / UV-scroll for smoke/
    // fire/dust/fog/particles). libshell drives this as the material's UniformUVOffset/Texm. Keyed
    // by the geo/node name (24/24 match the model nodes).
    struct UVTrack { int nFrames=0; std::vector<float> m; };   // m = nFrames * 6 (a,b,c, d,e,f)
    std::unordered_map<std::string, UVTrack> matUVAnim;
    struct UVAnimRec { size_t meshIdx; std::string node; std::vector<float> baseUV; };
    std::vector<UVAnimRec> uvAnimRecs;

    // mat.sanim "MaterialTint" = per-frame RGBA the shader multiplies into the fragment
    // (UniformColor). For fog/dust/flicker the ALPHA animates 0..~0.22, keeping the effect FAINT
    // and pulsing — dropping it makes fog render ~4-5x too dense. Keyed by geo/node name (same as
    // UVTransform). animate(t) samples it into MeshData.curTint; the renderer pushes it as the tint.
    struct TintTrack { int nFrames=0; std::vector<float> rgba; };  // rgba = nFrames * 4
    std::unordered_map<std::string, TintTrack> matTintAnim;
    struct TintRec { size_t meshIdx; std::string node; };
    std::vector<TintRec> tintRecs;

    // SpecIbl diffuse irradiance cubemap (RGBA16F), shipped as `*_diffuse.dds.opa`. The renderer bakes
    // diffuseCube(worldN) into the per-vertex color of `*_specibl` meshes (env-lit, not white/dark).
    ibl::Cubemap iblDiffuse;
    // SpecIbl SPECULAR (roughness-prefiltered) cubemap (RGBA16F), shipped as `*_specular.dds.opa`. We
    // decode mip0 for CPU per-vertex sampling (the sharp env reflection of metallic/gem shells) AND keep
    // the raw bytes for an optional GPU cube upload.
    ibl::Cubemap iblSpecular;
    std::vector<uint8_t> iblSpecularRaw;

    // ── VAT (Vertex Animation Texture): underwater coral/seaweed/fish/jellyfish ──────────────
    // libshell's CoVertexAnimation: a `t_*_vatdata.exr.opa` (cooked as an UNCOMPRESSED RGBA32F KTX,
    // width = #anim-verts, height = #frames) stores a per-frame, per-vertex POSITION OFFSET (frame 0
    // = 0 = rest pose; tiny ±0.1 sway). The mesh's UV1.x (a_texcoords zw@16) is the column (vertex
    // index) into that texture. Each frame: localPos = basePos + vatOffset[frame][col]; the instance
    // node transform places it. Keyed by the geo basename (sm_<X>.fbx <-> t_<X>_vatdata.exr).
    struct VatData { int cols=0, frames=0; std::vector<float> off; };  // off[(f*cols+c)*3 + xyz]
    std::unordered_map<std::string, VatData> vatByBase;
    struct VatRec { size_t meshIdx; std::vector<float> basePos; std::vector<int> col; Mat4 world; const VatData* vd=nullptr; };
    std::vector<VatRec> vatRecs;
    std::string curOpaBase;        // set before each parseModel (composed scene) for VAT matching
    float vatFps = 24.0f;

    bool hasAnimation() const { return (!animRecs.empty() || !skinRecs.empty() || !uvAnimRecs.empty() || !vatRecs.empty()) && (animMaxFrames > 1 || !vatRecs.empty()); }
    float animDuration() const { return animMaxFrames > 1 ? (float)animMaxFrames / animFps : 0.0f; }

    // ── COOK: SKINNED HZANIM extraction (the V79 OPA→V205 port for ALL skinned meshes — was dropped, cook only
    //    did rotation/UV). Builds a device-FAITHFUL HIERARCHICAL skeleton (NOT the flat world-bake): joints = the
    //    clip joints, parents = clip.parents, per-frame trsLocal = clip.mats (already parent-LOCAL, the renderer
    //    composes animWorld[j]=animWorld[parent]*mats[f][j]), bind = inverse(skin invBind) → composed jointBindWorld
    //    (sr.invBind[b]=inverse(jointBindWorld[boneClip[b]]), proven from the renderer's skinning at sub animate()).
    //    boneIdx remaps skin-bone→clip-joint via boneClip. The cook's useHz path (Incredibles-proven) emits HZAN:SKEL+
    //    ACL HZAN:ANIM + AnimatorPlatformComponent. project_hsl_cooker_expose_all_audit.
    struct OpaHzAnim {
        std::vector<float> jointPos, jointQuat, jointScale; std::vector<int> parents;
        std::vector<uint8_t> boneIdx, boneWgt; std::vector<float> trsLocal, restPos;
        int jointCount=0, frameCount=0; float fps=0.f;
        bool ok() const { return jointCount>0 && frameCount>1; }
    };
    OpaHzAnim extractHzAnim(int meshIdx) {
        OpaHzAnim e;
        const SkinRec* rec=nullptr; for (auto& r : skinRecs) if ((int)r.meshIdx==meshIdx) { rec=&r; break; }
        if (!rec || rec->clipIdx<0 || rec->clipIdx>=(int)clips.size()) return e;
        const AnimClip& clip = clips[rec->clipIdx];
        int nj=clip.numJoints, nf=clip.numFrames;
        if (nj<1 || nf<2 || (int)clip.mats.size() < nf*nj*16) return e;
        // column-major float[16] helpers (match gltf_loader matTrs/mulM)
        auto mul16=[](const float* a,const float* b,float* o){ for(int c=0;c<4;c++)for(int r=0;r<4;r++) o[c*4+r]=a[r]*b[c*4]+a[4+r]*b[c*4+1]+a[8+r]*b[c*4+2]+a[12+r]*b[c*4+3]; };
        auto invAff=[](const float* m,float* o){
            float M00=m[0],M01=m[4],M02=m[8], M10=m[1],M11=m[5],M12=m[9], M20=m[2],M21=m[6],M22=m[10];
            float det=M00*(M11*M22-M12*M21)-M01*(M10*M22-M12*M20)+M02*(M10*M21-M11*M20);
            float id=(det>1e-20f||det<-1e-20f)?1.f/det:0.f;
            o[0]=(M11*M22-M12*M21)*id; o[1]=(M12*M20-M10*M22)*id; o[2]=(M10*M21-M11*M20)*id;
            o[4]=(M02*M21-M01*M22)*id; o[5]=(M00*M22-M02*M20)*id; o[6]=(M01*M20-M00*M21)*id;
            o[8]=(M01*M12-M02*M11)*id; o[9]=(M02*M10-M00*M12)*id; o[10]=(M00*M11-M01*M10)*id;
            o[3]=o[7]=o[11]=0; o[15]=1;
            float tx=m[12],ty=m[13],tz=m[14];
            o[12]=-(o[0]*tx+o[4]*ty+o[8]*tz); o[13]=-(o[1]*tx+o[5]*ty+o[9]*tz); o[14]=-(o[2]*tx+o[6]*ty+o[10]*tz); };
        auto matTrs=[](const float* m,float* q,float* t,float* s){
            t[0]=m[12];t[1]=m[13];t[2]=m[14];
            s[0]=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=s[0]>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
            else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
            else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
            else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} };
        e.jointCount=nj; e.frameCount=nf; e.fps = animFps>0.f?animFps:30.f;
        e.parents=clip.parents; if((int)e.parents.size()<nj) e.parents.resize(nj,-1);
        e.restPos=rec->basePos;
        size_t nv=rec->basePos.size()/3;
        // per-vertex bone idx (skin-bone→clip-joint) + normalized u8 weights
        e.boneIdx.assign(nv*4,0); e.boneWgt.assign(nv*4,0);
        for(size_t v=0;v<nv;v++){ float ws=0; for(int c=0;c<4;c++) ws+=rec->jw[v*4+c];
            for(int c=0;c<4;c++){ int sb=rec->jidx[v*4+c]; int cj=(sb>=0&&sb<(int)rec->boneClip.size())?rec->boneClip[sb]:0; if(cj<0||cj>=nj)cj=0;
                e.boneIdx[v*4+c]=(uint8_t)cj; float w=ws>1e-6f?rec->jw[v*4+c]/ws:(c==0?1.f:0.f); int iw=(int)(w*255.f+0.5f); e.boneWgt[v*4+c]=(uint8_t)(iw<0?0:iw>255?255:iw); } }
        // jointBindWorld[j] = inverse(sr.invBind[bone mapping to j]); fallback identity for jointless joints
        std::vector<float> bw((size_t)nj*16,0.f); std::vector<char> have(nj,0);
        for(int b=0;b<rec->nJoints;b++){ int cj=(b<(int)rec->boneClip.size())?rec->boneClip[b]:-1;
            if(cj<0||cj>=nj||have[cj]||(size_t)(b*16+16)>rec->invBind.size()) continue;
            invAff(rec->invBind.data()+(size_t)b*16, bw.data()+(size_t)cj*16); have[cj]=1; }
        for(int j=0;j<nj;j++) if(!have[j]){ float* m=bw.data()+(size_t)j*16; for(int k=0;k<16;k++)m[k]=0; m[0]=m[5]=m[10]=m[15]=1; }
        // bind LOCAL (relative to parent joint) → jointPos/Quat(wxyz)/Scale
        e.jointPos.resize(nj*3); e.jointQuat.resize(nj*4); e.jointScale.resize(nj);
        for(int j=0;j<nj;j++){ float bl[16]; int p=e.parents[j];
            if(p>=0&&p<nj){ float ip[16]; invAff(bw.data()+(size_t)p*16,ip); mul16(ip,bw.data()+(size_t)j*16,bl); }
            else memcpy(bl,bw.data()+(size_t)j*16,64);
            float q[4],t[3],s[3]; matTrs(bl,q,t,s);
            e.jointPos[j*3]=t[0];e.jointPos[j*3+1]=t[1];e.jointPos[j*3+2]=t[2];
            e.jointQuat[j*4]=q[3];e.jointQuat[j*4+1]=q[0];e.jointQuat[j*4+2]=q[1];e.jointQuat[j*4+3]=q[2];
            e.jointScale[j]=(s[0]>1e-4f||s[0]<-1e-4f)?s[0]:1.f; }
        // per-frame local TRS (clip.mats are already parent-local) → trsLocal {qx,qy,qz,qw, t3, s3}
        e.trsLocal.resize((size_t)nf*nj*10);
        for(int f=0;f<nf;f++)for(int j=0;j<nj;j++){ const float* m=clip.mats.data()+((size_t)f*nj+j)*16;
            float q[4],t[3],s[3]; matTrs(m,q,t,s); float* o=e.trsLocal.data()+((size_t)f*nj+j)*10;
            o[0]=q[0];o[1]=q[1];o[2]=q[2];o[3]=q[3]; o[4]=t[0];o[5]=t[1];o[6]=t[2]; o[7]=s[0];o[8]=s[1];o[9]=s[2]; }
        return e;
    }

    // ── COOK: NON-skinned node-TRANSLATION (cars/train) → a 1-JOINT RIGID HZANIM clip. Reuses the cook's HZANIM
    //    emitter so arbitrary node PATHS port faithfully (the rotation-fit only does pure spin/sway; translation was
    //    DROPPED → cars static on device). joint0 = the node; clip[f] = nodeWorldAnim[node] sampled per frame (WORLD
    //    transform); restPos = node-LOCAL basePos; bind = IDENTITY (invBind=identity → skinMatrix(f)=nodeWorldAnim(f)
    //    → vertex(f)=nodeWorldAnim(f)*basePos = the renderer's node anim). Returns !ok() if the node doesn't TRANSLATE
    //    (origin static) so pure spins keep the lighter getTime() Rodrigues path. project_hsl_opa_anim_port_plan.
    OpaHzAnim extractNodeRigidHzAnim(int meshIdx) {
        OpaHzAnim e;
        if (animMaxFrames < 2) return e;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar || ar->basePos.size() < 9) return e;
        uint32_t node = ar->nodeIdx;
        float clipDur = animDuration(); if (clipDur <= 0.f) return e;
        int NF = animMaxFrames > 64 ? 64 : animMaxFrames; if (NF < 2) return e;
        auto matTrs=[](const float* m,float* q,float* t,float* s){
            t[0]=m[12];t[1]=m[13];t[2]=m[14];
            s[0]=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=s[0]>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
            else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
            else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
            else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} };
        std::vector<float> trs((size_t)NF*10); float o0[3]={0,0,0}, maxd=0.f;
        for (int f=0; f<NF; f++) { evalAnimNodes(clipDur * (float)f / (float)(NF-1));
            Mat4 w = (node < nodeWorldAnim.size()) ? nodeWorldAnim[node] : identity();
            float q[4],t[3],s[3]; matTrs(w.m, q, t, s);
            float* p=trs.data()+(size_t)f*10; p[0]=q[0];p[1]=q[1];p[2]=q[2];p[3]=q[3]; p[4]=t[0];p[5]=t[1];p[6]=t[2]; p[7]=s[0];p[8]=s[1];p[9]=s[2];
            if (f==0){ o0[0]=t[0];o0[1]=t[1];o0[2]=t[2]; }
            else { float dx=t[0]-o0[0],dy=t[1]-o0[1],dz=t[2]-o0[2]; float d=std::sqrt(dx*dx+dy*dy+dz*dz); if(d>maxd)maxd=d; } }
        animate(0.f);   // restore rest pose (geometry bake reads the renderer's GPU model, this just resets the loader)
        if (maxd < 0.01f) return e;   // node doesn't TRANSLATE → leave pure spins to the getTime() Rodrigues path
        size_t nv = ar->basePos.size()/3;
        // fps over the DOWNSAMPLED frame count so the device loop = the REAL clipDur (the OPA analogue of the
        // 70274aa warp-speed fix). NF<=64 frames are sampled INCLUSIVELY over [0,clipDur] (line ~414) = NF-1
        // intervals, so fps MUST be (NF-1)/clipDur, NOT the global animFps. With animFps a >64-frame path played
        // in (NF-1)/animFps s (e.g. 63/30=2.1s for a real 10s path = ~5x too fast). clipDur>0 + NF>=2 hold above.
        e.jointCount=1; e.frameCount=NF; e.fps = (float)(NF-1) / clipDur;
        e.parents={-1}; e.jointPos={0,0,0}; e.jointQuat={1,0,0,0}; e.jointScale={1};   // identity bind
        e.restPos = ar->basePos;   // node-LOCAL verts (the clip's WORLD transform places them)
        e.boneIdx.assign(nv*4,0); e.boneWgt.assign(nv*4,0); for (size_t v=0;v<nv;v++) e.boneWgt[v*4]=255;
        e.trsLocal = std::move(trs);
        return e;
    }

    // ── COOK: node TRANSLATION (cars/train) → the NET world displacement over the clip, for a ShellPoseAnimationComponent
    //    (the FAITHFUL, device-proven V79→V205 node-anim port — NOT a 1-joint skin, which the device's MeshDefinition::fix
    //    REJECTS as a degenerate maxBoneIdx=0 skin → "flatbuffer verification failed" → no render). Mesh stays STATIC
    //    (valid), the entity pose lerps rest→rest+delta. Returns false if the node doesn't translate (pure spin keeps
    //    the getTime() Rodrigues path). delta = the MAX origin displacement over the clip. ──
    bool extractNodeTranslate(int meshIdx, float delta[3]) {
        delta[0]=delta[1]=delta[2]=0.f;
        if (animMaxFrames < 2) return false;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        uint32_t node=ar->nodeIdx; float cd=animDuration(); if (cd<=0.f) return false;
        evalAnimNodes(0.f);
        float o0[3]={0,0,0}; if (node<nodeWorldAnim.size()){ o0[0]=nodeWorldAnim[node].m[12]; o0[1]=nodeWorldAnim[node].m[13]; o0[2]=nodeWorldAnim[node].m[14]; }
        float best[3]={0,0,0}, bestd=0.f; const int NS=24;
        for (int f=1; f<=NS; f++) { evalAnimNodes(cd*(float)f/(float)NS);
            if (node>=nodeWorldAnim.size()) continue;
            float dx=nodeWorldAnim[node].m[12]-o0[0], dy=nodeWorldAnim[node].m[13]-o0[1], dz=nodeWorldAnim[node].m[14]-o0[2];
            float d=dx*dx+dy*dy+dz*dz; if (d>bestd){ bestd=d; best[0]=dx;best[1]=dy;best[2]=dz; } }
        animate(0.f);
        if (bestd < 1e-4f) return false;
        delta[0]=best[0]; delta[1]=best[1]; delta[2]=best[2];
        return true;
    }

    // ── COOK: node SCALE "breathe" (NON-UNIFORM per-axis) → N per-axis FACTOR frames (= scale(t)/scale(0), frame0=(1,1,1))
    //    sampled over the SCALE track's OWN length, for a shadergen::SCALE getTime() shader. pivot = node WORLD origin
    //    (the shader scales in place). Scale is a LOCAL channel, so the factor needs only the node's own scale track.
    //    Returns false if the node has no scale track or it doesn't actually breathe (matches the glTF extractNodeScaleFrames). ──
    bool cookExtractNodeScaleFrames(int meshIdx, int N, std::vector<float>& frameFactors, float& loopSec, float pivot[3]) {
        frameFactors.clear(); loopSec = 0.f; pivot[0]=pivot[1]=pivot[2]=0.f;
        if (animMaxFrames < 2 || N < 2) return false;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        uint32_t node=ar->nodeIdx; if (node >= animNodes.size()) return false;
        auto it = nodeAnim.find(animNodes[node].name);
        if (it == nodeAnim.end() || it->second.s.nFrames <= 1 || it->second.s.comps != 3) return false;   // no real scale track
        const Track& st = it->second.s;
        // loopSec = the SCALE track's OWN length (frames/fps), like the glTF per-clip-duration. animFps = the engine rate.
        loopSec = (animFps > 0.f) ? (float)st.nFrames / animFps : 0.f;
        if (loopSec <= 1e-4f) loopSec = animDuration();
        if (loopSec <= 1e-4f) return false;
        float base[3]={animNodes[node].s[0],animNodes[node].s[1],animNodes[node].s[2]};
        sampleTrack(st, 0.f, animFps, 3, base);                       // t=0 baked scale (clamped first frame)
        for (int c=0;c<3;c++) if (base[c]==0.f) base[c]=1.f;
        frameFactors.assign((size_t)N*3, 1.f); float maxDev=0.f;
        for (int fi=0; fi<N; ++fi){ float s[3]={base[0],base[1],base[2]};
            sampleTrack(st, loopSec*(float)fi/(float)N, animFps, 3, s);
            for (int c=0;c<3;c++){ float f=s[c]/base[c]; frameFactors[(size_t)fi*3+c]=f; float d=f>1.f?f-1.f:1.f-f; if(d>maxDev)maxDev=d; } }
        evalAnimNodes(0.f);   // rest pose -> node world origin = pivot
        if (node < nodeWorldAnim.size()){ pivot[0]=nodeWorldAnim[node].m[12]; pivot[1]=nodeWorldAnim[node].m[13]; pivot[2]=nodeWorldAnim[node].m[14]; }
        animate(0.f);
        return maxDev > 1e-3f;   // actually breathes
    }
    // ── COOK: batch-fit every node-animated mesh to a SPIN/SWAY about an axis (the V79->V203 port). Samples each
    //    animated mesh's WORLD positions across the clip via animate(t), runs the shared noderot::fit, and returns
    //    meshIdx -> Result. Leaves the meshes at REST (animate(0)) so the static geometry bake is the t=0 pose. The
    //    cooker's useRot path then ships a getTime() Rodrigues shader. (Node TRANSFORM anims = the bulk of OPA motion;
    //    UV-scroll / skinned / VAT are separate passes.) ──
    void cookExtractRotations(std::unordered_map<size_t, noderot::Result>& out) {
        out.clear();
        if (animRecs.empty() || animMaxFrames < 2) return;
        float clipDur = animDuration(); if (clipDur <= 0.f) return;
        const int NS = 24;
        const size_t budget = 48000000;                  // ≤ ~190MB of position floats in flight at once (no swap/softlock)
        const size_t maxNv  = budget / ((size_t)(NS+1)*3);   // a single mesh bigger than this cooks static (huge meshes aren't the spinning ones)
        struct M { size_t idx; size_t nv; uint32_t node; const std::vector<float>* base; float pivot[3]; };
        std::vector<M> ms; std::unordered_map<size_t,int> seen;
        for (auto& ar : animRecs) { size_t nv = ar.basePos.size()/3; if (nv < 3 || nv > maxNv) continue; if (seen.count(ar.meshIdx)) continue;
            seen[ar.meshIdx]=1; ms.push_back({ar.meshIdx, nv, ar.nodeIdx, &ar.basePos, {0,0,0}}); }
        if (ms.empty()) { animate(0.f); return; }
        evalAnimNodes(0.f);   // rest pose -> each node's world origin = its pivot
        for (auto& m : ms) if (m.node < nodeWorldAnim.size()) { m.pivot[0]=nodeWorldAnim[m.node].m[12]; m.pivot[1]=nodeWorldAnim[m.node].m[13]; m.pivot[2]=nodeWorldAnim[m.node].m[14]; }
        // MEMORY-BOUNDED chunks: sample NS frames for a chunk of meshes (node-only eval), fit, free, repeat.
        size_t i0 = 0;
        while (i0 < ms.size()) {
            size_t i1 = i0, cv = 0;
            while (i1 < ms.size() && (cv + ms[i1].nv) * (size_t)(NS+1) * 3 <= budget) { cv += ms[i1].nv; ++i1; }
            if (i1 == i0) i1 = i0 + 1;
            std::vector<std::vector<std::vector<float>>> fr(i1 - i0);
            for (size_t k=i0;k<i1;k++) fr[k-i0].resize(NS+1);
            for (int f=0; f<=NS; f++) { evalAnimNodes(clipDur * (float)f / (float)NS);
                for (size_t k=i0;k<i1;k++){ M& m=ms[k]; auto& F=fr[k-i0][f]; F.resize(m.nv*3); Mat4 w=(m.node<nodeWorldAnim.size())?nodeWorldAnim[m.node]:identity();
                    for (size_t v=0; v<m.nv; v++){ float o[3]; xform(w,(*m.base)[v*3],(*m.base)[v*3+1],(*m.base)[v*3+2],o); F[v*3]=o[0];F[v*3+1]=o[1];F[v*3+2]=o[2]; } } }
            for (size_t k=i0;k<i1;k++){ M& m=ms[k]; std::vector<const float*> fp(NS+1); for (int f=0;f<=NS;f++) fp[f]=fr[k-i0][f].data();
                noderot::Result r = noderot::fit(fp, m.nv, m.pivot, clipDur); if (r.rotAnim) out[m.idx] = r; }
            i0 = i1;
        }
        animate(0.f);   // FULL restore (skinning/UV too) -> leave every mesh at rest for the geometry bake
    }

    // ── COOK: mat.sanim UV-SCROLL port. A UVTrack is a per-frame 2x3 affine [a,b,c,d,e,f]; a continuous SCROLL
    //    (water/foam/waterfall) keeps the 2x2 ~ identity and TRANSLATES (c,f). Derive the VISIBLE scroll rate the
    //    rate = avg per-frame UV delta * fps = UV/second (the source's literal visible speed) -> uvRate for a
    //    getTime() uv += rate*time shader. Flipbook atlases (2x2 = cell scale) are a separate pass -> skipped. ──
    void cookExtractUVScroll(std::unordered_map<size_t, std::pair<float,float>>& out) {
        out.clear();
        if (uvAnimRecs.empty() || animFps <= 0.f) return;
        for (auto& ur : uvAnimRecs) {
            if (out.count(ur.meshIdx)) continue;
            auto it = matUVAnim.find(ur.node);
            if (it == matUVAnim.end() || it->second.nFrames < 2) continue;
            const UVTrack& tr = it->second; const float* M0 = tr.m.data();
            float a=M0[0], b=M0[1], dd=M0[3], e=M0[4];
            if (std::fabs(a-1.f)>0.05f || std::fabs(e-1.f)>0.05f || std::fabs(b)>0.05f || std::fabs(dd)>0.05f) continue;  // flipbook atlas, not a scroll
            // AVERAGE per-frame delta from a CAPPED window (water tracks are uniform; iterating 10k+ frames softlocks),
            // then scale to the full track length.
            int win = tr.nFrames - 1; if (win > 256) win = 256;
            double sdu=0, sdv=0; int n=0;
            for (int f=0; f<win; f++) {
                float dc = tr.m[(size_t)(f+1)*6+2] - tr.m[(size_t)f*6+2];
                float df = tr.m[(size_t)(f+1)*6+5] - tr.m[(size_t)f*6+5];
                if (std::fabs(dc)>0.5f || std::fabs(df)>0.5f) continue;   // skip wrap jumps
                sdu += dc; sdv += df; ++n;
            }
            if (n < 1) continue;
            // FAITHFUL scroll rate = avg per-frame UV delta * fps = UV/second, exactly the source's visible speed.
            // The getTime() shader is `uv += rate*time` (unbounded; the texture's REPEAT wrap makes it loop), so ONLY
            // the rate matters -- the old HSR_MATLOOP=5s loopSec cap + 0.5 UV/s clamp were guesses that divorced the
            // cooked rate from the source (lakeside water: a true ~336s loop forced to 5s then clamped 0.5 = no relation).
            float ru = (float)(sdu / n * animFps), rv = (float)(sdv / n * animFps);
            if (std::sqrt(ru*ru + rv*rv) < 1e-4f) continue;
            out[ur.meshIdx] = std::make_pair(ru, rv);
        }
    }

    // ── Per-clip frame-rate READ FROM THE COOKED DATA, not hardcoded ─────────────────────────
    // libshell pulls the rate from a NAMED "FrameRate" field (driver sub_2EAEF5C does
    // sub_2EF36E0(asset,"FrameRate") -> f32). We do the same: scan for the OPAA name record
    // [0xFFFF][u16 len=9]["FrameRate"] and take the f32 right after. Returns <=0 when the cook
    // stores no rate -> the caller keeps the engine default (many V79 .sanim/.mat.sanim cooks
    // store NO rate field at all and are sampled at the engine's fixed cook rate).
    static float findFrameRate(const std::vector<uint8_t>& d) {
        static const char K[9] = {'F','r','a','m','e','R','a','t','e'};
        if (d.size() < 21) return -1.0f;
        for (size_t i = 0; i + 17 <= d.size(); ++i) {
            if (d[i] != 0xFF || d[i+1] != 0xFF) continue;
            uint16_t ln; memcpy(&ln, d.data()+i+2, 2);
            if (ln != 9 || memcmp(d.data()+i+4, K, 9) != 0) continue;
            float f; memcpy(&f, d.data()+i+13, 4);
            if (f > 0.5f && f < 480.0f) return f;       // sane fps only
        }
        return -1.0f;
    }

    // Evaluate ONLY the animated node hierarchy at time t -> nodeWorldAnim (NO mesh deform, NO skinning/UV/tint).
    // The cook rotation sampler calls this per frame instead of the full animate() (which skins/UV-transforms EVERY
    // mesh -> far too slow over 33 frames on a big env like lakesidepeak).
    void evalAnimNodes(float t) {
        nodeWorldAnim.resize(animNodes.size());
        for (size_t i = 0; i < animNodes.size(); ++i) {
            const Node& nd = animNodes[i];
            float T[3]={nd.t[0],nd.t[1],nd.t[2]};
            float R[4]={nd.r[0],nd.r[1],nd.r[2],nd.r[3]};  // static R already (x,y,z,w)
            float S[3]={nd.s[0],nd.s[1],nd.s[2]};
            auto it = nodeAnim.find(nd.name);
            if (it != nodeAnim.end()) {
                const NodeTracks& nt = it->second;
                sampleTrack(nt.t, t, animFps, 3, T);
                sampleTrack(nt.r, t, animFps, 4, R);
                sampleTrack(nt.s, t, animFps, 3, S);
                if (nt.r.nFrames > 0) { float qw=R[0],qx=R[1],qy=R[2],qz=R[3]; R[0]=qx; R[1]=qy; R[2]=qz; R[3]=qw; }
                float ql = sqrtf(R[0]*R[0]+R[1]*R[1]+R[2]*R[2]+R[3]*R[3]);
                if (ql > 1e-6f) { R[0]/=ql; R[1]/=ql; R[2]/=ql; R[3]/=ql; } else { R[0]=R[1]=R[2]=0; R[3]=1; }
            }
            Mat4 local = trs(T, R, S);
            int par = nd.parent;
            nodeWorldAnim[i] = (par >= 0 && par < (int)i) ? mul(nodeWorldAnim[par], local) : local;
        }
    }

    static void sampleTrack(const Track& tr, float t, float fps, int comps, float* out) {
        if (tr.nFrames <= 0 || tr.comps != comps) return;          // leave caller default
        // Each track loops on its OWN length at the engine rate (NOT stretched to the global longest
        // clip — with a 2501-frame bird path that made a short fan-rotation loop crawl over 83s).
        float f = fmodf(t * fps, (float)tr.nFrames);
        if (f < 0.0f) f += (float)tr.nFrames;
        int i0 = (int)f; float frac = f - (float)i0;
        if (i0 >= tr.nFrames) { i0 = tr.nFrames - 1; frac = 0.0f; }
        int i1 = (i0 + 1 < tr.nFrames) ? i0 + 1 : 0;               // wrap to frame 0 -> smooth loop seam
        for (int c = 0; c < comps; ++c)
            out[c] = tr.v[(size_t)i0*comps + c] * (1.0f - frac) + tr.v[(size_t)i1*comps + c] * frac;
    }
    // Sample each animated node at looped time t and rewrite its mesh's world positions.
    void animate(float t) {
        if (!hasAnimation()) return;
        // ── Evaluate the WHOLE node hierarchy with animation (libshell's scene graph): for each node
        //    world[i] = world[parent] * localTRS(i), where localTRS uses the sanim-sampled TRS when the
        //    node is keyed, else the node's STATIC local transform. This makes a mesh move when ANY
        //    ANCESTOR is animated (e.g. the bird mesh under the animated `birdBody_path` node) — not
        //    only when its own node is keyed. (Nodes are stored parent-before-child, so one pass.)
        if (!animRecs.empty()) evalAnimNodes(t);   // node hierarchy -> nodeWorldAnim (shared with the cook sampler)
        for (auto& ar : animRecs) {
            Mat4 m = (ar.nodeIdx < nodeWorldAnim.size()) ? nodeWorldAnim[ar.nodeIdx] : identity();
            MeshData& md = meshes[ar.meshIdx];
            size_t nv = ar.basePos.size() / 3;
            static int birddbg = -1; if (birddbg<0) birddbg = std::getenv("HSR_BIRDDBG")?1:0;
            if (birddbg && nv>0) {
                float bmn[3]={1e9f,1e9f,1e9f}, bmx[3]={-1e9f,-1e9f,-1e9f};
                for (size_t i=0;i<nv;i++) for(int c=0;c<3;c++){float v=ar.basePos[i*3+c]; if(v<bmn[c])bmn[c]=v; if(v>bmx[c])bmx[c]=v;}
                fprintf(stderr,"[BIRDDBG] mesh#%zu node=%u  nodeWt=(%.2f,%.2f,%.2f)  baseAABB=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)  diag=%.2f\n",
                    ar.meshIdx, ar.nodeIdx, m.m[12],m.m[13],m.m[14], bmn[0],bmn[1],bmn[2], bmx[0],bmx[1],bmx[2],
                    std::sqrt((bmx[0]-bmn[0])*(bmx[0]-bmn[0])+(bmx[1]-bmn[1])*(bmx[1]-bmn[1])+(bmx[2]-bmn[2])*(bmx[2]-bmn[2])));
            }
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            for (size_t i = 0; i < nv; ++i) {
                float o[3]; xform(m, ar.basePos[i*3], ar.basePos[i*3+1], ar.basePos[i*3+2], o);
                md.positions[i*3]=o[0]; md.positions[i*3+1]=o[1]; md.positions[i*3+2]=o[2];
            }
        }
        // ── Skeletal LBS (skinned meshes) — libshell's vertex shader, on the CPU:
        //    localPos = Σ_i ( Joints[idx_i] * bindPos ) * weight_i   (i = 0..3) ─────────────────
        // The .anim + .skel store LOCAL (parent-relative) joint poses. For the current frame:
        //   animWorld[j] = animWorld[parent[j]] * animLocal(f)[j]   (parent[j] < j, single pass)
        //   Joints[j]    = animWorld[j] * invBind[j]                (invBind = inverse bind WORLD)
        // (matches V79 libshell Skeleton.cpp: m_jointParents + m_jointLocalPoses).
        auto& clipSkin = _clipSkin; clipSkin.resize(clips.size());   // reused scratch (capacity kept)
        for (size_t ci = 0; ci < clips.size(); ++ci) {
            const AnimClip& clip = clips[ci];
            if (clip.numFrames < 1 || (int)clip.parents.size() < clip.numJoints) { clipSkin[ci].clear(); continue; }
            // Interpolate BETWEEN baked frames (like sampleTrack does for sanim). Snapping to an
            // integer frame made skeletal anims step at the baked fps = stutter (the chicken looked
            // smooth only because it rides the interpolating sanim path). Dense baked frames => small
            // per-frame delta => element-wise LERP of the LOCAL joint matrices is visually smooth.
            // Each clip loops on its OWN length at the engine sample rate (animFps), independent of
            // other clips — NOT stretched to the global longest-clip duration (that desynced/slowed
            // shorter clips). i1 wraps to frame 0 so the loop seam interpolates smoothly.
            float f = fmodf(t * animFps, (float)clip.numFrames);
            if (f < 0.0f) f += (float)clip.numFrames;
            int i0 = (int)f; float frac = f - (float)i0;
            if (i0 < 0) { i0 = 0; frac = 0.0f; }
            if (i0 >= clip.numFrames) { i0 = clip.numFrames - 1; frac = 0.0f; }
            int i1 = (i0 + 1 < clip.numFrames) ? i0 + 1 : 0;
            const float* fm0 = clip.mats.data() + (size_t)i0 * clip.numJoints * 16;
            const float* fm1 = clip.mats.data() + (size_t)i1 * clip.numJoints * 16;
            // Joints[j] = jointWORLD = compose(animLocal, parents). The SkinnedPos verts are
            // authored joint-LOCAL (near origin), so jointWorld places them onto the building +
            // poses them — NO inverse-bind (that would cancel the placement -> owl back at origin).
            clipSkin[ci].resize((size_t)clip.numJoints*16);
            for (int j = 0; j < clip.numJoints; ++j) {
                float L[16];
                for (int k = 0; k < 16; ++k)
                    L[k] = fm0[(size_t)j*16+k]*(1.0f-frac) + fm1[(size_t)j*16+k]*frac;
                int p = clip.parents[j];
                if (p < 0 || p >= j) memcpy(clipSkin[ci].data()+(size_t)j*16, L, 16*sizeof(float));
                else mat4mul(clipSkin[ci].data()+(size_t)p*16, L, clipSkin[ci].data()+(size_t)j*16);
            }
        }
        for (auto& sr : skinRecs) {
            if (sr.clipIdx < 0 || sr.clipIdx >= (int)clips.size()) continue;
            const AnimClip& clip = clips[sr.clipIdx];
            if (clipSkin[sr.clipIdx].empty() || sr.nJoints < 1) continue;
            const float* cw = clipSkin[sr.clipIdx].data();   // clip joint WORLD matrices (composed)
            // Per skin-bone skinning matrix = clipJointWorld[boneClip[bone]] * invBind[bone].
            // (invBind = the SKIN's per-bone mesh-bind->joint-local; jointWorld places + poses.)
            auto& sm = _sm; sm.resize((size_t)sr.nJoints*16);   // reused scratch (capacity kept)
            for (int b = 0; b < sr.nJoints; ++b) {
                int cj = (b < (int)sr.boneClip.size()) ? sr.boneClip[b] : -1;
                if (cj < 0 || cj >= clip.numJoints || (size_t)(b*16+16) > sr.invBind.size()) {
                    for (int k=0;k<16;++k) sm[b*16+k] = (k%5==0)?1.f:0.f; continue;
                }
                mat4mul(cw + (size_t)cj*16, sr.invBind.data()+(size_t)b*16, sm.data()+(size_t)b*16);
            }
            MeshData& md = meshes[sr.meshIdx];
            size_t nv = sr.basePos.size() / 3;
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            for (size_t v = 0; v < nv; ++v) {
                float bx=sr.basePos[v*3], by=sr.basePos[v*3+1], bz=sr.basePos[v*3+2];
                float ox=0, oy=0, oz=0, wsum=0;
                for (int i = 0; i < 4; ++i) {
                    float w = sr.jw[v*4+i]; int b = sr.jidx[v*4+i];
                    if (w <= 0.0f || b < 0 || b >= sr.nJoints) continue;
                    const float* M = sm.data() + (size_t)b*16;   // column-major skinning matrix
                    ox += w * (M[0]*bx + M[4]*by + M[8]*bz + M[12]);
                    oy += w * (M[1]*bx + M[5]*by + M[9]*bz + M[13]);
                    oz += w * (M[2]*bx + M[6]*by + M[10]*bz + M[14]);
                    wsum += w;
                }
                if (wsum < 1e-4f) { ox=bx; oy=by; oz=bz; }            // unrigged vertex -> bind pose
                else if (wsum < 0.999f || wsum > 1.001f) { ox/=wsum; oy/=wsum; oz/=wsum; }  // normalize (weights not guaranteed to sum to 1)
                md.positions[v*3]=ox; md.positions[v*3+1]=oy; md.positions[v*3+2]=oz;
            }
        }
        // ── mat.sanim UV/flipbook: transform each animated mesh's base UVs by the current frame's
        //    2x3 matrix [a,b,c, d,e,f]: uv' = (a*u + b*v + c, d*u + e*v + f). ──────────────────────
        for (auto& ur : uvAnimRecs) {
            auto it = matUVAnim.find(ur.node);
            if (it == matUVAnim.end() || it->second.nFrames < 1) continue;
            const UVTrack& tr = it->second;
            // These are FLIPBOOK atlases (e.g. *_flipbook.png; the UV offset steps by exactly
            // 1/cols per frame — CARD_steam = +0.125/frame = an 8-wide sheet). libshell plays them
            // by SNAPPING to the integer frame: interpolating would slide the sample window across
            // a cell boundary and blend two sprites = smear. Each flipbook loops on its OWN length
            // at the engine sample rate (animFps); tying it to the global (longest-clip) duration is
            // what made short flipbooks crawl ("looks like a flipbook, not animated"). Direction +
            // per-frame speed are already baked into the keyframe deltas.
            // VISIBLE-LOOP: the cooked UV tracks span from a few flipbook frames to THOUSANDS of
            // dense scroll steps (lakeside water/waterfall = 10078 frames). libshell times these
            // off a runtime animation::PlaybackState we proved can't be read statically, and at one
            // global fps the dense scrolls crawl invisibly (10078/30 = 336s -> looks frozen). So cap
            // the loop period (HSR_MATLOOP, default 5s): short flipbooks keep their own fast pace,
            // long scrolls loop in <=matLoopMax so the water/waterfall/smoke actually animate.
            static float matLoopMax = -1.f;
            if (matLoopMax < 0.f) { const char* e = std::getenv("HSR_MATLOOP"); matLoopMax = e ? (float)atof(e) : 5.0f; }
            float loopSec = (animFps > 0.f) ? (float)tr.nFrames / animFps : 0.f;
            if (matLoopMax > 0.f && loopSec > matLoopMax) loopSec = matLoopMax;
            float phase = (loopSec > 1e-4f) ? fmodf(t / loopSec, 1.0f) * (float)tr.nFrames
                                            : fmodf(t * animFps, (float)tr.nFrames);
            if (phase < 0.0f) phase += (float)tr.nFrames;
            int frame = (int)phase;
            if (frame < 0) frame = 0; if (frame >= tr.nFrames) frame = tr.nFrames - 1;
            const float* M = tr.m.data() + (size_t)frame * 6;
            MeshData& md = meshes[ur.meshIdx];
            size_t nuv = ur.baseUV.size() / 2;
            if (md.uvs.size() < nuv*2) md.uvs.resize(nuv*2);
            for (size_t i = 0; i < nuv; ++i) {
                float u0 = ur.baseUV[i*2], v0 = ur.baseUV[i*2+1];
                md.uvs[i*2]   = M[0]*u0 + M[1]*v0 + M[2];
                md.uvs[i*2+1] = M[3]*u0 + M[4]*v0 + M[5];
            }
            static int matdbg = -1; if (matdbg<0) matdbg = std::getenv("HSR_MATDBG")?1:0;
            if (matdbg && nuv>0) fprintf(stderr, "[MATDBG] t=%.2f mesh#%zu '%s' fr=%d/%d loopSec=%.2f M=[%.3f %.3f] uv0=(%.4f,%.4f)\n",
                t, ur.meshIdx, ur.node.c_str(), frame, tr.nFrames, loopSec, M[2], M[5], md.uvs[0], md.uvs[1]);
        }
        // ── mat.sanim MaterialTint: per-frame RGBA the shader multiplies into the fragment
        //    (UniformColor). UNLIKE the UV flipbook this is a smooth opacity fade, so LERP between
        //    frames (snapping would make the fog visibly step). This is the fog/dust/flicker
        //    OPACITY (alpha 0..~0.22) — without it fog renders far too dense. ───────────────────────
        for (auto& tr : tintRecs) {
            auto it = matTintAnim.find(tr.node);
            if (it == matTintAnim.end() || it->second.nFrames < 1) continue;
            const TintTrack& tt = it->second;
            static float matLoopMaxT = -1.f;
            if (matLoopMaxT < 0.f) { const char* e = std::getenv("HSR_MATLOOP"); matLoopMaxT = e ? (float)atof(e) : 5.0f; }
            float loopSecT = (animFps > 0.f) ? (float)tt.nFrames / animFps : 0.f;
            if (matLoopMaxT > 0.f && loopSecT > matLoopMaxT) loopSecT = matLoopMaxT;
            float phase = (loopSecT > 1e-4f) ? fmodf(t / loopSecT, 1.0f) * (float)tt.nFrames
                                             : fmodf(t * animFps, (float)tt.nFrames);
            if (phase < 0.0f) phase += (float)tt.nFrames;
            int f0 = (int)phase; float frac = phase - (float)f0;
            if (f0 >= tt.nFrames) { f0 = tt.nFrames - 1; frac = 0.0f; }
            int f1 = (f0 + 1 < tt.nFrames) ? f0 + 1 : 0;     // wrap for a seamless loop
            const float* a = tt.rgba.data() + (size_t)f0 * 4;
            const float* b = tt.rgba.data() + (size_t)f1 * 4;
            MeshData& md = meshes[tr.meshIdx];
            for (int c = 0; c < 4; ++c) md.curTint[c] = a[c]*(1.0f-frac) + b[c]*frac;
        }
        // ── VAT (Vertex Animation Texture): localPos = basePos + offset[frame][col], then place by
        //    the instance world matrix. Offsets are LERP'd between frames (loops on its own length
        //    at vatFps). This is the coral/seaweed/fish/jellyfish sway. ────────────────────────────
        for (auto& vr : vatRecs) {
            if (!vr.vd || vr.vd->frames < 1 || vr.vd->cols < 1) continue;
            const VatData& vd = *vr.vd;
            float f = fmodf(t * vatFps, (float)vd.frames); if (f < 0.0f) f += (float)vd.frames;
            int i0 = (int)f; float frac = f - (float)i0;
            if (i0 < 0) i0 = 0; if (i0 >= vd.frames) { i0 = vd.frames - 1; frac = 0.0f; }
            int i1 = (i0 + 1 < vd.frames) ? i0 + 1 : 0;       // wrap for a seamless loop
            MeshData& md = meshes[vr.meshIdx];
            size_t nv = vr.basePos.size() / 3;
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            for (size_t v = 0; v < nv; ++v) {
                int col = vr.col[v]; if (col < 0 || col >= vd.cols) col = 0;
                const float* o0 = &vd.off[((size_t)i0*vd.cols + col)*3];
                const float* o1 = &vd.off[((size_t)i1*vd.cols + col)*3];
                float lx = vr.basePos[v*3]   + o0[0]*(1.0f-frac) + o1[0]*frac;
                float ly = vr.basePos[v*3+1] + o0[1]*(1.0f-frac) + o1[1]*frac;
                float lz = vr.basePos[v*3+2] + o0[2]*(1.0f-frac) + o1[2]*frac;
                float wp[3]; xform(vr.world, lx, ly, lz, wp);
                md.positions[v*3]=wp[0]; md.positions[v*3+1]=wp[1]; md.positions[v*3+2]=wp[2];
            }
        }
    }

    // Parse sanim.opa -> nodeAnim (called before parseModel so it can mark animated meshes).
    void loadAnim(const std::vector<uint8_t>& sceneZip) {
        std::vector<uint8_t> sa;
        { mz_zip_archive z; memset(&z, 0, sizeof(z));
          if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
          uint32_t nf = mz_zip_reader_get_num_files(&z); int found = -1;
          for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            // Match the NODE-animation file (...fbx.sanim.opa) but NOT ...fbx.mat.sanim.opa, which
            // ALSO ends in ".sanim.opa" — grabbing that (UV flipbook tracks, no Translation/Rotation)
            // gave "0 animated nodes" so the bird's flight + windmill fans never animated.
            bool isMat = fn.size() >= 14 && fn.compare(fn.size()-14, 14, ".mat.sanim.opa") == 0;
            if (!isMat && fn.size() >= 10 && fn.compare(fn.size()-10, 10, ".sanim.opa") == 0) { found = (int)i; break; } }
          if (found < 0) { mz_zip_reader_end(&z); return; }
          size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, found, &sz, 0);
          mz_zip_reader_end(&z);
          if (!d) return; sa.assign((uint8_t*)d, (uint8_t*)d + sz); mz_free(d); }
        if (sa.size() < 52 || memcmp(sa.data(), "OPAA", 4) != 0) return;
        { float fr = findFrameRate(sa); if (fr > 0.f) { animFps = fr; fprintf(stderr, "[OPA] sanim FrameRate (from data) = %.2f\n", fr); } }
        uint32_t hdr; memcpy(&hdr, sa.data()+16, 4); if (hdr < 48 || hdr >= sa.size()) hdr = 48;
        auto rdName = [&](size_t off, std::string& out) -> size_t {
            if (off + 4 > sa.size() || sa[off] != 0xFF || sa[off+1] != 0xFF) return 0;
            uint16_t ln; memcpy(&ln, sa.data()+off+2, 2);
            if (ln == 0 || ln > 64 || off + 4 + ln > sa.size()) return 0;
            for (uint16_t k = 0; k < ln; ++k) { uint8_t ch = sa[off+4+k]; if (ch < 32 || ch > 126) return 0; }
            out.assign((char*)sa.data()+off+4, ln); return 4u + ln; };
        std::string curNode; size_t p = hdr + 4;
        while (p + 4 <= sa.size()) {
            std::string nm; size_t adv = rdName(p, nm);
            if (!adv) { ++p; continue; }
            if (nm == "Translation" || nm == "Rotation" || nm == "Scale") {
                size_t kp = p + adv;
                if (kp + 12 > sa.size()) { p += adv; continue; }
                uint32_t nKeys, nVals;
                memcpy(&nKeys, sa.data()+kp+4, 4); memcpy(&nVals, sa.data()+kp+8, 4);
                // comps come from the CHANNEL, not nKeys: Translation/Scale=3, Rotation=4 (quat); the
                // value block is nVals floats right after the 12B header. Deriving frames from nKeys+1
                // is wrong for flag=1 tracks (the bird's path: nVals=6000, nKeys+1=2001 -> 2001*3≠6000,
                // so it was rejected and the bird never moved). frames = nVals/comps handles both flags.
                int comps = (nm == "Rotation") ? 4 : 3;
                size_t trackEnd = kp + 12 + (size_t)nVals*4;
                if (trackEnd <= sa.size() && nVals >= (uint32_t)comps && (nVals % (uint32_t)comps) == 0) {
                    int nFrames = (int)(nVals / (uint32_t)comps);
                    if (!curNode.empty()) {
                        Track tr; tr.comps = comps; tr.nFrames = nFrames; tr.v.resize(nVals);
                        memcpy(tr.v.data(), sa.data()+kp+12, (size_t)nVals*4);
                        // Recover the authored motion range. Some cooked tracks store the real
                        // keyframes then HOLD the final value to pad out to the master clip length
                        // (e.g. bluehillgoldmine's background train: 625 frames crossing + ~1875
                        // identical "parked" frames). Looping the full track makes the prop sit
                        // still for most of the cycle ("train frozen"). Drop a pure trailing run of
                        // frames identical to the last keyframe so the loop covers only the motion.
                        {
                            // Per-channel trailing-static length (where THIS channel stops changing).
                            // Stored, NOT applied yet — we sync a node's T/R/S together below so they
                            // don't drift out of phase over loops.
                            const float* vv = tr.v.data();
                            auto same = [&](int A,int B){ for(int c=0;c<comps;++c) if (fabsf(vv[(size_t)A*comps+c]-vv[(size_t)B*comps+c])>1e-5f) return false; return true; };
                            int eff = tr.nFrames;
                            while (eff > 2 && same(eff-1, eff-2)) --eff;
                            tr.effFrames = eff;
                        }
                        NodeTracks& ntk = nodeAnim[curNode];
                        if (nm == "Translation") ntk.t = std::move(tr);
                        else if (nm == "Rotation") ntk.r = std::move(tr);
                        else ntk.s = std::move(tr);
                    }
                    p = trackEnd; continue;   // resync (byte-scan) handles any flag=1 trailing key-time data
                }
                p += adv; continue;   // bogus header -> resync by scanning for the next name marker
            }
            curNode = nm; p += adv;     // a node name
        }
        // SYNC each node's channels: loop ALL of T/R/S on the SAME length = the latest frame ANY of
        // them is still moving (max of the per-channel trailing-static lengths). Trimming each channel
        // to its OWN trailing-static point (the old bug) desynced them over loops — the winterlodge
        // tram's small rotation track trimmed shorter than its long translation, so over a few cycles
        // the cabin's orientation drifted out of phase with its position ("elevator flew out / went
        // funny / not looping"). A fully-parked node (background train) still trims (all channels
        // become static together, so the shared max is the parked frame). Each animation thus loops
        // independently on its own true length, with its channels kept in lockstep — faithful.
        animMaxFrames = 0;
        for (auto& kv : nodeAnim) {
            NodeTracks& nt = kv.second;
            int e = 2;
            if (nt.t.nFrames > 0) e = std::max(e, nt.t.effFrames);
            if (nt.r.nFrames > 0) e = std::max(e, nt.r.effFrames);
            if (nt.s.nFrames > 0) e = std::max(e, nt.s.effFrames);
            auto clampN = [&](Track& t){ if (t.nFrames > 0) { int d = t.comps ? (int)(t.v.size()/t.comps) : 0; t.nFrames = std::min(e, d); } };
            clampN(nt.t); clampN(nt.r); clampN(nt.s);
            if (e > animMaxFrames) animMaxFrames = e;
        }
        log("sanim: %zu animated nodes, maxFrames=%d (%.1fs @%.0ffps)",
            nodeAnim.size(), animMaxFrames, animDuration(), animFps);
    }

    // Parse *.mat.sanim.opa -> matUVAnim: per geo/node a "UVTransform" track = nFrames x 2x3 UV
    // matrix (flipbook/scroll for smoke/fire/dust/fog/particles). Same track encoding as sanim.
    void loadMatAnim(const std::vector<uint8_t>& sceneZip) {
        // A scene can ship MULTIPLE *.mat.sanim.opa: one scene-wide (papercraft.fbx.mat.sanim.opa,
        // holds e.g. the waterCard UVTransform) PLUS per-mesh ones (hummingbird_winguv...). libshell
        // loads each cooked mesh's own material-anim, so we parse them ALL and merge by geo name —
        // grabbing only the first (the old bug) missed storybook's animated water -> it rendered the
        // whole 2x2 atlas static (green + black blotches) = the "dark / messed up moving lilypad".
        std::vector<std::vector<uint8_t>> files;
        { mz_zip_archive z; memset(&z, 0, sizeof(z));
          if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
          uint32_t nf = mz_zip_reader_get_num_files(&z);
          for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() >= 14 && fn.compare(fn.size()-14, 14, ".mat.sanim.opa") == 0) {
                size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
                if (d) { files.emplace_back((uint8_t*)d, (uint8_t*)d + sz); mz_free(d); } } }
          mz_zip_reader_end(&z); }
        for (auto& sa : files) {
            if (sa.size() < 52 || memcmp(sa.data(), "OPAA", 4) != 0) continue;
            { float fr = findFrameRate(sa); if (fr > 0.f) animFps = fr; }   // data-driven rate, not hardcoded
            uint32_t hdr; memcpy(&hdr, sa.data()+16, 4); if (hdr < 48 || hdr >= sa.size()) hdr = 48;
            auto rdName = [&](size_t off, std::string& out) -> size_t {
                if (off + 4 > sa.size() || sa[off] != 0xFF || sa[off+1] != 0xFF) return 0;
                uint16_t ln; memcpy(&ln, sa.data()+off+2, 2);
                if (ln == 0 || ln > 64 || off + 4 + ln > sa.size()) return 0;
                for (uint16_t k = 0; k < ln; ++k) { uint8_t ch = sa[off+4+k]; if (ch < 32 || ch > 126) return 0; }
                out.assign((char*)sa.data()+off+4, ln); return 4u + ln; };
            std::string curGeo; size_t p = hdr + 4;
            while (p + 4 <= sa.size()) {
                std::string nm; size_t adv = rdName(p, nm);
                if (!adv) { ++p; continue; }
                if (nm == "UVTransform" || nm == "MaterialTint") {
                    size_t kp = p + adv;
                    if (kp + 12 > sa.size()) { p += adv; continue; }
                    uint32_t nVals; memcpy(&nVals, sa.data()+kp+8, 4);
                    size_t end = kp + 12 + (size_t)nVals*4;
                    if (end > sa.size()) { p += adv; continue; }
                    // UVTransform = a 2x3 UV matrix per frame (6 floats). Derive frame count from the
                    // VALUE block (nVals/6), NOT nKeys+1 — flag=1 tracks store extra key-time data so
                    // nKeys+1 mismatches nVals (same fix loadSanim uses for comps=3/4 node tracks).
                    if (nm == "UVTransform" && nVals >= 6 && (nVals % 6) == 0 && !curGeo.empty()) {
                        UVTrack tr; tr.nFrames = (int)(nVals / 6); tr.m.resize(nVals);
                        memcpy(tr.m.data(), sa.data()+kp+12, (size_t)nVals*4);
                        if (tr.nFrames > animMaxFrames) animMaxFrames = tr.nFrames;
                        matUVAnim[curGeo] = std::move(tr);
                    }
                    // MaterialTint = per-frame RGBA (4 floats/frame); the fog/dust OPACITY animation.
                    if (nm == "MaterialTint" && nVals >= 4 && (nVals % 4) == 0 && !curGeo.empty()) {
                        TintTrack tr; tr.nFrames = (int)(nVals / 4); tr.rgba.resize(nVals);
                        memcpy(tr.rgba.data(), sa.data()+kp+12, (size_t)nVals*4);
                        if (tr.nFrames > animMaxFrames) animMaxFrames = tr.nFrames;
                        matTintAnim[curGeo] = std::move(tr);
                    }
                    p = end; continue;     // track consumed
                }
                curGeo = nm; p += adv;     // a geo/node name
            }
        }
        log("mat.sanim: %zu UV-animated meshes (from %zu files)", matUVAnim.size(), files.size());
    }

    // ── public entry: returns true if this APK is an OPA env we parsed ──
    bool load(const std::string& apkPath) {
        std::vector<uint8_t> sceneZip;
        if (!extractSceneZip(apkPath, sceneZip)) return false;
        // Enumerate geometry *.fbx.opa. A COMPOSED multi-asset scene (underwater/oceanarium) ships
        // MANY world-baked single-mesh *.fbx.opa placed by per-entity files (no instance list /
        // one main model); a normal home ships ONE baked *.fbx.opa with an instance list. Materials
        // (*.mat.opa), skins (*.skin.opa) etc. don't end in ".fbx.opa", so this selects geometry only.
        std::vector<std::string> fbxOpas;
        { mz_zip_archive z; memset(&z,0,sizeof(z));
          if (mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) {
              uint32_t nf = mz_zip_reader_get_num_files(&z);
              for (uint32_t i=0;i<nf;++i){ mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
                  std::string fn(st.m_filename);
                  if (fn.size()>=8 && fn.compare(fn.size()-8,8,".fbx.opa")==0) fbxOpas.push_back(fn); }
              mz_zip_reader_end(&z);
          } }
        if (fbxOpas.empty()) return false;   // not an OPA env
        auto extractNamed = [&](const std::string& nm, std::vector<uint8_t>& out)->bool{
            mz_zip_archive z; memset(&z,0,sizeof(z));
            if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return false;
            size_t sz=0; void* d = mz_zip_reader_extract_file_to_heap(&z, nm.c_str(), &sz, 0);
            mz_zip_reader_end(&z);
            if (!d) return false; out.assign((uint8_t*)d,(uint8_t*)d+sz); mz_free(d); return true;
        };
        parseAssetMeta(sceneZip);   // .mat.txt (blend modes) + .png.asset (AssetId -> tex) FIRST
        loadTextures(sceneZip);
        loadIBL(sceneZip);          // SpecIbl diffuse irradiance cubemap (*_diffuse.dds.opa = RGBA16F KTX)
        loadVatData(sceneZip);      // VAT vertex-animation textures (underwater coral/fish/seaweed)
        loadAnim(sceneZip);
        loadMatAnim(sceneZip);      // mat.sanim UV/flipbook effect animation (smoke/fire/dust/fog)
        // Geo base from a *.fbx.opa name (strip path, leading "sm_", trailing ".fbx.opa") -> VAT key.
        auto opaBase = [](const std::string& nm){ size_t s=nm.find_last_of('/'); std::string b=(s==std::string::npos)?nm:nm.substr(s+1);
            std::string l; for(char c:b) l+=(char)tolower((unsigned char)c);
            size_t d=l.find(".fbx.opa"); if(d!=std::string::npos) l=l.substr(0,d);
            if(l.size()>3 && l.compare(0,3,"sm_")==0) l=l.substr(3); return l; };
        bool ok;
        if (fbxOpas.size() > 1) {
            // Composed scene: load + MERGE every world-baked *.fbx.opa (each parseModel appends to
            // meshes; nodes/materials are per-opa state, textures resolve into each MeshData).
            log("composed scene: merging %zu .fbx.opa", fbxOpas.size());
            int n=0; for (auto& nm : fbxOpas) { std::vector<uint8_t> opa; curOpaBase = opaBase(nm);
                if (extractNamed(nm, opa) && parseModel(opa)) ++n; }
            log("composed scene: merged %d/%zu .fbx.opa -> %zu meshes", n, (size_t)fbxOpas.size(), meshes.size());
            ok = (n > 0);
        } else {
            std::vector<uint8_t> opa;
            if (!extractNamed(fbxOpas[0], opa)) return false;
            log("geometry asset: %s (%zu bytes)", fbxOpas[0].c_str(), opa.size());
            ok = parseModel(opa);
        }
        // Skinned meshes (animals/flags/ships). The LBS math is implemented + confirmed from
        // libshell, BUT correct deform/placement needs the true inverse-bind from the .skel (its
        // compact bind layout isn't decoded yet); the .anim joint matrices are WORLD-space, so
        // applying them without inverseBind spikes/mis-places. Until that's cracked we render the
        // skins STATIC at bind pose (coherent geometry, no spikes). Set HSR_OPA_SKIN to try the
        // (still-WIP) skeletal animation. TODO: decode .skel bind transforms -> real inverseBind.
        loadAnimClips(sceneZip);
        loadSkins(sceneZip);
        if (hasAnimation()) animate(0.0f);
        if (std::getenv("HSR_ANIMDBG") && hasAnimation()) {
            auto centroid = [&](size_t mi, float& x, float& y, float& z){
                x=y=z=0; size_t np=meshes[mi].positions.size()/3; if(!np)return;
                for(size_t q=0;q<np;q++){x+=meshes[mi].positions[q*3];y+=meshes[mi].positions[q*3+1];z+=meshes[mi].positions[q*3+2];}
                x/=np;y/=np;z/=np; };
            log("[ANIMDBG] animRecs=%zu uvAnimRecs=%zu skinRecs=%zu animMaxFrames=%d", animRecs.size(), uvAnimRecs.size(), skinRecs.size(), animMaxFrames);
            for (auto& ar : animRecs) {
                animate(40.0f); float x0,y0,z0; centroid(ar.meshIdx,x0,y0,z0);  // a window that was PARKED before the trim
                animate(45.0f); float x1,y1,z1; centroid(ar.meshIdx,x1,y1,z1);
                float d = sqrtf((x1-x0)*(x1-x0)+(y1-y0)*(y1-y0)+(z1-z0)*(z1-z0));
                if (d > 0.01f || meshes[ar.meshIdx].name.find("train")!=std::string::npos || meshes[ar.meshIdx].name.find("rain")!=std::string::npos) {
                    // raw basePos (cooked, pre-transform) centroid to tell LOCAL vs WORLD space
                    float bx=0,by=0,bz=0; size_t bn=ar.basePos.size()/3;
                    for(size_t q=0;q<bn;q++){bx+=ar.basePos[q*3];by+=ar.basePos[q*3+1];bz+=ar.basePos[q*3+2];}
                    if(bn){bx/=bn;by/=bn;bz/=bn;}
                    log("[ANIMDBG] mesh[%zu] node=%d '%s' rawBase=(%.1f,%.1f,%.1f) world0=(%.1f,%.1f,%.1f) move@5s=%.2f", ar.meshIdx, ar.nodeIdx, meshes[ar.meshIdx].name.c_str(), bx,by,bz, x0,y0,z0, d);
                }
            }
            animate(0.0f);
            // Dump the node chain for any 'train' node: local TRS + parent so we can see if it floats.
            for (size_t i = 0; i < nodes.size(); ++i) {
                std::string ln; for (char ch : nodes[i].name) ln += (char)tolower((unsigned char)ch);
                if (ln.find("train")!=std::string::npos || ln.find("track")!=std::string::npos) {
                    int par = nodes[i].parent;
                    log("[ANIMDBG] node[%zu] '%s' parent=%d('%s') localT=(%.2f,%.2f,%.2f) S=(%.3f,%.3f,%.3f) keyed=%d",
                        i, nodes[i].name.c_str(), par, (par>=0&&par<(int)nodes.size())?nodes[par].name.c_str():"-",
                        nodes[i].t[0],nodes[i].t[1],nodes[i].t[2], nodes[i].s[0],nodes[i].s[1],nodes[i].s[2], (int)nodeAnim.count(nodes[i].name));
                }
            }
        }
        return ok || !meshes.empty();
    }

private:
    // Read the cooked metadata the home ships so rendering is FAITHFUL + GENERAL (works for any
    // OPA home, not just spacestation): *.mat.txt give per-material blend mode + diffuse texture
    // ref; *.png.asset give each texture's AssetId so a material's diffuse Id resolves to a file.
    void parseAssetMeta(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        auto readText = [&](uint32_t i) -> std::string {
            size_t sz=0; void* d=mz_zip_reader_extract_to_heap(&z,i,&sz,0);
            if(!d) return {}; std::string s((char*)d,sz); mz_free(d); return s;
        };
        auto findU64 = [](const std::string& s, size_t from)->uint64_t{
            size_t i=from; while(i<s.size() && !(s[i]>='0'&&s[i]<='9')) ++i;
            uint64_t v=0; bool any=false; for(;i<s.size()&&s[i]>='0'&&s[i]<='9';++i){v=v*10+(s[i]-'0');any=true;}
            return any?v:0;
        };
        for (uint32_t i=0;i<nf;++i) {
            mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
            std::string fn(st.m_filename);
            if (fn.size()>8 && fn.substr(fn.size()-8)==".mat.txt") {
                std::string s = readText(i);
                MatProps mp; mp.found=true;
                auto flag=[&](const char* key)->bool{
                    size_t p=s.find(key); if(p==std::string::npos) return false;
                    size_t c=s.find(':',p); if(c==std::string::npos) return false;
                    size_t t=s.find_first_not_of(" \t",c+1);
                    return t!=std::string::npos && s.compare(t,4,"true")==0;
                };
                mp.transparent = flag("Transparent");
                mp.additive    = flag("Additive");
                mp.alphaTest   = flag("AlphaTest");
                mp.doubleSided = flag("DoubleSided");
                mp.unlit       = flag("Unlit");
                // Color texture ref: ShellEnv materials name it "diffuse"; SpecIbl name it
                // "basecolor" (some "albedo"). CRITICAL: search ONLY inside the "Textures:" section.
                // SpecIbl materials ALSO declare 'basecolor'/'diffuse' UNIFORMS (in the Uniforms
                // section, with a Value but NO texture Id) — matching those gave Id=0 -> ground/
                // terrain/cavern rendered untextured = the "grey/white where it shouldn't be".
                size_t texSec = s.find("Textures:");
                size_t from = (texSec != std::string::npos) ? texSec : 0;
                size_t dp = std::string::npos;
                for (const char* key : {"Name: diffuse","Name:diffuse","Name: basecolor","Name:basecolor","Name: albedo","Name:albedo"}) {
                    size_t p = s.find(key, from);
                    if (p!=std::string::npos && (dp==std::string::npos || p<dp)) dp = p;
                }
                if (dp!=std::string::npos) {
                    size_t idp = s.find("Id:", dp);
                    if (idp!=std::string::npos && idp < dp+200) mp.diffuseId = findU64(s, idp+3);
                    size_t pp = s.find("Path:", dp);
                    if (pp!=std::string::npos && pp < dp+200) {
                        size_t a=s.find_first_not_of(" \t",pp+5), b=s.find_first_of("\r\n",a);
                        std::string path=s.substr(a, b==std::string::npos?std::string::npos:b-a);
                        size_t sl=path.find_last_of("/\\"); if(sl!=std::string::npos) path=path.substr(sl+1);
                        size_t d2=path.find(".png"); if(d2!=std::string::npos) path=path.substr(0,d2);
                        mp.diffuseBase = lc(path);
                    }
                }
                // BAKED lightmap texture ref (Textures section "Name: lightmap" -> Id). The interior
                // SHELL meshes carry their full baked lighting/detail here; resolve it so no-albedo
                // shells (helmet/gem/...) can use it as their visible surface instead of a flat blob.
                if (texSec != std::string::npos) {
                    size_t lp = s.find("Name: lightmap", texSec);
                    if (lp == std::string::npos) lp = s.find("Name:lightmap", texSec);
                    if (lp != std::string::npos) {
                        size_t idp = s.find("Id:", lp);
                        if (idp!=std::string::npos && idp < lp+200) mp.lightmapId = findU64(s, idp+3);
                    }
                }
                // 'diffuse' basecolor UNIFORM (NOT the Textures-section ref). When a material has
                // no texture this IS its flat color (black_mtl=[0,0,0] -> black/invisible; stars=[0.5];
                // SpecIbl terrain=[1,1,1] tint). Search only inside the Uniforms section.
                size_t uniSec = s.find("Uniforms:");
                if (uniSec != std::string::npos) {
                    size_t dn = std::string::npos;
                    for (const char* key : {"Name: diffuse","Name:diffuse","Name: basecolor","Name:basecolor"}) {
                        size_t p = s.find(key, uniSec);
                        if (p!=std::string::npos && (dn==std::string::npos || p<dn)) dn = p;
                    }
                    if (dn != std::string::npos) {
                        size_t vp = s.find("Value", dn);
                        if (vp!=std::string::npos && vp < dn+60) {
                            size_t lim = s.size();
                            for (const char* k : {"UniformProperty","Textures:"}) { size_t p=s.find(k,vp+6); if(p!=std::string::npos&&p<lim) lim=p; }
                            { size_t p=s.find("Name:",vp+6); if(p!=std::string::npos&&p<lim) lim=p; }
                            int got=0; size_t q=s.find(':',vp); if(q!=std::string::npos) ++q;
                            while (got<3 && q!=std::string::npos && q<lim) {
                                bool num  = (s[q]>='0'&&s[q]<='9');
                                bool sign = (s[q]=='-'||s[q]=='.') && q+1<lim && ((s[q+1]>='0'&&s[q+1]<='9')||s[q+1]=='.');
                                if (!num && !sign) { ++q; continue; }
                                size_t s0=q++; while(q<lim && ((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='e'||s[q]=='E'||s[q]=='-'||s[q]=='+')) ++q;
                                try { mp.diffuseColor[got++] = std::stof(s.substr(s0,q-s0)); } catch(...) {}
                            }
                        }
                    }
                    // 'alpha' UNIFORM (first component). It precedes 'alphatestthreshold' in the
                    // Uniforms list, so the first "Name: alpha" whose next char is whitespace/newline
                    // (NOT the 't' of alphatestthreshold) is the one we want.
                    size_t ap = uniSec;
                    while ((ap = s.find("Name:", ap)) != std::string::npos) {
                        size_t c = ap + 5; while (c<s.size() && (s[c]==' '||s[c]=='\t')) ++c;
                        if (s.compare(c,5,"alpha")==0 && c+5<s.size() && s[c+5]!='t') {
                            size_t vp = s.find("Value", c);
                            if (vp!=std::string::npos && vp < c+60) {
                                size_t q = s.find(':', vp); if (q!=std::string::npos) ++q;
                                while (q<s.size() && !((s[q]>='0'&&s[q]<='9')||((s[q]=='-'||s[q]=='.')&&q+1<s.size()&&((s[q+1]>='0'&&s[q+1]<='9')||s[q+1]=='.')))) ++q;
                                size_t s0=q; while(q<s.size() && ((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='e'||s[q]=='E'||s[q]=='-'||s[q]=='+')) ++q;
                                try { mp.alpha = std::stof(s.substr(s0,q-s0)); } catch(...) {}
                            }
                            break;
                        }
                        ap = c;
                    }
                }
                // PBR/SpecIbl scalars (first vec component) — read VERBATIM from the cooked uniforms,
                // for the no-albedo metallic/gem shells' split-sum IBL (divingHelmet, rubyGem, ...).
                mp.isSpecibl = (s.find("Shader: SpecIbl")!=std::string::npos) || (s.find("Shader:SpecIbl")!=std::string::npos);
                auto uniScalar=[&](const char* name, float defv)->float{
                    size_t nl=std::strlen(name), pos=0;
                    while ((pos=s.find("Name:",pos))!=std::string::npos) {
                        size_t c=pos+5; while(c<s.size()&&(s[c]==' '||s[c]=='\t'))++c;
                        if (s.compare(c,nl,name)==0) {
                            char after = (c+nl<s.size())? s[c+nl] : '\n';
                            if (after=='\n'||after=='\r'||after==' '||after=='\t') {  // exact name, not a prefix
                                size_t vp=s.find("Value",c);
                                if (vp!=std::string::npos && vp<c+80) {
                                    size_t q=s.find('[',vp);
                                    if (q!=std::string::npos) { ++q;
                                        while(q<s.size()&&!((s[q]>='0'&&s[q]<='9')||((s[q]=='-'||s[q]=='.')&&q+1<s.size())))++q;
                                        size_t s0=q; while(q<s.size()&&((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='-'||s[q]=='+'||s[q]=='e'||s[q]=='E'))++q;
                                        try { return std::stof(s.substr(s0,q-s0)); } catch(...) {}
                                    }
                                }
                            }
                        }
                        pos=c;
                    }
                    return defv;
                };
                mp.metallic         = uniScalar("metallic", 0.0f);
                mp.roughness        = uniScalar("roughness", 1.0f);
                mp.speciblDiffScale = uniScalar("specibldiffusescale", 1.0f);
                mp.speciblSpecScale = uniScalar("speciblspecularscale", 1.0f);
                // lightmappower: per-channel HDR boost on the baked lightmap = the neon/glow tint.
                // Read all 3 components (libshell: lightmapColor *= lightmappower.rgb). "lightmappowertweaks"
                // is the same slot in some materials. uniScalar reads the first component; pull xyz here.
                auto uniVec3=[&](const char* name, float* out){
                    size_t nl=std::strlen(name), pos=0;
                    while ((pos=s.find("Name:",pos))!=std::string::npos) {
                        size_t c=pos+5; while(c<s.size()&&(s[c]==' '||s[c]=='\t'))++c;
                        if (s.compare(c,nl,name)==0) { char a=(c+nl<s.size())?s[c+nl]:'\n';
                            if (a=='\n'||a=='\r'||a==' '||a=='\t') {
                                size_t q=s.find("Value",c); if(q==std::string::npos||q>=c+80){pos=c;continue;}
                                // Read up to 3 numbers between Value and the NEXT property (handles BOTH
                                // inline `Value: [a, b, c]` and multi-line `Value:\n - a\n - b\n - c`).
                                size_t lim=std::min(s.size(), q+160);
                                for (const char* k : {"UniformProperty","Name:","Textures:"}) { size_t p=s.find(k,q+5); if(p!=std::string::npos&&p<lim) lim=p; }
                                int got=0; size_t i=q+5;
                                while (got<3 && i<lim) {
                                    if ((s[i]>='0'&&s[i]<='9')||((s[i]=='-'||s[i]=='.')&&i+1<lim&&((s[i+1]>='0'&&s[i+1]<='9')||s[i+1]=='.'))) {
                                        size_t s0=i++; while(i<lim&&((s[i]>='0'&&s[i]<='9')||s[i]=='.'||s[i]=='-'||s[i]=='+'||s[i]=='e'||s[i]=='E'))++i;
                                        try { out[got++]=std::stof(s.substr(s0,i-s0)); } catch(...){ }
                                    } else ++i;
                                }
                                return got>0;
                            }
                        }
                        pos=c;
                    }
                    return false;
                };
                if (!uniVec3("lightmappower", mp.lightmapPower)) uniVec3("lightmappowertweaks", mp.lightmapPower);
                matProps[matStem(fn)] = mp;
            } else if (fn.size()>10 && fn.substr(fn.size()-10)==".png.asset") {
                std::string s = readText(i);
                size_t ip = s.find("AssetId:");
                uint64_t id = (ip!=std::string::npos) ? findU64(s, ip+8) : 0;
                size_t sl=fn.find_last_of('/'); std::string base=(sl==std::string::npos)?fn:fn.substr(sl+1);
                size_t d2=base.find(".png.asset"); if(d2!=std::string::npos) base=base.substr(0,d2);
                if (id) assetIdToTexBase[id] = lc(base);
            }
        }
        mz_zip_reader_end(&z);
        log("parsed metadata: %zu materials, %zu texture AssetIds", matProps.size(), assetIdToTexBase.size());
    }

    // Find + decode the SpecIbl DIFFUSE irradiance cubemap (`*_diffuse.dds.opa` / `*hdr*diffuse*`,
    // RGBA16F KTX, 6 faces). The renderer uses it to env-light `*_specibl` materials. (The matching
    // `*_specular.dds.opa` is for the reflection layer — TODO.)
    void loadIBL(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z); int found = -1, foundSpec = -1;
        for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename); std::string lo=fn; for(auto&c:lo)c=(char)tolower((unsigned char)c);
            if (lo.size()>8 && lo.compare(lo.size()-8,8,".dds.opa")==0) {
                if (lo.find("diffuse")  != std::string::npos && found < 0)     found = (int)i;
                if (lo.find("specular") != std::string::npos && foundSpec < 0) foundSpec = (int)i;
            }
        }
        if (found >= 0) {
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, found, &sz, 0);
            if (d) { if (ibl::decodeCubemap((const uint8_t*)d, sz, iblDiffuse))
                         log("IBL diffuse cubemap loaded: %d^2 x6 faces", iblDiffuse.size);
                     else log("IBL diffuse cubemap: decode FAILED");
                     mz_free(d); }
        }
        if (foundSpec >= 0) {   // keep the raw RGBA16F specular bytes AND decode mip0 for CPU sampling
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, foundSpec, &sz, 0);
            if (d) { iblSpecularRaw.assign((uint8_t*)d, (uint8_t*)d + sz);
                     if (ibl::decodeCubemap((const uint8_t*)d, sz, iblSpecular))
                         log("IBL specular cubemap decoded: %d^2 x6 (+%zu raw bytes)", iblSpecular.size, iblSpecularRaw.size());
                     else log("IBL specular cubemap raw kept (%zu bytes, mip0 decode FAILED)", iblSpecularRaw.size());
                     mz_free(d); }
        }
        mz_zip_reader_end(&z);
    }

    // Decode EVERY *.png.opa (OPAA container -> KTX at payload offset -> ASTC -> RGBA), keyed by
    // its cooked basename so any home resolves (spacestation "tx_*", rockquarry "*_diffuse", ...).
    void loadTextures(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        for (uint32_t i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            size_t slash = fn.find_last_of('/');
            std::string base = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
            size_t dot = base.find(".png.opa");
            if (dot == std::string::npos) continue;
            std::string key = lc(base.substr(0, dot));   // full basename (no "tx_" stripping)
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!d) continue;
            if (sz > 48 && memcmp(d, "OPAA", 4) == 0) {
                uint32_t hdr; memcpy(&hdr, (uint8_t*)d + 16, 4); if (hdr < 48 || hdr >= sz) hdr = 48;
                Tex t; t.key = key;
                if (decodeKtxBaseMip((uint8_t*)d + hdr, sz - hdr, t.rgba, t.w, t.h) && !t.rgba.empty()) {
                    // KEEP the real KTX alpha (we render unlit, outputting texture rgb + a). Blend
                    // mode comes from the material's .mat.txt (Transparent/Additive), not a guess;
                    // hasAlpha stays only as a fallback for materials with no .mat.txt.
                    size_t total = t.rgba.size() / 4, lowA = 0;
                    for (size_t px = 3; px < t.rgba.size(); px += 4) if (t.rgba[px] < 128) ++lowA;
                    t.hasAlpha = total && (lowA * 100 > total * 3);
                    textures.push_back(std::move(t));
                }
            }
            mz_free(d);
        }
        mz_zip_reader_end(&z);
        log("decoded %zu textures", textures.size());
    }

    // Decode every *_vatdata.exr.opa. Despite the ".exr" name it's cooked as an UNCOMPRESSED
    // RGBA32F KTX (glType GL_FLOAT 5126, glIntFmt GL_RGBA32F 0x8814): width = #anim verts,
    // height = #frames, each texel.xyz = the per-frame vertex POSITION OFFSET (frame0 = 0 = rest).
    // Keyed by the geo base (strip "t_" + "_vatdata.exr") to match the mesh ("sm_<X>.fbx").
    void loadVatData(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z,0,sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        static const uint8_t kid[12]={0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        for (uint32_t i=0;i<nf;++i){ mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
            std::string fn(st.m_filename);
            size_t slash=fn.find_last_of('/'); std::string base=(slash==std::string::npos)?fn:fn.substr(slash+1);
            size_t dot=base.find("_vatdata.exr.opa"); if(dot==std::string::npos) continue;
            std::string key=lc(base.substr(0,dot));
            if(key.size()>2 && key[0]=='t' && key[1]=='_') key=key.substr(2);     // t_<X> -> <X>
            size_t sz=0; void* dd=mz_zip_reader_extract_to_heap(&z,i,&sz,0); if(!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd,(uint8_t*)dd+sz); mz_free(dd);
            uint32_t hdr; if(file.size()<20){continue;} memcpy(&hdr,file.data()+16,4); if(hdr<48||hdr>=file.size()) hdr=48;
            const uint8_t* k=file.data()+hdr; size_t kn=file.size()-hdr;
            if (kn<80 || memcmp(k,kid,12)!=0) continue;
            auto u=[&](size_t o){ uint32_t v; memcpy(&v,k+o,4); return v; };
            uint32_t glType=u(16), w=u(36), h=u(40), kv=u(60);
            size_t off=64+kv+4;                                  // skip header + kvData + u32 imageSize
            if (glType!=5126 || !w || !h) continue;              // expect GL_FLOAT RGBA32F
            if (off + (size_t)w*h*16 > kn) continue;
            VatData vd; vd.cols=(int)w; vd.frames=(int)h; vd.off.resize((size_t)w*h*3);
            const float* src=(const float*)(k+off);
            for (size_t t=0;t<(size_t)w*h;++t){ vd.off[t*3]=src[t*4]; vd.off[t*3+1]=src[t*4+1]; vd.off[t*3+2]=src[t*4+2]; }
            vatByBase[key]=std::move(vd);
        }
        mz_zip_reader_end(&z);
        if (!vatByBase.empty()) log("loaded %zu VAT vertex-animations", vatByBase.size());
    }
    // Match a material base name to a texture by longest common prefix (the names differ:
    // M_station_tubes_a_01 -> tx_station_tubes_a_03, M_ui_ring_a_dblsided -> tx_ui_ring_a_01).
    const Tex* bestTexFor(const std::string& matBaseLower) const {
        const Tex* best = nullptr; size_t bestLen = 0;
        for (auto& t : textures) {
            size_t l = 0;
            while (l < matBaseLower.size() && l < t.key.size() && matBaseLower[l] == t.key[l]) ++l;
            if (l > bestLen) { bestLen = l; best = &t; }
        }
        return (bestLen >= 3) ? best : nullptr;
    }
    static std::string matBaseName(const std::string& path) {
        size_t a = path.find(".M_"); if (a == std::string::npos) return {};
        a += 3; size_t b = path.find(".mat.asset");
        std::string s = (b != std::string::npos && b > a) ? path.substr(a, b - a) : path.substr(a);
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }

    static bool extractSceneZip(const std::string& apkPath, std::vector<uint8_t>& out) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_file(&z, apkPath.c_str(), 0)) return false;
        int idx = mz_zip_reader_locate_file(&z, "assets/scene.zip", nullptr, 0);
        if (idx < 0) { mz_zip_reader_end(&z); return false; }
        size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
        mz_zip_reader_end(&z);
        if (!d) return false;
        out.assign((uint8_t*)d, (uint8_t*)d + sz);
        mz_free(d);
        return true;
    }
    // Find the geometry .opa: a "*.fbx.opa" that is NOT a per-material "*.mat.opa".
    static bool findFbxOpa(const std::vector<uint8_t>& sceneZip, std::vector<uint8_t>& out, std::string& nameOut) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return false;
        u32 nf = mz_zip_reader_get_num_files(&z);
        int best = -1; size_t bestSz = 0; std::string bestName;
        for (u32 i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            auto ends = [&](const char* suf) {
                size_t l = strlen(suf);
                return fn.size() >= l && fn.compare(fn.size()-l, l, suf) == 0;
            };
            if (ends(".fbx.opa") && !ends(".mat.opa")) {
                // pick the largest (the geometry blob dwarfs anything else)
                if ((size_t)st.m_uncomp_size > bestSz) { best = (int)i; bestSz = (size_t)st.m_uncomp_size; bestName = fn; }
            }
        }
        if (best < 0) { mz_zip_reader_end(&z); return false; }
        size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, best, &sz, 0);
        mz_zip_reader_end(&z);
        if (!d) return false;
        out.assign((uint8_t*)d, (uint8_t*)d + sz);
        mz_free(d);
        nameOut = bestName;
        return true;
    }

    void computeNodeWorld() {
        nodeWorld.assign(nodes.size(), identity());
        // nodes are stored parent-before-child here (RootNode first); compute in order,
        // falling back to identity for any out-of-range parent.
        for (size_t i = 0; i < nodes.size(); ++i) {
            Mat4 local = trs(nodes[i].t, nodes[i].r, nodes[i].s);
            int par = nodes[i].parent;
            if (par >= 0 && par < (int)i) nodeWorld[i] = mul(nodeWorld[par], local);
            else nodeWorld[i] = local;
        }
    }

    // ── *.anim.opa skeletal clip: per-frame, per-joint 4x4 SKINNING matrices (pre-multiplied
    // jointWorld*inverseBind; frame0 ~= identity). Confirmed via libshell's skinning vertex shader
    // (linear blend skinning): localPos = Σ Joints[idx_i]*pos * weight_i. ────────────────────
    // .skel.opa (positional, like V79 libshell Skeleton.cpp): ver, extra, jointIds[u64], jointNames
    // [str], jointParents[i32], jointLocalPoses[T3 f32 + R4 f32 (w,x,y,z) + S3 f32]. -> parents + bind LOCAL 4x4.
    bool parseSkel(const std::vector<uint8_t>& file, std::vector<int>& parents, std::vector<float>& bindLocal) {
        if (file.size() < 52 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdr; memcpy(&hdr, file.data()+16, 4); if (hdr < 48 || hdr >= file.size()) hdr = 48;
        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdr;
        uint32_t sver = c.u32();                       // ver
        if (sver >= 0x408) c.u32();                    // extra(=1) — ONLY ver>=0x408; ver 0x405 has none
                                                       // (reading it unconditionally desynced -> 0 joints ->
                                                       //  spacestation's 25 vehicle bones never loaded -> ships frozen)
        uint32_t nIds = c.u32();   for (uint32_t i=0;i<nIds  && c.ok;++i) c.u64();   // jointIds  (skip)
        uint32_t nNm  = c.u32();   for (uint32_t i=0;i<nNm   && c.ok;++i) c.str();   // jointNames(skip)
        uint32_t nPar = c.u32();   parents.resize(nPar); for (uint32_t i=0;i<nPar && c.ok;++i) parents[i]=c.i32();
        uint32_t nPose= c.u32();
        if (!c.ok || nPose != nPar || nPose == 0) return false;
        bindLocal.resize((size_t)nPose*16);
        for (uint32_t j=0;j<nPose && c.ok;++j) {
            float T[3]={c.f32(),c.f32(),c.f32()};
            float Rw=c.f32(),Rx=c.f32(),Ry=c.f32(),Rz=c.f32();   // skel quat = (w,x,y,z)
            float S[3]={c.f32(),c.f32(),c.f32()};
            float q[4]={Rx,Ry,Rz,Rw};                            // trs() wants (x,y,z,w)
            Mat4 m = trs(T, q, S);
            memcpy(bindLocal.data()+(size_t)j*16, m.m, 16*sizeof(float));
        }
        return c.ok;
    }
    void loadAnimClips(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        auto baseOf = [](const std::string& fn, const char* suf)->std::string{
            size_t sl=fn.find_last_of('/'); std::string b=(sl==std::string::npos)?fn:fn.substr(sl+1);
            size_t sp=b.rfind(suf); if(sp!=std::string::npos) b=b.substr(0,sp); return lc(b); };
        // pass 1: skeletons (parents + bind local)
        for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".skel.opa") continue;
            size_t sz=0; void* dd=mz_zip_reader_extract_to_heap(&z,i,&sz,0); if(!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd,(uint8_t*)dd+sz); mz_free(dd);
            std::vector<int> par; std::vector<float> bl;
            if (parseSkel(file, par, bl)) skelData[baseOf(fn,".skel.opa")] = { std::move(par), std::move(bl) };
        }
        // pass 2: anim clips, attach the matching skel -> compute inverse bind WORLD
        for (uint32_t i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".anim.opa") continue;
            if (fn.find(".sanim.") != std::string::npos) continue;   // sanim = node TRS (handled elsewhere)
            size_t sz = 0; void* dd = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd, (uint8_t*)dd + sz); mz_free(dd);
            AnimClip clip;
            if (parseAnimClip(file, clip) && clip.numFrames > 1 && clip.numJoints > 0) {
                auto sit = skelData.find(baseOf(fn, ".anim.opa"));
                if (sit != skelData.end() && (int)sit->second.first.size() == clip.numJoints
                                          && (int)sit->second.second.size() == clip.numJoints*16) {
                    clip.parents = sit->second.first;
                    const std::vector<float>& bl = sit->second.second;
                    std::vector<float> bw((size_t)clip.numJoints*16);    // bind WORLD = compose(bindLocal,parents)
                    for (int j=0;j<clip.numJoints;++j) { int p=clip.parents[j];
                        if (p<0||p>=j) memcpy(bw.data()+(size_t)j*16, bl.data()+(size_t)j*16, 16*sizeof(float));
                        else mat4mul(bw.data()+(size_t)p*16, bl.data()+(size_t)j*16, bw.data()+(size_t)j*16); }
                    clip.invBind.resize((size_t)clip.numJoints*16);
                    for (int j=0;j<clip.numJoints;++j) mat4affineInverse(bw.data()+(size_t)j*16, clip.invBind.data()+(size_t)j*16);
                }
                int ci = (int)clips.size();
                for (int j = 0; j < (int)clip.joints.size(); ++j) jointToClip[clip.joints[j]] = {ci, j};
                if (clip.numFrames > animMaxFrames) animMaxFrames = clip.numFrames;
                clips.push_back(std::move(clip));
            }
        }
        mz_zip_reader_end(&z);
        log("loaded %zu skeletal anim clips (%zu skeletons)", clips.size(), skelData.size());
    }
    bool parseAnimClip(const std::vector<uint8_t>& file, AnimClip& clip) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4); if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;
        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdrSize;
        c.u32();                                  // ver
        uint32_t jc = c.u32();
        if (c.p + 2 <= c.n) { uint16_t m; memcpy(&m, c.d+c.p, 2); if (m != 0xFFFF) jc = c.u32(); } // skip extra field
        for (uint32_t j = 0; j < jc && c.ok; ++j) clip.joints.push_back(c.str());
        if (!c.ok) return false;
        // After the joint names there's a skeleton block (parent indices / bind data) before the
        // "Transform" track — scan to it rather than assuming it's immediate.
        size_t tp = std::string::npos;
        for (size_t q = c.p; q + 9 <= c.n; ++q) if (memcmp(c.d+q, "Transform", 9) == 0) { tp = q; break; }
        if (tp == std::string::npos || tp < 4) return false;
        c.p = tp - 4;                             // back to the string record (ffff + len)
        std::string track = c.str();              // "Transform"
        if (!c.ok || track != "Transform") return false;
        c.u32();                                  // flag
        c.u32();                                  // nKeys
        uint32_t nVals = c.u32();
        if (!jc || nVals == 0 || (nVals % (jc*16)) != 0) return false;
        if (!c.avail((size_t)nVals*4)) return false;
        clip.numJoints = (int)jc; clip.numFrames = (int)(nVals / (jc*16));
        clip.mats.resize(nVals);
        memcpy(clip.mats.data(), c.d + c.p, (size_t)nVals*4);   // per-frame LOCAL joint poses
        return true;
    }

    // ── Skinned meshes (*.skin.opa): the horses/owl/chickens/flags/spaceships ──────────────
    // The .skin.opa container = ver, a material ref (Id + Path to a .mat.asset), then the geometry
    // (posFmt "SkinnedPos" [stride 24 = pos f32x3 + weights f16x4 + bone idx u8x4], dataFmt
    // "StdData" [stride 20, uv f16x4 @12], indices "kUnsignedShort"), then a joint name list. We
    // render it at BIND POSE (the SkinnedPos verts are already in model space) so the animals/
    // ships APPEAR; full CPU skeletal skinning (via *.skel/*.anim) is a follow-up. Robust to the
    // exact container header by SCANNING for the "SkinnedPos" posFmt marker.
    void loadSkins(const std::vector<uint8_t>& sceneZip) {
        if (std::getenv("HSR_DUMPNODES")) { log("=== scene nodes: %zu ===", nodes.size());
            for (size_t k=0;k<nodes.size();++k){ const float* W=(k<nodeWorld.size())?nodeWorld[k].m:nullptr;
                log("node[%zu] '%s' par=%d t=(%.2f,%.2f,%.2f)", k, nodes[k].name.c_str(), nodes[k].parent, W?W[12]:0.f, W?W[13]:0.f, W?W[14]:0.f); } }
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        int loaded = 0;
        for (uint32_t i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".skin.opa") continue;
            size_t sz = 0; void* dd = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd, (uint8_t*)dd + sz); mz_free(dd);
            if (parseSkin(file, fn)) ++loaded;
        }
        mz_zip_reader_end(&z);
        log("loaded %d skinned meshes (bind pose)", loaded);
    }

    bool parseSkin(const std::vector<uint8_t>& file, const std::string& fn) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4); if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;
        uint32_t ver; memcpy(&ver, file.data()+hdrSize, 4);
        auto findStr = [&](const char* needle, size_t nlen, size_t from) -> size_t {
            for (size_t i = from; i + nlen <= file.size(); ++i)
                if (memcmp(file.data()+i, needle, nlen) == 0) return i;
            return std::string::npos;
        };
        // material .mat.asset path (for texture/blend resolution)
        std::string matRef;
        { size_t m = findStr(".mat.asset", 10, 0);
          if (m != std::string::npos) { size_t s = m + 10; while (s > 0) { char ch = (char)file[s-1];
              if (isalnum((unsigned char)ch) || ch=='_'||ch=='-'||ch=='.'||ch=='/') --s; else break; }
              matRef = std::string((const char*)file.data()+s, (m+10)-s); } }
        size_t sp = findStr("SkinnedPos", 10, hdrSize);
        if (sp == std::string::npos) return false;
        Cur c; c.d = file.data(); c.n = file.size(); c.p = sp - 4;   // at the posFmt string record
        std::string posFmt = c.str();      // "SkinnedPos"
        std::string dataFmt = c.str();     // "StdData"
        uint32_t listCount = c.u32(); if (listCount != 0) return false;
        uint32_t submeshCount = c.u32();
        struct SubR { uint32_t baseVertex, firstIndex, indexCount, matIndex; };
        std::vector<SubR> subs; subs.reserve(submeshCount);
        for (uint32_t s = 0; s < submeshCount && c.ok; ++s) {
            uint32_t bv=c.u32(), fi=c.u32(), ic=c.u32(), mi=c.u32();
            // Submesh extras by version — IDENTICAL to parseModel (the working 0x409 rigid path):
            // 0x407/0x408 add 2 u32, 0x409 adds 2 MORE (4 total). The old per-submesh skip(24)+u8 here
            // was wrong (parseModel has no per-submesh AABB) and made every 0x409 skin fail to parse
            // -> "loaded 0 skinned meshes" (lakeside tent/trees never reached the GPU).
            if (ver >= 0x407) { c.u32(); c.u32(); }
            if (ver >= 0x409) { c.u32(); c.u32(); }
            c.skip(24); c.u8();   // per-submesh AABB (24) + flag (1) — the SKIN has this, the rigid model doesn't
            subs.push_back({bv, fi, ic, mi});
        }
        c.skip(24); uint32_t vertCount = c.u32(); (void)vertCount;
        uint32_t posBytes = c.u32(); size_t posOff = c.p; c.skip(posBytes);
        uint32_t stdBytes = c.u32(); size_t stdOff = c.p; c.skip(stdBytes);
        uint32_t idxCnt = c.u32(); (void)idxCnt;
        std::string idxType = c.str();
        uint32_t idxBytes = c.u32(); size_t idxOff = c.p; c.skip(idxBytes);
        if (!c.ok || idxType != "kUnsignedShort") return false;
        uint32_t posStride = (posFmt.find("Skinned") != std::string::npos) ? 24 : 12;
        uint32_t nv = posBytes / posStride, nstd = stdBytes / 20, nidx = idxBytes / 2;
        const uint8_t* posP = c.at(posOff); const uint8_t* stdP = c.at(stdOff);
        const uint16_t* idxP = (const uint16_t*)c.at(idxOff);
        // texture/blend from the referenced material (.mat.txt metadata)
        const Tex* tex = nullptr; const MatProps* mp = nullptr;
        { std::string stem = matStem(matRef); auto it = matProps.find(stem); if (it != matProps.end()) mp = &it->second;
          if (mp) { if (!mp->diffuseBase.empty()) tex = texByBase(mp->diffuseBase);
                    if (!tex && mp->diffuseId) { auto a = assetIdToTexBase.find(mp->diffuseId); if (a != assetIdToTexBase.end()) tex = texByBase(a->second); } } }
        static int novflip = -1; if (novflip < 0) novflip = std::getenv("HSR_NOVFLIP") ? 1 : 0;
        // Skin's own joint name list (after the index data) -> map each bone index to a clip joint.
        // The skin's joints share names (and usually order) with its .anim clip. bone idx (u8x4)
        // indexes THIS list; we resolve name -> jointToClip to get the (clip, jointInClip).
        // ── Skin tail (libshell-faithful, DETERMINISTIC) ──────────────────────────────────
        // After the index data comes a u16 tail (ver>=0x404, same as the model mesh entry),
        // then THREE count-prefixed lists: jointNames [u32 cnt][cnt str], jointIds
        // [u32 cnt][cnt u64], and the per-bone INVERSE-BIND [u32 cnt][cnt 4x4 col-major].
        // libshell's string reader (sub_AEF1EC) copies raw length-prefixed bytes with NO
        // character filter, so joint names may contain ':' (Maya/FBX namespaces, e.g.
        // "tent_a_cloth_dembones:tentA1_joint" — the Asgard's Wrath DemBones tents). The bone
        // idx (u8x4 in SkinnedPos) indexes THIS jointNames list; resolve name -> jointToClip.
        std::vector<std::string> skinJoints;
        std::vector<float> skinInvBind;
        {
            Cur t = c;                                   // positioned right after the index data
            if (ver >= 0x404) t.skip(2);                 // u16 tail
            uint32_t jnCount = t.u32();
            bool good = t.ok && jnCount >= 1 && jnCount <= 512;
            std::vector<std::string> names;
            for (uint32_t j = 0; good && j < jnCount; ++j) { std::string s = t.str(); if (!t.ok) { good = false; break; } names.push_back(std::move(s)); }
            if (good) { uint32_t jidCount = t.u32(); if (t.ok && jidCount <= 512) t.skip((size_t)jidCount*8); else good = false; }
            if (good) { uint32_t ibCount = t.u32();
                if (t.ok && ibCount == jnCount && t.avail((size_t)ibCount*64)) {
                    skinJoints = std::move(names);
                    skinInvBind.resize((size_t)ibCount*16);
                    memcpy(skinInvBind.data(), t.d + t.p, (size_t)ibCount*64);
                } }
        }
        // Fallback (robust to any container quirk): scan for length-prefixed name records
        // (':' allowed) and take the trailing [u32 cnt][cnt 4x4] invBind block.
        if (skinJoints.empty() || skinInvBind.empty()) {
            skinJoints.clear(); skinInvBind.clear();
            size_t q = idxOff + (size_t)idxBytes;
            while (q + 4 <= file.size() && skinJoints.size() < 512) {
                uint16_t mk; memcpy(&mk, file.data()+q, 2);
                if (mk == 0xFFFF) { uint16_t L; memcpy(&L, file.data()+q+2, 2);
                    if (L >= 2 && L <= 64 && q+4+L <= file.size()) {
                        bool nameish = true; for (uint16_t t=0;t<L;++t){ char ch=(char)file[q+4+t]; if(!(isalnum((unsigned char)ch)||ch=='_'||ch=='-'||ch==':')){nameish=false;break;} }
                        if (nameish) { skinJoints.push_back(std::string((const char*)file.data()+q+4, L)); q += 4+L; continue; } } }
                ++q; }
            int nJ = (int)skinJoints.size();
            if (nJ > 0 && file.size() >= (size_t)nJ*64 + 4) {
                size_t cntOff = file.size() - (size_t)nJ*64 - 4;
                uint32_t cnt; memcpy(&cnt, file.data()+cntOff, 4);
                if (cnt == (uint32_t)nJ) { skinInvBind.resize((size_t)nJ*16); memcpy(skinInvBind.data(), file.data()+cntOff+4, (size_t)nJ*64); }
            }
        }
        int skinClip = -1;
        std::vector<int> boneToClipJoint(skinJoints.size(), -1);
        for (size_t b = 0; b < skinJoints.size(); ++b) {
            auto it = jointToClip.find(skinJoints[b]);
            if (it != jointToClip.end()) { boneToClipJoint[b] = it->second.second; if (skinClip < 0) skinClip = it->second.first; }
        }
        bool canSkin = (skinClip >= 0) && !skinInvBind.empty();
        int emitted = 0;
        for (auto& sub : subs) {
            if (sub.indexCount == 0 || sub.firstIndex + sub.indexCount > nidx) continue;
            MeshData md; md.name = fn;
            std::unordered_map<uint32_t,uint32_t> remap; remap.reserve(sub.indexCount);
            md.indices.reserve(sub.indexCount);
            SkinRec sr; sr.clipIdx = skinClip;
            for (uint32_t k = 0; k < sub.indexCount; ++k) {
                uint32_t oi = sub.baseVertex + idxP[sub.firstIndex + k];
                if (oi >= nv) oi = 0;
                auto it = remap.find(oi); uint32_t ni;
                if (it == remap.end()) {
                    ni = (uint32_t)(md.positions.size()/3); remap.emplace(oi, ni);
                    float px, py, pz; memcpy(&px, posP+oi*posStride+0, 4); memcpy(&py, posP+oi*posStride+4, 4); memcpy(&pz, posP+oi*posStride+8, 4);
                    md.positions.push_back(px); md.positions.push_back(py); md.positions.push_back(pz);  // bind verts (model space)
                    float u=0, v=0;
                    if (oi < nstd) { uint16_t hu, hv; memcpy(&hu, stdP+oi*20+12, 2); memcpy(&hv, stdP+oi*20+14, 2); u=h2f(hu); v=h2f(hv); if (!novflip) v=1.0f-v; }
                    md.uvs.push_back(u); md.uvs.push_back(v);
                    if (canSkin) {   // SkinnedPos: weights f16x4 @12, bone idx u8x4 @20 (jidx = SKIN bone)
                        const uint8_t* sv = posP + oi*posStride;
                        for (int w=0; w<4; ++w) {
                            uint16_t hw; memcpy(&hw, sv+12+w*2, 2); float wt = h2f(hw);
                            uint8_t bi = sv[20+w];
                            bool ok = (bi < boneToClipJoint.size() && boneToClipJoint[bi] >= 0);
                            sr.jidx.push_back(ok ? (int)bi : 0); sr.jw.push_back(ok ? wt : 0.0f);
                        }
                    }
                } else ni = it->second;
                md.indices.push_back((u32)ni);
            }
            md.nVerts = (u32)(md.positions.size()/3); md.nIdx = (u32)md.indices.size();
            if (tex) { md.texW=tex->w; md.texH=tex->h; md.hasTexture=true; md.texRGBA=tex->rgba;
                       md.tint[0]=md.tint[1]=md.tint[2]=1.0f; md.useBlend = mp ? (mp->transparent||mp->additive) : tex->hasAlpha;
                       md.alphaTest = mp ? mp->alphaTest : false; }
            else     { md.texW=md.texH=1; md.hasTexture=true;   // no texture -> flat diffuse UNIFORM color (not grey)
                       float dr=mp?mp->diffuseColor[0]:1.f, dg=mp?mp->diffuseColor[1]:1.f, db=mp?mp->diffuseColor[2]:1.f;
                       auto cl=[](float x){return (u8)(x<0?0:x>1?255:x*255.f+0.5f);};
                       md.texRGBA={cl(dr),cl(dg),cl(db),255}; }
            if (canSkin) {   // CPU linear-blend skinning every frame (animate()) — bind pose at t=0
                md.dynamicVerts = true;
                sr.meshIdx = meshes.size(); sr.basePos = md.positions;
                sr.boneClip = boneToClipJoint; sr.invBind = skinInvBind; sr.nJoints = (int)skinJoints.size();
                skinRecs.push_back(std::move(sr));
            }
            meshes.push_back(std::move(md)); ++emitted;
        }
        return emitted > 0;
    }

    bool parseModel(const std::vector<uint8_t>& file) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) { log("not an OPAA container"); return false; }
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4);
        if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;

        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdrSize;
        uint32_t ver = c.u32();
        // Newer cooked versions (seen in 0x408 stock Meta homes, e.g. bluehillgoldmine) insert an
        // extra u32 (=1) between the version and the root type-name string. The type name is always
        // the literal "MeshData", encoded with the long-string marker 0xFFFF — so if 0xFFFF isn't
        // the very next thing, consume the inserted field. (Robust across versions, not hardcoded.)
        if (c.p + 2 <= c.n) {
            uint16_t mark; memcpy(&mark, c.d + c.p, 2);
            if (mark != 0xFFFF) c.skip(4);
        }
        std::string type = c.str();
        log("version=0x%X type=%s", ver, type.c_str());
        if (!c.ok || type != "MeshData") { log("unexpected type"); return false; }

        // ── nodes ──
        uint32_t nodeCount = c.u32();
        nodes.clear(); nodes.reserve(nodeCount);
        for (uint32_t i = 0; i < nodeCount && c.ok; ++i) {
            Node nd;
            c.skip(8);                 // lead 8 bytes
            nd.name = c.str();
            nd.parent = c.i32();
            for (int k = 0; k < 3; ++k) nd.t[k] = c.f32();
            // OPA stores the quaternion W-FIRST (w,x,y,z) — Meta's cooked-asset convention — but
            // trs() (like glTF) wants (x,y,z,w). Reading it in file order made the RootNode's
            // identity (1,0,0,0=w-first) parse as x=1,w=0 = a 180° rotation about X -> the WHOLE
            // home rendered upside down (spacestation, bluehillgoldmine). Reorder to (x,y,z,w).
            { float qw=c.f32(), qx=c.f32(), qy=c.f32(), qz=c.f32();
              nd.r[0]=qx; nd.r[1]=qy; nd.r[2]=qz; nd.r[3]=qw; }
            for (int k = 0; k < 3; ++k) nd.s[k] = c.f32();
            nodes.push_back(nd);
        }
        if (!c.ok) { log("node parse failed"); return false; }
        computeNodeWorld();
        // Append THIS .fbx.opa's nodes to the persistent global list (parents offset) so the animRecs
        // created below can index their OWN nodes for the whole scene's lifetime (see animNodes decl).
        size_t animNodeBase = animNodes.size();
        for (const Node& nd : nodes) {
            Node n2 = nd;
            if (n2.parent >= 0) n2.parent += (int)animNodeBase;
            animNodes.push_back(n2);
        }
        log("nodes=%u (animNodeBase=%zu)", nodeCount, animNodeBase);

        // ── materials ──
        uint32_t matCount = c.u32();
        materials.clear(); materials.reserve(matCount);
        for (uint32_t i = 0; i < matCount && c.ok; ++i) {
            Mat mt;
            while (c.ok) {
                uint8_t tag = c.u8();
                if (tag == 0xC8) break;          // sub_AEF158: 0xC8 == end-of-object
                std::string fn = c.str();
                if (fn == "Id")        mt.id = c.u64();
                else if (fn == "Path") mt.path = c.str();
                else { log("material[%u] unknown field '%s' @%zu — cannot skip", i, fn.c_str(), c.p); return false; }
            }
            materials.push_back(mt);
        }
        if (!c.ok) { log("material parse failed"); return false; }
        log("materials=%u", matCount);

        // ── mesh entries ──
        uint32_t meshCount = c.u32();
        log("meshCount=%u", meshCount);
        // Parse mesh entries as GEOMETRY ONLY (offsets into the file). The mesh<->node placement
        // is the INSTANCE LIST that follows (read below) — NOT a 1:1 entry/node guess.
        struct SubR { uint32_t baseVertex, firstIndex, indexCount, matIndex; };
        struct MeshEntry { size_t posOff=0, stdOff=0, idxOff=0; uint32_t nv=0, nstd=0, nidx=0; bool ok=false; std::vector<SubR> subs; };
        std::vector<MeshEntry> entries; entries.reserve(meshCount);
        for (uint32_t e = 0; e < meshCount && c.ok; ++e) {
            std::string posFmt = c.str();
            std::string dataFmt = c.str();
            uint32_t listCount = c.u32();
            if (listCount != 0) { log("entry[%u] AF0A94 list count=%u unsupported", e, listCount); return false; }
            uint32_t submeshCount = c.u32();
            MeshEntry me; me.subs.reserve(submeshCount);
            for (uint32_t s = 0; s < submeshCount && c.ok; ++s) {
                // field[0] = baseVertex (drawIndexed vertexOffset; cooked meshes split into
                // <=65535-vert u16-indexable chunks). Then firstIndex, indexCount, matIndex.
                uint32_t baseVertex = c.u32();
                uint32_t firstIndex = c.u32();
                uint32_t indexCount = c.u32();
                uint32_t matIndex   = c.u32();
                if (ver >= 0x407) { c.u32(); c.u32(); }   // 2 extra u32 in newer cooks (0x407/0x408)
                if (ver >= 0x409) { c.u32(); c.u32(); }   // 0x409 (underwater/oceanarium) adds 2 MORE (4 total)
                c.skip(24);                                // submesh AABB (min/max vec3)
                c.u8();                                    // bool
                me.subs.push_back({baseVertex, firstIndex, indexCount, matIndex});
            }
            c.skip(24);                                    // whole-mesh AABB
            uint32_t vertCount = c.u32();                  // (== posBytes/12)
            uint32_t posBytes = c.u32(); me.posOff = c.p; c.skip(posBytes);
            uint32_t stdBytes = c.u32(); me.stdOff = c.p; c.skip(stdBytes);
            uint32_t idxCnt   = c.u32();
            std::string idxType = c.str();
            uint32_t idxBytes = c.u32(); me.idxOff = c.p; c.skip(idxBytes);
            if (ver >= 0x404) c.u16();                      // tail
            if (!c.ok) { log("entry[%u] truncated", e); return false; }
            me.nv = posBytes / 12; me.nstd = stdBytes / 20; me.nidx = idxBytes / 2;
            me.ok = (idxType == "kUnsignedShort");
            (void)vertCount; (void)dataFmt; (void)posFmt; (void)idxCnt;
            log("entry[%u] nv=%u std=%u idx=%u submeshes=%zu%s", e, me.nv, me.nstd, me.nidx,
                me.subs.size(), me.ok ? "" : " (idxType unsupported)");
            entries.push_back(std::move(me));
        }
        if (!c.ok) { log("mesh parse failed"); return false; }

        // ── INSTANCE LIST (libshell sub_AF1D74 -> sub_AF1E88): the AUTHORITATIVE mesh<->node map ──
        // [u32 count] then per instance [u32 nodeIndex][u32 meshIndex] (+3 u32 @ver>=0x407, more
        // @ver>=0x409). libshell draws mesh[meshIndex] at node[nodeIndex]'s transform. ONE mesh can
        // be instanced at MANY nodes (one car mesh -> several animated car_strip nodes); STATIC
        // meshes (vista/interior) land on static nodes -> no more "whole world moves", and props
        // (the fan) get their real node transform (on the roof, not the floor).
        struct Inst { uint32_t node, mesh, cell; };
        std::vector<Inst> instances;   // (nodeIdx, meshIdx, atlasCell)
        int atlasGrid = 1;             // >1 => per-instance variation atlas (remap UV0 into cell)
        {
            uint32_t instCount = c.ok ? c.u32() : 0;
            // Instance record size by version: 0x409 = 11 u32 ([node][mesh] + 9 extra), 0x407/0x408 = 5,
            // older = 2. The 0x409 record is 44 bytes (libshell sub_AF1D74/sub_AF1E88): [0]=node [1]=mesh
            // [3]=rotation [4]=ATLAS CELL INDEX [5]=scale [7..10]=color(rgba). field[4] is the per-instance
            // diffuse-atlas cell: the cooker bakes one texture VARIANT per instance into a square atlas, and
            // each instance samples ITS cell. The mesh's UV0 is authored in [0,1] (one cell); without the
            // remap a single coral samples the WHOLE atlas -> scrambled patchwork (the "bad texture").
            int perInst = (ver >= 0x409) ? 11 : (ver >= 0x407) ? 5 : 2;
            static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
            uint32_t f4max=0; bool f4varies=false; uint32_t f4prev=0;
            for (uint32_t i = 0; i < instCount && c.ok; ++i) {
                uint32_t rec[16] = {0};
                uint32_t nodeIdx = rec[0] = c.u32();
                uint32_t meshIdx = rec[1] = c.u32();
                for (int k = 2; k < perInst; ++k) rec[k] = c.u32();   // skip extra fields (LOD/material override)
                uint32_t cell = (perInst >= 5) ? rec[4] : 0;
                if (perInst >= 5) { if(cell>f4max)f4max=cell; if(i>0 && cell!=f4prev)f4varies=true; f4prev=cell; }
                instances.push_back({nodeIdx, meshIdx, cell});
            }
            // Square atlas grid: cells 0..f4max packed row-major in a ceil(sqrt(N))² grid (matches the
            // decoded atlases: angelfish 2x2, antlercoral 4x4, braincoral 7x7). Only meshes whose cell
            // index actually VARIES across instances are atlas-variation meshes; tiled/static props keep
            // field4==0 (no remap).
            if (f4varies) { int n = (int)f4max + 1; atlasGrid = (int)std::ceil(std::sqrt((double)n)); if (atlasGrid < 1) atlasGrid = 1; }
            log("instances=%zu (perInst=%d) atlasGrid=%d", instances.size(), perInst, atlasGrid);
        }
        // Fallback for cooks with no instance list -> 1:1 entry[i] at node[i].
        if (instances.empty())
            for (uint32_t e = 0; e < (uint32_t)entries.size(); ++e) instances.push_back({e, e, 0});

        // VAT (Vertex Animation Texture) mesh? (curOpaBase is set by the composed-scene loader)
        static int novat = -1; if (novat<0) novat = std::getenv("HSR_NOVAT") ? 1 : 0;
        const VatData* curVat = (!novat && vatByBase.count(curOpaBase)) ? &vatByBase[curOpaBase] : nullptr;

        // ── emit one renderable MeshData per (instance, submesh) ──
        for (auto& inst : instances) {
            uint32_t nodeIdx = inst.node, meshIdx = inst.mesh;
            // Per-instance diffuse-atlas cell remap (see INSTANCE LIST note): map this instance's
            // UV0 (authored in [0,1]) into its atlas cell so each variant shows ONE coherent texture.
            int aCol = atlasGrid>1 ? (int)(inst.cell % (uint32_t)atlasGrid) : 0;
            int aRow = atlasGrid>1 ? (int)(inst.cell / (uint32_t)atlasGrid) : 0;
            if (meshIdx >= entries.size()) continue;
            MeshEntry& me = entries[meshIdx];
            if (!me.ok || me.subs.empty()) continue;
            // Animated -> keep LOCAL verts; animate() applies the node's full animated world matrix
            // each frame. Static -> bake its full world transform into the verts. A mesh counts as
            // animated if its own node OR ANY ANCESTOR is keyed in the sanim (e.g. the bird mesh sits
            // under the animated `birdBody_path` parent — only the parent moves), matching libshell's
            // scene-graph propagation.
            bool animated = false;
            for (int an = (int)nodeIdx; an >= 0 && an < (int)nodes.size(); an = nodes[an].parent)
                if (nodeAnim.count(nodes[an].name) > 0) { animated = true; break; }
            Mat4 world = animated ? identity()
                                  : ((nodeIdx < nodeWorld.size()) ? nodeWorld[nodeIdx] : identity());
            Mat4 parentWorld = identity();
            if (animated) {
                int par = nodes[nodeIdx].parent;
                if (par >= 0 && par < (int)nodeWorld.size()) parentWorld = nodeWorld[par];
            }
            const uint8_t* posP = c.at(me.posOff);
            const uint8_t* stdP = c.at(me.stdOff);
            const uint16_t* idxP = (const uint16_t*)c.at(me.idxOff);
            uint32_t nv = me.nv, nstd = me.nstd, nidx = me.nidx;
            for (auto& sub : me.subs) {
                if (sub.indexCount == 0) continue;
                if (sub.firstIndex + sub.indexCount > nidx) { log("inst mesh%u submesh range OOB", meshIdx); continue; }
                MeshData md;
                md.name = (sub.matIndex < materials.size()) ? materials[sub.matIndex].path : (type + ".sub");
                std::unordered_map<uint32_t, uint32_t> remap;
                remap.reserve(sub.indexCount);
                md.indices.reserve(sub.indexCount);
                std::vector<int> vatCols;   // VAT: per-emitted-vertex column (UV1.x) into the vatdata
                double aR=0, aG=0, aB=0; uint32_t aN=0;   // StdData a_color average (baked albedo)
                double aASum=0; uint8_t aAMin=255, aAMax=0;   // a_color ALPHA range (DBG: fog opacity?)
                for (uint32_t k = 0; k < sub.indexCount; ++k) {
                    uint32_t oi = sub.baseVertex + idxP[sub.firstIndex + k];  // baseVertex = vertexOffset
                    if (oi >= nv) { oi = 0; }
                    auto it = remap.find(oi);
                    uint32_t ni;
                    if (it == remap.end()) {
                        ni = (uint32_t)(md.positions.size() / 3);
                        remap.emplace(oi, ni);
                        // position (RigidPos f32x3). For VAT meshes keep LOCAL (animate() adds the
                        // per-frame offset then applies the instance world matrix); else world-bake.
                        float px, py, pz; memcpy(&px, posP+oi*12+0, 4); memcpy(&py, posP+oi*12+4, 4); memcpy(&pz, posP+oi*12+8, 4);
                        if (curVat) {
                            md.positions.push_back(px); md.positions.push_back(py); md.positions.push_back(pz);
                            uint16_t hc; memcpy(&hc, stdP+(oi<nstd?oi:0)*20+16, 2); int col=(int)(h2f(hc)+0.5f);  // UV1.x
                            vatCols.push_back((col>=0 && col<curVat->cols) ? col : 0);
                        } else {
                            float wp[3]; xform(world, px, py, pz, wp);
                            md.positions.push_back(wp[0]); md.positions.push_back(wp[1]); md.positions.push_back(wp[2]);
                        }
                        // uv: StdData = a_normal i16x2(@0) + a_tangent i16x2(@4) + a_color
                        // u8x4(@8) + a_texcoords f16x4(@12). UV is at offset 12 (NOT 8 — that's
                        // the color; reading it as f16 gave NaN UVs -> textures sampled garbage).
                        float u = 0, v = 0;
                        if (oi < nstd) {
                            uint16_t hu, hv; memcpy(&hu, stdP+oi*20+12, 2); memcpy(&hv, stdP+oi*20+14, 2);
                            u = h2f(hu); v = h2f(hv);
                            // V-FLIP: libshell's model shader does oTexCoord.y = 1.0 - TexCoord.y
                            // (the cooked texcoords use the bottom-up / OpenGL convention). Without
                            // this the textures map mirrored vertically = "not properly mapped".
                            // (HSR_NOVFLIP disables it for testing.)
                            static int novflip = -1;
                            if (novflip < 0) novflip = std::getenv("HSR_NOVFLIP") ? 1 : 0;
                            if (!novflip) v = 1.0f - v;
                        }
                        // Per-instance atlas cell remap: UV0 in [0,1] -> this instance's cell sub-rect.
                        if (atlasGrid > 1) {
                            float g = (float)atlasGrid;
                            u = (u + (float)aCol) / g;
                            v = (v + (float)aRow) / g;
                        }
                        md.uvs.push_back(u); md.uvs.push_back(v);
                        // uv1 (lightmap unwrap) = a_texcoords.zw @16. Sampled to bake the lightmap for
                        // textured ShellEnv shells. (For VAT meshes @16 is the column index, not a UV —
                        // harmless to store; only used when bakeLightmapVtx is set, which VAT never is.)
                        { float u1=0.f, v1=0.f;
                          if (oi < nstd) { uint16_t hu1,hv1; memcpy(&hu1,stdP+oi*20+16,2); memcpy(&hv1,stdP+oi*20+18,2);
                                           u1=h2f(hu1); v1=h2f(hv1);
                                           static int nv1=-1; if(nv1<0) nv1=std::getenv("HSR_NOVFLIP")?1:0; if(!nv1) v1=1.0f-v1; }
                          md.uvs2.push_back(u1); md.uvs2.push_back(v1); }
                        if (oi < nstd) { const uint8_t* cc = stdP+oi*20+8;   // a_color u8x4 @ offset 8
                            aR += cc[0]; aG += cc[1]; aB += cc[2]; ++aN;
                            aASum += cc[3]; if (cc[3]<aAMin) aAMin=cc[3]; if (cc[3]>aAMax) aAMax=cc[3]; }
                    } else ni = it->second;
                    md.indices.push_back((u32)ni);
                }
                md.nVerts = (u32)(md.positions.size() / 3);
                md.nIdx   = (u32)md.indices.size();
                // placeholder shading until tx_*.png.opa decode lands: stable tint per material
                uint64_t hsh = materials.empty() ? sub.matIndex : (sub.matIndex < materials.size() ? materials[sub.matIndex].id : sub.matIndex);
                md.tint[0] = 0.45f + 0.5f * (((hsh)      & 0xFF) / 255.0f);
                md.tint[1] = 0.45f + 0.5f * (((hsh >> 8) & 0xFF) / 255.0f);
                md.tint[2] = 0.45f + 0.5f * (((hsh >> 16)& 0xFF) / 255.0f);
                // Resolve base texture + blend mode the FAITHFUL way: the material's .mat.txt
                // (libshell's own description) names the diffuse texture (by AssetId or Path) and
                // declares Transparent/Additive. This generalises to ANY OPA home; we fall back to
                // the old name-prefix heuristic only when a material has no .mat.txt.
                const Tex* tex = nullptr;
                const Tex* lmTex = nullptr;
                const MatProps* mp = nullptr;
                if (sub.matIndex < materials.size()) {
                    std::string stem = matStem(materials[sub.matIndex].path);
                    auto it = matProps.find(stem);
                    if (it != matProps.end()) mp = &it->second;
                    if (mp) {
                        if (!mp->diffuseBase.empty())            tex = texByBase(mp->diffuseBase);
                        if (!tex && mp->diffuseId) {
                            auto ai = assetIdToTexBase.find(mp->diffuseId);
                            if (ai != assetIdToTexBase.end()) tex = texByBase(ai->second);
                        }
                        if (mp->lightmapId) {   // resolve the baked lightmap texture
                            auto li = assetIdToTexBase.find(mp->lightmapId);
                            if (li != assetIdToTexBase.end()) lmTex = texByBase(li->second);
                        }
                    }
                    if (!tex) {  // fallback: old prefix heuristic (spacestation-style names)
                        std::string b = matBaseName(materials[sub.matIndex].path);
                        if (!b.empty()) tex = bestTexFor(b);
                    }
                    // DBG (HSR_FOGDBG): decoded texture ALPHA range — the renderer's OWN decode of
                    // the sprite, to prove a transparent material's alpha is read faithfully. The
                    // per-texture scan is gated so it doesn't slow normal loads.
                    static int fogdbg = -1; if (fogdbg<0) fogdbg = std::getenv("HSR_FOGDBG") ? 1 : 0;
                    if (fogdbg) {
                        int txAMin=255, txAMax=0; double txASum=0; size_t txAN=0;
                        if (tex && !tex->rgba.empty())
                            for (size_t q=3; q<tex->rgba.size(); q+=4) { int a=tex->rgba[q];
                                if(a<txAMin)txAMin=a; if(a>txAMax)txAMax=a; txASum+=a; ++txAN; }
                        log("  submesh mat[%u] stem='%s' -> tex '%s' (%ux%u) blend=%d vcolRGB=(%.0f,%.0f,%.0f) vcolA[min=%u max=%u mean=%.0f] texAlpha[min=%d max=%d mean=%.1f]",
                            sub.matIndex, stem.c_str(), tex?tex->key.c_str():"<none>",
                            tex?tex->w:0, tex?tex->h:0, mp?(int)(mp->transparent||mp->additive):-1,
                            aN?aR/aN:0, aN?aG/aN:0, aN?aB/aN:0, aAMin, aAMax, aN?aASum/aN:0,
                            txAN?txAMin:0, txAN?txAMax:0, txAN?txASum/txAN:0.0);
                    } else
                    log("  submesh mat[%u] stem='%s' -> tex '%s' (%ux%u) blend=%d | diffId=%llu lmId=%llu lmTex='%s'",
                        sub.matIndex, stem.c_str(), tex?tex->key.c_str():"<none>",
                        tex?tex->w:0, tex?tex->h:0, mp?(int)(mp->transparent||mp->additive):-1,
                        (unsigned long long)(mp?mp->diffuseId:0), (unsigned long long)(mp?mp->lightmapId:0),
                        lmTex?lmTex->key.c_str():"<none>");
                    // HSR_VATDBG: flag near-BLACK textures + the mesh world centroid, to hunt down
                    // "black rectangle" meshes (a mesh whose UVs land on a black texture region).
                    static int vatdbg2 = -1; if (vatdbg2<0) vatdbg2 = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg2 && tex && !tex->rgba.empty()) {
                        double br=0; size_t np=tex->rgba.size()/4, step=np>4096?np/4096:1, cnt=0;
                        for (size_t q=0;q<np;q+=step){ br+=tex->rgba[q*4]+tex->rgba[q*4+1]+tex->rgba[q*4+2]; ++cnt; }
                        double mean = cnt? br/(cnt*3):0;
                        if (mean < 35.0) {
                            const Mat4& nw=(nodeIdx<nodeWorld.size())?nodeWorld[nodeIdx]:world;
                            double vr=aN?aR/aN:0, vg=aN?aG/aN:0, vb=aN?aB/aN:0;   // avg a_color (vertex colour)
                            log("  DARKTEX texmean=%.1f vcol=(%.0f,%.0f,%.0f) mesh#%zu stem='%s' tex='%s'",
                                mean, vr,vg,vb, meshes.size(), stem.c_str(), tex->key.c_str());
                        }
                    }
                }
                // Face culling — FAITHFUL to the material's `DoubleSided` flag (libshell back-face
                // culls single-sided materials). This matters most for TRANSPARENT cards: a
                // DoubleSided:false fog/smoke card drawn with CULL_NONE blends BOTH faces (front +
                // back), ~doubling its apparent density ("fog too visible"). Respecting the flag culls
                // the back face so each card blends once. Default stays doubleSided=true (CULL_NONE)
                // for materials with no .mat.txt, preserving prior OPA behaviour. HSR_NOOPACULL keeps
                // everything double-sided (fallback if a CW-wound mesh culls its visible face).
                // Scope to TRANSPARENT materials: that's where back-face culling has a real effect
                // (a 2-sided blended card blends both faces). For OPAQUE meshes 2-sided vs culled is
                // visually identical (front face covers the back), so we leave them CULL_NONE to avoid
                // any winding-regression risk on other OPA envs.
                static int noOpaCull = -1; if (noOpaCull<0) noOpaCull = std::getenv("HSR_NOOPACULL") ? 1 : 0;
                if (mp && !noOpaCull && (mp->transparent || mp->additive)) md.doubleSided = mp->doubleSided;
                // SpecIbl materials are env-lit by the IBL cubemap (whether or not they have an albedo).
                bool isSpecibl = false;
                if (sub.matIndex < materials.size()) { std::string mpath = materials[sub.matIndex].path;
                    for (auto& c : mpath) c=(char)tolower((unsigned char)c); isSpecibl = mpath.find("specibl")!=std::string::npos; }
                if (tex) {
                    md.texW = tex->w; md.texH = tex->h; md.hasTexture = true;
                    md.texRGBA = tex->rgba;
                    // CAPTURE-CONFIRMED: no runtime IBL shader exists; textured specibl = diffuse·lightmap
                    // (set below) or diffuse-only. Old IBL path is opt-in via HSR_IBL for A/B comparison.
                    md.iblLit = isSpecibl && std::getenv("HSR_IBL");
                    md.tint[0] = md.tint[1] = md.tint[2] = 1.0f;   // texture carries the color
                    // Blend from the material spec; if no .mat.txt, fall back to the alpha scan.
                    md.useBlend = mp ? (mp->transparent || mp->additive) : tex->hasAlpha;
                    md.additive = mp ? mp->additive : false;   // god-rays/glow -> ADD blend (not alpha)
                    md.alphaTest = mp ? mp->alphaTest : false;  // cutout -> opaque pass + discard
                    // Scale the per-mesh texture alpha by the material 'alpha' UNIFORM (libshell does
                    // this in the shader). A transparent effect with a fully-opaque texture but a low
                    // alpha uniform (forge flicker=0.27) must be a FAINT overlay, not an opaque dark
                    // box that occludes everything behind it. Only for alpha-blended (not additive).
                    if (md.useBlend && !md.additive && mp && mp->alpha < 0.999f) {
                        uint8_t a8 = (uint8_t)(mp->alpha * 255.0f + 0.5f);
                        for (size_t q = 3; q < md.texRGBA.size(); q += 4)
                            md.texRGBA[q] = (uint8_t)((md.texRGBA[q] * a8) / 255);
                    }
                    // TEXTURED interior shell that ALSO has a baked lightmap (concrete/floor/walls):
                    // libshell shades `diffuse · lightmap · lightmappower`. We sample the lightmap at uv1
                    // per vertex, scale by lightmappower, and bake it into the per-vertex colour the frag
                    // multiplies by (diffuse · vColor). lightmappower is the coloured neon/glow boost.
                    // (Only opaque shells; skip blended/additive effects.) Disables IBL (lightmap is the light).
                    if (lmTex && !lmTex->rgba.empty() && !md.uvs2.empty() && !md.useBlend) {
                        // PER-PIXEL lightmap (libshell ShellEnv = diffuse·lightmap·lightmappower). The V79
                        // shader samples the 'lightmap' sampler (set2 bind3) at uv1 and multiplies; we
                        // pre-bake the per-channel lightmappower into the lightmap texels here (LDR clamp).
                        md.lmRGBA = lmTex->rgba; md.lmW = lmTex->w; md.lmH = lmTex->h;
                        md.hasLightmap = true; md.iblLit = false; md.bakeLightmapVtx = false;
                        // FAITHFUL (captured MeshShellEnv frag): lit = diffuse·lightmap·lightmappower applied
                        // IN-SHADER (HDR order, clamped once at output) via the per-mesh tint push-constant
                        // (renderer pushes UniformColor = tint*lmPow). Previously lightmappower was pre-baked
                        // into the 8-bit lightmap with an LDR clamp, which CLIPPED the HDR neon boost
                        // (concrete=[3.76,3.21,4.36]) -> washed-out/flat lighting. Keep the lightmap RAW.
                        md.lightmapPower[0]=mp?mp->lightmapPower[0]:1.f;
                        md.lightmapPower[1]=mp?mp->lightmapPower[1]:1.f;
                        md.lightmapPower[2]=mp?mp->lightmapPower[2]:1.f;
                    }
                } else if (lmTex && !lmTex->rgba.empty()) {
                    // No diffuse texture, but a BAKED LIGHTMAP exists. The interior SHELL meshes
                    // (divingHelmet, rubyGem, octo, loft_table/lamp, ...) bake their full lit
                    // appearance + surface detail into the lightmap. libshell shades these as
                    // `colour = basecolorFactor · lightmap`. We use the lightmap AS the base texture
                    // (sampled with uv0 = the lightmap unwrap on these no-albedo merged shells) and
                    // multiply the basecolor factor into it. This replaces the flat IBL blob with the
                    // real baked detail. Do NOT also IBL-light it — the lightmap already IS the light.
                    float lp0 = mp?mp->lightmapPower[0]:1.f, lp1 = mp?mp->lightmapPower[1]:1.f, lp2 = mp?mp->lightmapPower[2]:1.f;
                    static float lmGain = -1.f; if (lmGain<0.f){ const char* g=std::getenv("HSR_LMGAIN"); lmGain = g?(float)atof(g):1.0f; }
                    if (!md.uvs2.empty() && !std::getenv("HSR_LMBASE")) {
                        // FAITHFUL MeshShellEnv: no basecolor texture -> base = BaseColorFactor (white), lit by
                        // the lightmap sampled at uv1 (its proper unwrap) × lightmappower applied IN-SHADER via
                        // the tint push-constant (HDR, clamped once). Replaces lightmap-as-base in the uv0
                        // diffuse slot × guessed 2.5 gain (WRONG uv channel + clipped tone -> "messed up"
                        // loft_lamp). loft_lamp/helmet/gem/octo/pencil are single PBR shells; lightmap = uv1.
                        md.texW = 1; md.texH = 1; md.hasTexture = true;
                        md.texRGBA = {255,255,255,255};
                        md.lmRGBA = lmTex->rgba; md.lmW = lmTex->w; md.lmH = lmTex->h;
                        md.hasLightmap = true; md.bakeLightmapVtx = false;
                        md.lightmapPower[0]=lp0; md.lightmapPower[1]=lp1; md.lightmapPower[2]=lp2;
                    } else {
                        // Fallback: the lightmap is unwrapped in uv0 here (single UV set) -> use it as the uv0
                        // base, RAW. lightmappower applied IN-SHADER via tint (gm.lmPow) — NOT pre-baked + LDR
                        // clamped, which blew the bright baked shiny detail to WHITE (the "messed up"/textureless
                        // lamp). HSR_LMGAIN (default 1.0 = faithful) is an optional global lift folded into power.
                        md.texW = lmTex->w; md.texH = lmTex->h; md.hasTexture = true;
                        md.texRGBA = lmTex->rgba;
                        md.lightmapPower[0]=lp0*lmGain; md.lightmapPower[1]=lp1*lmGain; md.lightmapPower[2]=lp2*lmGain;
                    }
                    md.tint[0] = md.tint[1] = md.tint[2] = 1.0f;
                    md.iblLit = false; md.iblFullSpec = false;
                    static int lmdbg = -1; if (lmdbg<0) lmdbg = std::getenv("HSR_LMDBG") ? 1 : 0;
                    if (lmdbg) log("  LIGHTMAP-BASE mesh#%zu tex='%s' (%ux%u) lmPow=(%.1f,%.1f,%.1f) gain=%.1f",
                                   meshes.size(), lmTex->key.c_str(), lmTex->w, lmTex->h, lp0,lp1,lp2,lmGain);
                } else {
                    // No diffuse texture. FAITHFUL to libshell's model shader (IDA 0x1e76b0): when a
                    // material has no base-colour texture the fragment colour is the colour UNIFORM
                    // ITSELF — `lowp vec4 color = BaseColorFactor;` with NO vertex-colour modulation.
                    // So a flat ShellEnv material renders as its `diffuse` uniform: oldWest_ember =
                    // (0.78,0.53,0.24) ORANGE (was rendering BLACK because we multiplied by the ember's
                    // black a_color), black_mtl = (0,0,0). The ONLY surfaces that need the StdData
                    // a_color are SpecIbl ground/terrain whose `diffuse` is the neutral [1,1,1] TINT and
                    // whose baked albedo lives in the vertex colour — detect that (tint≈white) and use
                    // the vertex colour there; otherwise the uniform is the colour.
                    md.texW = 1; md.texH = 1; md.hasTexture = true;
                    if (isSpecibl && std::getenv("HSR_WHITEDBG")) {
                        std::string mstem = (sub.matIndex<materials.size()) ? materials[sub.matIndex].path : std::string("?");
                        uint64_t did = mp?mp->diffuseId:0, lid = mp?mp->lightmapId:0;
                        auto di = assetIdToTexBase.find(did); auto li2 = assetIdToTexBase.find(lid);
                        log("  WHITEDBG mesh#%zu mat='%s' mp=%d diffId=%llu(map=%d tex=%d) lmId=%llu(map=%d tex=%d) diffBase='%s'",
                            meshes.size(), mstem.c_str(), (int)(mp!=nullptr),
                            (unsigned long long)did, (int)(di!=assetIdToTexBase.end()), (int)(di!=assetIdToTexBase.end() && texByBase(di->second)!=nullptr),
                            (unsigned long long)lid, (int)(li2!=assetIdToTexBase.end()), (int)(li2!=assetIdToTexBase.end() && texByBase(li2->second)!=nullptr),
                            (mp?mp->diffuseBase.c_str():""));
                    }
                    bool transp = mp && (mp->transparent || mp->additive);
                    float dr = mp ? mp->diffuseColor[0] : 1.f, dg = mp ? mp->diffuseColor[1] : 1.f, db = mp ? mp->diffuseColor[2] : 1.f;
                    bool tint = (dr>0.97f && dg>0.97f && db>0.97f);   // diffuse==white => pure tint, real colour is a_color
                    float vr=1.f, vg=1.f, vb=1.f;
                    if (tint && aN) { vr=(float)aR/aN/255.f; vg=(float)aG/aN/255.f; vb=(float)aB/aN/255.f; }
                    auto cl = [](float x){ return (u8)(x<0?0:x>1?255:x*255.f+0.5f); };
                    if (transp) { md.texRGBA = {255,255,255,0}; md.useBlend = true; }
                    else        { md.texRGBA = {cl(vr*dr), cl(vg*dg), cl(vb*db), 255};
                                  // CAPTURE-CONFIRMED (Frida rip of the live shell across all 26 envs): home
                                  // meshes render ONLY through MeshShellEnv, which has NO IBL/specibl/fresnel
                                  // branch. The env `_ibl/*_hdri_*.dds` cubemaps are baked into the `lightmap`
                                  // (Modmap) at COOK time, not sampled at runtime. So a no-texture specibl mesh
                                  // = BaseColorFactor (the `diffuse` uniform) -> the vr/vg/vb·dr/dg/db colour we
                                  // just wrote, identical to a flat ShellEnv material. The old phantom split-sum
                                  // IBL path rendered WHITE when no cubemap was loaded; keep it opt-in (HSR_IBL).
                                  if (isSpecibl && mp && std::getenv("HSR_IBL")) {
                                      md.iblLit = true; md.iblFullSpec = true;
                                      md.texRGBA = {255,255,255,255};
                                      md.metallic = mp->metallic; md.roughness = mp->roughness;
                                      md.speciblDiffScale = mp->speciblDiffScale; md.speciblSpecScale = mp->speciblSpecScale;
                                      md.albedoFactor[0]=dr; md.albedoFactor[1]=dg; md.albedoFactor[2]=db;
                                  } }
                }
                // Animated node -> stream world positions every frame (renderer keeps the VBO
                // mapped). Record the LOCAL base positions; animate(t) rewrites md.positions.
                if (animated) {
                    md.dynamicVerts = true;
                    animRecs.push_back({ meshes.size(), (uint32_t)(animNodeBase + nodeIdx), parentWorld, md.positions });
                }
                // UV/flipbook material animation (mat.sanim) keyed by this mesh's geo/node name.
                if (nodeIdx < nodes.size() && matUVAnim.count(nodes[nodeIdx].name)) {
                    md.dynamicVerts = true;   // positions stay baked; UVs stream each frame
                    uvAnimRecs.push_back({ meshes.size(), nodes[nodeIdx].name, md.uvs });
                    static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg) {
                        const Mat4& nw = (nodeIdx < nodeWorld.size()) ? nodeWorld[nodeIdx] : parentWorld;
                        float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                        for(size_t q=0;q+1<md.uvs.size();q+=2){float u=md.uvs[q],v=md.uvs[q+1];if(u<u0)u0=u;if(u>u1)u1=u;if(v<v0)v0=v;if(v>v1)v1=v;}
                        log("  UVANIM mesh#%zu node='%s' fr=%d W=(%.1f,%.1f,%.1f) UV0 u[%.2f,%.2f] v[%.2f,%.2f]",
                            meshes.size(), nodes[nodeIdx].name.c_str(), matUVAnim[nodes[nodeIdx].name].nFrames, nw.m[12],nw.m[13],nw.m[14], u0,u1,v0,v1);
                    }
                }
                // MaterialTint (per-frame RGBA opacity) — keyed by the SAME geo/node name. The fog
                // (fog_0X_geo_Y) and dust carry an alpha 0..~0.22 fade; apply it as md.curTint each
                // frame so the fragment shader multiplies it in (faithful: frag = texture * tint).
                if (nodeIdx < nodes.size() && matTintAnim.count(nodes[nodeIdx].name)) {
                    tintRecs.push_back({ meshes.size(), nodes[nodeIdx].name });
                    md.dynamicVerts = true;   // so the renderer streams curTint each frame
                    const TintTrack& tt = matTintAnim[nodes[nodeIdx].name];
                    if (tt.nFrames > 0) { md.curTint[0]=tt.rgba[0]; md.curTint[1]=tt.rgba[1];
                                          md.curTint[2]=tt.rgba[2]; md.curTint[3]=tt.rgba[3]; }
                }
                // VAT: keep LOCAL basePos + per-vertex column; animate() adds the per-frame offset
                // and applies the instance world matrix (animate(0) places them at the rest pose).
                if (curVat && vatCols.size() == md.positions.size()/3) {
                    md.dynamicVerts = true;
                    VatRec vr; vr.meshIdx = meshes.size(); vr.basePos = md.positions;
                    vr.col = std::move(vatCols); vr.world = world; vr.vd = curVat;
                    vatRecs.push_back(std::move(vr));
                    if ((int)curVat->frames > animMaxFrames) animMaxFrames = curVat->frames;
                }
                {   // HSR_VATDBG: dump UV0 range for VAT meshes (one atlas cell vs whole atlas?)
                    static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg && curVat && !md.uvs.empty()) {
                        float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                        double cx=0,cy=0,cz=0; size_t nP=md.positions.size()/3;
                        for (size_t q=0;q+1<md.uvs.size();q+=2){ float u=md.uvs[q],v=md.uvs[q+1]; if(u<u0)u0=u;if(u>u1)u1=u;if(v<v0)v0=v;if(v>v1)v1=v; }
                        for (size_t q=0;q<nP;++q){ cx+=md.positions[q*3];cy+=md.positions[q*3+1];cz+=md.positions[q*3+2]; }
                        if(nP){cx/=nP;cy/=nP;cz/=nP;}
                        float wc[3]; xform(world,(float)cx,(float)cy,(float)cz,wc);
                        log("  VATDBG mesh#%zu '%s' UV0 u[%.3f,%.3f] v[%.3f,%.3f] verts=%zu worldC=(%.2f,%.2f,%.2f)", meshes.size(), curOpaBase.c_str(), u0,u1,v0,v1, nP, wc[0],wc[1],wc[2]);
                    }
                }
                meshes.push_back(std::move(md));
            }
        }
        log("emitted %zu renderable submeshes from %zu instances (%zu anim)",
            meshes.size(), instances.size(), animRecs.size());
        return !meshes.empty();
    }
};
