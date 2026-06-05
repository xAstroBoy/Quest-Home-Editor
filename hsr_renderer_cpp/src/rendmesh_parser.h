#pragma once
#include "types.h"
#include <cstring>
#include <cmath>

// RENDMESH FlatBuffer parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/MeshAssetHandler.cpp
// + MeshAssetBuilder.cpp (sub_1816448 in libshell_current)
//
// Confirmed 4-level hierarchy:
//   Root → LOD [0] → Part [0] → VS [0] → VB (stride-20) + IB (u16)

inline bool parseRendMesh(const std::vector<u8>& data,
                          std::vector<float>& positions,
                          std::vector<float>& uvs,
                          std::vector<u32>& indices,
                          std::vector<u8>* boneIndices = nullptr,
                          std::vector<u8>* boneWeights = nullptr,
                          const char* label = nullptr) {
    if (data.size() < 14 || memcmp(data.data() + 4, "MESH", 4) != 0)
        return false;

    auto rootOff = *reinterpret_cast<const u32*>(data.data());

    auto fieldFo = [&](u32 tablePos, u32 fi) -> u16 {
        i32 soff = *reinterpret_cast<const i32*>(data.data() + tablePos);
        u32 vt = tablePos - soff;
        if (vt + 4 > data.size()) return 0;
        u16 vtSize = *reinterpret_cast<const u16*>(data.data() + vt);
        u32 idx = vt + 4 + fi * 2;
        if (idx + 2 > vt + vtSize) return 0;
        return *reinterpret_cast<const u16*>(data.data() + idx);
    };

    auto followVec = [&](u32 tablePos, u32 fi, u32& baseOut, u32& countOut) -> bool {
        u16 fo = fieldFo(tablePos, fi);
        if (!fo) return false;
        u32 fieldSlot = tablePos + fo;
        if (fieldSlot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + fieldSlot);
        if (!uoff) return false;
        u32 vecPos = fieldSlot + uoff;
        if (vecPos + 4 > data.size()) return false;
        countOut = *reinterpret_cast<const u32*>(data.data() + vecPos);
        baseOut = vecPos + 4;
        return true;
    };

    auto followElem = [&](u32 elemBase, u32 i, u32& tableOut) -> bool {
        u32 slot = elemBase + i * 4;
        if (slot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + slot);
        if (!uoff) return false;
        tableOut = slot + uoff;
        return true;
    };

    auto followByteVec = [&](u32 tablePos, u32 fi, u32& startOut, u32& countOut) -> bool {
        u16 fo = fieldFo(tablePos, fi);
        if (!fo) return false;
        u32 fieldSlot = tablePos + fo;
        if (fieldSlot + 4 > data.size()) return false;
        u32 uoff = *reinterpret_cast<const u32*>(data.data() + fieldSlot);
        if (!uoff) return false;
        u32 vecPos = fieldSlot + uoff;
        if (vecPos + 4 > data.size()) return false;
        countOut = *reinterpret_cast<const u32*>(data.data() + vecPos);
        startOut = vecPos + 4;
        return true;
    };

    // Root -> LOD[0] (root field[1] = lod_uoff)
    u32 lodBase, lodCount;
    if (!followVec(rootOff, 1, lodBase, lodCount) || !lodCount) return false;
    u32 lodTable;
    if (!followElem(lodBase, 0, lodTable)) return false;

    // LOD -> Parts. A mesh can have MULTIPLE parts; prism_wave_a_01 has 2 — reading only Part[0]
    // rendered HALF the mesh (the "ring with a hole / borked" look). Read ALL parts, concatenating
    // verts and offsetting each part's index buffer by the running vertex count.
    u32 partBase, partCount;
    if (!followVec(lodTable, 0, partBase, partCount) || !partCount) return false;

    auto f16tof32 = [](u16 h) -> float {
        u32 s = (h >> 15) & 1;
        u32 e = (h >> 10) & 0x1F;
        u32 m = h & 0x3FF;
        if (e == 0) {
            if (m == 0) return s ? -0.0f : 0.0f;
            while (!(m & 0x400)) { m <<= 1; e--; }
            e++; m &= 0x3FF;
        } else if (e == 31) {
            return m ? NAN : (s ? -INFINITY : INFINITY);
        }
        u32 bits = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };

    u32 totalV = 0;
    for (u32 pi = 0; pi < partCount; ++pi) {
        u32 partTable;
        if (!followElem(partBase, pi, partTable)) continue;
        // Part -> VS[0] (field[0]=vs_uoff) + IB bytes (field[1]=idx_uoff)
        u32 vsBase, vsCount;
        if (!followVec(partTable, 0, vsBase, vsCount) || !vsCount) continue;
        u32 vsTable;
        if (!followElem(vsBase, 0, vsTable)) continue;
        u32 ibStart, ibCount;
        if (!followByteVec(partTable, 1, ibStart, ibCount) || !ibCount) continue;
        // VS -> vertex_count (field[1]) + VB bytes (field[2])
        u16 vcFo = fieldFo(vsTable, 1);
        if (!vcFo) continue;
        u32 nVerts = *reinterpret_cast<const u32*>(data.data() + vsTable + vcFo);
        u32 vbStart, vbCount;
        if (!followByteVec(vsTable, 2, vbStart, vbCount) || !nVerts) continue;
        if (vbCount % nVerts != 0) continue;
        u32 stride = vbCount / nVerts;
        if (stride < 20 || vbStart + (size_t)nVerts * stride > data.size()) continue;
        if (std::getenv("HSR_RMSTRUCT"))
            fprintf(stderr, "[RMSTRUCT] part %u/%u nVerts=%u stride=%u ibCount=%u base=%u\n",
                    pi, partCount, nVerts, stride, ibCount, totalV);

    // Position is always the first attribute (3x f32 @ +0). UV offset, however,
    // varies by vertex layout: nuxd is stride-20 with UV f16x2 @ +12, but other
    // envs (haven2025) use wider strides (48) with normal/tangent before the UV, so
    // a hardcoded +12 reads garbage. Auto-DETECT the UV attribute: scan every 2-byte
    // aligned slot in [12, stride-4) and score it as f16x2 across sampled vertices —
    // a real texcoord set has values mostly within roughly [-0.05, 1.05] (tiled UVs
    // can exceed 1 but cluster low) and is NOT constant. Pick the best-scoring slot.
    // UV is f16x2 and 4-byte aligned, right after position in most layouts (off 12).
    // Only consider 4-byte-aligned slots (mis-aligned 2-byte reads of color/normal
    // bytes can look UV-like and were being mis-picked, garbling props/pillows).
    // A real texcoord set: nearly all samples finite & in a sane range, AND varies
    // across vertices (constant slots are packed color/normal). Pick the EARLIEST
    // qualifying slot (UV precedes normal/tangent), preferring offset 12.
    u32 uvOff = 12;
    // Wide layout (haven2025 / V203 USD, stride 48): pos f32x3 @0 + normal f32x3 @12 + uv0 f16x2 @24
    // (matches the V203 shader's vertex stream pos/norm/uv). The generic f16x2 auto-detect below mis-fires
    // here because the f32 NORMAL bytes @12 reinterpret as plausible f16 UVs -> magenta UV scramble.
    // So FIRST probe for a unit-length f32 normal at @12; if present, uv0 sits right after it at @24.
    bool normAt12 = false;
    if (stride >= 40) {
        normAt12 = true;
        u32 sN = nVerts < 32 ? nVerts : 32;
        for (u32 i = 0; i < sN && normAt12; ++i) {
            const float* nf = reinterpret_cast<const float*>(data.data() + vbStart + i * stride + 12);
            float L = std::sqrt(nf[0]*nf[0] + nf[1]*nf[1] + nf[2]*nf[2]);
            if (!(L > 0.9f && L < 1.1f)) normAt12 = false;
        }
    }
    // stride==28 is the VERIFIED skinned layout (pos@0, uv f16x2 @12, color@16, boneIdx@20, boneWgt@24).
    // Do NOT auto-detect there: a skinned surface with TILED UVs (prism_wave, 32x32) has uv values > the
    // detector's [-4,16] range at off 12, so it rejected the real UV and mis-picked the boneIdx bytes as
    // UVs -> garbled texture. Auto-detect only for the variable non-skinned layouts.
    if (normAt12) {
        uvOff = 24;
    } else if (stride != 28) {
        u32 sampleN = nVerts < 128 ? nVerts : 128;
        u32 chosen = 0xFFFFFFFF;
        for (u32 off = 12; off + 4 <= stride; off += 4) {
            int inRange = 0; float minU=1e9f,maxU=-1e9f,minV=1e9f,maxV=-1e9f;
            for (u32 i = 0; i < sampleN; ++i) {
                const u8* v = data.data() + vbStart + i * stride;
                float u = f16tof32(*reinterpret_cast<const u16*>(v + off));
                float w = f16tof32(*reinterpret_cast<const u16*>(v + off + 2));
                if (std::isfinite(u) && std::isfinite(w) &&
                    u > -4.0f && u < 16.0f && w > -4.0f && w < 16.0f) {
                    inRange++;
                    if(u<minU)minU=u; if(u>maxU)maxU=u; if(w<minV)minV=w; if(w>maxV)maxV=w;
                }
            }
            bool allValid = (inRange >= (int)(sampleN * 0.95f));
            bool varies = (maxU - minU > 0.02f) || (maxV - minV > 0.02f);
            if (allValid && varies) { chosen = off; break; }  // earliest wins
        }
        uvOff = (chosen != 0xFFFFFFFF) ? chosen : 12;
    }
    if (const char* uo = std::getenv("HSR_UVOFF")) uvOff = (u32)atoi(uo);   // DIAG: force UV offset

    if (std::getenv("HSR_RMFMT") && label && strstr(label, "culptur")) {
        fprintf(stderr, "[RMFMT] %s part stride=%u uvOff_detected=%u nVerts=%u normAt12=%d\n", label, stride, uvOff, nVerts, (int)normAt12);
        const u8* v0 = data.data() + vbStart;
        const u8* v1 = data.data() + vbStart + stride;
        for (u32 o = 0; o + 4 <= stride; o += 4) {
            float f = *reinterpret_cast<const float*>(v0 + o);
            float a0 = f16tof32(*reinterpret_cast<const u16*>(v0 + o)), b0 = f16tof32(*reinterpret_cast<const u16*>(v0 + o + 2));
            float a1 = f16tof32(*reinterpret_cast<const u16*>(v1 + o)), b1 = f16tof32(*reinterpret_cast<const u16*>(v1 + o + 2));
            int16_t s0 = *reinterpret_cast<const int16_t*>(v0 + o), s2 = *reinterpret_cast<const int16_t*>(v0 + o + 2);
            fprintf(stderr, "   @%2u f32=%11.4f  f16v0=(%8.3f,%8.3f) f16v1=(%8.3f,%8.3f) i16=(%6d,%6d)\n",
                    o, f, a0, b0, a1, b1, s0, s2);
        }
    }

        u32 base = totalV;
        positions.resize((base + nVerts) * 3);
        uvs.resize((base + nVerts) * 2);
        for (u32 i = 0; i < nVerts; ++i) {
            const u8* v = data.data() + vbStart + i * stride;
            positions[(base+i)*3+0] = *reinterpret_cast<const float*>(v + 0);
            positions[(base+i)*3+1] = *reinterpret_cast<const float*>(v + 4);
            positions[(base+i)*3+2] = *reinterpret_cast<const float*>(v + 8);
            uvs[(base+i)*2+0] = f16tof32(*reinterpret_cast<const u16*>(v + uvOff));
            uvs[(base+i)*2+1] = f16tof32(*reinterpret_cast<const u16*>(v + uvOff + 2));
        }

        // Bone indices + weights. VERIFIED stride==28 layout: pos@0(12) + uv@12(4 f16x2) +
        // color@16(4 u8) + boneIdx@20(4 u8) + boneWgt@24(4 u8). Non-skinned parts (stride<28)
        // bind to bone 0 at full weight so they ride the node transform.
        // Only a skinned mesh (some part has the stride>=28 bone layout) gets bones; a purely static
        // mesh keeps boneIndices empty -> static pipeline. A static part inside a skinned mesh binds bone 0.
        if (boneIndices && boneWeights && (stride >= 28 || !boneIndices->empty())) {
            boneIndices->resize((base + nVerts) * 4, 0);
            boneWeights->resize((base + nVerts) * 4, 0);
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride;
                if (stride >= 28) {
                    for (int k = 0; k < 4; ++k) { (*boneIndices)[(base+i)*4+k] = v[20+k]; (*boneWeights)[(base+i)*4+k] = v[24+k]; }
                } else {
                    (*boneWeights)[(base+i)*4+0] = 255;  // bind to bone 0
                }
            }
        }

        // u16 index buffer, offset by this part's vertex base; clamp OOB to base.
        u32 nIdx = ibCount / 2;
        for (u32 i = 0; i < nIdx; ++i) {
            u32 off = ibStart + i * 2;
            if (off + 2 > data.size()) break;
            u16 idx = *reinterpret_cast<const u16*>(data.data() + off);
            indices.push_back((idx < nVerts) ? (base + (u32)idx) : base);
        }
        if (std::getenv("HSR_RMVTX") && stride >= 28 && nVerts > 1000) {
            float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
            for (u32 i = base; i < base+nVerts; ++i) for (int c=0;c<3;c++){ float p=positions[i*3+c]; if(p<mn[c])mn[c]=p; if(p>mx[c])mx[c]=p; }
            fprintf(stderr, "[RMVTX] part%u bounds x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\n", pi, mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]);
            for (int s=0;s<4;s++){ const u8* v=data.data()+vbStart+(size_t)s*stride;
                fprintf(stderr,"  v%d pos=(%.3f,%.3f,%.3f) raw=", s,*(const float*)v,*(const float*)(v+4),*(const float*)(v+8));
                for(int b=0;b<28;b++) fprintf(stderr,"%02x",v[b]); fprintf(stderr,"\n"); }
        }
        totalV += nVerts;
    }

    return totalV > 0 && !indices.empty();
}
