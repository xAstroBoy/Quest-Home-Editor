#pragma once
#include "core/types.h"
#include "loaders/asmh_parser.h"
#include "loaders/rendmesh_parser.h"
#include "loaders/rendtxtr_parser.h"
#include "loaders/matlmatl_parser.h"
#include "loaders/hstf_parser.h"
#include "loaders/rendshad_parser.h"   // parseRendShadForward -> faithful transparent-pass (f4-omitted) detection

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

#include "loaders/rendskel_parser.h"
class SceneLoader {
public:
    std::vector<MeshData> meshes;
    // V203 IBL: captured EQUIRECT reflection panorama (RGBA8) from the skybox reflectionMap / assets/ibl/ —
    // gated by HSR_SKYIBL. main.cpp feeds it to vkRenderer.setIblEquirectRGBA8() for the SpecIbl per-vertex bake.
    std::vector<u8> iblEquirectRGBA8; u32 iblEqW = 0, iblEqH = 0;
    bool verbose = true;
    // ── Baked-lightmap binding (the warm baked room lighting), DEVICE-FAITHFUL via the override chain:
    //   lmGuidName : meshGUID(13) -> mesh name   (cooked USD templates  *.usda/template_9k0v)
    //   lmGuidRaw  : meshGUID(13) -> raw lightmap (calming_lightmap_overrides/*_lm.hstf deltas:
    //                sourceId -> MeshPlatformComponent.lightingParams.lightMapTexture asset, readAsset'd)
    // Combine via GUID -> {mesh name -> baked lightmap} (incl shared merge-group atlases). Both maps cached
    // at load (scene.zip is freed after). Cross-loader: vista ships overrides+lightmaps, home ships USD
    // names+meshes -> pass the companion. lmRawByKey = stem name-match FALLBACK (envs without override HSTFs).
    std::map<std::string, std::vector<u8>> lmRawByKey;
    std::map<std::string, std::string>     lmGuidName;
    std::map<std::string, std::vector<u8>> lmGuidRaw;
    float sceneNearClip = 0.f, sceneFarClip = 0.f;   // device finite near/far clip planes (space.hstf ScenePlatformComponent)

    void applyLightmapOverrides(std::vector<MeshData>& targets, SceneLoader* other = nullptr) {
        auto lower = [](std::string s){ for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
        auto bind = [&](MeshData& md, const std::vector<u8>& raw) -> bool {
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(raw, ti)) return false;
            if (!astc::decodeASTC(raw.data()+ti.rawDataOffset, ti.rawDataLen,
                                  ti.width, ti.height, ti.blockW, ti.blockH, md.lmRGBA, true)) return false;
            md.lmW=ti.width; md.lmH=ti.height; md.hasLightmap=true; md.bakeLightmapVtx=true; return true;
        };
        // FAITHFUL: meshGUID -> name (this+other) crossed with meshGUID -> raw lightmap (this+other).
        std::map<std::string, const std::vector<u8>*> nameRaw;
        auto nameOf = [&](const std::string& g) -> const std::string* {
            auto it = lmGuidName.find(g); if (it != lmGuidName.end()) return &it->second;
            if (other) { auto j = other->lmGuidName.find(g); if (j != other->lmGuidName.end()) return &j->second; }
            return nullptr; };
        auto addRaw = [&](std::map<std::string,std::vector<u8>>& m){ for (auto& kv : m){ const std::string* nm=nameOf(kv.first); if (nm && !kv.second.empty()) nameRaw[lower(*nm)] = &kv.second; } };
        addRaw(lmGuidRaw); if (other) addRaw(other->lmGuidRaw);
        int bound = 0;
        for (auto& md : targets) {
            if (md.hasLightmap || md.uvs2.empty()) continue;
            auto it = nameRaw.find(lower(md.name));
            if (it != nameRaw.end() && bind(md, *it->second)) ++bound;
        }
        if (bound && verbose) log("  [LM-faithful] bound %d baked lightmaps via override GUID chain", bound);
        // FALLBACK: stem name-match (this loader's lmRawByKey) for envs lacking override HSTFs.
        int fb = 0;
        if (!lmRawByKey.empty()) for (auto& md : targets) {
            if (md.hasLightmap || md.uvs2.empty()) continue;
            std::string mk = lower(md.name);
            if (mk.rfind("root_xform_",0)==0) mk = mk.substr(11);
            { size_t l=mk.find("_lod"); if (l!=std::string::npos) mk=mk.substr(0,l); }
            for (bool ch=true; ch;){ ch=false; for (std::string pre : {std::string("sm_"),std::string("lbg_"),std::string("merged_"),std::string("inst_")}) if (mk.rfind(pre,0)==0){ mk=mk.substr(pre.size()); ch=true; } }
            const std::vector<u8>* raw=nullptr; auto it=lmRawByKey.find(mk); if (it!=lmRawByKey.end()) raw=&it->second;
            if (!raw) for (auto& kv : lmRawByKey){ const std::string& k=kv.first; if ((k.size()>3&&mk.find(k)!=std::string::npos)||(mk.size()>3&&k.find(mk)!=std::string::npos)){ raw=&kv.second; break; } }
            if (raw && bind(md, *raw)) ++fb;
        }
        if (fb && verbose) log("  [LM-namematch] bound %d via stem fallback", fb);
        // ORM MERGE-GROUP fallback (faithful): a mesh whose own name misses its lightmap shares a baked
        // merge-group with its ORM/rbaodir texture — the rbaodir map and the lmhdr lightmap for a baked
        // mesh-group carry the same group stem. The haven ceiling is the case: mesh "Root_Xform_SM_LBG_cieling"
        // (misspelled) + ORM "t_merged_ceilingtrim_rbaodir" + lightmap "t_sm_merged_ceilingtrim_..._lmhdr" are
        // ONE group, so match the lightmap by the ORM's group stem when the mesh name fails.
        int fg = 0;
        if (!lmRawByKey.empty()) for (auto& md : targets) {
            if (md.hasLightmap || md.uvs2.empty() || md.ormTexName.empty()) continue;
            std::string base = lower(md.ormTexName);
            size_t ext = base.find(".png"); if (ext==std::string::npos) ext = base.find(".hdr");
            if (ext==std::string::npos) continue;
            size_t st = base.rfind('/', ext); st = (st==std::string::npos)?0:st+1;
            base = base.substr(st, ext-st);                                  // t_merged_ceilingtrim_rbaodir
            for (std::string pre : {std::string("t_sm_"),std::string("t_")}) if (base.rfind(pre,0)==0){ base=base.substr(pre.size()); break; }
            for (std::string suf : {std::string("_rbaodir"),std::string("_onxrny"),std::string("_orm"),std::string("_nx"),std::string("_normal"),std::string("_basecolor")}) { size_t f=base.rfind(suf); if (f!=std::string::npos){ base=base.substr(0,f); break; } }
            for (bool ch=true; ch;){ ch=false; for (std::string pre : {std::string("sm_"),std::string("lbg_"),std::string("merged_")}) if (base.rfind(pre,0)==0){ base=base.substr(pre.size()); ch=true; } }  // -> ceilingtrim
            if (base.size()<4) continue;
            const std::vector<u8>* raw=nullptr; auto it=lmRawByKey.find(base); if (it!=lmRawByKey.end()) raw=&it->second;
            if (!raw) for (auto& kv : lmRawByKey){ const std::string& k=kv.first; if ((k.size()>3&&base.find(k)!=std::string::npos)||(base.size()>3&&k.find(base)!=std::string::npos)){ raw=&kv.second; break; } }
            if (raw && bind(md, *raw)) { ++fg; if (verbose) log("  [LM-ormgroup] '%s' lightmap via ORM merge-group '%s'", md.name.c_str(), base.c_str()); }
        }
        if (fg && verbose) log("  [LM-ormgroup] bound %d via ORM merge-group", fg);

        // MERGE-GROUP SIBLING fallback (faithful): per AREA (moat/front/rear/planter/window/garden/...), the
        // ground{X} + boulder{X} shell meshes share ONE baked lightmap ATLAS named after the boulder
        // (t_sm_merged_calming_boulder{X}_shell_lmhdr). The cooked override deltas bind the boulder{X} mesh but
        // MISS the ground{X} mesh -> its pbrlightmap material gets a white lightmap = the "groundMoat too white /
        // should be greenish" wash. Both meshes sample the SAME atlas (each keeps its OWN uv2 region), so copy the
        // bound same-area sibling's decoded lightmap texture to the unmapped one. HSR_NOLMSIB disables.
        if (!std::getenv("HSR_NOLMSIB")) {
            auto areaOf = [](const std::string& name) -> std::string {
                std::string n = name; for (char& c : n) c = (char)tolower((unsigned char)c);
                size_t p = n.find("ground"); size_t off = 6;
                if (p == std::string::npos) { p = n.find("boulder"); off = 7; }
                if (p == std::string::npos) return "";
                std::string a = n.substr(p + off); size_t e = 0;
                while (e < a.size() && a[e] >= 'a' && a[e] <= 'z') ++e;   // moat/front/rear/planter/window/garden/pot
                return a.substr(0, e);
            };
            int sib = 0;
            if (!lmRawByKey.empty()) for (auto& md : targets) {
                if (md.hasLightmap || md.uvs2.empty()) continue;
                std::string area = areaOf(md.name); if (area.size() < 3) continue;
                // the merged atlas is named after the boulder of this area (…calming_boulder{area}…_lmhdr).
                const std::vector<u8>* raw = nullptr; std::string hit;
                for (auto& kv : lmRawByKey) if (kv.first.find("boulder"+area) != std::string::npos || kv.first.find("ground"+area) != std::string::npos) { raw = &kv.second; hit = kv.first; break; }
                if (raw && bind(md, *raw)) { ++sib; if (verbose) log("  [LM-mergegroup] '%s' lightmap '%s' (area '%s')", md.name.c_str(), hit.c_str(), area.c_str()); }
            }
            if (sib && verbose) log("  [LM-mergegroup] bound %d via ground/boulder area sibling", sib);
        }
        if (std::getenv("HSR_LMDBG")) for (auto& md : targets) { if (md.hasLightmap||md.overlayKind) continue;
            log("  [LM-MISS] '%s' uv2=%zu", md.name.c_str(), md.uvs2.size()); }
    }
    // Skipped / undecoded elements, recorded during load and printed as a summary at the end (always, even
    // with verbose off) — so missing textures / unparsed meshes / unresolved assets are never silent.
    std::vector<std::string> skips;
    void recordSkip(const std::string& s) { skips.push_back(s); }

