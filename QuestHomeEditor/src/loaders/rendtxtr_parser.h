#pragma once
#include "core/types.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <vector>
#include "astcenc.h"

// RENDTXTR header parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/RenderTextureAsset.cpp
// (sub_D3AECC in libshell_current)
//
// 64-byte FlatBuffer header + raw ASTC mip data

struct RendtxtrInfo {
    u32 width = 0;
    u32 height = 0;
    u32 mipCount = 0;
    u8  formatCode = 0;
    u32 rawDataOffset = 0;   // byte offset of the ASTC payload within the file
    u32 rawDataLen = 0;      // exact ASTC payload length (from the FlatBuffer vector)
    // True ASTC block footprint — DERIVED from the data length, NOT from formatCode.
    // The block size that makes the full mip-chain byte-count equal the actual payload
    // length is the correct one (works for any env: nuxd, haven2025, ...).
    u32 blockW = 8;
    u32 blockH = 8;
};

// ── FlatBuffer vtable helpers (RENDTXTR header layout differs per env, so resolve
//    fields by INDEX via the vtable instead of hardcoding byte offsets). Field indices
//    are stable across envs: f3=width, f4=height, f6=format, f9=ASTC data vector.
inline u32 fbFieldSlot(const u8* d, size_t n, u32 root, u32 fi) {
    if ((size_t)root + 4 > n) return 0;
    i32 soff = *reinterpret_cast<const i32*>(d + root);
    // vtable lives at root - soff; soff may be negative (vtable after table).
    int64_t vt64 = (int64_t)root - (int64_t)soff;
    if (vt64 < 0 || (uint64_t)vt64 + 4 > n) return 0;
    u32 vt = (u32)vt64;
    u16 vtSize = *reinterpret_cast<const u16*>(d + vt);
    if ((size_t)vt + vtSize > n) return 0;
    uint64_t bo = (uint64_t)vt + 4 + (uint64_t)fi * 2;
    if (bo + 2 > (uint64_t)vt + vtSize) return 0;
    u16 fo = *reinterpret_cast<const u16*>(d + bo);
    if (fo == 0) return 0;
    uint64_t slot = (uint64_t)root + fo;
    if (slot + 2 > n) return 0;     // ensure at least 2 readable bytes at the slot
    return (u32)slot;
}

