#pragma once
#include "types.h"
#include "asmh_parser.h"
#include "rendmesh_parser.h"
#include "rendtxtr_parser.h"
#include "matlmatl_parser.h"
#include "hstf_parser.h"

#include "miniz.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <functional>
#include <set>
#include <cctype>


// ──────────────────────────────────────────────────────────────
// Scene Loader — mirrors libshell.so HSR environment loading
// ──────────────────────────────────────────────────────────────

#include "rendskel_parser.h"
class SceneLoader {
public:
    std::vector<MeshData> meshes;
    bool verbose = true;

    // ── v203 HzAnim skeletal animation (CPU skin into dynamicVerts, like OPA/glTF) ──
    RendSkel skel; RendClip clip;
    struct SkinRec { size_t meshIdx; std::vector<float> basePos; std::vector<u8> bIdx, bWgt; };
    std::vector<SkinRec> skinRecs;

    // Player spawn points (home_spawn_points.hstf JSON) -> editable cone markers (overlayKind 4).
    // tag "local" = the player start (cyan), "remote" = other spawns (yellow).
    struct SpawnPt { std::string name, tag; float pos[3]={0,0,0}, euler[3]={0,0,0}; int meshIdx=-1; };
    std::vector<SpawnPt> spawnPts;
    bool hasAnimation() const { return skel.ok() && clip.ok() && !skinRecs.empty(); }
    void animate(float t) {
        if (!hasAnimation()) return;
        std::vector<float> skin; sampleRendClip(skel, clip, t, skin);
        int nj = (int)skin.size()/16;
        for (auto& r : skinRecs) {
            auto& md = meshes[r.meshIdx];
            size_t nv = r.basePos.size()/3;
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            for (size_t v=0; v<nv; ++v) {
                float bx=r.basePos[v*3], by=r.basePos[v*3+1], bz=r.basePos[v*3+2];
                float ox=0,oy=0,oz=0,wsum=0;
                for (int k=0;k<4;k++) {
                    if (v*4+k >= r.bWgt.size()) break;
                    float w = r.bWgt[v*4+k]/255.0f; if (w<=0.f) continue;
                    int j = r.bIdx[v*4+k]; if (j<0||j>=nj) continue;
                    const float* m=&skin[16*j];
                    ox += w*(m[0]*bx+m[4]*by+m[8]*bz+m[12]);
                    oy += w*(m[1]*bx+m[5]*by+m[9]*bz+m[13]);
                    oz += w*(m[2]*bx+m[6]*by+m[10]*bz+m[14]);
                    wsum += w;
                }
                if (wsum < 1e-4f) { ox=bx; oy=by; oz=bz; }
                md.positions[v*3]=ox; md.positions[v*3+1]=oy; md.positions[v*3+2]=oz;
            }
            if (std::getenv("HSR_ANIMDBG") && nv > 1000) {
                float mn[3]={1e9f,1e9f,1e9f},mx[3]={-1e9f,-1e9f,-1e9f}, smin=1e9f,smax=-1e9f;
                for (size_t v=0; v<nv; ++v){ for(int c=0;c<3;c++){float p=md.positions[v*3+c]; if(p<mn[c])mn[c]=p; if(p>mx[c])mx[c]=p;} }
                for (int j=0;j<nj;j++){ if(skel.joints[j].scale<smin)smin=skel.joints[j].scale; if(skel.joints[j].scale>smax)smax=skel.joints[j].scale; }
                fprintf(stderr,"[ANIMDBG] mesh%zu nv=%zu pos x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f] jointScale[%.2f,%.2f]\n",
                        r.meshIdx, nv, mn[0],mx[0],mn[1],mx[1],mn[2],mx[2], smin,smax);
            }
        }
    }

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[LOADER] ");
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

