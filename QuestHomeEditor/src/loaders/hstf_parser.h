#pragma once
#include "core/types.h"
#include "core/tinyjson.h"
#include <string>
#include <vector>
#include <cmath>

// HSTF v5 JSON template parser — 1:1 replica of
// arvr/projects/mhe/frameworks/template/code/source/template/asset/TemplateAssetHandler.cpp
//
// Parses Horizon Scene Template Format JSON, extracting DrawableEntity list.

// Flatten a component's `data` JSON to (dotted-key, display-string) pairs so the editor can inspect/edit ANY
// component generically (nested objects -> "a.b.x", arrays -> "a[0].x"). Truth for field meaning: V203_COMPONENTS_IDA.md.
inline void flattenHstfJson(const tinyjson::Value& v, const std::string& prefix,
                            std::vector<std::pair<std::string,std::string>>& out, int depth = 0) {
    if (depth > 8) return;
    if (v.isObject()) {
        for (const auto& kv : v.objVal) {
            std::string key = prefix.empty() ? kv.first : prefix + "." + kv.first;
            flattenHstfJson(kv.second, key, out, depth + 1);
        }
    } else if (v.isArray()) {
        for (size_t i = 0; i < v.arrVal.size(); ++i)
            flattenHstfJson(v.arrVal[i], prefix + "[" + std::to_string(i) + "]", out, depth + 1);
    } else {
        char buf[64]; std::string s;
        if (v.isBool())        s = v.asBool() ? "true" : "false";
        else if (v.isInt())  { snprintf(buf, sizeof buf, "%lld", (long long)v.asInt()); s = buf; }
        else if (v.isFloat()){ snprintf(buf, sizeof buf, "%g", v.asFloat()); s = buf; }
        else if (v.isString()) s = v.asString();
        else s = "null";
        out.push_back({ prefix, s });
    }
}

// Capture EVERY component on an entity (class/version/flattened fields) for the generic editor inspector — even
// components the typed render pipeline ignores (sound, locomotion, lightprobes, sittable, colliders, extras...).
inline void captureHstfComponents(const tinyjson::Value& ent, DrawableEntity& draw) {
    if (!ent.has("components") || !ent["components"].isArray()) return;
    const auto& comps = ent["components"];
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        const auto& comp = comps[ci];
        if (!comp.has("data")) continue;
        const auto& cd = comp["data"];
        EnvComponent ec;
        ec.cls = cd.has("class") ? cd["class"].asString() : "";
        size_t p = ec.cls.rfind("::");
        ec.shortCls = (p == std::string::npos) ? ec.cls : ec.cls.substr(p + 2);
        ec.version = cd.has("version") ? (int)cd["version"].asInt() : 0;
        if (cd.has("data")) flattenHstfJson(cd["data"], "", ec.fields);
        if (std::getenv("HSR_COMPDUMP"))
            fprintf(stderr, "[COMP] %-40s v%d  %zu fields  (entity '%s')\n",
                    ec.shortCls.c_str(), ec.version, ec.fields.size(), draw.name.c_str());
        draw.components.push_back(std::move(ec));
    }
}

// Extract an entity's local TransformPlatformComponent (pos / rot[euler→quat or quat] / scale).
inline Transform parseHstfEntityTransform(const tinyjson::Value& ent) {
    Transform t;
    if (!ent.has("components") || !ent["components"].isArray()) return t;
    const auto& comps = ent["components"];
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        const auto& comp = comps[ci];
        if (!comp.has("data")) continue;
        const auto& cd = comp["data"];
        std::string cls = cd.has("class") ? cd["class"].asString() : "";
        if (cls.find("TransformPlatformComponent") == std::string::npos) continue;
        const auto& dat = cd.has("data") ? cd["data"] : tinyjson::Value();
        if (dat.has("localPosition")) {
            const auto& p = dat["localPosition"];
            t.pos[0]=(float)(p.has("x")?p["x"].asFloat():0); t.pos[1]=(float)(p.has("y")?p["y"].asFloat():0); t.pos[2]=(float)(p.has("z")?p["z"].asFloat():0);
        }
        if (dat.has("localRotationQuat")) {
            // AUTHORITATIVE rotation = the cooked quaternion. The sibling localRotation (euler) DISAGREES
            // (e.g. euler=180°-about-Y while the quat is identity) — euler is a stale/display value whose
            // axis order doesn't match. Reading euler flipped/tilted every rotated mesh (slanted sand, gaps).
            const auto& q = dat["localRotationQuat"];
            float qx=(float)(q.has("x")?q["x"].asFloat():0), qy=(float)(q.has("y")?q["y"].asFloat():0), qz=(float)(q.has("z")?q["z"].asFloat():0), qw=(float)(q.has("w")?q["w"].asFloat():1);
            float L=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw); if(L<1e-8f){qx=qy=qz=0;qw=1;L=1;}
            t.rot[0]=qx/L; t.rot[1]=qy/L; t.rot[2]=qz/L; t.rot[3]=qw/L;
        }
        else if (dat.has("localRotation")) {
            const auto& lr = dat["localRotation"];
            float rx=(float)(lr.has("x")?lr["x"].asFloat():0), ry=(float)(lr.has("y")?lr["y"].asFloat():0), rz=(float)(lr.has("z")?lr["z"].asFloat():0);
            // localRotation: WITH w = quaternion; WITHOUT w = EULER RADIANS (libshell sub_C3B280 = half-angle
            // euler→quat, confirmed in IDA — NOT a quat with derived w).
            if (lr.has("w")) { float rw=(float)lr["w"].asFloat(); float L=std::sqrt(rx*rx+ry*ry+rz*rz+rw*rw); if(L<1e-8f){rx=ry=rz=0;rw=1;L=1;} t.rot[0]=rx/L;t.rot[1]=ry/L;t.rot[2]=rz/L;t.rot[3]=rw/L; }
            else { float hx=rx*0.5f,hy=ry*0.5f,hz=rz*0.5f,cx=std::cos(hx),sx=std::sin(hx),cy=std::cos(hy),sy=std::sin(hy),cz=std::cos(hz),sz=std::sin(hz);
                   // intrinsic ZYX euler→quat (libshell sub_C3B280, IDA-exact). NOT XYZ.
                   t.rot[0]=sx*cy*cz-cx*sy*sz; t.rot[1]=cx*sy*cz+sx*cy*sz; t.rot[2]=cx*cy*sz-sx*sy*cz; t.rot[3]=cx*cy*cz+sx*sy*sz; }
        }
        if (dat.has("localScale")) {
            const auto& s = dat["localScale"];
            t.scale[0]=(float)(s.has("x")?s["x"].asFloat():1); t.scale[1]=(float)(s.has("y")?s["y"].asFloat():1); t.scale[2]=(float)(s.has("z")?s["z"].asFloat():1);
        }
    }
    return t;
}