// All ASTC block footprints the hardware supports, in descending bytes-per-texel.
static constexpr u32 kAstcBlocks[][2] = {
    {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
    {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
};

// Total bytes of a full ASTC mip chain (mip0 down to 1x1) for a given block size.
inline u32 astcMipChainBytes(u32 w, u32 h, u32 bw, u32 bh) {
    u64 total = 0;
    u32 mw = w, mh = h;
    while (true) {
        u64 cols = (mw + bw - 1) / bw;
        u64 rows = (mh + bh - 1) / bh;
        total += cols * rows * 16;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }
    return (u32)total;
}

inline u32 astcMip0Bytes(u32 w, u32 h, u32 bw, u32 bh) {
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
}

inline bool parseRendtxtrHeader(const std::vector<u8>& data, RendtxtrInfo& info) {
    if (data.size() < 16) return false;
    if (memcmp(data.data() + 4, "TXTR", 4) != 0) return false;

    const u8* d = data.data();
    size_t n = data.size();
    u32 root = *reinterpret_cast<const u32*>(d);
    if (root + 4 > n) return false;

    // Resolve fields by vtable index (stable across envs):
    //   f3 = width, f4 = height, f6 = format, f9 = ASTC data (ubyte vector).
    u32 sW = fbFieldSlot(d, n, root, 3);
    u32 sH = fbFieldSlot(d, n, root, 4);
    u32 sF = fbFieldSlot(d, n, root, 6);
    u32 sMip = fbFieldSlot(d, n, root, 7);
    u32 sData = fbFieldSlot(d, n, root, 9);

    info.width      = sW ? *reinterpret_cast<const u16*>(d + sW) : 0;
    info.height     = sH ? *reinterpret_cast<const u16*>(d + sH) : 0;
    info.formatCode = sF ? d[sF] : 0;
    info.mipCount   = sMip ? *reinterpret_cast<const u16*>(d + sMip) : 1;
    if (info.width == 0 || info.height == 0) return false;

    // f9 is a FlatBuffer vector: indirect uoffset -> [u32 count][bytes...].
    if (sData && (size_t)sData + 4 <= n) {
        u32 uoff = *reinterpret_cast<const u32*>(d + sData);
        uint64_t vecPos = (uint64_t)sData + uoff;
        if (vecPos + 4 <= n) {
            info.rawDataLen = *reinterpret_cast<const u32*>(d + vecPos);
            info.rawDataOffset = (u32)(vecPos + 4);
            if ((uint64_t)info.rawDataOffset + info.rawDataLen > n)
                info.rawDataLen = (u32)(n - info.rawDataOffset);
        }
    }
    // Fallback to legacy fixed 64-byte header if the vector wasn't found.
    if (info.rawDataLen == 0) {
        info.rawDataOffset = 64;
        info.rawDataLen = (u32)(n - 64);
    }

    u32 rawLen = info.rawDataLen;

    // Pick the block footprint whose FULL mip-chain length equals the payload.
    info.blockW = 0;
    for (auto& b : kAstcBlocks) {
        if (astcMipChainBytes(info.width, info.height, b[0], b[1]) == rawLen) {
            info.blockW = b[0]; info.blockH = b[1]; break;
        }
    }
    // Fallback: maybe only mip0 is present (no chain).
    if (info.blockW == 0) {
        for (auto& b : kAstcBlocks) {
            if (astcMip0Bytes(info.width, info.height, b[0], b[1]) == rawLen) {
                info.blockW = b[0]; info.blockH = b[1]; break;
            }
        }
    }
    // Last resort: legacy formatCode guess so we still attempt a decode.
    if (info.blockW == 0) {
        astcBlockSize(info.formatCode, info.blockW, info.blockH);
    }
    return true;
}

// ───────────────────────────────────────────────────────
// ASTC decode via ARM astcenc library (exact hardware match)
// ───────────────────────────────────────────────────────

namespace astc {

// IEEE half -> float (local copy so the parser has no dependency on ibl.h).
inline float astcHalf2Float(uint16_t h) {
    uint32_t s=(h&0x8000u)<<16, e=(h>>10)&0x1F, m=h&0x3FF, bits;
    if (e==0){ if(m==0) bits=s; else { e=127-15+1; while(!(m&0x400)){m<<=1;--e;} m&=0x3FF; bits=s|(e<<23)|(m<<13);} }
    else if (e==31) bits=s|0x7F800000u|(m<<13);
    else bits=s|((e+(127-15))<<23)|(m<<13);
    float f; std::memcpy(&f,&bits,4); return f;
}

inline bool decodeASTC(const u8* rawMips, u32 rawLen, u32 width, u32 height,
                       u32 bw, u32 bh, std::vector<u8>& outRGBA, bool hdr = false) {
    // ⛔ UNCOMPRESSED RENDTXTR payloads (NOT ASTC): some maps are cooked raw — esp. tiny VAT placeholders like
    // horror's `t_vat_blackblack` (1x1, formatCode 1, 8 bytes = one RGBA16F texel). astcenc rejects them
    // (8 < a 16-byte ASTC block) -> "ASTC decode failed" + the texture was SKIPPED. Detect by exact byte-count
    // (rawLen == w*h*8 -> RGBA16F half-float; == w*h*4 -> RGBA8) and decode directly. Generalizes to any env.
    if (rawLen == (u32)((u64)width * height * 8)) {
        outRGBA.resize((size_t)width * height * 4);
        for (size_t i = 0; i < (size_t)width * height; ++i)
            for (int c = 0; c < 4; ++c) { uint16_t h; std::memcpy(&h, rawMips + i*8 + c*2, 2);
                float f = astcHalf2Float(h); int v = (int)(std::max(0.f, std::min(1.f, f)) * 255.f + 0.5f); outRGBA[i*4+c] = (u8)v; }
        return true;
    }
    if (rawLen == (u32)((u64)width * height * 4)) { outRGBA.assign(rawMips, rawMips + (size_t)width*height*4); return true; }
    // Clamp to exactly the first mip's block data — extra mip tails make astcenc reject.
    u32 cols = (width  + bw - 1) / bw;
    u32 rows = (height + bh - 1) / bh;
    u32 mip0Size = cols * rows * 16;
    if (rawLen > mip0Size) rawLen = mip0Size;
    if (rawLen < mip0Size) return false;

    astcenc_config cfg = {};
    // HDR-encoded ASTC (the .hdr baked lightmaps) MUST use the HDR profile. Decoding them as LDR makes
    // astcenc emit MAGENTA error texels, which the lightmap shader multiplies into the base -> purple ground.
    astcenc_error err = astcenc_config_init(
        hdr ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR, bw, bh, 1,
        ASTCENC_PRE_MEDIUM,
        ASTCENC_FLG_DECOMPRESS_ONLY,
        &cfg);
    if (err != ASTCENC_SUCCESS) return false;

    // Decode ACROSS CORES: astcenc cooperatively decompresses one image from N worker threads (each gets a
    // thread_index 0..N-1 on the same context). The 2048² baked textures dominate load time; this divides the
    // per-image decode by the core count. The loader allocates a fresh context per call (one image), so no
    // inter-image reset is needed. HSR_ASTC_THREADS overrides the worker count.
    unsigned N = std::thread::hardware_concurrency(); if (N < 1) N = 1; if (N > 16) N = 16;
    if (const char* e = std::getenv("HSR_ASTC_THREADS")) { int v = atoi(e); if (v >= 1) N = (unsigned)v; }

    astcenc_context* ctx = nullptr;
    err = astcenc_context_alloc(&cfg, N, &ctx);
    if (err != ASTCENC_SUCCESS) return false;

    outRGBA.resize(width * height * 4);
    const astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    auto runMT = [&](astcenc_image* im) -> astcenc_error {
        if (N <= 1) return astcenc_decompress_image(ctx, rawMips, rawLen, im, &swz, 0);
        std::vector<std::thread> th; std::vector<astcenc_error> e(N, ASTCENC_SUCCESS);
        for (unsigned t = 0; t < N; ++t)
            th.emplace_back([&, t] { e[t] = astcenc_decompress_image(ctx, rawMips, rawLen, im, &swz, t); });
        for (auto& x : th) x.join();
        for (auto ee : e) if (ee != ASTCENC_SUCCESS) return ee;
        return ASTCENC_SUCCESS;
    };

    if (hdr) {
        // Decode HDR ASTC to float, then clamp to [0,1] -> u8 (baked lighting; >1 highlights clamp to white).
        std::vector<float> fbuf((size_t)width * height * 4);
        float* fptr = fbuf.data();
        astcenc_image fimg = {}; fimg.dim_x = width; fimg.dim_y = height; fimg.dim_z = 1;
        fimg.data_type = ASTCENC_TYPE_F32; fimg.data = reinterpret_cast<void**>(&fptr);
        err = runMT(&fimg);
        astcenc_context_free(ctx);
        if (err != ASTCENC_SUCCESS) return false;
        // Baked HDR lightmaps are RADIANCE. The device EXPOSES + ACES-tonemaps them; a raw [0,1] clamp leaves
        // the baked lighting TOO DARK (chair/horror/candle). Apply exposure + ACES filmic to RGB so it reads
        // naturally; alpha passes through linearly. HSR_LMEXP tunes the exposure.
        float lmExp = g_lmExposure; if (const char* e = std::getenv("HSR_LMEXP")) { float ev=(float)atof(e); if (ev>0) lmExp=ev; }
        for (size_t i = 0; i < fbuf.size(); ++i) {
            float v = fbuf[i]; if (v < 0) v = 0;
            if ((i & 3) != 3) { v *= lmExp; v = (v*(2.51f*v+0.03f))/(v*(2.43f*v+0.59f)+0.14f); }
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            outRGBA[i] = (u8)(v * 255.0f + 0.5f);
        }
        return true;
    }

    u8* slicePtr = outRGBA.data();
    astcenc_image img = {};
    img.dim_x = width;
    img.dim_y = height;
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    img.data = reinterpret_cast<void**>(&slicePtr);

    err = runMT(&img);
    astcenc_context_free(ctx);

    return err == ASTCENC_SUCCESS;
}

} // namespace astc
