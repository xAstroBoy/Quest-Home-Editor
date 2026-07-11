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
#include <cstring>
using namespace physx;

// ── Re-layout a stock-serialized SEBD collection to MATCH Meta's device 4.1.64 padding (DATA-PROVEN vs
//    cooker/realfloor_phys.bin). Stock 4.1.2 packs each ref array right after its count; Meta's deserializer does
//    alignPtr(16) AFTER each count, before the array (manifest@80 not @68, internalPtr@144 not @132). Reading our
//    tight layout with Meta's aligned reader = wrong object offset = "loads, no collision". We parse our stock bytes
//    and re-emit with the 16-byte array alignment + 0x42 marked-padding. Struct layouts are identical (GUID matches),
//    so object/extra data copy verbatim (the serialized vtable ptrs are fixed up on load regardless of value). ──
static std::vector<uint8_t> relayoutSEBDForMeta(const std::vector<uint8_t>& in) {
    if (in.size() < 64) return in;
    auto a16 = [](size_t x){ return (x + 15) & ~size_t(15); };
    auto rd32 = [&](size_t o){ uint32_t v; std::memcpy(&v, &in[o], 4); return v; };
    std::vector<uint8_t> out; out.reserve(in.size() + 512);
    auto pad16 = [&](){ while (out.size() & 15) out.push_back(0x42); };
    auto put = [&](const void* p, size_t n){ const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
    auto put32 = [&](uint32_t v){ put(&v, 4); };
    // --- parse stock `in`: alignPtr(16) BEFORE each count; arrays packed immediately after the count ---
    size_t p = 48; put(in.data(), 48);                                   // SEBD header verbatim
    p = a16(p); uint32_t nbObj = rd32(p); p += 4;
    p = a16(p); uint32_t nbMan = rd32(p); p += 4; const uint8_t* man = &in[p]; p += (size_t)nbMan * 8;
    uint32_t objEnd = rd32(p); p += 4;
    p = a16(p); uint32_t nbImp = rd32(p); p += 4; const uint8_t* imp = &in[p]; p += (size_t)nbImp * 16;
    p = a16(p); uint32_t nbExp = rd32(p); p += 4; const uint8_t* exp = &in[p]; p += (size_t)nbExp * 16;
    p = a16(p); uint32_t nbIPtr = rd32(p); p += 4; const uint8_t* iptr = &in[p]; p += (size_t)nbIPtr * 16;
    uint32_t nbIH16 = rd32(p); p += 4; const uint8_t* ih16 = &in[p]; p += (size_t)nbIH16 * 8;
    auto a128 = [](size_t x){ return (x + 127) & ~size_t(127); };
    size_t objStart = a16(p); const uint8_t* objData = &in[objStart];
    // Extra data: GuRTree::exportExtraData does alignData(128) before the RTree pages. Stock's RTree sits at
    // a128(a16(objEnd)); copy the extra data FROM there (skip stock's own 128-pad) and re-128-align it in our buffer
    // so it lands at the same absolute offset Meta expects (@512, like realfloor). Copying verbatim from a16 shifted
    // the RTree off its 128 boundary -> the device narrow-phase found no triangles -> fall-through.
    size_t stockRTree = a128(a16(objStart + objEnd));
    // --- re-emit with Meta alignment: alignPtr(16) after each count, before every non-empty array ---
    pad16(); put32(nbObj);
    pad16(); put32(nbMan); pad16(); put(man, (size_t)nbMan * 8); put32(objEnd);
    pad16(); put32(nbImp);  if (nbImp)  { pad16(); put(imp,  (size_t)nbImp  * 16); }
    pad16(); put32(nbExp);  if (nbExp)  { pad16(); put(exp,  (size_t)nbExp  * 16); }
    pad16(); put32(nbIPtr); if (nbIPtr) { pad16(); put(iptr, (size_t)nbIPtr * 16); }
             put32(nbIH16); if (nbIH16) { pad16(); put(ih16, (size_t)nbIH16 *  8); }
    pad16(); size_t myObj = out.size(); put(objData, objEnd);
    // Layer 3 (DATA-PROVEN vs realfloor): Meta 4.1.64's RefCountable drops its 4-byte trailing pad (PxBase+RefCountable
    // = 28B, not our 32B), so TriangleMesh mNbVertices/mNbTriangles sit at struct+28/+32, NOT +32/+36. The device reads
    // mNbVertices at +28 = our 0xcd pad = a garbage count -> RTree narrow-phase SIGSEGV. Move the two u32 counts back 4
    // bytes; mVertices(+40) and everything after are 8-aligned and unchanged. (RTreeTriangleMesh = the only obj we cook.)
    if (objEnd >= 44) {
        uint32_t nbV, nbT, zero = 0;
        std::memcpy(&nbV, &out[myObj + 32], 4);
        std::memcpy(&nbT, &out[myObj + 36], 4);
        std::memcpy(&out[myObj + 28], &nbV, 4);
        std::memcpy(&out[myObj + 32], &nbT, 4);
        std::memcpy(&out[myObj + 36], &zero, 4);
    }
    while (out.size() % 128) out.push_back(0x42);                 // RTree pages -> 128-aligned (matches realfloor @512)
    if (stockRTree < in.size()) put(&in[stockRTree], in.size() - stockRTree);
    return out;
}

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
            // DOUBLE-SIDE the collision: the thumbstick-glide character controller sweeps the capsule against the
            // trimesh and CULLS BACKFACES, so wall tris facing away from the player are passable (the floor works
            // because its up-facing front faces meet the downward ground-sweep). Duplicate each tri reversed -> solid
            // from BOTH sides, blocking walls/columns regardless of winding. HSR_NAV1SIDED disables.
            std::vector<uint32_t> dsIdx; const uint32_t* useIdx = idx; uint32_t useNt = nt;
            if (!std::getenv("HSR_NAV1SIDED")) {
                dsIdx.reserve((size_t)nt * 6);
                for (uint32_t t = 0; t < nt; t++) {
                    uint32_t a = idx[t*3], b = idx[t*3+1], c = idx[t*3+2];
                    dsIdx.push_back(a); dsIdx.push_back(b); dsIdx.push_back(c);
                    dsIdx.push_back(a); dsIdx.push_back(c); dsIdx.push_back(b);
                }
                useIdx = dsIdx.data(); useNt = nt * 2;
            }
            PxTriangleMeshDesc desc;
            desc.points.count = nv; desc.points.stride = 12; desc.points.data = verts;
            desc.triangles.count = useNt; desc.triangles.stride = 12; desc.triangles.data = useIdx;
            PxDefaultMemoryOutputStream cooked;
            if (cooking->cookTriangleMesh(desc, cooked)) {
                PxDefaultMemoryInputData cookedIn(cooked.getData(), cooked.getSize());
                if (PxTriangleMesh* triMesh = physics->createTriangleMesh(cookedIn)) {
                    // Add the mesh to the collection. Meta's realfloor has it in the MANIFEST (nbManifest=1) with NO
                    // export reference (nbExport=0). An explicit PxSerialObjectId would emit an extra export section the
                    // device's collection lacks (shifts objData @192 vs realfloor @176); the manifest is populated by
                    // add()+complete() regardless. HSR_SEBD_EXPORT forces the old id-export path if needed.
                    PxCollection* col = PxCreateCollection();
                    if (std::getenv("HSR_SEBD_EXPORT")) col->add(*triMesh, PxSerialObjectId(1));
                    else                                 col->add(*triMesh);
                    PxSerializationRegistry* sr = PxSerialization::createSerializationRegistry(*physics);
                    // HAND-ROLL aid: dump our 4.1.2 binary metadata (every struct's field layout) -> diff baseline vs the
                    // device's 4.1.64 (reversed from cooker/realfloor_phys.bin) to map the per-struct offset deltas.
                    if (const char* mp = std::getenv("HSR_DUMP_META")) { PxDefaultFileOutputStream ms(mp); PxSerialization::dumpBinaryMetaData(ms, *sr); }
                    PxSerialization::complete(*col, *sr);
                    PxDefaultMemoryOutputStream os;
                    if (PxSerialization::serializeCollectionToBinary(os, *col, *sr)) {
                        out.assign(os.getData(), os.getData() + os.getSize());
                        // ── /goal: match Meta's 4.1.64 collection padding (16-align the ref arrays) so the device's
                        //    aligned deserializer reads our manifest/refs at the right offsets = REAL trimesh collision. ──
                        if (!std::getenv("HSR_NO_RELAYOUT")) {
                            std::vector<uint8_t> rl = relayoutSEBDForMeta(out);
                            if (rl.size() > 64) { out.swap(rl); fprintf(stderr, "[COOK] SEBD relayout -> Meta padding (%zu bytes)\n", out.size()); }
                        }
                        // ── ConvX the 4.1.2/x64 SEBD -> the device's 4.1.64/ARM64 layout. src = our dumped metadata,
                        //    dst = the reversed device metadata file (HSR_DST_META). Identity test: point it at our own dump. ──
                        if (const char* dm = std::getenv("HSR_DST_META")) {
                            PxDefaultMemoryOutputStream srcMetaOut; PxSerialization::dumpBinaryMetaData(srcMetaOut, *sr);
                            PxDefaultFileInputData dstMeta(dm);
                            if (dstMeta.getLength() > 0) {
                                PxBinaryConverter* cv = PxSerialization::createBinaryConverter();
                                PxDefaultMemoryInputData srcMeta(srcMetaOut.getData(), srcMetaOut.getSize());
                                if (cv && cv->setMetaData(srcMeta, dstMeta)) {
                                    PxDefaultMemoryInputData srcBin(out.data(), (PxU32)out.size());
                                    PxDefaultMemoryOutputStream conv;
                                    if (cv->convert(srcBin, srcBin.getLength(), conv) && conv.getSize() > 64) {
                                        out.assign(conv.getData(), conv.getData() + conv.getSize());
                                        fprintf(stderr, "[COOK] ConvX -> %u bytes (dst meta %s)\n", conv.getSize(), dm);
                                    } else fprintf(stderr, "[COOK] ConvX convert FAILED\n");
                                } else fprintf(stderr, "[COOK] ConvX setMetaData FAILED\n");
                                if (cv) cv->release();
                            } else fprintf(stderr, "[COOK] ConvX: dst metadata '%s' empty/missing\n", dm);
                        }
                        // DEVICE-PROVEN (3 tests + byte-diff): public PhysX 4.1.2 (release OR checked) can't match Meta's
                        // device build. Native 4.1.2 stamp -> SIGSEGV; 4.1.64 stamp -> loads, no crash, but no collision
                        // (4.1.2 body read with the 4.1.64 layout = wrong offsets). The 4.1.64 stamp at least avoids the
                        // crash. A WORKING trimesh needs Meta's exact 4.1.64 PhysX libs (drop into third_party/physx).
                        if (out.size() > 64) {
                            out[40]='A'; out[41]='N'; out[42]='D'; out[43]='R';   // platform tag -> Quest (LE64)
                            out[5]=0x40; out[44]=0x01; for(int i=52;i<64;i++) out[i]=0x42;   // 4.1.64 + checked markers (no-crash)
                        }
                        // NOTE: byte-diffing haven2025's working SEBD (cooker/realfloor_phys.bin) PROVED the device PhysX is a
                        // CHECKED build (0xCD/0x42 debug fills + a 0x12345678 sentinel @176 + markedPadding throughout) whose
                        // serialized struct layout shifts every field vs our PhysX 4.1.2-RELEASE image. Matching the header is
                        // not enough (the whole body differs). A device-compatible trimesh needs PhysX built with PX_CHECKED.
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