// World transform of a child under a parent: world = parent ∘ child (scale·rotate·translate).
inline Transform composeTransform(const Transform& P, const Transform& C) {
    Transform R;
    R.scale[0]=P.scale[0]*C.scale[0]; R.scale[1]=P.scale[1]*C.scale[1]; R.scale[2]=P.scale[2]*C.scale[2];
    float px=P.rot[0],py=P.rot[1],pz=P.rot[2],pw=P.rot[3];
    float cx=C.rot[0],cy=C.rot[1],cz=C.rot[2],cw=C.rot[3];
    R.rot[0]=pw*cx+px*cw+py*cz-pz*cy;  R.rot[1]=pw*cy-px*cz+py*cw+pz*cx;
    R.rot[2]=pw*cz+px*cy-py*cx+pz*cw;  R.rot[3]=pw*cw-px*cx-py*cy-pz*cz;
    float vx=P.scale[0]*C.pos[0], vy=P.scale[1]*C.pos[1], vz=P.scale[2]*C.pos[2];
    float tx=2*(py*vz-pz*vy), ty=2*(pz*vx-px*vz), tz=2*(px*vy-py*vx);
    R.pos[0]=P.pos[0]+vx+pw*tx+(py*tz-pz*ty);
    R.pos[1]=P.pos[1]+vy+pw*ty+(pz*tx-px*tz);
    R.pos[2]=P.pos[2]+vz+pw*tz+(px*ty-py*tx);
    return R;
}

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
            captureHstfComponents(ent, draw);   // generic capture of ALL components for the editor inspector

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
                        // A TransformPlatformComponent with NO local pos/rot/scale (data:{}) is the cook
                        // anomaly (e.g. oceanarium Root_sand_SHELL): a flat read leaves it at the origin.
                        // Flagged here so the loader can resolve it from the level's group offset.
                        if (!dat.has("localPosition") && !dat.has("localRotation") &&
                            !dat.has("localRotationQuat") && !dat.has("localScale"))
                            draw.emptyTransform = true;
                        if (dat.has("localPosition")) {
                            draw.transform.pos[0] = (float)(dat["localPosition"].has("x") ? dat["localPosition"]["x"].asFloat() : 0);
                            draw.transform.pos[1] = (float)(dat["localPosition"].has("y") ? dat["localPosition"]["y"].asFloat() : 0);
                            draw.transform.pos[2] = (float)(dat["localPosition"].has("z") ? dat["localPosition"]["z"].asFloat() : 0);
                        }
                        if (dat.has("localRotationQuat")) {
                            // AUTHORITATIVE rotation = the cooked quaternion. The sibling localRotation (euler)
                            // DISAGREES with it (e.g. euler says 180°-about-Y while the quat is identity) — the
                            // euler is a stale/display value whose axis order doesn't match ours. Reading it
                            // flipped/tilted every rotated mesh (slanted sand, "gaps"). Use the quat directly.
                            const auto& q = dat["localRotationQuat"];
                            float qx=(float)(q.has("x")?q["x"].asFloat():0), qy=(float)(q.has("y")?q["y"].asFloat():0);
                            float qz=(float)(q.has("z")?q["z"].asFloat():0), qw=(float)(q.has("w")?q["w"].asFloat():1);
                            float L=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw); if(L<1e-8f){qx=qy=qz=0;qw=1;L=1;}
                            draw.transform.rot[0]=qx/L; draw.transform.rot[1]=qy/L; draw.transform.rot[2]=qz/L; draw.transform.rot[3]=qw/L;
                        }
                        else if (dat.has("localRotation")) {
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
                                // localRotation WITHOUT a w = EULER RADIANS → quaternion, EXACTLY as libshell.
                                // From IDA disasm of TransformPlatformComponent.cpp's reader (sub_C99330 →
                                // sub_C3B280, then sub_C3B228 normalises): half-angle euler in **intrinsic ZYX**
                                // order (validated: identity→(0,0,0,1)). The old code used intrinsic XYZ — every
                                // cross-term sign was FLIPPED, which spun the turtles' swim into the home/floor.
                                float hx=rx*0.5f, hy=ry*0.5f, hz=rz*0.5f;
                                float cx=std::cos(hx), sx=std::sin(hx);
                                float cy=std::cos(hy), sy=std::sin(hy);
                                float cz=std::cos(hz), sz=std::sin(hz);
                                draw.transform.rot[0] = sx*cy*cz - cx*sy*sz;
                                draw.transform.rot[1] = cx*sy*cz + sx*cy*sz;
                                draw.transform.rot[2] = cx*cy*sz - sx*sy*cz;
                                draw.transform.rot[3] = cx*cy*cz + sx*sy*sz;
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
                    else if (cls.find("MaterialPropertyOverrides") != std::string::npos) {
                        // Per-instance material overrides. The unlitatlasinstance coral carry
                        // "instance.atlasCellIndex" (value.x = which atlas cell = which colour variant). Without
                        // it every coral sampled cell 0 (one colour: coral_22 was blue, should be pink=cell 21).
                        if (dat.has("constantParameters") && dat["constantParameters"].isArray()) {
                            const auto& cps = dat["constantParameters"];
                            for (size_t ci = 0; ci < cps.size(); ++ci) {
                                const auto& cp = cps[ci].has("data") ? cps[ci]["data"] : tinyjson::Value();
                                if (!cp.has("parameterName") || !cp.has("value") || !cp["value"].has("x")) continue;
                                const std::string pn = cp["parameterName"].asString();
                                const float vx = (float)cp["value"]["x"].asFloat();
                                // VAT per-instance "instance" UBO {atlasCellIndex,animTrackIndex,animRateFactor,
                                // animTimeOffset} — each creature swims its OWN track/phase (else all snap to
                                // track 0 in sync → wrong pose → clip). Reversed from vatunlit* shader set2 bind1.
                                if      (pn.find("atlasCellIndex") != std::string::npos) draw.atlasCellIndex = vx;
                                else if (pn.find("animTrackIndex") != std::string::npos) draw.vatTrackIndex  = vx;
                                else if (pn.find("animRateFactor") != std::string::npos) draw.vatRateFactor  = vx;
                                else if (pn.find("animTimeOffset") != std::string::npos) draw.vatTimeOffset  = vx;
                                // Capture the FULL override (xyzw) for the renderer to apply over the cooked
                                // material block: matParams.tint (per-instance colour), matParams.windIntensityMax/
                                // ampMax/windSpeedMax/leafAmt/windDir (plant sway), RoughnessMultiplier/NormalGain,
                                // etc. Without these, grey butterflies stay grey + plants don't sway as authored.
                                MatOverride ov; ov.name = pn;
                                ov.v[0] = vx;
                                ov.v[1] = cp["value"].has("y") ? (float)cp["value"]["y"].asFloat() : 0.0f;
                                ov.v[2] = cp["value"].has("z") ? (float)cp["value"]["z"].asFloat() : 0.0f;
                                ov.v[3] = cp["value"].has("w") ? (float)cp["value"]["w"].asFloat() : 0.0f;
                                draw.matOverrides.push_back(ov);
                                if (verbose) fprintf(stderr, "[HSTF]     override %s = (%.3f,%.3f,%.3f,%.3f)\n",
                                    pn.c_str(), ov.v[0], ov.v[1], ov.v[2], ov.v[3]);
                            }
                        }
                    }
                    else if (cls.find("SkyboxPlatformComponent") != std::string::npos) {
                        // The sky dome: its geometry is `skyboxMesh` and its texture is `colorTexture` — NOT a
                        // MeshPlatformComponent, so without this the skybox entity was dropped (hasMesh=false) =
                        // "No skybox support". Extract them so the dome renders with the panorama.
                        auto readRef = [&](const char* k) -> AssetKey {
                            if (!dat.has(k)) return {}; const auto& m = dat[k];
                            return { (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                     (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                     (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0) };
                        };
                        AssetKey sm = readRef("skyboxMesh");
                        if (sm.ing != 0) {
                            draw.meshRef = sm; draw.skyboxTex = readRef("colorTexture"); draw.isSkybox = true; hasMesh = true;
                            if (verbose) fprintf(stderr, "[HSTF]     SKYBOX mesh ing=%016llX colorTex ing=%016llX\n",
                                (unsigned long long)sm.ing, (unsigned long long)draw.skyboxTex.ing);
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
