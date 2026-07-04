#pragma once
// HSL SCENE COOKER / GENERATOR — the ENCODE side, built into the renderer so it is a scene MAKER (not just a
// viewer): take a loaded/edited scene (geometry + textures) and GENERATE a cooked V203/HSL scene.zip that boots on
// a Quest. 1:1 inverse of the renderer's decoders (rendmesh/rendtxtr/matl/rendshad/asmh parsers). Every format here
// was verified end-to-end against libshell by the python prototype (a fully self-cooked checkerboard env renders +
// installs). See the project_hsl_cooker_apk_baker memory for the full format notes.
#include "core/types.h"
#include "core/config.h"                 // AppConfig::detectBuildTools() — portable Android SDK auto-detect for signing
#include "core/scene_items.h"           // editor-authored haven2025 components (spawn/chair/collider/wall/hotspot)
#include "cook/physx_navmesh.h"          // INTEGRATED PhysX navmesh cook (cookNavmeshSEBD — links the vendored PhysX libs)
#include "cook/haven_manifest_axml.h"   // haven2025's AndroidManifest.xml (binary AXML), hardcoded; the cook rewrites its package
#include "shadergen/shadergen.h"        // getTime() animation shader GENERATOR (C++; was cooker/make_*_shader.py)
#include <vector>
#include <array>
#include <cmath>
#include <string>
#include <map>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <functional>   // exportSceneAPK progress callback
#include <cstdlib>
#include <ctime>
#include <thread>        // multi-threaded ASTC encode
#include <fstream>       // texture-encode disk cache
#include <filesystem>    // texture-encode disk cache dir
#include "astcenc.h"   // ASTC ENCODE (full astcenc build) — RGBA8 -> ASTC blocks for RENDTXTR
#include "miniz.h"     // ZIP read/write — scene.zip assembly + APK splice (already linked by the project)
#include "cook/embedded_assets.h"   // STANDALONE: cooker templates + Nuxd donor files baked into the binary
#include "cook/hzanim_acl.h"  // hzAclEncode (HZANIM ACL clip) — only the declaration; ACL lives in hzanim_acl.cpp
#include "cook/cook_verify.h"  // cook-time FlatBuffer verifier (device's stock-flatbuffers structural check, schema from Meta assets)

namespace hslcook {

// ── minimal FlatBuffer builder (back-to-front), enough for RENDMESH / MATL / RENDSHAD ─────────────────────────
struct FB {
    std::vector<uint8_t> buf; size_t head; int minalign = 1;
    std::vector<int> vt; int objEnd = 0;
    explicit FB(size_t cap = 1024) { buf.assign(cap, 0); head = buf.size(); }
    std::vector<uint8_t> output() { return std::vector<uint8_t>(buf.begin() + head, buf.end()); }
    int offset() { return (int)(buf.size() - head); }
    void grow() { size_t old = buf.size(); std::vector<uint8_t> nb(old * 2, 0); memcpy(nb.data() + old, buf.data(), old); buf.swap(nb); head += old; }
    void pad(int n) { for (int i = 0; i < n; i++) { head--; buf[head] = 0; } }
    void prep(int sz, int extra) { if (sz > minalign) minalign = sz; size_t used = buf.size() - head; int a = ((~((int)used + extra)) + 1) & (sz - 1); while ((int)head < a + sz + extra) grow(); pad(a); }
    template <class T> void place(T x) { head -= sizeof(T); memcpy(buf.data() + head, &x, sizeof(T)); }
    template <class T> void prependScalar(T x) { prep(sizeof(T), 0); place(x); }
    void prependUOff(int off) { prep(4, 0); place((uint32_t)(offset() - off + 4)); }
    int createByteVector(const uint8_t* d, size_t n) { prep(4, (int)n); head -= n; memcpy(buf.data() + head, d, n); prependScalar((uint32_t)n); return offset(); }
    int createString(const std::string& s) { size_t n = s.size(); prep(4, (int)n + 1); head--; buf[head] = 0; head -= n; if (n) memcpy(buf.data() + head, s.data(), n); prependScalar((uint32_t)n); return offset(); }
    int createOffsetVector(const std::vector<int>& offs) { prep(4, 4 * (int)offs.size()); for (auto it = offs.rbegin(); it != offs.rend(); ++it) prependUOff(*it); prependScalar((uint32_t)offs.size()); return offset(); }
    int createStructVector(const uint8_t* raw, int stride, int count, int align) { int n = stride * count; prep(4, n); prep(align, n); head -= n; memcpy(buf.data() + head, raw, n); prependScalar((uint32_t)count); return offset(); }
    void startObject(int n) { vt.assign(n, 0); objEnd = offset(); }
    void slot(int s) { vt[s] = offset(); }
    void addOffset(int s, int off) { if (off) { prependUOff(off); slot(s); } }
    template <class T> void addScalar(int s, T x) { if (x) { prependScalar(x); slot(s); } }
    void addStructSlot(int s, const uint8_t* raw, int sz, int align) { prep(align, sz); head -= sz; memcpy(buf.data() + head, raw, sz); slot(s); }
    int endObject() { prependScalar((int32_t)0); int objOff = offset(); int i = (int)vt.size() - 1; while (i >= 0 && vt[i] == 0) i--; int trimmed = i + 1;
        for (int fi = trimmed - 1; fi >= 0; fi--) place((uint16_t)(vt[fi] ? objOff - vt[fi] : 0));
        place((uint16_t)(objOff - objEnd)); place((uint16_t)((trimmed + 2) * 2));
        int vtOff = offset(); int32_t soff = vtOff - objOff; memcpy(buf.data() + buf.size() - objOff, &soff, 4); return objOff; }
    void finish(int root, const char* fid) { int extra = 4 + (fid ? 4 : 0); prep(minalign, extra); if (fid) for (int i = 3; i >= 0; i--) { head--; buf[head] = (uint8_t)fid[i]; } prependUOff(root); }
};

// MurmurHash64A — the device's content-hash for mesh buffers (part.f5 = murmur64A(indexBuffer) low 32 bits;
// VS.f4 = the same over the vertex buffer). Verified: part.f5 of nuxd's dome mesh == murmur64A(its IB) & 0xFFFFFFFF.
inline uint64_t murmur64A(const uint8_t* data, size_t n, uint64_t seed = 0) {
    const uint64_t M = 0xC6A4A7935BD1E995ULL; const int R = 47;
    uint64_t h = seed ^ ((uint64_t)n * M);
    size_t nb = n & ~(size_t)7;
    for (size_t i = 0; i < nb; i += 8) { uint64_t k; memcpy(&k, data + i, 8); k *= M; k ^= k >> R; k *= M; h ^= k; h *= M; }
    uint64_t k = 0; const uint8_t* tail = data + nb;
    switch (n & 7) {
        case 7: k ^= (uint64_t)tail[6] << 48; [[fallthrough]];
        case 6: k ^= (uint64_t)tail[5] << 40; [[fallthrough]];
        case 5: k ^= (uint64_t)tail[4] << 32; [[fallthrough]];
        case 4: k ^= (uint64_t)tail[3] << 24; [[fallthrough]];
        case 3: k ^= (uint64_t)tail[2] << 16; [[fallthrough]];
        case 2: k ^= (uint64_t)tail[1] << 8;  [[fallthrough]];
        case 1: k ^= (uint64_t)tail[0]; h ^= k; h *= M;
    }
    h ^= h >> R; h *= M; h ^= h >> R; return h;
}

inline uint16_t f32_to_f16(float f) { uint32_t x; memcpy(&x, &f, 4); uint32_t s = (x >> 16) & 0x8000; int e = ((x >> 23) & 0xff) - 112; uint32_t m = x & 0x7fffff; if (e <= 0) return (uint16_t)s; if (e >= 31) return (uint16_t)(s | 0x7c00); return (uint16_t)(s | (e << 10) | (m >> 13)); }

// ── RENDMESH (file_id "MESH"): fully replicate nuxd's mesh so the device MeshAssetBuilder accepts it. Vertex
//    format = nuxd's common stride-20 layout (POS f32x3@0, TEXCOORD0 f16x2@12, NORMAL fmt0x13@16=0xFFFFFFFF).
//    Structure: VS{ field0=format-hash const, 1=count, 2=VB, 3=attrs(3), 4=content-hash } ; part{ 0=VS,1=IB,
//    4=boundMin,5=content-hash,6=52 } ; LOD{ 0=parts,1=screenSize,3=boundMin } ; root{ 1=LOD,2=materials,
//    LOD.field1 = the per-LOD "screen-size" cutoff. IDA: MeshDefinition::parseLod defaults it to 0.0 when absent, and
//    MeshAssetBuilder aggregates the MIN across LODs as the asset's visibility floor — so an object is dropped once its
//    on-screen coverage falls below the SMALLEST LOD's cutoff. We emit 0.0 (= libshell's own default = "never size-cull",
//    matching V79's no-LOD-cull behavior). The old 0.2 was blindly copied from nuxd's full-screen skybox DOME (which never
//    shrinks past 0.2), so it culled small/distant single-LOD props at range ("objects disappear at distance").
//    3=min,4=max,8=1.0 }. (format-hash 0xDBE0A523 is CONSTANT for this layout; content-hashes appear to be cache
//    keys — using deterministic placeholders. part.field3 material-binding is omitted; the entity's
//    MaterialPlatformComponent supplies the material.) ────────────────────────────────────────────────────────
// HSR_NOCULL — the "why do vistas never clip but cooked homes do" fix, replicating V79. V79 shipped NO environment-mesh
// culler (it drew everything); V205 added GPU frustum + Hi-Z occlusion + CLOD-budget + distance/size culling. There is no
// per-env "disableCulling" flag we can ship (it's a global engine setting). But every cull is driven off each renderable's
// BOUNDS — and frustum culling honoring the cooked AABB is DEVICE-PROVEN (the existing spinBoundsY swept-bounds fix). So
// scene-spanning bounds make V205 treat every mesh as always-visible: never frustum/occlusion/size-culled. The geometry
// still renders at its real baked vertex positions (bounds don't move verts) — only culling is defeated. Opt-in: it trades
// the Quest's culling perf for V79-style "draw everything," which is exactly what porting old homes wants.
inline void nocullBounds(float aabb[6], float& radius) {
    // DEFAULT ON (V79-faithful "draw everything"): V205's GPU frustum/Hi-Z-occlusion/CLOD/size culler drops cooked-home
    // meshes that V79 always drew — e.g. cyberhome's flying CARS vanish the moment you look at them (occlusion/size cull).
    // Scene-spanning bounds make V205 treat every mesh as always-visible so it's never wrongly culled. HSR_CULL=1 opts back
    // into the (buggy-for-us) device culler. This is generic: ANY env's small/distant/animated meshes stop disappearing.
    const char* e = std::getenv("HSR_CULL");
    if (e && *e && *e!='0') return;     // explicit opt-in to V205 culling -> keep the mesh's own tight bounds
    aabb[0]=aabb[1]=aabb[2]=-1.0e5f; aabb[3]=aabb[4]=aabb[5]=1.0e5f; radius=1.74e5f;
}
inline std::vector<uint8_t> encodeRendMesh(const std::vector<float>& posXYZ, const std::vector<float>& uvUV, const std::vector<uint16_t>& idx, const std::vector<uint8_t>& embeddedMatl = {}) {
    int nv = (int)(posXYZ.size() / 3); std::vector<uint8_t> vb;
    float mn[3] = { 1e30f,1e30f,1e30f }, mx[3] = { -1e30f,-1e30f,-1e30f };
    for (int i = 0; i < nv; i++) {
        for (int k = 0; k < 3; k++) { float p = posXYZ[i * 3 + k]; uint8_t b[4]; memcpy(b, &p, 4); vb.insert(vb.end(), b, b + 4); if (p < mn[k]) mn[k] = p; if (p > mx[k]) mx[k] = p; }
        uint16_t u = f32_to_f16(uvUV[i * 2]), v = f32_to_f16(uvUV[i * 2 + 1]);
        uint8_t bb[4] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)v, (uint8_t)(v >> 8) }; vb.insert(vb.end(), bb, bb + 4);
        uint8_t nb[4] = { 0xFF,0xFF,0xFF,0xFF }; vb.insert(vb.end(), nb, nb + 4);   // packed normal (fmt 0x13) placeholder
    }
    std::vector<uint8_t> ib; for (uint16_t i : idx) { ib.push_back((uint8_t)i); ib.push_back((uint8_t)(i >> 8)); }
    // The bounds fields are 24-byte AABB STRUCTS (min.xyz,max.xyz), NOT single floats — the verifier reads the
    // full struct, so 4-byte floats overrun the table -> reject. root.f4 = a 4-byte bounding radius. VS.f0 is a
    // 12-byte format struct (CONSTANT for this stride-20 layout). VS.f4 / part.f5 = {murmur64A(VB/IB) u64, 0} (12B).
    float aabb[6] = { mn[0],mn[1],mn[2], mx[0],mx[1],mx[2] };
    float radius = 0; for (int k = 0; k < 3; k++) { float a = mn[k] < 0 ? -mn[k] : mn[k], c = mx[k] < 0 ? -mx[k] : mx[k]; if (a > radius) radius = a; if (c > radius) radius = c; }
    nocullBounds(aabb, radius);   // HSR_NOCULL: V79-style "draw everything" — scene-spanning bounds so V205's frustum/occlusion/size culler can't drop the mesh
    static const uint8_t VSF0[12] = { 0x23,0xa5,0xe0,0xdb, 0x22,0x95,0x8f,0xf3, 0,0,0,0 };
    uint64_t vbH = murmur64A(vb.data(), vb.size()), ibH = murmur64A(ib.data(), ib.size());
    uint8_t vsf4[12] = {0}; memcpy(vsf4, &vbH, 8);
    uint8_t pf5[12]  = {0}; memcpy(pf5, &ibH, 8);
    float ones[2] = { 1.f,1.f }; uint8_t rf8[8]; memcpy(rf8, ones, 8);
    uint8_t attr[12] = { 0,0x32,0,0, 5,0x21,0,0, 4,0x13,0,0 };   // POS f32x3@0, UV f16x2@12, NORMAL fmt0x13@16
    FB b(vb.size() + ib.size() + 512);
    int vbo = b.createByteVector(vb.data(), vb.size()), ibo = b.createByteVector(ib.data(), ib.size()), ao = b.createStructVector(attr, 4, 3, 4);
    b.startObject(5); b.addStructSlot(0, VSF0, 8, 8); b.addScalar<uint32_t>(1, nv); b.addOffset(2, vbo); b.addOffset(3, ao); b.addStructSlot(4, vsf4, 8, 8); int vs = b.endObject();
    int vsvec = b.createOffsetVector({ vs });
    int matEmb = embeddedMatl.empty() ? 0 : b.createByteVector(embeddedMatl.data(), embeddedMatl.size());  // part.field3 = embedded MATL
    b.startObject(6); b.addOffset(0, vsvec); b.addOffset(1, ibo); b.addOffset(3, matEmb); b.addStructSlot(4, (const uint8_t*)aabb, 24, 4); b.addStructSlot(5, pf5, 8, 8); int part = b.endObject();   // ALIGNMENT FIX (IDA-proven, V205.2 verify @0xeba93c): part.f5 = murmur64A(IB) is an 8-byte 8-ALIGNED struct (line ~552: v158 & 7 == 0). vertexStream.f0/f4 same (lines 444/491: & 7). Cook emitted these 12B/align-4 -> when a hash field landed at a 4-but-not-8 offset the NEWLY-ADDED verifier rejected the whole mesh -> skinned crash. Old libshell never verified, so it "worked". (part.f6 also dropped: not in schema -> verifier skips voffset 16 (reads v155[6,7,9]); it was harmless either way.)
    int pv = b.createOffsetVector({ part });
    b.startObject(4); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); int lod = b.endObject();   // LOD screenSize MUST be non-zero: addScalar elides a 0.0 -> field omitted -> device MeshDefinition verifier REJECTS the mesh (all-dark). Size-cull is handled by HSR_NOCULL bounds, not this value.
    int lv = b.createOffsetVector({ lod });
    int matVec = b.createOffsetVector({});   // EMPTY materials vector (the dome's root.f2 is empty — a populated
                                             // {field0=4} element fails the verifier); materials come from part.field3
    b.startObject(9); b.addOffset(1, lv); b.addOffset(2, matVec); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); b.addScalar<float>(4, radius); b.addStructSlot(8, rf8, 8, 4); int root = b.endObject();
    b.finish(root, "MESH"); return b.output();
}

// ── RENDSHAD (file_id "SHAD"): wrap SPIR-V modules; renderer scans for 0x07230203 ─────────────────────────────
inline std::vector<uint8_t> encodeSurface(const std::vector<std::vector<uint8_t>>& spv) {
    size_t tot = 0; for (auto& m : spv) tot += m.size(); FB b(tot + 2048); std::vector<int> st;
    for (auto& code : spv) { int dv = b.createByteVector(code.data(), code.size()); b.startObject(1); b.addOffset(0, dv); st.push_back(b.endObject()); }
    int vec = b.createOffsetVector(st); b.startObject(1); b.addOffset(0, vec); int root = b.endObject(); b.finish(root, "SHAD"); return b.output();
}

// ── MATL (file_id "MATL"): field7 inline shader AssetRef + 0x6E4CC522 texture AssetRef + field5 matParams ──────
inline std::vector<uint8_t> encodeMatl(uint64_t shaderPkg, uint64_t shaderIng, uint64_t texPkg, uint64_t texIng, float r = 1, float g = 1, float bl = 1, float a = 1) {
    FB b(1024); float tint[4] = { r, g, bl, a }; int mp = b.createByteVector((uint8_t*)tint, 16);
    uint8_t texraw[24]; memset(texraw, 0, 24); uint32_t tt = 0x6E4CC522; memcpy(texraw, &texPkg, 8); memcpy(texraw + 8, &texIng, 8); memcpy(texraw + 16, &tt, 4); int tv = b.createStructVector(texraw, 24, 1, 8);
    b.startObject(10); b.addOffset(5, mp); b.addOffset(8, tv);
    uint8_t sref[20]; uint32_t st = 0xA1767FE9; memcpy(sref, &shaderPkg, 8); memcpy(sref + 8, &shaderIng, 8); memcpy(sref + 16, &st, 4); b.addStructSlot(7, sref, 20, 8);
    int root = b.endObject(); b.finish(root, "MATL"); return b.output();
}

// ── MATL (real device format, 176B): a cooked-env material structure with the shader + texture AssetRefs patched
//    to ours. The DEVICE's MaterialAsset/MeshAssetBuilder use this exact layout (fields 1..5, nested texture-binding
//    tables) — the older encodeMatl above (fields 5/7/8) is rejected. The mesh embeds this SAME material in
//    part.field3 so MeshAssetBuilder::generatePart can resolve the part's material. Patch points: shader AssetRef
//    pkg@48/ing@56, texture AssetRef pkg@120/ing@128. f5=matParams (1,1,0,0). ───────────────────────────────────
static const uint8_t MATL_TEMPLATE[176] = {
    0x1c,0x00,0x00,0x00,0x4d,0x41,0x54,0x4c,0x00,0x00,0x00,0x00,0x10,0x00,0x30,0x00,
    0x00,0x00,0x14,0x00,0x10,0x00,0x0c,0x00,0x08,0x00,0x04,0x00,0x10,0x00,0x00,0x00,
    0x2c,0x00,0x00,0x00,0x3c,0x00,0x00,0x00,0x68,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x8d,0x59,0x24,0x54,0xce,0x25,0x8b,0x60,0x47,0xba,0xd4,0x66,0xb9,0x67,0xfb,0xfb,
    0xe9,0x7f,0x76,0xa1,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,
    0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x08,0x00,0x20,0x00,0x08,0x00,0x04,0x00,
    0x08,0x00,0x00,0x00,0xde,0x82,0xc1,0xe9,0x52,0xb7,0x1f,0x4b,0xaf,0x9c,0x8b,0x5f,
    0x03,0x87,0x0d,0x3a,0x5d,0xff,0x72,0xc9,0x22,0xc5,0x4c,0x6e,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x0c,0x00,0x08,0x00,
    0x00,0x00,0x04,0x00,0x0a,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x0c,0xad,0x4a,0x5f,
};
inline std::vector<uint8_t> buildMaterial(uint64_t shaderPkg, uint64_t shaderIng, uint64_t texPkg, uint64_t texIng, bool patchShader = false) {
    std::vector<uint8_t> m(MATL_TEMPLATE, MATL_TEMPLATE + 176);
    // patchShader=true: point the shader AssetRef at OUR shader (same package as the mesh) — required for the
    // mesh's EMBEDDED material so MeshAssetBuilder's deserialize resolves it. patchShader=false: keep the
    // template's renderer_module shader ref — required for the ENTITY's standalone material so the descriptor
    // layout matches the real shader (our shader mismatches and crashes RenderSystem::CreateRenderGroups).
    if (patchShader) { memcpy(m.data() + 48, &shaderPkg, 8); memcpy(m.data() + 56, &shaderIng, 8); }
    memcpy(m.data() + 120, &texPkg, 8);   memcpy(m.data() + 128, &texIng, 8);     // texture AssetRef pkg/ing
    return m;
}

// ── ASMH assets.manifest: header + root.field1 = CONTENT vector of entry tables. The entry's "type" field is 8
//    bytes = [category fourcc][subtype fourcc] (e.g. REND:SHAD, MATL:MATL, HRZN:TMPL) — the device looks up the
//    AssetInitializer by this full 8-byte type, so the SUBTYPE is REQUIRED (else templates get HRZN:\0\0\0\0). ──
struct AsmhEntry { uint64_t pkg, ing; uint32_t tgt; std::string path; uint32_t fourcc; uint32_t subtype; uint32_t size; };
inline uint32_t assetFourcc(const std::vector<uint8_t>& d) { if (d.size() < 8) return 0x4E5A5248; uint32_t m; memcpy(&m, d.data() + 4, 4); if (m == 0x4C54414D) return 0x4C54414D; if (m == 0x4853454D || m == 0x52545854 || m == 0x44414853) return 0x444E4552; return 0x4E5A5248; }
// subtype fourcc (the second half of the 8-byte type): RENDMESH/TXTR/SHAD/MATL keep their file magic; HSTF/JSON
// templates use "TMPL" (the magic the TemplateAssetInitializer registers for).
inline uint32_t assetSubtype(const std::vector<uint8_t>& d) { if (d.size() < 8) return 0x4C504D54; uint32_t m; memcpy(&m, d.data() + 4, 4); if (m == 0x4853454D || m == 0x52545854 || m == 0x44414853 || m == 0x4C54414D) return m; return 0x4C504D54; /*"TMPL"*/ }
// Split a content path "meta/<pkg>/<rel...>/<subfile>" -> baseDir="meta/<pkg>" (first 2 segs), subName=last
// seg, relPath=the middle segs. Matches how the device's LocalAssetManifest joins f2[pkg]+f3[ing]+f4[tgt].
inline void decomposePath(const std::string& path, std::string& bd, std::string& rp, std::string& sn) {
    std::vector<std::string> seg; size_t a = 0;
    for (size_t i = 0; i <= path.size(); i++) if (i == path.size() || path[i] == '/') { seg.push_back(path.substr(a, i - a)); a = i + 1; }
    sn = seg.empty() ? "" : seg.back();
    bd = seg.size() >= 2 ? seg[0] + "/" + seg[1] : (seg.empty() ? "" : seg[0]);
    rp.clear(); for (size_t i = 2; i + 1 < seg.size(); i++) { if (!rp.empty()) rp += "/"; rp += seg[i]; }
}
inline std::vector<uint8_t> buildAsmh(const std::vector<AsmhEntry>& E) {
    // ── build the three hash->string maps (field 2 base-dir / field 3 rel-path / field 4 sub-name). The hash is
    //    the entry's pkg/ing/tgt; assets sharing a string MUST share the hash (keyForPath guarantees this). ──
    struct ME { uint64_t h; std::string s; };
    std::vector<ME> dirs, rels, subs;
    auto addU = [](std::vector<ME>& v, uint64_t h, const std::string& s) { for (auto& m : v) if (m.s == s) return; v.push_back({ h, s }); };
    for (auto& e : E) { std::string bd, rp, sn; decomposePath(e.path, bd, rp, sn);
        addU(dirs, e.pkg, bd); addU(rels, e.ing, rp); addU(subs, (uint64_t)e.tgt, sn); }   // sub hash is u32
    int N = (int)E.size();
    int p = 20; auto al = [&](int a) { while (p % a) p++; }; auto al8m4 = [&]() { while (p % 8 != 4) p++; };
    al(4); int rootP = p; p += 4 + 16;            // root table: soffset + 4 uoffsets (f1..f4)
    al(2); int rvt = p; p += 4 + 5 * 2;            // root vtable: vtsize,tablesize + 5 field slots (f0..f4)
    al(4); int f1vec = p; p += 4 + 4 * N;
    al(4); int f2vec = p; p += 4 + 4 * (int)dirs.size();
    al(4); int f3vec = p; p += 4 + 4 * (int)rels.size();
    al(4); int f4vec = p; p += 4 + 4 * (int)subs.size();
    al(2); int evt = p; p += 4 + 2 * 4;            // CONTENT entry vtable (4 fields)
    al(2); int mvt64 = p; p += 4 + 2 * 2;          // map element vtable: u64 hash@4, string uoff@12 (dirs/rels)
    al(2); int mvt32 = p; p += 4 + 2 * 2;          // map element vtable: u32 hash@4, string uoff@8  (sub-names)
    std::vector<int> elem; for (int i = 0; i < N; i++) { al8m4(); elem.push_back(p); p += 52; }  // entry table 52B (8-byte type field)
    auto mapelems64 = [&](std::vector<ME>& v) { std::vector<int> o; for (size_t i = 0; i < v.size(); i++) { al(8);   o.push_back(p); p += 16; } return o; };  // table%8==0 -> u64 hash@8 aligned
    auto mapelems32 = [&](std::vector<ME>& v) { std::vector<int> o; for (size_t i = 0; i < v.size(); i++) { al(4);   o.push_back(p); p += 12; } return o; };
    std::vector<int> d2 = mapelems64(dirs), d3 = mapelems64(rels), d4 = mapelems32(subs);
    std::vector<std::string> uniq; std::vector<int> uoff;
    auto strpos = [&](const std::string& s) -> int { for (size_t i = 0; i < uniq.size(); i++) if (uniq[i] == s) return uoff[i]; al(4); int o = p; uniq.push_back(s); uoff.push_back(o); p += 4 + (int)s.size() + 1; return o; };
    std::vector<int> spos(N); for (int i = 0; i < N; i++) spos[i] = strpos(E[i].path);
    std::vector<int> dpos(dirs.size()), rpos(rels.size()), npos(subs.size());
    for (size_t i = 0; i < dirs.size(); i++) dpos[i] = strpos(dirs[i].s);
    for (size_t i = 0; i < rels.size(); i++) rpos[i] = strpos(rels[i].s);
    for (size_t i = 0; i < subs.size(); i++) npos[i] = strpos(subs[i].s);
    std::vector<uint8_t> b(p, 0);
    auto w32 = [&](int o, uint32_t v) { memcpy(b.data() + o, &v, 4); }; auto wi32 = [&](int o, int32_t v) { memcpy(b.data() + o, &v, 4); }; auto w16 = [&](int o, uint16_t v) { memcpy(b.data() + o, &v, 2); }; auto w64 = [&](int o, uint64_t v) { memcpy(b.data() + o, &v, 8); };
    memcpy(b.data(), "ASMH", 4); w32(4, 2); w64(8, 0); w32(16, rootP - 16);
    wi32(rootP, rootP - rvt);
    w32(rootP + 4,  f1vec - (rootP + 4));  w32(rootP + 8,  f2vec - (rootP + 8));
    w32(rootP + 12, f3vec - (rootP + 12)); w32(rootP + 16, f4vec - (rootP + 16));
    w16(rvt, 14); w16(rvt + 2, 20); w16(rvt + 4, 0); w16(rvt + 6, 4); w16(rvt + 8, 8); w16(rvt + 10, 12); w16(rvt + 12, 16);
    w32(f1vec, (uint32_t)N); for (int i = 0; i < N; i++) { int slot = f1vec + 4 + 4 * i; wi32(slot, elem[i] - slot); }
    auto wvec = [&](int vp, std::vector<int>& off) { w32(vp, (uint32_t)off.size()); for (size_t i = 0; i < off.size(); i++) { int slot = vp + 4 + 4 * (int)i; wi32(slot, off[i] - slot); } };
    wvec(f2vec, d2); wvec(f3vec, d3); wvec(f4vec, d4);
    w16(evt, 12); w16(evt + 2, 52); int fo[4] = { 4, 32, 36, 44 }; for (int k = 0; k < 4; k++) w16(evt + 4 + 2 * k, (uint16_t)fo[k]);
    w16(mvt64, 8); w16(mvt64 + 2, 16); w16(mvt64 + 4, 8); w16(mvt64 + 6, 4);    // field0 hash u64@8, field1 string uoff@4
    w16(mvt32, 8); w16(mvt32 + 2, 12); w16(mvt32 + 4, 4); w16(mvt32 + 6, 8);    // field0 hash u32@4, field1 string uoff@8
    for (int i = 0; i < N; i++) { int ep = elem[i]; wi32(ep, ep - evt); w64(ep + 4, E[i].pkg); w64(ep + 12, E[i].ing); w32(ep + 20, E[i].tgt); wi32(ep + 32, spos[i] - (ep + 32)); w32(ep + 36, E[i].fourcc); w32(ep + 40, E[i].subtype); w32(ep + 44, E[i].size); }
    auto wmap64 = [&](std::vector<ME>& v, std::vector<int>& off, std::vector<int>& sp) { for (size_t i = 0; i < v.size(); i++) { int ep = off[i]; wi32(ep, ep - mvt64); wi32(ep + 4, sp[i] - (ep + 4)); w64(ep + 8, v[i].h); } };
    auto wmap32 = [&](std::vector<ME>& v, std::vector<int>& off, std::vector<int>& sp) { for (size_t i = 0; i < v.size(); i++) { int ep = off[i]; wi32(ep, ep - mvt32); w32(ep + 4, (uint32_t)v[i].h); wi32(ep + 8, sp[i] - (ep + 8)); } };
    wmap64(dirs, d2, dpos); wmap64(rels, d3, rpos); wmap32(subs, d4, npos);
    for (size_t i = 0; i < uniq.size(); i++) { int sp = uoff[i]; w32(sp, (uint32_t)uniq[i].size()); memcpy(b.data() + sp + 4, uniq[i].data(), uniq[i].size()); }
    return b;
}

// sub-target ids (StringId of the sub-asset name) — same constants the manifest uses.
enum : uint32_t { TGT_MESH = 0x4D455348, TGT_TEX = 0x6E4CC522, TGT_SURFACE = 0xA1767FE9, TGT_MATERIAL = 0x095BD446, TGT_TEMPLATE = 2719744159u };

// ── RENDTXTR (file_id "TXTR"): RGBA8 -> ASTC mip0 blocks wrapped in a FlatBuffer — exact inverse of
//    parseRendtxtrHeader (f3=w u16, f4=h u16, f6=format u8, f7=mipCount u16, f9=ASTC ubyte vector). The
//    decoder DERIVES the block footprint from the payload length, so we just keep (w,h,block) consistent. ──
// formatCode = color/pixel format enum (11 = sRGB albedo, like nuxd's tx_dome). f2 = ASTC block enum
// (6x6->18, 8x8->20, 12x12->24). The device GPU-uploads the full mip chain (ErrorInvalidArg without it / with a
// bad format). A box-filtered RGBA mip chain down to 1x1, each level ASTC-encoded and concatenated into f9.
inline std::vector<uint8_t> encodeRendTxtr(const uint8_t* rgba, int w, int h, int bw = 8, int bh = 8, uint8_t formatCode = 11, bool alphaWeighted = false, int capCols = 1, int capRows = 1, bool coveragePreserve = false) {
    // capCols/capRows > 1: SPRITESHEET flipbook atlas (capCols×capRows cells). CAP the mip chain so coarse mips never
    // AVERAGE ADJACENT CELLS (device "borders around the animated cards": at distance the GPU picks a coarse mip where
    // all cells blur together, bleeding the neighbour frame in as a rectangular border; desktop uses mip0 so it's clean).
    // Stop the chain while each cell is still >= 2 ASTC blocks.
    // alphaWeighted: for BLEND (transparent) textures, downsample mip RGB weighted by alpha
    // (mipRGB = Σ rgb·a / Σ a) instead of a straight box average. Straight-box lets fully-transparent
    // "garbage" RGB (the dust atlas is 21% α≈0 and some of those texels are pure white) bleed into the
    // small mips, so the device draws a bright halo tracing the card silhouette at minified/grazing
    // edges (the "white border"). Alpha-weighting gives invisible texels ZERO color weight = no bleed.
    // Matches the desktop renderer's createTextureImageAW so cook (device) == preview.
    // ── CONTENT-HASH DISK CACHE: re-encoding the same texture every re-cook is the dominant cost (lakeside = many
    //    2048² ASTC encodes ≈ the whole ~10min). Cache the ASTC mip chain by an FNV-1a hash of the FINAL RGBA (+dims+
    //    block), so re-cooks that change shaders/routing but NOT textures (i.e. every iteration here) skip the encode
    //    entirely -> near-instant re-cooks. HSR_NOTEXCACHE disables it. ──
    uint64_t hsh = 1469598103934665603ull; auto mix=[&](uint64_t v){ hsh=(hsh^v)*1099511628211ull; };
    { size_t nb=(size_t)w*h*4, n64=nb/8; const uint64_t* p=(const uint64_t*)rgba;
      for (size_t k=0;k<n64;k++) mix(p[k]); for (size_t k=n64*8;k<nb;k++) mix(rgba[k]);
      mix(((uint64_t)(uint32_t)w<<32)|(uint32_t)h); mix(((uint64_t)bw<<8)|(uint32_t)bh); mix(alphaWeighted?0x4157u:0u);
      mix(coveragePreserve?0xC0FEu:0u);   // coverage-preserving alpha mips = distinct chain
      mix(((uint64_t)(uint32_t)capCols<<32)|(uint32_t)capRows); }   // cap is part of the mip chain -> distinct cache entry
    std::error_code _ec; std::filesystem::path cdir = std::filesystem::temp_directory_path()/"hsr_texcache";
    bool useCache = !std::getenv("HSR_NOTEXCACHE");
    if (useCache) std::filesystem::create_directories(cdir,_ec);
    char cn[80]; snprintf(cn,sizeof cn,"%016llx_%d_%d_%dx%d.astcmip",(unsigned long long)hsh,w,h,bw,bh);
    std::filesystem::path cf = cdir/cn;
    std::vector<uint8_t> data;                                  // concatenated ASTC mip chain
    int mipCount = 0; { int d0 = w>h?w:h; while (true){ mipCount++; if (d0<=1) break; d0/=2; } }
    if (capCols > 1 || capRows > 1) {   // SPRITESHEET: keep each cell >= 4 ASTC blocks at the coarsest mip so cells NEVER
        // average together (no cross-cell bleed = no seam). 4 blocks (not 2) leaves a wider guard band -> the faint
        // residual seam the user saw at 2 blocks disappears. Slightly fewer mips = negligible extra shimmer on a small card.
        int cellW = w / (capCols>0?capCols:1), cellH = h / (capRows>0?capRows:1);
        int minCell = cellW < cellH ? cellW : cellH, blk = bw > bh ? bw : bh;
        int lvls = 1; while (lvls < mipCount && (minCell >> lvls) >= 4*blk) lvls++;   // coarsest level keeping cell >= 4 blocks
        if (lvls < mipCount) mipCount = lvls;
    }
    if (useCache) { std::ifstream f(cf,std::ios::binary); if (f){ data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); } }
    if (data.empty()) {                                        // cache miss -> ENCODE (multi-threaded by default)
        astcenc_config cfg{};
        if (astcenc_config_init(ASTCENC_PRF_LDR, (unsigned)bw, (unsigned)bh, 1, ASTCENC_PRE_MEDIUM, 0, &cfg) != ASTCENC_SUCCESS) return {};
        const astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());   // ASTC compress parallelizes across all cores
        astcenc_context* ctx = nullptr; if (astcenc_context_alloc(&cfg, nthreads, &ctx) != ASTCENC_SUCCESS) return {};
        std::vector<uint8_t> cur(rgba, rgba + (size_t)w * h * 4); int cw = w, ch = h; bool fail=false; int genLvl = 0;
        // ALPHA-TEST COVERAGE TARGET: the fraction of mip0 texels with alpha >= 128 (the shader's 0.5 discard).
        // Box-averaging alpha down the chain SHRINKS this coverage every level (leaf silhouettes thin, then the
        // per-8x8-ASTC-block quantization flips whole blocks under/over the threshold at distance = the "blocky"
        // leaves the device shows at range). Rescale each coarse mip's alpha so its coverage matches mip0 →
        // leaves keep their shape + area at every distance instead of dissolving into blocks.
        double covTarget = 0.0;
        if (coveragePreserve) { size_t cov=0, tot=(size_t)w*h; for (size_t k=0;k<tot;k++) if (rgba[k*4+3]>=128) cov++; covTarget = tot? (double)cov/tot : 0.0; }
        while (true) {
            int cols = (cw + bw - 1) / bw, rows = (ch + bh - 1) / bh;
            std::vector<uint8_t> blocks((size_t)cols * rows * 16);
            void* slice = cur.data();
            astcenc_image img{}; img.dim_x = (unsigned)cw; img.dim_y = (unsigned)ch; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
            std::vector<std::thread> pool;                      // worker threads 1..N-1; this thread runs index 0
            for (unsigned t=1;t<nthreads;t++) pool.emplace_back([&,t]{ astcenc_compress_image(ctx,&img,&swz,blocks.data(),blocks.size(),t); });
            astcenc_error e = astcenc_compress_image(ctx, &img, &swz, blocks.data(), blocks.size(), 0);
            for (auto& th:pool) th.join();
            if (e != ASTCENC_SUCCESS) { fail=true; break; }
            data.insert(data.end(), blocks.begin(), blocks.end());
            if (++genLvl >= mipCount || (cw == 1 && ch == 1)) break;   // stop at the (possibly cell-capped) mip count
            astcenc_compress_reset(ctx);                        // reuse the (multi-thread) context for the next mip
            int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
            std::vector<uint8_t> nx((size_t)nw * nh * 4);
            for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
                int sx = x * 2, sy = y * 2, sx1 = sx + 1 < cw ? sx + 1 : sx, sy1 = sy + 1 < ch ? sy + 1 : sy;
                const uint8_t* p[4] = { &cur[((size_t)sy*cw+sx)*4], &cur[((size_t)sy*cw+sx1)*4],
                                        &cur[((size_t)sy1*cw+sx)*4], &cur[((size_t)sy1*cw+sx1)*4] };
                uint8_t* o = &nx[((size_t)y*nw+x)*4];
                int aA = p[0][3], aB = p[1][3], aC = p[2][3], aD = p[3][3];
                o[3] = (uint8_t)((aA + aB + aC + aD + 2) / 4);               // alpha = plain box
                if (alphaWeighted) {
                    int wsum = aA + aB + aC + aD;
                    if (wsum > 0) for (int c = 0; c < 3; c++)
                        o[c] = (uint8_t)((p[0][c]*aA + p[1][c]*aB + p[2][c]*aC + p[3][c]*aD + wsum/2) / wsum);
                    else o[0] = o[1] = o[2] = 0;                             // fully transparent 2x2 -> no color to carry
                } else for (int c = 0; c < 3; c++)
                    o[c] = (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
            }
            if (coveragePreserve && covTarget > 0.0 && covTarget < 1.0) {
                // find an alpha SCALE s so fraction(alpha*s >= 128) == covTarget (binary search; monotonic in s).
                size_t np = (size_t)nw*nh;
                auto coverageAt=[&](float s)->double{ size_t c=0; for (size_t k=0;k<np;k++){ float a=nx[k*4+3]*s; if (a>=128.f) c++; } return (double)c/np; };
                float lo=0.03f, hi=32.f;
                for (int it=0; it<18; ++it){ float mid=0.5f*(lo+hi); if (coverageAt(mid) < covTarget) lo=mid; else hi=mid; }
                float s=0.5f*(lo+hi);
                for (size_t k=0;k<np;k++){ int a=(int)(nx[k*4+3]*s+0.5f); nx[k*4+3]=(uint8_t)(a<0?0:a>255?255:a); }
            }
            cur.swap(nx); cw = nw; ch = nh;
        }
        astcenc_context_free(ctx);
        if (fail) return {};
        if (useCache) { std::ofstream f(cf,std::ios::binary); f.write((const char*)data.data(),(std::streamsize)data.size()); }
    }
    uint8_t blkEnum = (bw == 6 && bh == 6) ? 18 : (bw == 12 && bh == 12) ? 24 : 20;   // 8x8 default = 20
    FB b(data.size() + 512);
    int dv = b.createByteVector(data.data(), data.size());
    b.startObject(10);
    b.addScalar<uint8_t>(2, blkEnum);
    b.addScalar<uint16_t>(3, (uint16_t)w); b.addScalar<uint16_t>(4, (uint16_t)h);
    b.addScalar<uint16_t>(5, 1);
    b.addScalar<uint8_t>(6, (uint8_t)mipCount);  // f6 = mipCount (floor(log2(maxDim))+1), NOT a color format
    b.addScalar<uint16_t>(7, 1);
    b.addOffset(9, dv);
    (void)formatCode;
    int root = b.endObject(); b.finish(root, "TXTR"); return b.output();
}

// ── LOSSLESS pass-through: package a texture's ORIGINAL ASTC mip chain (from the source KTX) verbatim into the
//    RENDTXTR wrapper, skipping decode→re-encode. Byte-for-byte the source quality (the fix for "textures lost
//    definition" — double ASTC compression at MEDIUM softens every re-encoded texture). Only valid for a plain
//    single-face KTX-ASTC whose footprint the device supports (6x6/8x8/12x12 enum). ──
inline std::vector<uint8_t> encodeRendTxtrFromAstc(const std::vector<uint8_t>& astcMips, int w, int h, int bw, int bh, int mipCount) {
    if (astcMips.empty() || w < 1 || h < 1 || mipCount < 1) return {};
    uint8_t blkEnum = (bw == 6 && bh == 6) ? 18 : (bw == 12 && bh == 12) ? 24 : (bw == 8 && bh == 8) ? 20 : 0;
    if (blkEnum == 0) return {};   // unsupported footprint -> caller re-encodes
    FB b(astcMips.size() + 512);
    int dv = b.createByteVector(astcMips.data(), astcMips.size());
    b.startObject(10);
    b.addScalar<uint8_t>(2, blkEnum);
    b.addScalar<uint16_t>(3, (uint16_t)w); b.addScalar<uint16_t>(4, (uint16_t)h);
    b.addScalar<uint16_t>(5, 1);
    b.addScalar<uint8_t>(6, (uint8_t)mipCount);
    b.addScalar<uint16_t>(7, 1);
    b.addOffset(9, dv);
    int root = b.endObject(); b.finish(root, "TXTR"); return b.output();
}

// ── VAT (Vertex Animation Texture) offset texture: the NON-skeletal animation path (vista fish/coral). Same
//    RENDTXTR wrapper as above but f2=5 (R16G16B16A16_SFLOAT, libshell TextureFormat 97), width=vertexCount,
//    height=frameCount; each texel = that vertex's OFFSET (x,y,z)+w=1 for that frame. vatunlitbasecolor samples
//    it by (uv1.x = vertexIndex/count, globalTime) and does pos += offset — so a node rotation = bake the rotated
//    offsets per frame, no skeleton/skinning. Full half-float mip chain (device GPU-uploads it like ASTC). ──
//    offsets layout: offsets[(frame*vertexCount + vert)*3 + {0,1,2}] in MESH-LOCAL space.
inline std::vector<uint8_t> encodeVatTexture(const std::vector<float>& offsets, int vertexCount, int frames) {
    if (vertexCount < 1 || frames < 1 || offsets.size() < (size_t)vertexCount * frames * 3) return {};
    int w = vertexCount, h = frames;
    std::vector<float> cur((size_t)w * h * 4);
    for (int f = 0; f < h; f++) for (int v = 0; v < w; v++) {
        size_t si = (size_t)f * w + v; const float* o = &offsets[si * 3];
        cur[si*4+0] = o[0]; cur[si*4+1] = o[1]; cur[si*4+2] = o[2]; cur[si*4+3] = 1.0f;
    }
    std::vector<uint8_t> data; int cw = w, ch = h, mipCount = 0;
    while (true) {
        mipCount++;
        for (size_t i = 0; i < (size_t)cw * ch * 4; i++) { uint16_t hf = f32_to_f16(cur[i]); data.push_back((uint8_t)hf); data.push_back((uint8_t)(hf >> 8)); }
        if (cw == 1 && ch == 1) break;
        int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        std::vector<float> nx((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
            int sx = x*2, sy = y*2, sx1 = sx+1 < cw ? sx+1 : sx, sy1 = sy+1 < ch ? sy+1 : sy;
            nx[((size_t)y*nw + x)*4 + c] = (cur[((size_t)sy*cw+sx)*4+c] + cur[((size_t)sy*cw+sx1)*4+c] + cur[((size_t)sy1*cw+sx)*4+c] + cur[((size_t)sy1*cw+sx1)*4+c]) * 0.25f;
        }
        cur.swap(nx); cw = nw; ch = nh;
    }
    FB b(data.size() + 512);
    int dv = b.createByteVector(data.data(), data.size());
    b.startObject(10);
    b.addScalar<uint16_t>(0, 2);
    b.addScalar<uint8_t>(2, 5);                          // f2 = 5 -> R16G16B16A16_SFLOAT (VAT)
    b.addScalar<uint16_t>(3, (uint16_t)w); b.addScalar<uint16_t>(4, (uint16_t)h);
    b.addScalar<uint16_t>(5, 1);
    b.addScalar<uint8_t>(6, (uint8_t)mipCount);
    b.addScalar<uint16_t>(7, 1);
    b.addOffset(9, dv);
    int root = b.endObject(); b.finish(root, "TXTR"); return b.output();
}

// ── deterministic asset-key RNG (so re-cooks are reproducible) ────────────────────────────────────────
struct CookRng { uint64_t s; explicit CookRng(uint64_t seed = 0xC0FFEEULL) : s(seed ? seed : 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } };
struct AssetKey3 { uint64_t pkg, ing; uint32_t tgt; };
inline AssetKey3 makeKey(CookRng& r, uint32_t tgt, bool zeroPkg = false) {
    uint64_t pkg = zeroPkg ? 0 : r.next();
    uint64_t ing = r.next() | (1ull << 63);                 // bit63 set + always >= 2^32 (manifest reader filters small ings)
    return { pkg, ing, tgt };
}
// FNV-1a 64-bit — our own deterministic hash for base-dir / rel-path (the device resolves via the field2/3 maps,
// so the exact hash doesn't matter as long as it's CONSISTENT per string and present in the map). bit63 set so the
// manifest reader's ing>=2^32 filter passes.
inline uint64_t h64fnv(const std::string& s) { uint64_t h = 1469598103934665603ull; for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h | (1ull << 63); }
// sub-name -> StringId (tgt) = MurmurHash3_x86_32(subName, seed 0). CRACKED from nuxd's field4 (tex/shader/
// material/template all match murmur3 exactly); murmur3_x86_32 is the libshell StringId hash (types.h).
inline uint32_t subNameStringId(const std::string& sn) { return murmur3_x86_32(sn.data(), sn.size(), 0); }
// base-dir / rel-path -> u64 StringId. That 64-bit hash isn't cracked yet, so for the dir/rel STRINGS we reuse
// from nuxd we use the REAL u64 hashes harvested from nuxd's manifest field2/3 (the loose-loader scans our files
// and computes the same hash for the same string, so the AssetRef matches). Unknown strings fall back to FNV
// (won't resolve on-device until the u64 hash is cracked). [[project_hsl_ondevice_envload]]
inline uint64_t pathStringId(const std::string& s, bool* known = nullptr) {
    static const std::pair<const char*, uint64_t> T[] = {
        {"meta/nux", 0x5F8B9CAF4B1FB752ull}, {"meta/renderer_module", 0x608B25CE5424598Dull}, {"meta/shell-env-nux-d", 0xFDA57FCF9EA7713Full},
        {"space.hstf", 0xD43FDE9CD96341FAull}, {"nux_d/nux_d.hstf", 0x8B19C4A3CFB24686ull},
        {"shaders/unlit.surface", 0x78686234E7611EFCull}, {"shaders/unlitBlend.surface", 0xFBFB67B966D4BA47ull}, {"shaders/unlitBlendSkinned.surface", 0xFCF6A70495BF7B47ull},
        {"nux_d/unlit_floor_x_12_inner_01.material", 0x5F5792C558FA8EBCull}, {"nux_d/unlit_floor_x_12_outer_01.material", 0x1D90D5C6CECC33DFull},
        {"nux_d/unlit_motes_c_04.material", 0x5E0C17B09CF2662Eull}, {"nux_d/unlit_dome_c_top_01.material", 0x45842E1785C6ABD6ull},
        {"nux_d/tx_dome_a_03.png", 0x5B56E58C3DFD35FAull}, {"nux_d/tx_mote_c_05.png", 0x31DA3C216D0FED77ull}, {"nux_d/tx_iridescent_06.png", 0x8BF874E15DC99444ull},
        {"nux_d/tx_skybox_i_03.png", 0x3B3C15362A3D5368ull}, {"nux_d/tx_hotspot_a_02.png", 0xC972FF5D3A0D8703ull},
        {"nux_d/tx_floor_12_304_inner_01.png", 0xF33E70EFD3E2DC1Full}, {"nux_d/tx_floor_12_304_outer_01.png", 0x4BC84AC31E34B035ull},
        {"nux_d/OVE-ARNX-20220218b_aa.fbx", 0x2EFC58E9E24EAD2Eull},
    };
    for (auto& p : T) if (s == p.first) { if (known) *known = true; return p.second; }
    if (known) *known = false; return h64fnv(s);
}
// keyForPath — the AssetRef for a content path, consistent with the field2/3/4 maps buildAsmh emits AND with the
// loose loader's on-device file scan: pkg = StringId(baseDir), ing = StringId(relPath), tgt = murmur3(subName).
inline AssetKey3 keyForPath(const std::string& contentPath) {
    std::string bd, rp, sn; decomposePath(contentPath, bd, rp, sn);
    return { pathStringId(bd), pathStringId(rp), subNameStringId(sn) };
}
inline std::string u64s(uint64_t v) { char b[32]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); return b; }
inline std::string refJson(const AssetKey3& k) {
    return std::string("{\"packageOrRemoteId\":\"") + u64s(k.pkg) + "\",\"ingestionId\":\"" + u64s(k.ing) + "\",\"targetId\":" + std::to_string(k.tgt) + "}";
}
inline std::string makeUuid(CookRng& r) {
    uint64_t a = r.next(), b = r.next(); uint8_t x[16];
    memcpy(x, &a, 8); memcpy(x + 8, &b, 8); x[6] = (x[6] & 0x0F) | 0x40; x[8] = (x[8] & 0x3F) | 0x80;
    char s[40]; snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        x[0],x[1],x[2],x[3],x[4],x[5],x[6],x[7],x[8],x[9],x[10],x[11],x[12],x[13],x[14],x[15]);
    return s;
}
inline std::vector<uint8_t> jbytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// ── HSTF JSON writers — field set matches REAL V203 envs (IDA-verified, sub_21D8E4C): MeshPlatformComponent
//    carries just the mesh ref; Transform carries full pos/rot/scale; the extra render flags default safely. ──
inline std::string posJson(const float p[3]) { char b[96]; snprintf(b,sizeof b,"{\"x\":%g,\"y\":%g,\"z\":%g}",p[0],p[1],p[2]); return b; }
inline std::string comp(const char* cls, int ver, const std::string& data) {
    return std::string("{\"data\":{\"class\":\"horizon::platform_api::") + cls + "\",\"version\":" + std::to_string(ver) +
           ",\"data\":" + data + "},\"dataType\":\"horizon::DataDefinitionAsset\"}";
}
// A drawable entity: transform (pos/rot-euler/scale) + mesh ref + material refs.
inline std::string entityJson(const std::string& id, const std::string& name,
                              const float pos[3], const float rotEuler[3], const float scale[3],
                              const AssetKey3& meshRef, const std::vector<AssetKey3>& matRefs,
                              const AssetKey3& colliderRef = AssetKey3{0,0,0}, const std::string& extraComp = std::string(),
                              int meshVer = 5, bool animated = false, const std::string& poseAnimVal = std::string()) {
    // GROUND TRUTH — extracted from the WORKING calming_butterflies.hstf (a skinned+animated entity that renders AND
    // animates on device). Its component layout is EXACTLY: Transform **v1** -> Mesh **v5** -> Material v1
    // {materials,constantParameters:[],textureParameters:[]} -> AnimatorPlatformComponent v4 {skeleton,animations,
    // sockets:[]} (animator is LAST). Earlier guesses (Transform v2, Mesh v6, Animator-before-Material) all CONTRADICT
    // this reference and are removed. Static entities = the same minus the animator.
    char tb[256]; snprintf(tb,sizeof tb,
        "{\"localPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localRotation\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localScale\":{\"x\":%g,\"y\":%g,\"z\":%g}}",
        pos[0],pos[1],pos[2], rotEuler[0],rotEuler[1],rotEuler[2], scale[0],scale[1],scale[2]);
    std::string mats; for (size_t i=0;i<matRefs.size();++i){ if(i)mats+=","; mats+=refJson(matRefs[i]); }
    (void)animated;
    bool skinned = !extraComp.empty();
    // SKINNED CULL FIX (oceanarium whale parity): the device IGNORES the entity transform when rendering a skinned mesh
    // (it places it via the skeleton) but the CULLER uses the transform's position as the bounds center -> a populated
    // transform makes the cull center MISMATCH where the mesh actually renders -> false frustum cull (the omnidroid).
    // Whale01 ships an EMPTY TransformPlatformComponent ({}) so the cull falls back to the skeleton-derived bounds.
    // Default ON for skinned; HSR_HZSOLIDTF restores the old populated transform.
    std::string tfData = (skinned && !std::getenv("HSR_HZSOLIDTF")) ? std::string("{}") : std::string(tb);
    std::string comps =
        comp("TransformPlatformComponent", 1, tfData) + "," +
        comp("MeshPlatformComponent", meshVer, std::string("{\"mesh\":") + refJson(meshRef) + "}") + "," +
        comp("MaterialPlatformComponent", 1, std::string("{\"materials\":[") + mats + "],\"constantParameters\":[],\"textureParameters\":[]}");
    if (skinned) comps += "," + extraComp;   // AnimatorPlatformComponent LAST (exact calming butterfly order)
    // CULL — THE REAL FIX (horizon::renderer::MeshPartBoundsOverride): the device RECOMPUTES each renderable's cull
    // bound at runtime (for skinned meshes from the posed skeleton EVERY frame; it IGNORES the cooked RENDMESH AABB and
    // added mesh verts — the AABB/PART.f6/bounds-vert/anchor attempts all did NOTHING, device-proven via render-trace).
    // That recomputed bound OSCILLATES with the animation and flickers out of the frustum → "shows then goes invisible".
    // DEVICE-PROVEN (render-trace dump): the skinned omnidroid pieces that GOT this override read center(0,0,0)/half=1e5
    // and stay VISIBLE, while any entity WITHOUT it (the omnidroid's body renderable + walls) still oscillates and
    // FRUSTUM-CULLs → so it must go on EVERY mesh entity, not just `skinned` ones (user: "any animated mesh").
    // MeshPartBoundsOverride {partIndex, boundsCenter, boundsHalfSize} (IDA: type reg sub_2B712FC, field reflect
    // sub_2B8CF18) REPLACES the per-part cull bound with a fixed AABB. Emit a SCENE-SPANNING box (center 0, halfSize 1e5)
    // so the cull bound always covers the view → never frustum/occlusion-culled. partIndex 0 = each entity's single
    // cooked part (one material per entity). Skip the NavDebug overlay. HSR_NOBOUNDSOVERRIDE disables for A/B.
    if (name != "NavDebug" && !std::getenv("HSR_NOBOUNDSOVERRIDE"))
        comps += ",{\"data\":{\"class\":\"horizon::renderer::MeshPartBoundsOverride\",\"version\":1,\"data\":"
                 "{\"partIndex\":0,\"boundsCenter\":{\"x\":0,\"y\":0,\"z\":0},"
                 "\"boundsHalfSize\":{\"x\":100000,\"y\":100000,\"z\":100000}}},\"dataType\":\"horizon::DataDefinitionAsset\"}";
    // walkable: collision mesh + static physics body (so locomotion/teleport land on it)
    if (colliderRef.pkg || colliderRef.ing || colliderRef.tgt) {
        comps += "," + comp("ColliderMeshPlatformComponent", 1, std::string("{\"meshAsset\":") + refJson(colliderRef) + "}");
        comps += "," + comp("PhysicsBodyPlatformComponent", 1, "{\"type\":\"StaticCollision\"}");
    }
    // "poseAnimation" = a TOP-LEVEL entity key (IDA V205 EnvironmentEntitySystem), sibling to components[], NOT a component.
    std::string poseKv = poseAnimVal.empty() ? std::string() : (std::string(",\"poseAnimation\":") + poseAnimVal);
    return std::string("{\"id\":\"") + id + "\",\"name\":\"" + name + "\",\"components\":[" + comps + "]" + poseKv + ",\"attributes\":[]}";
}
// FAR BACKDROP -> the SKYBOX pass (depth-clamped, NO far-clip) so distant geometry the main camera would clip still draws —
// exactly how official vistas avoid the far plane. SkyboxPlatformComponent{skyboxMesh,colorTexture}, NO Transform (the skybox
// is camera-locked, fine for distant scenery), NO material/shader (the shell's built-in skybox shader renders it). Matches
// the real vista recipe (com_meta_shell_env_vista_central → central_lighting.hstf). [[project_hsr_v205_culling_farplane]]
// FULL vista recipe (extracted from com_meta_shell_env_vista_calming → calming_lighting.hstf "Skybox" entity): the
// SkyboxPlatformComponent needs **colorTexture + reflectionMap** AND a sibling **MaterialPropertyOverridesPlatformComponent**
// pinning `@runtime.lighting.ibl.reflection.max.adaption.factor` = 0 — WITHOUT the override + a real colorTexture the shell's
// skybox shader renders BLACK (the skyBoxCubeMap path fails). skyboxMesh = our cooked backdrop mesh (loaded as the dome);
// colorTexture/reflectionMap = its texture. NO Transform (the skybox is camera-locked by design — fine for km-distant scenery).
// This is the ONLY device-proven way past the hard PortalStereoCamera far=5000 clip. [[project_hsr_eye_subcamera_farclip]]
inline std::string skyboxEntityJson(const std::string& id, const std::string& name, const AssetKey3& meshRef, const AssetKey3& texRef) {
    std::string sky = std::string("{\"skyboxMesh\":") + refJson(meshRef) +
        ",\"colorTexture\":" + refJson(texRef) + ",\"reflectionMap\":" + refJson(texRef) + "}";
    std::string mpo = "{\"constantParameters\":[{\"class\":\"horizon::renderer::ConstantMaterialParameter\",\"data\":{"
        "\"parameterName\":\"@runtime.lighting.ibl.reflection.max.adaption.factor\",\"value\":{\"x\":0,\"y\":0,\"z\":0,\"w\":0}}}]}";
    std::string comps = comp("SkyboxPlatformComponent", 1, sky) + "," +
                        comp("MaterialPropertyOverridesPlatformComponent", 1, mpo);
    return std::string("{\"id\":\"") + id + "\",\"name\":\"" + name + "\",\"components\":[" + comps + "],\"attributes\":[]}";
}
// Component wrapper with an EXPLICIT full class string (Shell-anim components are NOT under horizon::platform_api::).
inline std::string compFull(const std::string& fullClass, int ver, const std::string& data) {
    return std::string("{\"data\":{\"class\":\"") + fullClass + "\",\"version\":" + std::to_string(ver) +
           ",\"data\":" + data + "},\"dataType\":\"horizon::DataDefinitionAsset\"}";
}
// EXTEND THE MAIN-VIEW FAR CLIP PLANE — the cooked-APK-only fix for distant geometry the shell's default far plane clips
// (no root/patch/skybox). GROUND TRUTH from every official vista (com_meta_shell_env_vista_*.apk → space.hstf): the env
// ships a **horizon::platform_api::ScenePlatformComponent version 3** carrying nearClippingPlane + farClippingPlane (+ fog).
// (focused=150000, calming=20000, horror/oceanarium=2000.) That sets the internal ClippingPlanesComponent on the view.
// fogDensity=0 → no fog (so the distant city isn't hazed out). One Scene entity per env.
inline std::string sceneEntityJson(const std::string& id, float nearP, float farP) {
    // ⛔ PLAIN DECIMAL, never %g: snprintf("%g",1e6) emits "1e+06" (scientific) and the device's HSTF JSON parser
    // does NOT accept exponent form → farClippingPlane fails to parse → falls back to the shell's SMALL default far
    // → distant geometry CLIPS (the "far plane still interferes" bug). %.3f forces "1000000.000" which parses.
    // ⛔ FOG: every official env ships fogColor != black AND fogDensity != 0 (e.g. calming: {0.36,0.57,0.68},
    // start=3000, density=1e-5). A BLACK fogColor + fogDensity=0 made the device fade distant geometry to a BLACK
    // dome ("black circle blocking the view from afar") / treat the scene config as inert. Match the official: a
    // subtle non-black haze + non-zero density, with fogStart pushed near the far so foreground geometry is unfogged.
    // HSR_FOGCOLOR=r,g,b overrides the haze color. fogStart = 0.6*far → the room/city (well inside) stays crisp.
    float fr=0.05f, fg=0.06f, fb=0.09f, fStart=farP*0.6f, fDens=1.0e-5f;
    if (const char* fcc = std::getenv("HSR_FOGCOLOR")) { float a,b,c; if (sscanf(fcc,"%f,%f,%f",&a,&b,&c)==3){fr=a;fg=b;fb=c;} }
    if (const char* fs = std::getenv("HSR_FOGSTART"))   { float v=(float)atof(fs); if (v>=0) fStart=v; }
    if (const char* fd = std::getenv("HSR_FOGDENSITY")) { float v=(float)atof(fd); if (v>=0) fDens=v; }
    char d[360]; snprintf(d, sizeof d,
        "{\"nearClippingPlane\":%.4f,\"farClippingPlane\":%.3f,\"fogColor\":{\"r\":%.4f,\"g\":%.4f,\"b\":%.4f,\"a\":1},\"fogStart\":%.3f,\"fogDensity\":%.8f}",
        nearP, farP, fr, fg, fb, fStart, fDens);
    // ⛔ FAR-CLIP MECHANISM — CRACKED IN IDA (V205 libshell, 2026-06-18):
    //   env `platform_api::ScenePlatformComponent.farClippingPlane`(struct@16)  --vtable slot-3 bridge
    //   `ScenePlatformComponent_vf3` (0x21E499C)-->  `renderer::SceneComponent.far`(struct@48)  --> camera projection.
    //   The SceneComponent default ctor (0x252A8DC) sets far = **500.0** (0x43FA0000); vf3 OVERWRITES it with our
    //   value only if it parsed. near@44+far@48 sit together (fog is separate @96+), so far@48 IS the projection clip.
    //   vf3 is a GENERIC component-lifecycle slot (shared `ChangeNotifierComponent` iface) → it runs for ANY loaded
    //   ScenePlatformComponent (vista OR combined OR footprint) — there is no vista-only gate. So the ONLY way our
    //   far doesn't apply is if the component never instantiates. ⇒ EMIT ScenePlatformComponent FIRST and ALONE so a
    //   sibling-component parse failure can't take the whole Scene entity (and our far) down with it. If the entity
    //   were dropped, far reverts to 500 = exactly the "head-locked black dome that clips the distance".
    std::string comps = comp("ScenePlatformComponent", 3, d);
    // The arrival-fog animation (horizon::hpi::FogTravelComponent v7, verbatim from calming) is OPTIONAL and a parse
    // RISK (hand-written hpi AnimatedCurve JSON) — gate it behind HSR_FOGTRAVEL so the default cook keeps the far
    // bulletproof. It is NOT needed for the far clip (the user confirmed: fog is not the issue).
    if (std::getenv("HSR_FOGTRAVEL")) {
        static const char* fogTravel =
          "{\"data\":{\"class\":\"horizon::hpi::FogTravelComponent\",\"version\":7,\"data\":{"
          "\"propSkyboxFogFadeStartMs\":1400.0,"
          "\"propFogDistanceCurve\":{\"class\":\"horizon::hpi::AnimatedCurve\",\"data\":{\"keyframes\":["
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":375.0}},"
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":2000.0,\"value\":3000.0}}]}},"
          "\"propFogDensityCurve\":{\"class\":\"horizon::hpi::AnimatedCurve\",\"data\":{\"keyframes\":["
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"value\":1.0}},"
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":250.0,\"value\":0.10000000149011612}},"
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":500.0,\"value\":0.009999999776482582}},"
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":1000.0,\"value\":0.0010000000474974513}},"
            "{\"class\":\"horizon::hpi::KeyFrame\",\"data\":{\"time\":2000.0,\"value\":9.999999747378752e-05}}]}}}},"
          "\"dataType\":\"horizon::DataDefinitionAsset\"}";
        comps += ","; comps += fogTravel;
    }
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Scene\",\"components\":[" + comps + "],\"attributes\":[]}";
}
// Non-skeletal pose animation VALUE object. ⛔ IDA V205 (device build): EnvironmentEntitySystem (sub_1148BFC) registers
// the pose animation under the TOP-LEVEL entity key **"poseAnimation"** — NOT as a horizon::platform_api:: component in
// components[] (that's why every earlier cook's pose component was SILENTLY IGNORED -> static wisp -> the "fade"). The
// handler -> loader sub_1162EF0 reads start/end Position/Orientation/Scale + start/end time and LERPS the entity pose over
// [start,end]. The mesh keeps its normal (unlitblend) shader, so transparency is unaffected; only the transform scales.
inline std::string poseAnimationValue(const float pos[3], const float startScale[3], const float endScale[3], float dur, const float* transDelta = nullptr) {
    float ep[3] = { pos[0], pos[1], pos[2] };   // endPosition = pos (+ node-translation delta for cars/train)
    if (transDelta) { ep[0]+=transDelta[0]; ep[1]+=transDelta[1]; ep[2]+=transDelta[2]; }
    char b[640]; snprintf(b, sizeof b,
        "{\"startPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"startOrientation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1},\"startScale\":{\"x\":%g,\"y\":%g,\"z\":%g},"
        "\"endPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"endOrientation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1},\"endScale\":{\"x\":%g,\"y\":%g,\"z\":%g},"
        "\"start\":0,\"end\":%g}",
        pos[0],pos[1],pos[2], startScale[0],startScale[1],startScale[2],
        ep[0],ep[1],ep[2], endScale[0],endScale[1],endScale[2], dur);
    return b;
}
// AnimatorPlatformComponent v4 (HZANIM): binds a skeleton (HZAN:SKEL) + an animation (HZAN:ANIM/ACL clip). Drives the
// skinned mesh's sbSkinningMatrices (HzAnimSystem). Same shape nuxd's prism_wave/motes use.
inline std::string animatorComponentJson(const AssetKey3& skel, const AssetKey3& anim) {
    return comp("AnimatorPlatformComponent", 4,
        std::string("{\"skeleton\":") + refJson(skel) +
        ",\"animations\":[{\"class\":\"horizon::animation::Animation\",\"data\":{\"animation\":" + refJson(anim) + "}}],\"sockets\":[]}");   // "sockets" (calming butterflies = WORKING skinned) NOT "remapPages" (nuxd prism_wave = invisible, same bug)
}
// Spawn point: where the player starts (just above the floor). Without one the shell uses a fallback that can
// leave you off the floor. Transform + SpawnPointPlatformComponent{stateEnabled,allowStart}.
inline std::string spawnPointEntityJson(const std::string& id, const float pos[3]) {
    char tb[256]; snprintf(tb,sizeof tb,
        "{\"localPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localRotation\":{\"x\":0,\"y\":0,\"z\":0},\"localScale\":{\"x\":1,\"y\":1,\"z\":1}}",
        pos[0],pos[1],pos[2]);
    std::string comps = comp("TransformPlatformComponent", 1, tb) + "," +
        comp("SpawnPointPlatformComponent", 1, "{\"stateEnabled\":true,\"allowStart\":true}");
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Spawn Point\",\"components\":[" + comps + "],\"attributes\":[]}";
}
// Environment ROOT entity — every scene needs one (locomotion: "Invalid environment root cannot teleport player"
// without it). Identity transform, empty data {}; the scene entities parent to it via RelationChildOf.
inline std::string rootEntityJson(const std::string& id) {
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Root\",\"components\":[" +
        comp("TransformPlatformComponent", 1, "{}") + "],\"attributes\":[]}";
}
// Background music: an entity whose SoundPlatformComponent (v14) AUTO-STARTs + LOOPs a SoundAsset. The asset is the
// V79 `_BACKGROUND_LOOP.ogg` shipped RAW (manifest type FMOD:SND -> AudioFmodSoundAssetInitializer -> FMOD
// createSound, which auto-detects ogg — no FSB/SoundDefinition needed). IDA: field names from reflection sub_22C6D44
// (soundAsset@16 is the only required field; loop/autoStart/playVolume are simple bool/f32). Placed at the spawn so a
// default 3D emitter is still audible right where the player starts.
inline std::string audioEntityJson(const std::string& id, const float pos[3], const AssetKey3& soundRef) {
    char tb[256]; snprintf(tb,sizeof tb,
        "{\"localPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localRotation\":{\"x\":0,\"y\":0,\"z\":0},\"localScale\":{\"x\":1,\"y\":1,\"z\":1}}",
        pos[0],pos[1],pos[2]);
    std::string snd = comp("SoundPlatformComponent", 14,
        std::string("{\"soundAsset\":") + refJson(soundRef) + ",\"playVolume\":1,\"loop\":true,\"autoStart\":true}");
    std::string comps = comp("TransformPlatformComponent", 1, tb) + "," + snd;
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Background Audio\",\"components\":[" + comps + "],\"attributes\":[]}";
}
inline std::string relChildOf(const std::string& child, const std::string& parent) {
    return std::string("{\"relationshipType\":\"RelationChildOf\",\"source\":\"") + child + "\",\"destination\":\"" + parent + "\"}";
}
// Invisible ground collider (Transform + ColliderMesh + StaticCollision, no render mesh) — a flat PHSX:3MSH plane
// scaled to cover the scene so you can walk anywhere on a ported env. (Set HSR_NAVMESH=<meshIndex> to base its
// size/position on a specific mesh instead of the whole-scene bounds.)
inline std::string colliderGroundEntityJson(const std::string& id, const float pos[3], const float scale[3], const AssetKey3& colliderRef) {
    char tb[256]; snprintf(tb,sizeof tb,
        "{\"localPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localRotation\":{\"x\":0,\"y\":0,\"z\":0},\"localScale\":{\"x\":%g,\"y\":%g,\"z\":%g}}",
        pos[0],pos[1],pos[2], scale[0],scale[1],scale[2]);
    std::string comps = comp("TransformPlatformComponent", 1, tb) + "," +
        comp("ColliderMeshPlatformComponent", 2, std::string("{\"meshAsset\":") + refJson(colliderRef) + "}") + "," +   // v2 + PhysicsBody v7 = haven2025 navmesh versions (v1 dropped -> not walkable)
        comp("PhysicsBodyPlatformComponent", 7, "{\"type\":\"StaticCollision\"}");
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Ground\",\"components\":[" + comps + "],\"attributes\":[]}";
}
// ── NAVMESH -> ColliderBox grid ───────────────────────────────────────────────────────────────────────────────────
// The device's PhysX REJECTS our PC-cooked triangle-mesh SEBD: the cooked RTree is binary-incompatible across PhysX
// build versions (ours 4.1.2 release vs the device's checked build) so the narrow-phase SIGSEGVs traversing it — proven
// even for a trivial flat 2-tri mesh, while Meta's own realfloor (cooked by THEIR PhysX) walks fine. ColliderBox shapes
// are instead built by the device's OWN PhysX from `halfExtents` (NO cooked data at all — that's how haven2025's 44
// invisible walls work), so they're always compatible. We approximate the walkable surface as a grid of axis-aligned
// boxes (one per coarse cell, greedy-merged along X into runs) whose TOPS sit at the road height -> walkable everywhere.
inline std::vector<std::array<float,6>> navmeshToBoxes(const std::vector<float>& V, float cell, float thick, int maxBoxes) {
    std::vector<std::array<float,6>> boxes;           // {cx,cy,cz, hx,hy,hz}
    if (V.size() < 9) return boxes;
    float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
    for (size_t i=0;i+2<V.size();i+=3) for(int k=0;k<3;k++){ mn[k]=std::min(mn[k],V[i+k]); mx[k]=std::max(mx[k],V[i+k]); }
    float extx=mx[0]-mn[0], extz=mx[2]-mn[2];
    // ONE box PER occupied cell, top at that cell's EXACT surface height (no cross-height merging -> no float/phasing),
    // each box widened by an overlap margin so neighbours seal (no seams to fall through). Fine cell -> hugs the road's
    // height/tilt closely via small steps. Coarsen only if the box count would blow past maxBoxes.
    for (int attempt=0; attempt<14; ++attempt) {
        boxes.clear();
        int NX=(int)(extx/cell)+1, NZ=(int)(extz/cell)+1; if(NX<1)NX=1; if(NZ<1)NZ=1;
        if ((long long)NX*NZ > 900000) { cell*=1.35f; continue; }
        std::vector<float> H((size_t)NX*NZ, -1e30f);
        for (size_t i=0;i+2<V.size();i+=3){
            int gx=(int)((V[i]-mn[0])/cell), gz=(int)((V[i+2]-mn[2])/cell);
            if(gx<0)gx=0; if(gx>=NX)gx=NX-1; if(gz<0)gz=0; if(gz>=NZ)gz=NZ-1;
            float& h=H[(size_t)gz*NX+gx]; if(V[i+1]>h) h=V[i+1];
        }
        float hm = cell*0.5f + cell*0.22f;             // half-cell + overlap margin so adjacent boxes seal the seams
        for (int gz=0; gz<NZ; gz++) for (int gx=0; gx<NX; gx++){
            float y=H[(size_t)gz*NX+gx]; if(y < -1e29f) continue;
            float cx=mn[0]+(gx+0.5f)*cell, cz=mn[2]+(gz+0.5f)*cell;
            boxes.push_back({cx, y-thick, cz, hm, thick, hm});   // top face sits exactly on the surface height
        }
        if ((int)boxes.size() <= maxBoxes) break;
        cell*=1.35f;                                   // too many boxes -> coarsen + retry
    }
    return boxes;
}
// EXACT-FOLLOW variant: one thin box PER walkable triangle, tilted to that triangle's plane (so it follows the road's
// real shape/height/tilt — REUSES the mesh triangles instead of a rebuilt grid). Each box carries a localRotationQuat
// (the device Transform reads it @112, IDA sub_25383A0). Returns {cx,cy,cz, hx,hy,hz, qx,qy,qz,qw} per box.
inline std::vector<std::array<float,10>> navmeshTrisToBoxes(const std::vector<float>& V, const std::vector<uint32_t>& I, float thick, float margin, float inflate, bool smooth=false) {
    std::vector<std::array<float,10>> boxes;
    size_t nt = I.size()/3;
    auto dot=[](const float*a,const float*b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    // SMOOTH (HSR_NAVSMOOTH): per-vertex normals = area-weighted avg of adjacent face normals. Tilting each box to the
    // averaged surface normal (instead of its own flat triangle) makes neighbouring boxes share a tilt -> the per-edge
    // CREASES that make the box navmesh feel jaggedy on device blend out into a smooth slope. (Tops drift a hair off the
    // exact verts, fine for a walkable collider.) Off by default = exact per-triangle follow.
    std::vector<float> vn;
    if (smooth) {
        vn.assign(V.size(), 0.f);
        for (size_t t=0;t<nt;t++){ uint32_t ia=I[t*3],ib=I[t*3+1],ic=I[t*3+2];
            if((size_t)ia*3+2>=V.size()||(size_t)ib*3+2>=V.size()||(size_t)ic*3+2>=V.size()) continue;
            const float*a=&V[ia*3];const float*b=&V[ib*3];const float*c=&V[ic*3];
            float e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]},e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
            float fn[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};   // area-weighted (un-normalized)
            if(fn[1]<0){fn[0]=-fn[0];fn[1]=-fn[1];fn[2]=-fn[2];}
            for(uint32_t vi:{ia,ib,ic}) for(int k=0;k<3;k++) vn[(size_t)vi*3+k]+=fn[k];
        }
        for(size_t v=0;v+2<vn.size();v+=3){ float l=std::sqrt(vn[v]*vn[v]+vn[v+1]*vn[v+1]+vn[v+2]*vn[v+2]); if(l>1e-6f){vn[v]/=l;vn[v+1]/=l;vn[v+2]/=l;} else vn[v+1]=1.f; }
    }
    for (size_t t=0;t<nt;t++){
        uint32_t ia=I[t*3], ib=I[t*3+1], ic=I[t*3+2];
        if ((size_t)ia*3+2>=V.size()||(size_t)ib*3+2>=V.size()||(size_t)ic*3+2>=V.size()) continue;
        const float* a=&V[ia*3]; const float* b=&V[ib*3]; const float* c=&V[ic*3];
        float e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
        float N[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
        float nl=std::sqrt(N[0]*N[0]+N[1]*N[1]+N[2]*N[2]); if(nl<1e-7f) continue;
        N[0]/=nl;N[1]/=nl;N[2]/=nl; if(N[1]<0){N[0]=-N[0];N[1]=-N[1];N[2]=-N[2];}   // box +Y (top) = surface normal, up
        if (smooth) {   // replace the flat triangle tilt with the SMOOTHED (vertex-averaged) normal -> adjacent boxes blend
            float sn[3]={ vn[(size_t)ia*3]+vn[(size_t)ib*3]+vn[(size_t)ic*3], vn[(size_t)ia*3+1]+vn[(size_t)ib*3+1]+vn[(size_t)ic*3+1], vn[(size_t)ia*3+2]+vn[(size_t)ib*3+2]+vn[(size_t)ic*3+2] };
            float sl=std::sqrt(sn[0]*sn[0]+sn[1]*sn[1]+sn[2]*sn[2]); if(sl>1e-6f){ N[0]=sn[0]/sl;N[1]=sn[1]/sl;N[2]=sn[2]/sl; if(N[1]<0){N[0]=-N[0];N[1]=-N[1];N[2]=-N[2];} }
        }
        float X[3]={e1[0],e1[1],e1[2]}; float xn=dot(X,N); X[0]-=xn*N[0];X[1]-=xn*N[1];X[2]-=xn*N[2];   // X = edge1 ⊥ N
        float xl=std::sqrt(dot(X,X)); if(xl<1e-6f) continue; X[0]/=xl;X[1]/=xl;X[2]/=xl;
        float Z[3]={N[1]*X[2]-N[2]*X[1], N[2]*X[0]-N[0]*X[2], N[0]*X[1]-N[1]*X[0]};   // Z = N × X
        // rotation matrix columns = [X, N, Z]; convert to quaternion {x,y,z,w}
        float m00=X[0],m10=X[1],m20=X[2], m01=N[0],m11=N[1],m21=N[2], m02=Z[0],m12=Z[1],m22=Z[2];
        float tr=m00+m11+m22, qx,qy,qz,qw;
        if(tr>0){float s=std::sqrt(tr+1.f)*2.f; qw=0.25f*s; qx=(m21-m12)/s; qy=(m02-m20)/s; qz=(m10-m01)/s;}
        else if(m00>m11&&m00>m22){float s=std::sqrt(1.f+m00-m11-m22)*2.f; qw=(m21-m12)/s; qx=0.25f*s; qy=(m01+m10)/s; qz=(m02+m20)/s;}
        else if(m11>m22){float s=std::sqrt(1.f+m11-m00-m22)*2.f; qw=(m02-m20)/s; qx=(m01+m10)/s; qy=0.25f*s; qz=(m12+m21)/s;}
        else{float s=std::sqrt(1.f+m22-m00-m11)*2.f; qw=(m10-m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s; qz=0.25f*s;}
        float ctr[3]={(a[0]+b[0]+c[0])/3.f,(a[1]+b[1]+c[1])/3.f,(a[2]+b[2]+c[2])/3.f};
        float hx=0,hz=0; for(const float* p:{a,b,c}){ float d[3]={p[0]-ctr[0],p[1]-ctr[1],p[2]-ctr[2]}; hx=std::max(hx,std::fabs(dot(d,X))); hz=std::max(hz,std::fabs(dot(d,Z))); }
        hx+=margin; hz+=margin;
        float drop=thick-inflate;   // top face = tri + inflate*N (peeks slightly ABOVE the visual surface)
        float cen[3]={ctr[0]-drop*N[0], ctr[1]-drop*N[1], ctr[2]-drop*N[2]};
        boxes.push_back({cen[0],cen[1],cen[2], hx,thick,hz, qx,qy,qz,qw});
    }
    return boxes;
}
// TILTED-GRID variant for DENSE walkable meshes (too many tris for 1:1 boxes): coarse grid, but each cell's box is
// TILTED to the AREA-WEIGHTED AVERAGE of the actual triangle normals in it + sat at their mean height -> follows the
// road's real slope/bank with a BOUNDED box count. Reuses the triangle normals/heights (not a flat heightfield).
inline std::vector<std::array<float,10>> navmeshTiltedGrid(const std::vector<float>& V, const std::vector<uint32_t>& I, float cell, float thick, float margin, int maxBoxes) {
    std::vector<std::array<float,10>> boxes;
    if (V.size()<9 || I.size()<3) return boxes;
    float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
    for (size_t i=0;i+2<V.size();i+=3) for(int k=0;k<3;k++){ mn[k]=std::min(mn[k],V[i+k]); mx[k]=std::max(mx[k],V[i+k]); }
    float extx=mx[0]-mn[0], extz=mx[2]-mn[2];
    auto dot=[](const float*a,const float*b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    for (int attempt=0; attempt<14; ++attempt){
        boxes.clear();
        int NX=(int)(extx/cell)+1, NZ=(int)(extz/cell)+1; if(NX<1)NX=1; if(NZ<1)NZ=1;
        if ((long long)NX*NZ > 900000){ cell*=1.35f; continue; }
        size_t NC=(size_t)NX*NZ;
        std::vector<float> aN(NC*3,0.f), aHs(NC,0.f), aW(NC,0.f);
        size_t nt=I.size()/3;
        for (size_t t=0;t<nt;t++){
            uint32_t ia=I[t*3],ib=I[t*3+1],ic=I[t*3+2];
            if((size_t)ia*3+2>=V.size()||(size_t)ib*3+2>=V.size()||(size_t)ic*3+2>=V.size()) continue;
            const float*a=&V[ia*3];const float*b=&V[ib*3];const float*c=&V[ic*3];
            float e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
            float N[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};   // length = 2*area
            float a2=std::sqrt(dot(N,N)); if(a2<1e-9f) continue; if(N[1]<0){N[0]=-N[0];N[1]=-N[1];N[2]=-N[2];}
            float cx=(a[0]+b[0]+c[0])/3.f, cy=(a[1]+b[1]+c[1])/3.f, cz=(a[2]+b[2]+c[2])/3.f;
            int gx=(int)((cx-mn[0])/cell), gz=(int)((cz-mn[2])/cell);
            if(gx<0)gx=0;if(gx>=NX)gx=NX-1;if(gz<0)gz=0;if(gz>=NZ)gz=NZ-1;
            size_t ci=(size_t)gz*NX+gx;
            aN[ci*3]+=N[0]; aN[ci*3+1]+=N[1]; aN[ci*3+2]+=N[2]; aHs[ci]+=cy*a2; aW[ci]+=a2;
        }
        float hm = cell*0.5f + margin;
        for (size_t ci=0; ci<NC; ci++){
            if(aW[ci]<=0.f) continue;
            int gx=(int)(ci%NX), gz=(int)(ci/NX);
            float N[3]={aN[ci*3],aN[ci*3+1],aN[ci*3+2]}; float nl=std::sqrt(dot(N,N));
            if(nl<1e-6f){N[0]=0;N[1]=1;N[2]=0;} else {N[0]/=nl;N[1]/=nl;N[2]/=nl;}
            float surfY=aHs[ci]/aW[ci];
            float X[3]={1,0,0}; float xn=dot(X,N); X[0]-=xn*N[0];X[1]-=xn*N[1];X[2]-=xn*N[2];
            float xl=std::sqrt(dot(X,X)); if(xl<1e-5f){X[0]=0;X[1]=0;X[2]=1; xn=dot(X,N);X[0]-=xn*N[0];X[1]-=xn*N[1];X[2]-=xn*N[2];xl=std::sqrt(dot(X,X));}
            X[0]/=xl;X[1]/=xl;X[2]/=xl;
            float Z[3]={N[1]*X[2]-N[2]*X[1], N[2]*X[0]-N[0]*X[2], N[0]*X[1]-N[1]*X[0]};
            float m00=X[0],m10=X[1],m20=X[2], m01=N[0],m11=N[1],m21=N[2], m02=Z[0],m12=Z[1],m22=Z[2];
            float tr=m00+m11+m22,qx,qy,qz,qw;
            if(tr>0){float s=std::sqrt(tr+1.f)*2.f;qw=0.25f*s;qx=(m21-m12)/s;qy=(m02-m20)/s;qz=(m10-m01)/s;}
            else if(m00>m11&&m00>m22){float s=std::sqrt(1.f+m00-m11-m22)*2.f;qw=(m21-m12)/s;qx=0.25f*s;qy=(m01+m10)/s;qz=(m02+m20)/s;}
            else if(m11>m22){float s=std::sqrt(1.f+m11-m00-m22)*2.f;qw=(m02-m20)/s;qx=(m01+m10)/s;qy=0.25f*s;qz=(m12+m21)/s;}
            else{float s=std::sqrt(1.f+m22-m00-m11)*2.f;qw=(m10-m01)/s;qx=(m02+m20)/s;qy=(m12+m21)/s;qz=0.25f*s;}
            float ccx=mn[0]+(gx+0.5f)*cell, ccz=mn[2]+(gz+0.5f)*cell;
            float cen[3]={ccx - thick*N[0], surfY - thick*N[1], ccz - thick*N[2]};
            boxes.push_back({cen[0],cen[1],cen[2], hm,thick,hm, qx,qy,qz,qw});
        }
        if((int)boxes.size()<=maxBoxes) break;
        cell*=1.35f;
    }
    return boxes;
}
inline std::string templateJson(const std::string& entities, const std::string& relationships = std::string()) {
    return std::string("{\"version\":5,\"entities\":[") + entities + "],\"relationships\":[" + relationships + "]}";
}
// space.hstf = the firstWorldAsset. One entity whose `type` is the content-template AssetRef, PLUS the Scene entity.
// ⛔ GROUND TRUTH (every official vista: com_meta_shell_env_vista_* → content/meta/<env>/space.hstf): the
// ScenePlatformComponent (the far-clip-plane config that drives the main-view projection) lives HERE in space.hstf,
// as a sibling of the content-ref "Root" entity — NOT in content.hstf. Cooking it into content.hstf left it never
// applied (far stayed at the shell default → distant geometry far-clipped = the "black dome"). farP<=0 disables.
inline std::string spaceJson(CookRng& r, const std::string& name, const AssetKey3& contentRef, float farP) {
    std::string rootId = makeUuid(r);
    std::string contentEnt = std::string("{\"id\":\"") + rootId + "\",\"name\":\"" + name + "\",\"type\":" + refJson(contentRef) + ",\"deltas\":[],\"attributes\":[]}";
    std::string ent, rels;
    if (farP > 0.f) {
        // ⛔ DEVICE-VERIFIED STRUCTURE (official calming space.hstf, 2026-06-18) — the head-locked far-clip-plane fix.
        // The device reads the world's clip planes (ScenePlatformComponent) from **entity[0]** — the FIRST-LISTED
        // entity, which is ALSO the parentless root. Calming: [0]="Calming" {FogTravel,ScenePlatform} (root) ; [1]=
        // content type-ref, ChildOf Calming. Merely making Scene the parentless root was NOT enough — it must be the
        // FIRST entity in the array. The old cook listed the content-ref first → device saw no ScenePlatformComponent
        // on entity[0] → fell back to the shell's small DEFAULT far → a view-space (head-locked) far plane clipped all
        // distant geometry to black. So: emit the Scene entity FIRST, then the content-ref as its child.
        std::string sceneId = makeUuid(r);
        ent  = sceneEntityJson(sceneId, 0.1f, farP) + "," + contentEnt;   // Scene = entity[0] = root (its ScenePlatformComponent IS applied)
        rels = relChildOf(rootId, sceneId);                               // content-ref ChildOf Scene
    } else {
        ent = contentEnt;
    }
    return templateJson(ent, rels);
}
inline std::string shellConfigJson(const AssetKey3& spaceRef, bool locomotion) {
    return std::string("{\"firstWorldAssetId\":") + refJson(spaceRef) + ",\"supportsLocomotion\":" + (locomotion?"true":"false") + "}";
}

// ── Binary AndroidManifest (AXML) package rename — port of axml.py: rebuild the string pool with `old`→`new`
//    in every pooled string (node chunks reference strings by INDEX, so they stay valid). ───────────────────
inline std::vector<uint8_t> patchAxml(const std::vector<uint8_t>& axml, const std::string& oldS, const std::string& newS, bool exact=false) {
    auto u16=[&](size_t o){ uint16_t v; memcpy(&v,&axml[o],2); return v; };
    auto u32=[&](size_t o){ uint32_t v; memcpy(&v,&axml[o],4); return v; };
    if (axml.size()<8 || u16(0)!=0x0003) return axml;
    size_t sp=8; if (u16(sp)!=0x0001) return axml;
    uint16_t sp_hdr=u16(sp+2); uint32_t sp_size=u32(sp+4);
    uint32_t strCount=u32(sp+8), styleCount=u32(sp+12), flags=u32(sp+16), stringsStart=u32(sp+20);
    bool utf8=(flags&0x100)!=0;
    size_t off0=sp+sp_hdr, sbase=sp+stringsStart;
    std::vector<std::string> strings(strCount);
    for (uint32_t i=0;i<strCount;i++){ size_t p=sbase+u32(off0+i*4);
        if (utf8){ auto dl=[&](size_t q,size_t&nq){ uint32_t n=axml[q]; if(n&0x80){ n=((n&0x7f)<<8)|axml[q+1]; nq=q+2;} else nq=q+1; return n; };
            size_t q; dl(p,q); uint32_t bl=dl(q,q); strings[i].assign((const char*)&axml[q],bl);
        } else { uint32_t l=u16(p); size_t q=p+2; if(l&0x8000){ l=((l&0x7fff)<<16)|u16(q); q+=2; }
            std::string s; s.reserve(l); for(uint32_t c=0;c<l;c++){ uint16_t w=u16(q+c*2); s+=(char)(w&0xff); } strings[i]=s; } }
    bool any=false; for (auto&s:strings) if (exact ? (s==oldS) : (s.find(oldS)!=std::string::npos)){ any=true; break; }
    if (!any) return axml;
    // exact: replace ONLY whole-string matches (used to flip the hsr_package_type VALUE "footprint"->"combined" without
    // touching the package name "com.meta.shell.env.footprint.haven2025" or build_rule "footprint-haven2025...").
    // non-exact (substring): advance past each replacement — otherwise if newS contains oldS (e.g. the haven->haven
    // spoof, oldS==newS) this would find the same spot forever and HANG the cook.
    auto repl=[&](std::string s){ if(oldS.empty()||oldS==newS) return s; if(exact) return s==oldS ? newS : s;
        size_t p=0; while((p=s.find(oldS,p))!=std::string::npos){ s.replace(p,oldS.size(),newS); p+=newS.size(); } return s; };
    std::vector<uint8_t> data; std::vector<uint32_t> noff;
    auto enc=[&](uint32_t n,std::vector<uint8_t>&o){ if(n<0x80)o.push_back((uint8_t)n); else { o.push_back(0x80|(n>>8)); o.push_back(n&0xff);} };
    for (auto&s0:strings){ std::string s=repl(s0); noff.push_back((uint32_t)data.size());
        if (utf8){ enc((uint32_t)s.size(),data); enc((uint32_t)s.size(),data); data.insert(data.end(),s.begin(),s.end()); data.push_back(0); }
        else { uint16_t l=(uint16_t)s.size(); data.push_back(l&0xff); data.push_back(l>>8); for(char c:s){ data.push_back((uint8_t)c); data.push_back(0);} data.push_back(0); data.push_back(0); } }
    while (data.size()%4) data.push_back(0);
    std::vector<uint8_t> off_arr; for (uint32_t o:noff){ for(int k=0;k<4;k++) off_arr.push_back((o>>(8*k))&0xff); }
    std::vector<uint8_t> style_arr(axml.begin()+off0+strCount*4, axml.begin()+sbase);  // style offsets + pad (usually empty)
    uint32_t new_stringsStart=sp_hdr+(uint32_t)off_arr.size()+(uint32_t)style_arr.size();
    std::vector<uint8_t> body;
    auto p32=[&](uint32_t v){ for(int k=0;k<4;k++) body.push_back((v>>(8*k))&0xff); };
    p32(strCount); p32(styleCount); p32(flags); p32(new_stringsStart); p32(0);
    body.insert(body.end(),off_arr.begin(),off_arr.end());
    body.insert(body.end(),style_arr.begin(),style_arr.end());
    body.insert(body.end(),data.begin(),data.end());
    uint32_t new_sp_size=8+(uint32_t)body.size();
    std::vector<uint8_t> out(axml.begin(),axml.begin()+8);
    out.push_back(0x01); out.push_back(0x00); out.push_back(sp_hdr&0xff); out.push_back(sp_hdr>>8);
    for(int k=0;k<4;k++) out.push_back((new_sp_size>>(8*k))&0xff);
    out.insert(out.end(),body.begin(),body.end());
    out.insert(out.end(),axml.begin()+sp+sp_size,axml.end());
    uint32_t total=(uint32_t)out.size(); memcpy(&out[4],&total,4);
    return out;
}

// Read the <manifest package="..."> attribute out of a binary AndroidManifest (AXML) — so the cook can use the
// LOADED env's OWN package instead of a hardcoded one. Returns "" if not found.
inline std::string readAxmlPackage(const std::vector<uint8_t>& axml) {
    auto u16=[&](size_t o){ uint16_t v; memcpy(&v,&axml[o],2); return v; };
    auto u32=[&](size_t o){ uint32_t v; memcpy(&v,&axml[o],4); return v; };
    if (axml.size()<8 || u16(0)!=0x0003) return "";
    size_t sp=8; if (u16(sp)!=0x0001) return "";
    uint16_t sp_hdr=u16(sp+2); uint32_t sp_size=u32(sp+4);
    uint32_t strCount=u32(sp+8), flags=u32(sp+16), stringsStart=u32(sp+20);
    bool utf8=(flags&0x100)!=0; size_t off0=sp+sp_hdr, sbase=sp+stringsStart;
    auto getStr=[&](uint32_t idx)->std::string{ if(idx>=strCount) return ""; size_t p=sbase+u32(off0+idx*4);
        if (utf8){ auto dl=[&](size_t q,size_t&nq){ uint32_t n=axml[q]; if(n&0x80){ n=((n&0x7f)<<8)|axml[q+1]; nq=q+2;} else nq=q+1; return n; };
            size_t q; dl(p,q); uint32_t bl=dl(q,q); return std::string((const char*)&axml[q],bl); }
        else { uint32_t l=u16(p); size_t q=p+2; if(l&0x8000){ l=((l&0x7fff)<<16)|u16(q); q+=2; } std::string s; for(uint32_t c=0;c<l;c++) s+=(char)(u16(q+c*2)&0xff); return s; } };
    size_t o = sp + sp_size;                                       // walk the XML chunks after the string pool
    while (o + 36 <= axml.size()) {
        uint16_t type=u16(o); uint32_t csize=u32(o+4); if (csize<8 || o+csize>axml.size()) break;
        if (type==0x0102 && getStr(u32(o+20))=="manifest") {       // START_ELEMENT named "manifest"
            size_t ab = o + 16 + u16(o+24); uint16_t attrCount=u16(o+28);
            for (uint16_t a=0; a<attrCount && ab+20<=axml.size(); a++, ab+=20)
                if (getStr(u32(ab+4))=="package") { uint32_t raw=u32(ab+8); return raw!=0xffffffffu ? getStr(raw) : getStr(u32(ab+16)); }
        }
        o += csize;
    }
    return "";
}

// ── ZIP/APK assembly (miniz) ──────────────────────────────────────────────────────────────────────────
typedef std::pair<std::string,std::vector<uint8_t>> CookFile;
inline std::vector<uint8_t> buildZip(const std::vector<CookFile>& files, bool compress = true) {
    mz_zip_archive zip; memset(&zip,0,sizeof zip); mz_zip_writer_init_heap(&zip,0,0);
    for (auto& f : files)
        mz_zip_writer_add_mem(&zip, f.first.c_str(), f.second.data(), f.second.size(), compress?MZ_DEFAULT_COMPRESSION:MZ_NO_COMPRESSION);
    void* p=nullptr; size_t n=0; mz_zip_writer_finalize_heap_archive(&zip,&p,&n);
    std::vector<uint8_t> out((uint8_t*)p,(uint8_t*)p+n); mz_free(p); mz_zip_writer_end(&zip); return out;
}

// ⛔ scene.zip MUST be written this way (NOT miniz): all entries STORED with the CRC + sizes in the LOCAL file
//    header and general-purpose flag = 0. miniz writes STORED entries with the DATA-DESCRIPTOR bit (flag 0x0808)
//    and csz/usz/crc = 0 in the local header (real sizes trail the data). The on-device LooseFileAssetLoader reads
//    the LOCAL header directly, sees size 0, and fails with "Allocation failed for asset ..." → the env is rejected
//    and the shell falls back to nuxd. nuxd's scene.zip uses flag=0 + sizes-in-local-header. Verified on-device.
inline void zput16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); }
inline void zput32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8*i)) & 0xff); }
inline std::vector<uint8_t> buildStoredZip(const std::vector<CookFile>& files) {
    std::vector<uint8_t> out;
    struct CDirEnt { std::string name; uint32_t crc, size, off; };
    std::vector<CDirEnt> cd;
    const uint16_t DOS_TIME = 0, DOS_DATE = 0x0021;   // 1985-02-01 (cosmetic; loader ignores it)
    for (const auto& f : files) {
        uint32_t crc  = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)f.second.data(), f.second.size());
        uint32_t size = (uint32_t)f.second.size();
        uint32_t off  = (uint32_t)out.size();
        zput32(out, 0x04034b50);            // local file header signature
        zput16(out, 20);                    // version needed (2.0)
        zput16(out, 0);                     // general purpose flag = 0  (NO data descriptor, NO UTF-8 bit)
        zput16(out, 0);                     // method = 0 (STORED)
        zput16(out, DOS_TIME); zput16(out, DOS_DATE);
        zput32(out, crc); zput32(out, size); zput32(out, size);   // crc, csz, usz — ALL in the local header
        zput16(out, (uint16_t)f.first.size()); zput16(out, 0);    // name len, extra len
        out.insert(out.end(), f.first.begin(), f.first.end());
        out.insert(out.end(), f.second.begin(), f.second.end());
        cd.push_back({ f.first, crc, size, off });
    }
    uint32_t cdStart = (uint32_t)out.size();
    for (const auto& c : cd) {
        zput32(out, 0x02014b50);            // central directory header signature
        zput16(out, 20); zput16(out, 20);   // version made by, version needed
        zput16(out, 0); zput16(out, 0);     // flag = 0, method = 0 (STORED)
        zput16(out, DOS_TIME); zput16(out, DOS_DATE);
        zput32(out, c.crc); zput32(out, c.size); zput32(out, c.size);
        zput16(out, (uint16_t)c.name.size()); zput16(out, 0); zput16(out, 0);  // name, extra, comment len
        zput16(out, 0); zput16(out, 0); zput32(out, 0);                        // disk start, int attr, ext attr
        zput32(out, c.off);                 // local header offset
        out.insert(out.end(), c.name.begin(), c.name.end());
    }
    uint32_t cdSize = (uint32_t)out.size() - cdStart;
    zput32(out, 0x06054b50);                // end of central directory signature
    zput16(out, 0); zput16(out, 0);         // disk number, cd start disk
    zput16(out, (uint16_t)cd.size()); zput16(out, (uint16_t)cd.size());
    zput32(out, cdSize); zput32(out, cdStart);
    zput16(out, 0);                         // comment length
    return out;
}

// Splice a freshly-cooked scene.zip into the V203 shell. STANDALONE: the donor files (classes.dex / resources.arsc /
// kotlin builtins / res / assets) are EMBEDDED (no external Nuxd.apk); the AndroidManifest is hardcoded (haven), the
// scene.zip is the env's. Returns the unsigned APK bytes. `baseApk` is ignored (kept for signature compatibility).
inline std::vector<uint8_t> spliceAPK(const std::string& baseApk, const std::vector<uint8_t>& sceneZip,
                                      const std::string& oldPkg, const std::string& newPkg, bool* ok = nullptr,
                                      const std::string& editorSession = {}) {
    (void)baseApk;
    if (ok) *ok = false;
    mz_zip_archive out; memset(&out,0,sizeof out); mz_zip_writer_init_heap(&out,0,0);
    auto donor = embassets::nuxdDonor();   // every kept Nuxd file (scene.zip + AndroidManifest are added below, not here)
    if (donor.empty()) { mz_zip_writer_end(&out); return {}; }
    auto addFile = [&](const std::string& name, std::vector<uint8_t> data){
        mz_uint fl = (name=="resources.arsc")?MZ_NO_COMPRESSION:MZ_DEFAULT_COMPRESSION;
        mz_zip_writer_add_mem(&out,name.c_str(),data.data(),data.size(),fl);
    };
    for (auto& kv : donor) addFile(kv.first, std::move(kv.second));   // donor excludes scene.zip / manifest / META-INF
    addFile("assets/scene.zip", sceneZip);
    if (!editorSession.empty())   // self-contained round-trip: the editor reads this back when a cooked APK has no source .hsledit
        addFile("assets/_editor_session.hsledit", std::vector<uint8_t>(editorSession.begin(), editorSession.end()));
    {   // AndroidManifest = hardcoded haven, package renamed to the target (LATEST manifest the engine tolerates).
        std::string name="AndroidManifest.xml"; std::vector<uint8_t> data;
        {
            std::vector<uint8_t> hm(HAVEN_MANIFEST_AXML, HAVEN_MANIFEST_AXML + HAVEN_MANIFEST_AXML_LEN);
            data = patchAxml(hm, "com.meta.shell.env.footprint.haven2025", newPkg);
            // ⛔ CRITICAL (device-proven, logcat 2026-06-10): haven2025's manifest declares
            //    com.meta.shell.env.hsr_package_type="footprint". The shell then loads the env as a FOOTPRINT OVERLAY —
            //    it pulls a companion vista (vista.focused) + the base home_c25 template layers
            //    (home_w_focused_lm.hstf -> home_audio.hstf) that a STANDALONE cook doesn't contain ->
            //    "Asset data load failed" -> "Scene loaded: <none>" -> fallback to nuxd (the bug we hit on erebor).
            //    Every working cook (aurora/OW) ships "combined" (envIsFootprint=0) and loads as a standalone Environment.
            //    Flip ONLY that exact value string; exact=true leaves the package name + build_rule "footprint-haven2025..."
            //    untouched, so the haven SPOOF package name stays valid.
            data = patchAxml(data, "footprint", "combined", /*exact=*/true);
        }
        addFile(name, std::move(data));
    }
    void* op=nullptr; size_t on=0; mz_zip_writer_finalize_heap_archive(&out,&op,&on);
    std::vector<uint8_t> apk((uint8_t*)op,(uint8_t*)op+on); mz_free(op); mz_zip_writer_end(&out);
    if (ok) *ok=true; return apk;
}

// ── High-level "Export APK" — the editor calls this. Pass the cooked scene assets; get an unsigned APK. ──
struct CookAsset { std::string contentPath; uint32_t tgt; std::vector<uint8_t> data; AssetKey3 key; uint32_t cat = 0, sub = 0; };
static const uint32_t TYPE_PHSX = 0x58534850u, TYPE_3MSH = 0x48534D33u;   // physics collision mesh (PHSX:3MSH)
static const uint32_t TYPE_HZAN = 0x4E415A48u, TYPE_SKEL = 0x4C454B53u, TYPE_ANIM = 0x4D494E41u;   // HZAN:SKEL / HZAN:ANIM
inline std::vector<uint8_t> assembleSceneZip(std::vector<CookAsset>& assets,
                                             const std::vector<uint8_t>& shellConfig) {
    std::vector<CookFile> files;
    std::vector<AsmhEntry> man;
    for (auto& a : assets) {
        files.push_back({ "content/" + a.contentPath, a.data });
        man.push_back({ a.key.pkg, a.key.ing, a.key.tgt, a.contentPath, a.cat ? a.cat : assetFourcc(a.data), a.sub ? a.sub : assetSubtype(a.data), (uint32_t)a.data.size() });
    }
    files.push_back({ "content/assets.manifest", buildAsmh(man) });
    files.push_back({ "content/configs/shellconfig.jsonc", shellConfig });
    // scene.zip MUST use the hand-rolled STORED writer (sizes/CRC in the local header, flag=0) — see buildStoredZip.
    return buildStoredZip(files);
}

// ── multi-part RENDMESH: split arbitrary (u32-indexed) geometry into u16 parts of <=60000 unique verts, each with
//    its own VB + LOCAL u16 IB. parseRendMesh concatenates parts' VBs and offsets each part's indices by the
//    running vertex base, so per-part local 0-based indices are correct. Handles meshes of any size. ──────────
inline std::vector<uint8_t> encodeRendMeshParts(const std::vector<float>& pos_, const std::vector<float>& uv_,
                                                const std::vector<uint32_t>& idx_, const std::vector<uint8_t>& embeddedMatl = {},
                                                int vatVertexCount = 0,
                                                const std::vector<uint8_t>& boneIdx_ = {}, const std::vector<uint8_t>& boneWgt_ = {},
                                                const std::vector<uint32_t>& jointIds = {},   // jointIds = murmur3(joint name) per skeleton joint
                                                bool spinBoundsY = false,    // Y-ROTATION mesh: bake the AABB as the swept cylinder so the device doesn't FRUSTUM-CULL the getTime()-rotated geometry (it can't see the shader's vertex spin)
                                                const std::vector<uint8_t>& vertCol = {}) {   // per-ORIGINAL-vertex sem4 COLOR_0 RGBA (baked lightmap → shader does base×COLOR0); empty → white (neutral)
    struct Part { std::vector<uint8_t> vb, ib; uint32_t nv = 0; };
    std::vector<Part> parts;
    // BOUNDS VERTEX (skinned-mesh CULL fix, V205): the device culls SKINNED meshes by their runtime SKINNED-VERTEX extent,
    // NOT the cooked MeshDefinition AABB (DEVICE-PROVEN: scene-spanning lod.f3 AND a far skeleton anchor joint BOTH did
    // nothing — the omnidroid still vanished on a small rotate). FIX = append ONE far vertex (rest pos ~1e5, weighted 100% to
    // joint 0) as a DEGENERATE (zero-area, all-indices-equal → invisible) triangle: it skins to ~1e5 every frame → the runtime
    // skinned bounds span the scene → the frustum/occlusion cull can never drop the mesh. Joint 0 is already in the palette, so
    // the dense palette / maxBoneIdx / verifier are unaffected. HSR_NOBOUNDSVERT disables. (The skin-vertex analogue of HSR_NOCULL.)
    bool skinned_ = boneIdx_.size() >= (pos_.size()/3)*4 && boneWgt_.size() >= (pos_.size()/3)*4;
    // SKINNED CULL FIX (default ON): weight the ±1e5 bounds verts to the dedicated STATIC ANCHOR joint = the LAST
    // skeleton joint (the cook appends it: identity bind + identity-translation clip track, unit scale). The device's
    // skinned cull bound = union of each PALETTE joint's influenced-vertex AABB × posed-joint-matrix, but it SKIPS
    // JOINT 0 and a SCALING joint collapses far verts to ~0 — so the old "weight to joint 0 / to the highest used
    // joint" both failed. The anchor is index>=1 (counted), unit-scale (never collapses), so the ±1e5 verts stay
    // ±1e5 each frame -> the skinned bound spans the scene -> the mesh is never frustum/occlusion-culled (the skin-
    // path analogue of the static-mesh ±1e5 nocullBounds). HSR_NOBOUNDSVERT disables. Anchor index = last jointId.
    int anchorBase = (int)jointIds.size() - 6;   // the 6 bounds-anchor joints occupy the LAST 6 palette slots (see jointIds build)
    bool bvert = skinned_ && !pos_.empty() && anchorBase >= 1 && !std::getenv("HSR_NOBOUNDSVERT");
    std::vector<float> posA, uvA; std::vector<uint32_t> idxA; std::vector<uint8_t> biA, bwA;
    if (bvert) {
        posA = pos_; uvA = uv_; idxA = idx_; biA = boneIdx_; bwA = boneWgt_;
        if (uvA.size() < (posA.size()/3)*2) uvA.resize((posA.size()/3)*2, 0.f);   // UV parity before the append
        uint32_t base = (uint32_t)(posA.size()/3);
        // ANCHOR JOINT (IDA-PROVEN root cause, SkeletonSystem_vf7__1480F70): the device computes a SKINNED mesh's CULL
        // bounds from the SKELETON's per-joint boxes (each joint's bind-pose influenced-vert AABB) transformed by the posed
        // joint matrices — and its accumulation loop SKIPS JOINT 0 (iterates joints 1..n-1 starting at jointArray+120; the
        // init fn__AF08EC only zeroes the accumulator, it does NOT add joint 0). So the old bounds vert weighted 100% to
        // joint 0 was SILENTLY IGNORED (omnidroid still culled). FIX = weight it to the HIGHEST joint this mesh actually
        // uses (>=1, already in the dense palette, and iterated by the loop) so THAT joint's box spans the scene -> the
        // skeleton bounds span the scene -> the frustum/occlusion cull can never drop the skinned mesh.
        uint8_t anchorJoint = 0;
        for (size_t i = 0; i < boneIdx_.size() && i < boneWgt_.size(); i++)
            if (boneWgt_[i] > 0 && boneIdx_[i] > anchorJoint) anchorJoint = boneIdx_[i];
        if (anchorJoint == 0) anchorJoint = 1;   // mesh uses only joint 0 (which the loop skips): reference joint 1 (the bounds vert's weight=255 makes it a legit palette entry)
        // TWO tiny NON-DEGENERATE far triangles at OPPOSITE corners (±1e5) — the SKINNED-BOUNDS FIX. Root cause chain:
        //  (1) the device recomputes a skinned mesh's CULL bounds from its geometry (ignores the cooked AABB);
        //  (2) it SKIPS degenerate/zero-area triangles, so our old single far vert in a (base,base,base) tri was NEVER
        //      counted → every prior variant failed identically;
        //  (3) a single NON-degenerate far triangle at +X DID get counted (DEVICE-PROVEN) but gave an ASYMMETRIC box
        //      (omnidroid..+1e5) → the omnidroid still culled when looking AWAY from +X near spawn, but NOT from elsewhere.
        // FIX = two real triangles at OPPOSITE corners (-1e5,-1e5,-1e5) and (+1e5,+1e5,+1e5) → the recomputed AABB spans the
        // scene SYMMETRICALLY in every direction → never culled from any viewpoint. Each tri is sub-pixel AND past the
        // 150000 far-clip (corner dist ~1.73e5) → invisible. All weighted to a live joint so they skin to ~±1e5 each frame.
        // ON-AXIS at 1e5 (NOT diagonal corners): the device EXCLUDES verts past the far-clip (150000) from the recomputed
        // bounds — the diagonal corners (±1e5,±1e5,±1e5) at dist ~1.73e5 were clipped out (symmetric-corner attempt reverted
        // to culling), while a +X vert at dist 1e5 WAS counted. Six axis extremes at dist 1e5 (< far-clip from anywhere in
        // the ±200 scene) → AABB spans ±1e5 in EVERY direction, all counted → never culled. Sub-pixel at 1e5 → invisible.
        const float C = 1.0e5f;
        const float ext[6][3] = { { C,0,0 }, { -C,0,0 }, { 0,C,0 }, { 0,-C,0 }, { 0,0,C }, { 0,0,-C } };
        for (int e = 0; e < 6; e++) {
            for (int t = 0; t < 3; t++) {
                posA.push_back(ext[e][0] + (t == 1 ? 1.0f : 0.0f));
                posA.push_back(ext[e][1] + (t == 2 ? 1.0f : 0.0f));
                posA.push_back(ext[e][2]);
                uvA.push_back(0.f); uvA.push_back(0.f);
                biA.push_back((uint8_t)(anchorBase + e)); biA.push_back(0); biA.push_back(0); biA.push_back(0);   // weight this triangle to the e-th bounds-anchor joint (at the ±C axis extreme) so THAT anchor is counted in the device's per-joint position bound -> the bound spans ±C symmetrically
                bwA.push_back(255); bwA.push_back(0); bwA.push_back(0); bwA.push_back(0);
            }
            uint32_t b = base + (uint32_t)e * 3;
            idxA.push_back(b); idxA.push_back(b + 1); idxA.push_back(b + 2);   // NON-degenerate + within far-clip → counted; sub-pixel → invisible
        }
    }
    const std::vector<float>&    pos     = bvert ? posA : pos_;
    const std::vector<float>&    uv      = bvert ? uvA  : uv_;
    const std::vector<uint32_t>& idx     = bvert ? idxA : idx_;
    const std::vector<uint8_t>&  boneIdx = bvert ? biA  : boneIdx_;
    const std::vector<uint8_t>&  boneWgt = bvert ? bwA  : boneWgt_;
    bool haveUv = uv.size() >= (pos.size() / 3) * 2;
    size_t nvTotal = pos.size() / 3;
    bool skinned = boneIdx.size() >= nvTotal * 4 && boneWgt.size() >= nvTotal * 4;   // stride-24 SKINNED mesh (sem7 idx, sem8 wgt)
    // DENSE JOINT PALETTE: ROOT.f2 must list ONLY the skeleton joints this mesh's vertices actually reference, and each
    // vertex bone index is remapped into that palette (local slot). The device's skin build REJECTS a palette that
    // contains joints no vertex uses (MeshDefinition::fix() "flatbuffer verification failed" -> std::length_error abort).
    // A 0..maxBoneIdx palette only works when the references are contiguous (m001 droid body uses {0,1,2,3}); the omnidroid
    // SHIELD weights only joints {0,4}, so a 0..4 palette would include unused 1,2,3 and crash the device. This is the
    // standard glTF skin-palette remap. [[project_hsr_skinned_rendmesh_skinblock]]
    std::vector<uint8_t> palette;       // local slot -> skeleton joint index (sorted, unique, only referenced)
    uint8_t boneRemap[256]; memset(boneRemap, 0, sizeof boneRemap);
    if (skinned) {
        bool seen[256] = { false };
        for (size_t i = 0; i < nvTotal * 4 && i < boneIdx.size(); i++) seen[boneIdx[i]] = true;
        for (int j = 0; j < 256; j++) if (seen[j]) { boneRemap[j] = (uint8_t)palette.size(); palette.push_back((uint8_t)j); }
    }
    // AABB from the REAL geometry (pos_, the ORIGINAL input) — NOT `pos`, which for a skinned mesh has the ±1e5
    // bounds-anchor triangles appended. Including those blew the skinned mesh's RENDMESH AABB up to ±1e5 (line 1173:
    // skinned meshes must keep their tiny model-local extent like the butterfly, ~0.1, NOT ±1e5). A ±1e5 AABB on a
    // 0.34u comet let the device size/precision-scale it down to an invisible speck (the "comet shrunk by the culling
    // patch" bug). The anchor VERTS still ship in the VB (so the skeleton joint boxes span the scene = nocull); only
    // the AABB metadata now excludes them.
    const std::vector<float>& aabbSrc = pos_;
    float mn[3] = { 1e30f,1e30f,1e30f }, mx[3] = { -1e30f,-1e30f,-1e30f };
    for (size_t i = 0; i + 2 < aabbSrc.size(); i += 3) for (int k = 0; k < 3; k++) { float p = aabbSrc[i + k]; if (p < mn[k]) mn[k] = p; if (p > mx[k]) mx[k] = p; }
    size_t ntri = idx.size() / 3;
    size_t t = 0;
    if (ntri == 0) { Part p; parts.push_back(p); }  // degenerate guard
    while (t < ntri) {   // split into parts of <=60000 unique verts. RENDMESH indices are u16 + MULTI-PART (the device &
        // renderer offset each part by its running vertex base). Cap < 65535 (0xFFFF): the device verifier REJECTS a part
        // of exactly 65535 verts (the erebor statues/halls + moria pillar vanished). Multi-MATERIAL meshes are already
        // separate ExportMeshes, so this is purely a u16-index size split.
        Part pr; std::unordered_map<uint32_t, uint16_t> remap; remap.reserve(70000);
        while (t < ntri) {
            uint32_t tri[3] = { idx[t*3], idx[t*3+1], idx[t*3+2] };
            int newv = 0; for (uint32_t g : tri) if (!remap.count(g)) newv++;
            if (!remap.empty() && remap.size() + (size_t)newv > 60000) break;   // flush this part
            for (uint32_t g : tri) {
                auto it = remap.find(g); uint16_t l;
                if (it == remap.end()) {
                    l = (uint16_t)remap.size(); remap.emplace(g, l);
                    float x = pos[(size_t)g*3], y = pos[(size_t)g*3+1], z = pos[(size_t)g*3+2];
                    uint8_t b[4];
                    memcpy(b,&x,4); pr.vb.insert(pr.vb.end(),b,b+4);
                    memcpy(b,&y,4); pr.vb.insert(pr.vb.end(),b,b+4);
                    memcpy(b,&z,4); pr.vb.insert(pr.vb.end(),b,b+4);
                    uint16_t u = f32_to_f16(haveUv ? uv[(size_t)g*2] : 0.f), v = f32_to_f16(haveUv ? uv[(size_t)g*2+1] : 0.f);
                    uint8_t bb[4] = { (uint8_t)u,(uint8_t)(u>>8),(uint8_t)v,(uint8_t)(v>>8) }; pr.vb.insert(pr.vb.end(),bb,bb+4);
                    if (skinned) {   // SKINNED stride-28 (match butterflies/unlitdoublesidedskinned): COLOR0(sem4) + boneIdx(sem7) + weights(sem8 u8x4-UNORM). Was stride-24 (no sem4) -> shader read bone idx/weight at wrong offsets -> mesh collapsed.
                        // sem4 = vertexColor0 (COLOR_0), u8x4 RGBA — NOT a normal. GROUND TRUTH: the working butterfly mesh
                        // stores (255,255,255,255) here, and the source omnidroid's COLOR_0 is (1,1,1,1). The shader does
                        // baseColor * vertexColor0, so the old (0,0,127,0) "normal placeholder" multiplied the droid down to
                        // near-black (THE "full dark" bug). White = neutral = the texture (incl. the red strip) shows fully.
                        uint8_t col0[4] = { 0xFF,0xFF,0xFF,0xFF };   // vertexColor0 sem4 (white=neutral, OR baked lightmap)
                        if (vertCol.size() >= (size_t)(g+1)*4) { col0[0]=vertCol[(size_t)g*4]; col0[1]=vertCol[(size_t)g*4+1]; col0[2]=vertCol[(size_t)g*4+2]; col0[3]=vertCol[(size_t)g*4+3]; }
                        pr.vb.insert(pr.vb.end(), col0, col0+4);
                        uint8_t bidx[4] = { boneRemap[boneIdx[(size_t)g*4]], boneRemap[boneIdx[(size_t)g*4+1]],
                                            boneRemap[boneIdx[(size_t)g*4+2]], boneRemap[boneIdx[(size_t)g*4+3]] };  // remap to dense palette slot
                        pr.vb.insert(pr.vb.end(), bidx, bidx+4);
                        pr.vb.insert(pr.vb.end(), &boneWgt[(size_t)g*4], &boneWgt[(size_t)g*4]+4);
                    } else if (vatVertexCount > 0) {   // VAT column = the RAW vertex index as f16 (replaces the normal slot).
                        // ⛔ The vatunlitbasecolor/doublesided VS does `vertexCol = uvVAT.x + 0.5` and texelFetches column
                        // floor(vertexCol) DIRECTLY (no ×textureWidth) — so uvVAT.x MUST be the integer vertex index. The
                        // old (index+0.5)/count value (a 0..1 UNORM fraction) floors to column 0/1 for EVERY corner -> all
                        // verts read the same 1-2 texels -> the quad can't deform -> the "collapse". Store the raw index as
                        // an f16 (UV0's SFLOAT format), exact for indices < 2048 (VAT meshes are small).
                        uint16_t hx = f32_to_f16((float)g), hy = 0;
                        uint8_t cb[4] = { (uint8_t)hx, (uint8_t)(hx>>8), (uint8_t)hy, (uint8_t)(hy>>8) }; pr.vb.insert(pr.vb.end(),cb,cb+4);
                    } else {
                        uint8_t nb[4] = { 0xFF,0xFF,0xFF,0xFF };   // sem4 COLOR_0 (white=neutral, OR baked lightmap → base×COLOR0)
                        if (vertCol.size() >= (size_t)(g+1)*4) { nb[0]=vertCol[(size_t)g*4]; nb[1]=vertCol[(size_t)g*4+1]; nb[2]=vertCol[(size_t)g*4+2]; nb[3]=vertCol[(size_t)g*4+3]; }
                        pr.vb.insert(pr.vb.end(),nb,nb+4);
                    }
                    pr.nv++;
                } else l = it->second;
                pr.ib.push_back((uint8_t)l); pr.ib.push_back((uint8_t)(l >> 8));
            }
            t++;
        }
        parts.push_back(std::move(pr));
    }
    float aabb[6] = { mn[0],mn[1],mn[2], mx[0],mx[1],mx[2] };
    // Y-ROTATION: geometry is centered on the (origin) pivot; the getTime() shader spins it about Y. The device
    // frustum-culls per-draw with this STATIC AABB (it can't see the shader's rotation), so a planet orbiting at
    // radius R is culled whenever its REST AABB is off-screen -> "planets disappear when watched". Fix: bake the
    // AABB as the SWEPT CYLINDER (X,Z = ±maxR over the orbit; Y unchanged) so the box covers the whole orbit.
    if (spinBoundsY) {   // geometry is centered on the pivot; an axis-through-origin rotation preserves each vert's
        float maxR2 = 0; // DISTANCE from origin, so the swept volume = a SPHERE of radius max|v|. Box it (±maxR all axes)
        for (size_t v = 0; v + 2 < pos.size(); v += 3) { float r2 = pos[v]*pos[v] + pos[v+1]*pos[v+1] + pos[v+2]*pos[v+2]; if (r2 > maxR2) maxR2 = r2; }
        float maxR = sqrtf(maxR2);
        aabb[0] = -maxR; aabb[1] = -maxR; aabb[2] = -maxR; aabb[3] = maxR; aabb[4] = maxR; aabb[5] = maxR;
    }
    float radius = 0; for (int k = 0; k < 3; k++) { float a = aabb[k] < 0 ? -aabb[k] : aabb[k], c = aabb[k+3] < 0 ? -aabb[k+3] : aabb[k+3]; if (a > radius) radius = a; if (c > radius) radius = c; }
    // SKINNED meshes must keep the REAL MODEL-LOCAL AABB (computed above from hzRestPos), exactly like every
    // official skinned mesh (the vista butterfly's RENDMESH AABB is its tiny model-local extent, ~0.1, NOT ±1e5).
    // The ±1e5 nocullBounds "draw-everything" trick ONLY works for STATIC meshes: V205 frustum-culls those by the
    // cooked AABB, so a scene-spanning box is never dropped. But for SKINNED meshes the device IGNORES the cooked
    // AABB and RECOMPUTES the cull bounds per-frame from the skeleton's posed joints — so a ±1e5 AABB does nothing
    // and a WORLD-BAKED skeleton yields world-placed, animation-oscillating bounds that leave the frustum and cull
    // the mesh at certain camera angles (device-confirmed via the render-trace MCP; preview renders fine because it
    // doesn't run V205's skinned cull). The real fix is the model-local skeleton (gltf_loader/opa_loader
    // butterfly-parity); keeping a model-local AABB here matches the official skinned-mesh shape. HSR_NOCULL_SKIN
    // forces the old ±1e5 behavior for A/B. [[project_hsr_ondevice_rendertrace]]
    if (!skinned || std::getenv("HSR_NOCULL_SKIN"))
        nocullBounds(aabb, radius);   // HSR_NOCULL: V79-style draw-everything (scene-spanning bounds defeat V205 frustum/occlusion/size cull)
    // POS+UV0+NORMAL by default; for VAT the 3rd attr is UV1 (the column) and the format-hash differs (both reversed
    // from real meshes: oceanarium VAT-format VS.f0 = {0x4157E789,0x79BF0758,0}).
    static const uint8_t VSF0_N[12] = { 0x23,0xa5,0xe0,0xdb, 0x22,0x95,0x8f,0xf3, 0,0,0,0 };
    static const uint8_t VSF0_V[12] = { 0x89,0xe7,0x56,0x41, 0x58,0x07,0xbf,0x79, 0,0,0,0 };
    static const uint8_t VSF0_S[12] = { 0x92,0xb6,0x7a,0xd8, 0x01,0x13,0x34,0x77, 0,0,0,0 };   // SKINNED format hash — unlitdoublesidedskinned stride-28 (POS+UV+NORMAL+boneIdx+weight), matched byte-for-byte to the calming butterflies' working skinned mesh
    const uint8_t* VSF0 = skinned ? VSF0_S : (vatVertexCount > 0 ? VSF0_V : VSF0_N);
    float ones[2] = { 1.f,1.f }; uint8_t rf8[8]; memcpy(rf8, ones, 8);
    uint8_t attr[24] = { 0,0x32,0,0, 5,0x21,0,0, 4,0x13,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };  // POS f32x3@0, UV f16x2@12, NORMAL(sem4) fmt0x13@16
    int nattr = 3;
    if (skinned) { attr[12]=7; attr[13]=0x13; attr[14]=0; attr[15]=0; attr[16]=8; attr[17]=0x13; attr[18]=0; attr[19]=0; nattr=5; }  // POS,UV,NORMAL(sem4),boneIdx(sem7),weight(sem8) stride28 — exact butterflies layout
    else if (vatVertexCount > 0) { attr[8]=5; attr[9]=0x21; attr[10]=1; attr[11]=0; }   // 3rd attr -> UV1 f16x2 SFLOAT (VAT column = raw vertex index, NOT u16 UNORM 0x27 which can't hold an index >1)
    size_t cap = 2048 + embeddedMatl.size() * (parts.size() + 1); for (auto& p : parts) cap += p.vb.size() + p.ib.size() + 256;
    FB b(cap);
    std::vector<int> partOffs;
    for (auto& p : parts) {
        int vbo = b.createByteVector(p.vb.data(), p.vb.size());
        int ibo = b.createByteVector(p.ib.data(), p.ib.size());
        int ao  = b.createStructVector(attr, 4, nattr, 4);
        uint64_t vbH = murmur64A(p.vb.data(), p.vb.size()), ibH = murmur64A(p.ib.data(), p.ib.size());
        uint8_t vsf4[12] = {0}; memcpy(vsf4, &vbH, 8); uint8_t pf5[12] = {0}; memcpy(pf5, &ibH, 8);
        b.startObject(5); b.addStructSlot(0, VSF0, 8, 8); b.addScalar<uint32_t>(1, p.nv); b.addOffset(2, vbo); b.addOffset(3, ao); b.addStructSlot(4, vsf4, 8, 8); int vs = b.endObject();
        int vsvec = b.createOffsetVector({ vs });
        int matEmb = embeddedMatl.empty() ? 0 : b.createByteVector(embeddedMatl.data(), embeddedMatl.size());
        b.startObject(7); b.addOffset(0, vsvec); b.addOffset(1, ibo); b.addOffset(3, matEmb); b.addStructSlot(4, (const uint8_t*)aabb, 24, 4); b.addStructSlot(5, pf5, 8, 8);   // ALIGNMENT FIX (IDA-proven, V205.2 verify @0xeba93c): part.f5 = murmur64A(IB) is an 8-byte 8-ALIGNED struct. vertexStream.f0/f4 same.
        // part.f6 (52B) — present on EVERY official V205 vista/skinned mesh (butterfly, whale, coral, fish). Decoded
        // layout {u32 52, u64 ibHash, u32 0, float[6] aabb, u32 16, u32 f1Val, u32 f0Val}. The float[6] is the per-part
        // MODEL-LOCAL AABB. The earlier "device-disproven" test was INVALID: it wrote the ±1e5 scene-spanning aabb (the
        // pre-fix nocullBounds value), so f6 carried garbage bounds. Now that skinned meshes keep their real model-local
        // aabb (see the nocullBounds skip above), f6 carries the CORRECT model-local AABB — exactly like the official
        // butterfly (decoded f6 aabb = [-0.091,-0.005,-0.074, 0.091,0.008,0.071] = its model-local extent). The device
        // reads this per-part model-local bound to seed the SKINNED cull (it can't use the per-frame skeleton sanely
        // without it). DEFAULT ON for skinned; HSR_NOPARTF6 disables for A/B. [[project_hsr_ondevice_rendertrace]]
        if (skinned && !std::getenv("HSR_NOPARTF6")) {
            uint8_t partF6[52]; memset(partF6, 0, 52);
            uint32_t f6sz = 52, f6c = 16, f6ib = (uint32_t)p.ib.size();
            memcpy(partF6, &f6sz, 4); memcpy(partF6 + 4, &ibH, 8); memcpy(partF6 + 16, (const uint8_t*)aabb, 24);  // aabb = model-local (skinned)
            memcpy(partF6 + 40, &f6c, 4); memcpy(partF6 + 48, &f6ib, 4);
            b.addStructSlot(6, partF6, 52, 4);
        }
        int part = b.endObject();
        partOffs.push_back(part);
    }
    int pv = b.createOffsetVector(partOffs);
    // SKINNED meshes need the device's "has skin weights" markers (shipped whale mesh has them; static meshes don't):
    // LOD.f2 = u16 max bone index, LOD.f4 = u16 0, ROOT.f0 = empty vector. Without them the device logs
    // "Mesh ... has no skin weights and will not follow skeleton animation" and renders the frozen bind pose.
    bool markers = skinned && !std::getenv("HSR_HZNOMARK");   // diag: isolate skin markers from the stride-24 format
    // LOD.f2 = max LOCAL (remapped) bone index = palette.size()-1 — the highest slot any vertex now uses (dense).
    uint16_t maxBoneIdx = (skinned && !palette.empty()) ? (uint16_t)(palette.size() - 1) : 0;
    uint16_t zero16 = 0;
    int lod;
    if (markers) {
        b.startObject(5); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f);   // non-zero required (addScalar elides 0.0 -> verifier rejects)
        b.addStructSlot(2, (const uint8_t*)&maxBoneIdx, 2, 2);
        b.addStructSlot(3, (const uint8_t*)aabb, 24, 4);
        b.addStructSlot(4, (const uint8_t*)&zero16, 2, 2);
        lod = b.endObject();
    } else {
        b.startObject(4); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); lod = b.endObject();   // non-zero required (addScalar elides 0.0 -> verifier rejects)
    }
    int lv = b.createOffsetVector({ lod });
    // ROOT.f2: static meshes leave this EMPTY. SKINNED meshes populate it with ONE element whose field0 is an OFFSET to a
    // vector<u32> of JOINT-ID HASHES = MurmurHash3_x86_32(joint name, seed 0), one per PALETTE slot (the joints the mesh
    // actually references, dense). The device maps each vertex's (remapped) local bone index -> f2[localIdx] -> StringId ->
    // skeleton joint. f2 MUST contain only referenced joints: an unreferenced palette entry makes the device's skin build
    // reject the mesh (MeshDefinition::fix verification fail -> crash). f2[local] = murmur3(joint name of palette[local]).
    // VERIFIED: butterfly f2 ids == murmur3 of the joints its verts use. Static meshes keep f2 EMPTY (mat via part.f3).
    int matVec;
    if (skinned && !jointIds.empty() && !palette.empty()) {
        std::vector<uint32_t> paletteIds; paletteIds.reserve(palette.size());
        for (uint8_t sk : palette) paletteIds.push_back(sk < jointIds.size() ? jointIds[sk] : 0u);
        int jidVec = b.createStructVector((const uint8_t*)paletteIds.data(), 4, (int)paletteIds.size(), 4);  // vector<u32> palette joint ids
        b.startObject(1); b.addOffset(0, jidVec); int skinDesc = b.endObject();
        matVec = b.createOffsetVector({ skinDesc });
    } else {
        matVec = b.createOffsetVector({});
    }
    uint16_t rootF0val = 2;   // ROOT.f0 = u16 2 (the whale's skinned marker). I'd wrongly made it a 4-byte EMPTY
                              // VECTOR (offset) -> the strict device Verifier rejected the buffer ("MeshDefinition::fix").
    int badF0 = (markers && std::getenv("HSR_HZBADF0")) ? b.createOffsetVector({}) : 0;   // demo: reproduce the old bug
    b.startObject(9);
    if (markers) { if (badF0) b.addOffset(0, badF0); else b.addStructSlot(0, (const uint8_t*)&rootF0val, 2, 2); }
    b.addOffset(1, lv); b.addOffset(2, matVec); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); b.addScalar<float>(4, radius); b.addStructSlot(8, rf8, 8, 4); int root = b.endObject();
    b.finish(root, "MESH"); return b.output();
}

// ── "Export edited scene -> APK" — what the editor's Export button calls. Each mesh's positions must already be
//    in WORLD space (the editor bakes gm.model in), so every entity uses an identity transform. One shared unlit
//    shader; per-mesh texture+material. Returns the unsigned APK bytes (sign separately). ──────────────────────
// ── HZANIM: skeletal animation port (the incredibles + any V79 skinned env). ──
// HZAN:SKEL "Skel" encoder — exact inverse of parseRendSkelV203: magic"Skel"(4) ver u32 nameLen u32 name |
// u16 jointCount | per joint: i16 parent, u32 nameLen, name, T f32x3, Q(w,x,y,z) f32x4, S f32x1. Parents precede children.
struct HzJoint { int parent; std::string name; float pos[3]; float quat[4]; float scale; };  // quat = (w,x,y,z), LOCAL bind
inline std::vector<uint8_t> encodeHzSkel(const std::vector<HzJoint>& joints, const std::string& skelName = "Skeleton_0") {
    std::vector<uint8_t> b;
    auto w32=[&](uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)(v>>(8*i))); };
    auto w16=[&](uint16_t v){ b.push_back((uint8_t)v); b.push_back((uint8_t)(v>>8)); };
    auto wf =[&](float f){ uint32_t u; memcpy(&u,&f,4); w32(u); };
    b.push_back('l'); b.push_back('e'); b.push_back('k'); b.push_back('S');   // magic bytes "lekS" (u32 0x536B656C) — real device skels store this, NOT ASCII "Skel"
    w32(1);                                                        // version
    w32((uint32_t)skelName.size()); b.insert(b.end(), skelName.begin(), skelName.end());
    w16((uint16_t)joints.size());
    for (const auto& j : joints) {
        w16((uint16_t)(int16_t)j.parent);                         // i16 parent (-1 -> 0xFFFF)
        w32((uint32_t)j.name.size()); b.insert(b.end(), j.name.begin(), j.name.end());
        wf(j.pos[0]); wf(j.pos[1]); wf(j.pos[2]);
        wf(j.quat[0]); wf(j.quat[1]); wf(j.quat[2]); wf(j.quat[3]);
        wf(j.scale);
    }
    return b;
}

struct ExportMesh {
    std::string name;
    std::vector<float> positions;   // WORLD-space xyz*nVerts (editor pre-bakes the model matrix)
    std::vector<float> uvs;         // uv*nVerts (optional)
    std::vector<uint32_t> indices;  // triangle list
    std::vector<uint8_t> rgba;      // decoded base-color RGBA8 (optional)
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> srcAstc;   // LOSSLESS pass-through: source ASTC mip chain (blocks) if the base tex is a plain KTX-ASTC and UNMODIFIED
    uint32_t srcAstcBw = 0, srcAstcBh = 0, srcAstcMips = 0;
    std::vector<uint8_t> iblVertCol; // FAITHFUL SpecIbl diffuse-irradiance per-vertex RGBA = diffuseCube(worldN)·ambientIBLTint, baked in buildExportMeshes (the renderer's exact uploadMesh bake). Device base·vertexColor0 = env-lit lake water, NOT the dark basecolor (the "black lake" bug). Empty for non-specibl meshes.
    bool blend = false;             // alpha-blended (transparent) -> route to unlitblend.surface
    bool alphaTest = false;         // MASK/cutout (foliage, *_masked scenery): DEPTH-WRITE cutout shader (alpha-test discard) so it OCCLUDES + discards transparent texels — was routed to the no-depth-write blend pass = the lakeside 2D-scenery overlay/flash. Meta cuts depth-write too (unlitfoliage f2,f3 PRESENT).
    bool additive = false;          // EMISSIVE GLOW (warp streaks, nebula fog, force-field) -> route to the OPAQUE-pass shader (unlit/unlitdoublesidedskinned, f2,f3 PRESENT) kept in the TRANSPARENT MATL = ADDITIVE blend (src+dst). Alpha-blended these vanish on a dark backdrop (the Star Trek warp/fog "IS NOT VISIBLE" bug). 3x device-proven: opaque vs alpha shaders differ ONLY in pass f2,f3; the shipped unlitspritesheetflipbookadditive keeps f2,f3 PRESENT.
    bool doubleSided = false;       // glTF doubleSided material -> cook appends REVERSED tris so the single-sided unlit shader still draws back faces (else flat/open meshes back-face-cull on device = see-through HOLES; renderer honors doubleSided so its preview looked fine)
    bool flipbook = false;          // animated flat material -> route to unlitspritesheetflipbookadditive.surface (GPU getTime() spritesheet cycle, NO skeleton)
    int flipCols = 0, flipRows = 0; // spritesheet grid (cols x rows)
    int flipFrames = 0;             // total cells cycled (0 -> cols*rows)
    float flipFps = 0.f;            // playback rate (0 -> default 5fps); cook auto-generates the shadergen::FLIPBOOK shader for this grid/fps
    bool flipOffset = false;        // OFFSET flipbook (mat.sanim waterfall/stream/fog): mesh UV maps to ONE cell -> uv'=inUv+(col/cols,row/rows), NO cell scale (vs spritesheet = full-quad scale flipbook)
    std::vector<float> flipUVMats;   // EXACT per-frame FULL 2x3 UV matrices from the mat.sanim track (flipN frames × 6 = [a,b,c,d,e,f]) — the SAME data the desktop's animate() plays (uv' = M[frame]·uv). Replayed frame-snapped in the fragment so the device matches the desktop VERBATIM (no derived-grid guessing; handles scroll, sprite-cell AND 2x2 SCALE/dust). Empty -> cols/rows grid.
    int flipN = 0; float flipLoop = 0.f;
    float matTint[4] = {1,1,1,1};   // the mesh's OWN base-color tint (glTF baseColorFactor) for its material params
    std::vector<float> vatOffsets;  // VAT vertex offsets, frames*vertexCount*3 (WORLD space) -> animated via VAT (non-skeletal)
    int vatFrames = 0;
    bool pulse = false;             // node-animated billboard (flame wisp) -> CUSTOM wisp_pulse.surface (unlitblend + getTime() brightness pulse)
    bool skybox = false;            // user-marked far backdrop -> SkyboxPlatformComponent (depth-clamped, EXEMPT from the PortalStereoCamera far=5000 clip)
    // V79 node Y-ROTATION (Outer Wilds skybox/Interloper) -> getTime() Y-rotation shader cooker/rot_<periodms>_<p|m>.surface.
    bool rotAnim = false; float rotOmega = 0.f; float rotAxis[3] = {0,1,0}; float rotPivot[3] = {0,0,0};   // ARBITRARY-axis spin: signed rad/s about rotAxis around rotPivot (node WORLD origin; children orbit it)
    bool rotReplay = false; int rotReplayN = 0; float rotLoopSec = 0.f; std::vector<float> rotQuats;   // EXACT per-frame QUATERNION replay (aurora): shadergen::ROTREPLAY, (rotReplayN+1)*4 relative quats over rotLoopSec about rotPivot (nlerp+rotate in the vertex shader)
    bool rotOsc = false; float rotAmp = 0.f; float rotPeriod = 0.f;   // OSCILLATION (sway): angle=(amp/2)(1-cos(2pi t/period)) about rotAxis — clips that swing out & back (Snake Way King Kai planet), NOT a spin
    bool uvScroll = false; float uvRate[2] = {0.f, 0.f};   // mat.sanim UV-SCROLL (water/foam/waterfall) -> getTime() uv += uvRate*time shader
    // GENERAL node TRANSLATION replay (Star Trek sliding screens etc.) -> shadergen::TRANSLATE getTime() shader (faithful, any track)
    bool transAnim = false; std::vector<float> transFrames; int transN = 0; float transLoop = 0.f;   // transN frames of vec3 OFFSET, loop seconds
    bool fadeAnim = false; std::vector<float> fadeFrames; int fadeN = 0; float fadeLoop = 0.f;   // mat.sanim MaterialTint ALPHA fade curve replayed in the flipbook frag; fadeLoop synced to transLoop
    bool tintAnim = false; std::vector<float> tintFrames; int tintN = 0; float tintLoop = 0.f;   // mat.sanim MaterialTint FULL RGBA cycle (fireworks flashes / window-light flicker) -> shadergen::TINTREPLAY chained onto the mesh's shader (supersedes the alpha-only fade when set)
    // GENERAL node SCALE "breathe" replay (Erebor's 12 wisps, NON-UNIFORM per-axis) -> shadergen::SCALE getTime() shader.
    // scaleFrames = scaleN frames of per-axis FACTOR (= scale(t)/scale(0), frame0=(1,1,1)); scalePivot = node WORLD origin.
    bool scaleAnim = false; std::vector<float> scaleFrames; int scaleN = 0; float scaleLoop = 0.f; float scalePivot[3] = {0,0,0};
    // V203 NON-skeletal pose/scale animation (the FAITHFUL wisp port — IDA: ShellPoseAnimationComponent -> CoPoseAnimation,
    // ticked by AnimationSystem, NO skeleton). Lerps the entity pose start->end; for a wisp = startScale<->endScale (V79 min/max).
    bool poseAnim = false;
    float poseStartScale[3] = {1,1,1}, poseEndScale[3] = {1,1,1};   // V79 node-SCALE per-component min / max
    float poseTransDelta[3] = {0,0,0};   // V79 node-TRANSLATION net displacement (cars/train) -> pose end = centroid+delta
    float poseCentroid[3]  = {0,0,0};                               // wisp world centroid -> recenter geometry + entity pos (scale pivots in place)
    float poseDuration = 1.f;                                       // seconds, the start->end window (V79 anim period)
    // HZANIM (skeletal): hzFrames>1 -> skinned RENDMESH + HZAN:SKEL + ACL HZAN:ANIM + AnimatorPlatformComponent
    std::vector<float> hzJointPos, hzJointQuat, hzJointScale;   // jointCount * {3, 4(wxyz), 1}
    std::vector<int>   hzParents;                               // jointCount
    std::vector<uint8_t> hzBoneIdx, hzBoneWgt;                  // nv*4 (sem7 indices, sem8 weights)
    std::vector<float> hzTrsLocal;                              // frames*jointCount*10 (quat xyzw + trans3 + scale3)
    std::vector<float> hzRestPos;                              // nv*3 REST positions for the skinned mesh (NOT world-baked)
    int hzJointCount = 0, hzFrames = 0; float hzFps = 0.f;
    // ANIMATED COLLIDER: user marked this (animated) mesh as a collider -> ship a ColliderMesh + PhysicsBody on the
    // SAME entity as the mesh+animator, so V205's kinematic system (KinematicPose/ShapeUpdateTask) drives it with the
    // SAME animation (skeleton for skinned, transform for node-anim) — the collider follows the animation.
    bool wantCollider = false;
    // ── EXPOSE-ALL (user demand): decoded streams the cook must consume for a faithful V205 port. Populated from
    //    MeshData in buildExportMeshes; the cook emits each in the V205 format. See project_hsl_cooker_expose_all_audit.
    std::vector<float> uvs2;                                 // uv1 = lightmap unwrap -> TEXCOORD1
    std::vector<uint8_t> lmRGBA; uint32_t lmW=0, lmH=0;      // baked lightmap -> lightingParams.lightMapTexture
    bool hasLightmap=false, bakeLightmapVtx=false; float lightmapPower[3]={1,1,1};
    std::vector<uint8_t> normalRGBA; uint32_t normalW=0, normalH=0; bool hasNormal=false;   // normal map
    float albedoFactor[3]={1,1,1};                           // basecolor factor (metal/gem tint) -> matParams
    float curTint[4]={1,1,1,1};                              // per-frame mat.sanim MaterialTint (fog/dust/foam opacity)
    float vatInstTrackIndex=-1, vatInstRateFactor=-1, vatInstTimeOffset=-1, atlasCellIndex=-1;   // per-instance VAT -> `instance` UBO (set2 bind1)
};
inline std::vector<uint8_t> readFileBytes(const std::string& p) {
    { std::vector<uint8_t> e; if (embassets::get(p, e)) return e; }   // STANDALONE: cooker/*.bin templates are baked in
    std::vector<uint8_t> b; FILE* f = fopen(p.c_str(), "rb"); if (!f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { b.resize((size_t)n); size_t r = fread(b.data(), 1, (size_t)n, f); b.resize(r); } fclose(f); return b;
}
inline bool writeFileBytes(const std::string& p, const std::vector<uint8_t>& b) {
    FILE* f = fopen(p.c_str(), "wb"); if (!f) return false; if (!b.empty()) fwrite(b.data(), 1, b.size(), f); fclose(f); return true;
}
// Detect a V79 "spritesheet flipbook" mesh (sonic_schoolhouse Sprite_3x3, Rick&Morty animated TV 10x10): K quads
// whose UVs tile a cols x rows grid — each quad is one frame, skinned to a bone the armature scale-toggles on/off
// (the V79 way to flipbook a flat sprite). At the streamed ANIMATED frame exactly ONE cell is full-area at the
// screen and the rest are collapsed (degenerate) — so they overlap there. COLLAPSE to ONE quad with UV 0..1 and
// return cols/rows for ANY grid; the cook then AUTO-GENERATES a getTime() shadergen::FLIPBOOK shader sized to that
// grid (no longer 3x3-only). Runs on the WORLD-baked export geometry (the screen overlap survives the xform).
inline bool detectAndCollapseFlipbook(std::vector<float>& pos, std::vector<float>& uvs,
                                      std::vector<uint32_t>& idx, int& cols, int& rows) {
    size_t nv = pos.size()/3;
    if (nv < 8 || nv % 4 != 0 || uvs.size() < nv*2) return false;
    size_t nq = nv/4;
    auto fabsf2=[](float x){ return x<0?-x:x; };
    // per-quad: bbox center + TRUE planar AREA (two triangles) + UV cell origin. Area (not bbox diag) is the key:
    // the V79 skinned flipbook collapses each OFF frame's quad to a LINE (scale.Y->0) which keeps full X width
    // (big diag) but ~zero area — orientation-independent, works for wall posters facing any axis.
    auto triArea=[&](const float*a,const float*b,const float*c)->float{
        float u[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, w[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
        float cx=u[1]*w[2]-u[2]*w[1], cy=u[2]*w[0]-u[0]*w[2], cz=u[0]*w[1]-u[1]*w[0];
        return 0.5f*(float)std::sqrt(cx*cx+cy*cy+cz*cz); };
    auto quadInfo=[&](size_t q, float* c, float& area, float& u0, float& v0){
        float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f}; u0=v0=1e30f;
        for (int k=0;k<4;k++){ size_t v=q*4+k;
            for(int j=0;j<3;j++){ float p=pos[v*3+j]; if(p<lo[j])lo[j]=p; if(p>hi[j])hi[j]=p; }
            if(uvs[v*2]<u0)u0=uvs[v*2]; if(uvs[v*2+1]<v0)v0=uvs[v*2+1]; }
        for(int j=0;j<3;j++) c[j]=0.5f*(lo[j]+hi[j]);
        const float *p0=&pos[(q*4)*3],*p1=&pos[(q*4+1)*3],*p2=&pos[(q*4+2)*3],*p3=&pos[(q*4+3)*3];
        area = triArea(p0,p1,p2)+triArea(p0,p2,p3); };
    auto addUnique=[](std::vector<int>& v,int x){ for(int e:v) if(e==x) return; v.push_back(x); };
    std::vector<int> ucol, vrow;
    int refQ=0; float maxArea=-1.f;
    for (size_t q=0;q<nq;q++){ float c[3],ar,u0,v0; quadInfo(q,c,ar,u0,v0);
        if (ar>maxArea){ maxArea=ar; refQ=(int)q; }
        addUnique(ucol,(int)(u0*1000+0.5f)); addUnique(vrow,(int)(v0*1000+0.5f)); }
    cols=(int)ucol.size(); rows=(int)vrow.size();
    // every NON-visible quad must be DEGENERATE (area ~0 — a collapsed OFF frame) or COINCIDENT with the visible
    // (max-area) quad. Side-by-side TILED images (9 spread, full-area quads) are rejected — NOT a flipbook.
    float rc[3],rar,ru0,rv0; quadInfo((size_t)refQ,rc,rar,ru0,rv0);
    float refDiag=0; { float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
        for(int k=0;k<4;k++){ size_t v=(size_t)refQ*4+k; for(int j=0;j<3;j++){ float p=pos[v*3+j]; if(p<lo[j])lo[j]=p; if(p>hi[j])hi[j]=p; } }
        float dx=hi[0]-lo[0],dy=hi[1]-lo[1],dz=hi[2]-lo[2]; refDiag=(float)std::sqrt(dx*dx+dy*dy+dz*dz); }
    bool flipShape = (maxArea>1e-5f);
    for (size_t q=0;q<nq && flipShape;q++){ float c[3],ar,u0,v0; quadInfo(q,c,ar,u0,v0);
        bool degen = ar < 0.05f*maxArea;
        float dd=0; for(int j=0;j<3;j++){ float e=c[j]-rc[j]; dd+=e*e; } dd=(float)std::sqrt(dd);
        bool coinc = dd < 0.15f*(refDiag>1e-4f?refDiag:1.f);
        if (!degen && !coinc) flipShape=false; }
    if (std::getenv("HSR_FLIPDBG") && nq<=16)
        fprintf(stderr, "[FLIPDBG] nv=%zu nq=%zu cols=%d rows=%d flipShape=%d refQ=%d maxArea=%.3f\n",
            nv, nq, cols, rows, (int)flipShape, refQ, maxArea);
    if (!flipShape) return false;
    if (cols*rows != (int)nq || cols<1 || rows<1 || cols>32 || rows>32) return false;   // ANY C×R grid (was 3x3-only): the flipbook shader is now auto-generated per-grid (shadergen::FLIPBOOK). Rick&Morty TV = 10x10/100 cells.
    int q0=refQ; float um=ru0, vm=rv0;          // collapse to the visible (max-area) quad; remap ITS cell -> 0..1
    float cw=1.f/cols, ch=1.f/rows;
    std::vector<float> p2(12), u2(8);
    for (int k=0;k<4;k++){ size_t v=(size_t)q0*4+k;
        p2[k*3]=pos[v*3]; p2[k*3+1]=pos[v*3+1]; p2[k*3+2]=pos[v*3+2];
        u2[k*2]=(uvs[v*2]-um)/cw; u2[k*2+1]=(uvs[v*2+1]-vm)/ch; }
    std::vector<uint32_t> i2;
    for (size_t t=0; t+2<idx.size(); t+=3){ uint32_t a=idx[t],bb=idx[t+1],c=idx[t+2];
        uint32_t lo=(uint32_t)q0*4, hi=lo+4;
        if (a>=lo&&a<hi&&bb>=lo&&bb<hi&&c>=lo&&c<hi){ i2.push_back(a-lo); i2.push_back(bb-lo); i2.push_back(c-lo); } }
    if (i2.size()<6){ i2.clear(); uint32_t q[6]={0,1,2,0,2,3}; i2.assign(q,q+6); }
    pos.swap(p2); uvs.swap(u2); idx.swap(i2);
    return true;
}
// Split a LARGE static mesh (>cap unique verts) into SEPARATE single-part meshes (each its own entity). MULTI-PART
// static RENDMESHes pass the device verifier but DON'T RENDER (no official env ships a size-split static mesh; the
// device only draws multi-part for skinned/multi-material). u32 single-part renders as GARBAGE (the device reads the
// RENDMESH index buffer as u16). So the only faithful path for a >65535-vert static mesh (erebor Statues 91306 / Halls
// 87956, moria Pillar 235183) is many SINGLE-PART (u16) meshes. Chunks share the source texture (dedup'd in the cook).
inline std::vector<ExportMesh> splitLargeStaticMeshes(const std::vector<ExportMesh>& in, size_t cap = 60000) {
    std::vector<ExportMesh> out;
    for (const auto& m : in) {
        size_t nv = m.positions.size() / 3;
        bool skinnedOrVat = m.hzJointCount > 0 || m.vatFrames > 0 || !m.hzBoneIdx.empty();
        if (nv <= cap || skinnedOrVat || m.indices.size() < 3) { out.push_back(m); continue; }
        bool haveUv = m.uvs.size() >= nv * 2;
        size_t ntri = m.indices.size() / 3, t = 0;
        while (t < ntri) {
            ExportMesh c; c.name = m.name; c.blend = m.blend; c.alphaTest = m.alphaTest; c.additive = m.additive; c.w = m.w; c.h = m.h; c.rgba = m.rgba;
            c.srcAstc = m.srcAstc; c.srcAstcBw = m.srcAstcBw; c.srcAstcBh = m.srcAstcBh; c.srcAstcMips = m.srcAstcMips;   // split parts keep the lossless source-ASTC pass-through
            c.skybox = m.skybox;   // split parts MUST inherit the skybox mark, else a big dome (>60k verts) loses it -> far-clipped/black
            for (int k = 0; k < 4; k++) c.matTint[k] = m.matTint[k];
            c.tintAnim = m.tintAnim; c.tintFrames = m.tintFrames; c.tintN = m.tintN; c.tintLoop = m.tintLoop;   // split parts keep the tint cycle
            std::unordered_map<uint32_t, uint32_t> remap; remap.reserve(cap + 16);
            while (t < ntri) {
                uint32_t tri[3] = { m.indices[t*3], m.indices[t*3+1], m.indices[t*3+2] };
                int newv = 0; for (uint32_t g : tri) if (!remap.count(g)) newv++;
                if (!remap.empty() && remap.size() + (size_t)newv > cap) break;
                for (uint32_t g : tri) {
                    auto it = remap.find(g); uint32_t l;
                    if (it == remap.end()) {
                        l = (uint32_t)remap.size(); remap.emplace(g, l);
                        c.positions.push_back(m.positions[g*3]); c.positions.push_back(m.positions[g*3+1]); c.positions.push_back(m.positions[g*3+2]);
                        c.uvs.push_back(haveUv ? m.uvs[g*2] : 0.f); c.uvs.push_back(haveUv ? m.uvs[g*2+1] : 0.f);
                    } else l = it->second;
                    c.indices.push_back(l);
                }
                t++;
            }
            out.push_back(std::move(c));
        }
    }
    return out;
}
// Split a LARGE SKINNED mesh (>cap unique verts) into SEPARATE single-part skinned meshes, each its own entity that
// references the SAME skeleton + anim (deduped by the skel/anim byte-pool → one armatureN.skel + animclipN.anim). The
// skinned RENDMESH encoder ALREADY multi-parts a >60k-vert mesh into u16 parts, but a MULTI-PART skinned mesh CRASHES
// the device: DEVICE-PROVEN (snakeway 5_Car, 125,460 verts → 3 parts) the V205 HzAnimSystem skin-bind reads a per-part
// bone-id list (ptr,len) and murmur3-hashes `len` bytes; with one skin descriptor but N parts it indexes past the
// 1-element descriptor vector → garbage `len` → reads off the end of the mmap'd asset → SIGSEGV/SEGV_ACCERR on
// HSR:Async-0 during load (every other working skinned env — incredibles droid, calming butterflies — is SINGLE-part,
// <3k verts). The faithful fix is the SAME as the static-mesh split: many SINGLE-PART (u16) skinned meshes. Each piece
// carries its own per-vertex restPos / boneIdx / boneWgt / uv / uv1 subset; the skeleton + clip are identical bytes so
// they dedup to one shared asset every piece's AnimatorPlatformComponent points at (nuxd prism_wave + motes prove a
// shared skeleton/anim across entities). Splitting by triangle preserves each vertex's skin weights, so all pieces
// deform + animate together as one dragon. HSR_NOSKINSPLIT = keep the (crashing) multi-part path for diagnostics.
inline std::vector<ExportMesh> splitLargeSkinnedMeshes(const std::vector<ExportMesh>& in, size_t cap = 60000) {
    if (std::getenv("HSR_NOSKINSPLIT")) return in;
    // DEVICE-PROVEN: MeshAssetBuilder::updateAsset (Clay) over-allocates building a LARGE SKINNED mesh — its per-part
    // skinning buffer scales ~O(verts²) so a 60000-vert part balloons to gigabytes ("Requested new size exceeds size
    // representable by size_type" → std::length_error / OOM → env load REJECTED → nuxd fallback). The shipped skinned
    // envs (incredibles droid, calming butterflies) are all <3k verts/part, so they never hit it. Keep skinned parts
    // SMALL so verts² stays bounded; this just makes more (cheap) single-part skinned entities sharing the one skeleton.
    // Cap = balance of TWO opposing device failure modes (both throw std::length_error during the cold async load):
    //   • per-part verts TOO BIG  → MeshAssetBuilder over-allocates (~O(verts²)) → OOM.
    //   • TOO MANY pieces         → too many entities concurrently load the SHARED skel/anim → async-loader RACE.
    // Device-swept: 60000=OOM, 8000(18pc)=race-prone w/ a 256-frame clip, 24000(6pc)=OOM-prone, 16000(9pc)=best
    // (loads first-try most cold switches, ≤1 quick retry — beats the un-split incredibles). HSR_SKINCAP overrides.
    if (const char* e = std::getenv("HSR_SKINCAP")) { long c = atol(e); if (c >= 1000) cap = (size_t)c; }
    else cap = 16000;
    std::vector<ExportMesh> out;
    for (const auto& m : in) {
        size_t nv = m.positions.size() / 3;
        bool skinned = m.hzJointCount > 0 && m.hzBoneIdx.size() >= nv * 4;
        if (!skinned || nv <= cap || m.indices.size() < 3) { out.push_back(m); continue; }
        bool haveUv   = m.uvs.size()       >= nv * 2;
        bool haveUv2  = m.uvs2.size()      >= nv * 2;
        bool haveRest = m.hzRestPos.size() >= nv * 3;
        bool haveBW   = m.hzBoneWgt.size() >= nv * 4;
        size_t ntri = m.indices.size() / 3, t = 0; int npiece = 0;
        while (t < ntri) {
            ExportMesh c = m;   // copy ALL shared fields (skeleton hzJoint*/hzParents, anim hzTrsLocal, scalars, texture)
            c.positions.clear(); c.uvs.clear(); c.uvs2.clear(); c.indices.clear();
            c.hzRestPos.clear(); c.hzBoneIdx.clear(); c.hzBoneWgt.clear();
            std::unordered_map<uint32_t, uint32_t> remap; remap.reserve(cap + 16);
            while (t < ntri) {
                uint32_t tri[3] = { m.indices[t*3], m.indices[t*3+1], m.indices[t*3+2] };
                int newv = 0; for (uint32_t g : tri) if (!remap.count(g)) newv++;
                if (!remap.empty() && remap.size() + (size_t)newv > cap) break;
                for (uint32_t g : tri) {
                    auto it = remap.find(g); uint32_t l;
                    if (it == remap.end()) {
                        l = (uint32_t)remap.size(); remap.emplace(g, l);
                        c.positions.push_back(m.positions[g*3]); c.positions.push_back(m.positions[g*3+1]); c.positions.push_back(m.positions[g*3+2]);
                        if (haveUv)   { c.uvs.push_back(m.uvs[g*2]); c.uvs.push_back(m.uvs[g*2+1]); }
                        if (haveUv2)  { c.uvs2.push_back(m.uvs2[g*2]); c.uvs2.push_back(m.uvs2[g*2+1]); }
                        if (haveRest) { c.hzRestPos.push_back(m.hzRestPos[g*3]); c.hzRestPos.push_back(m.hzRestPos[g*3+1]); c.hzRestPos.push_back(m.hzRestPos[g*3+2]); }
                        for (int k = 0; k < 4; k++) c.hzBoneIdx.push_back(m.hzBoneIdx[g*4+k]);
                        for (int k = 0; k < 4; k++) c.hzBoneWgt.push_back(haveBW ? m.hzBoneWgt[g*4+k] : (uint8_t)(k==0?255:0));
                    } else l = it->second;
                    c.indices.push_back(l);
                }
                t++;
            }
            out.push_back(std::move(c)); npiece++;
        }
        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] SKINNED-SPLIT '%s' %zu verts -> %d single-part pieces (avoids multi-part skinned device crash)\n", m.name.c_str(), nv, npiece);
    }
    return out;
}
// ── AUTO-PROVISION the signing toolchain when a machine has NOTHING installed ───────────────────────────────
// curl (ships with Win10+/Linux/macOS) fetches the OS's official package; miniz/tar extracts it BESIDE the exe.
// Cached after the first run. So `--sign` / Cook works on a clean PC with no Android SDK and no JDK — zero setup.
inline bool httpDownload(const std::string& url, const std::string& outPath,
                         const std::function<void(float,const char*)>& prog, const char* what) {
    namespace fs = std::filesystem; std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    if (prog) prog(0.10f, what);
#ifdef _WIN32
    auto bs = [](std::string p){ for (char& c : p) if (c == '/') c = '\\'; return p; };
    std::string cmd = "curl.exe -L --fail --silent --show-error -o \"" + bs(outPath) + "\" \"" + url + "\"";
#else
    std::string cmd = "curl -L --fail --silent --show-error -o \"" + outPath + "\" \"" + url + "\"";
#endif
    int rc = system(cmd.c_str());
    if (rc != 0 || !fs::exists(outPath, ec) || fs::file_size(outPath, ec) < 1000000) {
        if (prog) prog(1.0f, "download failed (need curl + a network connection)");
        return false;
    }
    return true;
}

inline bool extractZipTo(const std::string& zipPath, const std::string& destDir) {
    namespace fs = std::filesystem; std::error_code ec; fs::create_directories(destDir, ec);
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zipPath.c_str(), 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&z); bool ok = true;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { ok = false; break; }
        std::string outp = destDir + "/" + st.m_filename;
        if (mz_zip_reader_is_file_a_directory(&z, i)) { fs::create_directories(outp, ec); continue; }
        fs::create_directories(fs::path(outp).parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&z, i, outp.c_str(), 0)) { ok = false; break; }
    }
    mz_zip_reader_end(&z); return ok;
}

// find the dir directly containing `tool`(.bat/.exe) anywhere under `root`.
inline std::string findToolDirUnder(const std::string& root, const char* tool) {
    namespace fs = std::filesystem; std::error_code ec;
    if (!fs::is_directory(root, ec)) return "";
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto& d = it->path();
        if (fs::exists(d / (std::string(tool) + ".bat"), ec) || fs::exists(d / tool, ec) ||
            fs::exists(d / (std::string(tool) + ".exe"), ec)) return d.string();
    }
    return "";
}
#ifndef _WIN32
inline void chmodExec(const std::string& dir, std::initializer_list<const char*> tools) {
    namespace fs = std::filesystem; std::error_code ec;
    for (const char* t : tools) { auto p = fs::path(dir) / t;
        if (fs::exists(p, ec)) fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec); }
}
#endif

// DOWNLOAD Google's official Android build-tools (apksigner + zipalign) beside the exe. `force` ignores any SDK.
inline std::string downloadBuildTools(const std::function<void(float,const char*)>& prog) {
    namespace fs = std::filesystem; std::error_code ec;
    std::string root = AppConfig::exeRel("android-build-tools");
    std::string have = findToolDirUnder(root, "apksigner");        // already fetched on a previous run?
    if (!have.empty()) return have;
#if defined(_WIN32)
    const char* osTag = "windows";
#elif defined(__APPLE__)
    const char* osTag = "macosx";
#else
    const char* osTag = "linux";
#endif
    std::string url = std::string("https://dl.google.com/android/repository/build-tools_r34-") + osTag + ".zip";
    std::string zip = root + "/build-tools.zip";
    if (!httpDownload(url, zip, prog, "Downloading Android build-tools (~58MB, one time)")) return "";
    if (prog) prog(0.55f, "Extracting build-tools");
    if (!extractZipTo(zip, root)) { if (prog) prog(1.0f, "build-tools extract failed"); return ""; }
    fs::remove(zip, ec);
    std::string bt = findToolDirUnder(root, "apksigner");
#ifndef _WIN32
    if (!bt.empty()) chmodExec(bt, { "apksigner", "zipalign", "aapt2", "aapt", "d8" });
#endif
    return bt;
}

// build-tools to use: HSR_BUILDTOOLS -> installed SDK -> AUTO-DOWNLOAD beside the exe.
inline std::string ensureBuildTools(const std::function<void(float,const char*)>& prog) {
    const char* btEnv = std::getenv("HSR_BUILDTOOLS");
    std::string bt = btEnv ? std::string(btEnv) : AppConfig::detectBuildTools();
    if (!bt.empty()) return bt;
    return downloadBuildTools(prog);   // nothing installed -> fetch it
}

// JAVA to use for apksigner/keytool. "" = a working `java` is already on PATH (use it). Otherwise auto-download a
// Temurin JRE beside the exe and return its home (caller puts <home>/bin on PATH + sets JAVA_HOME). "" on failure.
inline std::string ensureJava(const std::function<void(float,const char*)>& prog) {
    namespace fs = std::filesystem; std::error_code ec;
#ifdef _WIN32
    if (system("java -version >NUL 2>&1") == 0) return "";
#else
    if (system("java -version >/dev/null 2>&1") == 0) return "";
#endif
    std::string root = AppConfig::exeRel("jre");
    auto findJavaHome = [&](const std::string& r)->std::string{
        if (!fs::is_directory(r, ec)) return "";
        for (auto it = fs::recursive_directory_iterator(r, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_directory(ec) || it->path().filename() != "bin") continue;
#ifdef _WIN32
            if (fs::exists(it->path() / "keytool.exe", ec)) return it->path().parent_path().string();
#else
            if (fs::exists(it->path() / "keytool", ec)) return it->path().parent_path().string();
#endif
        }
        return "";
    };
    std::string jh = findJavaHome(root);
    if (!jh.empty()) return jh;
#if defined(_WIN32)
    const char* os = "windows"; const char* ext = "zip";
#elif defined(__APPLE__)
    const char* os = "mac"; const char* ext = "tar.gz";
#else
    const char* os = "linux"; const char* ext = "tar.gz";
#endif
    std::string url = std::string("https://api.adoptium.net/v3/binary/latest/17/ga/") + os + "/x64/jre/hotspot/normal/eclipse";
    std::string arc = root + "/jre." + ext;
    if (!httpDownload(url, arc, prog, "Downloading Java runtime (~40MB, one time)")) return "";
    if (prog) prog(0.20f, "Extracting Java runtime");
    bool ex;
#ifdef _WIN32
    ex = extractZipTo(arc, root);
#else
    ex = (system(("tar -xzf \"" + arc + "\" -C \"" + root + "\"").c_str()) == 0);
#endif
    fs::remove(arc, ec);
    if (!ex) return "";
    jh = findJavaHome(root);
#ifndef _WIN32
    if (!jh.empty()) chmodExec(jh + "/bin", { "java", "keytool" });
#endif
    return jh;
}

// Is a command runnable (quietly probe `<cmd> <verArg>`)?
inline bool toolPresent(const char* cmd, const char* verArg = "--version") {
    std::string c = std::string(cmd) + " " + verArg;
#ifdef _WIN32
    c += " >NUL 2>&1";
#else
    c += " >/dev/null 2>&1";
#endif
    return system(c.c_str()) == 0;
}

// One-shot STARTUP readiness check: report what the APK-signing toolchain has now, and what will be auto-fetched
// on first use. Cheap (a couple of quiet `--version` probes + a filesystem scan); doesn't download anything.
inline void reportToolchain(FILE* out = stderr) {
    namespace fs = std::filesystem; std::error_code ec;
#ifdef _WIN32
    bool curl = toolPresent("curl.exe");
#else
    bool curl = toolPresent("curl");
#endif
    bool java = toolPresent("java", "-version");   // keytool ships in the same bin dir, so java is the proxy
    const char* btEnv = std::getenv("HSR_BUILDTOOLS");
    std::string bt = btEnv ? std::string(btEnv) : AppConfig::detectBuildTools();
    if (bt.empty()) bt = findToolDirUnder(AppConfig::exeRel("android-build-tools"), "apksigner");  // already downloaded?
    std::string jdl = java ? "" : findToolDirUnder(AppConfig::exeRel("jre"), "keytool");           // downloaded JRE?

    fprintf(out, "[TOOLS] APK signing toolchain (cook / --sign):\n");
    fprintf(out, "  curl ......... %s\n", curl ? "ok (auto-download available)" : "MISSING (auto-download disabled)");
    if (!bt.empty())     fprintf(out, "  build-tools .. ok: %s\n", bt.c_str());
    else                 fprintf(out, "  build-tools .. none%s\n", curl ? " -> auto-download (~58MB) on first sign" : " -> install Android SDK or set HSR_BUILDTOOLS (no curl)");
    if (java)            fprintf(out, "  java/keytool . on PATH\n");
    else if (!jdl.empty()) fprintf(out, "  java/keytool . ok: %s (downloaded)\n", fs::path(jdl).parent_path().string().c_str());
    else                 fprintf(out, "  java/keytool . none%s\n", curl ? " -> auto-download a JRE on first sign" : " -> install a JDK (no curl)");
    bool ready = !bt.empty() && (java || !jdl.empty());
    fprintf(out, "  status ....... %s\n", ready ? "READY" : (curl ? "will auto-provision on first sign" : "NOT READY (need curl, or install the SDK/JDK)"));
}

// AUTO-SIGN a cooked APK: zipalign + apksigner with a debug keystore -> an INSTALLABLE signed APK. PORTABLE (no
// hardcoded SDK path): build-tools are AUTO-DETECTED (AppConfig::detectBuildTools scans ANDROID_HOME / ANDROID_SDK_ROOT
// / LOCALAPPDATA / ~/Android/Sdk / common dirs), and the keystore is AUTO-GENERATED with keytool if missing (so a fresh
// clone on any machine signs with zero setup — keytool ships with the JDK that apksigner needs anyway). Cross-platform
// (Windows .bat/.exe + backslashes; POSIX apksigner/zipalign). Override via HSR_BUILDTOOLS / HSR_KEYSTORE.
inline bool signApk(const std::string& unsignedApk, const std::string& signedApk,
                    const std::function<void(float,const char*)>& progress = {}) {
    auto prog = [&](float f, const char* s){ if (progress) progress(f, s); };
    namespace fs = std::filesystem;
    std::string BT = ensureBuildTools(progress);                  // installed SDK, or AUTO-DOWNLOAD beside the exe
    if (BT.empty()) { prog(1.0f, "no Android build-tools (auto-download failed - need curl + a network connection, or set HSR_BUILDTOOLS)"); return false; }
    std::string JH = ensureJava(progress);                        // "" = java already on PATH; else a downloaded JRE
    if (!JH.empty()) {                                            // route apksigner + keytool at the fetched JRE
        std::string p = std::getenv("PATH") ? std::getenv("PATH") : "";
#ifdef _WIN32
        _putenv_s("JAVA_HOME", JH.c_str()); _putenv_s("PATH", (JH + "\\bin;" + p).c_str());
#else
        setenv("JAVA_HOME", JH.c_str(), 1); setenv("PATH", (JH + "/bin:" + p).c_str(), 1);
#endif
    }
    const char* ksEnv = std::getenv("HSR_KEYSTORE");
    std::error_code ec;
    std::string KS;
    if (ksEnv) KS = ksEnv;
    else {   // prefer an existing keystore: beside the exe, then cooker/, then cwd
        for (const std::string& c : { AppConfig::exeRel("debug.keystore"), std::string("cooker/debug.keystore"), std::string("debug.keystore") })
            if (fs::exists(c, ec)) { KS = c; break; }
        if (KS.empty()) KS = AppConfig::exeRel("debug.keystore");   // none found -> auto-generate it beside the exe
    }
    if (!fs::exists(KS, ec)) {                                 // no keystore on this machine -> make a throwaway debug one
        prog(0.90f, "Generating debug keystore");
        if (!fs::path(KS).parent_path().empty()) fs::create_directories(fs::path(KS).parent_path(), ec);
        std::string kt = "keytool -genkeypair -keystore \"" + KS + "\" -alias myhome -storepass android -keypass android"
                         " -keyalg RSA -keysize 2048 -validity 36500 -dname \"CN=HSR Debug\"";
        if (system(kt.c_str()) != 0 || !fs::exists(KS, ec)) { prog(1.0f, "keystore gen failed (need JDK keytool on PATH)"); return false; }
    }
#ifdef _WIN32
    auto bs = [](std::string p){ for (char& c : p) if (c == '/') c = '\\'; return p; };
    std::string ZA = bs(BT) + "\\zipalign.exe", AS = bs(BT) + "\\apksigner.bat";
    std::string U = bs(unsignedApk), S = bs(signedApk), A = S + ".aligned", K = bs(KS);
    char cmd[1600];
    // Windows system() runs `cmd /c <str>` which strips ONE outer quote pair; wrap the WHOLE command in an extra pair.
    prog(0.92f, "Zipalign");
    snprintf(cmd, sizeof cmd, "\"\"%s\" -f -p 4 \"%s\" \"%s\"\"", ZA.c_str(), U.c_str(), A.c_str());
    if (system(cmd) != 0) { remove(A.c_str()); prog(1.0f, "Zipalign failed"); return false; }
    prog(0.96f, "apksigner");
    snprintf(cmd, sizeof cmd, "\"\"%s\" sign --ks \"%s\" --ks-pass pass:android --key-pass pass:android --ks-key-alias myhome --out \"%s\" \"%s\"\"",
             AS.c_str(), K.c_str(), S.c_str(), A.c_str());
    int rc = system(cmd);
#else
    std::string ZA = BT + "/zipalign", AS = BT + "/apksigner";
    std::string U = unsignedApk, S = signedApk, A = S + ".aligned", K = KS;
    char cmd[1600];
    prog(0.92f, "Zipalign");
    snprintf(cmd, sizeof cmd, "\"%s\" -f -p 4 \"%s\" \"%s\"", ZA.c_str(), U.c_str(), A.c_str());
    if (system(cmd) != 0) { remove(A.c_str()); prog(1.0f, "Zipalign failed"); return false; }
    prog(0.96f, "apksigner");
    snprintf(cmd, sizeof cmd, "\"%s\" sign --ks \"%s\" --ks-pass pass:android --key-pass pass:android --ks-key-alias myhome --out \"%s\" \"%s\"",
             AS.c_str(), K.c_str(), S.c_str(), A.c_str());
    int rc = system(cmd);
#endif
    remove(A.c_str()); remove((signedApk + ".idsig").c_str());
    prog(1.0f, rc == 0 ? "Signed OK" : "Sign failed");   // ASCII only (the editor font atlas is Latin-1)
    return rc == 0;
}

// Port ANY decoded scene (e.g. a V79 .ovrscene loaded by the renderer) to a bootable V203 APK: per-mesh RENDMESH
// (struct-bounds, embedded material) + RENDTXTR + the floor MATL template patched to that mesh's texture, all bound
// to the real renderer_module unlit.surface shader; Root entity + child relationships + spawn point.
inline std::vector<uint8_t> exportSceneAPK(const std::vector<ExportMesh>& meshes, const std::string& nuxdApk,
                                           const std::vector<uint8_t>& vspv, const std::vector<uint8_t>& fspv,
                                           bool locomotion = true, bool* ok = nullptr, const float* camPos = nullptr,
                                           std::vector<uint8_t>* outSceneZip = nullptr,
                                           const std::vector<uint8_t>& bgOgg = {},
                                           const std::function<void(float,const char*)>& progress = {},
                                           const std::vector<sitem::Item>& sceneItems = {},
                                           const std::string& editorSession = {}) {   // .hsledit text -> embedded in the APK so an ORPHAN cook re-opens with its scene items
    auto prog = [&](float f, const char* s){ if (progress) progress(f, s); };
    prog(0.02f, "Preparing scene");
    if (ok) *ok = false;
    (void)vspv; (void)fspv;
    CookRng rng;
    std::vector<CookAsset> assets;
    // ⛔ CACHE-BUST: the device's AssetManager caches parsed assets by AssetKey (= hash of the asset PATH) and serves the
    //    cached copy across env reloads. Re-cooking with the SAME "meta/myhome/..." paths -> SAME keys -> the headset
    //    keeps showing the PREVIOUS cook's materials/content.hstf (device-proven: a pose-anim re-cook still rendered the
    //    old brightness-pulse wisps). A UNIQUE per-cook namespace forces fresh keys = a real reload every cook.
    //    HSR_COOK_NS pins it (e.g. for diffing); default = "myhome_<time>" so every cook busts the cache.
    // The scene.zip's PACKAGE dir matches stock haven2025: `content/meta/home_c25/...`. decomposePath() takes the
    // baseDir = the FIRST TWO path segments, so MH = "meta/home_c25/<token>" => baseDir = "meta/home_c25" (== stock,
    // resolves the same way via the field2 string->hash map) while the per-cook <token> lands in the RELPATH. That keeps
    // the cache-bust (re-cooks get fresh relPath keys = a real reload) WITHOUT moving the package off home_c25. The
    // rest of the assets are just relinked under it. HSR_COOK_NS pins the <token> (e.g. for diffing).
    std::string nsName;
    if (const char* e = std::getenv("HSR_COOK_NS")) nsName = e;
    else { char tb[32]; snprintf(tb, sizeof tb, "%llx", (unsigned long long)time(nullptr)); nsName = tb; }
    const std::string MH = "meta/home_c25/" + nsName;   // baseDir = meta/home_c25 (stock); <token>/asset = the relinked relPath
    // Bundle the V203 render system's OWN shaders + materials: opaque -> unlit.surface (floor MATL), transparent ->
    // unlitblend.surface (dome MATL). V79 glTF envs are textured PBR (no custom GLSL), so these faithfully port the
    // shading AND fix the "black where transparent" bug (blend meshes were drawn opaque).
    // HSR_DEPTHCLAMP: ship the DEPTH-CLAMP unlit/unlitblend shaders (gl_Position.z clamped to [0,w]) so geometry past the
    // shell's hard 5000 far-clip plane RENDERS instead of being culled — the UNROOTED, cooked-APK fix for the far dome
    // (no Frida/system patch). Near geometry is unaffected (clamp is a no-op when z is already in range). [[project_hsr_eye_subcamera_farclip]]
    bool dclamp = std::getenv("HSR_DEPTHCLAMP") != nullptr;
    bool fit = std::getenv("HSR_FITCLIP") != nullptr;
    // FAR-ONLY DEPTH-REMAP (HSR_DEPTHCLAMP): the GLOBAL remap broke near shading (it faked EVERY mesh's depth, desyncing the
    // shell's depth-consuming passes — SSAO/fog/motion). FIX = per-mesh routing: near geometry keeps the NORMAL shaders
    // (byte-exact depth -> correct shading); ONLY meshes whose farthest vertex exceeds the 5000 clip use the REMAP variant
    // (infinite reversed-Z z=near/w) so they render past the clip, ordered among themselves; their depth-effects are wrong
    // only in the far distance (invisible). camPos = the spawn. [[project_hsr_eye_subcamera_farclip]]
    auto shad      = readFileBytes("cooker/nuxd_unlit_shader.bin");        // NORMAL (near) — byte-exact
    auto shadBlend = readFileBytes("cooker/nuxd_unlitblend_shader.bin");
    auto shadDC      = dclamp ? readFileBytes("cooker/nuxd_unlit_depthclamp.bin")      : std::vector<uint8_t>{};   // REMAP (far only)
    auto shadBlendDC = dclamp ? readFileBytes("cooker/nuxd_unlitblend_depthclamp.bin") : std::vector<uint8_t>{};
    float farMargin = 4500.f; if (const char* fm = std::getenv("HSR_FARCLIP_R")) { float v=(float)atof(fm); if (v>1.f) farMargin=v; }
    if (dclamp) fprintf(stderr, "[COOK] FAR-ONLY DEPTH-REMAP ON: meshes beyond %.0f from spawn use the remap shader; near byte-exact\n", farMargin);
    auto matTpl    = readFileBytes("cooker/realfloor_mat.bin");   // unlit.surface MATL template
    auto matBlend  = readFileBytes("cooker/realdome_mat.bin");    // unlitblend.surface MATL template
    // ADDITIVE MATL = realdome (blend) with the blend-MODE field (MATL field2 @off44: opaque=absent, alpha-blend=2)
    // set to 3 = ADDITIVE (dst=ONE). The DEVICE blend equation comes from the MATL blend-mode field, NOT the shader
    // (proven: flipbookadditive.surface == unlitblend.surface forward-pass byte-for-byte). The old "opaque-pass shader
    // in transparent MATL" trick only yields ALPHA-blend on device -> the lake foam's black-bg texture showed as a dark
    // card (user: "full dark foam thingy"). A true additive MATL makes black add 0 = transparent overlay, white = foam.
    auto matAdd    = readFileBytes("cooker/realadditive_mat.bin");
    bool haveAdd   = matAdd.size() >= 176;
    auto shadVat   = readFileBytes("cooker/vat_shader.bin");      // vatunlitbasecolor (vertex-animation) shader — OPAQUE (fish/foliage)
    auto shadVatBlend = readFileBytes("cooker/vatunlitblend.surface.bin"); // vatunlitbasecolor made TRANSPARENT alpha-blend (cooker/make_vat_blend_shader.py): the faithful V79 wisp port (alphaMode=BLEND sparkle). Identical VAT vertex morph + descriptor layout as vatunlitbasecolor; only pass render-state fields f2,f3 are dropped to match the SHIPPED vatlitbubble's proven transparent state. ⛔ ALSO dropping f4 (doubleSided) = a combo NO shipped shader has → broke the pipeline → INVISIBLE sparkles on device (device-proven). Kept single-sided; the wisp billboards face the player.
    auto matVat    = readFileBytes("cooker/vat_mat.bin");         // VAT MATL template (416B, shader@48/56/64 + base@152/160 + VAT@192/200)
    // (flipbook shaders are auto-generated per-grid via shadergen::FLIPBOOK from the unlitblend base, below)
    auto shadSkin  = readFileBytes("cooker/unlitdoublesidedskinned.bin");  // OPAQUE skinned (butterflies) — for opaque skinned meshes (droid body). matParams = uvScaleOffset.
    auto shadSkinB = readFileBytes("cooker/unlitblendskinned.bin");        // BLEND skinned (Nuxd renderer_module) — alpha-blends transparent skinned meshes (the omnidroid SHIELD force-field). SAME matParams layout (uvScaleOffset) as unlitdoublesidedskinned, so the SAME field-7 material template works; only the shader ref differs.
    auto matSkin   = readFileBytes("cooker/skinned_mat.bin");     // OLD motes MATL template (176B, NO field7 — fails device skinned path)
    auto matSkin2  = readFileBytes("cooker/skinned_mat_v2.bin");  // CURRENT-format skinned MATL (butterflies: field7=shader ref @48, tex ing@128). The device's skinned render path reads the shader from MATL FIELD 7; the old motes material has only 6 fields (no field7) so it read garbage -> ShaderAsset/TextureAsset MISSING -> ErrorNotReady -> invisible droid.
    // BLEND skinned material = skinned_mat_v2 + a PROPER field2=2 (transparent-pass flag), generated in code so no extra
    // asset file is needed. The butterflies template (root@32, vtable@10) has a 24-byte shader-ref struct @48..72 and
    // field0 (u16=2) @74, leaving 2 free padding bytes @72-73: write u16=2 there (table voffset 40) and point field2's
    // vtable slot (@18) at voffset 40 -> a non-overlapping field2=2. This makes BLEND skinned meshes (the omnidroid
    // SHIELD) sort into the device's ALPHA-BLEND transparent pass with proper depth (was opaque-pass -> "no depth, the
    // rest overlays it, only visible against black"). Ref offsets (@48/56 shader, @120/128 tex) are unchanged. Official
    // transparent skinned mats (central donut_shield, calming waterfalls) confirm the field set [0,2,3,5,7,8].
    std::vector<uint8_t> matSkinB = matSkin2;
    if (matSkinB.size() >= 76) { matSkinB[72] = 2; matSkinB[73] = 0; matSkinB[18] = 40; matSkinB[19] = 0; }
    auto refSkinMesh = readFileBytes("cooker/ref_skinned_mesh.bin"); // Meta-shipped skinned RENDMESH = cook-verify schema oracle
    auto refStaticMesh = readFileBytes("cooker/ref_static_mesh.bin"); // Meta-shipped STATIC RENDMESH = cook-verify oracle for non-skinned meshes (optional; drop in to verify static meshes too)
    if (shad.empty() || matTpl.size() < 176) return {};
    AssetKey3 shaderK = { 0x608B25CE5424598Dull, 0x78686234E7611EFCull, 0xA1767FE9u };
    assets.push_back({ "meta/renderer_module/shaders/unlit.surface/shader", shaderK.tgt, shad, shaderK });
    bool haveBlend = !shadBlend.empty() && matBlend.size() >= 176;
    if (haveBlend) {
        AssetKey3 shaderBlendK = { 0x608B25CE5424598Dull, 0xFBFB67B966D4BA47ull, 0xA1767FE9u };
        assets.push_back({ "meta/renderer_module/shaders/unlitblend.surface/shader", shaderBlendK.tgt, shadBlend, shaderBlendK });
    }
    // CUTOUT (alpha-test discard) shader — generated from unlit via cooker/make_cutout_shader.py: injects an OpKill on
    // texel alpha<0.5 into the unlit FORWARD-frag SPIR-V (spirv-dis -> add discard -> spirv-as spv1.0 -> spirv-val).
    // DEPTH-WRITES (inherits unlit's f2,f3 render-state) so cutouts OCCLUDE correctly + discard transparent texels —
    // instead of the no-depth-write blend pass that made the lakeside MASK scenery (campsite *_dblsided_masked) overlay
    // everything + flash. Reuses the unlit MATL (matTpl); shipped env-local. [[project_hsr_cook_2d_anim_depth_flash]]
    // SHADERGEN the cutout from the stock unlit base (in-binary alpha-test discard via shadergen::CUTOUT — no NDK
    // spirv tools, no pre-baked .bin, no haveCutout/CWD fragility). Faithful to Meta's unlitfoliage (decompiled): the
    // forward frag gets `if (baseColor.a < 0.5) discard;` while KEEPING unlit's depth-write -> the card OCCLUDES (no
    // depth issues) + drops its transparent silhouette to the sky (no black background). Falls back to a file if needed.
    auto shadCutout = (!shad.empty()) ? shadergen::generate(shad, shadergen::CUTOUT, 0.5f) : std::vector<uint8_t>();
    if (shadCutout.empty()) shadCutout = readFileBytes("cooker/nuxd_unlitcutout_shader.bin");
    bool haveCutout = !shadCutout.empty() && matTpl.size() >= 176;
    AssetKey3 shaderCutoutK{};
    if (haveCutout) {
        std::string pCut = MH + "/shaders/unlitcutout.surface/shader"; shaderCutoutK = keyForPath(pCut);
        assets.push_back({ pCut, shaderCutoutK.tgt, shadCutout, shaderCutoutK });
    }
    // TREE-LINE cutout variant: a LOW discard threshold (0.12) so SPARSE tree cards (Hill/LakeTrees BG: ~20% solid +
    // soft foliage, ~68% transparent) keep their foliage AND depth-write. At the standard 0.5 the soft foliage (alpha<0.5)
    // was all discarded -> the BG trees rendered sparse ("don't render") and the gaps showed what's behind = depth issue.
    auto shadCutoutTree = (!shad.empty()) ? shadergen::generate(shad, shadergen::CUTOUT, 0.12f) : std::vector<uint8_t>();
    bool haveCutoutTree = !shadCutoutTree.empty() && matTpl.size() >= 176;
    AssetKey3 shaderCutoutTreeK{};
    if (haveCutoutTree) {
        std::string pCutT = MH + "/shaders/unlitcutouttree.surface/shader"; shaderCutoutTreeK = keyForPath(pCutT);
        assets.push_back({ pCutT, shaderCutoutTreeK.tgt, shadCutoutTree, shaderCutoutTreeK });
    }
    // FAR-ONLY remap shader variants — shipped under the env's OWN namespace; far meshes (below) repoint their material's
    // field7 shader ref to these so ONLY they get the infinite-projection remap (near meshes keep unlit/unlitblend = exact).
    AssetKey3 shaderDCK{}, shaderBlendDCK{};
    if (dclamp && !shadDC.empty()) {
        std::string pDC = MH + "/shaders/unlit_dc.surface/shader"; shaderDCK = keyForPath(pDC);
        assets.push_back({ pDC, shaderDCK.tgt, shadDC, shaderDCK });
        if (haveBlend && !shadBlendDC.empty()) {
            std::string pBDC = MH + "/shaders/unlitblend_dc.surface/shader"; shaderBlendDCK = keyForPath(pBDC);
            assets.push_back({ pBDC, shaderBlendDCK.tgt, shadBlendDC, shaderBlendDCK });
        }
    }
    bool haveVat = !shadVat.empty() && matVat.size() >= 416;
    if (haveVat) {   // ship the shared VAT shader at its AssetRef
        AssetKey3 shaderVatK = { 0x91897C97D84A4317ull, 0xFB22F5F00E5164C0ull, 0xA1767FE9u };
        assets.push_back({ "meta/horizon_shared_shaders/shaders/vat/vatunlitbasecolor.surface/shader", shaderVatK.tgt, shadVat, shaderVatK });
    }
    // TRANSPARENT+DOUBLE-SIDED VAT shader (the V79 BLEND wisp port) shipped under the env's OWN namespace at a
    // keyForPath AssetRef (same pattern as wisppulse/flipbook). Blend node-anim meshes (erebor's flame wisps =
    // alphaMode BLEND) repoint their VAT material's shader ref @48/56/64 to this so the texture alpha FADES.
    bool haveVatBlend = haveVat && !shadVatBlend.empty();
    std::string pVatBlend = MH + "/shaders/vatunlitblend.surface/shader";
    AssetKey3 vatBlendK = keyForPath(pVatBlend);
    if (haveVatBlend) assets.push_back({ pVatBlend, vatBlendK.tgt, shadVatBlend, vatBlendK });
    // FAITHFUL V79 wisp animation: unlitblend RENDSHAD with a getTime()-driven SCALE BREATHE injected into the forward
    // VERTEX stage (cooker/make_wisp_scale_shader.py). The V79 wisps (Plane.001..012) are a 100-key node SCALE
    // oscillation (~4.17s loop, z compresses more) — GEOMETRY scale, not a brightness/alpha pulse. The shader scales
    // inPos in MODEL space (so the cooker centers the geometry + puts the entity at the centroid → pivots in place),
    // loops off globalUniforms.time, and phases per-wisp off worldFromModel.translation.x. VAT didn't render on device
    // and ShellPoseAnimation is a one-shot; this getTime() vertex shader loops + is the real scale motion.
    auto shadPulse = readFileBytes("cooker/wispscale.surface.bin");
    bool havePulse = !shadPulse.empty() && haveBlend;
    std::string pPulse = MH + "/shaders/wispscale.surface/shader";
    AssetKey3 shaderPulseK = keyForPath(pPulse);
    if (havePulse) assets.push_back({ pPulse, shaderPulseK.tgt, shadPulse, shaderPulseK });
    bool haveSkin = !shadSkin.empty() && matSkin2.size() >= 176;   // require the CURRENT-format skinned material
    AssetKey3 shaderSkinK{}, shaderSkinBK{};
    if (haveSkin) {   // ship our skinned shaders ENV-LOCAL (the device resolves the env's own package; renderer_module copies
                      // are shadowed by the system module). The material's field7 ref is patched (below) to the exact key,
                      // so it resolves to our shipped shader. Ship BOTH the opaque (unlitdoublesidedskinned) and the BLEND
                      // (unlitblendskinned) skinned shaders; blend skinned meshes (the shield) route to the blend one.
        std::string pSkinShader = MH + "/shaders/unlitdoublesidedskinned.surface/shader";
        shaderSkinK = keyForPath(pSkinShader);
        assets.push_back({ pSkinShader, shaderSkinK.tgt, shadSkin, shaderSkinK });
        if (!shadSkinB.empty()) {
            std::string pSkinShaderB = MH + "/shaders/unlitblendskinned.surface/shader";
            shaderSkinBK = keyForPath(pSkinShaderB);
            assets.push_back({ pSkinShaderB, shaderSkinBK.tgt, shadSkinB, shaderSkinBK });
        }
    }
    // FLIPBOOK shader (animated flat material, no skeleton): ship env-local when any mesh is a flipbook.
    // (Flipbook shaders are now AUTO-GENERATED per grid/fps in the per-mesh loop via shadergen::FLIPBOOK from the
    // unlitblend base — no static 3x3 flipbook.surface asset is shipped anymore.)
    std::vector<uint8_t> whiteTex; { std::vector<uint8_t> w4((size_t)4*4*4, 255); whiteTex = encodeRendTxtr(w4.data(), 4, 4, 8, 8); }
    std::string pWhite = MH + "/white.tex/tex"; AssetKey3 whiteK = keyForPath(pWhite); bool whiteUsed = false; bool anySkybox = false;
    // dedup skel + anim INDEPENDENTLY: the appliances share ONE animation clip but each has its OWN bind skeleton.
    // The old (skel,anim)-PAIR dedup re-emitted the identical 286KB anim once PER mesh -> N duplicate large async
    // loads -> device asset-loader RACE -> std::length_error crash-loop. Sharing the anim asset (1 copy, referenced
    // by every animator, like nuxd's prism_wave+motes) removes the duplicates.
    struct AssetPoolEntry { std::vector<uint8_t> bytes; AssetKey3 key; };
    std::vector<AssetPoolEntry> skelPool, animPool;
    std::string rootId = makeUuid(rng);
    std::string entities = rootEntityJson(rootId), rels;
    // Far-clip-plane extension (ScenePlatformComponent v3) is emitted into space.hstf below — NOT here in content.hstf.
    // (Vista-proven: the shell only applies it from space.hstf. See spaceJson + the HSR_FARCLIP read at the spaceJson call.)
    float pos0[3]={0,0,0}, rot0[3]={0,0,0}, scl1[3]={1,1,1};
    const char* navE = getenv("HSR_NAVMESH"); int navIdx = navE ? atoi(navE) : -1;   // base navmesh on one mesh, else whole scene
    float smn[3]={1e30f,1e30f,1e30f}, smx[3]={-1e30f,-1e30f,-1e30f};
    // Split >60000-vert STATIC meshes into separate single-part meshes (multi-part static doesn't render on device).
    std::vector<ExportMesh> meshesV = splitLargeStaticMeshes(meshes);
    // Split >60000-vert SKINNED meshes the SAME way (multi-part skinned CRASHES the device — snakeway 5_Car dragon).
    meshesV = splitLargeSkinnedMeshes(meshesV);
    // ── HSR_FITCLIP: scale EVERY mesh under the shell's fixed far-clip plane (default R=4500, margin under 5000) ──
    // Per-mesh UNIFORM scale ABOUT THE SPAWN: each vertex keeps its DIRECTION (angular position) and the mesh keeps its
    // apparent size (size/distance) from the spawn -> looks identical, textures untouched, internal depth scales with it
    // (so the mesh keeps its OWN depth ordering = no z-fight overlay). Geometry op -> covers ANY shader incl. animated.
    // Only meshes whose farthest vertex exceeds R are touched; everything already inside is left EXACT. [[project_hsr_eye_subcamera_farclip]]
    if (fit) {
        float MAXR = 4500.f; if (const char* fr = std::getenv("HSR_FITCLIP_R")) { MAXR = (float)atof(fr); if (MAXR < 1.f) MAXR = 4500.f; }
        float o[3] = { camPos?camPos[0]:0.f, camPos?camPos[1]:1.6f, camPos?camPos[2]:0.f };
        int nfit = 0;
        for (size_t mi = 0; mi < meshesV.size(); ++mi) {
            ExportMesh& m = meshesV[mi];
            const std::vector<float>& gp = (!m.hzRestPos.empty() && m.hzRestPos.size()==m.positions.size()) ? m.hzRestPos : m.positions;
            float maxD2 = 0.f;
            for (size_t v=0; v+2<gp.size(); v+=3){ float dx=gp[v]-o[0],dy=gp[v+1]-o[1],dz=gp[v+2]-o[2]; float d=dx*dx+dy*dy+dz*dz; if(d>maxD2)maxD2=d; }
            float maxD = sqrtf(maxD2);
            if (maxD <= MAXR) continue;                 // already inside the clip -> exact, untouched
            float f = MAXR / maxD;
            auto scaleAbout = [&](std::vector<float>& P){ for (size_t v=0; v+2<P.size(); v+=3) for(int k=0;k<3;k++) P[v+k]=o[k]+(P[v+k]-o[k])*f; };
            scaleAbout(m.positions);
            if (!m.hzRestPos.empty())  scaleAbout(m.hzRestPos);
            if (!m.hzJointPos.empty()) scaleAbout(m.hzJointPos);
            for (int k=0;k<3;k++){ m.rotPivot[k]=o[k]+(m.rotPivot[k]-o[k])*f; m.poseCentroid[k]=o[k]+(m.poseCentroid[k]-o[k])*f; m.poseTransDelta[k]*=f; }
            nfit++;
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' FITCLIP d=%.0f->%.0f f=%.3f (under far clip; apparent size preserved)\n", mi, m.name.c_str(), maxD, MAXR, f);
        }
        fprintf(stderr, "[COOK] FITCLIP: scaled %d mesh(es) under R=%.0f about spawn (%.1f,%.1f,%.1f) — nothing exceeds the far clip\n", nfit, MAXR, o[0],o[1],o[2]);
    }
    std::unordered_map<uint64_t, std::pair<AssetKey3, std::string>> texCache;  // rgba hash -> shared texture (split chunks share one)
    std::unordered_map<std::string, AssetKey3> rotShaders;  // rot shader filename -> shipped key (one per distinct period/dir Y-spin)
    for (size_t i = 0; i < meshesV.size(); ++i) {
        if (progress && (i % 8 == 0 || i + 1 == meshesV.size())) { char sb[64]; snprintf(sb, sizeof sb, "Cooking mesh %zu/%zu", i+1, meshesV.size()); prog(0.05f + 0.70f*(float)i/(float)meshesV.size(), sb); }
        const ExportMesh& m = meshesV[i];
        if (m.positions.size() < 9 || m.indices.size() < 3) continue;   // skip empty
        // GENERIC depth-write discriminator (V79 MeshShellEnvMaskDepthWrite + HAS_ALPHACUTOFF): a HARD-EDGED / BIMODAL
        // transparent card — pixels are ~all opaque or fully transparent (midF~0) with real opaque content — must OCCLUDE.
        // Cook it with alpha-cutoff DISCARD + DEPTH-WRITE on WHICHEVER path the mesh takes (static OR skinned/rigid-hzanim:
        // the moving train card). Soft-gradient transparents (fog/smoke/glow/aurora/ice: midF high) stay alpha BLEND.
        // Texture-driven, no material names. Computed once so every path (below) agrees.
        bool bimodalOccluder = false;
        if (m.blend && !m.additive && m.rgba.size() >= 4) {
            size_t nt=m.rgba.size()/4, opq=0, mid=0;
            for (size_t k=3;k<m.rgba.size();k+=4){ uint8_t a=m.rgba[k]; if(a>=230)opq++; if(a>=40&&a<=215)mid++; }
            float opF = nt?(float)opq/(float)nt:0.f, midF = nt?(float)mid/(float)nt:1.f;
            bimodalOccluder = (midF < 0.05f) && (opF > 0.004f);
        }
        // FAR-ONLY remap routing (HSR_DEPTHCLAMP): does this mesh poke past the 5000 clip from the spawn? If so it gets the
        // remap shader (renders past the clip); near meshes stay byte-exact. camPos = spawn.
        bool meshFar = false;
        if (dclamp) {
            float o[3] = { camPos?camPos[0]:0.f, camPos?camPos[1]:1.6f, camPos?camPos[2]:0.f };
            float md2 = 0.f;
            for (size_t v=0; v+2<m.positions.size(); v+=3){ float dx=m.positions[v]-o[0],dy=m.positions[v+1]-o[1],dz=m.positions[v+2]-o[2]; float d=dx*dx+dy*dy+dz*dz; if(d>md2)md2=d; }
            meshFar = sqrtf(md2) > farMargin;
            if (meshFar && std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' FAR (>%.0f) -> remap shader\n", i, m.name.c_str(), farMargin);
        }
        { // skip DEGENERATE meshes: all verts at one point (0-area). For a SKINNED mesh (e.g. the omnidroid's Shield) the
          // bind/rest pose is real geometry and only the ANIMATED rest collapses (Shield joint scale 0->1 pop); the cooked
          // skinned mesh uses hzRestPos (un-collapsed bind), so test THAT, not m.positions. Only a truly 0-area mesh
          // (even at rest) is dropped. (The old check dropped the Shield purely because its m.positions = animated bind.)
          bool willSkin = std::getenv("HSR_HZANIM") && m.hzFrames > 1 && m.hzRestPos.size() == m.positions.size();
          const std::vector<float>& dp = willSkin ? m.hzRestPos : m.positions;
          float dmn[3]={1e30f,1e30f,1e30f}, dmx[3]={-1e30f,-1e30f,-1e30f};
          for (size_t v=0; v+2<dp.size(); v+=3) for(int k=0;k<3;k++){ float p=dp[v+k]; if(p<dmn[k])dmn[k]=p; if(p>dmx[k])dmx[k]=p; }
          if (dmx[0]-dmn[0]<1e-3f && dmx[1]-dmn[1]<1e-3f && dmx[2]-dmn[2]<1e-3f) { fprintf(stderr,"[COOK] skip degenerate mesh m%03zu '%s' (0-area)\n", i, m.name.c_str()); continue; } }
        // navmesh bounds: explicit mesh (HSR_NAVMESH), else substantial GROUND geometry only — skip the skybox and
        // the billboard-planets (few verts, far away) so the ground sits ON the terrain, not hundreds of m below it.
        size_t mnv = m.positions.size() / 3;
        // Only OPAQUE ground/walls define the playable bounds (scale/spawn). Skip skyboxes AND transparent EFFECT meshes
        // (warp tunnel / nebula "Glow" / sliding screens are blend) — Star Trek's 286-unit-long warp glow was inflating the
        // bounds to ext=286 -> gs=47.7 -> the whole bridge cooked tiny (and the fog/everything shrank with it).
        bool navCand = (navIdx >= 0) ? ((int)i == navIdx)
                     : (mnv >= 100 && !m.blend && m.name.find("sky") == std::string::npos && m.name.find("Sky") == std::string::npos);
        if (navCand) for (size_t v = 0; v + 2 < m.positions.size(); v += 3) for (int k = 0; k < 3; k++) { float p = m.positions[v + k]; if (p < smn[k]) smn[k] = p; if (p > smx[k]) smx[k] = p; }
        // Embed the SOURCE mesh name (m.name, e.g. "VE_WESTERN...fbx.comet_alpha.mat") into the cook asset id so
        // the on-device AssetRef name is SEARCHABLE — findmat/findmesh/envdump then show "m005_comet_alpha" instead
        // of an opaque hash/"m005". Keep the m%03zu prefix for uniqueness/ordering. HSR_NONAMETAG = plain m%03zu.
        char base[160];
        if (std::getenv("HSR_NONAMETAG")) { snprintf(base, sizeof base, "%s/m%03zu", MH.c_str(), i); }
        else {
            std::string src = m.name; size_t fp = src.rfind(".fbx.");            // strip the common "…fbx." prefix
            const char* nm = (fp != std::string::npos) ? src.c_str() + fp + 5
                           : (src.rfind('/') != std::string::npos ? src.c_str() + src.rfind('/') + 1 : src.c_str());
            char tag[32]; int t = 0; for (const char* c = nm; *c && t < (int)sizeof tag - 1; ++c) { char ch = *c;
                tag[t++] = ((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')) ? ch : '_'; }
            tag[t] = 0;
            snprintf(base, sizeof base, "%s/m%03zu_%s", MH.c_str(), i, tag);
        }
        std::string pMesh = std::string(base) + ".rendmesh/mesh", pMat = std::string(base) + ".material/material",
                    pTex = std::string(base) + ".tex/tex";
        AssetKey3 meshK = keyForPath(pMesh), matK = keyForPath(pMat);
        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK-ID] m%03zu '%s' assetId=%016llx%016llx\n", i, m.name.c_str(), (unsigned long long)meshK.ing, (unsigned long long)meshK.pkg);
        AssetKey3 texK{}; std::vector<uint8_t> tex;   // value-init -> tgt==0 means "no texture yet" (used by the white-fallback check)
        // An OPAQUE skinned mesh (e.g. the Incredibles droid: source alphaMode=OPAQUE) is drawn through the
        // unlitblendskinned BLEND shader, so its texture's UV-atlas-mask alpha (alpha 0 outside the islands) would
        // make it transparent. The source ignores that alpha (opaque); cook the texture OPAQUE to match.
        // The droid's texture is a UV-ATLAS MASK: alpha=0 outside the UV islands. An opaque-source skinned mesh that
        // ends up on an alpha-respecting material then renders TRANSPARENT (invisible) wherever alpha=0. Force the texture
        // OPAQUE for ANY opaque skinned-source mesh — NOT gated behind HSR_HZANIM (the static cook needs it too; that gate
        // was THE bug that made the static droid invisible on-device while the sim's alpha-ignoring fallback still showed it).
        bool skinnedOpaque = haveSkin && m.hzJointCount > 0 && !m.blend;
        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' skinnedOpaque=%d blend=%d hzJoints=%d haveSkin=%d w=%d h=%d rgba=%zu\n", i, m.name.c_str(), skinnedOpaque, (int)m.blend, m.hzJointCount, (int)haveSkin, m.w, m.h, m.rgba.size());
        std::vector<uint8_t> texOpaque; const uint8_t* texSrc = m.rgba.data();
        if (skinnedOpaque && !m.alphaTest && m.rgba.size() >= (size_t)m.w * m.h * 4) {
            // (MASKED skinned foliage keeps its REAL alpha — the skinned-CUTOUT shader below discards on it.
            //  Forcing alpha=255 here rendered the stinson materialsplit trees' transparent leaf gaps SOLID
            //  = "tree leaves in cook are all blocky".)
            texOpaque = m.rgba; for (size_t k = 3; k < texOpaque.size(); k += 4) texOpaque[k] = 255; texSrc = texOpaque.data();
        }
        else if (m.additive && m.uvScroll) {
            // FOAM (lake_foam Additive:true + mat.sanim): a BRIGHT-rgb foam texture with a LOW NATIVE ALPHA (the alpha IS
            // the subtle foam coverage). Use the texture VERBATIM (no premultiply, no luminance) on the PROVEN alpha-blend
            // path -> the device alpha-blends base·alpha over the lake = the faithful subtle translucent white foam. (Both
            // the additive-MATL enum trick = dark sheet, AND alpha=luminance = an opaque white sheet, were wrong.)
            texSrc = m.rgba.data();
        }
        else if (m.additive && m.rgba.size() >= (size_t)m.w * m.h * 4) {
            // ADDITIVE glow → ALPHA conversion (the storybook godRays "still black" fix). GROUND TRUTH from the
            // official V205 files: the additive flipbook .surface forward pass is BYTE-IDENTICAL in state to the
            // OPAQUE one, and the official flame(additive)/mist(alpha) MATLs both carry field2=2 — so there is NO
            // independent additive pipeline the cook can target, and the old "opaque-pass shader in transparent
            // MATL" trick just renders UNBLENDED on device: a texture that stores its glow in RGB over BLACK with
            // a UNIFORM alpha (godRays texA=255 everywhere; lake foam) drew its black background as a DARK SHEET
            // over the bright scene. Port additive faithfully-visible instead: alpha := max(r,g,b) so black adds
            // nothing (transparent) and the bright rays show, rgb UNTOUCHED (no premult double-attenuation), on
            // the DEVICE-PROVEN unlitblend path (shader routing below). Textures that already carry a real alpha
            // falloff (Star Trek warp: soft edge in alpha, non-uniform) keep alpha' = a·max(rgb) — same formula,
            // their authored falloff still multiplies in. Cache key (rh below) hashes texSrc, so this variant
            // dedups correctly and never collides with a straight tex.
            texOpaque = m.rgba;
            for (size_t k = 0; k + 3 < texOpaque.size(); k += 4) {
                uint32_t lum = texOpaque[k+0]; if (texOpaque[k+1] > lum) lum = texOpaque[k+1]; if (texOpaque[k+2] > lum) lum = texOpaque[k+2];
                texOpaque[k+3] = (uint8_t)(lum * texOpaque[k+3] / 255);   // a' = max(rgb)·a  (uniform-alpha glows -> pure luminance mask)
            }
            texSrc = texOpaque.data();
        }
        bool droidWhite = std::getenv("HSR_DROIDWHITE") && m.hzJointCount > 0;   // diag: force the droid to the white fallback tex (isolate texture vs mesh)
        // SPRITESHEET FLIPBOOK grid (smoke/fire animated cards): derive cols×rows so the encoder caps the mip chain per
        // cell (no cross-cell averaging -> no device "borders around the animated cards"). Matrix-replay steps the UV
        // OFFSET by 1/cols (2x2 identity); the grid-flipbook path stores cols/rows.
        int capCols = 1, capRows = 1;
        if (m.flipbook) {
            if (m.flipUVMats.size() >= 12) {
                float du = std::fabs(m.flipUVMats[6+2]-m.flipUVMats[2]), dv = std::fabs(m.flipUVMats[6+5]-m.flipUVMats[5]);
                if (du > 1e-4f) capCols = (int)(1.0f/du + 0.5f);
                if (dv > 1e-4f) capRows = (int)(1.0f/dv + 0.5f);
            } else { if (m.flipCols > 1) capCols = m.flipCols; if (m.flipRows > 1) capRows = m.flipRows; }
            if (capCols < 1) capCols = 1; if (capRows < 1) capRows = 1;
            if (capCols > 64) capCols = 1; if (capRows > 64) capRows = 1;   // a huge "grid" = a fine continuous SCROLL, not a real spritesheet -> don't mip-cap (was mis-capping the aurora as 278x1)
        }
        if (m.rgba.size() >= (size_t)m.w * m.h * 4 && m.w && m.h && !droidWhite) {
            uint64_t rh = murmur64A(texSrc, (size_t)m.w * m.h * 4);   // dedup identical textures (esp. the chunks a big mesh was split into)
            rh ^= (uint64_t)(uint32_t)(capCols*73856093 ^ capRows*19349663);   // spritesheet cap is part of the encoded chain -> distinct entry
            auto cit = texCache.find(rh);
            if (cit != texCache.end()) { texK = cit->second.first; pTex = cit->second.second; }  // reuse the shared texture; leave `tex` empty so it isn't re-encoded/re-pushed
            else { texK = keyForPath(pTex);
                  // ── LOSSLESS PASS-THROUGH (fixes "textures lost definition") ──────────────────────────────────
                  // If the base texture is UNMODIFIED (texSrc still points at the raw decoded m.rgba — NOT the
                  // force-255 / luminance-alpha variants) and we kept its ORIGINAL ASTC mip chain, ship those blocks
                  // VERBATIM instead of decode→re-encode (double ASTC at MEDIUM softens every texture). Byte-exact
                  // source quality + faster (no encode) + the ORIGINAL Meta mips (authoritative). Skip for
                  // spritesheets (need the per-cell mip cap). HSR_NOTEXPASS forces re-encode.
                  bool unmodified = (texSrc == m.rgba.data());
                  bool canPass = unmodified && !m.srcAstc.empty() && capCols==1 && capRows==1
                               && !std::getenv("HSR_NOTEXPASS")
                               && ((m.srcAstcBw==8&&m.srcAstcBh==8)||(m.srcAstcBw==6&&m.srcAstcBh==6)||(m.srcAstcBw==12&&m.srcAstcBh==12));
                  if (canPass) tex = encodeRendTxtrFromAstc(m.srcAstc, (int)m.w, (int)m.h, (int)m.srcAstcBw, (int)m.srcAstcBh, (int)m.srcAstcMips);
                  if (tex.empty()) {   // pass-through N/A or unsupported footprint -> re-encode
                      // alpha-test foliage (masked trees/leaves): alpha-weight the mip RGB (no transparent-bg bleed halo) AND
                      // preserve alpha COVERAGE down the chain, so the leaves don't dissolve into blocky ASTC chunks at range.
                      bool aw = m.blend || m.alphaTest; bool cov = m.alphaTest && !m.blend;
                      tex = encodeRendTxtr(texSrc, (int)m.w, (int)m.h, 8, 8, 11, aw, capCols, capRows, cov);
                  } else if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' TEX PASS-THROUGH (source ASTC %dx%d %ux mips, lossless)\n", i, m.name.c_str(), (int)m.srcAstcBw, (int)m.srcAstcBh, m.srcAstcMips);
                  texCache[rh] = { texK, pTex };
                  if ((capCols>1||capRows>1) && std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' SPRITESHEET %dx%d -> mip-capped\n", i, m.name.c_str(), capCols, capRows); }
        }
        if (tex.empty() && texK.tgt == 0) { texK = whiteK; whiteUsed = true; }     // share the white fallback (only when there was NO texture at all, not for a dedup hit)
        // ANIMATED (VAT) mesh -> bake the offset texture + the vatunlitbasecolor MATL (base tex + VAT tex); else
        // transparent -> unlitblend MATL, opaque -> floor MATL. Patch each to THIS mesh's texture(s).
        int vc = (int)(m.positions.size() / 3);
        // HZANIM (skinned animation) gated behind HSR_HZANIM. The earlier "large skinned meshes crash the verifier"
        // theory was WRONG (a 924-byte mesh failed too): the real cause was the vertexStream.f0/f4 + part.f5 8-byte
        // hash structs being emitted 12B/align-4 when V205.2's NEW MeshDefinition verifier requires 8B/align-8
        // (IDA @0xeba93c). Fixed in encodeRendMeshParts -> no size cap needed. HSR_HZMAXVERTS still overrides if set.
        int hzMaxVerts = std::getenv("HSR_HZMAXVERTS") ? atoi(std::getenv("HSR_HZMAXVERTS")) : 0x7FFFFFFF;
        // Clip byte budget: NONE by default — FULL PORT, every source frame ships untouched (user directive).
        // ⚠ The old 20000-byte default was FEAR FROM A MISDIAGNOSIS: the lakesidepeak `std::length_error` was NEVER
        // clip size — it was the missing 5-byte trailing channel-name table (IDA-proven @0x1617f4c, fixed in
        // hzAclEncode; "never a malformed clip, data-dependent"). The old "signed-16 size field" theory is disproven
        // by GROUND TRUTH: the official oceanarium whale ships 240,520-byte HzAnim clips. The 20 KB ceiling was
        // silently DESTROYING big-rig anims: cyberhome's screens (153 joints x 578 frames) got subsampled
        // 578 -> 17 frames = 0.83 fps = the "2D anims not ported" slideshow. HSR_HZMAXBYTES opts back into a budget
        // (the subsample loop below then reduces the frame count, DURATION preserved, until it fits).
        size_t hzMaxBytes = SIZE_MAX;
        if (const char* eb = std::getenv("HSR_HZMAXBYTES")) { long v = atol(eb); if (v > 0) hzMaxBytes = (size_t)v; }
        std::vector<uint8_t> hzAnimPre; bool hzClipFits = true;
        if (std::getenv("HSR_HZANIM") && haveSkin && m.hzFrames > 1 && m.hzJointCount > 0
            && m.hzTrsLocal.size() >= (size_t)m.hzFrames*m.hzJointCount*10) {
            // SKELETON BOUNDS ANCHOR (skinned-mesh CULL fix, V205): V205 culls SKINNED meshes by their SKELETON's animated
            // joint extent, NOT the cooked mesh AABB — device-proven (the scene-spanning HSR_NOCULL AABB does NOTHING for
            // skinned meshes: the omnidroid/cyberhome cars still vanish on a small rotate once their animated parts exceed the
            // tight skeleton bound). FIX = append a FAR "anchor" joint (child of root, +1e5 on X) to the skeleton + clip so the
            // skeleton bounds span the scene → the frustum/occlusion cull can never drop it (the skin-path analogue of the
            // HSR_NOCULL mesh bounds). The anchor weights NO vertex (it's absent from the palette/f2/markers), so the skinning
            // and the device MeshDefinition verifier are unaffected. HSR_NOSKELANCHOR disables it.
            // SKINNED CULL FIX — DEVICE-PROVEN, PURE COOK (works UNROOTED): the device builds a skinned mesh's
            // cull bound from its JOINT POSITIONS (not vertex extents — that's why every ±1e5-vertex attempt failed).
            // Append 6 STATIC anchor joints at the ±C axis extremes (child of root): the joint-position AABB then spans
            // ±C SYMMETRICALLY around the mesh, enclosing the player, so the frustum/occlusion cull can never drop it
            // from ANY angle. (A single far anchor extended the bound but only one way -> still clipped from the back;
            // 6 symmetric anchors fix it.) Each anchor gets one bounds vertex (see encodeRendMeshParts) so it's counted
            // in the per-joint loop. C is under the device's 150000 far-clip exclusion even after the ~1.5x root xform.
            const int   nAnch = std::getenv("HSR_NOSKELANCHOR") ? 0 : 6;   // HSR_NOSKELANCHOR disables
            const float AC    = 50000.f;
            const float anchOff[6][3] = { {AC,0,0},{-AC,0,0},{0,AC,0},{0,-AC,0},{0,0,AC},{0,0,-AC} };
            const int   njReal  = m.hzJointCount;
            const int   nj0  = njReal + nAnch;              // joint count INCLUDING the 6 bounds anchors — skel + clip BOTH use this
            // REPEAT loop seam (GROUND TRUTH = the renderer's OPA/glTF playback, opa_loader.h sampleNodeTRS:
            // `f = fmod(t*fps, nFrames); i1 = (i0+1<nFrames)?i0+1:0` — ALL N frames are distinct and frame[N-1]
            // INTERPOLATES back to frame[0] over one frame-step = REPEAT). The device's ACL clip is inclusive
            // [0,(N-1)/fps], so a plain N-frame clip ends AT frame[N-1] and the device's loop wraps frame[N-1]->frame[0]
            // INSTANTLY (no interpolation step) = the "backward fast" snap the user saw. FIX: append a copy of frame 0 as
            // the final clip frame so the device interpolates frame[N-1]->frame[0] over a real frame-step = smooth REPEAT,
            // exactly like the renderer. HSR_NOLOOPWRAP disables.
            const int   nfSrc   = m.hzFrames;
            // How far does the clip END drift from its START? (max per-joint local translation delta.) Large => a
            // ONE-WAY motion (car/train fly-across); ~0 => an in-place spin/door that returns to its start.
            float endDrift = 0.f;
            if (nfSrc > 1 && (int)m.hzTrsLocal.size() >= nfSrc*njReal*10) {
                const float* d0 = &m.hzTrsLocal[0];
                const float* dL = &m.hzTrsLocal[(size_t)(nfSrc-1)*njReal*10];
                for (int j=0;j<njReal;j++) for (int c=4;c<7;c++){ float d=d0[j*10+c]-dL[j*10+c]; if(d<0)d=-d; if(d>endDrift)endDrift=d; }
            }
            // The REPEAT seam (append a copy of frame 0 so the device INTERPOLATES the wrap) is smooth ONLY when
            // end~=start: the interpolated wrap is then imperceptible. For a ONE-WAY motion (end drifts far) that
            // same interpolation is a VISIBLE multi-frame BACKWARD SLIDE on the 90fps device ("speed backward
            // quickly", train/cars) — whereas a plain clip (no seam) wraps end->start in a SINGLE frame = instant
            // teleport reset = continuous traffic, matching the desktop renderer's near-instant 1-frame fmod wrap
            // (it lerps frame[N-1]->frame[0] over one frame-step, ~33ms = 1-2 frames at 30-60fps = a blink). So:
            // seam for in-place spins/doors, teleport for one-way fly-across. Threshold 5 units cleanly splits
            // (appliances/doors ~0.18 vs cars/train ~336). HSR_FORCELOOPWRAP overrides (always seam).
            const bool  oneWay  = endDrift > 5.0f && !std::getenv("HSR_FORCELOOPWRAP");
            const bool  loopWrap = !std::getenv("HSR_NOLOOPWRAP") && nfSrc > 1 && !oneWay;
            const int   nf0  = nfSrc + (loopWrap ? 1 : 0);  // +1 = appended copy of frame 0 (the REPEAT seam step)
            const float fps0 = m.hzFps;
            // clip duration (seconds) — preserved across every subsample so playback speed never changes.
            const float dur  = (fps0 > 1e-6f && nf0 > 1) ? (float)(nf0 - 1) / fps0 : 0.f;
            std::vector<int> parentsA(m.hzParents.begin(), m.hzParents.begin() + njReal);
            for (int k = 0; k < nAnch; ++k) parentsA.push_back(0);   // each anchor parented to root joint 0
            std::vector<float> trs0((size_t)nf0 * nj0 * 10);   // FULL-res source clip (+REPEAT seam frame), augmented with the 6 static FAR anchor tracks
            for (int f = 0; f < nf0; ++f) {
                int src = (loopWrap && f == nf0 - 1) ? 0 : f;   // appended final frame == frame 0 -> the device interpolates the REPEAT wrap smoothly
                std::memcpy(&trs0[(size_t)f*nj0*10], &m.hzTrsLocal[(size_t)src*njReal*10], (size_t)njReal*10*sizeof(float));
                for (int k = 0; k < nAnch; ++k) { float* a = &trs0[((size_t)f*nj0 + njReal + k)*10];   // cook layout per joint = {qx,qy,qz,qw, t3, s3}
                    a[0]=0;a[1]=0;a[2]=0;a[3]=1;                       // identity quat (xyzw)
                    a[4]=anchOff[k][0];a[5]=anchOff[k][1];a[6]=anchOff[k][2];   // FAR static translation = the k-th ±C axis extreme
                    a[7]=1;a[8]=1;a[9]=1; }                            // unit scale
            }
            // LOOPDBG: how far does the source END from its START? (root + max joint local-pos delta). Large => the
            // source is a ONE-WAY motion (fly-across), so REPEAT inherently zips back; small => seamless circuit.
            if (std::getenv("HSR_VERBOSE") && nfSrc > 1 && (int)m.hzTrsLocal.size() >= nfSrc*njReal*10) {
                const float* f0 = &m.hzTrsLocal[0];
                const float* fL = &m.hzTrsLocal[(size_t)(nfSrc-1)*njReal*10];
                fprintf(stderr,"[LOOPDBG] '%s' nfSrc=%d root0=(%.2f,%.2f,%.2f) rootLast=(%.2f,%.2f,%.2f) endDrift=%.3f oneWay=%d loop=%s\n",
                        m.name.c_str(), nfSrc, f0[4],f0[5],f0[6], fL[4],fL[5],fL[6], endDrift, (int)oneWay,
                        oneWay ? "TELEPORT" : (loopWrap ? "SEAM" : "none"));
            }
            // ── ACL SHELL DISTANCE = the farthest vertex from its DOMINANT joint in JOINT-LOCAL units (the cyan
            //    "animations very shaky" fix). ACL evaluates its rotation-error budget AT shell_distance; the old
            //    fixed 1.0 let rigs with ~0.01 node scale (verts 100-500 joint-local units out) wobble by
            //    centimeters at the real geometry. Compose each REAL joint's bind world from the SKEL data, take
            //    max |inv(bindWorld[dominantBone]) · restVert| over the mesh. Anchors excluded (constant tracks).
            float shellDist = 1.f;
            {
                std::vector<std::array<float,16>> bw((size_t)njReal);
                for (int j2 = 0; j2 < njReal; ++j2) {
                    float qw=m.hzJointQuat[j2*4], qx=m.hzJointQuat[j2*4+1], qy=m.hzJointQuat[j2*4+2], qz=m.hzJointQuat[j2*4+3];
                    float s = m.hzJointScale[j2]; if (s < 1e-6f && s > -1e-6f) s = 1.f;
                    float lx=m.hzJointPos[j2*3], ly=m.hzJointPos[j2*3+1], lz=m.hzJointPos[j2*3+2];
                    float l[16] = { (1-2*(qy*qy+qz*qz))*s, (2*(qx*qy+qw*qz))*s, (2*(qx*qz-qw*qy))*s, 0,
                                    (2*(qx*qy-qw*qz))*s, (1-2*(qx*qx+qz*qz))*s, (2*(qy*qz+qw*qx))*s, 0,
                                    (2*(qx*qz+qw*qy))*s, (2*(qy*qz-qw*qx))*s, (1-2*(qx*qx+qy*qy))*s, 0,
                                    lx, ly, lz, 1 };
                    int p = m.hzParents[j2];
                    if (p >= 0 && p < j2) { const float* a = bw[(size_t)p].data(); float* o = bw[(size_t)j2].data();
                        for (int c2=0;c2<4;c2++) for (int r2=0;r2<4;r2++) o[c2*4+r2] = a[r2]*l[c2*4] + a[4+r2]*l[c2*4+1] + a[8+r2]*l[c2*4+2] + a[12+r2]*l[c2*4+3];
                    } else std::memcpy(bw[(size_t)j2].data(), l, sizeof l);
                }
                auto invLever = [&](const float* mtx, float x, float y, float z)->float {
                    float M00=mtx[0],M01=mtx[4],M02=mtx[8], M10=mtx[1],M11=mtx[5],M12=mtx[9], M20=mtx[2],M21=mtx[6],M22=mtx[10];
                    float det=M00*(M11*M22-M12*M21)-M01*(M10*M22-M12*M20)+M02*(M10*M21-M11*M20);
                    float id=(det>1e-20f||det<-1e-20f)?1.f/det:0.f;
                    float i00=(M11*M22-M12*M21)*id, i01=(M02*M21-M01*M22)*id, i02=(M01*M12-M02*M11)*id;
                    float i10=(M12*M20-M10*M22)*id, i11=(M00*M22-M02*M20)*id, i12=(M02*M10-M00*M12)*id;
                    float i20=(M10*M21-M11*M20)*id, i21=(M01*M20-M00*M21)*id, i22=(M00*M11-M01*M10)*id;
                    float tx=x-mtx[12], ty=y-mtx[13], tz=z-mtx[14];
                    float px=i00*tx+i01*ty+i02*tz, py=i10*tx+i11*ty+i12*tz, pz=i20*tx+i21*ty+i22*tz;
                    return std::sqrt(px*px+py*py+pz*pz); };
                size_t nvv = m.hzRestPos.size()/3;
                for (size_t v2 = 0; v2 < nvv; ++v2) {
                    int b = (v2*4 < m.hzBoneIdx.size()) ? (int)m.hzBoneIdx[v2*4] : 0; if (b < 0 || b >= njReal) b = 0;
                    float lv = invLever(bw[(size_t)b].data(), m.hzRestPos[v2*3], m.hzRestPos[v2*3+1], m.hzRestPos[v2*3+2]);
                    if (lv > shellDist) shellDist = lv;
                }
                if (shellDist > 100000.f) shellDist = 100000.f;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu ACL shellDist=%.1f (joint-local max vertex lever)\n", i, shellDist);
            }
            int    nf   = nf0;
            float  fps  = fps0;
            std::vector<float> trsCur = trs0;               // current candidate clip (starts = full clip)
            hzAnimPre = hzAclEncode(trsCur.data(), parentsA.data(), nj0, nf, fps, shellDist);
            // Reduce N (~3/4 each step, floor 8) until the encoded clip fits the byte budget. trsCur[(f*nj+j)*10 .. +10)
            // = {qx,qy,qz,qw, t3, s3} — sample N evenly-spaced source frames (endpoints inclusive) into a tight buffer.
            while ((hzAnimPre.empty() || hzAnimPre.size() > hzMaxBytes) && nf > 8 && dur > 0.f) {
                int newNf = (int)(nf * 3 / 4); if (newNf < 8) newNf = 8; if (newNf >= nf) newNf = nf - 1;
                trsCur.assign((size_t)newNf * nj0 * 10, 0.f);
                for (int f = 0; f < newNf; ++f) {
                    // map output frame f -> source frame index (inclusive endpoints): f*(nf0-1)/(newNf-1)
                    int sf = (newNf > 1) ? (int)((double)f * (nf0 - 1) / (newNf - 1) + 0.5) : 0;
                    if (sf < 0) sf = 0; if (sf > nf0 - 1) sf = nf0 - 1;
                    std::memcpy(trsCur.data() + (size_t)f * nj0 * 10,
                                trs0.data()    + (size_t)sf * nj0 * 10,
                                (size_t)nj0 * 10 * sizeof(float));
                }
                nf  = newNf;
                fps = (newNf > 1) ? (float)(newNf - 1) / dur : fps0;   // DURATION-preserving fps (no #3 warp-speed bug)
                hzAnimPre = hzAclEncode(trsCur.data(), parentsA.data(), nj0, nf, fps, shellDist);
            }
            if (hzAnimPre.empty() || hzAnimPre.size() > hzMaxBytes) hzClipFits = false;   // even 8 frames won't fit -> static
            // The subsampled fps lives INSIDE the re-encoded ACL clip (hzAclEncode's fps arg), so hzAnimPre alone carries
            // the slower-fps-but-same-duration motion downstream — m is const & the emit block reads only the bind pose
            // (m.hzJointPos/Quat/Scale, unchanged) + this hzAnimPre, never m.hzFrames/m.hzFps after here. Nothing to mutate.
            if (std::getenv("HSR_VERBOSE"))
                fprintf(stderr, "[COOK] m%03zu HZANIM clip %zuB (j=%d f=%d->%d fps=%.2f->%.2f dur=%.2fs budget=%zu) %s\n",
                        i, hzAnimPre.size(), nj0, nf0, nf, fps0, fps, dur, hzMaxBytes,
                        hzClipFits ? (nf != nf0 ? "OK (subsampled)" : "OK") : "-> STATIC (8 frames over budget)");
            // ── ACL ROUND-TRIP VERIFY (does the DEVICE's clip preserve the motion?): decode the ENCODED clip (exactly
            //    what ships to device) and print the mover joint's LOCAL translation vs the INPUT. If the decoded sweep
            //    is flat/clamped while the input sweeps -> ACL lost it (device plays a frozen comet). HSR_ACLVERIFY. ──
            if ((std::getenv("HSR_ACLVERIFY") || std::getenv("HSR_VERBOSE")) && !hzAnimPre.empty() && njReal >= 1) {
                int mj = njReal - 1;   // the MOVING joint (last real joint; anchors follow it)
                const float* inp = trsCur.data();  // the clip actually encoded
                float in0x = inp[((size_t)0*nj0 + mj)*10+4], in0y = inp[((size_t)0*nj0 + mj)*10+5], in0z = inp[((size_t)0*nj0 + mj)*10+6];
                float inLx = inp[((size_t)(nf-1)*nj0 + mj)*10+4], inLy = inp[((size_t)(nf-1)*nj0 + mj)*10+5], inLz = inp[((size_t)(nf-1)*nj0 + mj)*10+6];
                float inMax=0.f; for (int f=0;f<nf;f++){ for(int c=4;c<7;c++){ float v=std::fabs(inp[((size_t)f*nj0+mj)*10+c]); if(v>inMax)inMax=v; } }
                HzAclClip* vclip = hzAclCreate(hzAnimPre.data(), hzAnimPre.size());
                fprintf(stderr, "[ACL-VERIFY] m%03zu '%s' mover=j%d  INPUT: t0=(%.1f,%.1f,%.1f) tLast=(%.1f,%.1f,%.1f) maxAbs=%.1f\n",
                        i, m.name.c_str(), mj, in0x,in0y,in0z, inLx,inLy,inLz, inMax);
                if (vclip) { float vd = hzAclDuration(vclip); float decMax=0.f;
                    static float out[256*8];
                    fprintf(stderr, "    [CLIP] duration=%.3fs sampleRate=%.1ffps  => FULL DECODED PATH (device plays this):\n", vd, hzAclSampleRate(vclip));
                    for (int s=0;s<=12;s++){ float t = (vd>0?vd*(float)s/12.f:0.f); int n = hzAclSampleLocal(vclip, t, out, 256);
                        if (mj<n){ for(int c=4;c<7;c++){ float v=std::fabs(out[mj*8+c]); if(v>decMax)decMax=v; }
                            fprintf(stderr, "    t=%.2fs (%.1f,%.1f,%.1f) sc=%.4f\n", t, out[mj*8+4],out[mj*8+5],out[mj*8+6], out[mj*8+7]); } }
                    { int n0 = hzAclSampleLocal(vclip, 0.f, out, 256);   // scale roundtrip: INPUT trsLocal s vs decoded (koi = 25x blowup when ACL drops it)
                      for (int j2=0; j2<m.hzJointCount && j2<n0; ++j2){ float inS=m.hzTrsLocal[(size_t)j2*10+7], deS=out[j2*8+7];
                          if (std::fabs(inS-deS) > 0.01f*std::fabs(inS)+1e-4f)
                              fprintf(stderr, "    [SCALE-MISMATCH] j%d input=%.4f decoded=%.4f\n", j2, inS, deS); } }
                    fprintf(stderr, "    => INPUT maxAbs=%.1f vs DECODED maxAbs=%.1f  %s\n", inMax, decMax,
                            (inMax>10.f && decMax < inMax*0.5f) ? "⛔ ACL LOST THE MOTION (device comet FROZEN)" : "ok (motion preserved)");
                    hzAclDestroy(vclip);
                } else fprintf(stderr, "    [ACL-VERIFY] decode FAILED (clip invalid)\n");
            }
        }
        bool useHz  = std::getenv("HSR_HZANIM") && haveSkin && m.hzFrames > 1 && m.hzJointCount > 0
                    && m.hzBoneIdx.size() >= (size_t)vc*4 && m.hzBoneWgt.size() >= (size_t)vc*4
                    && m.hzTrsLocal.size() >= (size_t)m.hzFrames*m.hzJointCount*10
                    && m.hzRestPos.size() == m.positions.size()   // centered rest positions present
                    && hzClipFits                                 // clip fits the device signed-16 size field (else static)
                    && vc <= hzMaxVerts;                          // no longer a verifier guard; only the optional HSR_HZMAXVERTS override
        if (std::getenv("HSR_VERBOSE") && haveSkin && m.hzJointCount>0)
            fprintf(stderr, "[COOK] m%03zu useHz=%d HZANIM=%d frames=%d joints=%d boneIdx=%zu(need%d) boneWgt=%zu(need%d) trs=%zu(need%zu) rest=%zu(pos%zu)\n",
                i, (int)useHz, std::getenv("HSR_HZANIM")?1:0, m.hzFrames, m.hzJointCount,
                m.hzBoneIdx.size(), vc*4, m.hzBoneWgt.size(), vc*4,
                m.hzTrsLocal.size(), (size_t)m.hzFrames*m.hzJointCount*10,
                m.hzRestPos.size(), m.positions.size());
        bool useRot = !useHz && m.rotAnim && ((m.rotOmega > 1e-5f || m.rotOmega < -1e-5f) || (m.rotOsc && (m.rotAmp > 0.02f || m.rotAmp < -0.02f)) || (m.rotReplay && m.rotReplayN >= 2 && (int)m.rotQuats.size() == (m.rotReplayN+1)*4));   // node spin OR sway OR per-frame QUATERNION REPLAY (aurora) -> getTime() vertex shader
        bool useTrans = !useHz && !useRot && m.transAnim && m.transN >= 2 && m.transFrames.size() >= (size_t)m.transN*3;   // GENERAL node-translation replay (sliding screens)
        bool useScale = !useHz && !useRot && !useTrans && m.scaleAnim && m.scaleN >= 2 && m.scaleFrames.size() >= (size_t)m.scaleN*3;   // GENERAL node-SCALE breathe replay (Erebor wisps, per-axis)
        bool useUvScroll = !useHz && !useRot && !useTrans && !useScale && m.uvScroll && (m.uvRate[0]!=0.f || m.uvRate[1]!=0.f);   // mat.sanim UV scroll (water/foam) -> getTime() uv-translate shader
        bool usePulse = !useHz && !useRot && !useTrans && !useScale && !useUvScroll && m.pulse && havePulse;   // node-animated billboard -> custom getTime() pulse shader
        bool useVat = !useHz && !useRot && !useTrans && !useScale && !useUvScroll && !usePulse && !m.vatOffsets.empty() && m.vatFrames > 1 && haveVat && m.vatOffsets.size() >= (size_t)vc * m.vatFrames * 3;
        std::vector<uint8_t> vatTex; AssetKey3 vatTexK;
        if (useVat) { vatTex = encodeVatTexture(m.vatOffsets, vc, m.vatFrames); if (vatTex.empty()) useVat = false; }
        // MASKED FOLIAGE → ALPHA-BLEND (device anti-aliasing): a hard alpha-test `discard` has NO edge AA on the
        // Quest (MSAA smooths geometry edges, not per-fragment discard — you'd need alpha-to-coverage, which the
        // shadergen CUTOUT shader doesn't set), so every cutout leaf/flower renders JAGGED/BLOCKY on device while
        // the DESKTOP preview looks fine because it resolves with AA (this hid the bug through many "fixed in
        // preview" iterations). The stinson zen tree + bougainvillea + palms are all masked skinned foliage → route
        // them to the BLEND skinned pipeline (unlitblendskinned + transparent MATL) so the leaf-mask alpha feathers
        // the edges smoothly on device. HSR_FOLIAGE_CUTOUT restores the old hard-discard path.
        bool foliageBlend = useHz && m.alphaTest && !m.blend && !m.additive && !std::getenv("HSR_FOLIAGE_CUTOUT");
        std::vector<uint8_t> matl; std::string animatorComp;
        if (useHz) {   // HZANIM: CURRENT-format skinned MATL (field7=shader) + HZAN:SKEL + ACL HZAN:ANIM + AnimatorPlatformComponent
            // BLEND skinned (the SHIELD) -> the transparent-flagged material (field2=2) so it alpha-blends in the
            // transparent pass; OPAQUE skinned (body) -> the plain butterflies template.
            matl = std::getenv("HSR_HZMATTPL") ? matTpl : ((m.blend || foliageBlend) && matSkinB.size() >= 176 ? matSkinB : matSkin2);
            // (Moving/skinned transparent cards e.g. the TRAIN stay ALPHA-BLEND — a hard alpha-cutoff cutout here
            //  destroyed the train's soft transparency. Depth-write for MOVING cards is intentionally NOT forced.)
            if (!std::getenv("HSR_HZMATTPL") && matl.size() >= 140) {
                // field7 shader ref @48(pkg)/@56(ing)/@64(tgt) -> our env-local skinned shader; tgt @64 stays 0xA1767FE9 (murmur3"shader").
                // BLEND skinned meshes (the omnidroid SHIELD = alphaMode BLEND) use unlitBLENDskinned so the device alpha-blends
                // them (transparent force-field); OPAQUE skinned (droid body) use unlitdoublesidedskinned. Same matParams layout.
                // ADDITIVE skinned glow (the warp-streak tunnel): use the OPAQUE-pass skinned shader (unlitdoublesidedskinned,
                // f2,f3 PRESENT) but keep the TRANSPARENT skinned MATL (matSkinB, field2=2) -> ADDITIVE blend = bright streaks
                // on the dark viewscreen (alpha-blend made them near-invisible). Non-emissive blend skinned (a real force-field
                // glass) still uses unlitBLENDskinned (alpha).
                // ADDITIVE skinned GLOW (stinson city color_beam searchlights, Additive:true): the OLD "opaque shader
                // + transparent MATL" trick renders OPAQUE on device — unlitdoublesidedskinned forces alpha=1, so the
                // alpha-blend MATL just composites the full (city-atlas) texture as a solid cyan/pink BLOB ("huge, no
                // transparent, off place"). Use the BLEND skinned shader (unlitblendskinned, passes texture alpha)
                // together with the luminance-alpha texture the additive branch already bakes (alpha = max(rgb)·a) →
                // the bright beam feathers in over the sky and the dark atlas background drops out = a real glow.
                AssetKey3 sk = ((m.blend || foliageBlend || m.additive) && (shaderSkinBK.pkg || shaderSkinBK.ing)) ? shaderSkinBK : shaderSkinK;
                // MASKED skinned foliage (stinson materialsplit trees: authored AlphaTest, "_masked") -> a skinned
                // CUTOUT shader: the opaque skinned base + an alpha<0.5 discard in the forward frag (same
                // shadergen::CUTOUT edit the static path uses — it only touches the FRAGMENT, skinning untouched).
                // Without it the opaque skinned pipeline drew the leaf textures' transparent gaps SOLID ("blocky").
                // The texture keeps its REAL alpha for this path (see the skinnedOpaque force-255 exemption above).
                if (m.alphaTest && !m.blend && !foliageBlend && !shadSkin.empty()) {
                    // A masked skin may ALSO carry a mat.sanim MaterialTint RGBA cycle (stinson zen tree:
                    // 8 skinned canopy layers pulse green→cyan→pink over 35s). Chain TINTREPLAY onto the
                    // CUTOUT frag so the skinned leaves recolor on-device (it only edits the fragment; the
                    // vertex SKINNING is untouched). Cache key folds the tint curve so each distinct cycle
                    // dedups. No tint -> the shared plain skincutout (unchanged for the palm/materialsplit trees).
                    bool sctint = m.tintAnim && m.tintN >= 2 && m.tintFrames.size() >= (size_t)m.tintN*4 && m.tintLoop > 1e-4f;
                    char cfn[80];
                    if (sctint) { uint32_t th = 0x811C9DC5u ^ (uint32_t)(long)(m.tintLoop*1000);
                        for (float v : m.tintFrames) th = th*16777619u ^ (uint32_t)(long)(v*10000);
                        snprintf(cfn, sizeof cfn, "cooker/skincutout_t%08x.surface.bin", th); }
                    else snprintf(cfn, sizeof cfn, "cooker/skincutout.surface.bin");
                    AssetKey3 skinCutK{};
                    auto itc0 = rotShaders.find(cfn);
                    if (itc0 != rotShaders.end()) skinCutK = itc0->second;
                    else {
                        auto cb = readFileBytes(cfn);
                        if (cb.empty()) { cb = shadergen::generate(shadSkin, shadergen::CUTOUT, 0.5f);
                                          if (!cb.empty() && sctint) { auto tb = shadergen::generate(cb, shadergen::TINTREPLAY, m.tintLoop, 0,0,0,0, m.tintFrames, m.tintN); if (!tb.empty()) cb = tb; }
                                          if (!cb.empty()) writeFileBytes(cfn, cb); }
                        if (!cb.empty()) {
                            // asset path = the cache filename's basename (e.g. "skincutout_t1a2b3c4d.surface"),
                            // so the tinted variant is a distinct on-device shader asset from the plain one.
                            std::string base(cfn+7); size_t dot = base.rfind(".bin"); if (dot!=std::string::npos) base = base.substr(0,dot);
                            char cp2[200]; snprintf(cp2, sizeof cp2, "%s/shaders/%s/shader", MH.c_str(), base.c_str());
                            skinCutK = keyForPath(cp2); assets.push_back({ cp2, skinCutK.tgt, cb, skinCutK }); rotShaders[cfn] = skinCutK; }
                    }
                    if (skinCutK.pkg || skinCutK.ing) { sk = skinCutK;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' SKINNED-CUTOUT%s (masked foliage, discard a<0.5)\n", i, m.name.c_str(), sctint?"+TINTREPLAY":""); }
                }
                // ── COMBINED effect card (RIGID-HZANIM + material UV/fade) — NO EXCLUSION ──────────────────────────
                // A fog/dust card whose node has R/S rides a RIGID-HZANIM skeleton (faithful T+R+S) AND has a UV flipbook
                // + opacity fade. Bake the UV-matrix replay + fade into a PER-MESH SKINNED shader variant: FLIPBOOK mode
                // edits ONLY the fragment (base-color UV + alpha), so the vertex SKINNING is untouched. Result: the mesh
                // deforms via the skeleton (node motion) AND its texture cycles + fades on the same skinned pipeline.
                bool skFlip = !std::getenv("HSR_NOHZUV") && m.flipbook && m.flipN>=2 && m.flipUVMats.size()>=(size_t)m.flipN*6;
                if (skFlip) {
                    const std::vector<uint8_t>& sbase = (m.blend && !shadSkinB.empty()) ? shadSkinB : shadSkin;
                    bool skFade = m.fadeAnim && m.fadeN>=2 && m.fadeFrames.size()>=(size_t)m.fadeN && m.fadeLoop>1e-4f;
                    uint32_t hh = 0x811C9DC5u ^ (uint32_t)(long)(m.flipLoop*1000);
                    for (size_t k=0;k<m.flipUVMats.size();++k) hh = hh*16777619u ^ (uint32_t)(long)(m.flipUVMats[k]*100000);
                    if (skFade) { hh = hh*16777619u ^ (uint32_t)(long)(m.fadeLoop*1000);
                        for (size_t k=0;k<m.fadeFrames.size();++k) hh = hh*16777619u ^ (uint32_t)(long)(m.fadeFrames[k]*100000); }
                    char sfn[168]; snprintf(sfn,sizeof sfn,"cooker/skinuv_%08x%s.surface.bin", hh, m.blend?"_b":"");
                    auto itc = rotShaders.find(sfn);
                    if (itc != rotShaders.end()) sk = itc->second;
                    else {
                        auto sb = readFileBytes(sfn);
                        if (sb.empty() && !sbase.empty()) { static const std::vector<float> noFade;
                            sb = shadergen::generate(sbase, shadergen::FLIPBOOK, m.flipLoop, 0.f,0.f,1.f,0.f, m.flipUVMats, m.flipN,
                                                     skFade ? m.fadeFrames : noFade, skFade ? m.fadeLoop : 0.f);
                            if (!sb.empty()) writeFileBytes(sfn, sb); }
                        if (!sb.empty()) { char sp[184]; snprintf(sp,sizeof sp,"%s/shaders/skinuv_%08x%s.surface/shader", MH.c_str(), hh, m.blend?"_b":"");
                            sk = keyForPath(sp); assets.push_back({ sp, sk.tgt, sb, sk }); rotShaders[sfn] = sk;
                            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' RIGID-HZANIM + SKINNED-UV%s shader (combined T+R+S + UV + fade)\n", i, m.name.c_str(), skFade?"+FADE":""); } }
                }
                memcpy(matl.data() + 48, &sk.pkg, 8); memcpy(matl.data() + 56, &sk.ing, 8);
                // texture ref pkg @120 / ing @128 -> THIS mesh's texture; tgt @136 stays 0x6E4CC522 (murmur3"tex").
                // pkg is NOT implicit: the butterflies template's @120 holds the CALMING package, so leaving it makes the
                // texture resolve to {calming_pkg, our_ing} = MISSING TextureAsset -> ErrorNotReady -> INVISIBLE DROID.
                // (The shader ref above patches pkg+ing; the texture was only half-patched. THIS was the real blocker.)
                memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);
            } else if (matl.size() >= 96) {
                memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);  // legacy (HSR_HZMATTPL) path
            }
            std::vector<HzJoint> joints; joints.reserve(m.hzJointCount);
            for (int j = 0; j < m.hzJointCount; ++j) {
                HzJoint jt; jt.parent = m.hzParents[j];
                char jn[24]; snprintf(jn, sizeof jn, "joint_%d", j); jt.name = jn;
                jt.pos[0]=m.hzJointPos[j*3]; jt.pos[1]=m.hzJointPos[j*3+1]; jt.pos[2]=m.hzJointPos[j*3+2];
                jt.quat[0]=m.hzJointQuat[j*4]; jt.quat[1]=m.hzJointQuat[j*4+1]; jt.quat[2]=m.hzJointQuat[j*4+2]; jt.quat[3]=m.hzJointQuat[j*4+3];
                jt.scale = m.hzJointScale[j];
                joints.push_back(jt);
            }
            // 6 FAR bounds-anchor joints (match the clip's appended tracks at njReal..njReal+5) → the device's
            // joint-POSITION skinned cull bound spans ±C SYMMETRICALLY → never frustum/occlusion-culled from any angle.
            // Each gets ONE bounds vertex (encodeRendMeshParts) so it's counted in the per-joint loop. MUST match the
            // clip's joint count (nj0) AND the jointIds palette order — all three gate on the SAME env var + same names.
            if (!std::getenv("HSR_NOSKELANCHOR")) {
                const float AC = 50000.f;
                const float anchOff[6][3] = { {AC,0,0},{-AC,0,0},{0,AC,0},{0,-AC,0},{0,0,AC},{0,0,-AC} };
                for (int k = 0; k < 6; ++k) {
                    HzJoint a; a.parent = 0;
                    char nm[20]; snprintf(nm, sizeof nm, "bounds_anchor_%d", k); a.name = nm;
                    a.pos[0]=anchOff[k][0]; a.pos[1]=anchOff[k][1]; a.pos[2]=anchOff[k][2];   // FAR bind = the k-th ±C axis extreme (matches the clip track)
                    a.quat[0]=1; a.quat[1]=0; a.quat[2]=0; a.quat[3]=0;   // encodeHzSkel writes (w,x,y,z) → identity = (1,0,0,0)
                    a.scale=1;
                    joints.push_back(a);
                }
            }
            // SHARE one skeleton + one anim across all meshes that use the SAME armature (the omnidroid body + shield are
            // the same skin[0] -> identical encoded skel+anim). GROUND TRUTH: nuxd's prism_wave + motes both reference the
            // SAME skeleton/anim asset (one skeleton_0 + one take_001 in its scene). Shipping a DUPLICATE per-mesh skel/anim
            // made the device's async asset loader race -> std::length_error crash-loop (5-6x then loaded). Dedup by bytes:
            // ship a shared armatureN.skel/anim once; later meshes with identical bytes reference it.
            auto skelBytes = encodeHzSkel(joints);
            auto anim = std::move(hzAnimPre);   // reuse the clip pre-encoded above (size already checked vs the device signed-16 limit)
            if (!anim.empty()) {
                // ── SKEL pool: each mesh's own bind skeleton (dedup byte-identical). ──
                AssetKey3 skK{};
                int sIdx = -1;
                for (size_t r = 0; r < skelPool.size(); ++r) if (skelPool[r].bytes == skelBytes) { sIdx = (int)r; break; }
                if (sIdx >= 0) skK = skelPool[sIdx].key;
                else {
                    char rb[96]; snprintf(rb, sizeof rb, "%s/armature%zu", MH.c_str(), skelPool.size());
                    std::string pSkel = std::string(rb) + ".skel/skeleton";
                    skK = keyForPath(pSkel); skK.tgt = 2292226755u;  // HZAN:SKEL FIXED type targetId (all official envs)
                    // ⚠ CRASH FIX: the device animation registry keys skeletons by their INTERNAL name. encodeHzSkel defaults
                    // every skel to "Skeleton_0", so an env with several distinct skeletons (lakesidepeak ships 7) registers
                    // them all under the SAME name -> the registry's std::string key corrupts -> env-unload/Full-compaction
                    // std::length_error crash-loop (nuxd/incredibles never hit it: they ship ONE skeleton_0). Give each its
                    // own name. Dedup above stays byte-on-geometry (constant name), so byte-identical armatures still share.
                    auto skelShip = encodeHzSkel(joints, "Skeleton_" + std::to_string(skelPool.size()));
                    assets.push_back({ pSkel, skK.tgt, skelShip, skK, TYPE_HZAN, TYPE_SKEL });
                    skelPool.push_back({ skelBytes, skK });
                }
                // ── ANIM pool: shared clip emitted ONCE (THE crash fix — no duplicate large async loads). ──
                AssetKey3 anK{};
                int aIdx = -1;
                for (size_t r = 0; r < animPool.size(); ++r) if (animPool[r].bytes == anim) { aIdx = (int)r; break; }
                if (aIdx >= 0) anK = animPool[aIdx].key;
                else {
                    char rb[96]; snprintf(rb, sizeof rb, "%s/animclip%zu", MH.c_str(), animPool.size());
                    std::string pAnim = std::string(rb) + ".anim/anim";
                    anK = keyForPath(pAnim); anK.tgt = 1496459219u;  // HZAN:ANIM FIXED type targetId
                    // ⚠ CRASH FIX (same root as the skel name): hzAclEncode hardcodes the clip name "Take 001" (8 bytes @+32),
                    // so multiple clips collide in the device animation registry -> std::length_error. Patch THIS shipped copy's
                    // name to a unique "Take NNN"; dedup below stays on the original constant-name motion bytes.
                    auto animShip = anim;
                    // LOOP FIX (V205 RE, SampleAnimClipNode): the device LOOPS a clip only when it's the default take
                    // "Take 001" (WrapMode!=3); a non-default take name => play-once => the "backward snap" at reset.
                    // The cook USED to rename each clip to a unique "Take NNN" to dodge a registry-collision crash, but
                    // that broke looping. Official envs keep ALL clips "Take 001" (distinct asset PATHS avoid collision —
                    // which the cook also has: animclipN). Default = "Take 001" (loops); HSR_UNIQUETAKE restores the old
                    // unique names if the collision crash returns.
                    if (animShip.size() >= 40) {
                        char tk[9];
                        if (std::getenv("HSR_UNIQUETAKE")) { snprintf(tk, sizeof tk, "Take %03zu", animPool.size()); std::memcpy(animShip.data()+32, tk, 8); }
                        else std::memcpy(animShip.data()+32, "Take 001", 8);   // default take => device loops it
                    }
                    assets.push_back({ pAnim, anK.tgt, animShip, anK, TYPE_HZAN, TYPE_ANIM });
                    animPool.push_back({ anim, anK });
                }
                animatorComp = animatorComponentJson(skK, anK);
            }
        } else if (usePulse) {
            // FAITHFUL wisp SCALE breathe: unlitblend material pointed at our getTime() vertex-scale shader.
            // Same interface as unlitblend (realdome_mat verbatim); only the field7 shader ref changes. The geometry
            // is centered + the entity sits at the centroid (below) so the shader's model-space scale pivots in place.
            matl = matBlend;
            memcpy(matl.data() + 48,  &shaderPulseK.pkg, 8); memcpy(matl.data() + 56,  &shaderPulseK.ing, 8);  // field7 -> wispscale shader
            memcpy(matl.data() + 120, &texK.pkg, 8);         memcpy(matl.data() + 128, &texK.ing, 8);          // base tex
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' WISPSCALE (getTime vertex breathe)\n", i, m.name.c_str());
        } else if (useRot) {
            // V79 node Y-ROTATION -> our getTime() Y-rotation shader. GLOBAL: the (period,dir,blend) come from the
            // decoded source; the shader is generated ON-DEMAND for any combo (no pre-baked per-env shaders). TRANSPARENT
            // billboards (OW planets/Interloper alpha bg) route to a BLEND base (unlitblend) so the bg isn't an opaque
            // BOX; the opaque skybox uses unlit (depth write). Geometry is baked relative to the pivot + entity at the
            // pivot (useCenter below): the rotating node spins in place, its child meshes orbit it. Swept AABB (above).
            bool rblend = m.blend && haveBlend;
            // hash the motion (spin: omega+axis ; sway: amp+period+axis) + blend -> a stable filename; the shader is
            // GENERATED on demand for this EXACT motion — the global converter, no pre-baked per-env shaders.
            long ax=(long)(m.rotAxis[0]*1000), ay=(long)(m.rotAxis[1]*1000), az=(long)(m.rotAxis[2]*1000);
            long mv = m.rotOsc ? ((long)(m.rotAmp*100000) ^ ((long)(m.rotPeriod*1000)<<1)) : (long)(m.rotOmega*100000);
            uint32_t h = (uint32_t)(0x9E3779B9u*(uint32_t)ax ^ 0x85EBCA6Bu*(uint32_t)ay ^ 0xC2B2AE35u*(uint32_t)az ^ 0x27D4EB2Fu*(uint32_t)mv ^ (m.rotOsc?0x5BD1E995u:0u));
            if (m.rotReplay) { h = h*16777619u ^ (uint32_t)(long)(m.rotLoopSec*1000) ^ (uint32_t)(m.rotReplayN*2654435761u);   // ROTREPLAY: fold the angle table + loop + pivot into the cache key
                for (float a : m.rotQuats) h = h*16777619u ^ (uint32_t)(long)(a*10000);
                for (int k=0;k<3;k++) h = h*16777619u ^ (uint32_t)(long)(m.rotPivot[k]*100); }
            // COMBINED node SPIN + material UV-SCROLL (aurora): the node ROTATES the geometry (vertex) AND the mat.sanim
            // UVTransform scrolls the texture (fragment) — V79's two INDEPENDENT continuous channels (ModelMatrix vs
            // BaseTextureMtx). Chain them into one shader (UVSCROLL on the frag first, ROTATE on the vertex on top). Fold
            // the scroll rate into the cache key so a spin+scroll shader is distinct from a plain spin.
            bool rscroll = m.uvScroll && (m.uvRate[0]!=0.f || m.uvRate[1]!=0.f);
            if (rscroll) h = h*16777619u ^ (uint32_t)((long)(m.uvRate[0]*100000) ^ ((long)(m.uvRate[1]*100000)<<1));
            const char* pfx = m.rotReplay ? "rotrep" : (m.rotOsc ? "osc" : "rot");
            char rfn[160]; snprintf(rfn, sizeof rfn, "cooker/%s_%08x%s%s%s.surface.bin", pfx, h, rscroll ? "s" : "", rblend ? "_b" : "", meshFar ? "_dc" : "");   // _dc (FAR mesh): rot generated from the remap base -> inherits infinite projection; separate cache
            AssetKey3 rotK{};
            auto it = rotShaders.find(rfn);
            if (it != rotShaders.end()) rotK = it->second;
            else {
                auto rbytes = readFileBytes(rfn);
                if (rbytes.empty()) {   // Rodrigues shader for this exact motion (continuous spin OR (1-cos) sway) — C++ generator
                    const std::vector<uint8_t>& gbase = meshFar ? (rblend ? shadBlendDC : shadDC) : (rblend ? shadBlend : shad);   // FAR -> remap base
                    // material UV-SCROLL (FRAGMENT) first, then node SPIN (vertex position) on top — independent SPIR-V stages
                    // (same chaining as TRANSLATE+UVSCROLL). Gives the aurora a smooth continuous shimmer scroll AND Y-spin.
                    std::vector<uint8_t> base2 = gbase;
                    if (rscroll) { auto fb = shadergen::generate(gbase, shadergen::UVSCROLL, m.uvRate[0], m.uvRate[1]); if (!fb.empty()) base2 = fb; }
                    if (m.rotReplay) {   // AURORA: EXACT per-frame QUATERNION replay (tframes = pivot(3) + (N+1)*4 relative quats).
                        // useCenter (above) already re-centered the geometry to rotPivot, so the SHADER pivot is ORIGIN (0).
                        std::vector<float> tf; tf.reserve(3 + m.rotQuats.size());
                        tf.push_back(0.f); tf.push_back(0.f); tf.push_back(0.f);
                        tf.insert(tf.end(), m.rotQuats.begin(), m.rotQuats.end());
                        rbytes = shadergen::generate(base2, shadergen::ROTREPLAY, m.rotLoopSec, 0.f, 0.f, 0.f, 0.f, tf, m.rotReplayN);
                    }
                    else if (m.rotOsc) rbytes = shadergen::generate(base2, shadergen::OSCILLATE, m.rotAmp, m.rotPeriod, m.rotAxis[0], m.rotAxis[1], m.rotAxis[2]);
                    else          rbytes = shadergen::generate(base2, shadergen::ROTATE,    m.rotOmega, 0.f,       m.rotAxis[0], m.rotAxis[1], m.rotAxis[2]);
                    if (!rbytes.empty()) writeFileBytes(rfn, rbytes);   // cache to disk (reused on re-cook + inspectable)
                }
                if (!rbytes.empty()) { char rp[168]; snprintf(rp, sizeof rp, "%s/shaders/%s_%08x%s%s%s.surface/shader", MH.c_str(), pfx, h, rscroll ? "s" : "", rblend ? "_b" : "", meshFar ? "_dc" : "");
                    rotK = keyForPath(rp); assets.push_back({ rp, rotK.tgt, rbytes, rotK }); rotShaders[rfn] = rotK; } }
            matl = rblend ? matBlend : matTpl;                          // blend base for transparent billboards, else opaque unlit
            if (rotK.ing) { memcpy(matl.data() + 48, &rotK.pkg, 8); memcpy(matl.data() + 56, &rotK.ing, 8); }  // field7 -> rot shader (else static = safe fallback)
            memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);                  // base tex
            if (std::getenv("HSR_VERBOSE")) { if (m.rotReplay) fprintf(stderr, "[COOK] m%03zu '%s' ROTREPLAY %d keys loop=%.1fs axis=(%.2f,%.2f,%.2f) %s %s\n", i, m.name.c_str(), m.rotReplayN, m.rotLoopSec, m.rotAxis[0],m.rotAxis[1],m.rotAxis[2], rblend ? "BLEND" : "opaque", rotK.ing ? "" : "(MISSING -> static)");
                else if (m.rotOsc) fprintf(stderr, "[COOK] m%03zu '%s' SWAY amp=%+.0fdeg period=%.1fs axis=(%.2f,%.2f,%.2f) dir=%s %s %s\n", i, m.name.c_str(), m.rotAmp*57.2958f, m.rotPeriod, m.rotAxis[0],m.rotAxis[1],m.rotAxis[2], m.rotAmp>=0?"+":"-", rblend ? "BLEND" : "opaque", rotK.ing ? "" : "(MISSING -> static)");
                else fprintf(stderr, "[COOK] m%03zu '%s' SPIN%s %.1fs axis=(%.2f,%.2f,%.2f) dir=%s(%+.4f rad/s) %s %s\n", i, m.name.c_str(), rscroll?"+UVSCROLL":"", 6.2831853f/(m.rotOmega<0?-m.rotOmega:m.rotOmega), m.rotAxis[0],m.rotAxis[1],m.rotAxis[2], m.rotOmega>=0?"CCW":"CW", m.rotOmega, rblend ? "BLEND" : "opaque", rotK.ing ? "" : "(MISSING -> static)"); }
        } else if (useTrans) {
            // GENERAL node-TRANSLATION replay (Star Trek sliding screens): a shadergen::TRANSLATE getTime() shader that
            // adds the piecewise-linear-interpolated sampled OFFSET to inPos -> FAITHFULLY ports ANY translation track.
            // Generated on demand, hashed by the frames+loop (dedup). Position-anim -> edits all vertex stages (like rot).
            bool tblend = m.blend && haveBlend;
            // COMBINED TRANSLATE + FLIPBOOK (train STEAM): the card rides the moving train (TRANSLATE, vertex position)
            // AND its texture is a spritesheet flipbook (FLIPBOOK, fragment sample-UV). The two edit INDEPENDENT stages,
            // so we chain them — FLIPBOOK on the base first, then TRANSLATE on top — into one shader that moves AND cycles.
            bool tflip = m.flipbook;
            bool tscroll = m.uvScroll && !tflip && (m.uvRate[0]!=0.f || m.uvRate[1]!=0.f);
            uint32_t h = (uint32_t)(0x9E3779B9u*(uint32_t)m.transN ^ 0x85EBCA6Bu*(uint32_t)(long)(m.transLoop*1000));
            for (size_t k=0;k<m.transFrames.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.transFrames[k]*10000);
            if (tflip) { h = h*16777619u ^ (uint32_t)(m.flipCols*73856093 ^ m.flipRows*19349663 ^ m.flipFrames*83492791 ^ (int)(m.flipFps*100));
                for (size_t k=0;k<m.flipUVMats.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.flipUVMats[k]*100000); }
            bool tfade = tflip && m.fadeAnim && m.fadeN>=2 && m.fadeFrames.size()>=(size_t)m.fadeN && m.fadeLoop>1e-4f;
            if (tfade) { h = h*16777619u ^ (uint32_t)(long)(m.fadeLoop*1000);
                for (size_t k=0;k<m.fadeFrames.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.fadeFrames[k]*100000); }
            if (tscroll) h = h*16777619u ^ (uint32_t)((long)(m.uvRate[0]*100000) ^ ((long)(m.uvRate[1]*100000)<<1));
            const char* usuf = tflip?"f":(tscroll?"s":"");
            char tfn[160]; snprintf(tfn, sizeof tfn, "cooker/trans_%08x%s%s%s.surface.bin", h, usuf, tblend ? "_b" : "", meshFar ? "_dc" : "");
            AssetKey3 transK{}; auto it = rotShaders.find(tfn);
            if (it != rotShaders.end()) transK = it->second;
            else {
                auto tbytes = readFileBytes(tfn);
                if (tbytes.empty()) { const std::vector<uint8_t>& gbase = meshFar ? (tblend ? shadBlendDC : shadDC) : (tblend ? shadBlend : shad);
                    std::vector<uint8_t> base2 = gbase;
                    // material UV anim (FRAGMENT) first; node TRANSLATE (vertex position) layered on top — independent stages.
                    if (tflip) { bool rep = m.flipUVMats.size() >= (size_t)m.flipN*6 && m.flipN >= 2;
                        static const std::vector<float> noFade;
                        auto fb = rep ? shadergen::generate(gbase, shadergen::FLIPBOOK, m.flipLoop, 0.f,0.f,1.f,0.f, m.flipUVMats, m.flipN,
                                            tfade ? m.fadeFrames : noFade, tfade ? m.fadeLoop : 0.f)   // EXACT per-frame matrix replay + opacity fade (matches desktop)
                                      : shadergen::generate(gbase, shadergen::FLIPBOOK, (float)m.flipCols,(float)m.flipRows,(float)m.flipFrames,m.flipFps, m.flipOffset?1.f:0.f);
                        if (!fb.empty()) base2 = fb; }
                    else if (tscroll) { auto fb = shadergen::generate(gbase, shadergen::UVSCROLL, m.uvRate[0], m.uvRate[1]); if (!fb.empty()) base2 = fb; }
                    tbytes = shadergen::generate(base2, shadergen::TRANSLATE, m.transLoop, 0.f, 0.f, 1.f, 0.f, m.transFrames, m.transN);
                    if (!tbytes.empty()) writeFileBytes(tfn, tbytes); }
                if (!tbytes.empty()) { char tp[176]; snprintf(tp, sizeof tp, "%s/shaders/trans_%08x%s%s%s.surface/shader", MH.c_str(), h, usuf, tblend ? "_b" : "", meshFar ? "_dc" : "");
                    transK = keyForPath(tp); assets.push_back({ tp, transK.tgt, tbytes, transK }); rotShaders[tfn] = transK; } }
            matl = tblend ? matBlend : matTpl;
            if (transK.ing) { memcpy(matl.data()+48,&transK.pkg,8); memcpy(matl.data()+56,&transK.ing,8); }   // field7 -> translate(+flipbook/scroll) shader
            memcpy(matl.data()+120,&texK.pkg,8); memcpy(matl.data()+128,&texK.ing,8);                          // base tex
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' TRANSLATE%s%s %d frames loop=%.2fs %s %s\n", i, m.name.c_str(), tflip?"+FLIPBOOK":(tscroll?"+UVSCROLL":""), tfade?"+FADE":"", m.transN, m.transLoop, tblend ? "BLEND" : "opaque", transK.ing ? "" : "(MISSING -> static)");
        } else if (useScale) {
            // GENERAL node-SCALE "breathe" replay (Erebor's 12 wisps, NON-UNIFORM per-axis): a shadergen::SCALE getTime()
            // shader that does pos' = pivot + (inPos-pivot)*factor(t), where factor is the piecewise-linear-interpolated
            // sampled per-axis SCALE FACTOR (= source scale(t)/scale(0)). FAITHFUL per-axis amplitudes (the single hand-rolled
            // wispscale.surface couldn't). Generated on demand, hashed by the frames+loop+pivot (dedup). The shader pivots in
            // WORLD space, so the geometry stays at its baked world verts (NOT recentered — useCenter excludes useScale).
            // Position-anim -> edits ALL vertex stages (like rot/trans) so depth/motion match.
            bool sblend = m.blend && haveBlend;
            uint32_t h = (uint32_t)(0x9E3779B9u*(uint32_t)m.scaleN ^ 0x85EBCA6Bu*(uint32_t)(long)(m.scaleLoop*1000));
            for (int k=0;k<3;k++) h = h*16777619u ^ (uint32_t)(long)(m.scalePivot[k]*1000);
            for (size_t k=0;k<m.scaleFrames.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.scaleFrames[k]*10000);
            char sfn[160]; snprintf(sfn, sizeof sfn, "cooker/scale_%08x%s%s.surface.bin", h, sblend ? "_b" : "", meshFar ? "_dc" : "");
            AssetKey3 scaleK{}; auto it = rotShaders.find(sfn);
            if (it != rotShaders.end()) scaleK = it->second;
            else {
                auto sbytes = readFileBytes(sfn);
                if (sbytes.empty()) { const std::vector<uint8_t>& gbase = meshFar ? (sblend ? shadBlendDC : shadDC) : (sblend ? shadBlend : shad);
                    sbytes = shadergen::generate(gbase, shadergen::SCALE, m.scaleLoop, 0.f, m.scalePivot[0], m.scalePivot[1], m.scalePivot[2], m.scaleFrames, m.scaleN);
                    if (!sbytes.empty()) writeFileBytes(sfn, sbytes); }
                if (!sbytes.empty()) { char sp[176]; snprintf(sp, sizeof sp, "%s/shaders/scale_%08x%s%s.surface/shader", MH.c_str(), h, sblend ? "_b" : "", meshFar ? "_dc" : "");
                    scaleK = keyForPath(sp); assets.push_back({ sp, scaleK.tgt, sbytes, scaleK }); rotShaders[sfn] = scaleK; } }
            matl = sblend ? matBlend : matTpl;
            if (scaleK.ing) { memcpy(matl.data()+48,&scaleK.pkg,8); memcpy(matl.data()+56,&scaleK.ing,8); }   // field7 -> scale shader
            memcpy(matl.data()+120,&texK.pkg,8); memcpy(matl.data()+128,&texK.ing,8);                          // base tex
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' SCALE %d frames loop=%.2fs pivot=(%.2f,%.2f,%.2f) %s %s\n", i, m.name.c_str(), m.scaleN, m.scaleLoop, m.scalePivot[0],m.scalePivot[1],m.scalePivot[2], sblend ? "BLEND" : "opaque", scaleK.ing ? "" : "(MISSING -> static)");
        } else if (useUvScroll) {
            // mat.sanim UV-SCROLL (water/foam/waterfall): a getTime() uv += rate*time shader, generated on demand per
            // (rate,blend) — the global converter. Cooked geometry is unchanged; only the sampled UV scrolls.
            // FOAM (lake_foam Additive:true) renders via the PROVEN alpha-BLEND path: its texture alpha was set to LUMINANCE
            // (texture stage above), so white-on-black foam -> white=opaque (shows) / black=alpha0 (transparent, the lake shows
            // through). The foam's m.blend is already true (editor: additive->blend), so it routes as normal blend (matBlend +
            // unlitblend uvscroll). This is robust vs the additive-MATL blend-mode enum, which I couldn't verify -> a wrong value
            // made the whole-lake foam mesh a DARK SHEET over the water. (stream_foam = Additive:false, native alpha, same path.)
            bool ublend = m.blend && haveBlend;
            const char* usuf = ublend ? "_b" : "";
            long ru=(long)(m.uvRate[0]*100000), rv=(long)(m.uvRate[1]*100000);
            uint32_t h = (uint32_t)(0x9E3779B9u*(uint32_t)ru ^ 0xC2B2AE35u*(uint32_t)rv ^ (ublend?0x68bc21ebu:0u));
            bool utint = m.tintAnim && m.tintN >= 2 && m.tintFrames.size() >= (size_t)m.tintN*4 && m.tintLoop > 1e-4f;
            if (utint) { h = h*16777619u ^ (uint32_t)m.tintN ^ (uint32_t)(long)(m.tintLoop*1000);   // fold the tint cycle into the cache key
                for (float v : m.tintFrames) h = h*16777619u ^ (uint32_t)(long)(v*10000); }
            char ufn[160]; snprintf(ufn, sizeof ufn, "cooker/uvscroll_%08x%s%s%s.surface.bin", h, usuf, utint?"t":"", meshFar ? "_dc" : "");
            AssetKey3 uvK{};
            auto it = rotShaders.find(ufn);
            if (it != rotShaders.end()) uvK = it->second;
            else {
                auto ubytes = readFileBytes(ufn);
                if (ubytes.empty()) {   // getTime() uv += rate*time shader — C++ generator
                    const std::vector<uint8_t>& gbase = meshFar ? (ublend ? shadBlendDC : shadDC) : (ublend ? shadBlend : shad);   // FAR -> remap base
                    ubytes = shadergen::generate(gbase, shadergen::UVSCROLL, m.uvRate[0], m.uvRate[1]);
                    // MaterialTint RGBA cycle (fireworks flash / window flicker) chained onto the same forward frag
                    if (!ubytes.empty() && utint) { auto tb = shadergen::generate(ubytes, shadergen::TINTREPLAY, m.tintLoop, 0,0,0,0, m.tintFrames, m.tintN); if (!tb.empty()) ubytes = tb; }
                    if (!ubytes.empty()) writeFileBytes(ufn, ubytes);
                }
                if (!ubytes.empty()) { char up[160]; snprintf(up, sizeof up, "%s/shaders/uvscroll_%08x%s%s.surface/shader", MH.c_str(), h, usuf, meshFar ? "_dc" : "");
                    uvK = keyForPath(up); assets.push_back({ up, uvK.tgt, ubytes, uvK }); rotShaders[ufn] = uvK; } }
            matl = ublend ? matBlend : matTpl;   // foam/water uvscroll -> blend (luminance alpha) or opaque
            if (uvK.ing) { memcpy(matl.data() + 48, &uvK.pkg, 8); memcpy(matl.data() + 56, &uvK.ing, 8); }   // field7 -> uvscroll shader
            memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);                  // base tex
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' UVSCROLL rate=(%.3f,%.3f)/s %s%s %s\n", i, m.name.c_str(), m.uvRate[0], m.uvRate[1], (m.additive&&m.uvScroll)?"BLEND(lum-foam) ":"", ublend ? "BLEND" : "opaque", uvK.ing ? "" : "(MISSING -> static)");
        } else if (useVat) {
            std::string pVatTex = std::string(base) + ".vat.tex/tex"; vatTexK = keyForPath(pVatTex);
            matl = matVat;
            // BLEND wisp (V79 alphaMode=BLEND, e.g. erebor flames): repoint the VAT material's shader ref
            // @48/56/64 to the transparent+doubleSided VAT shader so the texture alpha FADES (vatunlitbasecolor
            // is opaque -> the V79 fade was lost). Opaque VAT meshes keep the baked-in vatunlitbasecolor ref.
            bool vatBlend = m.blend && haveVatBlend && !std::getenv("HSR_VATOPAQUE");   // HSR_VATOPAQUE: test plain shipped vatunlitbasecolor (isolate VAT-renders? from my custom blend shader)
            if (vatBlend) {
                memcpy(matl.data() + 48, &vatBlendK.pkg, 8); memcpy(matl.data() + 56, &vatBlendK.ing, 8);
                memcpy(matl.data() + 64, &vatBlendK.tgt, 4);
            }
            memcpy(matl.data() + 152, &texK.pkg, 8);    memcpy(matl.data() + 160, &texK.ing, 8);     // base color tex
            memcpy(matl.data() + 192, &vatTexK.pkg, 8); memcpy(matl.data() + 200, &vatTexK.ing, 8);   // VAT offset tex
            // ⛔ matParams blob @80 = {atlasSubDivX, atlasSubDivY, vat_AnimTracksCount, vat_AnimSkipFrames} (IDA: VAT vert
            //    shader struct %191; the vert column is mapped through an atlasSubDivX×atlasSubDivY grid). The vat_mat
            //    template is oceanarium's 4×4 per-INSTANCE atlas (fish variants), so for a SINGLE editor-baked node-anim
            //    mesh each vertex lands in 1/16th of the row -> garbage texels -> the geometry "collapse". Force 1×1 / 1
            //    track: our VAT tex is exactly width=vertexCount × height=frames, no atlas. (frameCount = texH / tracks.)
            if (matl.size() >= 92) { uint32_t one = 1; memcpy(matl.data()+80,&one,4); memcpy(matl.data()+84,&one,4); memcpy(matl.data()+88,&one,4); }
            assets.push_back({ pVatTex, vatTexK.tgt, vatTex, vatTexK });
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' VAT frames=%d verts=%u (atlas 1x1)%s\n", i, m.name.c_str(), m.vatFrames, vc, vatBlend ? " BLEND" : " opaque");
        } else if (m.flipbook && haveBlend && std::getenv("HSR_FLIPSTATIC")) {
            // DIAGNOSTIC: cook the flipbook as a PLAIN static unlit quad (NO generated shader) to isolate whether the
            // generated getTime flipbook shader is what breaks the device env render.
            matl = matTpl; memcpy(matl.data()+120,&texK.pkg,8); memcpy(matl.data()+128,&texK.ing,8);
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' FLIPBOOK -> STATIC unlit (HSR_FLIPSTATIC, no gen shader)\n", i, m.name.c_str());
        } else if (m.flipbook && haveBlend) {
            // FLIPBOOK (sonic_schoolhouse 3x3, Rick&Morty animated TV 10x10): the overlapping cell-quads were collapsed
            // to ONE quad (UV 0..1) in detectAndCollapseFlipbook. AUTO-GENERATE a getTime() shadergen::FLIPBOOK VERTEX
            // shader sized to THIS grid/fps that offsets inUv to the current cell — no skeleton (no device crash), loops
            // forever from globalUniforms.time. On-demand per (cols,rows,frames,fps); no pre-baked per-grid shaders.
            int fcols=m.flipCols>0?m.flipCols:3, frows=m.flipRows>0?m.flipRows:3;
            int fframes=m.flipFrames>0?m.flipFrames:fcols*frows; float ffps=m.flipFps>0.f?m.flipFps:5.f;
            float foff = m.flipOffset ? 1.f : 0.f;   // OFFSET flipbook (mat.sanim waterfall: uv'=inUv+cellOff, no scale) vs SCALE spritesheet
            bool useReplay = m.flipUVMats.size() >= (size_t)m.flipN*6 && m.flipN >= 2;   // EXACT per-frame source matrices present -> replay (fog/dust fix)
            bool ftint = m.tintAnim && m.tintN >= 2 && m.tintFrames.size() >= (size_t)m.tintN*4 && m.tintLoop > 1e-4f;   // FULL RGBA tint cycle (fireworks flashes)
            bool ffade = !ftint && useReplay && m.fadeAnim && m.fadeN>=2 && m.fadeFrames.size()>=(size_t)m.fadeN && m.fadeLoop>1e-4f;   // opacity fade curve (alpha-only subset — superseded by the RGBA tint replay)
            uint32_t h;
            if (useReplay) { h = (uint32_t)(0x9E3779B9u*(uint32_t)m.flipN ^ 0x85EBCA6Bu*(uint32_t)(long)(m.flipLoop*1000));
                for (size_t k=0;k<m.flipUVMats.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.flipUVMats[k]*100000); }
            else h=(uint32_t)(0x9E3779B9u*(uint32_t)fcols ^ 0xC2B2AE35u*(uint32_t)frows ^ 0x27D4EB2Fu*(uint32_t)fframes ^ 0x85EBCA6Bu*(uint32_t)(long)(ffps*1000) ^ (m.flipOffset?0xA1B2C3D4u:0u));
            if (ffade) { h = h*16777619u ^ (uint32_t)(long)(m.fadeLoop*1000);
                for (size_t k=0;k<m.fadeFrames.size();++k) h = h*16777619u ^ (uint32_t)(long)(m.fadeFrames[k]*100000); }
            if (ftint) { h = h*16777619u ^ (uint32_t)m.tintN ^ (uint32_t)(long)(m.tintLoop*1000);   // fold the tint cycle into the cache key
                for (float v : m.tintFrames) h = h*16777619u ^ (uint32_t)(long)(v*10000); }
            char ffn[160]; snprintf(ffn,sizeof ffn,"cooker/flipbook_%08x%s%s%s%s%s.surface.bin", h, useReplay?"r":"", ffade?"a":"", ftint?"t":"", m.flipOffset?"o":"", meshFar?"_dc":"");
            AssetKey3 flipK{}; auto it=rotShaders.find(ffn);
            if (it!=rotShaders.end()) flipK=it->second;
            else {
                auto fbytes=readFileBytes(ffn);
                if (fbytes.empty()) { const std::vector<uint8_t>& gbase = meshFar?shadBlendDC:shadBlend;   // unlitblend base (FAR -> depth-clamp remap)
                    static const std::vector<float> noFade;
                    fbytes = useReplay ? shadergen::generate(gbase, shadergen::FLIPBOOK, m.flipLoop, 0.f,0.f,1.f,0.f, m.flipUVMats, m.flipN,
                                             ffade ? m.fadeFrames : noFade, ffade ? m.fadeLoop : 0.f)   // EXACT per-frame matrix replay + opacity fade (desktop data)
                                       : shadergen::generate(gbase, shadergen::FLIPBOOK, (float)fcols,(float)frows,(float)fframes,ffps,foff);    // grid
                    // MaterialTint RGBA cycle (fireworks flash colors) chained onto the flipbook frag — replaces the
                    // alpha-only fade (the tint table carries alpha too, so both channels replay in one pass)
                    if (!fbytes.empty() && ftint) { auto tb = shadergen::generate(fbytes, shadergen::TINTREPLAY, m.tintLoop, 0,0,0,0, m.tintFrames, m.tintN); if (!tb.empty()) fbytes = tb; }
                    if(!fbytes.empty()) writeFileBytes(ffn,fbytes); }
                if(!fbytes.empty()){ char fp2[176]; snprintf(fp2,sizeof fp2,"%s/shaders/flipbook_%08x%s%s%s%s%s.surface/shader", MH.c_str(), h, useReplay?"r":"", ffade?"a":"", ftint?"t":"", m.flipOffset?"o":"", meshFar?"_dc":"");
                    flipK=keyForPath(fp2); assets.push_back({fp2, flipK.tgt, fbytes, flipK}); rotShaders[ffn]=flipK; } }
            matl = matBlend;
            if (flipK.ing){ memcpy(matl.data()+48,&flipK.pkg,8); memcpy(matl.data()+56,&flipK.ing,8); }   // field7 -> generated flipbook shader
            memcpy(matl.data()+120,&texK.pkg,8); memcpy(matl.data()+128,&texK.ing,8);                      // base tex -> the full spritesheet
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' FLIPBOOK %dx%d %dframes @%.2ffps %s (auto-gen) %s\n", i, m.name.c_str(), fcols,frows,fframes,ffps, m.flipOffset?"OFFSET":"scale", flipK.ing?"":"(MISSING -> static)");
        } else {
            // GROUND TRUTH from the live-captured MeshShellEnv fragment + libshell ShaderCache (sub_2C6C06C/sub_2C6D468):
            // the env shader has a HAS_MAKEOPAQUE variant (forces color.a=1 -> OPAQUE, ignores texture alpha) chosen for
            // blend-mode 0/1, and a HAS_ALPHACUTOFF variant (discard alpha<AlphaCutoff). The V79 source flags are IDENTICAL
            // for cliff/mountains AND fog/leaves (all Transparent:true, AlphaTest:false), so the device picks the variant by
            // the TEXTURE's OPAQUE FRACTION (decoded from the real ASTC). Replicate:
            //   AlphaTest:true ("_masked" GROUND) -> hard cutout (opaque MATL + alpha-test discard shader).
            //   Transparent:true (mountain/cliff/lakeshore CARDS, foliage, fog) -> alpha BLEND (unlitblend) -> composites
            //     over the cooked SkyDome, EXACTLY like the foliage that already cooks+renders fine. A prior
            //     opaque-fraction>0.35 heuristic forced the mostly-opaque mountain/lakeshore cards to the CUTOUT path;
            //     in the cook that landed them on the plain unlit (opaque) shader -> their transparent silhouette
            //     rendered as BLACK (user: "transparent background rendered black" / glitched mountain cards). The OPA
            //     preview discards-to-sky so it looked OK, but blend-over-sky is the faithful + robust cook. REMOVED.
            // CUTOUT (shadergen depth-write alpha-test discard) for scenery that must OCCLUDE + drop its transparent
            // silhouette to the sky: (a) authored AlphaTest "_masked" GROUND, and (b) mostly-OPAQUE Transparent CARDS
            // (mountains/cliffs/lakeshore: a silhouette mountain shape + transparent sky around). Soft/gappy Transparent
            // (foliage ~22% opaque, fog/smoke/waterfall ~0%) stays BLEND. Routing the mostly-opaque cards to BLEND (no
            // depth-write) was the regression: they didn't occlude (DEPTH ISSUES) + blended over black where the SkyDome
            // wasn't already in the framebuffer (BLACK background). Clean split at opaque>~35% (terrain >=47% vs foliage <=22%).
            bool cutoutOK = (m.alphaTest && !m.additive && haveCutout && !meshFar);   // authored MASK
            bool isTreeCutout = false;   // sparse tree-line card -> use the LOW-threshold cutout (keeps foliage, still depth-writes)
            if (!cutoutOK && m.blend && !m.additive && haveCutout && !meshFar && m.rgba.size() >= 4) {
                size_t nt = m.rgba.size()/4, opq=0, mid=0;
                for (size_t k=3; k<m.rgba.size(); k+=4){ uint8_t a=m.rgba[k]; if (a>=230) opq++; if (a>=40 && a<=215) mid++; }
                float opF = nt ? (float)opq/(float)nt : 0.f, midF = nt ? (float)mid/(float)nt : 1.f;
                // (a) mostly-opaque silhouette cards (mountains/cliffs/lakeshore: opaque>35%); (b) distant TREE-LINE cards
                // (LakeTrees etc.: sparser ~22% opaque + soft edges, so the opaque-fraction test misses them) — the user
                // wants these SOLID, not see-through blend cards with a black border. This branch is STATIC-only (skinned
                // foreground foliage takes the skinned path), so close soft foliage is unaffected by the tree-name rule.
                // GENERIC (no material names): the DEVICE picks cutout-vs-blend by the texture's ALPHA SHAPE
                // (ShellEnv HAS_ALPHACUTOFF variant = discard alpha<cutoff + depth-write, vs plain alpha BLEND).
                // A HARD-EDGED / BIMODAL card — pixels are ~all opaque or fully transparent, negligible soft
                // gradient (midF ~ 0) — is a mask / foliage / vista card that must OCCLUDE: cook it as a
                // depth-write cutout (discard the transparent gaps). SOFT-alpha transparents (fog / smoke / glow /
                // aurora / ice: a real mid-alpha gradient, midF high) stay BLEND. This REPLACES the old
                // name-based "Tree" rule + opaque-fraction threshold, which missed sparse foliage
                // (winterwonderland gazeboTrees/lodgeTrees opF=0.01-0.05 -> blend -> saw-through). Verified split:
                // trees midF=0.00, vistaCards midF=0.01 (CUTOUT) vs moonGlow 0.13 / lambert34 0.14 / ice 0.58 (BLEND).
                bool bimodal   = (midF < 0.05f);   // hard-edged cutout mask (no soft gradient)
                bool hasOpaque = (opF  > 0.004f);  // has real opaque content (skip near-empty cards)
                // SOLID-BODIED soft-edged cards (vista's layered diorama: midBG opF=0.35, LtFg 0.49, RtFg 0.26
                // with midF 0.07-0.14 = a solid silhouette wearing an anti-aliased EDGE) — V79 renders these via
                // the dedicated MeshShellEnvMaskDepthWrite pass (depth-write the body, blend the edge). Cooking
                // them plain BLEND (no depth-write) broke layer occlusion = vista "depth issues everywhere".
                // Port = the LOW-threshold cutout: depth-writes the body, discards the transparent sky, keeps
                // most of the soft edge (0.12 cut). Pure-soft FX (fog/glow/glass: opF~0 or midF high) stay BLEND.
                bool solidBody = (opF > 0.25f && midF < 0.30f);
                // FLAT HORIZONTAL DECAL guard (property, not name): a pond shadow / water-colour overlay lies IN the
                // floor plane (Y is its SMALLEST extent) and paints translucently ONTO the coplanar floor — it must
                // NOT depth-write or it z-fights the floor ("texture fighting" on pond_color_sh over platformCenter).
                // A vista scenery card is VERTICAL (large Y extent) and must occlude, so it still depth-writes. Only
                // the solidBody branch is gated (a genuinely bimodal mask decal keeps its cutout).
                float pmn[3]={1e30f,1e30f,1e30f}, pmx[3]={-1e30f,-1e30f,-1e30f};
                for (size_t q=0; q+2<m.positions.size(); q+=3) for(int a=0;a<3;a++){ float p=m.positions[q+a]; if(p<pmn[a])pmn[a]=p; if(p>pmx[a])pmx[a]=p; }
                float exX=pmx[0]-pmn[0], exY=pmx[1]-pmn[1], exZ=pmx[2]-pmn[2];
                bool flatHorizontalDecal = (exY < 0.15f*exX && exY < 0.15f*exZ);   // near-planar in Y = ground/pond decal
                if (!flatHorizontalDecal && ((bimodal && hasOpaque) || solidBody)) {   // flat floor decals stay BLEND (no depth-write = no z-fight with the coplanar floor)
                    cutoutOK = true;
                    isTreeCutout = true;   // LOW 0.12 discard: depth-writes + keeps any faint soft edge; for midF~0 it equals a 0.5 cut
                }
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' blendcard opF=%.2f midF=%.2f -> %s\n", i, m.name.c_str(), opF, midF, cutoutOK?(bimodal?"CUTOUT(bimodal)":"CUTOUT(solid-body)"):"blend");
            }
            matl = (m.blend && haveBlend && !cutoutOK) ? matBlend : matTpl;
            if (cutoutOK) {   // CUTOUT = opaque MATL + alpha-test DISCARD shader -> DEPTH-WRITE: authored MASK scenery OCCLUDES (fixes back-overlays-front); discard cuts the hard mask. Double-sided handled by the cook's reversed-tri append.
                const AssetKey3& ck = (isTreeCutout && haveCutoutTree) ? shaderCutoutTreeK : shaderCutoutK;   // sparse tree cards -> low (0.12) threshold so the foliage survives the discard
                memcpy(matl.data()+48,&ck.pkg,8); memcpy(matl.data()+56,&ck.ing,8);
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] m%03zu '%s' CUTOUT depth-write (blend=%d mask=%d tree=%d)\n", i, m.name.c_str(), (int)m.blend, (int)m.alphaTest, (int)isTreeCutout);
            } else if (meshFar) { AssetKey3 dck = (m.blend && haveBlend) ? shaderBlendDCK : shaderDCK;   // FAR -> remap shader (renders past the 5000 clip); near keeps unlit/unlitblend = byte-exact
                           if (dck.ing) { memcpy(matl.data()+48,&dck.pkg,8); memcpy(matl.data()+56,&dck.ing,8); } }
            else if (m.additive && haveBlend) {
                // EMISSIVE GLOW (godRays/warp/nebula): unlitblend (the alpha-pass shader) + matBlend + the
                // luminance-alpha texture (converted above). The OLD "opaque-pass unlit shader in the transparent
                // MATL = additive" trick is DISPROVEN on device (storybook godRays / lake foam rendered their
                // black background as a DARK SHEET over bright scenes — it draws UNBLENDED; the official additive
                // .surface pass state is byte-identical to opaque, so f2,f3-presence never encoded a blend mode).
                // HSR_ADDGLOW_OLD restores the trick for A/B.
                if (std::getenv("HSR_ADDGLOW_OLD")) {
                    memcpy(matl.data()+48,&shaderK.pkg,8); memcpy(matl.data()+56,&shaderK.ing,8);
                    if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' ADDITIVE glow (OLD opaque-pass trick, HSR_ADDGLOW_OLD)\n", i, m.name.c_str());
                } else if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' ADDITIVE glow -> luminance-alpha BLEND (unlitblend, device-proven path)\n", i, m.name.c_str());
            }
            // STATIC mesh with a MaterialTint RGBA cycle (stinson city window lights / lightsStatic / terraces:
            // geometry static, the mat.sanim tint FLASHES the emissive texture) — generate a TINTREPLAY variant
            // of the plain unlit/unlitblend base and point field7 at it. Without this the device showed one
            // constant "generic" color (the renderer's animate() played the same track live, so preview was right).
            else if (m.tintAnim && m.tintN >= 2 && m.tintFrames.size() >= (size_t)m.tintN*4 && m.tintLoop > 1e-4f) {
                uint32_t th = (uint32_t)(0x9E3779B9u*(uint32_t)m.tintN ^ 0x85EBCA6Bu*(uint32_t)(long)(m.tintLoop*1000) ^ ((m.blend&&haveBlend)?0x68bc21ebu:0u));
                for (float v : m.tintFrames) th = th*16777619u ^ (uint32_t)(long)(v*10000);
                char tfn[160]; snprintf(tfn, sizeof tfn, "cooker/tint_%08x%s%s.surface.bin", th, (m.blend&&haveBlend)?"_b":"", meshFar?"_dc":"");
                AssetKey3 tK{}; auto itT = rotShaders.find(tfn);
                if (itT != rotShaders.end()) tK = itT->second;
                else {
                    auto tbytes = readFileBytes(tfn);
                    if (tbytes.empty()) {
                        const std::vector<uint8_t>& gbase = meshFar ? ((m.blend&&haveBlend) ? shadBlendDC : shadDC) : ((m.blend&&haveBlend) ? shadBlend : shad);
                        tbytes = shadergen::generate(gbase, shadergen::TINTREPLAY, m.tintLoop, 0,0,0,0, m.tintFrames, m.tintN);
                        if (!tbytes.empty()) writeFileBytes(tfn, tbytes);
                    }
                    if (!tbytes.empty()) { char tp[168]; snprintf(tp, sizeof tp, "%s/shaders/tint_%08x%s%s.surface/shader", MH.c_str(), th, (m.blend&&haveBlend)?"_b":"", meshFar?"_dc":"");
                        tK = keyForPath(tp); assets.push_back({ tp, tK.tgt, tbytes, tK }); rotShaders[tfn] = tK; }
                }
                if (tK.ing) { memcpy(matl.data()+48,&tK.pkg,8); memcpy(matl.data()+56,&tK.ing,8); }
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' TINTREPLAY %d frames loop=%.2fs %s %s\n", i, m.name.c_str(), m.tintN, m.tintLoop, (m.blend&&haveBlend)?"BLEND":"opaque", tK.ing?"":"(MISSING -> static tint)");
            }
            memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);
        }
        // Skinned: use the CENTERED rest positions (extractHzAnim centered mesh+skeleton+clip near origin so the
        // verifier accepts them); place the rig via the entity transform at the baked mesh's center (skinPos below).
        // Diagnostic: HSR_HZWORLD=1 -> skinned mesh uses the EXACT world-baked positions that pass as static (isolates
        // whether the skin markers/skeleton/animator break the verifier vs. the rest-position coordinate change).
        const std::vector<float>& hzMeshPos = std::getenv("HSR_HZWORLD") ? m.positions : m.hzRestPos;
        if (useHz && std::getenv("HSR_VERBOSE")) {
            int maxIdx = 0; for (auto b : m.hzBoneIdx) if ((int)b > maxIdx) maxIdx = (int)b;
            float rmn[3]={1e30f,1e30f,1e30f}, rmx[3]={-1e30f,-1e30f,-1e30f};
            for (size_t v=0; v+2<hzMeshPos.size(); v+=3) for(int k=0;k<3;k++){ float p=hzMeshPos[v+k]; if(p<rmn[k])rmn[k]=p; if(p>rmx[k])rmx[k]=p; }
            float wsum = 0; for (size_t k=0; k<4 && k<m.hzBoneWgt.size(); k++) wsum += m.hzBoneWgt[k];
            float wmn[3]={1e30f,1e30f,1e30f}, wmx[3]={-1e30f,-1e30f,-1e30f};
            for (size_t v=0; v+2<m.positions.size(); v+=3) for(int k=0;k<3;k++){ float p=m.positions[v+k]; if(p<wmn[k])wmn[k]=p; if(p>wmx[k])wmx[k]=p; }
            fprintf(stderr, "[HZDIAG] m%03zu joints=%d maxBoneIdx=%d v0wgtSum=%.3f restB=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f) worldB=(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n",
                    i, m.hzJointCount, maxIdx, wsum, rmn[0],rmn[1],rmn[2], rmx[0],rmx[1],rmx[2], wmn[0],wmn[1],wmn[2], wmx[0],wmx[1],wmx[2]);
            for (int j=0; j<m.hzJointCount && j<8 && (int)m.hzJointPos.size()>=(j+1)*3; j++)
                fprintf(stderr, "  [HZJOINT] m%03zu j%d parent=%d localPos=(%.2f,%.2f,%.2f) scl=%.3f\n", i, j,
                        j<(int)m.hzParents.size()?m.hzParents[j]:-9, m.hzJointPos[j*3],m.hzJointPos[j*3+1],m.hzJointPos[j*3+2],
                        j<(int)m.hzJointScale.size()?m.hzJointScale[j]:0.f);
        }
        std::vector<uint32_t> jointIds;   // ROOT.f2 joint-binding table = murmur3(joint name) per skeleton joint (m###.skel order)
        if (useHz) {
            for (int j = 0; j < m.hzJointCount; ++j) { char jn[24]; snprintf(jn, sizeof jn, "joint_%d", j); jointIds.push_back(murmur3_x86_32(jn, strlen(jn), 0)); }
            // The cook appends 6 STATIC bounds-anchor joints (skel + clip, indices m.hzJointCount..+5) when !HSR_NOSKELANCHOR;
            // their ids MUST be in this palette table (same order/names as the skeleton joints) so the 6 bounds verts
            // (one per anchor, in encodeRendMeshParts) map to them. MUST match the nAnch gate + names above.
            if (!std::getenv("HSR_NOSKELANCHOR"))
                for (int k = 0; k < 6; ++k) { char nm[20]; snprintf(nm, sizeof nm, "bounds_anchor_%d", k); jointIds.push_back(murmur3_x86_32(nm, strlen(nm), 0)); }
        }
        std::vector<float> dbgPos; const std::vector<float>* staticPos = &m.positions;
        if (std::getenv("HSR_DROIDFRONT") && m.hzJointCount > 0) {   // diag: shrink+move the droid right in front of spawn (isolate position/cull vs mesh-reject)
            float c[3]={0,0,0}; size_t np=m.positions.size()/3;
            for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) c[k]+=m.positions[v+k];
            for(int k=0;k<3;k++) c[k]/= (np?(float)np:1.f);
            dbgPos.resize(m.positions.size()); float tgt[3]={0,1.5f,-3.f}, sc=0.08f;
            for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) dbgPos[v+k]=(m.positions[v+k]-c[k])*sc+tgt[k];
            staticPos = &dbgPos;
        }
        // SCALE-ANIM wisp: recenter the geometry on its centroid so the scale pivots IN PLACE — the entity transform
        // puts it back at the centroid. Needed for BOTH the wispscale shader (model-space scale) and the (one-shot)
        // ShellPoseAnimationComponent. Without centering, scaling model-space pos around origin flings the wisp.
        std::vector<float> poseCentered; float poseCtr[3] = {0,0,0};
        bool usePose = m.poseAnim && !useHz && !useVat;
        bool useCenter = (usePose || usePulse || useRot) && !useHz && !useVat;
        if (useCenter) {
            if (useRot) {   // ROTATION pivots around the animated node's WORLD origin — the skybox node spins in place,
                for (int k=0;k<3;k++) poseCtr[k] = m.rotPivot[k];   // its CHILD planets (offset from the pivot) ORBIT it.
            } else {        // SCALE/pose: pivot in place around the mesh centroid.
                size_t np = m.positions.size()/3; double c[3]={0,0,0};
                for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) c[k]+=m.positions[v+k];
                for (int k=0;k<3;k++) poseCtr[k] = np ? (float)(c[k]/(double)np) : 0.f;
            }
            poseCentered.resize(m.positions.size());
            for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) poseCentered[v+k]=m.positions[v+k]-poseCtr[k];
            staticPos = &poseCentered;
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' CENTERED ctr=(%.2f,%.2f,%.2f) for %s\n",
                    i, m.name.c_str(), poseCtr[0],poseCtr[1],poseCtr[2], useRot ? "Y-rotation (pivot)" : usePulse ? "wispscale shader" : "poseAnimation");
        }
        // DIAGNOSTIC/FIX: HSR_PULLIN=<dist> pulls FAR static geometry toward the view origin (camPos) so its centroid
        // lands within <dist>, PRESERVING its apparent size from the viewpoint. The shell's far clip plane is fixed and
        // hard-capped at 1000 (IDA: ViewCameraManager sub_20BC124) and NOT env-expandable; this fits the distant city
        // INSIDE it instead. Also a clean test: if the backdrop renders once pulled in, the "black dome" was the far clip.
        std::vector<float> pulledPos;
        if (const char* pe = std::getenv("HSR_PULLIN")) {
            float thr = (float)atof(pe); if (thr < 1.f) thr = 1.f;
            float o[3] = { camPos?camPos[0]:0.f, camPos?camPos[1]:1.6f, camPos?camPos[2]:0.f };
            const std::vector<float>& src = *staticPos; size_t np = src.size()/3; double c[3]={0,0,0};
            for (size_t v=0; v+2<src.size(); v+=3) for(int k=0;k<3;k++) c[k]+=src[v+k];
            if (np) { float cx=(float)(c[0]/np)-o[0], cy=(float)(c[1]/np)-o[1], cz=(float)(c[2]/np)-o[2];
                float d = sqrtf(cx*cx+cy*cy+cz*cz);
                if (d > thr) { float f=(thr*0.9f)/d; pulledPos.resize(src.size());
                    for (size_t v=0; v+2<src.size(); v+=3) for(int k=0;k<3;k++) pulledPos[v+k]=o[k]+(src[v+k]-o[k])*f;
                    staticPos = &pulledPos;
                    if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' PULLED IN d=%.0f->%.0f f=%.3f\n", i, m.name.c_str(), d, thr*0.9f, f); } }
        }
        // BAKED LIGHTMAP → sem4 COLOR_0: sample the EXPOSED lightmap (m.lmRGBA, already ACES-toned) at each vertex's
        // uv1 (m.uvs2). The unlit/skinned shader does base×COLOR0, so the cooked mesh carries the baked room lighting
        // with NO extra shader/texture/UV-stream — the whole point of exposing lmRGBA+uvs2 to the cook. (HSR_LMNOVFLIP
        // if the V axis comes out inverted.) Skinned/animated meshes have no baked lightmap → empty → white (neutral).
        // UNIFIED sem4 COLOR_0 bake = albedoFactor × curTint × (exposed lightmap × lightmapPower, else white). The
        // unlit/skinned shader does base×COLOR0, so this single stream carries ALL the exposed shading: the baked
        // room lighting (per-vertex, sampled at uv1), the basecolor/metal/gem factor, and the per-frame mat.sanim
        // MaterialTint (fog/dust/foam opacity in alpha) — no new shader/texture/UV-stream. (HSR_LMNOVFLIP for V.)
        std::vector<uint8_t> vertCol;
        if (!m.iblVertCol.empty() && m.iblVertCol.size() >= (m.positions.size()/3)*4) {
            // SpecIbl lake water: use the faithful diffuse-irradiance bake (env-lit) as vertexColor0 -> device base·col =
            // the lit water instead of the dark basecolor. (Specibl meshes have no lightmap, so the lightmap path below is a no-op.)
            vertCol = m.iblVertCol;
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' SpecIbl irradiance vertexcolor baked (%zu verts)\n", i, m.name.c_str(), m.iblVertCol.size()/4);
        } else {
            size_t nvc = m.positions.size()/3;
            bool hasLm = m.hasLightmap && !m.lmRGBA.empty() && m.lmW>0 && m.lmH>0 && m.uvs2.size() >= nvc*2;
            // FADED effect cards (fog/dust): the getTime flipbook shader REPLAYS the mat.sanim tint-alpha curve, so the
            // per-vertex COLOR_0 alpha must be NEUTRAL 1.0 (else static curTint[3] — 0 at cook phase 0 — ×  shaderFade = 0
            // = invisible, the "only one fog visible" bug). The shader owns the opacity for these.
            float fr=m.albedoFactor[0]*m.curTint[0], fg=m.albedoFactor[1]*m.curTint[1], fb=m.albedoFactor[2]*m.curTint[2];
            float fa=(m.fadeAnim && m.fadeN>=2) ? 1.0f : m.curTint[3];
            bool needCol = hasLm || fr!=1.f||fg!=1.f||fb!=1.f||fa!=1.f;
            if (needCol && nvc>0) {
                vertCol.resize(nvc*4);
                bool vflip = !std::getenv("HSR_LMNOVFLIP");
                auto c8=[](float x){ int i=(int)(x*255.f+0.5f); return (uint8_t)(i<0?0:i>255?255:i); };
                for (size_t v=0; v<nvc; v++) {
                    float lr=1.f,lg=1.f,lb=1.f;
                    if (hasLm) {
                        float u=m.uvs2[v*2], w=m.uvs2[v*2+1]; u-=std::floor(u); w-=std::floor(w); if(vflip) w=1.f-w;
                        int px=(int)(u*(float)(m.lmW-1)+0.5f), py=(int)(w*(float)(m.lmH-1)+0.5f);
                        px=px<0?0:(px>=(int)m.lmW?(int)m.lmW-1:px); py=py<0?0:(py>=(int)m.lmH?(int)m.lmH-1:py);
                        const uint8_t* tx=&m.lmRGBA[((size_t)py*m.lmW+px)*4];
                        lr=tx[0]/255.f*m.lightmapPower[0]; lg=tx[1]/255.f*m.lightmapPower[1]; lb=tx[2]/255.f*m.lightmapPower[2];
                    }
                    vertCol[v*4]=c8(fr*lr); vertCol[v*4+1]=c8(fg*lg); vertCol[v*4+2]=c8(fb*lb); vertCol[v*4+3]=c8(fa);
                }
            }
        }
        // SKYBOX meshes (camera-locked pass) need two cook-time fixes, else the dome is invisible:
        //  (1) RE-CENTER verts at origin: the skybox is rendered centered on the camera (IDA: its transform is camera-
        //      derived; calming's skybox entity has no Transform). V79 bakes geometry at WORLD coords, so an un-centered
        //      dome renders offset by its centroid (thousands of units) -> off-screen. Subtract the centroid.
        //  (2) DOUBLE-SIDED: a dome viewed from inside has outward (back) faces -> culled -> invisible. Append reversed tris.
        std::vector<float> skyPos; const std::vector<float>* posPtr = staticPos;
        std::vector<uint32_t> dsIdx;   const std::vector<uint32_t>* sIdx = &m.indices;
        if (m.skybox && !useHz) {
            skyPos = *staticPos; size_t nvp = skyPos.size()/3; double cc[3]={0,0,0};
            for (size_t v=0; v<nvp; v++){ cc[0]+=skyPos[v*3]; cc[1]+=skyPos[v*3+1]; cc[2]+=skyPos[v*3+2]; }
            if (nvp){ float cx=(float)(cc[0]/nvp), cy=(float)(cc[1]/nvp), cz=(float)(cc[2]/nvp);
                      for (size_t v=0; v<nvp; v++){ skyPos[v*3]-=cx; skyPos[v*3+1]-=cy; skyPos[v*3+2]-=cz; } }
            posPtr = &skyPos;
        }
        // DOUBLE-SIDED: append REVERSED tris so the single-sided cooked unlit shader still draws back faces. The skybox
        // dome needs it (viewed from inside); ANY glTF doubleSided source material needs it too — else flat/open meshes
        // (the bat-computer monitor-screen quad, thin wall panels) get back-face-culled on device = see-through HOLES,
        // even though the editor preview (which honors doubleSided = no cull) showed them. Geometry-only, no z-fight (for
        // any pixel exactly one of the coincident tris is front-facing). Skinned (own doublesided shader) / VAT+rot+flip
        // billboards (intentionally single-sided, face the player) keep their own handling.
        // DOUBLE-SIDED billboard cards (the fog/dust/smoke/steam/comet "invisible + not animating" fix): a single-sided
        // quad viewed from its BACK is back-face-culled = INVISIBLE (and its texture animation can't be seen). Append
        // reversed-winding tris (INDEX-ONLY, reuses the same verts → same skin weights, geometry-only, no z-fight) so
        // both faces draw. Now applies to the ANIMATED card meshes too (flipbook/useTrans/useRot/useVat AND the useHz
        // node-rigid comet) — the old gate excluded exactly those, hiding them. HSR_NODBLCARD disables. skybox dome too.
        bool wantDouble = !std::getenv("HSR_NODBLCARD") &&
            (m.skybox || m.doubleSided || m.flipbook || useTrans || useRot || useUvScroll || useVat || (useHz && m.blend));
        std::vector<uint32_t> dsIdxHz;
        if (wantDouble) {
            std::vector<uint32_t>& dst = useHz ? dsIdxHz : dsIdx;
            dst = m.indices; dst.reserve(m.indices.size()*2);
            for (size_t t=0; t+2<m.indices.size(); t+=3) { dst.push_back(m.indices[t]); dst.push_back(m.indices[t+2]); dst.push_back(m.indices[t+1]); }
            if (!useHz) sIdx = &dsIdx;
        }
        const std::vector<uint32_t>& hzIdx = (wantDouble && useHz) ? dsIdxHz : m.indices;
        // SPRITESHEET flipbook (fire/smoke): INSET the card's base UVs a couple texels toward their cell so the per-cell
        // sample never touches the cell EDGE — kills the residual CLOSE-UP seam (mip0 linear-filter + ASTC-block bleed into
        // the neighbour cell). The mip-cap handled the distant coarse-mip bleed; this handles up-close. Real grids only.
        const std::vector<float>* uvPtr = &m.uvs; std::vector<float> insetUv;
        if ((capCols>1 || capRows>1) && m.uvs.size()>=4) {
            float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
            for (size_t k=0;k+1<m.uvs.size();k+=2){ float u=m.uvs[k],v=m.uvs[k+1]; if(u<umin)umin=u;if(u>umax)umax=u;if(v<vmin)vmin=v;if(v>vmax)vmax=v; }
            float du=umax-umin, dv=vmax-vmin, mu=(m.w>0)?8.0f/(float)m.w:0.f, mv=(m.h>0)?8.0f/(float)m.h:0.f;   // inset one ASTC block (8 texels) so the sample clears the cell-boundary block that straddles two cells
            if (du>4*mu && dv>4*mv) { insetUv=m.uvs; float su=(du-2*mu)/du, sv=(dv-2*mv)/dv;
                for (size_t k=0;k+1<insetUv.size();k+=2){ insetUv[k]=umin+mu+(insetUv[k]-umin)*su; insetUv[k+1]=vmin+mv+(insetUv[k+1]-vmin)*sv; }
                uvPtr=&insetUv;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] m%03zu '%s' UV-INSET 8 texels toward cell (close-up seam fix)\n", i, m.name.c_str()); }
        }
        auto mesh = useHz ? encodeRendMeshParts(hzMeshPos, *uvPtr, hzIdx, matl, 0, m.hzBoneIdx, m.hzBoneWgt, jointIds, false, vertCol)
                          : encodeRendMeshParts(*posPtr, *uvPtr, *sIdx, matl, useVat ? vc : 0, {}, {}, {}, useRot, vertCol);
        // COOK-TIME VERIFY: check the just-cooked skinned RENDMESH against the Meta-shipped reference schema (the
        // device runs the stock flatbuffers verifier; this catches structural divergence — e.g. a field emitted as
        // an offset where the schema wants an inline scalar — BEFORE the asset ever ships to the headset).
        {   // verify BOTH from-scratch encoders (skinned + static) against the matching Meta RENDMESH oracle; crash-proof
            const std::vector<uint8_t>& meshRef = useHz ? refSkinMesh : refStaticMesh;
            if (meshRef.size() >= 8) {
                auto probs = fbVerifyAgainst(mesh, meshRef);   // fbVerifyAgainst is bounds-checked (never crashes on bad input)
                if (probs.empty()) fprintf(stderr, "[COOK-VERIFY] %s : OK (matches Meta %s schema)\n", pMesh.c_str(), useHz?"skinned-mesh":"static-mesh");
                else { fprintf(stderr, "[COOK-VERIFY] %s : %zu PROBLEM(S) vs device schema (mesh format: lods->parts->vertexStreams{elements,vertexData}+bounds+indexData+materialDefinitionData):\n", pMesh.c_str(), probs.size());
                       for (auto& s : probs) fprintf(stderr, "    !! %s\n", s.c_str()); }
            }
        }
        assets.push_back({ pMesh, meshK.tgt, mesh, meshK });
        if (!tex.empty()) assets.push_back({ pTex, texK.tgt, tex, texK });
        assets.push_back({ pMat, matK.tgt, matl, matK });
        std::string nm = m.name.empty() ? ("mesh" + std::to_string(i)) : m.name;
        std::string mid = makeUuid(rng);
        // SKINNED: the mesh+rig are centered at origin; place them at the baked mesh's center (= the scene position) via the entity transform.
        float skinPos[3] = {0,0,0};
        if (useHz) { float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
            for (size_t v=0; v+2<m.positions.size(); v+=3) for(int k=0;k<3;k++){ float p=m.positions[v+k]; if(p<mn[k])mn[k]=p; if(p>mx[k])mx[k]=p; }
            for(int k=0;k<3;k++) skinPos[k]=0.5f*(mn[k]+mx[k]); }
        static const float fixedPos[3] = {0.f, 1.4f, 0.f}; static const float fixedScl[3] = {0.03f, 0.03f, 0.03f};  // HSR_HZFIXED: can't-miss visibility test (tiny droid at spawn)
        const float* hzPos = (useHz && std::getenv("HSR_HZFIXED")) ? fixedPos : (useHz && std::getenv("HSR_HZCENTER")) ? skinPos : useCenter ? poseCtr : pos0;
        const float* hzScl = (useHz && std::getenv("HSR_HZFIXED")) ? fixedScl : scl1;
        // skinned -> AnimatorPlatformComponent (a real components[] entry). pose-anim wisp -> the TOP-LEVEL "poseAnimation"
        // entity key (IDA V205 EnvironmentEntitySystem sub_1148BFC), NOT a component — the mesh keeps its unlitblend shader.
        std::string extraComp = !animatorComp.empty() ? animatorComp : std::string();
        // ── ANIMATED COLLIDER: if this mesh is marked as a collider, PhysX-cook its geometry into a ColliderMesh +
        //    StaticCollision PhysicsBody. Placement is the whole game here (japan "colliders went berserk"):
        //    SKINNED (useHz) colliders must NOT ship hzRestPos — that's the butterfly-parity ROOT-JOINT-LOCAL space,
        //    and a rig can carry its whole unit conversion in the root joint SCALE (japan koi: Maya cm rig, root scale
        //    ~0.0127 — the RENDER path shrinks the 90m rest verts through the animated root joint every frame, but a
        //    PhysX kinematic pose is RIGID: scale is DROPPED, so the shipped collider stayed a ~90m monster sweeping
        //    the whole map). Ship the t=0 WORLD rest verts (m.positions — the fish at its real size and place, exactly
        //    what the editor preview collides with) on a STATIC SIBLING entity instead: correct place, correct size,
        //    device-safe. (The kinematic follow for skinned rigs was never device-validated —
        //    project_hsr_v205_kinematic_collider is still OPEN — so a resting collider beats a berserk one.) ──
        if (m.wantCollider) {
            const std::vector<float>& cpos = useHz ? m.positions : *staticPos;
            if (cpos.size() >= 9 && m.indices.size() >= 3) {
                auto sebd = cookNavmeshSEBD(cpos.data(), (uint32_t)(cpos.size()/3), m.indices.data(), (uint32_t)(m.indices.size()/3));
                if (sebd.size() > 64 && sebd[0]=='S'&&sebd[1]=='E'&&sebd[2]=='B'&&sebd[3]=='D') {
                    std::string pCol = std::string(base) + ".collider.phys/phys"; AssetKey3 colK = keyForPath(pCol);
                    assets.push_back({ pCol, colK.tgt, sebd, colK, TYPE_PHSX, TYPE_3MSH });
                    std::string colComp = comp("ColliderMeshPlatformComponent", 2, std::string("{\"meshAsset\":") + refJson(colK) + "}")
                                        + "," + comp("PhysicsBodyPlatformComponent", 7, "{\"type\":\"StaticCollision\"}");
                    // ── KINEMATIC-PROMOTION TRAP (the bluehills windmill_tails "stuck at spawn" bug) ──────────────────────
                    // libshell promotes a PhysicsBody that sits on an ANIMATED entity to a KINEMATIC body and drives its
                    // KinematicPose from the entity's pose source. Entity-pose meshes (usePose → top-level "poseAnimation")
                    // DO carry such a driver, so their collider follows the animation — keep it on the SAME entity. But a
                    // SHADER-ONLY animation (useRot/useTrans/useScale/usePulse/useUvScroll/useVat: the motion lives in the
                    // material getTime() shader while the ENTITY transform is static) has NO pose driver — libshell still
                    // promotes the body to kinematic and, finding nothing to pose it, COLLAPSES the body AND the entity
                    // transform the visual mesh rides on to the kinematic origin == spawn (m161 windmill_tails). And a
                    // SKINNED mesh (useHz) ships WORLD verts now (see cpos above), so the animated entity must not re-pose
                    // them either. FIX: emit the collider on a SEPARATE STATIC sibling entity (no animation → never
                    // promoted). Transform: hzPos/hzScl — identity for useHz (verts are world), poseCtr for the centered
                    // shader-anim cases (their SEBD is in the mesh's local/centered space; the ±few-degree shader sway is
                    // negligible for collision). project_hsr_v205_kinematic_collider.
                    bool shaderAnim = useRot || useTrans || useScale || usePulse || useUvScroll || useVat;
                    bool poseDriven = usePose;
                    if ((shaderAnim || useHz) && !poseDriven) {
                        std::string cid = makeUuid(rng);
                        char ctf[256]; snprintf(ctf,sizeof ctf,
                            "{\"localPosition\":{\"x\":%g,\"y\":%g,\"z\":%g},\"localRotation\":{\"x\":0,\"y\":0,\"z\":0},\"localScale\":{\"x\":%g,\"y\":%g,\"z\":%g}}",
                            hzPos[0],hzPos[1],hzPos[2], hzScl[0],hzScl[1],hzScl[2]);
                        entities += "," + std::string("{\"id\":\"") + cid + "\",\"name\":\"" + nm + "_col\",\"components\":["
                                  + comp("TransformPlatformComponent", 1, ctf) + "," + colComp + "],\"attributes\":[]}";
                        rels += (rels.empty() ? std::string() : std::string(",")) + relChildOf(cid, rootId);
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' %s collider -> %zuB SEBD (%zu tris) on a STATIC SIBLING entity @(%.1f,%.1f,%.1f) (no kinematic collapse)\n", i, m.name.c_str(), useHz?"skinned (world-rest)":"shader-anim", sebd.size(), m.indices.size()/3, hzPos[0],hzPos[1],hzPos[2]);
                    } else {
                        if (!extraComp.empty()) extraComp += ",";
                        extraComp += colComp;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' collider -> %zuB SEBD (%zu tris) on the mesh entity (%s)\n", i, m.name.c_str(), sebd.size(), m.indices.size()/3, poseDriven ? "kinematic, pose-driven" : "static");
                    }
                } else fprintf(stderr, "[COOK] m%03zu '%s' animated-collider PhysX cook FAILED\n", i, m.name.c_str());
            }
        }
        // ── PER-INSTANCE VAT (the vatunlit `instance` UBO set2 bind1): emit the source's atlasCellIndex/animTrackIndex/
        //    animRateFactor/animTimeOffset as a MaterialPropertyOverridesPlatformComponent so each cooked VAT creature
        //    plays its OWN track + phase (else all default to track 0 = the turtle-sync bug). Component class+version+
        //    element format verified from official oceanarium foliagevats. ──
        if (useVat && (m.atlasCellIndex>=0.f || m.vatInstTrackIndex>=0.f || m.vatInstRateFactor>=0.f || m.vatInstTimeOffset>=0.f)) {
            auto cpv=[&](const char* pn, float v){ char b[220]; snprintf(b,sizeof b,
                "{\"class\":\"horizon::renderer::ConstantMaterialParameter\",\"data\":{\"parameterName\":\"%s\",\"value\":{\"x\":%g,\"y\":0,\"z\":0,\"w\":0}}}", pn, v); return std::string(b); };
            std::vector<std::string> cpl;
            if (m.atlasCellIndex   >= 0.f) cpl.push_back(cpv("instance.atlasCellIndex", m.atlasCellIndex));
            if (m.vatInstTrackIndex>= 0.f) cpl.push_back(cpv("instance.animTrackIndex", m.vatInstTrackIndex));
            if (m.vatInstRateFactor>= 0.f) cpl.push_back(cpv("instance.animRateFactor", m.vatInstRateFactor));
            if (m.vatInstTimeOffset>= 0.f) cpl.push_back(cpv("instance.animTimeOffset", m.vatInstTimeOffset));
            if (!cpl.empty()) { std::string arr; for (size_t k=0;k<cpl.size();k++){ if(k)arr+=","; arr+=cpl[k]; }
                if (!extraComp.empty()) extraComp += ",";
                extraComp += comp("MaterialPropertyOverridesPlatformComponent", 1, std::string("{\"constantParameters\":[")+arr+"]}"); }
        }
        std::string poseKv = usePose ? poseAnimationValue(poseCtr, m.poseStartScale, m.poseEndScale, m.poseDuration, m.poseTransDelta) : std::string();
        // MeshPlatformComponent **v5** for ALL meshes (static AND skinned). GROUND TRUTH: the working calming butterfly
        // skinned entity uses Mesh v5 (extracted from calming_butterflies.hstf). v6 was a guess that contradicts it.
        // FAR-BACKDROP -> skybox pass (no far-clip). The main camera far-clips distant geometry; routing far meshes through
        // the skybox pass (like official vistas) makes them render. Heuristic: geometry centroid distance > HSR_SKYBOX_DIST
        // (default 80m). Only plain STATIC meshes (animated/skinned keep their normal walkable path).
        bool skybox = false;
        if (!dclamp && !fit && !useHz && !usePose && !usePulse && !useRot && !useTrans) {   // depth-clamp/fitclip make far geometry render in place -> no skybox needed (and it's camera-locked/fragile)
            // skyboxEntityJson now emits the FULL vista recipe (colorTexture+reflectionMap+MaterialPropertyOverrides) so the
            // backdrop renders (no longer black). Enabled by the editor "Far backdrop -> skybox" toggle => HSR_SKYBOX_DIST=<m>.
            // Far backdrop beyond this radius is drawn in the depth-clamped skybox pass, EXEMPT from the hard
            // PortalStereoCamera far=5000 clip (device-proven the only escape — official homes skybox their distance, see
            // [[project_hsr_eye_subcamera_farclip]]). Only plain STATIC meshes (animated/skinned keep the normal walkable path).
            if (m.skybox) skybox = true;   // EXPLICIT per-mesh mark (editor right-click "Make skybox backdrop") — always wins
            const char* sd = std::getenv("HSR_SKYBOX_DIST"); float thr = sd ? (float)atof(sd) : 0.f;
            if (!skybox && thr > 0) { size_t np = m.positions.size()/3; double cc[3]={0,0,0};
                for (size_t v=0; v+2<m.positions.size(); v+=3) for(int k=0;k<3;k++) cc[k]+=m.positions[v+k];
                if (np){ float cx=(float)(cc[0]/np), cy=(float)(cc[1]/np), cz=(float)(cc[2]/np); skybox = sqrtf(cx*cx+cy*cy+cz*cz) > thr; } } }
        // ── FAR-CLIP DIAGNOSTIC: the device's PortalStereoCamera hard-clips at far=5000m — geometry past it DISAPPEARS on
        //    device unless it's a SkyboxPlatformComponent (far-clip-exempt) or depth-clamped. Warn the cooker so they KNOW a
        //    far mesh will clip in the cook and can mark it (right-click "Make skybox backdrop") — see project_hsr_eye_subcamera_farclip.
        if (!skybox && !dclamp && !fit && !useHz) {
            float md=0.f;   // max vertex distance from origin (~the device camera/spawn)
            for (size_t v=0; v+2<m.positions.size(); v+=3){ float dx=m.positions[v], dy=m.positions[v+1], dz=m.positions[v+2]; float d=dx*dx+dy*dy+dz*dz; if(d>md)md=d; }
            // The device's SceneComponent DEFAULT far is 500m (IDA crack @0x252A8DC); the cook extends it via the
            // ScenePlatformComponent (farClippingPlane). Geometry beyond 500m renders ONLY if that component applies on device.
            if (md > 500.f*500.f) fprintf(stderr, "[COOK] far-clip note: mesh '%s' reaches %.0fm (> the device 500m DEFAULT far) -> renders only via the cook's ScenePlatformComponent far extension; if that fails to parse on device it CLIPS. Mark 'Make skybox backdrop' if it's a backdrop.\n", m.name.c_str(), sqrtf(md));
        }
        // GENERIC sky-DOME fix: the SkyboxPlatformComponent binds the dome's 2D texture as a CUBEMAP (colorTexture +
        // reflectionMap) — the shell's skybox shader samples it as a cube and renders a 2D equirect/dome texture BLACK
        // (cyberhome mat7 = the sunset dome). A 2D-textured backdrop renders correctly as a NORMAL far mesh: the cook's
        // 150000 farClippingPlane already covers it (PROVEN — the cyberhome city buildings at 7–9.5km render fine as
        // normal meshes). So render skybox-marked meshes as normal far meshes by default; HSR_SKYBOX_COMPONENT forces the
        // cubemap component back (only correct for an actual cubemap skybox). This fixes ANY 2D sky dome, preview + device.
        if (skybox && std::getenv("HSR_SKYBOX_COMPONENT")) {
            AssetKey3 stex = texK.tgt ? texK : whiteK; if (!texK.tgt) whiteUsed = true;
            entities += "," + skyboxEntityJson(mid, nm, meshK, stex);
            anySkybox = true;
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' -> SKYBOX component (cubemap)\n", i, m.name.c_str());
        } else {
            if (skybox && std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%03zu '%s' -> far backdrop as NORMAL mesh (2D dome; farClip 150000 covers it — was the black cubemap skybox)\n", i, m.name.c_str());
            entities += "," + entityJson(mid, nm, hzPos, rot0, hzScl, meshK, { matK }, AssetKey3{0,0,0}, extraComp, 5, false, poseKv);
        }
        rels += (rels.empty() ? std::string() : std::string(",")) + relChildOf(mid, rootId);
    }
    if (rels.empty()) return {};
    if (whiteUsed) assets.push_back({ pWhite, whiteK.tgt, whiteTex, whiteK });
    // The SkyboxPlatformComponent makes the shell load its built-in skybox SURFACE shader + a texture from the SHELL package
    // 30952996677677480 — which a STANDALONE cooked env can't reach (device: "Asset is missing ... ShaderAsset/TextureAsset"
    // -> skybox draws BLACK). SHIP them in OUR scene.zip under the EXACT canonical keys the shell requests so they resolve
    // from us. Shader bytes = the shell's own skyboxwithglobalfog.surface (extracted from a vista). [[project_hsr_eye_subcamera_farclip]]
    if (anySkybox) {
        auto skyShader = readFileBytes("cooker/skybox_surface.bin");
        if (!skyShader.empty()) {
            AssetKey3 skyShK{ 30952996677677480ull, 820840225639963422ull, TGT_SURFACE };
            AssetKey3 skyTxK{ 30952996677677480ull, 414607546753497478ull, TGT_TEX };
            assets.push_back({ "meta/skysys/skyboxfog.surface/shader", TGT_SURFACE, skyShader, skyShK });
            assets.push_back({ "meta/skysys/skybox.tex/tex",           TGT_TEX,     whiteTex,  skyTxK });
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] shipped shell skybox surface shader (%zuB)+tex under canonical keys (fixes missing-asset BLACK skybox)\n", skyShader.size());
        } else fprintf(stderr, "[COOK] WARN: cooker/skybox_surface.bin missing -> skybox will be BLACK (no surface shader). Extract it from a vista.\n");
    }
    // FALLBACK nav bounds for TILE-BASED envs (sonic_schoolhouse = hundreds of 4-vert wall/floor quads, NONE >=100
    // verts -> the navCand heuristic bounded nothing -> no ground collider -> player FALLS THROUGH the floor). If no
    // nav candidate set the bounds, accumulate ALL non-sky geometry so the collider still covers the walkable area.
    if (smx[0] < smn[0]) {
        for (const auto& m : meshesV) {
            if (m.positions.size() < 9) continue;
            if (m.name.find("sky") != std::string::npos || m.name.find("Sky") != std::string::npos) continue;
            for (size_t v=0; v+2<m.positions.size(); v+=3) for(int k=0;k<3;k++){ float p=m.positions[v+k]; if(p<smn[k])smn[k]=p; if(p>smx[k])smx[k]=p; }
        }
        if (std::getenv("HSR_VERBOSE") && smx[0]>=smn[0]) fprintf(stderr, "[COOK] nav fallback (tile env): bounds (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n", smn[0],smn[1],smn[2],smx[0],smx[1],smx[2]);
    }
    // SPAWN/FLOOR ANCHOR — NOT the editor camera (it's an arbitrary VIEWING pose, never where the player belongs).
    // The spawn is EITHER a user-placed Spawn Point (handled in the scene-items loop, takes priority) OR an automatic
    // one HERE: the centroid of the user navmesh = a guaranteed point on the real walkable floor. No navmesh -> the
    // scene-bounds center at the scene floor (min Y), refined by the smart-floor heightfield below. (No camera.)
    float gx = (smn[0]+smx[0])*0.5f, gz = (smn[2]+smx[2])*0.5f, gy = smn[1];
    { // anchor = a CENTRAL point on the navmesh FLOOR band. A plain centroid Y is wrong when the navmesh includes wall
      // triangles (their height pulls it up off the floor), so: centroid XZ, then the nearest navmesh vert in the LOWEST
      // 40% Y band (= floor, not walls/upper levels). This is a guaranteed walkable floor point — no camera involved.
      double ax=0,az=0; size_t an=0; float nmin=1e30f, nmax=-1e30f;
      for (const auto& si : sceneItems) if (si.type==sitem::NAVMESH)
          for (size_t v=0; v+2<si.navVerts.size(); v+=3){ ax+=si.navVerts[v]; az+=si.navVerts[v+2]; ++an; float y=si.navVerts[v+1]; if(y<nmin)nmin=y; if(y>nmax)nmax=y; }
      if (an){ float cx=(float)(ax/an), cz=(float)(az/an), band=nmin+(nmax-nmin)*0.4f, best=1e30f; bool got=false;
        for (const auto& si : sceneItems) if (si.type==sitem::NAVMESH)
            for (size_t v=0; v+2<si.navVerts.size(); v+=3){ float y=si.navVerts[v+1]; if (y>band) continue;   // floor band only (skip walls/upper)
                float dx=si.navVerts[v]-cx, dz=si.navVerts[v+2]-cz, d=dx*dx+dz*dz; if (d<best){ best=d; gx=si.navVerts[v]; gy=y; gz=si.navVerts[v+2]; got=true; } }
        if (!got){ gx=cx; gz=cz; gy=nmin; } } }
    float ex = smx[0]-smn[0], ez = smx[2]-smn[2], ext = ex > ez ? ex : ez;
    float gs = ext > 1.f ? (ext / 12.0f) * 2.0f : 8.0f;         // BIG invisible plane (nuxd circular floor ~12.8m x2 the scene)
    float spawnSurfY = gy;   // road/floor surface Y under the spawn (set from the heightfield) -> spawn ON it, not above
    bool userNav = false;    // if the user placed a navmesh, THAT is the ground — skip the redundant auto smart-floor
    if (!std::getenv("HSR_NONAV"))   // diag: HSR_NONAV ignores user navmeshes -> tests Meta's realfloor collider in isolation
        for (const auto& si : sceneItems) if (si.type == sitem::NAVMESH && si.navVerts.size() >= 9) { userNav = true; break; }
    auto phys = readFileBytes("cooker/realfloor_phys.bin");
    if (!phys.empty() && smx[0] >= smn[0]) {
        std::string pPhys = MH + "/ground.phys/phys"; AssetKey3 colliderK = keyForPath(pPhys);
        assets.push_back({ pPhys, colliderK.tgt, phys, colliderK, TYPE_PHSX, TYPE_3MSH });
        int nTiles = 0; bool flatFloor = std::getenv("HSR_FLATFLOOR") != nullptr;
        // ── SMART FLOOR (default): rasterize the scene's WALKABLE (up-facing) triangles into an XZ heightfield, then
        //    drop one flat collider tile per occupied cell at its surface height. You walk the ACTUAL floor/road shape
        //    (Dragon Ball Snake Way's winding road) at the right height — not a generic flat disk over the void. Gaps
        //    (cells with no walkable geometry) get no tile, so you can't walk on air. Rooms get their floor; the disk
        //    is the fallback when nothing walkable is found (e.g. a fully-curved env). HSR_FLATFLOOR forces the disk. ──
        if (!flatFloor) {
            const float planeBase = 12.8f;                          // realfloor_phys.bin plane size
            float headY = gy + 2.5f;                               // ignore surfaces above ~head ABOVE THE FLOOR (roofs/ceilings) — floor-relative, NOT the camera
            // PASS 1: collect the WALKABLE triangles + their own XZ bounds (the road footprint). Gridding THAT instead
            // of the inflated whole-scene nav bounds gives far finer cells that hug the road, not a coarse disk.
            struct WT { float pa[3],pb[3],pc[3]; };
            std::vector<WT> wt; float wmnx=1e30f,wmnz=1e30f,wmxx=-1e30f,wmxz=-1e30f;
            for (const auto& m : meshesV) {
                if (m.positions.size() < 9 || m.indices.size() < 3) continue;
                if (m.name.find("sky")!=std::string::npos || m.name.find("Sky")!=std::string::npos) continue;
                for (size_t t=0; t+2<m.indices.size(); t+=3) {
                    uint32_t a=m.indices[t], b=m.indices[t+1], c=m.indices[t+2];
                    if ((size_t)c*3+2 >= m.positions.size() || (size_t)a*3+2 >= m.positions.size() || (size_t)b*3+2 >= m.positions.size()) continue;
                    const float* pa=&m.positions[a*3]; const float* pb=&m.positions[b*3]; const float* pc=&m.positions[c*3];
                    float ux=pb[0]-pa[0],uy=pb[1]-pa[1],uz=pb[2]-pa[2], vx=pc[0]-pa[0],vy=pc[1]-pa[1],vz=pc[2]-pa[2];
                    float ny=uz*vx-ux*vz; float nl=sqrtf((uy*vz-uz*vy)*(uy*vz-uz*vy)+ny*ny+(ux*vy-uy*vx)*(ux*vy-uy*vx));
                    // walkable-slope cutoff: loosened 0.5->0.15 (keep up to ~81deg slopes) so curved/hilly envs don't
                    // leave colander holes; HSR_NAVSLOPE overrides (0 = include every face). Matches editor bakeNavGeometry.
                    float floorSlope=0.15f; if(const char* e=std::getenv("HSR_NAVSLOPE")){ float s=(float)atof(e); if(s>=0.0f) floorSlope=s; }
                    if (nl<1e-6f || (floorSlope>0.0f && fabsf(ny)/nl < floorSlope)) continue;   // not walkable enough
                    float cy=(pa[1]+pb[1]+pc[1])/3.f; if (cy > headY) continue;   // above head -> roof/floating, skip
                    WT w; for(int k=0;k<3;k++){w.pa[k]=pa[k];w.pb[k]=pb[k];w.pc[k]=pc[k];} wt.push_back(w);
                    for (const float* p : {pa,pb,pc}){ if(p[0]<wmnx)wmnx=p[0]; if(p[0]>wmxx)wmxx=p[0]; if(p[2]<wmnz)wmnz=p[2]; if(p[2]>wmxz)wmxz=p[2]; }
                }
            }
            if (userNav) { nTiles=1; if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] smart floor skipped (user navmesh is the ground)\n"); goto floorFallback; }
            if (wt.empty() || wmxx<=wmnx || wmxz<=wmnz) goto floorFallback;
            {
            float ex2=wmxx-wmnx, ez2=wmxz-wmnz, ext2=ex2>ez2?ex2:ez2;
            // grid the ROAD footprint at ~3m cells. The cap is intentionally HIGH (the device handles dense tiling fine) so
            // the navmesh follows the real surface at full resolution; HSR_NAVTILES overrides the per-axis cap entirely.
            int navCap=256; if (const char* nc=std::getenv("HSR_NAVTILES")){ int v=atoi(nc); if(v>=4) navCap=v; }
            int N=(int)(ext2/3.0f); if(N<4)N=4; if(N>navCap)N=navCap;
            float cell=ext2/(float)N; if(cell<0.1f)cell=0.1f;
            std::vector<float> hf((size_t)N*N, -1e30f);            // per-cell highest walkable surface Y
            for (const WT& w : wt) {
                float cy=(w.pa[1]+w.pb[1]+w.pc[1])/3.f;
                float tminx=fminf(w.pa[0],fminf(w.pb[0],w.pc[0])), tmaxx=fmaxf(w.pa[0],fmaxf(w.pb[0],w.pc[0]));
                float tminz=fminf(w.pa[2],fminf(w.pb[2],w.pc[2])), tmaxz=fmaxf(w.pa[2],fmaxf(w.pb[2],w.pc[2]));
                int x0=(int)((tminx-wmnx)/cell), x1=(int)((tmaxx-wmnx)/cell), z0=(int)((tminz-wmnz)/cell), z1=(int)((tmaxz-wmnz)/cell);
                if(x0<0)x0=0; if(z0<0)z0=0; if(x1>=N)x1=N-1; if(z1>=N)z1=N-1;
                for(int cz=z0;cz<=z1;cz++) for(int cx=x0;cx<=x1;cx++){ float& h=hf[(size_t)cz*N+cx]; if(cy>h) h=cy; }
            }
            { int scx=(int)((gx-wmnx)/cell), scz=(int)((gz-wmnz)/cell);   // spawn ON the road surface at the camera XZ
              if (scx>=0&&scx<N&&scz>=0&&scz<N && hf[(size_t)scz*N+scx] > -1e29f) spawnSurfY = hf[(size_t)scz*N+scx]; }
            (void)planeBase;
            // ── ONE merged PhysX collision mesh from ALL walkable triangles — follows the road EXACTLY at full
            //    resolution and is a SINGLE collider entity. Per-tile entities DON'T scale: snakeway's 795m footprint
            //    = 10521 tiles, and the device renders then CRASHES processing thousands of colliders. One SEBD mesh
            //    (the device handles 200k-tri colliders fine) = no cap, no crash, exact surface. ──
            std::vector<float> nv; std::vector<uint32_t> ni; nv.reserve(wt.size()*9); ni.reserve(wt.size()*3);
            for (const WT& w : wt){ uint32_t b=(uint32_t)(nv.size()/3);
                nv.push_back(w.pa[0]);nv.push_back(w.pa[1]);nv.push_back(w.pa[2]);
                nv.push_back(w.pb[0]);nv.push_back(w.pb[1]);nv.push_back(w.pb[2]);
                nv.push_back(w.pc[0]);nv.push_back(w.pc[1]);nv.push_back(w.pc[2]);
                ni.push_back(b); ni.push_back(b+1); ni.push_back(b+2); }
            auto sfSebd = cookNavmeshSEBD(nv.data(), (uint32_t)(nv.size()/3), ni.data(), (uint32_t)(ni.size()/3));
            if (sfSebd.size()>64 && sfSebd[0]=='S'&&sfSebd[1]=='E'&&sfSebd[2]=='B'&&sfSebd[3]=='D') {
                std::string pSF=MH+"/smartfloor.phys/phys"; AssetKey3 sfK=keyForPath(pSF);
                assets.push_back({ pSF, sfK.tgt, sfSebd, sfK, TYPE_PHSX, TYPE_3MSH });
                std::string gid=makeUuid(rng); float gp[3]={0,0,0}, gsc[3]={1,1,1};
                entities += "," + colliderGroundEntityJson(gid, gp, gsc, sfK);
                rels += "," + relChildOf(gid, rootId); nTiles=1;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] smart floor: ONE merged collider %zuB SEBD (%zu walkable tris, footprint %.0fx%.0fm)\n", sfSebd.size(), wt.size(), ex2, ez2);
            } else {
                // SEBD unavailable (PhysX off / device-incompatible — the usual case). DON'T fall to the tiny origin disk
                // (that's the "collision is outside the map" + "you spawn off the floor & fall" bug). Emit the PROVEN
                // device-built ColliderBox GRID from the SAME walkable tris, so the floor covers the ACTUAL walkable
                // footprint at the real surface heights. [[project_hsl_navmesh_colliderbox]]
                int fbIdx = 600000;
                auto emitFloorBox=[&](const float c[3],const float he[3]){
                    sitem::Item bi; bi.type=sitem::BOXCOL; bi.name="FloorBox";
                    bi.pos[0]=c[0]; bi.pos[1]=c[1]; bi.pos[2]=c[2];
                    bi.half[0]=he[0]; bi.half[1]=he[1]; bi.half[2]=he[2];
                    bi.scale[0]=bi.scale[1]=bi.scale[2]=1.f;
                    std::string bid=sitem::uuid(fbIdx);
                    entities += "," + sitem::itemEntityJson(bi, fbIdx); ++fbIdx;
                    rels += "," + relChildOf(bid, rootId);
                };
                if (std::getenv("HSR_NOAUTOFLOOR")) goto floorFallback;   // toggle OFF: no auto grid either
                auto gb = navmeshToBoxes(nv, cell, 1.5f, 1500);     // grid the walkable verts -> one device box per occupied cell at its surface Y
                for (auto& b : gb){ float c[3]={b[0],b[1],b[2]}, he[3]={b[3],b[4],b[5]}; emitFloorBox(c,he); }
                if (!gb.empty()) { nTiles=1;
                    if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] smart floor -> %zu ColliderBox grid (SEBD unavailable; covers the map, %zu walkable tris, footprint %.0fx%.0fm)\n", gb.size(), wt.size(), ex2, ez2);
                } else if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] smart floor: no walkable boxes -> disk fallback\n");
            }
            }
            floorFallback: ;
        }
        if (std::getenv("HSR_NOAUTOFLOOR")) { nTiles = 1; flatFloor = false; }   // editor "Auto floor collision" OFF: ship NO generated floor at all (the invisible wall the items list never showed)
        if (flatFloor || nTiles == 0) {   // fallback: a single flat disk at the camera foot (the old behaviour)
            std::string gid = makeUuid(rng); float gp[3]={gx,gy,gz}, gsc[3]={gs,1.f,gs};
            entities += "," + colliderGroundEntityJson(gid, gp, gsc, colliderK);
            rels += "," + relChildOf(gid, rootId);
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] flat-disk floor @ (%.1f,%.1f,%.1f) ext=%.1f\n", gx,gy,gz,gs);
        }
    }
    // ── STOLEN chair-icon (haven2025 home/seating): the "Circle" button mesh + chair.png. The mesh has TWO embedded
    //    asset refs — @288 = its SHADER (RENDSHAD), @344 = its TEXTURE (RENDTXTR). haven's keys won't resolve in our
    //    package, so REPOINT them (the _ckcook pattern): @288 -> our shipped unlit shader, @344 -> our chair.png. The
    //    mesh's OWN key = keyForPath(our path). A chair_location seat needs a "ChairIcon" CHILD carrying THIS mesh or the
    //    HomeLocomotionSystem (sub_F78760) null-derefs the icon mesh and CRASHES. ──
    std::string pIcMesh = MH + "/chair_icon.mesh/mesh"; AssetKey3 icMeshK = keyForPath(pIcMesh);
    bool iconOk = false;
    {
        auto icMesh = readFileBytes("cooker/chair_icon_mesh.bin");
        auto icTex  = readFileBytes("cooker/chair_icon_tex.bin");
        if (icMesh.size() > 376 && icTex.size() > 64) {
            std::string pIcTex = MH + "/chair_icon.tex/tex"; AssetKey3 icTexK = keyForPath(pIcTex);
            // OFFICIAL icon shader: haven's icon mesh references shaders/billboard.surface (billboards to the
            // player + carries the pop/hover animation). Repointing it to our plain unlit was the "static dot
            // with 0 animations" - ship haven's billboard shader and point the icon at OUR copy of IT.
            AssetKey3 icShK = shaderK;   // fallback: plain unlit (icon still shows, just static)
            {
                auto icShad = readFileBytes("cooker/billboard.surface.bin");
                if (icShad.size() > 64) {
                    std::string pIcSh = MH + "/shaders/billboard.surface/shader";
                    icShK = keyForPath(pIcSh);
                    assets.push_back({ pIcSh, icShK.tgt, icShad, icShK });
                } else fprintf(stderr, "[COOK] WARNING: cooker/billboard.surface.bin missing -> chair icon ships STATIC (unlit) instead of haven billboard pop shader%s", "\n");
            }
            memcpy(icMesh.data()+288, &icShK.pkg, 8); memcpy(icMesh.data()+296, &icShK.ing, 8); memcpy(icMesh.data()+304, &icShK.tgt, 4);  // @288 SHADER -> haven billboard (fallback unlit)
            memcpy(icMesh.data()+344, &icTexK.pkg, 8); memcpy(icMesh.data()+352, &icTexK.ing, 8); memcpy(icMesh.data()+360, &icTexK.tgt, 4);     // @344 TEXTURE -> our chair.png
            assets.push_back({ pIcMesh, icMeshK.tgt, icMesh, icMeshK });
            assets.push_back({ pIcTex,  icTexK.tgt,  icTex,  icTexK });
            iconOk = true;
            if (std::getenv("HSR_VERBOSE")||std::getenv("HSR_NAVDBG")) fprintf(stderr,"[COOK] stole chair-icon mesh(%zuB)+tex(%zuB), repointed shader@288+tex@344\n", icMesh.size(), icTex.size());
        }
    }
    // ── editor-authored scene items -> the EXACT haven2025 component entities (spawn/chair/box/wall/hotspot).
    //    NAVMESH items are PhysX-cooked separately (below). Each becomes a child of Root. ──
    int userLocalSpawn = 0, sidx = 700000;
    for (const auto& si : sceneItems) {
        if (si.type == sitem::NAVMESH) continue;
        // SAFETY: a CHAIR's chair_location locator makes the HomeLocomotionSystem build a chair-button that DEREFERENCES
        // the ChairIcon child mesh — ship a chair without that icon and the device null-derefs -> CRASH LOOP. The icon is
        // embedded (embassets), so iconOk is normally true; if it's ever unavailable, SKIP the chair rather than crash.
        if (si.type == sitem::CHAIR && !iconOk) {
            fprintf(stderr, "[COOK] WARNING: chair-icon mesh unavailable -> SKIPPED chair '%s' (a chair without its ChairIcon child crash-loops the device). Rebuild with cooker/chair_icon_mesh.bin+chair_icon_tex.bin embedded.\n", si.name.c_str());
            continue;
        }
        std::string iid = sitem::uuid(sidx);
        // HOTSPOT reuses the stolen icon mesh as its visible "dot"; pass its ref as meshAssetJson.
        // SPAWN: the shell teleports the player's HEAD/EYE to the spawn transform (IDA: SpawnPointPlatformComponent is
        // pure data + nativeTeleportToCoordinates passes the coords verbatim; device-proven the player ends up WITH HEAD
        // at the spawn Y). The editor places/draws the spawn at FLOOR level, so raise the emitted Y by eye height (1.6,
        // the same constant the editor preview uses: eye = pos + 1.6) → the player's feet land on the placed floor.
        sitem::Item si2 = si; if (si.type == sitem::SPAWN) si2.pos[1] += 1.6f;
        std::string ej = (si.type == sitem::HOTSPOT && iconOk) ? sitem::itemEntityJson(si2, sidx, refJson(icMeshK))
                                                               : sitem::itemEntityJson(si2, sidx);
        if (ej.empty()) continue;
        ++sidx;
        entities += "," + ej; rels += "," + relChildOf(iid, rootId);
        // CHAIR: the chair-button NEEDS a "ChairIcon" CHILD carrying the stolen mesh (at the seat 0,0.25,0) or the
        // HomeLocomotionSystem update (sub_F78760) null-derefs the icon mesh and CRASHES. This is the recipe that makes
        // the chairs INTERACTABLE (DEVICE-PROVEN). The chair-button only warps you to the ChairIcon (no real sit).
        if (si.type == sitem::CHAIR && iconOk) {
            std::string iconId = sitem::uuid(sidx); ++sidx;
            // HAVEN-EXACT ChairIcon transform (hpi_locators.hstf): ONLY localPosition, no rotation/scale keys
            // ("exactly take whatever haven2025 has - that is the official way"). Height stays adjustable via
            // the same localPosition.y (buried icons); a scale key is added ONLY when the user deviates from 1.
            char icb[220];
            if (si.iconScale != 1.f)
                snprintf(icb, sizeof icb, "\"localPosition\":{\"x\":0,\"y\":%.4g,\"z\":0},\"localScale\":{\"x\":%.4g,\"y\":%.4g,\"z\":%.4g}", si.iconY, si.iconScale, si.iconScale, si.iconScale);
            else
                snprintf(icb, sizeof icb, "\"localPosition\":{\"x\":0,\"y\":%.4g,\"z\":0}", si.iconY);
            std::string iconTf = sitem::comp("horizon::platform_api::TransformPlatformComponent", 2, icb);
            std::string iconMeshComp = sitem::comp("horizon::platform_api::MeshPlatformComponent", 11,
                std::string("\"mesh\":") + refJson(icMeshK) + ",\"isVisibleSelf\":false");
            entities += ",{\"id\":\"" + iconId + "\",\"name\":\"ChairIcon\",\"components\":[" + iconTf + "," + iconMeshComp + "],\"attributes\":[]}";
            rels += "," + relChildOf(iconId, iid);
        }
        if (si.type == sitem::SPAWN && si.allowStart && si.isLocal) ++userLocalSpawn;
        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] item -> %s\n", sitem::metaName(si.type));
    }
    // spawn where the V79 camera/view is (XZ), at ground level — UNLESS the user placed their own local spawn.
    // AUTOMATIC spawn (only when the user placed NO Spawn Point): at the navmesh/scene ANCHOR (gx,gz), NOT the camera.
    // Y = floor + EYE HEIGHT: the shell positions the player AT the spawn transform (head/eye ref), NOT the feet
    // (device-proven: a floor-Y spawn → player UNDER the map), so +1.6 puts the FEET on the floor. (Matches the editor
    // preview's eye = pos + 1.6.)
    float spawnPos[3] = { gx, spawnSurfY + 1.6f, gz };
    if (userLocalSpawn == 0) {
        std::string spawnId = makeUuid(rng);
        entities += "," + spawnPointEntityJson(spawnId, spawnPos);
        rels += "," + relChildOf(spawnId, rootId);
    }
    // ── NAVMESH items: PhysX-cook the baked WORLD triangles into haven2025's stock-PhysX-4.x SEBD collider (via
    //    physx_cook.exe), ship it as a ColliderMesh + StaticCollision entity — a REAL walkable mesh, not flat tiles. ──
    for (const auto& si : sceneItems) {
        if (std::getenv("HSR_NONAV")) break;   // diag: skip custom navmesh colliders (test Meta's realfloor in isolation)
        if (si.type != sitem::NAVMESH || si.navVerts.size()<9) continue;
        // DEFAULT: device-safe ColliderBox floor (always works — built from halfExtents, no serialized struct to mismatch).
        // The REAL trimesh (opt-in HSR_NAVTRIMESH) needs Meta's exact 4.1.64 PhysX libs OR the in-progress hand-roll —
        // public 4.1.2 either crashes (native stamp) or loads-but-falls-through (4.1.64 stamp, geometry mis-read). So boxes
        // are the default until the hand-rolled 4.1.64 SEBD lands.
        if (std::getenv("HSR_NAVTRIMESH") && si.navIdx.size()>=3) {
            prog(0.79f, "PhysX-cooking navmesh");
            auto sebd = cookNavmeshSEBD(si.navVerts.data(), (uint32_t)(si.navVerts.size()/3), si.navIdx.data(), (uint32_t)(si.navIdx.size()/3));
            if (const char* dp=std::getenv("HSR_DUMP_PHYS")) { FILE* f=fopen(dp,"wb"); if(f){ fwrite(sebd.data(),1,sebd.size(),f); fclose(f); fprintf(stderr,"[COOK] dumped our navmesh SEBD (%zuB) -> %s\n", sebd.size(), dp); } }
            if (sebd.size()>64 && sebd[0]=='S'&&sebd[1]=='E'&&sebd[2]=='B'&&sebd[3]=='D') {
                std::string pNav=MH+"/navmesh_"+std::to_string(sidx)+".phys/phys"; AssetKey3 navK=keyForPath(pNav);
                assets.push_back({ pNav, navK.tgt, sebd, navK, TYPE_PHSX, TYPE_3MSH });
                std::string nid=sitem::uuid(sidx); std::string ej=sitem::itemEntityJson(si, sidx, refJson(navK)); ++sidx;
                entities += "," + ej; rels += "," + relChildOf(nid, rootId);
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] navmesh -> REAL SEBD trimesh collider (%zuB, %zu tris) — every triangle solid\n", sebd.size(), si.navIdx.size()/3);
                continue;   // SEBD emitted -> skip the box fallback
            }
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] navmesh SEBD empty (PhysX off? build -DHSR_HAVE_PHYSX=ON) -> ColliderBox fallback\n");
        }
        // FALLBACK: ColliderBox shapes (device-PhysX-built from halfExtents -> always compatible). PREFER one TILTED box
        // per walkable triangle (REUSES the real mesh tris -> exact shape/height/tilt). Fall back to the axis-aligned
        // height grid only if the mesh is too dense (or HSR_NAVGRID) to keep the box/entity count sane.
        prog(0.79f, "Building navmesh collision boxes");
        size_t ntri = si.navIdx.size()/3;
        int triCap = 15000; if(const char* e=std::getenv("HSR_NAVTRICAP")){ int c=atoi(e); if(c>0) triCap=c; }   // per-REAL-triangle = FULL coverage (no fall-through gaps); HSR_NAVTRICAP=3000 -> smoother-but-gappy averaged grid
        float inflate=0.02f; if(const char* e=std::getenv("HSR_NAVINFLATE")){ float c=(float)atof(e); inflate=c; }   // tiny peek above the surface (low = less bumpy)
        auto emitBox=[&](const float c[3],const float he[3],const float* q){
            sitem::Item bi; bi.type=sitem::BOXCOL; bi.name="NavBox";
            bi.pos[0]=c[0];bi.pos[1]=c[1];bi.pos[2]=c[2];
            bi.rot[0]=bi.rot[1]=bi.rot[2]=0.f; bi.scale[0]=bi.scale[1]=bi.scale[2]=1.f;
            bi.half[0]=he[0];bi.half[1]=he[1];bi.half[2]=he[2];
            if(q){bi.quat[0]=q[0];bi.quat[1]=q[1];bi.quat[2]=q[2];bi.quat[3]=q[3];}
            std::string nid=sitem::uuid(sidx); entities += "," + sitem::itemEntityJson(bi, sidx); ++sidx;
            rels += "," + relChildOf(nid, rootId);
        };
        bool tilted = ntri>=1 && (int)ntri<=triCap && !std::getenv("HSR_NAVGRID");
        size_t emitted=0;
        if (tilted) {
            bool navSmooth = std::getenv("HSR_NAVSMOOTH")!=nullptr;   // smoothed (vertex-averaged) tilts -> no per-edge creases
            // SPLIT floor vs wall: up-facing FLOOR tris -> THICK boxes extending DOWN (stand on top); near-vertical WALL tris
            // -> THIN slabs hugging the wall (no air-fill, no eject) so you can't phase through. (Device-safe stand-in for
            // haven2025's cooked ColliderMeshes, until the full 4.1.64 trimesh hand-roll lands.)
            std::vector<float> fv,wv; std::vector<uint32_t> fi,wi;
            for (size_t t=0;t+2<si.navIdx.size();t+=3){ uint32_t a=si.navIdx[t],b=si.navIdx[t+1],c=si.navIdx[t+2];
                if((size_t)a*3+2>=si.navVerts.size()||(size_t)b*3+2>=si.navVerts.size()||(size_t)c*3+2>=si.navVerts.size()) continue;
                const float* pa=&si.navVerts[a*3]; const float* pb=&si.navVerts[b*3]; const float* pc=&si.navVerts[c*3];
                float e1[3]={pb[0]-pa[0],pb[1]-pa[1],pb[2]-pa[2]}, e2[3]={pc[0]-pa[0],pc[1]-pa[1],pc[2]-pa[2]};
                float ny=e1[2]*e2[0]-e1[0]*e2[2]; float nl=std::sqrt((e1[1]*e2[2]-e1[2]*e2[1])*(e1[1]*e2[2]-e1[2]*e2[1])+ny*ny+(e1[0]*e2[1]-e1[1]*e2[0])*(e1[0]*e2[1]-e1[1]*e2[0])); if(nl<1e-9f) continue;
                bool floorT = (ny/nl > 0.5f);
                std::vector<float>& dv = floorT?fv:wv; std::vector<uint32_t>& di = floorT?fi:wi;
                uint32_t base=(uint32_t)(dv.size()/3); for(const float* p:{pa,pb,pc}){ dv.push_back(p[0]); dv.push_back(p[1]); dv.push_back(p[2]); } di.push_back(base); di.push_back(base+1); di.push_back(base+2);
            }
            auto fb = navmeshTrisToBoxes(fv, fi, 1.5f, navSmooth?0.05f:0.1f, inflate, navSmooth);   // FLOOR: thick, extends DOWN
            auto wb = navmeshTrisToBoxes(wv, wi, 0.12f, 0.05f, inflate, false);                      // WALLS: THIN slab at the surface
            tilted = !fb.empty() || !wb.empty();
            for (auto& b : fb){ float c[3]={b[0],b[1],b[2]}, he[3]={b[3],b[4],b[5]}, q[4]={b[6],b[7],b[8],b[9]}; emitBox(c,he,q); ++emitted; }
            for (auto& b : wb){ float c[3]={b[0],b[1],b[2]}, he[3]={b[3],b[4],b[5]}, q[4]={b[6],b[7],b[8],b[9]}; emitBox(c,he,q); ++emitted; }
            if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] collision: %zu floor boxes + %zu wall slabs\n", fb.size(), wb.size());
        }
        bool tiltGrid=false;
        if (!tilted) {
            float bcell=2.0f; if(const char* e=std::getenv("HSR_NAVBOXCELL")){ float c=(float)atof(e); if(c>0.5f) bcell=c; }
            float bthick=3.0f; if(const char* e=std::getenv("HSR_NAVTHICK")){ float c=(float)atof(e); if(c>0.2f) bthick=c; }
            int gmax=4000; if(const char* e=std::getenv("HSR_NAVGRIDMAX")){ int c=atoi(e); if(c>0) gmax=c; }   // box budget for the grid fallback: higher = finer cells = hugs curved terrain (Outer Wilds) tighter
            auto tg = navmeshTiltedGrid(si.navVerts, si.navIdx, bcell, bthick, 0.6f, gmax);   // thick slabs + overlap = no gaps to clip through; coarsens cell to fit gmax (always full coverage)
            if (!tg.empty()) { tiltGrid=true; for (auto& b : tg){ float c[3]={b[0],b[1],b[2]}, he[3]={b[3],b[4],b[5]}, q[4]={b[6],b[7],b[8],b[9]}; emitBox(c,he,q); ++emitted; } }
            else { auto gb = navmeshToBoxes(si.navVerts, bcell, 1.5f, 1500); for (auto& b : gb){ float c[3]={b[0],b[1],b[2]}, he[3]={b[3],b[4],b[5]}; emitBox(c,he,nullptr); ++emitted; } }
        }
        if (std::getenv("HSR_VERBOSE")||std::getenv("HSR_NAVDBG")) fprintf(stderr,"[COOK] navmesh -> %zu %s collision boxes (%zu source tris)\n", emitted, tilted?"TILTED per-triangle":(tiltGrid?"TILTED grid":"flat grid"), ntri);
        // ── HSR_NAVDEBUG: clone the navmesh source tris as a VISIBLE slope-colored renderable so the navmesh shows
        //    IN-HEADSET (debug on device, matching the editor's slope overlay). white-tex unlit material * per-vertex
        //    slope vertexColor0 (the unlit shader does base*COLOR0). Compare against where you actually WALK (the boxes)
        //    to spot collision jaggedness. Opt-in (HSR_NAVDEBUG) so normal cooks are unaffected. ──
        if (std::getenv("HSR_NAVDEBUG") && si.navVerts.size()>=9 && si.navIdx.size()>=3) {
            size_t nvv=si.navVerts.size()/3;
            std::vector<float> upAcc(nvv,0.f); std::vector<int> upCnt(nvv,0);
            for (size_t t=0;t+2<si.navIdx.size();t+=3){
                uint32_t a=si.navIdx[t],b=si.navIdx[t+1],c=si.navIdx[t+2];
                if(a>=nvv||b>=nvv||c>=nvv) continue;
                const float* A=&si.navVerts[a*3]; const float* B=&si.navVerts[b*3]; const float* C=&si.navVerts[c*3];
                float e1[3]={B[0]-A[0],B[1]-A[1],B[2]-A[2]}, e2[3]={C[0]-A[0],C[1]-A[1],C[2]-A[2]};
                float nx=e1[1]*e2[2]-e1[2]*e2[1], ny=e1[2]*e2[0]-e1[0]*e2[2], nz=e1[0]*e2[1]-e1[1]*e2[0];
                float nl=std::sqrt(nx*nx+ny*ny+nz*nz); float up= nl>1e-6f?std::fabs(ny)/nl:1.f;
                for(uint32_t vi:{a,b,c}){ upAcc[vi]+=up; upCnt[vi]++; }
            }
            auto c8=[](float f){ int i=(int)(f*255.f+0.5f); return (uint8_t)(i<0?0:i>255?255:i); };
            std::vector<uint8_t> vcol(nvv*4); std::vector<float> nuv(nvv*2,0.f);
            for(size_t v=0;v<nvv;v++){ float up= upCnt[v]?upAcc[v]/upCnt[v]:1.f;
                float rr= up>0.85f?(1.f-up)/0.15f:1.f, gg= up>0.5f?1.f:up/0.5f;
                vcol[v*4]=c8(std::min(1.f,rr)); vcol[v*4+1]=c8(std::min(1.f,gg)); vcol[v*4+2]=40; vcol[v*4+3]=255; }
            std::vector<uint8_t> dmat=matTpl;   // unlit material (field7 already = unlit shader); point its texture at white
            std::string pdw=std::string("meta/navdebug/w")+std::to_string(sidx)+".tex/tex"; AssetKey3 dwK=keyForPath(pdw);
            assets.push_back({ pdw, dwK.tgt, whiteTex, dwK });
            if (dmat.size()>=136){ memcpy(dmat.data()+120,&dwK.pkg,8); memcpy(dmat.data()+128,&dwK.ing,8); }
            auto dmesh = encodeRendMeshParts(si.navVerts, nuv, si.navIdx, dmat, 0, {}, {}, {}, false, vcol);
            if (dmesh.size()>64) {
                std::string pdm=std::string("meta/navdebug/m")+std::to_string(sidx)+".rendmesh/mesh"; AssetKey3 dmK=keyForPath(pdm);
                std::string pdt=std::string("meta/navdebug/m")+std::to_string(sidx)+".matl/mat";     AssetKey3 dtK=keyForPath(pdt);
                assets.push_back({ pdm, dmK.tgt, dmesh, dmK }); assets.push_back({ pdt, dtK.tgt, dmat, dtK });
                std::string did=sitem::uuid(900000+sidx); float o0[3]={0,0,0}, r0[3]={0,0,0}, s1[3]={1,1,1};
                entities += "," + entityJson(did, "NavDebug", o0, r0, s1, dmK, { dtK }, AssetKey3{0,0,0}, std::string(), 5, false, std::string());
                rels += "," + relChildOf(did, rootId); ++sidx;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] navmesh DEBUG clone mesh: %zu verts slope-colored -> VISIBLE on device (HSR_NAVDEBUG)\n", nvv);
            }
        }
    }
    // Background music: ship the V79 _BACKGROUND_LOOP.ogg RAW as a SoundAsset (FMOD:SND -> FMOD auto-detects ogg),
    // auto-started + looped by a SoundPlatformComponent. The global converter: ANY env with a loose .ogg gets its theme.
    if (!bgOgg.empty()) {
        std::string pSnd = MH + "/audio/background.ogg/sound"; AssetKey3 sndK = keyForPath(pSnd);
        assets.push_back({ pSnd, sndK.tgt, bgOgg, sndK, 0x444F4D46u /*"FMOD"*/, 0x20444E53u /*"SND "*/ });
        std::string audioId = makeUuid(rng);
        entities += "," + audioEntityJson(audioId, spawnPos, sndK);
        rels += "," + relChildOf(audioId, rootId);
        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[EXPORT] background audio: %zu KB ogg -> FMOD:SND SoundAsset (auto-start, loop)\n", bgOgg.size()/1024);
    }
    fprintf(stderr, "[EXPORT] cam@(%.1f,%.1f,%.1f) navBounds=(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) ground@(%.1f,%.1f,%.1f) scale=%.1f spawn@(%.1f,%.1f,%.1f)\n",
            camPos?camPos[0]:0,camPos?camPos[1]:0,camPos?camPos[2]:0, smn[0],smn[1],smn[2], smx[0],smx[1],smx[2], gx,gy,gz, gs, spawnPos[0],spawnPos[1],spawnPos[2]);
    std::string pContent = MH + "/content.hstf/template", pSpace = MH + "/space.hstf/template";
    AssetKey3 contentK = keyForPath(pContent), spaceK = keyForPath(pSpace);
    std::string content = templateJson(entities, rels);
    // Far clip plane (ScenePlatformComponent v3) goes in space.hstf — vista-proven the shell only applies it there.
    // V79 envs ship NO ScenePlatformComponent, so on V205 they fall back to the shell's SMALL default far and
    // distant geometry clips (the cyberhome "far plane showing up"). We AUTO-SIZE the far to the scene's own
    // extent (bounding-sphere radius from origin × 2.5 + 2000, floored at 40000) so no ported home can out-run
    // its own far plane regardless of size (official focused=150000). HSR_FARCLIP overrides explicitly.
    // FAR CLIP = MAX on EVERY bake (user demand: distant geometry must NEVER clip on device — V79 envs ship no
    // ScenePlatformComponent so the shell's small default far clips them). 150000 = the official `focused` vista's
    // far = the PROVEN device max (1e6 both (a) degenerates the projection AND (b) snprintf %g'd it to "1e+06"
    // which the device JSON parser rejects → far defaulted small → STILL CLIPPED). HSR_FARCLIP overrides.
    float farP = 150000.0f;
    if (const char* fc = std::getenv("HSR_FARCLIP")) farP = (float)atof(fc);
    fprintf(stderr, "[EXPORT] farClippingPlane=%.0f (MAX every bake; set HSR_FARCLIP to override)\n", farP);
    std::string space   = spaceJson(rng, "home_c25", contentK, farP);   // space display name = stock package (cosmetic)
    auto shellcfg = jbytes(shellConfigJson(spaceK, locomotion));
    assets.push_back({ pContent, TGT_TEMPLATE, jbytes(content), contentK });
    assets.push_back({ pSpace,   TGT_TEMPLATE, jbytes(space),   spaceK });
    prog(0.80f, "Packaging scene.zip");
    auto sceneZip = assembleSceneZip(assets, shellcfg);
    if (outSceneZip) *outSceneZip = sceneZip;     // expose so the caller can splice extra (spoofed) APKs without re-cooking
    prog(0.90f, "Building APK");
    // Package spoof: rename the shell's package to a chosen one so the port can MASQUERADE as an official env
    // (e.g. set HSR_COOK_SHELL=haven2025.apk + HSR_COOK_FROMPKG/HSR_COOK_PKG=<haven2025 pkg> to replace it). The
    // shell's current package is HSR_COOK_FROMPKG (default nuxd's); the new one is HSR_COOK_PKG.
    const char* fp = getenv("HSR_COOK_FROMPKG"); const char* tp = getenv("HSR_COOK_PKG");
    return spliceAPK(nuxdApk, sceneZip, fp ? fp : "com.meta.environment.prod.nuxd", tp ? tp : "com.environment.outerwilds", ok, editorSession);
}

} // namespace hslcook
