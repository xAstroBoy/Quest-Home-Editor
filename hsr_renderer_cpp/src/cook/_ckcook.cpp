// hsl_cook — self-contained V203/HSL APK cooker (CLI driver).
// Cooks a custom checker-floor home into a bootable Quest APK using ONLY the C++ encoders in hsl_cooker.h
// (RENDMESH/RENDTXTR-ASTC/RENDSHAD/MATL/HSTF/ASMH) + miniz (zip) + astcenc (ASTC encode). The only input
// borrowed from a real env is the Nuxd APK *shell* (AndroidManifest/resources/native libs); every scene
// asset is generated here. This is the same code path the editor's "Export APK" calls.
//   usage: hsl_cook [Nuxd.apk] [out.apk] [shadersDir]
#include "cook/hsl_cooker.h"
#include "io/gltf_import.h"     // headless --cookglb repro: raw .glb/.gltf import -> exportSceneAPK
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>

static std::vector<uint8_t> readFile(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) return {};
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(n > 0 ? n : 0); if (n > 0) { size_t r = fread(b.data(), 1, n, f); b.resize(r); } fclose(f); return b;
}

// Exercise the multi-mesh export path (encodeRendMeshParts + exportSceneAPK), incl. a >65535-vert mesh that
// forces the part splitter. Validated separately with the python decoders.
static int exportTest(int argc, char** argv) {
    using namespace hslcook;
    std::string nuxd  = argc > 2 ? argv[2] : "Nuxd.apk";
    std::string out   = argc > 3 ? argv[3] : "cooker/out/export_test_unsigned.apk";
    std::string shdir = argc > 4 ? argv[4] : "cooker/shaders";
    auto rd = [](const std::string& p){ std::vector<uint8_t> b; FILE* f=fopen(p.c_str(),"rb"); if(!f) return b;
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ b.resize(n); size_t r=fread(b.data(),1,n,f); b.resize(r);} fclose(f); return b; };
    auto vspv = rd(shdir + "/myunlit.vert.spv"), fspv = rd(shdir + "/myunlit.frag.spv");
    if (vspv.empty() || fspv.empty()) { fprintf(stderr, "[export-test] missing shaders in %s\n", shdir.c_str()); return 1; }

    std::vector<ExportMesh> ems;
    // mesh 0: small quad
    { ExportMesh m; m.name = "quadA";
      m.positions = { -2,0,-2,  2,0,-2,  2,0,2,  -2,0,2 };
      m.uvs       = { 0,0, 1,0, 1,1, 0,1 };
      m.indices   = { 0,2,1, 0,3,2 };
      ems.push_back(std::move(m)); }
    // mesh 1: a GxG vertex grid -> >65535 verts -> forces multi-part RENDMESH
    { const int G = 260; ExportMesh m; m.name = "bigGrid";
      m.positions.reserve((size_t)G*G*3); m.uvs.reserve((size_t)G*G*2);
      for (int z=0; z<G; z++) for (int x=0; x<G; x++) {
          m.positions.push_back((float)x*0.1f); m.positions.push_back(5.f); m.positions.push_back((float)z*0.1f);
          m.uvs.push_back((float)x/(G-1)); m.uvs.push_back((float)z/(G-1)); }
      for (int z=0; z<G-1; z++) for (int x=0; x<G-1; x++) {
          uint32_t a=z*G+x, b=z*G+x+1, c=(z+1)*G+x, d=(z+1)*G+x+1;
          m.indices.insert(m.indices.end(), { a,c,b,  b,c,d }); }
      ems.push_back(std::move(m)); }

    bool ok = false;
    auto apk = exportSceneAPK(ems, nuxd, vspv, fspv, true, &ok);
    if (!ok || apk.empty()) { fprintf(stderr, "[export-test] cook failed (shell: %s)\n", nuxd.c_str()); return 1; }
    FILE* f = fopen(out.c_str(), "wb"); if (!f) { fprintf(stderr, "[export-test] cannot write %s\n", out.c_str()); return 1; }
    fwrite(apk.data(), 1, apk.size(), f); fclose(f);
    printf("EXPORT-TEST: %zu meshes (incl. %u-vert grid) -> apk=%zuB -> %s\n", ems.size(), 260u*260u, apk.size(), out.c_str());
    return 0;
}

