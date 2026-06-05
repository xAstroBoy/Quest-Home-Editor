#pragma once
// IBL cubemap decode + CPU sampling for the V79 SpecIbl path.
//
// V79 OPA homes light `*_specibl` materials with image-based lighting: the env ships a DIFFUSE
// (irradiance) cubemap + a SPECULAR (roughness-prefiltered) cubemap as `cache/android/.../*_diffuse.dds.opa`
// / `*_specular.dds.opa`. Despite the `.dds` name they are cooked as **KTX**, format **GL_RGBA16F**
// (glType GL_HALF_FLOAT 0x140B, glInternalFormat GL_RGBA16F 0x881A), **6 cube faces** + mips —
// UNCOMPRESSED half-float, so no BC6H decode is needed, just half->float + the face/mip layout.
// (Confirmed from the V79 libshell: SpecIbl uses DiffuseCubemap + SpecularCubemap + specularIBLTint;
//  recipe = albedo·diffuseCube(N)·ambientTint + specCube(reflect(-V,N),rough)·specTint, no dir light.)
//
// We decode the cubemap to float RGB faces and sample it on the CPU per vertex (the diffuse layer is
// low-frequency irradiance, so per-vertex is faithful and avoids a cube-sampling SPIR-V shader).

#include "types.h"
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace ibl {

// IEEE half -> float
inline float half2float(uint16_t h) {
    uint32_t s = (h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1F;
    uint32_t m = h & 0x3FF;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) bits = s;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; --e; } m &= 0x3FF; bits = s | (e << 23) | (m << 13); }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + (127 - 15)) << 23) | (m << 13);
    }
    float f; std::memcpy(&f, &bits, 4); return f;
}

// A decoded float cubemap (mip 0 only; that's all the diffuse irradiance map has, and it's enough
// for diffuse lighting). faces[0..5] = +X,-X,+Y,-Y,+Z,-Z, each `size*size` RGB triples.
struct Cubemap {
    int size = 0;
    std::vector<float> faces[6];   // size*size*3 each
    bool ok() const { return size > 0 && !faces[0].empty(); }
};

// Decode an OPAA/KTX RGBA16F cubemap (mip 0, 6 faces) from the raw `.dds.opa` bytes.
inline bool decodeCubemap(const uint8_t* data, size_t n, Cubemap& out) {
    static const uint8_t id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
    // find the KTX magic (the OPAA container precedes it)
    size_t off = std::string::npos;
    for (size_t i = 0; i + 12 <= n; ++i) if (std::memcmp(data + i, id, 12) == 0) { off = i; break; }
    if (off == std::string::npos) return false;
    auto u32a = [&](size_t o){ uint32_t v; std::memcpy(&v, data + o, 4); return v; };
    size_t p = off + 12 + 4;                 // skip 12-byte identifier + 4-byte endianness
    // KTX header fields (u32, from p): glType@0 glTypeSize@4 glFormat@8 glInternalFormat@12
    // glBaseInternalFormat@16 pixelWidth@20 pixelHeight@24 pixelDepth@28 numArrayElems@32
    // numberOfFaces@36 numberOfMipmapLevels@40 bytesOfKeyValueData@44.
    uint32_t glType = u32a(p + 0);
    uint32_t glIntFmt = u32a(p + 12);
    uint32_t w = u32a(p + 20), h = u32a(p + 24);
    uint32_t nFaces = u32a(p + 36);
    uint32_t kvBytes = u32a(p + 44);
    if (glType != 0x140B || glIntFmt != 0x881A) return false;   // require GL_HALF_FLOAT + GL_RGBA16F
    if (nFaces != 6 || w == 0 || w != h) return false;
    size_t dp = p + 48 + kvBytes;
    if (dp + 4 > n) return false;
    uint32_t imageSize = u32a(dp); dp += 4;
    size_t faceBytes = (size_t)w * h * 4 * 2;            // RGBA half
    // imageSize is sometimes per-face, sometimes the whole 6-face block (cooked variant). Accept both.
    if (imageSize != faceBytes && imageSize != faceBytes * 6) {
        // be lenient: just require the data to be present
    }
    if (dp + faceBytes * 6 > n) return false;
    out.size = (int)w;
    for (int f = 0; f < 6; ++f) {
        out.faces[f].resize((size_t)w * h * 3);
        const uint8_t* fp = data + dp + (size_t)f * faceBytes;
        for (size_t t = 0; t < (size_t)w * h; ++t) {
            uint16_t r, g, b;
            std::memcpy(&r, fp + t*8 + 0, 2);
            std::memcpy(&g, fp + t*8 + 2, 2);
            std::memcpy(&b, fp + t*8 + 4, 2);
            out.faces[f][t*3+0] = half2float(r);
            out.faces[f][t*3+1] = half2float(g);
            out.faces[f][t*3+2] = half2float(b);
        }
    }
    return true;
}

