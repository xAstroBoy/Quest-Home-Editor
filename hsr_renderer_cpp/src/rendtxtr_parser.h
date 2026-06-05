#pragma once
#include "types.h"
#include <cstring>
#include <cmath>
#include <algorithm>
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

inline bool decodeASTC(const u8* rawMips, u32 rawLen, u32 width, u32 height,
                       u32 bw, u32 bh, std::vector<u8>& outRGBA) {
    // Clamp to exactly the first mip's block data — extra mip tails make astcenc reject.
    u32 cols = (width  + bw - 1) / bw;
    u32 rows = (height + bh - 1) / bh;
    u32 mip0Size = cols * rows * 16;
    if (rawLen > mip0Size) rawLen = mip0Size;
    if (rawLen < mip0Size) return false;

    astcenc_config cfg = {};
    astcenc_error err = astcenc_config_init(
        ASTCENC_PRF_LDR, bw, bh, 1,
        ASTCENC_PRE_MEDIUM,
        ASTCENC_FLG_DECOMPRESS_ONLY,
        &cfg);
    if (err != ASTCENC_SUCCESS) return false;

    astcenc_context* ctx = nullptr;
    err = astcenc_context_alloc(&cfg, 1, &ctx);
    if (err != ASTCENC_SUCCESS) return false;

    outRGBA.resize(width * height * 4);
    u8* slicePtr = outRGBA.data();

    astcenc_image img = {};
    img.dim_x = width;
    img.dim_y = height;
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    img.data = reinterpret_cast<void**>(&slicePtr);

    const astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    err = astcenc_decompress_image(ctx, rawMips, rawLen, &img, &swz, 0);
    astcenc_context_free(ctx);

    return err == ASTCENC_SUCCESS;
}

} // namespace astc
