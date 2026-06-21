// ── scene_items.h — editor-authored scene entities that cook to haven2025's REAL components ─────────────────
// One struct for every placeable thing haven (home_c25) offers — spawn points, chairs (AvatarSitting), box/path
// colliders, a navmesh baked from selected meshes, wall-placement zones, locomotion hotspots — each addable /
// removable / positionable in the editor and serialized to the exact haven HSTF component JSON at cook time.
// Format reverse-engineered from haven2025 scene.zip (see project_hsr_haven_editor_gizmos memory). Shared by the
// editor (ui) and the cooker (cook); pure data + JSON, no UI/Vulkan.
#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

namespace sitem {

enum Type { SPAWN=0, CHAIR=1, BOXCOL=2, NAVMESH=3, WALLPLACE=4, HOTSPOT=5, BOUNDARY=6, TYPE_COUNT=7 };

inline const char* typeName(int t) {
    switch (t) { case SPAWN:return "Spawn Point"; case CHAIR:return "Chair / Seat"; case BOXCOL:return "Box Collider";
                 case NAVMESH:return "Navmesh"; case WALLPLACE:return "Wall Placement"; case HOTSPOT:return "Locomotion Hotspot";
                 case BOUNDARY:return "Kill Floor / Boundary"; }
    return "Item";
}
// The REAL Meta/Horizon component class the item cooks to (shown in the Add menu so you know exactly what it is).
inline const char* metaName(int t) {
    switch (t) { case SPAWN:return "SpawnPointPlatformComponent"; case CHAIR:return "AvatarSittingPlatformComponent";
                 case BOXCOL:return "ColliderBoxPlatformComponent"; case NAVMESH:return "ColliderMeshPlatformComponent";
                 case WALLPLACE:return "hpi::WallPlacementComponent"; case HOTSPOT:return "hpi::LocomotionHotspotComponent";
                 case BOUNDARY:return "ColliderBoundaryPlanePlatformComponent"; }
    return "";
}
// horizon::physics::UnitAxis enum (IDA V205 sub_247082C) — the boundary plane's facing/normal. Serialized as a STRING.
inline const char* unitAxisName(int a) {
    switch (a) { case 0:return "PositiveX"; case 1:return "NegativeX"; case 2:return "PositiveY"; case 3:return "NegativeY";
                 case 4:return "PositiveZ"; case 5:return "NegativeZ"; }
    return "PositiveY";
}

struct Item {
    int   type = SPAWN;
    std::string name;
    bool  hidden = false;          // per-item visibility eye (hides this item's marker + gizmo in the editor)
    float pos[3]   = {0,0,0};       // world position
    float rot[3]   = {0,0,0};       // EULER DEGREES (UI); cooked as radians
    float scale[3] = {1,1,1};
    // SPAWN
    bool  allowStart = true;        // the local-start spawn (>=1 required)
    bool  isLocal    = true;        // tags:["local"] vs ["remote"]
    // CHAIR
    float exitPos[3] = {0, 0, -0.45f};   // playerExitPosition (stand-up spot), relative to the seat
    // BOXCOL (path blocker / invisible wall)
    float half[3]    = {0.5f, 0.5f, 0.5f};   // halfExtents
    float quat[4]    = {0,0,0,0};            // optional AUTHORITATIVE localRotationQuat (tilted nav boxes); all-zero = unused
    // WALLPLACE
    float propW = 1.8f, propH = 1.125f;      // propMaxWidth / propMaxHeight
    // BOUNDARY (kill floor): the plane's facing = horizon::physics::UnitAxis (default PositiveY = a floor normal pointing up)
    int axis = 2;                            // 2 = PositiveY
    // NAVMESH — baked from these editor mesh indices (the cook turns their triangles into a ColliderMesh)
    int navMode = 2;                  // 0=flat plane, 1=smart (up-facing tris only), 2=from selection (all tris)
    std::vector<int> srcMeshes;
    std::vector<float>    navVerts;   // world-space verts (the previewed/cooked navmesh geometry)
    std::vector<uint32_t> navIdx;     // triangle indices
};

// ── JSON helpers ──
inline std::string f(float v) { char b[32]; snprintf(b, sizeof b, "%.6g", v); return b; }
inline std::string vec3(const char* k, const float* v) {
    return std::string("\"")+k+"\":{\"x\":"+f(v[0])+",\"y\":"+f(v[1])+",\"z\":"+f(v[2])+"}";
}
inline std::string uuid(int idx) { char b[48]; snprintf(b, sizeof b, "ed170000-0000-4000-8000-%012x", idx); return b; }
inline std::string comp(const char* cls, int ver, const std::string& data) {
    return std::string("{\"data\":{\"class\":\"")+cls+"\",\"version\":"+std::to_string(ver)+",\"data\":{"+data+
           "}},\"dataType\":\"horizon::DataDefinitionAsset\"}";
}
inline std::string transformComp(const Item& it) {   // localRotation = EULER RADIANS (haven convention)
    float r[3] = { it.rot[0]*0.01745329f, it.rot[1]*0.01745329f, it.rot[2]*0.01745329f };
    std::string data = vec3("localPosition", it.pos)+","+vec3("localRotation", r)+","+vec3("localScale", it.scale);
    if (it.quat[0]||it.quat[1]||it.quat[2]||it.quat[3]) {   // tilted nav collider: emit the AUTHORITATIVE quaternion
        char qb[160]; snprintf(qb,sizeof qb,",\"localRotationQuat\":{\"x\":%.6g,\"y\":%.6g,\"z\":%.6g,\"w\":%.6g}",
            it.quat[0],it.quat[1],it.quat[2],it.quat[3]);
        data += qb;
    }
    return comp("horizon::platform_api::TransformPlatformComponent", 2, data);
}

// Entity JSON for one item. `meshAssetJson` (for NAVMESH) = the cook-provided {"packageOrRemoteId":..} ref string
// of the baked ColliderMesh; empty for the other types. Returns "" if the type needs an asset it wasn't given.
inline std::string itemEntityJson(const Item& it, int idx, const std::string& meshAssetJson = "") {
    std::string comps, nm = it.name.empty() ? typeName(it.type) : it.name;
    switch (it.type) {
        case SPAWN:
            comps = comp("horizon::platform_api::SpawnPointPlatformComponent", 1,
                std::string("\"stateEnabled\":true,\"allowStart\":")+(it.allowStart?"true":"false")+
                ",\"teleportOnSublevelLoad\":false,\"tags\":[\""+(it.isLocal?"local":"remote")+"\"]")
                + "," + transformComp(it);
            break;
        case CHAIR: {
            // EXACT haven2025 working-chair recipe (extracted from home_3d_staticarch_shell template): a sittable seat is
            // 5 components, NO render mesh — an INVISIBLE ColliderSphere is the gaze/interaction target. The old bare
            // AvatarSitting v1 was silently DROPPED (wrong version) -> chair never appeared. Versions are load-bearing.
            float radius = (it.half[0] > 0.65f) ? it.half[0] : 1.0f;     // sit-trigger/gaze sphere (haven 0.6 @ scale2.4; ours scale1 -> 1.0 for easy ray-hit)
            // The HomeLocomotionSystem chair-button (libshell sub_12BB068) does strstr(locator,"chair_location") -> the
            // locator name MUST contain "chair_location" or the seat is NOT interactable at all (DEVICE-PROVEN: a plain
            // locator = no chair-button = you can't sit/interact; AvatarSitting alone is NOT sittable without the button).
            // The chair-button only WARPS you to the ChairIcon (no real sit -> the playerExitPosition exit never runs);
            // teleport-to-a-destination = a LocomotionHotspot, not a chair.
            std::string loc = std::string("\"locator\":\"chair_location_") + std::to_string(idx) + "\"";
            std::string sph = std::string("\"collisionLayer\":\"None\",\"radius\":") + f(radius);
            // EXIT (stand-up) TELEPORT — IDA-PROVEN at the APPLICATION (CharacterSittingStates sit-state onExit
            // `sub_1445C9C`): it reads the AvatarSitting component and writes the exit transform straight to the player's
            // pending-transform component (`sub_12B9270(player)`), NO navmesh clamp in the path. THE GATE: the whole
            // exit-teleport block only runs when **allowExitThroughLocomotion(@109) == FALSE** (true SKIPS it -> you just
            // stand in place; my earlier true-flag cook was exactly wrong). Then it branches on useRelativeExitLocation(@176):
            //   false -> ABSOLUTE: player.pos = playerExitPosition(@112) directly (player.rot = playerExitRotation@128).
            //   true  -> seat-local compose of relativePlayerExitPosition(@144).
            // We use the ABSOLUTE branch = a direct world write (simplest, no seat-transform dependency). editor exitPos =
            // world delta from the seat, so playerExitPosition = seat + exitPos.
            float wexit[3] = { it.pos[0]+it.exitPos[0], it.pos[1]+it.exitPos[1], it.pos[2]+it.exitPos[2] };
            // playerExitRotation MUST be a valid quaternion: sub_1445C9C's absolute branch writes v23+128 (this field) as
            // the player's exit ROTATION alongside the position. Left unset it's (0,0,0,0) = a zero quat -> the locomotion
            // likely rejects the whole transform -> you stand up at the seat. Emit the seat's facing (identity here; the
            // engine forces position.w=1.0 already). seat euler->quat = intrinsic-ZYX (sub_C3B280, shared w/ hstf_parser).
            float erx=it.rot[0]*0.01745329f, ery=it.rot[1]*0.01745329f, erz=it.rot[2]*0.01745329f;
            float ehx=erx*0.5f,ehy=ery*0.5f,ehz=erz*0.5f,ecx=std::cos(ehx),esx=std::sin(ehx),ecy=std::cos(ehy),esy=std::sin(ehy),ecz=std::cos(ehz),esz=std::sin(ehz);
            float eqx=esx*ecy*ecz-ecx*esy*esz, eqy=ecx*esy*ecz+esx*ecy*esz, eqz=ecx*ecy*esz-esx*esy*ecz, eqw=ecx*ecy*ecz+esx*esy*esz;
            char erb[160]; snprintf(erb,sizeof erb,",\"playerExitRotation\":{\"x\":%.6g,\"y\":%.6g,\"z\":%.6g,\"w\":%.6g}",eqx,eqy,eqz,eqw);
            std::string sit = vec3("playerExitPosition", wexit) + erb
                + ",\"useRelativeExitLocation\":false,\"allowExitThroughLocomotion\":false";
            comps = transformComp(it)
                + "," + comp("horizon::platform_api::LocatorPlatformComponent", 1, loc)
                + "," + comp("horizon::platform_api::ColliderSpherePlatformComponent", 2, sph)
                + "," + comp("horizon::platform_api::PhysicsBodyPlatformComponent", 6, "\"type\":\"StaticCollision\"")
                + "," + comp("horizon::platform_api::AvatarSittingPlatformComponent", 7, sit);
            break;
        }
        case BOXCOL:
            comps = comp("horizon::platform_api::ColliderBoxPlatformComponent", 1, vec3("halfExtents", it.half))
                + "," + comp("horizon::platform_api::PhysicsBodyPlatformComponent", 6, "\"type\":\"StaticCollision\"")
                + "," + transformComp(it);
            break;
        case NAVMESH:
            if (meshAssetJson.empty()) return "";   // the cook bakes the ColliderMesh + supplies the ref
            // VERSIONS MATCH haven2025's HomeFGNavmesh_Mesh (extracted): ColliderMesh v2 + PhysicsBody v7. The old v1/v6
            // are silently dropped by the device deserializer -> no walkable collision -> "can't walk at all".
            comps = comp("horizon::platform_api::ColliderMeshPlatformComponent", 2, std::string("\"meshAsset\":")+meshAssetJson)
                + "," + comp("horizon::platform_api::PhysicsBodyPlatformComponent", 7, "\"type\":\"StaticCollision\"")
                + "," + transformComp(it);
            break;
        case WALLPLACE:
            comps = comp("horizon::hpi::WallPlacementComponent", 1,
                std::string("\"propRank\":0,\"propMaxWidth\":")+f(it.propW)+",\"propMaxHeight\":"+f(it.propH))
                + "," + transformComp(it);
            break;
        case HOTSPOT:
            // haven2025 "hotspot_dot" recipe: VISIBLE Mesh (the teleport dot) + LocomotionHotspot + a Layer30
            // ColliderSphere trigger + a Trigger PhysicsBody. The cook passes the icon-mesh ref as meshAssetJson.
            comps = transformComp(it)
                + (meshAssetJson.empty() ? std::string() : ("," + comp("horizon::platform_api::MeshPlatformComponent", 5, std::string("\"mesh\":")+meshAssetJson+",\"isVisibleSelf\":true")))
                + "," + comp("horizon::hpi::LocomotionHotspotComponent", 1, "\"propSnapRadius\":1.0")
                + "," + comp("horizon::platform_api::ColliderSpherePlatformComponent", 2, "\"collisionLayer\":\"Layer30\"")
                + "," + comp("horizon::platform_api::PhysicsBodyPlatformComponent", 1, "\"type\":\"Trigger\"");
            break;
        case BOUNDARY:   // ColliderBoundaryPlanePlatformComponent — only `direction` (UnitAxis string) is set; libshell
                         // defaults collisionLayer/friction/materialAsset. Transform localPosition.y = the kill height.
            comps = comp("horizon::platform_api::ColliderBoundaryPlanePlatformComponent", 1,
                std::string("\"direction\":\"")+unitAxisName(it.axis)+"\"")
                + "," + comp("horizon::platform_api::PhysicsBodyPlatformComponent", 6, "\"type\":\"StaticCollision\"")
                + "," + transformComp(it);
            break;
        default: return "";
    }
    return std::string("{\"id\":\"")+uuid(idx)+"\",\"name\":\""+nm+"\",\"components\":["+comps+"],\"attributes\":[]}";
}

} // namespace sitem