// --remanifest <in.apk> <out_unsigned.apk> <newPkg> — re-splice an already-cooked APK through the (fixed) spliceAPK:
// keeps its scene.zip byte-for-byte, regenerates the AndroidManifest (haven2025 base, hsr_package_type flipped
// footprint->combined so the env loads as a standalone Environment instead of a footprint overlay). Lets us apply the
// classification fix to existing APKs without re-running the full scene export — a clean A/B (only the manifest changes).
static int remanifest(int argc, char** argv) {
    using namespace hslcook;
    if (argc < 5) { fprintf(stderr, "usage: hsl_cook --remanifest <in.apk> <out_unsigned.apk> <newPkg>\n"); return 2; }
    std::string in = argv[2], out = argv[3], newPkg = argv[4];
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, in.c_str(), 0)) { fprintf(stderr, "[remanifest] cannot open %s\n", in.c_str()); return 1; }
    int idx = mz_zip_reader_locate_file(&z, "assets/scene.zip", nullptr, 0);
    if (idx < 0) { fprintf(stderr, "[remanifest] no assets/scene.zip in %s\n", in.c_str()); mz_zip_reader_end(&z); return 1; }
    size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0); mz_zip_reader_end(&z);
    if (!p) { fprintf(stderr, "[remanifest] failed to extract scene.zip\n"); return 1; }
    std::vector<uint8_t> sceneZip((uint8_t*)p, (uint8_t*)p + sz); mz_free(p);
    bool ok = false;
    auto apk = spliceAPK(in, sceneZip, "com.meta.environment.prod.nuxd", newPkg, &ok);  // base=in (keeps libs/res/dex/navmesh)
    if (!ok || apk.empty()) { fprintf(stderr, "[remanifest] splice failed\n"); return 1; }
    FILE* f = fopen(out.c_str(), "wb"); if (!f) { fprintf(stderr, "[remanifest] cannot write %s\n", out.c_str()); return 1; }
    fwrite(apk.data(), 1, apk.size(), f); fclose(f);
    printf("REMANIFEST: %s -> %s (pkg=%s, scene.zip=%zuB, apk=%zuB)\n", in.c_str(), out.c_str(), newPkg.c_str(), sceneZip.size(), apk.size());
    return 0;
}

// --cookglb <in.glb> [nuxd.apk] [out.apk] — HEADLESS repro of the editor's drag-in cook: import the raw glTF/glb
// exactly like the editor (gltfimport::importEnv), feed geometry-only ExportMeshes to exportSceneAPK, and time every
// stage (HSR_COOKTRACE=1 auto-on). The LAST [COOKTRACE] line before a freeze names the hanging stage. This mirrors the
// geometry path that stalls on load ("application unresponsive") without needing the Vulkan editor window.
static int cookGlb(int argc, char** argv) {
    using namespace hslcook;
    if (argc < 3) { fprintf(stderr, "usage: hsl_cook --cookglb <in.glb> [nuxd.apk] [out.apk]\n"); return 2; }
    std::string in = argv[2];
    std::string nuxd = argc > 3 ? argv[3] : "Nuxd.apk";
    std::string out  = argc > 4 ? argv[4] : "cooker/out/cookglb_unsigned.apk";
#ifdef _WIN32
    _putenv_s("HSR_COOKTRACE", "1");
#else
    setenv("HSR_COOKTRACE", "1", 1);
#endif
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* s){ double dt = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        fprintf(stderr, "[CImport] %8.2fs  %s\n", dt, s); fflush(stderr); };
    lap("importing glb");
    std::vector<MeshData> meshes;
    if (!gltfimport::importEnv(in, meshes) || meshes.empty()) { fprintf(stderr, "[cookglb] import failed / empty: %s\n", in.c_str()); return 1; }
    lap(("imported " + std::to_string(meshes.size()) + " meshes").c_str());
    std::vector<ExportMesh> ems; ems.reserve(meshes.size());
    size_t totV=0, totT=0;
    for (auto& md : meshes) {
        ExportMesh em; em.name = md.name;
        em.positions = md.positions; em.uvs = md.uvs; em.indices = md.indices;
        if (md.hasTexture) { em.rgba = md.texRGBA; }
        totV += em.positions.size()/3; totT += em.indices.size()/3;
        ems.push_back(std::move(em));
    }
    lap(("built ExportMeshes (" + std::to_string(totV) + " verts, " + std::to_string(totT) + " tris)").c_str());
    bool ok=false;
    auto progress = [](float f, const char* s){ fprintf(stderr, "[prog %3.0f%%] %s\n", f*100.f, s); fflush(stderr); };
    auto apk = exportSceneAPK(ems, nuxd, {}, {}, true, &ok, nullptr, nullptr, {}, progress);
    lap(ok ? "exportSceneAPK OK" : "exportSceneAPK FAILED");
    if (!ok || apk.empty()) { fprintf(stderr, "[cookglb] cook failed\n"); return 1; }
    FILE* f = fopen(out.c_str(), "wb"); if (!f) { fprintf(stderr, "[cookglb] cannot write %s\n", out.c_str()); return 1; }
    fwrite(apk.data(), 1, apk.size(), f); fclose(f);
    printf("COOKGLB OK: %zu meshes -> apk=%zuB -> %s\n", ems.size(), apk.size(), out.c_str());
    return 0;
}

