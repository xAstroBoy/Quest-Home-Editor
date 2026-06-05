#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <tuple>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;

struct AssetKey {
    u64 pkg = 0;
    u64 ing = 0;
    u32 tgt = 0;

    bool operator==(const AssetKey& o) const {
        return pkg == o.pkg && ing == o.ing && tgt == o.tgt;
    }
};

struct AssetKeyHash {
    size_t operator()(const AssetKey& k) const {
        auto h1 = std::hash<u64>{}(k.pkg);
        auto h2 = std::hash<u64>{}(k.ing);
        auto h3 = std::hash<u32>{}(k.tgt);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

using AssetMap = std::unordered_map<AssetKey, std::string, AssetKeyHash>;

struct Transform {
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1}; // xyzw quaternion
    float scale[3] = {1, 1, 1};
};

struct DrawableEntity {
    std::string name;
    Transform   transform;
    AssetKey    meshRef;
    std::vector<AssetKey> matRefs;
};

struct MeshData {
    std::string name;
    std::vector<float> positions;  // xyz * nVerts
    std::vector<float> uvs;        // uv * nVerts
    std::vector<u32>    indices;   // triangle list (32-bit: large glTF meshes exceed 65535 verts)
    u32 nVerts = 0;
    u32 nIdx   = 0;

    // Base-color texture (decoded RGBA). For isotropictiled this is BaseColorMetallic_Tx
    // (RGB=base color, A=metallic).
    std::vector<u8> texRGBA;
    u32 texW = 1, texH = 1;
    bool hasTexture = false;

    // Additional per-material textures, bound to shader slots BY ROLE (libshell binds
    // each shader texture resource by name to the material's named texture param):
    //   normal  -> ONxRNy_Tx  (packed occlusion/normal/roughness)
    //   orm     -> ORM/roughness/metallic/AO slot (if the shader has one)
    //   lightmap-> baked lightmap slot
    std::vector<u8> normalRGBA;  u32 normalW = 1, normalH = 1; bool hasNormal = false;
    std::vector<u8> ormRGBA;     u32 ormW = 1, ormH = 1;       bool hasOrm = false;
    std::vector<u8> emissiveRGBA; u32 emissiveW = 1, emissiveH = 1; bool hasEmissive = false;
    std::vector<u8> lmRGBA;      u32 lmW = 1, lmH = 1;         bool hasLightmap = false;
    // matParams.Tint (per-material RGB color multiplier; default white).
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // Ingestion id of the RENDSHAD this material references — the renderer picks the
    // matching shader pipeline per mesh (masked->discard, emissive->glow, etc.), as libshell does.
    u64 shaderIng = 0;
    // Resolved shader asset path (".../NAME.surface/shader...") — used to apply matParamsBlob only
    // when this material's shader matches the renderer's chosen global shader (same UBO layout).
    std::string shaderPath;
    // Cooked per-material matParams constant block (real Tint/LayerRed/LayerBlue/Metallic/...), in the
    // material's shader's matParams UBO byte layout. Empty if the material has no constants.
    std::vector<u8> matParamsBlob;

    // Raw ASTC data for GPU-native upload (preferred when GPU supports ASTC_LDR)
    std::vector<u8> astcRaw;
    u32 astcBw = 0, astcBh = 0;

    // Transform
    Transform transform;

    // Bone skinning data (for unlitblendskinned meshes, stride=28)
    std::vector<u8> boneIndices;  // 4 u8 per vertex (bone indices)
    std::vector<u8> boneWeights;  // 4 u8 per vertex (bone weights UNORM)
    bool hasBones = false;

    // Blend mode: true = SRC_ALPHA blend (dome/motes); false = opaque (floor/fallback)
    bool useBlend = false;

    // Additive blend (libshell material Additive:true — god-rays / light shafts / glow). Goes to
    // the blend pass but with ADD blending (dst += src) so black texels add nothing and the bright
    // shaft adds light. Without it, an additive light-shaft renders as an opaque dark rectangle.
    bool additive = false;

    // Alpha-test cutout: opaque pass, depth-write ON, fragment discards below threshold
    // (libshell AlphaTest materials — flags/foliage/animals). Distinct from useBlend.
    bool alphaTest = false;

    // Editor overlay kind: normally these meshes are authored alongside render geometry but NOT
    // drawn (collision/navigation). The editor loads them as translucent flat-colour overlays so
    // navmeshes/collision can be inspected + edited. 0=normal visible mesh, 1=navmesh (green),
    // 2=collision/wall (red), 3=phys (orange).
    int overlayKind = 0;

