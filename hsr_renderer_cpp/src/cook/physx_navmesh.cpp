// ── physx_navmesh.cpp — the navmesh/collider cook, compiled straight INTO the editor (links the vendored PhysX) ──
// The ONE TU that includes PhysX. cookNavmeshSEBD(): cookTriangleMesh(BVH33) -> PxTriangleMesh -> PxCollection ->
// serializeCollectionToBinary -> the exact `SEBD`+GUID 77E92B17... bytes (platform tag patched to ANDR for Quest).
#include "cook/physx_navmesh.h"

// PhysX is OPTIONAL (-DHSR_HAVE_PHYSX=ON + the vendored Windows PhysX libs). It is OFF by default because the cooked
// PhysX SEBD trimesh is binary-incompatible with the device's PhysX (crashes its BVH33 narrow-phase); the real,
// device-proven walkable collision is the ColliderBox grid (navmeshToBoxes / navmeshTrisToBoxes), which needs NO
// cooked data. With PhysX off, cookNavmeshSEBD() returns {} and every caller already falls back to ColliderBox.
// Keeping it off makes the whole build PhysX-free → it compiles on Linux/macOS, not just Windows.
#ifdef HSR_HAVE_PHYSX
#include "PxPhysicsAPI.h"
using namespace physx;

std::vector<uint8_t> cookNavmeshSEBD(const float* verts, uint32_t nv, const uint32_t* idx, uint32_t nt) {
    if (nv < 3 || nt < 1 || !verts || !idx) return {};
    static PxDefaultAllocator gAlloc; static PxDefaultErrorCallback gErr;
    PxFoundation* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, gAlloc, gErr);
    if (!foundation) return {};
    PxPhysics* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false, nullptr);
    std::vector<uint8_t> out;
    if (physics) {
        PxCookingParams cp(physics->getTolerancesScale());
        cp.midphaseDesc.setToDefault(PxMeshMidPhase::eBVH33);   // MATCH haven2025's realfloor (concrete type @84=BVH33);
        cp.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;   // the device's narrow-phase uses BVH33, not BVH34
        if (PxCooking* cooking = PxCreateCooking(PX_PHYSICS_VERSION, *foundation, cp)) {
            PxTriangleMeshDesc desc;
            desc.points.count = nv; desc.points.stride = 12; desc.points.data = verts;
            desc.triangles.count = nt; desc.triangles.stride = 12; desc.triangles.data = idx;
            PxDefaultMemoryOutputStream cooked;
            if (cooking->cookTriangleMesh(desc, cooked)) {
                PxDefaultMemoryInputData cookedIn(cooked.getData(), cooked.getSize());
                if (PxTriangleMesh* triMesh = physics->createTriangleMesh(cookedIn)) {
                    // CRITICAL: add the mesh WITH a serial object id so it lands in the binary MANIFEST (export count=1,
                    // like Meta's realfloor SEBD @44). With an empty manifest the device resolves the ColliderMesh to NO
                    // shape -> fall-through + narrow-phase null-deref crash. (createSerialObjectIds alone left @44=0.)
                    PxCollection* col = PxCreateCollection(); col->add(*triMesh, PxSerialObjectId(1));
                    PxSerializationRegistry* sr = PxSerialization::createSerializationRegistry(*physics);
                    PxSerialization::complete(*col, *sr);
                    PxDefaultMemoryOutputStream os;
                    if (PxSerialization::serializeCollectionToBinary(os, *col, *sr)) {
                        out.assign(os.getData(), os.getData() + os.getSize());
                        if (out.size() > 44) { out[40]='A'; out[41]='N'; out[42]='D'; out[43]='R'; }   // platform tag -> Quest (LE64)
                    }
                    sr->release(); col->release(); triMesh->release();
                }
            }
            cooking->release();
        }
        physics->release();
    }
    foundation->release();
    return out;
}
#else
// PhysX disabled (default) → no SEBD; the cooker falls back to the device-compatible ColliderBox grid.
std::vector<uint8_t> cookNavmeshSEBD(const float*, uint32_t, const uint32_t*, uint32_t) { return {}; }
#endif