    // ── v203 HzAnim skeletal animation (CPU skin into dynamicVerts, like OPA/glTF) ──
    struct SkinRec { size_t meshIdx; std::vector<float> basePos; std::vector<u8> bIdx, bWgt; };
    // Per-asset animation group: each animated fbx (bat/owl/windmill/lantern) owns its OWN skeleton+clip+
    // skinned meshes. The device AnimationSystem is PER-ENTITY; the old single skel/clip drove only ONE rig
    // (whale/incredibles), so multi-rig envs like horror had nothing/everything-wrong animate.
    struct AnimGroup { RendSkel skel; RendClip clip; std::vector<SkinRec> skinRecs; };
    std::vector<AnimGroup> animGroups;

    // Player spawn points (home_spawn_points.hstf JSON) -> editable cone markers (overlayKind 4).
    // tag "local" = the player start (cyan), "remote" = other spawns (yellow).
    struct SpawnPt { std::string name, tag; float pos[3]={0,0,0}, euler[3]={0,0,0}; int meshIdx=-1; };
    std::vector<SpawnPt> spawnPts;
    bool hasAnimation() const {
        for (auto& g : animGroups) if (g.skel.ok() && g.clip.ok() && !g.skinRecs.empty()) return true;
        return false;
    }
    void animate(float t) {
        for (auto& g : animGroups) {
            if (!(g.skel.ok() && g.clip.ok() && !g.skinRecs.empty())) continue;
            std::vector<float> skin; sampleRendClip(g.skel, g.clip, t, skin);   // device: quat->mat3*scale+T per joint
            int nj = (int)skin.size()/16;
            for (auto& r : g.skinRecs) {
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
                    else { float inv=1.0f/wsum; ox*=inv; oy*=inv; oz*=inv; }   // normalize (weights may not sum to 1)
                    md.positions[v*3]=ox; md.positions[v*3+1]=oy; md.positions[v*3+2]=oz;
                }
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
                if (n.find("hzanim_skel") != std::string::npos || n.find("__skel") != std::string::npos
                    || n.find(".skel/skeleton") != std::string::npos) {   // OUR cooker's naming
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
            // stackHstf = recursion-STACK (cycle guard only). The old code used a permanent visited-set,
            // which silently DROPPED any template reached via a 2nd parent/transform — i.e. legitimately
            // re-instanced templates (the repeated "pockets", the cave sand/ground clones) → seafloor/cave
            // GAPS. A stack guard still breaks real cycles (A→…→A) but lets a template instance many times.
            std::set<std::string> stackHstf;
            std::set<std::string> everHstf;   // diagnostic: which templates were being re-instanced (previously deduped)
            std::function<void(const std::string&, int, const Transform&, const float*)> loadHstfRec;
            loadHstfRec = [&](const std::string& path, int depth, const Transform& parent, const float* parentMat) {
                if (depth <= 0 || stackHstf.count(path)) return;        // skip only if already on the CURRENT path (a cycle)
                if (everHstf.count(path)) log("  RE-INSTANCE template (was previously deduped → gap): %s", path.c_str());
                stackHstf.insert(path); everHstf.insert(path);
                log("  [d=%d] %s", depth, path.c_str());

                auto hdata = readAsset(path);
                if (hdata.empty()) { stackHstf.erase(path); return; }
                std::string hjson((char*)hdata.data(), hdata.size());

                // Collect drawable entities in this HSTF (leaf behavior)
                std::vector<DrawableEntity> localEnts;
                parseHstf(hjson, localEnts, false);

                // FAITHFUL empty-transform resolution. The cooker sometimes emits a mesh entity whose
                // TransformPlatformComponent has NO localPosition (oceanarium Root_sand_SHELL): a flat read
                // drops it to the origin, leaving a seafloor GAP — but in-headset it sits with the rest of
                // the level. The cave content (rocks/sand/trim) all share one localPosition (10,0,-5) — a
                // group offset the cook baked per-mesh — while a few distant rocks sit elsewhere, so it is a
                // DOMINANT (majority) offset, not unanimous. When the same localPosition is shared by a strict
                // majority (>50%) of a level's positioned mesh siblings, an empty-transform mesh belongs at
                // that group offset. Levels whose meshes have VARIED positions (fgpockets — oceanGround
                // correctly stays at origin; every coral is unique → no majority) leave empty meshes alone.
                // The level ROOT has no mesh (meshRef.ing==0) so it never participates. Reproduces VR for both
                // levels; generalizes to any empty-transform mesh ("fix one fixes the rest").
                {
                    int nPos = 0, bestCnt = 0; float best[3] = {0,0,0};
                    for (size_t i = 0; i < localEnts.size(); ++i) {
                        if (localEnts[i].emptyTransform || localEnts[i].meshRef.ing == 0) continue;
                        ++nPos; const float* pi = localEnts[i].transform.pos; int cnt = 0;
                        for (const auto& e : localEnts) {
                            if (e.emptyTransform || e.meshRef.ing == 0) continue;
                            const float* pj = e.transform.pos;
                            if (std::fabs(pj[0]-pi[0])<1e-3f && std::fabs(pj[1]-pi[1])<1e-3f && std::fabs(pj[2]-pi[2])<1e-3f) ++cnt;
                        }
                        if (cnt > bestCnt) { bestCnt = cnt; best[0]=pi[0]; best[1]=pi[1]; best[2]=pi[2]; }
                    }
                    bool nonZero = std::fabs(best[0])>1e-3f || std::fabs(best[1])>1e-3f || std::fabs(best[2])>1e-3f;
                    if (bestCnt >= 2 && bestCnt * 2 > nPos && nonZero) {     // a GROUP (≥2) forming a strict majority share one offset
                        int fixed = 0;
                        for (auto& e : localEnts)
                            if (e.emptyTransform && e.meshRef.ing != 0) { e.transform.pos[0]=best[0]; e.transform.pos[1]=best[1]; e.transform.pos[2]=best[2]; ++fixed; }
                        if (fixed) log("  empty-transform mesh(es) inherited level group offset (%.2f,%.2f,%.2f) [%d/%d siblings]: %d fixed", best[0],best[1],best[2], bestCnt,nPos, fixed);
                    }
                }
                // Compose this level's accumulated world transform onto each local drawable (so template
                // instances land at their world placement, not template-local — fixes far-flung sunrays2,
                // seafloor gaps, etc.). Top level passes identity, so direct entities are unchanged.
                for (auto& e : localEnts) {
                    float lm[16]; trsToMat4(e.transform, lm);                 // this node's LOCAL T·R·S
                    mat4mul4(parentMat, lm, e.worldMatrix); e.hasWorldMatrix = true;  // faithful 4×4 world (keeps shear)
                    e.transform = composeTransform(parent, e.transform);      // composed TRS too (bounds/probe/editor)
                    entities.push_back(e);
                }

                // Follow entity type refs (intermediate/recursive behavior)
                try {
                    auto doc = tinyjson::parse(hjson);
                    if (!doc.has("entities") || !doc["entities"].isArray()) { stackHstf.erase(path); return; }
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
                        // Compose the INSTANCE entity's placement onto the parent before descending, so the
                        // referenced template's geometry inherits where this instance sits in the world.
                        Transform inst = parseHstfEntityTransform(ent);
                        float im[16], cm[16]; trsToMat4(inst, im); mat4mul4(parentMat, im, cm); // parent·instance (4×4)
                        loadHstfRec(*typePath, depth - 1, composeTransform(parent, inst), cm);
                    }
                } catch (...) {}
                stackHstf.erase(path);   // pop: allow re-entry of this template via a different parent/transform
            };
            float identMat[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            loadHstfRec(*spacePath, 6, Transform{}, identMat);
        }
        // DEDUP empty-transform template duplicates. A mesh defined in a level .usda with NO transform lands
        // at the ORIGIN (it's the PROTOTYPE); a sibling entity of the SAME NAME placed by an .hstf carries the
        // real transform. The device renders only the placed instance — so drop the origin-stuck empty
        // duplicate (the "plant in the center that shouldn't be there": Root_..._plantC = empty in the .usda
        // → origin, but calming_fgpocket.hstf places it at (-13.5,..)). Empty meshes with NO real-placed
        // namesake are untouched (they keep the group-offset inheritance path — the oceanarium sand).
        {
            std::set<std::string> realNamed;
            for (auto& e : entities) if (!e.emptyTransform && e.meshRef.ing != 0) realNamed.insert(e.name);
            size_t before = entities.size();
            entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const DrawableEntity& e){
                return e.emptyTransform && e.meshRef.ing != 0 && realNamed.count(e.name) != 0; }), entities.end());
            if (entities.size() != before) log("  dropped %zu empty-transform template duplicate(s) at origin (real-placed namesake exists)", before - entities.size());
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
                char b[64]; snprintf(b,sizeof b,"ing=%016llX tgt=%08X",(unsigned long long)texKey.ing,texKey.tgt);
                if (texKey.ing != 0)   // ing=0 = material declares no base texture (pos-only/emissive mesh) — not a failure
                    recordSkip(std::string("texture key not in manifest: ")+b);
                log("  Texture key not in manifest: ing=%016llX tgt=%08X",
                    (unsigned long long)texKey.ing, texKey.tgt);
                return false;
            }
            auto texData = readAsset(*tp);
            if (texData.empty()) { recordSkip("texture data empty: "+*tp); log("  Texture data empty: %s", tp->c_str()); return false; }
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(texData, ti)) {
                recordSkip("RENDTXTR header parse failed: "+*tp);
                log("  RENDTXTR header parse failed: %s", tp->c_str());
                return false;
            }
            u32 rawLen = ti.rawDataLen;
            if (!astc::decodeASTC(texData.data() + ti.rawDataOffset, rawLen,
                                  ti.width, ti.height, ti.blockW, ti.blockH, md.texRGBA)) {
                recordSkip("ASTC decode failed: "+*tp);
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
            if (!tp) return false;   // role-specific map simply not present — not an error
            auto texData = readAsset(*tp);
            if (texData.empty()) { recordSkip("aux-texture data empty: "+*tp); return false; }
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(texData, ti)) { recordSkip("aux-texture RENDTXTR parse fail: "+*tp); return false; }
            // .hdr/.exr baked lightmaps are ASTC-HDR — decode with the HDR profile (LDR decode = magenta).
            bool isHdr = tp->find(".hdr") != std::string::npos || tp->find(".exr") != std::string::npos;
            if (!astc::decodeASTC(texData.data() + ti.rawDataOffset, ti.rawDataLen,
                                  ti.width, ti.height, ti.blockW, ti.blockH, out, isHdr)) { recordSkip("aux-texture ASTC decode fail: "+*tp); return false; }
            ow = ti.width; oh = ti.height;
            return true;
        };
        // ── Helper: VAT offset texture (t_*_vatdata.exr -> tex). It's NOT ASTC — it's an
        //    R16G16B16A16_SFLOAT (half-float) image (libshell TextureFormat 97; sub_DE8EEC), width=vertexCount,
        //    height=frameCount, + a mip chain. Grab mip0 (w*h*8 raw half-float bytes) for verbatim upload.
        auto decodeVatFloat = [&](const AssetKey& texKey, MeshData& md) -> bool {
            const std::string* tp = resolve(texKey);
            if (!tp) return false;
            auto texData = readAsset(*tp);
            if (texData.empty()) return false;
            RendtxtrInfo ti;
            if (!parseRendtxtrHeader(texData, ti)) return false;
            size_t mip0 = (size_t)ti.width * ti.height * 8;   // 4 channels * 2 bytes (half)
            if (ti.width == 0 || ti.height == 0 || ti.rawDataOffset + mip0 > texData.size()) return false;
            md.vatRaw.assign(texData.begin() + ti.rawDataOffset, texData.begin() + ti.rawDataOffset + mip0);
            md.vatW = ti.width; md.vatH = ti.height; md.hasVat = true;
            return true;
        };
        // Classify a texture path into a shader role by its filename suffix — this mirrors
        // how libshell binds each shader texture resource (BaseColorMetallic_Tx / ONxRNy_Tx
        // / lightmap) to the material's like-named texture parameter.
        //   0=base 1=normal(onxrny) 2=orm/ao/rough/metal 3=emissive 4=lightmap -1=unknown
        auto texRole = [](const std::string& p) -> int {
            std::string s = p; for (auto& c : s) c = (char)tolower((unsigned char)c);
            if (s.find("unsupported") != std::string::npos) return -1;
            // Lightmap: the full word OR the "_lm" suffix (t_sand_lm.png). Without the suffix match the
            // sand's lightmap (t_sand_lm) was mis-classified as BASE color -> the sand floor rendered its
            // lightmap as the texture (broken). (loungeSand worked: its lightmap is named "..._lightmap".)
            if (s.find("lightmap") != std::string::npos ||
                s.find("_lm.") != std::string::npos || s.find("_lm/") != std::string::npos) return 4;
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
                // V203 IBL capture: the skybox reflectionMap / assets/ibl/ panorama (equirect ASTC-HDR). Prefer a
                // "refl"-named map; gated by HSR_SKYIBL so default rendering is untouched.
                if (std::getenv("HSR_SKYIBL")) {
                    std::string lp = path; for (char& c : lp) c = (char)tolower((unsigned char)c);
                    bool isRefl = lp.find("/ibl/")!=std::string::npos || lp.find("refl")!=std::string::npos ||
                                  lp.find("radiance")!=std::string::npos || lp.find("skydome")!=std::string::npos ||
                                  lp.find("panorama")!=std::string::npos;
                    if (isRefl && (iblEquirectRGBA8.empty() || lp.find("refl")!=std::string::npos)) {
                        iblEquirectRGBA8 = ut.texRGBA; iblEqW = ti.width; iblEqH = ti.height;
                        log("  [SKYIBL] captured IBL equirect: %s (%ux%u)", path.c_str(), ti.width, ti.height);
                    }
                }
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
            md.constParams = matInfo.constParams;    // {nameHash,off,size} -> bind by name (MurmurHash3)
            md.texSlots = matInfo.texSlots;          // sampler-slot name-hashes -> pick matching shader variant
            AssetKey sk = {0, matInfo.shaderIng, matInfo.shaderTgt};
            const std::string* sp = resolve(sk);
            if (!sp) { sk.pkg = matInfo.shaderPkg; sp = resolve(sk); }
            if (sp) md.shaderPath = *sp;             // so the renderer can match it to the global shader
            // FAITHFUL blend (data-proven, NOT name-based): the surface's RENDSHAD forward pass has field 4
            // OMITTED iff its pass is alpha-blended — true for animatedfog + mattepaintingalpha (the "white
            // fog" / "broken matte"), false for every opaque forward shader. Read the .surface + check it.
            // (Only ADDS blend. Other transparents whose pass == opaque are caught by the name rules below.)
            if (sp) {
                auto surfData = readAsset(*sp);
                if (!surfData.empty()) {
                    std::vector<SpirvBlob> tmpBlobs; bool passTransparent = false;
                    bool parsed = parseRendShadForward(surfData, tmpBlobs, &passTransparent);
                    if (passTransparent) { md.useBlend = true; log("  [blend] '%s' forward pass = ALPHA-BLENDED (f4-omitted)", md.name.c_str()); }
                    if (std::getenv("HSR_BLENDDBG")) log("  [blendchk] '%s' surf=%zuB parsed=%d transp=%d sp=%s", md.name.c_str(), surfData.size(), (int)parsed, (int)passTransparent, sp->c_str());
                }
            }
            if (sp && (sp->find("blend") != std::string::npos ||
                       sp->find("Blend") != std::string::npos))
                md.useBlend = true;
            // The cook's GENERATED rotation / oscillation / uv-scroll shaders use a "_b" filename-suffix for their
            // BLEND variant (rot_<hash>_b.surface, osc_*_b, uvscroll_*_b, +optional _dc) — built from the unlitblend
            // base, so they ARE alpha-blended, but the name carries no "blend" token and the forward pass-state matches
            // unlitblend's (which the renderer detects by NAME, not f4-omission). Without this the Outer Wilds planet /
            // erebor wisp billboards render OPAQUE in HSL-mode preview — their transparent margins become solid boxes,
            // so a planet's whole quad shows (looks huge / "super close"). Match the cook's _b suffix convention.
            if (sp && (sp->find("_b.surface") != std::string::npos || sp->find("_b_dc.surface") != std::string::npos))
                md.useBlend = true;
            // The cook's wisp-breathe shader (wispscale.surface, the erebor flame wisps) is built from the unlitblend
            // base + blend material, but its name has no "blend" token -> same opaque-box bug as the _b shaders above.
            if (sp && sp->find("wispscale") != std::string::npos) md.useBlend = true;
            // vatlitbubble is a TRANSLUCENT shader (its frag computes alpha/alphaSq/fogAlpha + ACES tonemap)
            // but its name has no "blend" token, so it was drawn in the OPAQUE pass — the alpha was ignored
            // and the bubbles rendered as solid GRAY spheres. Mark bubble shaders blend so the alpha shows.
            if (sp && sp->find("bubble") != std::string::npos) md.useBlend = true;
            // Foliage/card shaders (unlitfoliage — the vista MERGED_BG rocks/cliffs, cWeed/coral cards) are
            // alpha CUTOUTS: the texture's alpha=0 border must drop out, not render as solid black. The frag
            // discards by design but the per-material variant pick doesn't always hit the discard variant, so
            // mark it alpha-test (and blend as the safety net) to kill the black halo.
            if (sp && sp->find("foliage") != std::string::npos) { md.alphaTest = true; md.useBlend = true; }
            // Transparent atmospheric/water shaders that lack a "blend" token but ARE alpha-blended on device:
            // unlitmist (the mist planes rendered as SOLID BLOCKS), lakesurf/water (transparent surface), smoke.
            if (sp && (sp->find("mist") != std::string::npos || sp->find("smoke") != std::string::npos ||
                       sp->find("lakesurf") != std::string::npos || sp->find("water") != std::string::npos))
                md.useBlend = true;
            // animatedfog (atmospheric_fog_*, waterfallSplash) + mattepaintingalpha (waterfall_matte) are
            // ALPHA-BLENDED on device, but their forward pass-state is byte-identical to OPAQUE (proven via
            // readAsset f4-check: transp=0) — the blend lives in the MATL render-queue (not yet parsed). Their
            // base texture is opaque (alpha=255) so the device shader computes the fog/matte alpha itself; we
            // just need the alpha-blend pipeline. Match by device shader name as the render-queue proxy until
            // the MATL queue field is decoded. (The skybox 'skyboxwithglobalfog' is excluded — opaque dome.)
            if (sp && (sp->find("animatedfog") != std::string::npos || sp->find("mattepainting") != std::string::npos))
                md.useBlend = true;
            // Candle FLAME / glow = ADDITIVE emissive (self-lit, brightens the scene; not opaque).
            if (sp && (sp->find("flame") != std::string::npos || sp->find("glow") != std::string::npos)) {
                md.useBlend = true; md.additive = true;
            }

            // Load EVERY texture the material references into its role slot, exactly as
            // libshell binds each shader texture resource by name. A HAVEN prop material
            // carries base-color(+metallic), ONxRNy(normal+rough+AO), and sometimes ORM /
            // emissive / lightmap. The base-color is the one bound to BaseColorMetallic_Tx.
            // Base-color selection: a material may list the SYSTEM white.png default FIRST and the REAL
            // base-colour texture (e.g. t_fabricbeige_fabricteal_basecolormetallic = the blue cushion
            // fabric) after it. Picking "first base wins" grabbed white -> washed/flat props. PREFER a
            // texture whose name actually marks it as a base colour ("basecolor"/"albedo"/"diffuse") over
            // the generic white/renderer_module default.
            u64 baseIng = 0, defaultBaseIng = 0;
            // FAITHFUL texture-role assignment by samplerNameHash (device binds each texture to its shader
            // sampler by MurmurHash3(samplerName,0) — see matlmatl textureValues). The old filename heuristic
            // (texRole) missed horror's lightmaps (named _lm/.hdr/candle_lightmap) -> gm.lmView stayed WHITE
            // -> the baked-dark scene washed bright. PROVEN: MurmurHash3("baseColorTex")==0xe9c182de.
            auto roleFromSamplerHash = [](u32 h) -> int {
                static const std::pair<std::string,int> kS[] = {
                    {"lightmap",4},{"Lightmap_Tx",4},{"LightMap_Tx",4},
                    {"ONxRNy_Tx",1},{"Normal_Tx",1},
                    {"RBAoDir_Tx",2},{"metalnessRoughnessOcclusionTex",2},
                    {"EmissiveColor_Tx",3},{"Emissive_Tx",3},{"emissiveTex",3},
                    {"baseColorTex",0},{"BaseColorMetallic_Tx",0},{"BaseColor_Tx",0},
                };
                for (auto& s : kS) if (murmur3_x86_32(s.first.data(), s.first.size(), 0) == h) return s.second;
                return -1;
            };
            auto samplerHashForIng = [&](u64 ing) -> u32 {
                for (auto& st : matInfo.samplerTex) if (st.second == ing) return st.first;
                return 0;
            };
            for (u64 ing : matInfo.texIngs) {
                AssetKey k = {0, ing, matInfo.texTgt};
                const std::string* tp = resolve(k);
                if (!tp) { k.pkg = matInfo.texPkg; tp = resolve(k); }
                if (!tp) continue;
                {   // NEVER bind renderer_module/missing_lightmap.png — it's the "no baked lightmap yet"
                    // PLACEHOLDER (gray 64x64) the device swaps for the real baked lightmap at runtime.
                    // It was a role-4 entry listed AFTER the real lightmap (candle_lightmap) and OVERWROTE
                    // md.lmRGBA -> the candle (and others) rendered gray. Skip it so the real lightmap wins
                    // (and meshes that ONLY have it fall to the white default = show base albedo, not gray).
                    std::string ln=*tp; for (char& c:ln) c=(char)tolower((unsigned char)c);
                    if (ln.find("missing_lightmap") != std::string::npos) continue;
                }
                {   // VAT offset texture (half-float) — decode separately, never as ASTC/base.
                    std::string ln = *tp; for (char& c : ln) c = (char)tolower((unsigned char)c);
                    if (ln.find("vatdata") != std::string::npos) {
                        AssetKey tk = {matInfo.texPkg, ing, matInfo.texTgt};
                        if (decodeVatFloat(tk, md)) log("    [vat]      %s (%ux%u f16)", tp->c_str(), md.vatW, md.vatH);
                        continue;
                    }
                }
                u32 sh = samplerHashForIng(ing);
                int role = sh ? roleFromSamplerHash(sh) : -1;   // device-faithful: role by samplerNameHash
                if (role < 0) role = texRole(*tp);              // fallback: filename heuristic
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
                                md.hasOrm = true; md.ormTexName = *tp; log("    [orm]      %s", tp->c_str()); } break;
                    case 4: {
                                // ⛔ The pbrlightmap materials reference white.png / a renderer_module default in
                                // their lightmap slot as a PLACEHOLDER (the device swaps it for the real baked
                                // lightmap at runtime, like missing_lightmap.png). Binding it set hasLightmap=true,
                                // which made applyLightmapOverrides SKIP the mesh (line "if md.hasLightmap continue")
                                // -> the REAL baked lightmap (shipped by the vista's *_lm override) never bound ->
                                // haven ceiling/cushions/candles rendered FLAT/DARK ("not lit"). Skip the placeholder
                                // so the override chain binds the real lightmap.
                                std::string ln=*tp; for (char& c:ln) c=(char)tolower((unsigned char)c);
                                bool placeholder = ln.find("white.png")!=std::string::npos || ln.find("renderer_module")!=std::string::npos;
                                if (!placeholder && decodeTexInto(tk, md.lmRGBA, md.lmW, md.lmH)) {
                                    md.hasLightmap = true; log("    [lightmap] %s", tp->c_str());
                                }
                            } break;
                    case 3: if (decodeTexInto(tk, md.emissiveRGBA, md.emissiveW, md.emissiveH)) {
                                md.hasEmissive = true; log("    [emissive] %s", tp->c_str()); } break;
                    case 0: default: {
                        std::string ln = *tp; for (char& c : ln) c = (char)tolower((unsigned char)c);
                        bool isRealBase = ln.find("basecolor") != std::string::npos ||
                                          ln.find("albedo") != std::string::npos ||
                                          ln.find("diffuse") != std::string::npos;
                        bool isDefault  = ln.find("renderer_module") != std::string::npos ||
                                          ln.find("white.png") != std::string::npos;
                        if (isRealBase && !isDefault) { if (!baseIng) baseIng = ing; }   // the REAL fabric/albedo
                        else if (!defaultBaseIng) defaultBaseIng = ing;                  // white / generic fallback
                        break;
                    }
                }
            }
            if (!baseIng) baseIng = defaultBaseIng;
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

        // ── Baked-LIGHTMAP OVERRIDE map (THE warm room lighting). V205 binds each mesh's baked lightmap
        //    (t_<mesh>_lmhdr[2].hdr under .../lightmaps/) BY NAME, NOT via material textureValues — so they
        //    show up "unreferenced" and the room renders FLAT (vs the device's soft warm baked light). Build
        //    name->AssetKey so each mesh fetches its lightmap. (Vista envs ship these; haven2025 alone doesn't.)
        auto lmKeyOf = [](std::string p) -> std::string {
            for (auto& c : p) c = (char)tolower((unsigned char)c);
            // the asset path is ".../lightmaps/t_<mesh>_lmhdr[2].hdr/tex_r676" — the lightmap name is the
            // PATH COMPONENT containing "_lmhdr", NOT the basename ("tex_r676"). Extract that component.
            size_t h = p.find("_lmhdr"); if (h == std::string::npos) return "";
            size_t s = p.rfind('/', h); s = (s == std::string::npos) ? 0 : s + 1;
            std::string name = p.substr(s, h - s);     // e.g. "t_sm_platformblocks"
            if (name.rfind("t_", 0) == 0) name = name.substr(2);
            size_t sh = name.find("_shell"); if (sh != std::string::npos) name = name.substr(0, sh);
            // strip prefixes in ANY order (sm_merged_cabinetb -> cabinetb, sm_lbg_x -> x) so the lightmap
            // name ("t_sm_merged_cabinetb_shell") matches the mesh ("SM_LBG_cabinetB").
            for (bool ch = true; ch; ) { ch = false;
                for (std::string pre : {std::string("sm_"),std::string("merged_"),std::string("lbg_"),std::string("inst_")})
                    if (name.rfind(pre,0)==0) { name = name.substr(pre.size()); ch = true; } }
            return name;
        };
        for (auto& [key, path] : assetMap) {
            if (path.find("_lmhdr") == std::string::npos) continue;
            std::string k = lmKeyOf(path);
            if (k.empty() || lmRawByKey.count(k)) continue;
            auto raw = readAsset(path);
            if (!raw.empty()) lmRawByKey[k] = std::move(raw);
        }
        log("  Lightmap-override: cached %zu baked lightmaps", lmRawByKey.size());

        // ── DEVICE-FAITHFUL baked-lightmap mapping (JSON): cooked USD templates give meshGUID->name; the
        //    calming_lightmap_overrides *_lm.hstf give sourceId(meshGUID)->lightMapTexture(the _lmhdr asset).
        {
            auto guid13 = [](const std::string& s) -> std::string {
                size_t sl = s.find_last_of('/'); std::string g = (sl==std::string::npos)?s:s.substr(sl+1);
                return g.substr(0, 13);
            };
            int nf_lm = (int)mz_zip_reader_get_num_files(&sceneZip);
            mz_zip_archive_file_stat lmst;
            for (int zi = 0; zi < nf_lm; ++zi) {
                if (!mz_zip_reader_file_stat(&sceneZip, zi, &lmst)) continue;
                std::string path = lmst.m_filename;              // "content/meta/.../X.usda/template_9k0v"
                bool isUsdTpl   = path.find(".usda/template")     != std::string::npos;
                bool isOverride = path.find("_lm.hstf")           != std::string::npos ||
                                  path.find("lightmap_overrides") != std::string::npos;
                bool isSpace    = path.find("space.hstf")         != std::string::npos;
                if (!isUsdTpl && !isOverride && !isSpace) continue;
                size_t zsz = 0; void* zd = mz_zip_reader_extract_to_heap(&sceneZip, zi, &zsz, 0);
                if (!zd) continue;
                std::string t((const char*)zd, zsz); mz_free(zd);
                if (t.empty() || t[0] != '{') continue;
                if (isSpace) {  // ScenePlatformComponent/ClippingPlanesComponent: the DEVICE's finite far/near
                    auto num = [&](const char* k) -> float { size_t q=t.find(k); if(q==std::string::npos) return 0.f;
                        size_t c=t.find(':',q+strlen(k)); if(c==std::string::npos) return 0.f;
                        return (float)atof(t.c_str()+c+1); };
                    float fc=num("\"farClippingPlane\""), nc=num("\"nearClippingPlane\"");
                    if (fc>0.f) { sceneFarClip=fc; if(nc>0.f) sceneNearClip=nc;
                        log("  Scene clip planes (device-faithful): near=%.3f far=%.0f", sceneNearClip, sceneFarClip); }
                }
                if (t.empty() || (!isUsdTpl && !isOverride)) continue;
                // whitespace-tolerant: from key opening-quote pos, read its quoted string value (JSON is
                // pretty-printed: `"id": "..."` with spaces). Returns the value's closing-quote pos.
                auto vstr = [&](size_t keyPos, size_t keyLen, std::string& out) -> size_t {
                    size_t c = t.find(':', keyPos + keyLen); if (c == std::string::npos) return std::string::npos;
                    size_t q1 = t.find('"', c + 1); if (q1 == std::string::npos) return std::string::npos;
                    size_t q2 = t.find('"', q1 + 1); if (q2 == std::string::npos) return std::string::npos;
                    out = t.substr(q1 + 1, q2 - q1 - 1); return q2;
                };
                if (isUsdTpl) {                                   // "id": "<guid>" ... "name": "<name>"
                    size_t p = 0;
                    while ((p = t.find("\"id\"", p)) != std::string::npos) {
                        std::string idv; size_t ke = vstr(p, 4, idv);
                        if (ke == std::string::npos) { p += 4; continue; }
                        std::string gd = guid13(idv);
                        size_t nmpos = t.find("\"name\"", ke), nextid = t.find("\"id\"", ke);
                        if (nmpos != std::string::npos && (nextid == std::string::npos || nmpos < nextid)) {
                            std::string nm; vstr(nmpos, 6, nm);
                            if (!nm.empty() && !lmGuidName.count(gd)) lmGuidName[gd] = nm;
                        }
                        p = ke;
                    }
                }
                if (isOverride) {                                 // "sourceId": "<guid>" ... "lightMapTexture": {...}
                    size_t p = 0;
                    while ((p = t.find("\"sourceId\"", p)) != std::string::npos) {
                        std::string srcv; size_t ke = vstr(p, 10, srcv);
                        if (ke == std::string::npos) { p += 10; continue; }
                        std::string gd = guid13(srcv);
                        size_t nextSrc = t.find("\"sourceId\"", ke);
                        size_t lm = t.find("\"lightMapTexture\"", ke);
                        if (lm != std::string::npos && (nextSrc == std::string::npos || lm < nextSrc) && !lmGuidRaw.count(gd)) {
                            size_t ip = t.find("\"ingestionId\"", lm), pp = t.find("\"packageOrRemoteId\"", lm), tp = t.find("\"targetId\"", lm);
                            std::string ing, pkg, tgt;
                            if (ip != std::string::npos && (nextSrc==std::string::npos || ip<nextSrc)) vstr(ip, 13, ing);
                            if (pp != std::string::npos && (nextSrc==std::string::npos || pp<nextSrc)) vstr(pp, 19, pkg);
                            if (tp != std::string::npos && (nextSrc==std::string::npos || tp<nextSrc)) { size_t c=t.find(':',tp+10); if(c!=std::string::npos){ size_t e=t.find_first_of(",}",c+1); tgt=t.substr(c+1,e-c-1); } }
                            if (!ing.empty()) {
                                AssetKey lk{}; lk.ing = strtoull(ing.c_str(),nullptr,10);
                                lk.pkg = strtoull(pkg.c_str(),nullptr,10); lk.tgt = (u32)strtoul(tgt.c_str(),nullptr,10);
                                const std::string* lp = resolve(lk);
                                if (!lp) { lk.pkg = 0; lp = resolve(lk); }
                                if (lp) { auto lr = readAsset(*lp); if (!lr.empty()) lmGuidRaw[gd] = std::move(lr); }
                            }
                        }
                        p = ke;
                    }
                }
            }
            log("  Lightmap GUID chain: %zu mesh-names, %zu override lightmaps", lmGuidName.size(), lmGuidRaw.size());
        }

        for (size_t ei = 0; ei < entities.size(); ++ei) {
            auto& obj = entities[ei];
            log("========================================");
            log("Entity[%zu] '%s'", ei, obj.name.c_str());

            // ── Resolve mesh
            const std::string* meshPath = resolve(obj.meshRef);
            if (!meshPath) {
                char b[64]; snprintf(b,sizeof b,"ing=%016llX tgt=%08X",(unsigned long long)obj.meshRef.ing,obj.meshRef.tgt);
                recordSkip(std::string("mesh not in manifest: ")+b);
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
                // "blkt" = BLOCKOUT/greybox proxy: every blktGardenWall/blktDeskWall/blktPortalWall/blktBackWall/
                // blktFloorBlock/blktPlatformBlockB is a flat untextured (stride-12) twin of a DETAILED mesh
                // (gardenWall, deskWall, ...). Rendering it overlaps & HIDES the real grooved wall ("misplaced,
                // needs groves"). libshell doesn't show greybox — the detailed twin is the visual. Skip it.
                // ⛔ blkt (blockout) meshes are REAL render geometry — DO NOT skip them. IDA-VERIFIED (2026-06-18):
                // libshell has NO "blkt"/"blockout"/"greybox" string or skip path; the blkt meshes live in the
                // RENDER template (home_3d_staticarch_shell.usda, same as the detailed walls), carry the SAME
                // material (ing 3870980354968886336) + a MeshPlatformComponent with isVisibleSelf=true. The old
                // name-skip ("detailed twin renders instead") was a wrong guess: it dropped structural surfaces,
                // and blktGardenGravel has NO detailed twin → its gravel surface went MISSING ([NOT RENDERED] bug).
                // Editor: load navmesh (1) + collision/walls (2) as editable translucent overlays instead of
                // dropping them. Still skip physics-duplicate (__phys_) meshes and isVisibleSelf=false UI quads.
                // Navmesh/collision are RENDERED as editable overlays (green/red) — not "not rendered". Log them as
                // OVERLAY so the [NOT RENDERED] report only lists genuinely-dropped meshes (phys dup / hidden UI).
                if (isNav && !isPhys) { overlayKind = 1; log("  OVERLAY (navmesh, editable): %s", obj.name.c_str()); }
                else if (isCol && !isPhys) { overlayKind = 2; log("  OVERLAY (collision, editable): %s", obj.name.c_str()); }
                if (isPhys || isUI) {
                    if (isUI) recordSkip("NOT RENDERED — classified UI/gizmo: "+obj.name);
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
            md.components = obj.components;   // all hstf components -> editor inspector
            // V203 skybox-IBL (gated HSR_SKYIBL): mark meshes to receive the equirect diffuse-IBL ambient bake
            // (vk_renderer per-vertex). Opt-in so default rendering (lightmaps) is untouched.
            if (std::getenv("HSR_SKYIBL")) md.iblLit = true;
            md.meshPath  = *meshPath;   // for multi-rig skeleton matching (shared .fbx path)
            md.transform = obj.transform;
            md.atlasCellIndex = obj.atlasCellIndex;                         // per-instance atlas variant (coral colour)
            md.vatTrackIndex = obj.vatTrackIndex; md.vatRateFactor = obj.vatRateFactor; md.vatTimeOffset = obj.vatTimeOffset; // per-instance VAT track/phase
            // Per-instance MaterialPropertyOverrides (matParams.* over the cooked block). Carry them onto the mesh,
            // and pull out matParams.tint as the per-instance colour multiplier (grey butterfly -> rose-tinted).
            md.matOverrides = obj.matOverrides;
            for (const auto& ov : obj.matOverrides) {
                if (ov.name.find("tint") != std::string::npos || ov.name.find("Tint") != std::string::npos) {
                    md.tint[0]=ov.v[0]; md.tint[1]=ov.v[1]; md.tint[2]=ov.v[2];
                    md.tint[3] = (ov.v[3] > 0.0f) ? ov.v[3] : 1.0f;  // alpha defaults to opaque (override w often 0)
                }
            }
            md.hasWorldMatrix = obj.hasWorldMatrix;                         // faithful 4×4 world (parent→child)
            if (obj.hasWorldMatrix) memcpy(md.worldMatrix, obj.worldMatrix, sizeof(md.worldMatrix));
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
                               &md.boneIndices, &md.boneWeights, md.name.c_str(), &md.uvs2, &md.bonePalette, &md.colors, &md.uvs3, &md.uvs4)) {
                recordSkip("RENDMESH parse failed: " + (meshPath ? *meshPath : md.name));
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
            // SKYBOX dome (SkyboxPlatformComponent): bind its colorTexture panorama directly — it has no material.
            // Drawn as opaque geometry (the dome surrounds the scene). Without this the sky was missing entirely.
            md.isSkybox = obj.isSkybox;
            if (obj.isSkybox) {
                if (obj.skyboxTex.ing != 0 && loadTexture(obj.skyboxTex, md)) {
                    gotTex = true; log("  [skybox] '%s' colorTexture bound (%ux%u)", md.name.c_str(), md.texW, md.texH);
                } else log("  [skybox] '%s' has skyboxMesh but no colorTexture", md.name.c_str());
            }

            // Path 1: HSTF-provided matRefs (MaterialPlatformComponent — prod envs)
            for (auto& matRef : obj.matRefs) {
                const std::string* matPath = resolve(matRef);
                if (!matPath) {
                    log("  Material not in manifest: ing=%016llX tgt=%08X",
                        (unsigned long long)matRef.ing, matRef.tgt);
                    continue;
                }
                log("  Material: %s", matPath->c_str());
                // A "pbrlightmap_tiled" material (e.g. rugA) is genuinely TILED -> keep its cooked block
                // (Tint + GlobalTile) like the room; only the plain-unwrap props (bowls/vases) strip it.
                md.tiled = (matPath->find("_tiled") != std::string::npos);
                auto matData = readAsset(*matPath);
                if (matData.empty()) { recordSkip("material data empty: "+*matPath); log("  Material data empty"); continue; }

                MatlmatlInfo matInfo;
                if (!parseMatlmatl(matData, matInfo)) { recordSkip("MATLMATL parse failed: "+*matPath); log("  MATLMATL parse failed"); continue; }
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
                // VFX overlays (god rays / sun / surface waves) carry a mostly-TRANSPARENT glow texture and
                // have no material — drawn OPAQUE they showed as a solid WHITE blob (vfx_surfaceWaves_SUN).
                // They're ADDITIVE glow (light), so the transparent areas add nothing and the glow blends.
                // The real skybox dome stays opaque.
                std::string nm = md.name; for (char& c : nm) c = (char)tolower((unsigned char)c);
                bool isVfx = nm.find("vfx") != std::string::npos || nm.find("sunray") != std::string::npos ||
                             nm.find("surfacewave") != std::string::npos || nm.find("_sun") != std::string::npos ||
                             nm.find("godray") != std::string::npos || nm.find("caustic") != std::string::npos;
                // VFX (sun/rays/surface waves): ADDITIVE + premultiply, so the transparent surround adds
                // nothing (no white blob) but the disk/glow adds light. The no-material shader outputs
                // alpha=1, so plain alpha-blend can't drop the surround — additive on a premultiplied tex is
                // the only way to get the sun glow visible WITHOUT the full-white quad. Boost so it reads.
                md.useBlend = md.useBlend || isVfx;   // PRESERVE an already-set alpha-blend (f4-omitted fog/matte); don't reset to opaque
                md.additive = md.additive || isVfx;
                if (isVfx) {
                    for (size_t p = 0; p + 3 < md.texRGBA.size(); p += 4) {
                        u32 a = md.texRGBA[p+3];
                        for (int c = 0; c < 3; ++c) {
                            u32 v = (u32)md.texRGBA[p+c] * a / 255 * 3;   // premult × 3 brightness boost
                            md.texRGBA[p+c] = (u8)(v > 255 ? 255 : v);
                        }
                    }
                }
                gotTex = true;
                log("  UnrefTex[%zu] -> %ux%u (%s)",
                    unrefTexIdx % n, ut.texW, ut.texH, isVfx ? "VFX additive premult x3" : "skybox/env opaque");
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

            // Bind this mesh's baked LIGHTMAP override (the warm room lighting), matched BY NAME — only if it
            // didn't already get a lightmap from its material and it has lightmap UVs (uv1) to sample.
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
                { std::string bn=fname; size_t p=bn.rfind('/'); if(p!=std::string::npos) bn=bn.substr(p+1);
                  recordSkip("UNCLAIMED mesh — loaded at ORIGIN (no entity transform → gap at real spot): "+bn); }
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

                if (!parseRendMesh(mdata, md.positions, md.uvs, md.indices, &md.boneIndices, &md.boneWeights,
                                   nullptr, nullptr, nullptr, &md.colors, &md.uvs3, &md.uvs4)) {   // also extract sem4 vertexColor0 + uv2/uv3
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
            // Extract a zip entry by INDEX (we walk every entry to find all skeletons).
            auto extractIdx=[&](int fi, std::vector<u8>& out)->bool{
                size_t sz=0; void* p=mz_zip_reader_extract_to_heap(&sceneZip, fi, &sz, 0);
                if (p){ out.assign((u8*)p,(u8*)p+sz); mz_free(p); return true; } return false; };
            // MULTI-RIG: enumerate EVERY skeleton (one per animated fbx: bat/owl/windmill/lantern...). For each,
            // parse its skel + the matching anim under the SAME asset key (path up to ".../<name>.fbx" or
            // "....skel/"), then bind every skinned mesh whose meshPath shares that key. The device
            // AnimationSystem is PER-ENTITY, so each rig animates independently (was a single global skel/clip).
            for (int si=0; si<nf; ++si) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&sceneZip, si, &st)) continue;
                std::string sp = st.m_filename; bool cookedNaming=false; std::string zipKey;
                size_t kp = sp.find("/__hzanim_skel_sub_targets__");
                if (kp != std::string::npos) zipKey = sp.substr(0, kp);
                else { kp = sp.find(".skel/skeleton"); if (kp!=std::string::npos){ zipKey=sp.substr(0,kp); cookedNaming=true; } }
                if (zipKey.empty()) continue;                    // not a skeleton entry
                // Zip entries carry a "content/" prefix that manifest/mesh paths (md.meshPath) do NOT — strip
                // it for mesh matching (that prefix mismatch was binding 0 meshes to every rig).
                std::string key = (zipKey.rfind("content/",0)==0) ? zipKey.substr(8) : zipKey;
                std::vector<u8> sb; if (!extractIdx(si, sb)) continue;
                AnimGroup g; if (!parseRendSkel(sb, g.skel)) continue;
                // matching anim clip. V79/glTF share the .fbx base, so the clip path starts with the skeleton's
                // zipKey. The COOK instead names them armatureN.skel + animclipM.anim under ONE home dir (different
                // bases, independent dedup indices) — so prefix-matching found NO clip (cooked multi-rig = "0 groups",
                // every skinned mesh forced static). For cooked naming, match within the shared HOME DIR and require the
                // clip's JOINT COUNT to equal this skeleton's (the device pairs them via the AnimatorComponent; joint
                // count is the file-level proxy). [[project_hsr_skinned_rendmesh_skinblock]]
                std::string homeDir = zipKey; { size_t hs = zipKey.rfind('/'); if (hs != std::string::npos) homeDir = zipKey.substr(0, hs + 1); }
                std::vector<u8> ab; bool gotAnim=false;
                // PASS 0 = exact base prefix (every env where the clip shares the skeleton's .fbx/<name> base —
                // V79/glTF AND official V203). PASS 1 (cook only) = the cook's armatureN.skel + animclipM.anim live
                // under one home dir with different bases, so fall back to home-dir + matching JOINT COUNT. Exact-first
                // means official envs keep their precise pairing (no cross-rig mismatch); only our cook needs the fallback.
                for (int pass=0; pass<(cookedNaming?2:1) && !gotAnim; ++pass) {
                    const std::string& pre = (pass==0) ? zipKey : homeDir;
                    for (int ai=0; ai<nf && !gotAnim; ++ai) {
                        mz_zip_archive_file_stat at; if (!mz_zip_reader_file_stat(&sceneZip, ai, &at)) continue;
                        std::string ap = at.m_filename;
                        if (ap.size()<pre.size() || ap.compare(0,pre.size(),pre)!=0) continue;
                        if (ap.find("__hzanim_anim_sub_targets__")==std::string::npos && ap.find(".anim/anim")==std::string::npos) continue;
                        if (!extractIdx(ai, ab) || !parseRendClip(ab, (int)g.skel.joints.size(), g.clip)) continue;
                        if (pass==1) {   // home-dir fallback: disambiguate the rigs sharing the dir — clip must be THIS skeleton's
                            int cj = g.clip.acl ? g.clip.aclJoints : g.clip.nJoints;
                            if (cj > 0 && cj != (int)g.skel.joints.size()) { if (g.clip.acl) hzAclDestroy(g.clip.acl); g.clip = RendClip{}; continue; }
                        }
                        gotAnim=true;
                    }
                }
                if (!gotAnim) { log("  HzAnim '%s': %zu joints but no clip", key.c_str(), g.skel.joints.size()); continue; }
                // FAITHFUL slot->joint remap (the MAP libshell reads, NOT a DFS guess). The mesh's per-vertex
                // boneIndices are SLOTS into the mesh's bone PALETTE (md.bonePalette, read from RENDMESH
                // ROOT.f2[0].f0 = one MurmurHash3_x86_32(jointName,0) per slot). Each slot maps to the
                // skeleton joint whose name-hash matches. Precompute each skeleton joint's name-hash once.
                int nJ=(int)g.skel.joints.size();
                std::vector<u32> jointHash(nJ);
                for (int j=0;j<nJ;++j) jointHash[j]=murmur3_x86_32(g.skel.joints[j].name.data(), g.skel.joints[j].name.size(), 0);
                // DFS order kept ONLY as a fallback for meshes whose RENDMESH carries no palette.
                std::vector<int> dfs, stk;
                for (int j=nJ-1;j>=0;--j) if (g.skel.joints[j].parent<0) stk.push_back(j);
                while(!stk.empty()){int j=stk.back();stk.pop_back();dfs.push_back(j);for(int c=nJ-1;c>=0;--c) if(g.skel.joints[c].parent==j) stk.push_back(c);}
                bool ident=((int)dfs.size()==nJ); for(int s=0;ident&&s<(int)dfs.size();++s) if(dfs[s]!=s) ident=false;
                // bind every skinned mesh from THIS fbx (matched by shared asset path).
                for (size_t mi=0; mi<meshes.size(); ++mi) {
                    auto& md=meshes[mi];
                    if (!md.hasBones || md.boneIndices.size() < md.positions.size()/3*4) continue;
                    // PREFER the shared-base path match (V79/glTF/official: mesh shares the skeleton's .fbx/<name>
                    // base). The cook's mXXX.rendmesh does NOT share the armatureN base, so for cooked naming fall back
                    // to BONE PALETTE: the mesh belongs to THIS rig iff every palette slot's joint name-hash is in this
                    // skeleton (the faithful map the device uses) — disambiguates the spacestation's 1-joint vs 25-joint vehicle rigs.
                    bool pathMatch = (md.meshPath.size()>=key.size() && md.meshPath.compare(0,key.size(),key)==0);
                    if (!pathMatch) {
                        if (!cookedNaming || md.bonePalette.empty()) continue;
                        bool all=true; for (u32 ph : md.bonePalette){ bool f=false; for(int j=0;j<nJ;++j) if((u32)jointHash[j]==ph){f=true;break;} if(!f){all=false;break;} }
                        if (!all) continue;
                    }
                    if (!md.bonePalette.empty() && !std::getenv("HSR_HZNOREMAP")) {
                        // slot -> joint by the palette's name-hash (THE faithful map). Fixes the bird flock
                        // (palette != DFS); the whale palette == its DFS so it's unchanged.
                        std::vector<int> slotToJoint(md.bonePalette.size(), -1); int nMapped=0;
                        for (size_t s=0;s<md.bonePalette.size();++s)
                            for (int j=0;j<nJ;++j) if (jointHash[j]==md.bonePalette[s]) { slotToJoint[s]=j; ++nMapped; break; }
                        for (auto& bi : md.boneIndices) if ((size_t)bi<slotToJoint.size() && slotToJoint[bi]>=0) bi=(u8)slotToJoint[bi];
                        if (verbose) log("  [bonemap] '%s' palette %zu slots -> %d mapped by name-hash (faithful)", md.name.c_str(), md.bonePalette.size(), nMapped);
                    } else if ((int)dfs.size()==nJ && !ident && !std::getenv("HSR_HZNOREMAP") && !cookedNaming) {
                        for (auto& bi : md.boneIndices) if (bi<nJ) bi=(u8)dfs[bi];   // fallback: RENDMESH had no palette
                    }
                    SkinRec r; r.meshIdx=mi; r.basePos=md.positions; r.bIdx=md.boneIndices; r.bWgt=md.boneWeights;
                    g.skinRecs.push_back(std::move(r)); md.dynamicVerts=true;
                }
                size_t slash=key.rfind('/');
                log("  HzAnim rig '%s': %zu joints, %d frames, %zu skinned meshes",
                    key.substr(slash==std::string::npos?0:slash+1).c_str(),
                    g.skel.joints.size(), g.clip.nFrames, g.skinRecs.size());
                if (verbose && g.clip.acl) {
                    float dur = hzAclDuration(g.clip.acl);
                    std::vector<float> s0, s1; sampleRendClip(g.skel, g.clip, 0.f, s0); sampleRendClip(g.skel, g.clip, dur*0.5f, s1);
                    float maxd=0.f; for (size_t i=0;i<s0.size()&&i<s1.size();++i){ float dd=std::fabs(s0[i]-s1[i]); if(dd>maxd)maxd=dd; }
                    log("    [ACL] dur=%.3fs aclJoints=%d fps=%.1f  skinDelta(t=0 vs %.2fs)=%.4f %s",
                        dur, g.clip.aclJoints, g.clip.fps, dur*0.5f, maxd, maxd<1e-4f?"<<STATIC!":"(animates)");
                }
                if (!g.skinRecs.empty()) animGroups.push_back(std::move(g));
            }
            log("  HzAnim: %zu animation groups (multi-rig)", animGroups.size());
            // ONLY meshes bound to a real rig may animate. Horror's static trees/ground carry false-positive
            // sem7/8 "bone" bytes and envHasSkeleton flagged them skinned -> the skinner wiggled EVERY mesh
            // (user caught it in wireframe). Clear bone/dynamic flags on every non-rig mesh = static.
            {
                std::vector<char> rigged(meshes.size(), 0);
                for (auto& g : animGroups) for (auto& r : g.skinRecs) if (r.meshIdx < rigged.size()) rigged[r.meshIdx] = 1;
                int cleared = 0;
                for (size_t mi=0; mi<meshes.size(); ++mi) if (!rigged[mi]) {
                    auto& md = meshes[mi];
                    if (md.hasBones || md.dynamicVerts) ++cleared;
                    md.hasBones = false; md.dynamicVerts = false;
                    md.boneIndices.clear(); md.boneWeights.clear();
                }
                log("  HzAnim: cleared stray skinned flag on %d non-rig meshes (now static)", cleared);
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

        // ── PROBE: HSR_PROBE="x,z" lists the meshes nearest that world point (find a gap's mesh) ───────
        if (const char* pe = std::getenv("HSR_PROBE")) {
            float px = 0, pz = 0; sscanf(pe, "%f,%f", &px, &pz);
            // Meshes whose WORLD XZ-AABB CONTAINS the point (so big floor meshes centred elsewhere are found),
            // sorted by min-Y (the floor sits lowest). Transform every vert to world (quat xyzw · scale + pos).
            std::vector<std::pair<float,size_t>> hit;  // {minY, idx}
            for (size_t i = 0; i < meshes.size(); ++i) {
                auto& m = meshes[i]; size_t nv = m.positions.size()/3; if (!nv) continue;
                const float* q = m.transform.rot; const float* sc = m.transform.scale; const float* tp = m.transform.pos;
                float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
                for (size_t v=0; v<nv; ++v) {
                    float lx=m.positions[v*3]*sc[0], ly=m.positions[v*3+1]*sc[1], lz=m.positions[v*3+2]*sc[2];
                    float tx=2*(q[1]*lz-q[2]*ly), ty=2*(q[2]*lx-q[0]*lz), tz=2*(q[0]*ly-q[1]*lx);
                    float wx=lx+q[3]*tx+(q[1]*tz-q[2]*ty)+tp[0], wy=ly+q[3]*ty+(q[2]*tx-q[0]*tz)+tp[1], wz=lz+q[3]*tz+(q[0]*ty-q[1]*tx)+tp[2];
                    if(wx<mn[0])mn[0]=wx; if(wy<mn[1])mn[1]=wy; if(wz<mn[2])mn[2]=wz;
                    if(wx>mx[0])mx[0]=wx; if(wy>mx[1])mx[1]=wy; if(wz>mx[2])mx[2]=wz;
                }
                if (px>=mn[0]-0.5f && px<=mx[0]+0.5f && pz>=mn[2]-0.5f && pz<=mx[2]+0.5f)
                    hit.push_back({mn[1], i});
            }
            std::sort(hit.begin(), hit.end());
            fprintf(stderr, "[PROBE] %zu meshes whose world XZ-AABB contains (%.1f,%.1f) [sorted by minY]:\n", hit.size(), px, pz);
            for (int k=0; k<30 && k<(int)hit.size(); ++k) { size_t i=hit[k].second; auto& m=meshes[i];
                fprintf(stderr, "  minY=%6.1f blend=%d ovl=%d  %-44.44s  %s\n",
                    hit[k].first, (int)m.useBlend, m.overlayKind, m.name.c_str(), m.shaderPath.c_str()); }
        }

        // ── LOAD REPORT: skipped / undecoded elements (ALWAYS printed, even with verbose off) ──────────
        {
            std::sort(skips.begin(), skips.end());
            skips.erase(std::unique(skips.begin(), skips.end()), skips.end());
            fprintf(stderr, "[LOAD REPORT] %zu meshes loaded; %zu distinct skipped/undecoded element(s)%s\n",
                    meshes.size(), skips.size(), skips.empty() ? "  (all decoded)" : ":");
            size_t cap = skips.size() < 50 ? skips.size() : 50;
            for (size_t i = 0; i < cap; ++i) fprintf(stderr, "   - %s\n", skips[i].c_str());
            if (skips.size() > cap) fprintf(stderr, "   ... (%zu more)\n", skips.size() - cap);
        }

        mz_zip_reader_end(&sceneZip);
        free(sceneZipData);
        return loadedCount > 0;
    }
};
