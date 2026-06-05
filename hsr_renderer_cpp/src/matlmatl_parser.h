#pragma once
#include "types.h"
#include <cstring>
#include <vector>
#include <string>

// MATLMATL FlatBuffer parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/MaterialAssetHandler.cpp
// (sub_B374C4 in libshell)

struct MatlmatlInfo {
    u64 shaderPkg = 0;
    u64 shaderIng = 0;
    u32 shaderTgt = 0;
    u64 texPkg = 0;
    u64 texIng = 0;       // chosen (base-color) texture ing
    u32 texTgt = 0;
    std::vector<u64> texIngs;  // ALL texture ings referenced by this material, in file order
    // The cooked material bakes its final matParams constant block (Tint/LayerRed/LayerBlue/Metallic/
    // Roughness/... in the shader's matParams UBO byte layout): u32 size @ +76, raw bytes @ +80.
    std::vector<u8> constBlock;
};

inline bool parseMatlmatl(const std::vector<u8>& data, MatlmatlInfo& info) {
    // Need at least 68 bytes for shader ref (at offsets 48/56/64) + MATL magic at +4
    if (data.size() < 68) return false;
    if (memcmp(data.data() + 4, "MATL", 4) != 0) return false;

    // Shader ref: confirmed at offsets 48/56/64 for both v79_test (176B) and haven2025 (288B)
    info.shaderPkg = *reinterpret_cast<const u64*>(data.data() + 48);
    info.shaderIng = *reinterpret_cast<const u64*>(data.data() + 56);
    info.shaderTgt = *reinterpret_cast<const u32*>(data.data() + 64);

    // Constant params block: size u32 @ +76, raw matParams UBO bytes @ +80. This is the cooked,
    // final per-material parameter block in the material's shader's matParams layout — the REAL
    // Tint/LayerRed/LayerBlue/MetallicMultiplier/Roughness values (defaults are white -> gray).
    if (data.size() >= 80) {
        u32 blkSz = *reinterpret_cast<const u32*>(data.data() + 76);
        if (blkSz >= 4 && blkSz <= 1024 && (size_t)80 + blkSz <= data.size())
            info.constBlock.assign(data.begin() + 80, data.begin() + 80 + blkSz);
    }

    // Scan for texture ings by searching for tgt sentinel 0x6E4CC522 (all texture assets).
    // Layout: [8B ing][4B tgt=0x6E4CC522]. For pbrlightmap materials the LAST pair is basecolor.
    // v79_test (176B): single texture at ing@128, tgt@136 — correctly matched by this scan.
    // haven2025 (288B+): multiple textures; last is basecolor.
    // Collect ALL texture ings (a material may reference base-color + normal + AO +
    // lightmap). We keep them in file order; the loader picks the BASE-COLOR one by
    // resolving each ing's path and preferring "_basecolor"/"_basecolormetallic" over
    // normal("_onxrny")/AO("_rbaodir")/etc. Picking the wrong one rendered purple.
    static constexpr u32 TEX_TGT = 0x6E4CC522u;
    for (u32 i = 72; i + 12 <= (u32)data.size(); i += 4) {
        u32 val = *reinterpret_cast<const u32*>(data.data() + i);
        if (val == TEX_TGT && i >= 8) {
            u64 ing = *reinterpret_cast<const u64*>(data.data() + i - 8);
            if (ing >= 0x100000000ULL) {
                info.texIngs.push_back(ing);
                info.texIng = ing;       // default = last (overridden by loader)
                info.texTgt = TEX_TGT;
                info.texPkg = 0;
            }
        }
    }

    return true;
}