    bool load(const std::string& apkPath) {
        log("========================================");
        log("HSR Scene Loader — libshell.so pipeline");
        log("APK: %s", apkPath.c_str());
        log("========================================");

        // ── Step 0: Read APK (zip) → assets/scene.zip using miniz
        mz_zip_archive apkZip;
        memset(&apkZip, 0, sizeof(apkZip));
        if (!mz_zip_reader_init_file(&apkZip, apkPath.c_str(), 0)) {
            log("FATAL: Cannot open APK zip: %s", apkPath.c_str());
            log("  mz_zip_get_error_string: %s", mz_zip_get_error_string(mz_zip_get_last_error(&apkZip)));
            return false;
        }

        // Find scene.zip inside APK
        int sceneIdx = mz_zip_reader_locate_file(&apkZip, "assets/scene.zip", nullptr, 0);
        if (sceneIdx < 0) {
            log("FATAL: assets/scene.zip not found in APK");
            log("  Listing APK contents for 'scene':");
            mz_zip_archive_file_stat stat;
            for (int i = 0; i < (int)mz_zip_reader_get_num_files(&apkZip); ++i) {
                if (mz_zip_reader_file_stat(&apkZip, i, &stat)) {
                    std::string name(stat.m_filename);
                    if (name.find("scene") != std::string::npos)
                        log("    [%d] %s (%llu bytes)", i, stat.m_filename, (unsigned long long)stat.m_uncomp_size);
                }
            }
            mz_zip_reader_end(&apkZip);
            return false;
        }

        size_t sceneZipSize = 0;
        void* sceneZipData = mz_zip_reader_extract_to_heap(&apkZip, sceneIdx, &sceneZipSize, 0);
        if (!sceneZipData) {
            log("FATAL: Failed to extract scene.zip from APK");
            mz_zip_reader_end(&apkZip);
            return false;
        }
        log("Extracted scene.zip: %zu bytes", sceneZipSize);
        mz_zip_reader_end(&apkZip);

        // ── Step 1-2: Parse scene.zip contents
        mz_zip_archive sceneZip;
        memset(&sceneZip, 0, sizeof(sceneZip));
        if (!mz_zip_reader_init_mem(&sceneZip, sceneZipData, sceneZipSize, 0)) {
            log("FATAL: Cannot open scene.zip in memory");
            log("  szip error: %s", mz_zip_get_error_string(mz_zip_get_last_error(&sceneZip)));
            free(sceneZipData);
            return false;
        }

        // List all files in scene.zip for debugging
        if (verbose) {
            log("Scene.zip contents (%u files):", mz_zip_reader_get_num_files(&sceneZip));
            mz_zip_archive_file_stat fstat;
            for (int i = 0; i < (int)mz_zip_reader_get_num_files(&sceneZip) && i < 50; ++i) {
                if (mz_zip_reader_file_stat(&sceneZip, i, &fstat))
                    log("  [%3d] %s", i, fstat.m_filename);
            }
            if (mz_zip_reader_get_num_files(&sceneZip) > 50)
                log("  ... (%u more files)", mz_zip_reader_get_num_files(&sceneZip) - 50);
        }

        // Detect whether this env actually has skeletal animation. Skinning must be
        // gated on the presence of an HZANSKEL skeleton — NOT on vertex stride. Many
        // static envs (haven2025) use wide vertex layouts (stride 48: pos+normal+
        // tangent+multiUV+color) that are not bone data. Reading "bones" from those
        // bytes marked every static mesh as skinned and collapsed them.
        bool envHasSkeleton = false;
        {
            int nf = (int)mz_zip_reader_get_num_files(&sceneZip);
            mz_zip_archive_file_stat st;
            for (int i = 0; i < nf; ++i) {
                if (!mz_zip_reader_file_stat(&sceneZip, i, &st)) continue;
                std::string n(st.m_filename);
                if (n.find("hzanim_skel") != std::string::npos || n.find("__skel") != std::string::npos) {
                    envHasSkeleton = true; break;
                }
            }
            log("envHasSkeleton=%d (skinning %s)", (int)envHasSkeleton,
                envHasSkeleton ? "ENABLED" : "disabled — all meshes static");
        }

        // Helper: read a file from scene.zip by its manifest path.
        // Manifest paths are like "meta/nux/nux_d/tx_dome_a_03.png/tex" (no "content/" prefix).
        // The zip entry is stored as "content/meta/..." so we try both.
        auto readAsset = [&](const std::string& path) -> std::vector<u8> {
            std::string full = "content/" + path;
            int idx = mz_zip_reader_locate_file(&sceneZip, full.c_str(), nullptr, 0);
            if (idx < 0)
                idx = mz_zip_reader_locate_file(&sceneZip, path.c_str(), nullptr, 0);
            if (idx < 0) {
                if (verbose) log("  MISSING: %s", full.c_str());
                return {};
            }
            size_t sz = 0;
            void* d = mz_zip_reader_extract_to_heap(&sceneZip, idx, &sz, 0);
            if (!d) {
                log("  EXTRACT FAILED: %s", full.c_str());
                return {};
            }
            std::vector<u8> out((u8*)d, (u8*)d + sz);
            mz_free(d);
            return out;
        };

        // ── Step 3: Parse content/assets.manifest → complete (ing,tgt)→path map
        log("Step 1: Parsing ASMH manifest...");
        auto asmhData = readAsset("assets.manifest");
        if (asmhData.empty()) {
            log("FATAL: ASMH manifest not found");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        log("  ASMH size: %zu bytes, magic: '%c%c%c%c'",
            asmhData.size(), asmhData[0], asmhData[1], asmhData[2], asmhData[3]);

        AssetMap assetMap;
        if (!parseAsmh(asmhData, assetMap)) {
            log("FATAL: ASMH parse returned no entries");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        log("  Registered %zu asset map entries (includes pkg=0 fallbacks)", assetMap.size());
        if (verbose) {
            for (auto& [key, path] : assetMap) {
                if (key.pkg != 0)  // only print real entries to reduce noise
                    log("    ing=%016llX tgt=%08X -> %s",
                        (unsigned long long)key.ing, key.tgt, path.c_str());
            }
        }

        // Helper: resolve any AssetKey → path string (tries real pkg then pkg=0).
        auto resolve = [&](const AssetKey& k) -> const std::string* {
            auto it = assetMap.find(k);
            if (it != assetMap.end()) return &it->second;
            AssetKey k0 = k; k0.pkg = 0;
            it = assetMap.find(k0);
            if (it != assetMap.end()) return &it->second;
            return nullptr;
        };

        // ── Step 4: shellconfig → firstWorldAssetId
        log("Step 2: Reading shellconfig...");
        auto cfgData = readAsset("configs/shellconfig.jsonc");
        if (cfgData.empty()) {
            log("FATAL: shellconfig.jsonc not found");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        std::string cfgJson((char*)cfgData.data(), cfgData.size());
        log("  shellconfig: %zu bytes", cfgJson.size());

        auto cfg = tinyjson::parse(cfgJson);
        if (!cfg.has("firstWorldAssetId")) {
            log("FATAL: shellconfig missing firstWorldAssetId");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        auto& fw = cfg["firstWorldAssetId"];
        AssetKey rootKey;
        try {
            rootKey.pkg = fw.has("packageOrRemoteId") ? std::stoull(fw["packageOrRemoteId"].asString()) : 0ull;
            rootKey.ing = fw.has("ingestionId")      ? std::stoull(fw["ingestionId"].asString()) : 0ull;
            rootKey.tgt = fw.has("targetId")         ? (u32)fw["targetId"].asInt() : 0u;
        } catch (...) {
            log("FATAL: Failed to parse firstWorldAssetId values");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        log("  firstWorldAssetId: pkg=%016llX ing=%016llX tgt=%08X",
            (unsigned long long)rootKey.pkg,
            (unsigned long long)rootKey.ing,
            rootKey.tgt);

        const std::string* spacePath = resolve(rootKey);
        if (!spacePath) {
            log("FATAL: firstWorldAssetId not in manifest: pkg=%016llX ing=%016llX tgt=%08X",
                (unsigned long long)rootKey.pkg,
                (unsigned long long)rootKey.ing,
                rootKey.tgt);
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }
        log("  space.hstf path: %s", spacePath->c_str());

        // ── Step 3: Recursively load HSTF hierarchy → entity list
        // Handles both flat scenes (v79_test: space.hstf IS the leaf) and
        // deeply nested templates (haven2025: space → arch/props → shell.usda → entities)
        log("Step 3: Loading HSTF hierarchy recursively from: %s", spacePath->c_str());
        std::vector<DrawableEntity> entities;
        {
            std::set<std::string> visitedHstf;
            std::function<void(const std::string&, int)> loadHstfRec;
            loadHstfRec = [&](const std::string& path, int depth) {
                if (depth <= 0 || visitedHstf.count(path)) return;
                visitedHstf.insert(path);
                log("  [d=%d] %s", depth, path.c_str());

                auto hdata = readAsset(path);
                if (hdata.empty()) return;
                std::string hjson((char*)hdata.data(), hdata.size());

                // Collect drawable entities in this HSTF (leaf behavior)
                std::vector<DrawableEntity> localEnts;
                parseHstf(hjson, localEnts, false);
                for (auto& e : localEnts) entities.push_back(e);

                // Follow entity type refs (intermediate/recursive behavior)
                try {
                    auto doc = tinyjson::parse(hjson);
                    if (!doc.has("entities") || !doc["entities"].isArray()) return;
                    const auto& arr = doc["entities"];
                    for (size_t i = 0; i < arr.size(); ++i) {
                        const auto& ent = arr[i];
                        if (!ent.has("type")) continue;
                        const auto& typeRef = ent["type"];
                        AssetKey typeKey;
                        try {
                            typeKey.pkg = typeRef.has("packageOrRemoteId") ? std::stoull(typeRef["packageOrRemoteId"].asString()) : 0ull;
                            typeKey.ing = typeRef.has("ingestionId")      ? std::stoull(typeRef["ingestionId"].asString()) : 0ull;
                            typeKey.tgt = typeRef.has("targetId")         ? (u32)typeRef["targetId"].asInt() : 0u;
                        } catch (...) { continue; }
                        if (typeKey.ing == 0) continue;
                        const std::string* typePath = resolve(typeKey);
                        if (!typePath) {
                            log("    type ref unresolved: ing=%016llX tgt=%08X",
                                (unsigned long long)typeKey.ing, typeKey.tgt);
                            continue;
                        }
                        loadHstfRec(*typePath, depth - 1);
                    }
                } catch (...) {}
            };
            loadHstfRec(*spacePath, 6);
        }
        log("  Total drawable entities from HSTF hierarchy: %zu", entities.size());
        if (entities.empty()) {
            log("FATAL: No drawable entities found in HSTF hierarchy");
            mz_zip_reader_end(&sceneZip);
            free(sceneZipData);
            return false;
        }

        // ── Helper: load + decode texture via AssetKey
        auto loadTexture = [&](const AssetKey& texKey, MeshData& md) -> bool {
            const std::string* tp = resolve(texKey);
            if (!tp) {
                log("  Texture key not in manifest: ing=%016llX tgt=%08X",
                    (unsigned long long)texKey.ing, texKey.tgt);
                return false;
            }
            auto texData = readAsset(*tp);
            if (texData.empty()) { log("  Texture data empty: %s", tp->c_str()); return false; }
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(texData, ti)) {
                log("  RENDTXTR header parse failed: %s", tp->c_str());
                return false;
            }
            u32 rawLen = ti.rawDataLen;
            if (!astc::decodeASTC(texData.data() + ti.rawDataOffset, rawLen,
                                  ti.width, ti.height, ti.blockW, ti.blockH, md.texRGBA)) {
                log("  ASTC decode failed: %s", tp->c_str());
                return false;
            }
            md.texW = ti.width; md.texH = ti.height; md.hasTexture = true;
            log("  Texture: %s (%ux%u fmt=%u)", tp->c_str(), ti.width, ti.height, ti.formatCode);
            return true;
        };

        // ── Helper: decode a texture by AssetKey into an explicit RGBA buffer + dims
        //    (used to load normal/ORM/lightmap maps into their MeshData slots).
        auto decodeTexInto = [&](const AssetKey& texKey, std::vector<u8>& out, u32& ow, u32& oh) -> bool {
            const std::string* tp = resolve(texKey);
            if (!tp) return false;
            auto texData = readAsset(*tp);
            if (texData.empty()) return false;
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(texData, ti)) return false;
            if (!astc::decodeASTC(texData.data() + ti.rawDataOffset, ti.rawDataLen,
                                  ti.width, ti.height, ti.blockW, ti.blockH, out)) return false;
            ow = ti.width; oh = ti.height;
            return true;
        };
        // Classify a texture path into a shader role by its filename suffix — this mirrors
        // how libshell binds each shader texture resource (BaseColorMetallic_Tx / ONxRNy_Tx
        // / lightmap) to the material's like-named texture parameter.
        //   0=base 1=normal(onxrny) 2=orm/ao/rough/metal 3=emissive 4=lightmap -1=unknown
        auto texRole = [](const std::string& p) -> int {
            std::string s = p; for (auto& c : s) c = (char)tolower((unsigned char)c);
            if (s.find("unsupported") != std::string::npos) return -1;
            if (s.find("lightmap") != std::string::npos) return 4;
            if (s.find("emissive") != std::string::npos) return 3;
            if (s.find("onxrny") != std::string::npos || s.find("normal") != std::string::npos ||
                s.find("_nrm") != std::string::npos) return 1;
            if (s.find("_orm") != std::string::npos || s.find("rbaodir") != std::string::npos ||
                s.find("_ao") != std::string::npos || s.find("rough") != std::string::npos) return 2;
            if (s.find("basecolor") != std::string::npos || s.find("albedo") != std::string::npos ||
                s.find("diffuse") != std::string::npos || s.find("_color") != std::string::npos) return 0;
            return 0;  // default: treat as base color
        };

        // ── Helper: stable fallback color from mesh name
        auto fallbackColor = [](const std::string& name) -> std::array<u8,3> {
            u32 h = 2166136261u;
            for (char c : name) { h ^= (u8)c; h *= 16777619u; }
            u32 sel = h % 6, mid = 100 + (h >> 8) % 100;
            u8 r,g,b;
            switch (sel) {
                case 0: r=255; g=(u8)mid; b=60;       break;
                case 1: r=60;  g=255;     b=(u8)mid;  break;
                case 2: r=(u8)mid; g=60;  b=255;      break;
                case 3: r=255; g=60;      b=(u8)mid;  break;
                case 4: r=(u8)mid; g=255; b=60;       break;
                default:r=60;  g=(u8)mid; b=255;      break;
            }
            return {r,g,b};
        };

        // ── Pre-load all MATLMATL entries from ASMH for universal fallback assignment.
        // Used for APKs where HSTF entities have no MaterialPlatformComponent.
        struct MatEntry {
            AssetKey matKey;
            AssetKey texKey;
            u64 shaderIng = 0;
            u32 shaderTgt = 0;
            bool useBlend = false;
            std::vector<u8> texRGBA;
            u32 texW = 1, texH = 1;
            bool texOk = false;
        };
        std::vector<MatEntry> fallbackMats;
        {
            for (auto& [key, path] : assetMap) {
                if (key.pkg == 0) continue;
                if (path.find(".material/") == std::string::npos) continue;
                auto matData = readAsset(path);
                if (matData.empty()) continue;
                MatlmatlInfo mi;
                if (!parseMatlmatl(matData, mi)) continue;
                MatEntry me;
                me.matKey = key;
                me.shaderIng = mi.shaderIng;
                me.shaderTgt = mi.shaderTgt;
                me.texKey = {mi.texPkg, mi.texIng, mi.texTgt};
                // useBlend: check shader path in assetMap for "blend" or "Blend"
                {
                    AssetKey sk = {0, mi.shaderIng, mi.shaderTgt};
                    const std::string* sp = resolve(sk);
                    if (!sp) { sk.pkg = mi.shaderPkg; sp = resolve(sk); }
                    if (sp && (sp->find("blend") != std::string::npos ||
                               sp->find("Blend") != std::string::npos))
                        me.useBlend = true;
                }
                // Pre-decode texture
                const std::string* tp = resolve(me.texKey);
                if (tp) {
                    auto texData = readAsset(*tp);
                    if (!texData.empty()) {
                        RendtxtrInfo ti;
                        if (parseRendtxtrHeader(texData, ti)) {
                            u32 rawLen = ti.rawDataLen;
                            if (astc::decodeASTC(texData.data() + ti.rawDataOffset, rawLen,
                                                 ti.width, ti.height, ti.blockW, ti.blockH, me.texRGBA)) {
                                me.texW = ti.width; me.texH = ti.height; me.texOk = true;
                                log("  [FallbackMat] %s -> %s (%ux%u)", path.c_str(), tp->c_str(), ti.width, ti.height);
                            }
                        }
                    }
                }
                fallbackMats.push_back(std::move(me));
            }
            log("  Pre-loaded %zu fallback materials from ASMH", fallbackMats.size());
        }

        // ── Pre-load unreferenced textures from ASMH (textures NOT used by any material).
        // These are skybox / environment textures not referenced via any MATLMATL.
        // Sort descending by area (largest first = most likely to be the skybox dome).
        struct UnrefTex {
            std::vector<u8> texRGBA;
            u32 texW = 1, texH = 1;
        };
        std::vector<UnrefTex> unrefTextures;
        {
            // Collect all texIng values already claimed by fallbackMats.
            std::set<u64> usedTexIngs;
            for (auto& me : fallbackMats)
                usedTexIngs.insert(me.texKey.ing);

            static constexpr u32 TEX_TGT_SENTINEL = 0x6E4CC522u;
            for (auto& [key, path] : assetMap) {
                if (key.pkg == 0) continue;                    // skip pkg=0 alias entries
                if (key.tgt != TEX_TGT_SENTINEL) continue;    // only texture entries
                if (usedTexIngs.count(key.ing)) continue;      // skip already-used textures

                auto texData = readAsset(path);
                if (texData.empty()) continue;
                RendtxtrInfo ti;
                if (!parseRendtxtrHeader(texData, ti)) continue;
                u32 rawLen = ti.rawDataLen;
                UnrefTex ut;
                if (!astc::decodeASTC(texData.data() + ti.rawDataOffset, rawLen,
                                      ti.width, ti.height, ti.blockW, ti.blockH, ut.texRGBA))
                    continue;
                ut.texW = ti.width; ut.texH = ti.height;
                log("  [UnrefTex] %s (%ux%u)", path.c_str(), ti.width, ti.height);
                unrefTextures.push_back(std::move(ut));
            }
            // Sort descending by area (largest = most likely skybox)
            std::sort(unrefTextures.begin(), unrefTextures.end(),
                [](const UnrefTex& a, const UnrefTex& b) {
                    return (a.texW * a.texH) > (b.texW * b.texH);
                });
            log("  Pre-loaded %zu unreferenced textures (sorted by area desc)",
                unrefTextures.size());
        }

        // ── Helper: apply material info to MeshData (useBlend + texture)
        auto applyMat = [&](MatlmatlInfo& matInfo, MeshData& md) -> bool {
            // useBlend: only true alpha-blend shaders. (Masked/vegetation cutouts need
            // alpha-TESTING, not blending — blending them without depth writes bleeds
            // foliage cards over the scene. Faithful cutout handling needs the masked
            // shader variants, a follow-up.)
            md.shaderIng = matInfo.shaderIng;   // record which RENDSHAD this material uses
            md.matParamsBlob = matInfo.constBlock;   // real per-material matParams (Tint/Layer*/Metallic…)
            AssetKey sk = {0, matInfo.shaderIng, matInfo.shaderTgt};
            const std::string* sp = resolve(sk);
            if (!sp) { sk.pkg = matInfo.shaderPkg; sp = resolve(sk); }
            if (sp) md.shaderPath = *sp;             // so the renderer can match it to the global shader
            if (sp && (sp->find("blend") != std::string::npos ||
                       sp->find("Blend") != std::string::npos))
                md.useBlend = true;

            // Load EVERY texture the material references into its role slot, exactly as
            // libshell binds each shader texture resource by name. A HAVEN prop material
            // carries base-color(+metallic), ONxRNy(normal+rough+AO), and sometimes ORM /
            // emissive / lightmap. The base-color is the one bound to BaseColorMetallic_Tx.
            u64 baseIng = 0;
            for (u64 ing : matInfo.texIngs) {
                AssetKey k = {0, ing, matInfo.texTgt};
                const std::string* tp = resolve(k);
                if (!tp) { k.pkg = matInfo.texPkg; tp = resolve(k); }
                if (!tp) continue;
                int role = texRole(*tp);
                AssetKey tk = {matInfo.texPkg, ing, matInfo.texTgt};
                switch (role) {
                    case 1: if (decodeTexInto(tk, md.normalRGBA, md.normalW, md.normalH)) {
                                md.hasNormal = true; log("    [normal]   %s", tp->c_str());
                                // ONxRNy is a COMBINED occlusion/normal/roughness map; the V203 PBR shaders
                                // (isotropicemissiveusd etc.) bind it at metalnessRoughnessOcclusionTex and have
                                // NO separate normal slot — so ALSO feed it to ORM (the shader reads O/R from it).
                                { std::string ln=*tp; for (char& c : ln) c=(char)tolower((unsigned char)c);
                                  if (ln.find("onxrny")!=std::string::npos && !md.hasOrm) {
                                      md.ormRGBA=md.normalRGBA; md.ormW=md.normalW; md.ormH=md.normalH; md.hasOrm=true; } }
                            } break;
                    case 2: if (decodeTexInto(tk, md.ormRGBA, md.ormW, md.ormH)) {
                                md.hasOrm = true; log("    [orm]      %s", tp->c_str()); } break;
                    case 4: if (decodeTexInto(tk, md.lmRGBA, md.lmW, md.lmH)) {
                                md.hasLightmap = true; log("    [lightmap] %s", tp->c_str()); } break;
                    case 3: if (decodeTexInto(tk, md.emissiveRGBA, md.emissiveW, md.emissiveH)) {
                                md.hasEmissive = true; log("    [emissive] %s", tp->c_str()); } break;
                    case 0: default: if (!baseIng) baseIng = ing; break;  // first base wins
                }
            }
            if (!baseIng && !matInfo.texIngs.empty()) baseIng = matInfo.texIngs.front();
            AssetKey texKey = {matInfo.texPkg, baseIng, matInfo.texTgt};
            bool ok = loadTexture(texKey, md);
            log("    [base]     ing=%016llX normal=%d orm=%d lm=%d",
                (unsigned long long)baseIng, (int)md.hasNormal, (int)md.hasOrm, (int)md.hasLightmap);
            return ok;
        };

        // Determine if ALL entities lack material refs (test envs like v79_test).
        // In that case cycle fallback materials across all entities so textures are visible.
        // If SOME entities have matRefs (prod envs), unmaterialed entities get neutral white.
        bool allEntitiesNoMat = std::all_of(entities.begin(), entities.end(),
            [](const DrawableEntity& e){ return e.matRefs.empty(); });
        log("  allEntitiesNoMat=%d — %s fallback cycling",
            (int)allEntitiesNoMat, allEntitiesNoMat ? "ENABLING" : "disabling");

        // ── Step 7: Load mesh + material + texture for each HSTF entity
        int loadedCount = 0;
        int failedCount = 0;
        std::set<std::string> claimedPaths;
        size_t fallbackMatIdx = 0;  // cycles through fallbackMats for no-matRef entities
        size_t unrefTexIdx = 0;     // cycles through unrefTextures for no-mat prod entities

        for (size_t ei = 0; ei < entities.size(); ++ei) {
            auto& obj = entities[ei];
            log("========================================");
            log("Entity[%zu] '%s'", ei, obj.name.c_str());

            // ── Resolve mesh
            const std::string* meshPath = resolve(obj.meshRef);
            if (!meshPath) {
                log("  SKIP: mesh not in manifest (ing=%016llX tgt=%08X)",
                    (unsigned long long)obj.meshRef.ing, obj.meshRef.tgt);
                failedCount++;
                continue;
            }
            log("  Mesh: %s", meshPath->c_str());
            claimedPaths.insert(*meshPath);

            // Skip non-visual meshes. Two classes, both authored alongside render
            // geometry but NOT drawn in the environment by libshell:
            //  (a) physics/navigation: __phys_mesh, _col_mesh/_COL, navmesh.
            //  (b) UI/interaction gizmos: wall/object PLACEMENT markers, hotspot dots,
            //      and 2D ICONS (ChairIcon etc.) — flat 1x1 quads with an icon atlas
            //      that showed up as a stray red/orange strip. These are contextual UI.
            int overlayKind = 0;
            {
                std::string low = *meshPath, nlow = obj.name;
                for (auto& c : low)  c = (char)tolower((unsigned char)c);
                for (auto& c : nlow) c = (char)tolower((unsigned char)c);
                auto npos = std::string::npos;
                bool isPhys = low.find("__phys_") != npos;
                bool isNav  = low.find("navmesh") != npos || nlow.find("navmesh") != npos;
                bool isCol  = low.find("_col_mesh") != npos || nlow.find("_col") != npos || nlow.find("collision") != npos;
                bool isUI   = nlow.find("placement") != npos || nlow.find("hotspot") != npos || nlow.find("icon") != npos;
                // Editor: load navmesh (1) + collision/walls (2) as editable translucent overlays
                // instead of dropping them. Still skip physics-duplicate (__phys_) meshes and UI quads.
                if (isNav && !isPhys) overlayKind = 1;
                else if (isCol && !isPhys) overlayKind = 2;
                if (isPhys || isUI) {
                    log("  SKIP: non-visual (phys-duplicate / UI gizmo)");
                    continue;
                }
            }

            auto meshData = readAsset(*meshPath);
            if (meshData.empty()) {
                log("  SKIP: mesh data empty");
                failedCount++;
                continue;
            }

            MeshData md;
            md.name      = obj.name;
            md.transform = obj.transform;
            md.overlayKind = overlayKind;

            if (const char* dm = std::getenv("HSR_DUMPMESH")) {
              if (md.name.find(dm)!=std::string::npos) {
                std::string fn = std::string("_")+md.name+".rendmesh";
                FILE* f=fopen(fn.c_str(),"wb");
                if(f){ fwrite(meshData.data(),1,meshData.size(),f); fclose(f);
                       log("  DUMPMESH wrote %zu bytes -> %s", meshData.size(), fn.c_str()); }
              }
            }
            if (!parseRendMesh(meshData, md.positions, md.uvs, md.indices,
                               &md.boneIndices, &md.boneWeights, md.name.c_str())) {
                log("  SKIP: RENDMESH parse failed");
                failedCount++;
                continue;
            }
            md.hasBones = envHasSkeleton && !md.boneIndices.empty();
            if (!md.hasBones) { md.boneIndices.clear(); md.boneWeights.clear(); }
            md.nVerts = (u32)(md.positions.size() / 3);
            md.nIdx   = (u32)md.indices.size();
            log("  RENDMESH: %u verts, %u idx (%u tris)", md.nVerts, md.nIdx, md.nIdx/3);

            if (md.nVerts > 0) {
                float xmin=1e30f, xmax=-1e30f, ymin=1e30f, ymax=-1e30f, zmin=1e30f, zmax=-1e30f;
                for (u32 vi = 0; vi < md.nVerts; ++vi) {
                    float x=md.positions[vi*3], y=md.positions[vi*3+1], z=md.positions[vi*3+2];
                    if(x<xmin)xmin=x; if(x>xmax)xmax=x;
                    if(y<ymin)ymin=y; if(y>ymax)ymax=y;
                    if(z<zmin)zmin=z; if(z>zmax)zmax=z;
                }
                log("  Bounds: X[%.2f..%.2f] Y[%.2f..%.2f] Z[%.2f..%.2f] | pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f,%.2f) scl=(%.2f,%.2f,%.2f) name=%s",
                    xmin,xmax, ymin,ymax, zmin,zmax,
                    md.transform.pos[0], md.transform.pos[1], md.transform.pos[2],
                    md.transform.rot[0], md.transform.rot[1], md.transform.rot[2], md.transform.rot[3],
                    md.transform.scale[0], md.transform.scale[1], md.transform.scale[2], md.name.c_str());
            }

            // ── Resolve material → texture
            bool gotTex = false;

            // Path 1: HSTF-provided matRefs (MaterialPlatformComponent — prod envs)
            for (auto& matRef : obj.matRefs) {
                const std::string* matPath = resolve(matRef);
                if (!matPath) {
                    log("  Material not in manifest: ing=%016llX tgt=%08X",
                        (unsigned long long)matRef.ing, matRef.tgt);
                    continue;
                }
                log("  Material: %s", matPath->c_str());
                auto matData = readAsset(*matPath);
                if (matData.empty()) { log("  Material data empty"); continue; }

                MatlmatlInfo matInfo;
                if (!parseMatlmatl(matData, matInfo)) { log("  MATLMATL parse failed"); continue; }
                log("  Shader: ing=%016llX  Tex: ing=%016llX",
                    (unsigned long long)matInfo.shaderIng, (unsigned long long)matInfo.texIng);
                if (std::getenv("HSR_DUMPMAT2") &&
                    (md.name.find("BowlA")!=std::string::npos || md.name.find("SculptureA")!=std::string::npos)) {
                    std::string fn = std::string("_mat_") + (md.name.find("BowlA")!=std::string::npos?"bowla":"sculpturea") + ".bin";
                    FILE* f=fopen(fn.c_str(),"wb"); if(f){ fwrite(matData.data(),1,matData.size(),f); fclose(f); }
                    log("  MATDUMP %s -> %s (%zu bytes) texIngs=%zu", md.name.c_str(), fn.c_str(), matData.size(), matInfo.texIngs.size());
                }

                if (applyMat(matInfo, md)) { gotTex = true; break; }
            }

            // Path 2: Fallback — only when ALL entities lack matRefs (v79_test-like APKs).
            // Cycle through pre-loaded materials so each unmaterialed entity gets a texture.
            // In prod APKs (where only a few entities lack matRefs), skip this — use neutral.
            if (!gotTex && allEntitiesNoMat && !fallbackMats.empty()) {
                size_t n = fallbackMats.size();
                for (size_t fi = 0; fi < n && !gotTex; ++fi) {
                    auto& me = fallbackMats[(fallbackMatIdx + fi) % n];
                    if (me.texOk) {
                        md.texRGBA = me.texRGBA;
                        md.texW = me.texW; md.texH = me.texH;
                        md.hasTexture = true;
                        md.useBlend = me.useBlend;
                        gotTex = true;
                        log("  Fallback mat[%zu] -> tex %ux%u blend=%d",
                            (fallbackMatIdx + fi) % n, me.texW, me.texH, (int)me.useBlend);
                    }
                }
                fallbackMatIdx = (fallbackMatIdx + 1) % n;
            }

            // Path 3: Unreferenced textures — for prod envs where an entity has no matRefs.
            // Skybox / environment textures not tied to any material (sorted largest first).
            // Use useBlend=false since skybox geometry is opaque.
            if (!gotTex && !allEntitiesNoMat && !unrefTextures.empty()) {
                size_t n = unrefTextures.size();
                auto& ut = unrefTextures[unrefTexIdx % n];
                md.texRGBA = ut.texRGBA;
                md.texW = ut.texW; md.texH = ut.texH;
                md.hasTexture = true;
                md.useBlend = false;
                gotTex = true;
                log("  UnrefTex[%zu] -> %ux%u (skybox/env opaque)",
                    unrefTexIdx % n, ut.texW, ut.texH);
                unrefTexIdx = (unrefTexIdx + 1) % n;
            }

            if (!gotTex) {
                u8 r, g, b;
                if (allEntitiesNoMat) {
                    // v79_test: no materials at all — use hash color to distinguish meshes
                    auto [hr,hg,hb] = fallbackColor(md.name);
                    r = hr; g = hg; b = hb;
                } else {
                    // prod env: entity has no material component — render as dark sky-blue
                    // (matches the clear color background so it's less distracting)
                    r = 15; g = 15; b = 25;
                }
                log("  No texture — fallback color R=%u G=%u B=%u", r,g,b);
                md.texRGBA = {r,g,b,255};
                md.texW = md.texH = 1;
                md.hasTexture = true;
            }

            meshes.push_back(std::move(md));
            loadedCount++;
        }

        // ── Step 8: Load any __mesh_sub_targets__ files not claimed by Step 7.
        // These are present in the zip but not referenced by any HSTF entity.
        // With a complete manifest all entities should be claimed; this loop
        // catches any remaining geometry (e.g. environment meshes with no entity).
        log("========================================");
        log("Step 6: Scanning for unclaimed mesh sub-targets...");
        {
            u32 nFiles = mz_zip_reader_get_num_files(&sceneZip);
            for (u32 fi = 0; fi < nFiles; ++fi) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&sceneZip, fi, &st)) continue;
                std::string fname(st.m_filename);
                if (fname.find("__mesh_sub_targets__/") == std::string::npos) continue;
                if (fname.find("__phys_mesh") != std::string::npos) continue;

                // Normalise: strip "content/" prefix for set lookup
                std::string normPath = fname;
                if (normPath.size() > 8 && normPath.substr(0,8) == "content/")
                    normPath = normPath.substr(8);
                if (claimedPaths.count(fname) || claimedPaths.count(normPath)) continue;

                log("  Unclaimed: %s", fname.c_str());
                size_t msz = 0;
                void* mdptr = mz_zip_reader_extract_to_heap(&sceneZip, fi, &msz, 0);
                if (!mdptr) { log("  Extract failed"); continue; }

                MeshData md;
                {
                    std::string seg = fname;
                    if (auto p = seg.rfind('/'); p != std::string::npos) seg = seg.substr(p+1);
                    md.name = seg;
                }
                md.transform.scale[0] = md.transform.scale[1] = md.transform.scale[2] = 1.0f;
                md.transform.rot[3] = 1.0f;

                std::vector<u8> mdata((u8*)mdptr, (u8*)mdptr + msz);
                mz_free(mdptr);

                if (!parseRendMesh(mdata, md.positions, md.uvs, md.indices, &md.boneIndices, &md.boneWeights)) {
                    log("  RENDMESH parse failed — skip");
                    failedCount++;
                    continue;
                }
                md.hasBones = envHasSkeleton && !md.boneIndices.empty();
                if (!md.hasBones) { md.boneIndices.clear(); md.boneWeights.clear(); }
                md.nVerts = (u32)(md.positions.size() / 3);
                md.nIdx   = (u32)md.indices.size();
                log("  Loaded: %u verts, %u idx", md.nVerts, md.nIdx);

                // Assign fallback color — no material reference available for unclaimed meshes.
                auto [r,g,b] = fallbackColor(md.name);
                md.texRGBA = {r,g,b,255};
                md.texW = md.texH = 1;
                md.hasTexture = true;

                meshes.push_back(std::move(md));
                loadedCount++;
            }
        }

        log("========================================");
        log("LOAD COMPLETE: %d meshes loaded, %d failed", loadedCount, failedCount);
        log("========================================");

        // ── v203 HzAnim skeletal animation: load skeleton + clip, build CPU-skin records ──
        {
            int nf = (int)mz_zip_reader_get_num_files(&sceneZip);
            auto extractBy=[&](const char* marker, std::vector<u8>& out)->bool{
                for (int fi=0; fi<nf; ++fi) { mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(&sceneZip, fi, &st)) continue;
                    if (strstr(st.m_filename, marker)) { size_t sz=0; void* p=mz_zip_reader_extract_to_heap(&sceneZip, fi, &sz, 0);
                        if (p){ out.assign((u8*)p,(u8*)p+sz); mz_free(p); return true; } } }
                return false; };
            std::vector<u8> sb, ab;
            if (extractBy("__hzanim_skel_sub_targets__", sb) && parseRendSkel(sb, skel)) {
                log("  HzAnim skeleton: %zu joints (first '%s')", skel.joints.size(), skel.joints.empty()?"":skel.joints[0].name.c_str());
                if (extractBy("__hzanim_anim_sub_targets__", ab) && parseRendClip(ab, (int)skel.joints.size(), clip))
                    log("  HzAnim clip: %d frames x %d joints @ %.0f fps", clip.nFrames, clip.nJoints, clip.fps);
                else log("  HzAnim clip: parse FAILED (clip format unresolved)");
                if (skel.ok() && clip.ok()) for (size_t mi=0; mi<meshes.size(); ++mi) {
                    auto& md = meshes[mi];
                    if (!md.hasBones || md.boneIndices.size() < md.positions.size()/3*4) continue;
                    SkinRec r; r.meshIdx=mi; r.basePos=md.positions; r.bIdx=md.boneIndices; r.bWgt=md.boneWeights;
                    skinRecs.push_back(std::move(r)); md.dynamicVerts = true;
                }
                log("  HzAnim: %zu skinned meshes -> CPU skin", skinRecs.size());
            }

            // ── Player spawn points: parse home_spawn_points.hstf JSON -> facing-cone markers ──
            std::vector<u8> spj;
            if (extractBy("home_spawn_points", spj)) {
                std::string j((const char*)spj.data(), spj.size());
                auto fnum=[&](size_t from, const char* key)->float{
                    size_t k=j.find(key, from); if(k==std::string::npos) return 0.f;
                    k=j.find(':', k); if(k==std::string::npos) return 0.f;
                    return (float)atof(j.c_str()+k+1); };
                size_t p=0;
                while ((p=j.find("\"name\":\"Spawn Point", p)) != std::string::npos) {
                    SpawnPt sp; size_t nq=p+8, nqe=j.find('"', nq); sp.name=j.substr(nq, nqe-nq);
                    if (sp.name == "Spawn Points") { p = nqe; continue; }  // skip the parent group node
                    size_t tg=j.find("\"tags\":[\"", p); if(tg!=std::string::npos){ tg+=9; sp.tag=j.substr(tg, j.find('"',tg)-tg); }
                    size_t lp=j.find("\"localPosition\"", p);
                    sp.pos[0]=fnum(lp,"\"x\""); sp.pos[1]=fnum(lp,"\"y\""); sp.pos[2]=fnum(lp,"\"z\"");
                    size_t lr=j.find("\"localRotation\"", p);
                    sp.euler[0]=fnum(lr,"\"x\""); sp.euler[1]=fnum(lr,"\"y\""); sp.euler[2]=fnum(lr,"\"z\"");
                    spawnPts.push_back(sp); p = (lr==std::string::npos)? nqe : lr+12;
                }
                for (auto& sp : spawnPts) {
                    MeshData md; md.name=sp.name; md.overlayKind=4; md.useBlend=true; md.doubleSided=true;
                    md.texRGBA={255,255,255,255}; md.texW=1; md.texH=1;   // 1x1 white; tint gives the colour
                    auto add=[&](float lx,float ly,float lz){
                        md.positions.push_back(sp.pos[0]+lx); md.positions.push_back(sp.pos[1]+0.12f+ly); md.positions.push_back(sp.pos[2]+lz);
                        md.uvs.push_back(0.5f); md.uvs.push_back(0.5f); };
                    auto tri=[&](u32 a,u32 b,u32 c){ md.indices.push_back(a); md.indices.push_back(b); md.indices.push_back(c); };
                    float r=0.16f, h=0.55f; const int nb=6;
                    add(0,0,h);                                                       // 0 = apex (facing +Z)
                    for(int s=0;s<nb;s++){ float a=s*6.28319f/nb; add(r*cosf(a), r*sinf(a), 0.f); } // 1..nb base ring
                    for(int s=0;s<nb;s++) tri(0, 1+s, 1+(s+1)%nb);                    // cone sides
                    for(int s=1;s<nb-1;s++) tri(1, 1+s, 2+s);                         // base cap
                    md.nVerts=(u32)(md.positions.size()/3); md.nIdx=(u32)md.indices.size();
                    sp.meshIdx=(int)meshes.size(); meshes.push_back(std::move(md));
                }
                log("  Spawn points: %zu facing-cone markers", spawnPts.size());
            }
        }

        mz_zip_reader_end(&sceneZip);
        free(sceneZipData);
        return loadedCount > 0;
    }
};