int main(int argc, char** argv) {
    using namespace hslcook;
    if (argc > 1 && std::string(argv[1]) == "--export-test") return exportTest(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--remanifest") return remanifest(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--cookglb") return cookGlb(argc, argv);
    std::string nuxd  = argc > 1 ? argv[1] : "Nuxd.apk";
    std::string out   = argc > 2 ? argv[2] : "cooker/out/myhome_cpp_unsigned.apk";
    std::string shdir = argc > 3 ? argv[3] : "cooker/shaders";

    // ── floor geometry (20m, UV tiled x2) + checker albedo (256², 32px squares) ──
    float h = 20.f, t = 2.f;
    std::vector<float> P = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
    std::vector<float> U = { 0,0,  t,0,  t,t,  0,t };
    std::vector<uint16_t> I = { 0,2,1, 0,3,2 };
    int N = 256, sq = 32; std::vector<uint8_t> img((size_t)N * N * 4);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        uint8_t* px = &img[((size_t)y * N + x) * 4];
        bool m = ((x / sq + y / sq) & 1);
        px[0] = m ? 220 : 235; px[1] = m ? 40 : 235; px[2] = m ? 40 : 235; px[3] = 255;
    }

    // ── unlit SPIR-V (vert+frag) — shipped with the cooker ──
    auto vspv = readFile((shdir + "/myunlit.vert.spv").c_str());
    auto fspv = readFile((shdir + "/myunlit.frag.spv").c_str());
    if (vspv.empty() || fspv.empty()) { fprintf(stderr, "[cook] ERROR: missing myunlit.vert.spv/myunlit.frag.spv in %s\n", shdir.c_str()); return 1; }

    // ── content paths (relative to content/) + their hash-based AssetRefs (keyForPath = field2/3/4 consistent) ──
    CookRng rng;
    // dir/rel reuse nuxd's known-hash strings (real u64 StringId), sub-names use murmur3 (cracked) — so the
    // AssetRefs match whether the device resolves via the manifest maps OR scans the files. [[ondevice]]
    std::string pMesh = "meta/nux/nux_d/tx_mote_c_05.png/mesh", pTex = "meta/nux/nux_d/tx_dome_a_03.png/tex",
                pShad = "meta/nux/shaders/unlit.surface/shader", pMat = "meta/nux/nux_d/unlit_floor_x_12_inner_01.material/material",
                pContent = "meta/nux/nux_d/nux_d.hstf/template", pSpace = "meta/nux/space.hstf/template";
    AssetKey3 meshK = keyForPath(pMesh), texK = keyForPath(pTex), shdK = keyForPath(pShad),
              matK = keyForPath(pMat), contentK = keyForPath(pContent), spaceK = keyForPath(pSpace);

    // ── REAL floor: nuxd's *verified* floor mesh + material (our hand-built RENDMESH fails the device's strict
    //    FlatBuffer verifier `MeshDefinition::fix()`; the real mesh passes). We patch the texture AssetRefs — in
    //    BOTH the mesh's embedded material (@360 pkg/@368 ing) and the standalone material (@120/@128) — to OUR
    //    checker, and ship the unlit.surface shader the material binds. So: real floor GEOMETRY + OUR texture. ──
    auto mesh = readFile("cooker/realfloor_mesh.bin");
    auto matl = readFile("cooker/realfloor_mat.bin");
    auto phys = readFile("cooker/realfloor_phys.bin");           // PHSX:3MSH collision mesh -> walkable floor
    auto tex  = encodeRendTxtr(img.data(), N, N, 8, 8);
    if (mesh.empty() || matl.size() < 176 || tex.empty() || phys.empty()) { fprintf(stderr, "[cook] ERROR: missing realfloor_mesh/mat/phys.bin or ASTC failed\n"); return 1; }
    std::string pPhys = "meta/nux/nux_d/floor.phys/phys"; AssetKey3 colliderK = keyForPath(pPhys);
    if (mesh.size() > 376) { memcpy(mesh.data() + 360, &texK.pkg, 8); memcpy(mesh.data() + 368, &texK.ing, 8); }   // embedded-material tex -> ours
    memcpy(matl.data() + 120, &texK.pkg, 8); memcpy(matl.data() + 128, &texK.ing, 8);                              // standalone-material tex -> ours
    if (getenv("HSR_MYMESH")) mesh = encodeRendMesh(P, U, I, matl);   // TEST our OWN RENDMESH encoder (struct-bounds fix); embeds the patched material
    // the material binds the renderer_module unlit.surface shader by this exact AssetRef — ship it there:
    AssetKey3 shaderK = { 0x608B25CE5424598Dull, 0x78686234E7611EFCull, 0xA1767FE9u };
    std::string pShaderRM = "meta/renderer_module/shaders/unlit.surface/shader";
    auto shad = readFile("cooker/nuxd_unlit_shader.bin");
    if (shad.empty()) { fprintf(stderr, "[cook] ERROR: missing cooker/nuxd_unlit_shader.bin\n"); return 1; }
    (void)P; (void)U; (void)I; (void)vspv; (void)fspv;

    // ── HSTF scene graph: content template (the floor entity) + space template (firstWorldAsset) ──
    float pos[3] = {0,0,0}, rot[3] = {0,0,0}, scl[3] = {1,1,1};
    std::string rootId  = makeUuid(rng), floorId = makeUuid(rng), spawnId = makeUuid(rng);
    std::string ent     = entityJson(floorId, "CustomFloor", pos, rot, scl, meshK, { matK }, colliderK);
    float spawnPos[3]   = { 0.f, 0.01f, 0.f };                                  // on the floor
    std::string spawn   = spawnPointEntityJson(spawnId, spawnPos);
    std::string root    = rootEntityJson(rootId);
    std::string rels    = relChildOf(floorId, rootId) + "," + relChildOf(spawnId, rootId);
    std::string content = templateJson(root + "," + ent + "," + spawn, rels);
    std::string space   = spaceJson(rng, "myhome", contentK, 40000.f);  // far clip plane in space.hstf (vista-proven)
    auto shellcfg = jbytes(shellConfigJson(spaceK, /*locomotion*/true));

    // ── manifest paths (relative to content/) ──
    std::vector<CookAsset> assets = {
        { pMesh,     meshK.tgt,     mesh,            meshK },
        { pTex,      texK.tgt,      tex,             texK },
        { pShaderRM, shaderK.tgt,   shad,            shaderK },   // unlit.surface shader the floor material binds
        { pPhys,     colliderK.tgt, phys,            colliderK, TYPE_PHSX, TYPE_3MSH },  // collision mesh (walkable)
        { pMat,      matK.tgt,      matl,            matK },
        { pContent,  contentK.tgt,  jbytes(content), contentK },
        { pSpace,    spaceK.tgt,    jbytes(space),   spaceK },
    };

    auto sceneZip = assembleSceneZip(assets, shellcfg);
    bool ok = false;
    auto apk = spliceAPK(nuxd, sceneZip, "com.meta.environment.prod.nuxd", "com.environment.outerwilds", &ok);
    if (!ok || apk.empty()) { fprintf(stderr, "[cook] ERROR: APK splice failed (base shell: %s)\n", nuxd.c_str()); return 1; }

    FILE* f = fopen(out.c_str(), "wb");
    if (!f) { fprintf(stderr, "[cook] ERROR: cannot write %s\n", out.c_str()); return 1; }
    fwrite(apk.data(), 1, apk.size(), f); fclose(f);

    printf("COOKED (C++ self-contained, only the APK shell from Nuxd):\n");
    printf("  mesh=%zuB  tex=%zuB(ASTC 8x8)  shad=%zuB  matl=%zuB\n", mesh.size(), tex.size(), shad.size(), matl.size());
    printf("  scene.zip=%zuB  ->  apk=%zuB  ->  %s\n", sceneZip.size(), apk.size(), out.c_str());
    printf("  (UNSIGNED — sign with apksigner, or the built-in v2 signer once enabled)\n");
    return 0;
}
