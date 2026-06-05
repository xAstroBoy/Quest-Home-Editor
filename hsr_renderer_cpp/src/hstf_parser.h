#pragma once
#include "types.h"
#include "tinyjson.h"
#include <string>
#include <vector>
#include <cmath>

// HSTF v5 JSON template parser — 1:1 replica of
// arvr/projects/mhe/frameworks/template/code/source/template/asset/TemplateAssetHandler.cpp
//
// Parses Horizon Scene Template Format JSON, extracting DrawableEntity list.

inline bool parseHstf(const std::string& jsonStr, std::vector<DrawableEntity>& out,
                      bool verbose = true) {
    try {
        auto doc = tinyjson::parse(jsonStr);
        if (!doc.isObject() || !doc.has("entities")) {
            if (verbose) fprintf(stderr, "[HSTF] ERROR: missing 'entities' array in JSON\n");
            return false;
        }

        const auto& entities = doc["entities"];
        if (!entities.isArray()) {
            if (verbose) fprintf(stderr, "[HSTF] ERROR: 'entities' is not an array\n");
            return false;
        }
        if (verbose) fprintf(stderr, "[HSTF] Parsing %zu entities...\n", entities.size());

        // Index entities by id
        std::map<std::string, tinyjson::Value> entityMap;
        for (size_t i = 0; i < entities.size(); ++i) {
            const auto& e = entities[i];
            if (e.has("id")) entityMap[e["id"].asString()] = e;
        }

        for (const auto& [eid, ent] : entityMap) {
            DrawableEntity draw;
            draw.name = ent.has("name") ? ent["name"].asString() : eid;
            if (verbose) fprintf(stderr, "[HSTF]   Entity '%s' (id=%s)\n", draw.name.c_str(), eid.c_str());

            bool hasMesh = false;
            const auto& comps = ent.has("components") ? ent["components"] : tinyjson::Value();
            if (comps.isArray()) {
                for (size_t ci = 0; ci < comps.size(); ++ci) {
                    const auto& comp = comps[ci];
                    if (!comp.has("data")) continue;
                    const auto& cd = comp["data"];
                    std::string cls = cd.has("class") ? cd["class"].asString() : "";
                    const auto& dat = cd.has("data") ? cd["data"] : tinyjson::Value();

                    if (cls.find("TransformPlatformComponent") != std::string::npos) {
                        if (dat.has("localPosition")) {
                            draw.transform.pos[0] = (float)(dat["localPosition"].has("x") ? dat["localPosition"]["x"].asFloat() : 0);
                            draw.transform.pos[1] = (float)(dat["localPosition"].has("y") ? dat["localPosition"]["y"].asFloat() : 0);
                            draw.transform.pos[2] = (float)(dat["localPosition"].has("z") ? dat["localPosition"]["z"].asFloat() : 0);
                        }
                        if (dat.has("localRotation")) {
                            const auto& lr = dat["localRotation"];
                            float rx = (float)(lr.has("x") ? lr["x"].asFloat() : 0);
                            float ry = (float)(lr.has("y") ? lr["y"].asFloat() : 0);
                            float rz = (float)(lr.has("z") ? lr["z"].asFloat() : 0);
                            if (lr.has("w")) {
                                // Already a quaternion — normalize defensively (buildModelMatrix assumes unit).
                                float rw = (float)lr["w"].asFloat();
                                float L = std::sqrt(rx*rx + ry*ry + rz*rz + rw*rw);
                                if (L < 1e-8f) { rx = ry = rz = 0; rw = 1; L = 1; }
                                draw.transform.rot[0]=rx/L; draw.transform.rot[1]=ry/L;
                                draw.transform.rot[2]=rz/L; draw.transform.rot[3]=rw/L;
                            } else {
                                // HSTF TransformPlatformComponent stores localRotation as EULER RADIANS
                                // (x,y,z, no w). Reading it as a quaternion (w=1) yields a non-unit quat
                                // that buildModelMatrix turns into a huge scale distortion (items flung
                                // off the map). Convert euler->quaternion (intrinsic XYZ).
                                float hx=rx*0.5f, hy=ry*0.5f, hz=rz*0.5f;
                                float cx=std::cos(hx), sx=std::sin(hx);
                                float cy=std::cos(hy), sy=std::sin(hy);
                                float cz=std::cos(hz), sz=std::sin(hz);
                                draw.transform.rot[0] = sx*cy*cz + cx*sy*sz;
                                draw.transform.rot[1] = cx*sy*cz - sx*cy*sz;
                                draw.transform.rot[2] = cx*cy*sz + sx*sy*cz;
                                draw.transform.rot[3] = cx*cy*cz - sx*sy*sz;
                            }
                        }
                        if (dat.has("localScale")) {
                            draw.transform.scale[0] = (float)(dat["localScale"].has("x") ? dat["localScale"]["x"].asFloat() : 1);
                            draw.transform.scale[1] = (float)(dat["localScale"].has("y") ? dat["localScale"]["y"].asFloat() : 1);
                            draw.transform.scale[2] = (float)(dat["localScale"].has("z") ? dat["localScale"]["z"].asFloat() : 1);
                        }
                    }
                    else if (cls == "horizon::platform_api::MeshPlatformComponent") {
                        // Only exact MeshPlatformComponent — not ColliderMeshPlatformComponent etc.
                        // Try "mesh" key (standard) then "meshAsset" (some APK versions).
                        const tinyjson::Value* meshNode = nullptr;
                        if (dat.has("mesh")) meshNode = &dat["mesh"];
                        else if (dat.has("meshAsset")) meshNode = &dat["meshAsset"];
                        if (meshNode) {
                            const auto& m = *meshNode;
                            draw.meshRef = {
                                (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0)
                            };
                            hasMesh = draw.meshRef.ing != 0;
                            if (verbose && hasMesh)
                                fprintf(stderr, "[HSTF]     mesh: pkg=%016llX ing=%016llX tgt=%08X\n",
                                    (unsigned long long)draw.meshRef.pkg,
                                    (unsigned long long)draw.meshRef.ing,
                                    draw.meshRef.tgt);
                        }
                    }
                    else if (cls.find("MaterialPlatformComponent") != std::string::npos) {
                        if (dat.has("materials") && dat["materials"].isArray()) {
                            for (size_t mi = 0; mi < dat["materials"].size(); ++mi) {
                                const auto& m = dat["materials"][mi];
                                AssetKey matRef = {
                                    (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                    (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                    (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0)
                                };
                                draw.matRefs.push_back(matRef);
                                if (verbose) fprintf(stderr, "[HSTF]     material[%zu]: pkg=%016llX ing=%016llX tgt=%08X\n",
                                    mi, (unsigned long long)matRef.pkg,
                                    (unsigned long long)matRef.ing, matRef.tgt);
                            }
                        }
                    }
                }
            }
            if (hasMesh) out.push_back(draw);
        }
        if (verbose) fprintf(stderr, "[HSTF] Found %zu drawable entities\n", out.size());
        return !out.empty();
    } catch (const std::exception& e) {
        if (verbose) fprintf(stderr, "[HSTF] PARSE EXCEPTION: %s\n", e.what());
        return false;
    }
}