// Extract the RAW mip-0 RGBA16F face data (6 faces, contiguous +X,-X,+Y,-Y,+Z,-Z) straight from the
// KTX, for direct GPU upload as a VK_FORMAT_R16G16B16A16_SFLOAT cubemap (the SPECULAR reflection map).
// No float conversion — the half-float bytes go to the GPU as-is. Returns face size.
inline bool extractCubeRawRGBA16F(const uint8_t* data, size_t n, int& outSize, std::vector<uint8_t>& outFaces) {
    static const uint8_t id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
    size_t off = std::string::npos;
    for (size_t i = 0; i + 12 <= n; ++i) if (std::memcmp(data + i, id, 12) == 0) { off = i; break; }
    if (off == std::string::npos) return false;
    auto u32a = [&](size_t o){ uint32_t v; std::memcpy(&v, data + o, 4); return v; };
    size_t p = off + 12 + 4;
    uint32_t glType = u32a(p + 0), glIntFmt = u32a(p + 12);
    uint32_t w = u32a(p + 20), h = u32a(p + 24), nFaces = u32a(p + 36), kvBytes = u32a(p + 44);
    if (glType != 0x140B || glIntFmt != 0x881A || nFaces != 6 || w == 0 || w != h) return false;
    size_t dp = p + 48 + kvBytes;
    if (dp + 4 > n) return false;
    dp += 4;                                          // skip mip0 imageSize
    size_t faceBytes = (size_t)w * h * 4 * 2;
    if (dp + faceBytes * 6 > n) return false;
    outSize = (int)w;
    outFaces.assign(data + dp, data + dp + faceBytes * 6);
    return true;
}

// Sample the cubemap in direction d (need not be normalized). Bilinear within the selected face.
// Standard GL cube face selection (faces +X,-X,+Y,-Y,+Z,-Z), top-left origin.
inline void sample(const Cubemap& cm, float dx, float dy, float dz, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!cm.ok()) return;
    float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) { ma = ax; if (dx >= 0) { face = 0; sc = -dz; tc = -dy; } else { face = 1; sc = dz; tc = -dy; } }
    else if (ay >= az)        { ma = ay; if (dy >= 0) { face = 2; sc =  dx; tc =  dz; } else { face = 3; sc = dx; tc = -dz; } }
    else                      { ma = az; if (dz >= 0) { face = 4; sc =  dx; tc = -dy; } else { face = 5; sc = -dx; tc = -dy; } }
    if (ma < 1e-12f) ma = 1e-12f;
    float u = (sc / ma + 1.0f) * 0.5f;
    float v = (tc / ma + 1.0f) * 0.5f;
    int S = cm.size;
    float fx = std::min(std::max(u * S - 0.5f, 0.0f), (float)S - 1.0f);
    float fy = std::min(std::max(v * S - 0.5f, 0.0f), (float)S - 1.0f);
    int x0 = (int)fx, y0 = (int)fy; int x1 = std::min(x0+1, S-1), y1 = std::min(y0+1, S-1);
    float wx = fx - x0, wy = fy - y0;
    const std::vector<float>& F = cm.faces[face];
    auto px = [&](int x, int y, int c){ return F[((size_t)y*S + x)*3 + c]; };
    for (int c = 0; c < 3; ++c) {
        float a = px(x0,y0,c)*(1-wx) + px(x1,y0,c)*wx;
        float b = px(x0,y1,c)*(1-wx) + px(x1,y1,c)*wx;
        out[c] = a*(1-wy) + b*wy;
    }
}

// Decode the StdData octahedral a_normal (i16x2, range [-1,1]) -> unit vec3.
inline void octDecodeNormal(int16_t ex, int16_t ey, float out[3]) {
    float fx = std::max(-1.0f, std::min(1.0f, ex / 32767.0f));
    float fy = std::max(-1.0f, std::min(1.0f, ey / 32767.0f));
    float nx = fx, ny = fy, nz = 1.0f - std::fabs(fx) - std::fabs(fy);
    if (nz < 0.0f) {
        float ox = (1.0f - std::fabs(ny)) * (nx >= 0 ? 1.0f : -1.0f);
        float oy = (1.0f - std::fabs(nx)) * (ny >= 0 ? 1.0f : -1.0f);
        nx = ox; ny = oy;
    }
    float l = std::sqrt(nx*nx + ny*ny + nz*nz); if (l < 1e-8f) { out[0]=0;out[1]=0;out[2]=1; return; }
    out[0] = nx/l; out[1] = ny/l; out[2] = nz/l;
}

} // namespace ibl
