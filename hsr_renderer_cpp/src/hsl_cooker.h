#pragma once
// HSL SCENE COOKER / GENERATOR — the ENCODE side, built into the renderer so it is a scene MAKER (not just a
// viewer): take a loaded/edited scene (geometry + textures) and GENERATE a cooked V203/HSL scene.zip that boots on
// a Quest. 1:1 inverse of the renderer's decoders (rendmesh/rendtxtr/matl/rendshad/asmh parsers). Every format here
// was verified end-to-end against libshell by the python prototype (a fully self-cooked checkerboard env renders +
// installs). See the project_hsl_cooker_apk_baker memory for the full format notes.
#include "types.h"
#include <vector>
#include <string>
#include <map>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "astcenc.h"   // ASTC ENCODE (full astcenc build) — RGBA8 -> ASTC blocks for RENDTXTR
#include "miniz.h"     // ZIP read/write — scene.zip assembly + APK splice (already linked by the project)
#include "hzanim_acl.h"  // hzAclEncode (HZANIM ACL clip) — only the declaration; ACL lives in hzanim_acl.cpp
#include "cook_verify.h"  // cook-time FlatBuffer verifier (device's stock-flatbuffers structural check, schema from Meta assets)

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
//    4=boundMin,5=content-hash,6=52 } ; LOD{ 0=parts,1=0.2 screen-size,3=boundMin } ; root{ 1=LOD,2=materials,
//    3=min,4=max,8=1.0 }. (format-hash 0xDBE0A523 is CONSTANT for this layout; content-hashes appear to be cache
//    keys — using deterministic placeholders. part.field3 material-binding is omitted; the entity's
//    MaterialPlatformComponent supplies the material.) ────────────────────────────────────────────────────────
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
    static const uint8_t VSF0[12] = { 0x23,0xa5,0xe0,0xdb, 0x22,0x95,0x8f,0xf3, 0,0,0,0 };
    uint64_t vbH = murmur64A(vb.data(), vb.size()), ibH = murmur64A(ib.data(), ib.size());
    uint8_t vsf4[12] = {0}; memcpy(vsf4, &vbH, 8);
    uint8_t pf5[12]  = {0}; memcpy(pf5, &ibH, 8);
    float ones[2] = { 1.f,1.f }; uint8_t rf8[8]; memcpy(rf8, ones, 8);
    uint8_t attr[12] = { 0,0x32,0,0, 5,0x21,0,0, 4,0x13,0,0 };   // POS f32x3@0, UV f16x2@12, NORMAL fmt0x13@16
    FB b(vb.size() + ib.size() + 512);
    int vbo = b.createByteVector(vb.data(), vb.size()), ibo = b.createByteVector(ib.data(), ib.size()), ao = b.createStructVector(attr, 4, 3, 4);
    b.startObject(5); b.addStructSlot(0, VSF0, 12, 4); b.addScalar<uint32_t>(1, nv); b.addOffset(2, vbo); b.addOffset(3, ao); b.addStructSlot(4, vsf4, 12, 4); int vs = b.endObject();
    int vsvec = b.createOffsetVector({ vs });
    int matEmb = embeddedMatl.empty() ? 0 : b.createByteVector(embeddedMatl.data(), embeddedMatl.size());  // part.field3 = embedded MATL
    b.startObject(7); b.addOffset(0, vsvec); b.addOffset(1, ibo); b.addOffset(3, matEmb); b.addStructSlot(4, (const uint8_t*)aabb, 24, 4); b.addStructSlot(5, pf5, 12, 4); b.addScalar<uint32_t>(6, 52u); int part = b.endObject();
    int pv = b.createOffsetVector({ part });
    b.startObject(4); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); int lod = b.endObject();
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
inline std::vector<uint8_t> encodeRendTxtr(const uint8_t* rgba, int w, int h, int bw = 8, int bh = 8, uint8_t formatCode = 11) {
    astcenc_config cfg{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, (unsigned)bw, (unsigned)bh, 1, ASTCENC_PRE_MEDIUM, 0, &cfg) != ASTCENC_SUCCESS) return {};
    const astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    std::vector<uint8_t> data;                                  // concatenated ASTC mip chain
    std::vector<uint8_t> cur(rgba, rgba + (size_t)w * h * 4); int cw = w, ch = h; int mipCount = 0;
    while (true) {
        mipCount++;
        int cols = (cw + bw - 1) / bw, rows = (ch + bh - 1) / bh;
        std::vector<uint8_t> blocks((size_t)cols * rows * 16);
        astcenc_context* ctx = nullptr; if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return {};
        void* slice = cur.data();
        astcenc_image img{}; img.dim_x = (unsigned)cw; img.dim_y = (unsigned)ch; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
        astcenc_error e = astcenc_compress_image(ctx, &img, &swz, blocks.data(), blocks.size(), 0);
        astcenc_context_free(ctx);
        if (e != ASTCENC_SUCCESS) return {};
        data.insert(data.end(), blocks.begin(), blocks.end());
        if (cw == 1 && ch == 1) break;
        int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        std::vector<uint8_t> nx((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
            int sx = x * 2, sy = y * 2, sx1 = sx + 1 < cw ? sx + 1 : sx, sy1 = sy + 1 < ch ? sy + 1 : sy;
            int a = cur[((size_t)sy * cw + sx) * 4 + c], b2 = cur[((size_t)sy * cw + sx1) * 4 + c];
            int c2 = cur[((size_t)sy1 * cw + sx) * 4 + c], d2 = cur[((size_t)sy1 * cw + sx1) * 4 + c];
            nx[((size_t)y * nw + x) * 4 + c] = (uint8_t)((a + b2 + c2 + d2 + 2) / 4);
        }
        cur.swap(nx); cw = nw; ch = nh;
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
                              int meshVer = 5, bool animated = false) {
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
    std::string comps =
        comp("TransformPlatformComponent", 1, tb) + "," +
        comp("MeshPlatformComponent", meshVer, std::string("{\"mesh\":") + refJson(meshRef) + "}") + "," +
        comp("MaterialPlatformComponent", 1, std::string("{\"materials\":[") + mats + "],\"constantParameters\":[],\"textureParameters\":[]}");
    if (skinned) comps += "," + extraComp;   // AnimatorPlatformComponent LAST (exact calming butterfly order)
    // walkable: collision mesh + static physics body (so locomotion/teleport land on it)
    if (colliderRef.pkg || colliderRef.ing || colliderRef.tgt) {
        comps += "," + comp("ColliderMeshPlatformComponent", 1, std::string("{\"meshAsset\":") + refJson(colliderRef) + "}");
        comps += "," + comp("PhysicsBodyPlatformComponent", 1, "{\"type\":\"StaticCollision\"}");
    }
    return std::string("{\"id\":\"") + id + "\",\"name\":\"" + name + "\",\"components\":[" + comps + "],\"attributes\":[]}";
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
        comp("ColliderMeshPlatformComponent", 1, std::string("{\"meshAsset\":") + refJson(colliderRef) + "}") + "," +
        comp("PhysicsBodyPlatformComponent", 1, "{\"type\":\"StaticCollision\"}");
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Ground\",\"components\":[" + comps + "],\"attributes\":[]}";
}
inline std::string templateJson(const std::string& entities, const std::string& relationships = std::string()) {
    return std::string("{\"version\":5,\"entities\":[") + entities + "],\"relationships\":[" + relationships + "]}";
}
// space.hstf = one entity whose `type` is the content-template AssetRef (the firstWorldAsset points here).
inline std::string spaceJson(CookRng& r, const std::string& name, const AssetKey3& contentRef) {
    std::string ent = std::string("{\"id\":\"") + makeUuid(r) + "\",\"name\":\"" + name + "\",\"type\":" + refJson(contentRef) + ",\"deltas\":[],\"attributes\":[]}";
    return templateJson(ent);
}
inline std::string shellConfigJson(const AssetKey3& spaceRef, bool locomotion) {
    return std::string("{\"firstWorldAssetId\":") + refJson(spaceRef) + ",\"supportsLocomotion\":" + (locomotion?"true":"false") + "}";
}

// ── Binary AndroidManifest (AXML) package rename — port of axml.py: rebuild the string pool with `old`→`new`
//    in every pooled string (node chunks reference strings by INDEX, so they stay valid). ───────────────────
inline std::vector<uint8_t> patchAxml(const std::vector<uint8_t>& axml, const std::string& oldS, const std::string& newS) {
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
    bool any=false; for (auto&s:strings) if (s.find(oldS)!=std::string::npos){ any=true; break; }
    if (!any) return axml;
    auto repl=[&](std::string s){ size_t p; while((p=s.find(oldS))!=std::string::npos) s.replace(p,oldS.size(),newS); return s; };
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

// Splice a freshly-cooked scene.zip into a base APK shell (Nuxd): drop META-INF + stale text indices, rename the
// package in AndroidManifest, keep resources.arsc STORED. Returns the unsigned APK bytes.
inline std::vector<uint8_t> spliceAPK(const std::string& baseApk, const std::vector<uint8_t>& sceneZip,
                                      const std::string& oldPkg, const std::string& newPkg, bool* ok = nullptr) {
    if (ok) *ok = false;
    mz_zip_archive in; memset(&in,0,sizeof in);
    if (!mz_zip_reader_init_file(&in, baseApk.c_str(), 0)) return {};
    mz_zip_archive out; memset(&out,0,sizeof out); mz_zip_writer_init_heap(&out,0,0);
    static const char* SKIP[] = {"assets/one_query_hash.txt","assets/params_map.txt","assets/params_map_v4_u0.txt","assets/params_names_v4_u0.txt"};
    int n=(int)mz_zip_reader_get_num_files(&in);
    for (int i=0;i<n;i++){
        mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&in,i,&st)) continue;
        std::string name=st.m_filename;
        if (name.rfind("META-INF/",0)==0) continue;
        bool skip=false; for (auto* s:SKIP) if (name==s){ skip=true; break; } if (skip) continue;
        std::vector<uint8_t> data;
        if (name=="assets/scene.zip") data=sceneZip;
        else { size_t sz=0; void* p=mz_zip_reader_extract_to_heap(&in,i,&sz,0); if(!p) continue; data.assign((uint8_t*)p,(uint8_t*)p+sz); mz_free(p); }
        if (name=="AndroidManifest.xml") data=patchAxml(data,oldPkg,newPkg);
        mz_uint fl = (name=="resources.arsc")?MZ_NO_COMPRESSION:MZ_DEFAULT_COMPRESSION;
        mz_zip_writer_add_mem(&out,name.c_str(),data.data(),data.size(),fl);
    }
    mz_zip_reader_end(&in);
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
inline std::vector<uint8_t> encodeRendMeshParts(const std::vector<float>& pos, const std::vector<float>& uv,
                                                const std::vector<uint32_t>& idx, const std::vector<uint8_t>& embeddedMatl = {},
                                                int vatVertexCount = 0,
                                                const std::vector<uint8_t>& boneIdx = {}, const std::vector<uint8_t>& boneWgt = {},
                                                const std::vector<uint32_t>& jointIds = {}) {   // jointIds = murmur3(joint name) per skeleton joint
    struct Part { std::vector<uint8_t> vb, ib; uint32_t nv = 0; };
    std::vector<Part> parts;
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
    float mn[3] = { 1e30f,1e30f,1e30f }, mx[3] = { -1e30f,-1e30f,-1e30f };
    for (size_t i = 0; i + 2 < pos.size(); i += 3) for (int k = 0; k < 3; k++) { float p = pos[i + k]; if (p < mn[k]) mn[k] = p; if (p > mx[k]) mx[k] = p; }
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
                        uint8_t col0[4] = { 0xFF,0xFF,0xFF,0xFF }; pr.vb.insert(pr.vb.end(), col0, col0+4);   // vertexColor0 sem4 = white (source COLOR_0)
                        uint8_t bidx[4] = { boneRemap[boneIdx[(size_t)g*4]], boneRemap[boneIdx[(size_t)g*4+1]],
                                            boneRemap[boneIdx[(size_t)g*4+2]], boneRemap[boneIdx[(size_t)g*4+3]] };  // remap to dense palette slot
                        pr.vb.insert(pr.vb.end(), bidx, bidx+4);
                        pr.vb.insert(pr.vb.end(), &boneWgt[(size_t)g*4], &boneWgt[(size_t)g*4]+4);
                    } else if (vatVertexCount > 0) {   // VAT column: uv1.x = (vertexIndex+0.5)/count (u16 UNORM), replaces the normal slot
                        float col = ((float)g + 0.5f) / (float)vatVertexCount; col = col < 0 ? 0 : (col > 1 ? 1 : col);
                        uint16_t cu = (uint16_t)(col * 65535.0f + 0.5f);
                        uint8_t cb[4] = { (uint8_t)cu, (uint8_t)(cu>>8), 0, 0 }; pr.vb.insert(pr.vb.end(),cb,cb+4);
                    } else {
                        uint8_t nb[4] = { 0xFF,0xFF,0xFF,0xFF }; pr.vb.insert(pr.vb.end(),nb,nb+4);   // NORMAL fmt0x13 (stride 20)
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
    float radius = 0; for (int k = 0; k < 3; k++) { float a = mn[k] < 0 ? -mn[k] : mn[k], c = mx[k] < 0 ? -mx[k] : mx[k]; if (a > radius) radius = a; if (c > radius) radius = c; }
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
    else if (vatVertexCount > 0) { attr[8]=5; attr[9]=0x27; attr[10]=1; attr[11]=0; }   // 3rd attr -> UV1 u16x2 (VAT column)
    size_t cap = 2048 + embeddedMatl.size() * (parts.size() + 1); for (auto& p : parts) cap += p.vb.size() + p.ib.size() + 256;
    FB b(cap);
    std::vector<int> partOffs;
    for (auto& p : parts) {
        int vbo = b.createByteVector(p.vb.data(), p.vb.size());
        int ibo = b.createByteVector(p.ib.data(), p.ib.size());
        int ao  = b.createStructVector(attr, 4, nattr, 4);
        uint64_t vbH = murmur64A(p.vb.data(), p.vb.size()), ibH = murmur64A(p.ib.data(), p.ib.size());
        uint8_t vsf4[12] = {0}; memcpy(vsf4, &vbH, 8); uint8_t pf5[12] = {0}; memcpy(pf5, &ibH, 8);
        b.startObject(5); b.addStructSlot(0, VSF0, 12, 4); b.addScalar<uint32_t>(1, p.nv); b.addOffset(2, vbo); b.addOffset(3, ao); b.addStructSlot(4, vsf4, 12, 4); int vs = b.endObject();
        int vsvec = b.createOffsetVector({ vs });
        int matEmb = embeddedMatl.empty() ? 0 : b.createByteVector(embeddedMatl.data(), embeddedMatl.size());
        b.startObject(7); b.addOffset(0, vsvec); b.addOffset(1, ibo); b.addOffset(3, matEmb); b.addStructSlot(4, (const uint8_t*)aabb, 24, 4); b.addStructSlot(5, pf5, 12, 4); b.addScalar<uint32_t>(6, 52u); int part = b.endObject();
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
        b.startObject(5); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f);
        b.addStructSlot(2, (const uint8_t*)&maxBoneIdx, 2, 2);
        b.addStructSlot(3, (const uint8_t*)aabb, 24, 4);
        b.addStructSlot(4, (const uint8_t*)&zero16, 2, 2);
        lod = b.endObject();
    } else {
        b.startObject(4); b.addOffset(0, pv); b.addScalar<float>(1, 0.2f); b.addStructSlot(3, (const uint8_t*)aabb, 24, 4); lod = b.endObject();
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
    bool blend = false;             // alpha-blended (transparent) -> route to unlitblend.surface
    float matTint[4] = {1,1,1,1};   // the mesh's OWN base-color tint (glTF baseColorFactor) for its material params
    std::vector<float> vatOffsets;  // VAT vertex offsets, frames*vertexCount*3 (WORLD space) -> animated via VAT (non-skeletal)
    int vatFrames = 0;
    // HZANIM (skeletal): hzFrames>1 -> skinned RENDMESH + HZAN:SKEL + ACL HZAN:ANIM + AnimatorPlatformComponent
    std::vector<float> hzJointPos, hzJointQuat, hzJointScale;   // jointCount * {3, 4(wxyz), 1}
    std::vector<int>   hzParents;                               // jointCount
    std::vector<uint8_t> hzBoneIdx, hzBoneWgt;                  // nv*4 (sem7 indices, sem8 weights)
    std::vector<float> hzTrsLocal;                              // frames*jointCount*10 (quat xyzw + trans3 + scale3)
    std::vector<float> hzRestPos;                              // nv*3 REST positions for the skinned mesh (NOT world-baked)
    int hzJointCount = 0, hzFrames = 0; float hzFps = 0.f;
};
inline std::vector<uint8_t> readFileBytes(const std::string& p) {
    std::vector<uint8_t> b; FILE* f = fopen(p.c_str(), "rb"); if (!f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { b.resize((size_t)n); size_t r = fread(b.data(), 1, (size_t)n, f); b.resize(r); } fclose(f); return b;
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
            ExportMesh c; c.name = m.name; c.blend = m.blend; c.w = m.w; c.h = m.h; c.rgba = m.rgba;
            for (int k = 0; k < 4; k++) c.matTint[k] = m.matTint[k];
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
// Port ANY decoded scene (e.g. a V79 .ovrscene loaded by the renderer) to a bootable V203 APK: per-mesh RENDMESH
// (struct-bounds, embedded material) + RENDTXTR + the floor MATL template patched to that mesh's texture, all bound
// to the real renderer_module unlit.surface shader; Root entity + child relationships + spawn point.
inline std::vector<uint8_t> exportSceneAPK(const std::vector<ExportMesh>& meshes, const std::string& nuxdApk,
                                           const std::vector<uint8_t>& vspv, const std::vector<uint8_t>& fspv,
                                           bool locomotion = true, bool* ok = nullptr, const float* camPos = nullptr,
                                           std::vector<uint8_t>* outSceneZip = nullptr) {
    if (ok) *ok = false;
    (void)vspv; (void)fspv;
    CookRng rng;
    std::vector<CookAsset> assets;
    // Bundle the V203 render system's OWN shaders + materials: opaque -> unlit.surface (floor MATL), transparent ->
    // unlitblend.surface (dome MATL). V79 glTF envs are textured PBR (no custom GLSL), so these faithfully port the
    // shading AND fix the "black where transparent" bug (blend meshes were drawn opaque).
    auto shad      = readFileBytes("cooker/nuxd_unlit_shader.bin");
    auto shadBlend = readFileBytes("cooker/nuxd_unlitblend_shader.bin");
    auto matTpl    = readFileBytes("cooker/realfloor_mat.bin");   // unlit.surface MATL template
    auto matBlend  = readFileBytes("cooker/realdome_mat.bin");    // unlitblend.surface MATL template
    auto shadVat   = readFileBytes("cooker/vat_shader.bin");      // vatunlitbasecolor (vertex-animation) shader
    auto matVat    = readFileBytes("cooker/vat_mat.bin");         // VAT MATL template (416B, base@152/160 + VAT@192/200)
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
    if (shad.empty() || matTpl.size() < 176) return {};
    AssetKey3 shaderK = { 0x608B25CE5424598Dull, 0x78686234E7611EFCull, 0xA1767FE9u };
    assets.push_back({ "meta/renderer_module/shaders/unlit.surface/shader", shaderK.tgt, shad, shaderK });
    bool haveBlend = !shadBlend.empty() && matBlend.size() >= 176;
    if (haveBlend) {
        AssetKey3 shaderBlendK = { 0x608B25CE5424598Dull, 0xFBFB67B966D4BA47ull, 0xA1767FE9u };
        assets.push_back({ "meta/renderer_module/shaders/unlitblend.surface/shader", shaderBlendK.tgt, shadBlend, shaderBlendK });
    }
    bool haveVat = !shadVat.empty() && matVat.size() >= 416;
    if (haveVat) {   // ship the shared VAT shader at its AssetRef
        AssetKey3 shaderVatK = { 0x91897C97D84A4317ull, 0xFB22F5F00E5164C0ull, 0xA1767FE9u };
        assets.push_back({ "meta/horizon_shared_shaders/shaders/vat/vatunlitbasecolor.surface/shader", shaderVatK.tgt, shadVat, shaderVatK });
    }
    bool haveSkin = !shadSkin.empty() && matSkin2.size() >= 176;   // require the CURRENT-format skinned material
    AssetKey3 shaderSkinK{}, shaderSkinBK{};
    if (haveSkin) {   // ship our skinned shaders ENV-LOCAL (the device resolves the env's own package; renderer_module copies
                      // are shadowed by the system module). The material's field7 ref is patched (below) to the exact key,
                      // so it resolves to our shipped shader. Ship BOTH the opaque (unlitdoublesidedskinned) and the BLEND
                      // (unlitblendskinned) skinned shaders; blend skinned meshes (the shield) route to the blend one.
        std::string pSkinShader = "meta/myhome/shaders/unlitdoublesidedskinned.surface/shader";
        shaderSkinK = keyForPath(pSkinShader);
        assets.push_back({ pSkinShader, shaderSkinK.tgt, shadSkin, shaderSkinK });
        if (!shadSkinB.empty()) {
            std::string pSkinShaderB = "meta/myhome/shaders/unlitblendskinned.surface/shader";
            shaderSkinBK = keyForPath(pSkinShaderB);
            assets.push_back({ pSkinShaderB, shaderSkinBK.tgt, shadSkinB, shaderSkinBK });
        }
    }
    std::vector<uint8_t> whiteTex; { std::vector<uint8_t> w4((size_t)4*4*4, 255); whiteTex = encodeRendTxtr(w4.data(), 4, 4, 8, 8); }
    std::string pWhite = "meta/myhome/white.tex/tex"; AssetKey3 whiteK = keyForPath(pWhite); bool whiteUsed = false;
    struct SharedRig { std::vector<uint8_t> skel, anim; AssetKey3 skelK, animK; };  // dedup skel+anim across meshes sharing one armature (like nuxd)
    std::vector<SharedRig> rigs;
    std::string rootId = makeUuid(rng);
    std::string entities = rootEntityJson(rootId), rels;
    float pos0[3]={0,0,0}, rot0[3]={0,0,0}, scl1[3]={1,1,1};
    const char* navE = getenv("HSR_NAVMESH"); int navIdx = navE ? atoi(navE) : -1;   // base navmesh on one mesh, else whole scene
    float smn[3]={1e30f,1e30f,1e30f}, smx[3]={-1e30f,-1e30f,-1e30f};
    // Split >60000-vert STATIC meshes into separate single-part meshes (multi-part static doesn't render on device).
    std::vector<ExportMesh> meshesV = splitLargeStaticMeshes(meshes);
    std::unordered_map<uint64_t, std::pair<AssetKey3, std::string>> texCache;  // rgba hash -> shared texture (split chunks share one)
    for (size_t i = 0; i < meshesV.size(); ++i) {
        const ExportMesh& m = meshesV[i];
        if (m.positions.size() < 9 || m.indices.size() < 3) continue;   // skip empty
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
        bool navCand = (navIdx >= 0) ? ((int)i == navIdx)
                     : (mnv >= 100 && m.name.find("sky") == std::string::npos && m.name.find("Sky") == std::string::npos);
        if (navCand) for (size_t v = 0; v + 2 < m.positions.size(); v += 3) for (int k = 0; k < 3; k++) { float p = m.positions[v + k]; if (p < smn[k]) smn[k] = p; if (p > smx[k]) smx[k] = p; }
        char base[64]; snprintf(base, sizeof base, "meta/myhome/m%03zu", i);
        std::string pMesh = std::string(base) + ".rendmesh/mesh", pMat = std::string(base) + ".material/material",
                    pTex = std::string(base) + ".tex/tex";
        AssetKey3 meshK = keyForPath(pMesh), matK = keyForPath(pMat);
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
        if (skinnedOpaque && m.rgba.size() >= (size_t)m.w * m.h * 4) {
            texOpaque = m.rgba; for (size_t k = 3; k < texOpaque.size(); k += 4) texOpaque[k] = 255; texSrc = texOpaque.data();
        }
        bool droidWhite = std::getenv("HSR_DROIDWHITE") && m.hzJointCount > 0;   // diag: force the droid to the white fallback tex (isolate texture vs mesh)
        if (m.rgba.size() >= (size_t)m.w * m.h * 4 && m.w && m.h && !droidWhite) {
            uint64_t rh = murmur64A(texSrc, (size_t)m.w * m.h * 4);   // dedup identical textures (esp. the chunks a big mesh was split into)
            auto cit = texCache.find(rh);
            if (cit != texCache.end()) { texK = cit->second.first; pTex = cit->second.second; }  // reuse the shared texture; leave `tex` empty so it isn't re-encoded/re-pushed
            else { texK = keyForPath(pTex); tex = encodeRendTxtr(texSrc, (int)m.w, (int)m.h, 8, 8); texCache[rh] = { texK, pTex }; }
        }
        if (tex.empty() && texK.tgt == 0) { texK = whiteK; whiteUsed = true; }     // share the white fallback (only when there was NO texture at all, not for a dedup hit)
        // ANIMATED (VAT) mesh -> bake the offset texture + the vatunlitbasecolor MATL (base tex + VAT tex); else
        // transparent -> unlitblend MATL, opaque -> floor MATL. Patch each to THIS mesh's texture(s).
        int vc = (int)(m.positions.size() / 3);
        // HZANIM (skinned animation) gated behind HSR_HZANIM: the skin-marker fields trigger a device skinned-verify
        // that rejects the mesh (WIP). Default OFF -> skinned meshes export as STATIC (visible, like the static port).
        bool useHz  = std::getenv("HSR_HZANIM") && haveSkin && m.hzFrames > 1 && m.hzJointCount > 0
                    && m.hzBoneIdx.size() >= (size_t)vc*4 && m.hzBoneWgt.size() >= (size_t)vc*4
                    && m.hzTrsLocal.size() >= (size_t)m.hzFrames*m.hzJointCount*10
                    && m.hzRestPos.size() == m.positions.size();   // centered rest positions present
        bool useVat = !useHz && !m.vatOffsets.empty() && m.vatFrames > 1 && haveVat && m.vatOffsets.size() >= (size_t)vc * m.vatFrames * 3;
        std::vector<uint8_t> vatTex; AssetKey3 vatTexK;
        if (useVat) { vatTex = encodeVatTexture(m.vatOffsets, vc, m.vatFrames); if (vatTex.empty()) useVat = false; }
        std::vector<uint8_t> matl; std::string animatorComp;
        if (useHz) {   // HZANIM: CURRENT-format skinned MATL (field7=shader) + HZAN:SKEL + ACL HZAN:ANIM + AnimatorPlatformComponent
            // BLEND skinned (the SHIELD) -> the transparent-flagged material (field2=2) so it alpha-blends in the
            // transparent pass; OPAQUE skinned (body) -> the plain butterflies template.
            matl = std::getenv("HSR_HZMATTPL") ? matTpl : (m.blend && matSkinB.size() >= 176 ? matSkinB : matSkin2);
            if (!std::getenv("HSR_HZMATTPL") && matl.size() >= 140) {
                // field7 shader ref @48(pkg)/@56(ing)/@64(tgt) -> our env-local skinned shader; tgt @64 stays 0xA1767FE9 (murmur3"shader").
                // BLEND skinned meshes (the omnidroid SHIELD = alphaMode BLEND) use unlitBLENDskinned so the device alpha-blends
                // them (transparent force-field); OPAQUE skinned (droid body) use unlitdoublesidedskinned. Same matParams layout.
                const AssetKey3& sk = (m.blend && (shaderSkinBK.pkg || shaderSkinBK.ing)) ? shaderSkinBK : shaderSkinK;
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
            // SHARE one skeleton + one anim across all meshes that use the SAME armature (the omnidroid body + shield are
            // the same skin[0] -> identical encoded skel+anim). GROUND TRUTH: nuxd's prism_wave + motes both reference the
            // SAME skeleton/anim asset (one skeleton_0 + one take_001 in its scene). Shipping a DUPLICATE per-mesh skel/anim
            // made the device's async asset loader race -> std::length_error crash-loop (5-6x then loaded). Dedup by bytes:
            // ship a shared armatureN.skel/anim once; later meshes with identical bytes reference it.
            auto skelBytes = encodeHzSkel(joints);
            auto anim = hzAclEncode(m.hzTrsLocal.data(), m.hzParents.data(), m.hzJointCount, m.hzFrames, m.hzFps);
            if (!anim.empty()) {
                int rigIdx = -1;
                for (size_t r = 0; r < rigs.size(); ++r) if (rigs[r].skel == skelBytes && rigs[r].anim == anim) { rigIdx = (int)r; break; }
                if (rigIdx < 0) {
                    char rb[48]; snprintf(rb, sizeof rb, "meta/myhome/armature%zu", rigs.size());
                    std::string pSkel = std::string(rb) + ".skel/skeleton", pAnim = std::string(rb) + ".anim/anim";
                    AssetKey3 skK = keyForPath(pSkel), anK = keyForPath(pAnim);
                    // HZAN:SKEL / HZAN:ANIM use a FIXED type targetId across ALL official envs (nuxd/calming/horror all
                    // ship skel tgt=2292226755, anim tgt=1496459219 — NOT murmur3(subName)). The device animation system
                    // keys on these; my murmur3 tgts (513515675/1177748882) left an inconsistent anim record that the
                    // environment-unload/memory-compaction path choked on -> std::length_error crash-loop (stock envs on
                    // the SAME compaction path don't crash). The loose loader still resolves by (pkg,ing,tgt) since the
                    // manifest maps the overridden tgt -> subName, so it both animates AND matches the device's type id.
                    skK.tgt = 2292226755u;  // HZAN:SKEL fixed type targetId
                    anK.tgt = 1496459219u;  // HZAN:ANIM fixed type targetId
                    assets.push_back({ pSkel, skK.tgt, skelBytes, skK, TYPE_HZAN, TYPE_SKEL });
                    assets.push_back({ pAnim, anK.tgt, anim,      anK, TYPE_HZAN, TYPE_ANIM });
                    rigs.push_back({ skelBytes, anim, skK, anK });
                    rigIdx = (int)rigs.size() - 1;
                }
                animatorComp = animatorComponentJson(rigs[rigIdx].skelK, rigs[rigIdx].animK);
            }
        } else if (useVat) {
            std::string pVatTex = std::string(base) + ".vat.tex/tex"; vatTexK = keyForPath(pVatTex);
            matl = matVat;
            memcpy(matl.data() + 152, &texK.pkg, 8);    memcpy(matl.data() + 160, &texK.ing, 8);     // base color tex
            memcpy(matl.data() + 192, &vatTexK.pkg, 8); memcpy(matl.data() + 200, &vatTexK.ing, 8);   // VAT offset tex
            assets.push_back({ pVatTex, vatTexK.tgt, vatTex, vatTexK });
        } else {
            matl = (m.blend && haveBlend) ? matBlend : matTpl;
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
        if (useHz) for (int j = 0; j < m.hzJointCount; ++j) { char jn[24]; snprintf(jn, sizeof jn, "joint_%d", j); jointIds.push_back(murmur3_x86_32(jn, strlen(jn), 0)); }
        std::vector<float> dbgPos; const std::vector<float>* staticPos = &m.positions;
        if (std::getenv("HSR_DROIDFRONT") && m.hzJointCount > 0) {   // diag: shrink+move the droid right in front of spawn (isolate position/cull vs mesh-reject)
            float c[3]={0,0,0}; size_t np=m.positions.size()/3;
            for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) c[k]+=m.positions[v+k];
            for(int k=0;k<3;k++) c[k]/= (np?(float)np:1.f);
            dbgPos.resize(m.positions.size()); float tgt[3]={0,1.5f,-3.f}, sc=0.08f;
            for (size_t v=0;v+2<m.positions.size();v+=3) for(int k=0;k<3;k++) dbgPos[v+k]=(m.positions[v+k]-c[k])*sc+tgt[k];
            staticPos = &dbgPos;
        }
        auto mesh = useHz ? encodeRendMeshParts(hzMeshPos, m.uvs, m.indices, matl, 0, m.hzBoneIdx, m.hzBoneWgt, jointIds)
                          : encodeRendMeshParts(*staticPos, m.uvs, m.indices, matl, useVat ? vc : 0);
        // COOK-TIME VERIFY: check the just-cooked skinned RENDMESH against the Meta-shipped reference schema (the
        // device runs the stock flatbuffers verifier; this catches structural divergence — e.g. a field emitted as
        // an offset where the schema wants an inline scalar — BEFORE the asset ever ships to the headset).
        if (useHz && refSkinMesh.size() >= 8) {
            auto probs = fbVerifyAgainst(mesh, refSkinMesh);
            if (probs.empty()) fprintf(stderr, "[COOK-VERIFY] %s : OK (matches Meta skinned-mesh schema)\n", pMesh.c_str());
            else { fprintf(stderr, "[COOK-VERIFY] %s : %zu PROBLEM(S) vs device schema:\n", pMesh.c_str(), probs.size());
                   for (auto& s : probs) fprintf(stderr, "    !! %s\n", s.c_str()); }
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
        const float* hzPos = (useHz && std::getenv("HSR_HZFIXED")) ? fixedPos : (useHz && std::getenv("HSR_HZCENTER")) ? skinPos : pos0;
        const float* hzScl = (useHz && std::getenv("HSR_HZFIXED")) ? fixedScl : scl1;
        // MeshPlatformComponent **v5** for ALL meshes (static AND skinned). GROUND TRUTH: the working calming butterfly
        // skinned entity uses Mesh v5 (extracted from calming_butterflies.hstf). v6 was a guess that contradicts it.
        entities += "," + entityJson(mid, nm, hzPos, rot0, hzScl, meshK, { matK }, AssetKey3{0,0,0}, animatorComp, 5, false);
        rels += (rels.empty() ? std::string() : std::string(",")) + relChildOf(mid, rootId);
    }
    if (rels.empty()) return {};
    if (whiteUsed) assets.push_back({ pWhite, whiteK.tgt, whiteTex, whiteK });
    // auto-sized walkable ground collider: nuxd's flat ~12.8m PHSX:3MSH plane scaled to cover the nav bounds.
    // Centre it UNDER the camera at the camera's FOOT level (eye - 1.6m) — a 3D env (OW planets curve) has no flat
    // floor, so we drop a walkable plane right where the V79 view stands instead of at the terrain's lowest point.
    float gx = camPos ? camPos[0] : (smn[0]+smx[0])*0.5f;
    float gz = camPos ? camPos[2] : (smn[2]+smx[2])*0.5f;
    float gy = camPos ? camPos[1] - 1.6f : smn[1];
    float ex = smx[0]-smn[0], ez = smx[2]-smn[2], ext = ex > ez ? ex : ez;
    float gs = ext > 1.f ? (ext / 12.0f) * 2.0f : 8.0f;         // BIG invisible plane (nuxd circular floor ~12.8m x2 the scene)
    auto phys = readFileBytes("cooker/realfloor_phys.bin");
    if (!phys.empty() && smx[0] >= smn[0]) {
        std::string pPhys = "meta/myhome/ground.phys/phys"; AssetKey3 colliderK = keyForPath(pPhys);
        assets.push_back({ pPhys, colliderK.tgt, phys, colliderK, TYPE_PHSX, TYPE_3MSH });
        std::string gid = makeUuid(rng); float gp[3]={gx,gy,gz}, gsc[3]={gs,1.f,gs};
        entities += "," + colliderGroundEntityJson(gid, gp, gsc, colliderK);
        rels += "," + relChildOf(gid, rootId);
    }
    // spawn where the V79 camera/view is (XZ), at ground level — else the navmesh centre
    std::string spawnId = makeUuid(rng);
    float spawnPos[3] = { camPos ? camPos[0] : gx, gy + 0.1f, camPos ? camPos[2] : gz };
    entities += "," + spawnPointEntityJson(spawnId, spawnPos);
    rels += "," + relChildOf(spawnId, rootId);
    fprintf(stderr, "[EXPORT] cam@(%.1f,%.1f,%.1f) navBounds=(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) ground@(%.1f,%.1f,%.1f) scale=%.1f spawn@(%.1f,%.1f,%.1f)\n",
            camPos?camPos[0]:0,camPos?camPos[1]:0,camPos?camPos[2]:0, smn[0],smn[1],smn[2], smx[0],smx[1],smx[2], gx,gy,gz, gs, spawnPos[0],spawnPos[1],spawnPos[2]);
    AssetKey3 contentK = keyForPath("meta/myhome/content.hstf/template"), spaceK = keyForPath("meta/myhome/space.hstf/template");
    std::string content = templateJson(entities, rels);
    std::string space   = spaceJson(rng, "myhome", contentK);
    auto shellcfg = jbytes(shellConfigJson(spaceK, locomotion));
    assets.push_back({ "meta/myhome/content.hstf/template", TGT_TEMPLATE, jbytes(content), contentK });
    assets.push_back({ "meta/myhome/space.hstf/template",   TGT_TEMPLATE, jbytes(space),   spaceK });
    auto sceneZip = assembleSceneZip(assets, shellcfg);
    if (outSceneZip) *outSceneZip = sceneZip;     // expose so the caller can splice extra (spoofed) APKs without re-cooking
    // Package spoof: rename the shell's package to a chosen one so the port can MASQUERADE as an official env
    // (e.g. set HSR_COOK_SHELL=haven2025.apk + HSR_COOK_FROMPKG/HSR_COOK_PKG=<haven2025 pkg> to replace it). The
    // shell's current package is HSR_COOK_FROMPKG (default nuxd's); the new one is HSR_COOK_PKG.
    const char* fp = getenv("HSR_COOK_FROMPKG"); const char* tp = getenv("HSR_COOK_PKG");
    return spliceAPK(nuxdApk, sceneZip, fp ? fp : "com.meta.environment.prod.nuxd", tp ? tp : "com.environment.outerwilds", ok);
}

} // namespace hslcook
