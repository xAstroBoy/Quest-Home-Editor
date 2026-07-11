# V79 OPA — EXTRACTED PROPERTIES (1:1 port inventory)

Everything the loader/cook extracts from a V79 env APK and where it lands. Compiled 2026-07-05 during the
stinson zen-tree fidelity audit ("EXTRACT ALL OPA PROPERTIES").

## Materials (`*.mat.txt` / `*.mat.asset` MaterialProperties)
| Property | Extracted → | Notes |
|---|---|---|
| Shader (ShellEnv/SpecIbl) | `mp.isSpecibl` | routes IBL vs unlit chain |
| Transparent / Additive | `md.useBlend` / `md.additive` | blend vs additive pass |
| AlphaTest | `md.alphaTest` | cutout (opaque pass + discard) |
| **alphatestthreshold** | `mp.alphaCutoff → md/em.alphaCutoff` | **was silently DROPPED until 2026-07-05**; per-material discard threshold (tree 0.5, zenheroplants 0.25, balloons 0.75); per-cutoff shader variants `skincutout_cNN` / `unlitcutout_cNN` |
| DoubleSided | `md.doubleSided` | reversed-tri append in cook |
| Depth / Unlit / Effect / SkyPass | flags read; Unlit=true for ALL stinson materials | V79 frag = `BaseColorFactor × tex` (ripped MeshShellEnv_runtime.frag.glsl) |
| alpha uniform | `mp.alpha` | scales fragment alpha (faint overlays) |
| diffuse uniform (RGBA) | `mp.diffuseColor` → COLOR_0 bake | flat color when untextured; tint when textured |
| metallic/roughness/specibl scales | `mp.*` | SpecIbl split-sum vertex bake |
| lightmappower (vec3) | `mp.lightmapPower` | lightmap HDR boost |
| Texture slots (diffuse/lightmap/normal/mr/emissive/ao) | `assetIdToTexBase` → decode | per-slot AssetRef ids |

## Vertex streams (StdData stride 20 + SkinnedPos)
| Stream | Extracted → | Notes |
|---|---|---|
| a_normal i16x2 @0 | (normals path) | |
| a_tangent i16x2 @4 | – | not consumed by unlit chain |
| **a_color u8x4 @8** | `md.colors` (skins too, 2026-07-05) | tree measured PURE WHITE — authored neutral |
| a_texcoords f16x4 @12 | `md.uvs` (uv0) + `md.uvs2` (uv1 lightmap) | v = 1−v flip (shader convention); **byte-diffed source vs cooked f16: EXACT** |
| SkinnedPos weights f16x4 @12 / bone idx u8x4 @20 | `sr.jw/jidx` → RENDMESH sem7/sem8 | dense palette remap |

## Textures (KTX11 ASTC)
- Full footprint table 0x93B0..0x93BD / sRGB (14 footprints) → block dims.
- Base mip decode → `md.texRGBA`; **lossless pass-through** of the source ASTC mip chain (`srcAstc`) when
  unmodified (blkEnum 6x6=18 / 8x8=20 / 12x12=24); masked textures keep the SOURCE mip count verbatim
  (synthesized coarse mips push box-filtered alpha above the discard threshold → uncut smear at range).

## Animation
- sanim node TRS tracks (per-node loop periods), MaterialTint RGBA cycles (`cookExtractTintRGBA` →
  TINTREPLAY frag, ratio-to-frame0 + frame-0 COLOR_0 bake fail-safe), UVTransform matrices (flipbook /
  scroll), skinned clips (skin→clip best-match), VAT offsets, per-instance atlas cells (field[4]).

## Shader containers (RENDSHAD) — structure knowledge
- Root: f5 uniforms, f6 skinning buffers, f7 STAGE table (module blobs), f9 textures, f11 vertex stream,
  f12 PASS table. Skinned bases carry passes **forwardSkinned, forward, forwardSkinned_debug, forward_debug**
  — skinned meshes DRAW forwardSkinned (the 2026-07-05 leaf-saga root cause: edits landed on `forward`).
- MATL field0 = u16 mode(2) + u16 **param-blob byte size** + the matParams UBO values (NOT a flag bitfield).

## Deliberately NOT ported (off-spec for V79) — USER DIRECTIVE
- haven2025/V205 `pbrlightmap_masked` technique state (modern-home MSAA alpha-to-coverage + its
  blend/rasterization pass state). Rationale, in order:
  1. **User directive** (2026-07-05): "explain to me why you reading haven2025 WHEN WE WANT THE V79
     SHADER & MAT & WHATEVER IT HAS PORTED!?" — the port target is V79, not the modern home.
  2. **V79 ground truth** (ripped `MeshShellEnv_runtime.frag.glsl`): masked surfaces are a plain
     `if (color.a < AlphaCutoff) discard` — there is no a2c in the V79 shader chain to port.
  3. What WAS learned before the directive is recorded anyway: the modern leaf technique =
     `animVege_UnpackedIsotropic.surface` (per haven's assets.manifest, ing 0x0311FF31BF7DCD29);
     official MATL field0 = u16 mode(2) + u16 param-blob byte size + matParams values (masked blob
     leads with alphaCutoff f32); pass tables carry forward/forwardCastShadow/forward_dynamic(+debug)
     entries with per-pass sub-tables. If a2c is ever wanted as an OPT-IN enhancement, that's the
     place to resume — it is NOT part of the 1:1 V79 port.
