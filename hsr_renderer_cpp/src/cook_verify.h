#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>

// ── COOK-TIME FLATBUFFER VERIFIER ─────────────────────────────────────────────────────────────────────────────
// The device (libshell.so) statically links the stock Google FlatBuffers verifier — CONFIRMED via IDA: libshell has
// ZERO dynamic flatbuffers imports (.dynsym only), so the renderer's "MeshDefinition::fix() - flatbuffer
// verification failed" is the standard flatbuffers::Verifier algorithm. We reimplement that algorithm here AND drive
// the per-table field SCHEMA from a Meta-shipped reference asset (the device's own ground truth, e.g. the whale
// skinned RENDMESH) so the cooker rejects a malformed buffer BEFORE it ships — instead of guessing via slow
// on-device round-trips. This would have caught the ROOT.f0 bug instantly:
//   "root.f0: TYPE MISMATCH — cooked=offset, schema=inline-scalar".
namespace hslcook {

struct FbProbe {
    const uint8_t* d = nullptr; size_t n = 0;
    uint32_t u32(size_t p) const { uint32_t v=0; if (p+4<=n) memcpy(&v,d+p,4); return v; }
    int32_t  i32(size_t p) const { int32_t  v=0; if (p+4<=n) memcpy(&v,d+p,4); return v; }
    uint16_t u16(size_t p) const { uint16_t v=0; if (p+2<=n) memcpy(&v,d+p,2); return v; }
};

struct FbField { int idx=0, voff=0, size=0; bool isOffset=false; size_t target=0; };

// Parse one flatbuffer table's vtable into the present fields, with each field's inline size (from the voffset
// layout) and whether it is an OFFSET (a 4-byte uoffset that resolves forward to an in-bounds sub-object).
inline bool fbTableShape(const FbProbe& p, size_t toff, std::vector<FbField>& out, int& tabsize) {
    out.clear();
    if (toff + 4 > p.n) return false;
    long long vtl = (long long)toff - (long long)p.i32(toff);           // vtable = table - soffset
    if (vtl < 0 || (size_t)vtl + 4 > p.n) return false;
    size_t vtb = (size_t)vtl;
    int vts = p.u16(vtb); tabsize = p.u16(vtb + 2);
    if (vts < 4 || vtb + (size_t)vts > p.n || toff + (size_t)tabsize > p.n) return false;
    int nf = (vts - 4) / 2;
    std::vector<std::pair<int,int>> pres;
    for (int i = 0; i < nf; i++) { int vo = p.u16(vtb + 4 + i*2); if (vo) pres.push_back({i, vo}); }
    for (auto& pr : pres) {
        int vo = pr.second, hi = tabsize;
        for (auto& q : pres) if (q.second > vo && q.second < hi) hi = q.second;   // next field up = end of this one
        FbField f; f.idx = pr.first; f.voff = vo; f.size = hi - vo;
        if (f.size >= 4) {                                                         // could be a uoffset
            uint32_t uo = p.u32(toff + vo); size_t tgt = toff + (size_t)vo + uo;
            if (uo >= 4 && tgt + 4 <= p.n) { f.isOffset = true; f.target = tgt; }
        }
        out.push_back(f);
    }
    return true;
}

// Classify an offset target: 1 = vector-of-tables, 3 = table, 2 = leaf vector (bytes/structs) / empty.
inline int fbOffsetKind(const FbProbe& p, size_t tgt) {
    uint32_t cnt = p.u32(tgt);
    if (cnt > 64) return 2;   // >64 elements = a byte/struct vector (VB/IB/embedded-material nested-flatbuffer) -> opaque LEAF,
                              // NOT a vector-of-tables. (Embedded materials are a different type/instance per mesh — don't recurse.)
    if (cnt > 0 && cnt < 1000000u) {
        size_t e0 = tgt + 4; uint32_t eo = p.u32(e0); size_t et = e0 + eo;
        if (eo >= 4 && et + 4 <= p.n) {
            long long v = (long long)et - (long long)p.i32(et);
            if (v >= 0 && (size_t)v + 4 <= p.n) { int vs = p.u16((size_t)v); if (vs >= 4 && (size_t)v+vs <= p.n) return 1; }
        }
        return 2;
    }
    long long v = (long long)tgt - (long long)p.i32(tgt);
    if (v >= 0 && (size_t)v + 4 <= p.n) { int vs = p.u16((size_t)v); if (vs >= 4 && vs < 256 && (size_t)v+vs <= p.n) return 3; }
    return 2;
}

// Recursively compare a cooked table against the Meta reference table; record every structural divergence.
inline void fbCompare(const FbProbe& C, size_t cto, const FbProbe& R, size_t rto,
                      const std::string& path, std::vector<std::string>& probs, int depth = 0) {
    if (depth > 12) return;
    std::vector<FbField> cf, rf; int cts = 0, rts = 0;
    bool rok = fbTableShape(R, rto, rf, rts);
    bool cok = fbTableShape(C, cto, cf, cts);
    if (!rok) return;
    if (!cok) { probs.push_back(path + ": cooked table is malformed (vtable/bounds)"); return; }
    auto find = [](std::vector<FbField>& v, int idx) -> FbField* { for (auto& f : v) if (f.idx == idx) return &f; return nullptr; };
    for (auto& r : rf) {
        FbField* c = find(cf, r.idx);
        std::string fp = path + ".f" + std::to_string(r.idx);
        if (!c) { probs.push_back(fp + ": MISSING (schema has a " + std::to_string(r.size) + "B " + (r.isOffset?"offset":"inline") + " field here)"); continue; }
        if (c->isOffset != r.isOffset) {
            probs.push_back(fp + ": TYPE MISMATCH — cooked=" + (c->isOffset?"offset":"inline-scalar") + " schema=" + (r.isOffset?"offset":"inline-scalar"));
            continue;
        }
        if (!r.isOffset) {
            // The device's flatbuffers verify only checks the field FITS its schema type — a LARGER cooked field
            // (trailing struct padding) is tolerated (it reads schema_size bytes). Flag only TOO-SMALL = real corruption.
            if (c->size < r.size) probs.push_back(fp + ": FIELD TOO SMALL — cooked=" + std::to_string(c->size) + "B < schema=" + std::to_string(r.size) + "B");
        } else {
            int rk = fbOffsetKind(R, r.target), ck = fbOffsetKind(C, c->target);
            if (rk == 1 && ck == 1) {                                  // vector of tables -> recurse element 0
                size_t ce = c->target + 4, re = r.target + 4;
                fbCompare(C, ce + C.u32(ce), R, re + R.u32(re), fp + "[0]", probs, depth + 1);
            } else if (rk == 3 && ck == 3) {
                fbCompare(C, c->target, R, r.target, fp, probs, depth + 1);
            }
        }
    }
    for (auto& c : cf) if (!find(rf, c.idx)) probs.push_back(path + ".f" + std::to_string(c.idx) + ": EXTRA field (not in schema)");
}

// Verify a cooked flatbuffer against a Meta reference of the same asset type. Empty result = matches device schema.
inline std::vector<std::string> fbVerifyAgainst(const std::vector<uint8_t>& cooked, const std::vector<uint8_t>& ref) {
    std::vector<std::string> probs;
    if (cooked.size() < 8) { probs.push_back("buffer too small"); return probs; }
    if (ref.size()    < 8) return probs;                               // no reference -> can't check
    FbProbe C{cooked.data(), cooked.size()}, R{ref.data(), ref.size()};
    fbCompare(C, C.u32(0), R, R.u32(0), "root", probs);
    return probs;
}

}  // namespace hslcook