    // glTF face culling: a material is doubleSided=false by default (single-sided -> back-face
    // cull, matching libshell). doubleSided=true -> no cull. Cel-shading inverted-hull OUTLINE
    // meshes are single-sided: without back-face culling the fattened black hull renders as a
    // solid blob that covers the whole interior ("fully dark"); culled, it shows only edge rims.
    // (OPA meshes keep the default true => CULL_NONE, unchanged.)
    bool doubleSided = true;

    // Dynamic vertices: positions are rewritten every frame (glTF skeletal animation).
    // The renderer keeps this mesh's VBO persistently mapped so an external animator can
    // stream skinned positions in. gltfMeshIndex links it back to the GltfLoader record.
    bool dynamicVerts = false;
    int  gltfMeshIndex = -1;

    // IBL-lit (V79 SpecIbl + no-albedo shells): the renderer bakes diffuseCube(worldN)·ambientTint
    // into the per-vertex color so these env-lit surfaces show the environment light instead of the
    // white/dark no-texture fallback. Set for *_specibl materials AND any opaque mesh whose albedo
    // texture didn't resolve (phantom Id) — both are lit purely by IBL in these homes.
    bool iblLit = false;

    // Full split-sum IBL (V79 SpecIbl no-albedo metallic/gem shells, e.g. divingHelmet/rubyGem):
    // the per-vertex colour is the complete specular-IBL reduction, not just diffuse irradiance —
    //   diffuse  = (1-metallic)·albedo·diffuseCube(N)·specibldiffusescale
    //   specular = F0·mix(specularCube(N), diffuseCube(N), roughness)·speciblspecularscale   (F0 = mix(0.04,albedo,metallic))
    //   colour   = (diffuse + specular)·ambientIBLTint
    // Read VERBATIM from the material's cooked uniforms (metallic/roughness/basecolor + the 4 specibl
    // params). Scoped to no-albedo-texture shells so textured *_specibl (goldmine ground) is untouched.
    bool  iblFullSpec = false;
    // Baked-lightmap lighting for TEXTURED interior shells (ShellEnv concrete/floor/walls). libshell
    // shades these `colour = diffuse(uv0·repeatuv) · lightmap(uv1) · lightmappower`. The lightmappower
    // is a per-channel HDR boost that produces the coloured neon/glow (concrete≈[3.8,3.2,4.4] pink,
    // floor≈[17.6,8.3,3.2] warm). We sample the lightmap (md.lmRGBA) at uv1 per vertex, scale by
    // lightmapPower, and bake into the per-vertex colour the fragment already multiplies by.
    bool  bakeLightmapVtx = false;
    std::vector<float> uvs2;        // uv1 = the lightmap unwrap (StdData a_texcoords.zw @16)
    float lightmapPower[3] = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;          // 'metallic' uniform (0 dielectric .. 1 metal)
    float roughness = 1.0f;         // 'roughness' uniform (clamped 0..1 for the mip mix)
    float speciblDiffScale = 1.0f;  // 'specibldiffusescale'
    float speciblSpecScale = 1.0f;  // 'speciblspecularscale'
    float albedoFactor[3] = {1.0f, 1.0f, 1.0f};  // 'basecolor' factor (linear) = the metal/gem tint

    // Per-frame material tint (rgba) = the mat.sanim MaterialTint track sampled at the current
    // time. Default white (1,1,1,1 = no effect). For fog/dust/flicker materials this carries the
    // animated OPACITY (alpha 0..~0.22) that keeps the fog faint and pulsing. animate(t) updates
    // it; the renderer pushes it as the fragment shader's UniformColor (frag = texture * tint).
    float curTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// RENDTXTR format codes → ASTC block sizes
inline void astcBlockSize(u8 fmtCode, u32& bw, u32& bh) {
    switch (fmtCode) {
        case  8: bw = 8;  bh = 8;  break;
        case 10: bw = 4;  bh = 4;  break;
        case 11: bw = 8;  bh = 8;  break;
        case 12: bw = 6;  bh = 6;  break;
        case 13: bw = 12; bh = 12; break;
        case 14: bw = 4;  bh = 4;  break;
        case 20: bw = 8;  bh = 8;  break;
        default: bw = 8;  bh = 8;  break;
    }
}
