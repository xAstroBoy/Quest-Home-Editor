// HSR Renderer — C++ replica of libshell.so HSR environment render pipeline
// Uses Vulkan via volk + GLFW, loads ALL assets (shaders, meshes, textures) from APK.
//
// Pipeline: 1:1 match of Meta Horizon Environment System:
//   APK → scene.zip → ASMH → shellconfig → HSTF
//   → RENDSHAD (shader SPIRV from APK) → RENDMESH → MATLMATL → RENDTXTR (ASTC)
//   → Vulkan rendering
//
// Usage: hsr_renderer.exe <apk_path>
//
// Controls: WASD=move, QE=up/down, mouse drag=look, scroll=speed, Esc=quit

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>
#include <cctype>
#include <iostream>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "src/types.h"
#include "src/camera.h"
#include "src/asmh_parser.h"
#include "src/rendmesh_parser.h"
#include "src/rendtxtr_parser.h"
#include "src/matlmatl_parser.h"
#include "src/hstf_parser.h"
#include "src/rendshad_parser.h"
#include "src/universal_shader.h"
#include "src/audio.h"
#include "src/v79_shader.h"
#include "src/vk_renderer.h"
#include "src/scene_loader.h"
#include "src/gltf_loader.h"
#include "src/opa_loader.h"
#include "src/audio.h"
#include "src/editor.h"
#include "miniz.h"
#ifdef _WIN32
#include <windows.h>
#endif

// Global state
static VkRenderer*     g_renderer = nullptr;
static GLFWwindow*     g_window = nullptr;
static double          g_mx = 0, g_my = 0;
static bool            g_mouseDown = false;
static int             g_winW = 1280, g_winH = 720;
// Drag-and-drop reload: dropping an .apk relaunches the editor on that env (inherits env-var params,
// fully reloads shaders/scene/audio). Robust vs. tearing down live Vulkan resources in place.
static std::string     g_dropPath;
static bool            g_doReload = false;
// Editor animation control: when g_animOverride, the loop uses g_animScrub as the anim time (pause/scrub).
static bool            g_animOverride = false;
static float           g_animScrub = 0.0f;
// Click-to-select: a left press+release with <5px movement is a pick (set here, consumed in the loop).
static double          g_pressX = 0, g_pressY = 0, g_clickX = 0, g_clickY = 0;
static bool            g_clickPick = false;
static bool            g_rDown = false, g_rightClick = false;
static double          g_rPressX = 0, g_rPressY = 0, g_rightX = 0, g_rightY = 0;

static void dropCb(GLFWwindow*, int count, const char** paths) {
    if (count > 0 && paths[0]) {
        std::string p = paths[0];
        // accept .apk (and any path — the loader figures out the rest)
        g_dropPath = p; g_doReload = true;
        fprintf(stderr, "[DROP] reload -> %s\n", p.c_str());
    }
}

#ifdef _WIN32
static void relaunchSelf(const std::string& apk) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" \"" + apk + "\"";
    std::vector<char> cmdv(cmd.begin(), cmd.end()); cmdv.push_back(0);
    STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    // lpEnvironment = NULL -> inherit our environment (all HSR_* params carry over)
    if (CreateProcessA(exe, cmdv.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}
#endif

static void updateMeshSelection(int idx) {
    if (!g_renderer || g_renderer->gpuMeshes.empty()) return;
    int n = (int)g_renderer->gpuMeshes.size();
    if (idx < 0) idx = n - 1;
    if (idx >= n) idx = 0;
    g_renderer->selectedMesh = idx;
    auto& gm = g_renderer->gpuMeshes[idx];
    fprintf(stderr, "[SELECT] Mesh[%d/%d] '%s' | %s\n", idx, n-1, gm.name.c_str(), gm.info.c_str());
    std::string title = std::string("[") + std::to_string(idx) + "/" + std::to_string(n-1)
        + "] " + gm.name + "  " + gm.info
        + "  | Tab=next Shift+Tab=prev Esc=deselect F=wire";
    glfwSetWindowTitle(g_window, title.c_str());
}

static void keyCb(GLFWwindow* w, int key, int sc, int act, int mods) {
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard) return;  // editor has focus
    if (key == GLFW_KEY_ESCAPE && act == GLFW_PRESS) {
        if (g_renderer && g_renderer->selectedMesh >= 0) {
            // Deselect first, exit on second Esc
            g_renderer->selectedMesh = -1;
            glfwSetWindowTitle(w, "HSR Renderer [Vulkan] — Tab=select mesh  F=wireframe  Esc=quit");
        } else {
            glfwSetWindowShouldClose(w, GLFW_TRUE);
        }
    }
    if (key == GLFW_KEY_F && act == GLFW_PRESS && g_renderer) {
        g_renderer->wireframe = !g_renderer->wireframe;
        fprintf(stderr, "[MAIN] Wireframe: %s\n", g_renderer->wireframe ? "ON" : "OFF");
    }
    // P = save an in-engine screenshot of exactly what's on screen
    if (key == GLFW_KEY_P && act == GLFW_PRESS && g_renderer) {
        g_renderer->screenshot("hsr_shot.png");
    }
    // Tab = select next mesh, Shift+Tab = select previous
    if (key == GLFW_KEY_TAB && act == GLFW_PRESS && g_renderer) {
        int cur = g_renderer->selectedMesh;
        if (mods & GLFW_MOD_SHIFT)
            updateMeshSelection(cur - 1);
        else
            updateMeshSelection(cur + 1);
    }
    // N = next, B = back (alternate keys for mesh cycling)
    if (key == GLFW_KEY_N && act == GLFW_PRESS && g_renderer)
        updateMeshSelection(g_renderer->selectedMesh + 1);
    if (key == GLFW_KEY_B && act == GLFW_PRESS && g_renderer)
        updateMeshSelection(g_renderer->selectedMesh - 1);
}

static bool uiWantsMouse() {
    if (!ImGui::GetCurrentContext()) return false;
    if (ImGui::GetIO().WantCaptureMouse) return true;
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return true;   // over/dragging the move gizmo
    return false;
}
static bool uiWantsKeyboard() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard; }

static void mouseBtnCb(GLFWwindow* w, int btn, int act, int mods) {
    if (btn == GLFW_MOUSE_BUTTON_LEFT) {
        if (act == GLFW_PRESS) {
            if (uiWantsMouse()) { g_mouseDown = false; return; }   // click started on a panel / gizmo
            g_mouseDown = true;
            glfwGetCursorPos(w, &g_mx, &g_my);
            g_pressX = g_mx; g_pressY = g_my;
        } else if (act == GLFW_RELEASE) {
            bool wasDown = g_mouseDown;
            g_mouseDown = false;
            if (!wasDown) return;                       // press began on UI/gizmo -> not a scene click
            double cx, cy; glfwGetCursorPos(w, &cx, &cy);
            double dx = cx - g_pressX, dy = cy - g_pressY;
            if (dx*dx + dy*dy < 25.0) {                 // moved < 5px => a click (select), not a look-drag
                g_clickPick = true; g_clickX = cx; g_clickY = cy;
            }
        }
    } else if (btn == GLFW_MOUSE_BUTTON_RIGHT) {   // right-click in the 3D view -> object context menu
        if (act == GLFW_PRESS) {
            if (uiWantsMouse()) { g_rDown = false; return; }
            g_rDown = true; glfwGetCursorPos(w, &g_rPressX, &g_rPressY);
        } else if (act == GLFW_RELEASE) {
            bool wasDown = g_rDown; g_rDown = false;
            if (!wasDown) return;
            double cx, cy; glfwGetCursorPos(w, &cx, &cy);
            double dx = cx - g_rPressX, dy = cy - g_rPressY;
            if (dx*dx + dy*dy < 25.0) { g_rightClick = true; g_rightX = cx; g_rightY = cy; }
        }
    }
}

static void cursorCb(GLFWwindow* w, double x, double y) {
    if (uiWantsMouse()) return;
    if (!g_mouseDown || !g_renderer) return;
    g_renderer->cam.rotate((float)(x - g_mx), (float)(y - g_my));
    g_mx = x; g_my = y;
}

static void scrollCb(GLFWwindow* w, double xo, double yo) {
    if (uiWantsMouse()) return;
    // Mousewheel adjusts FLY speed: up = faster, down = slower (1.25x per notch for a wide range).
    if (g_renderer) g_renderer->cam.adjustSpeed(yo > 0 ? 1.25f : 0.8f);
}

static void fbSizeCb(GLFWwindow* w, int w_, int h_) {
    g_winW = w_; g_winH = h_;
    if (g_renderer) g_renderer->framebufferResized = true;
}

static void errorCb(int err, const char* desc) {
    fprintf(stderr, "[GLFW] Err %d: %s\n", err, desc);
}

int main(int argc, char** argv) {
    fprintf(stderr, "========================================================\n");
    fprintf(stderr, " HSR Renderer / Editor — libshell.so Vulkan replica\n");
    fprintf(stderr, " Drag an .apk onto the window to load it\n");
    fprintf(stderr, "========================================================\n\n");
#ifdef _WIN32
    // Console is VISIBLE by default (you need it to read coords/logs while debugging). Only hide it
    // when HSR_HIDE_CONSOLE is explicitly set (GUI-only mode). Also bring it to the foreground.
    if (std::getenv("HSR_HIDE_CONSOLE")) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
    } else {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_SHOW);
    }
#endif

    std::string apkPath;
    if (argc >= 2) {
        apkPath = argv[1];
    } else {
        // No command-line arg (double-clicked / launched bare): NO console, NO stdin. Pop up a small
        // GLFW window and wait for the user to DRAG an environment .apk onto it. Whatever they drop
        // is loaded and rendered from scratch (parse -> meshes -> textures -> shaders -> GPU -> render).
        if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);   // on top so the drop target is easy to hit
        GLFWwindow* dropWin = glfwCreateWindow(760, 300,
            "HSR Renderer   —   drag an environment .apk onto this window", nullptr, nullptr);
        if (!dropWin) { fprintf(stderr, "GLFW window failed\n"); glfwTerminate(); return 1; }
        glfwSetDropCallback(dropWin, dropCb);
        g_doReload = false; g_dropPath.clear();
        // Block until something is dropped (g_doReload set by dropCb) or the window is closed.
        while (!glfwWindowShouldClose(dropWin) && !g_doReload) glfwWaitEvents();
        bool got = g_doReload && !g_dropPath.empty();
        apkPath = g_dropPath;
        g_doReload = false; g_dropPath.clear();   // reset so the live render loop's drop handling is clean
        glfwDestroyWindow(dropWin);
        if (!got) { glfwTerminate(); return 0; }  // closed without dropping anything
    }
    fprintf(stderr, "[MAIN] APK: %s\n\n", apkPath.c_str());

    // ── Step A: Load scene ─────────────────────────────────────
    // Detect format: a raw V79 env ships a *.gltf.ovrscene (glTF 2.0 + ASTC KTX); the new
    // HSR/Haven format ships RENDMESH/MATLMATL/RENDSHAD. Try V79 (glTF) first, else HSR.
    SceneLoader loader;
    loader.verbose = true;
    GltfLoader gltf;
    OpaLoader opa;
    std::vector<MeshData>* sceneMeshes = nullptr;
    bool isV79 = gltf.load(apkPath);
    bool isOpa = false;
    if (isV79) {
        fprintf(stderr, "[MAIN] Detected V79 .gltf.ovrscene env — %zu mesh primitives\n", gltf.meshes.size());
        sceneMeshes = &gltf.meshes;
    } else if ((isOpa = opa.load(apkPath))) {
        // V79 OLD OFFICIAL home: cooked .opa (faithful libshell reflection-format loader)
        fprintf(stderr, "[MAIN] Detected V79 .opa official home — %zu renderable submeshes\n", opa.meshes.size());
        sceneMeshes = &opa.meshes;
    } else {
        if (!loader.load(apkPath)) {
            fprintf(stderr, "\n[MAIN] FATAL: Scene load failed\n");
            return 1;
        }
        sceneMeshes = &loader.meshes;
    }

    // Extract shaders from the manifest (RENDSHAD entries)
    std::vector<SpirvBlob> shaders;
    fprintf(stderr, "\n[MAIN] Searching for shaders in ASMH...\n");

    // Loads all RENDSHAD blobs from an APK's scene.zip into `shaders`
    auto loadShadersFromApk = [&shaders](const std::string& srcPath) {
        mz_zip_archive apkZ;
        memset(&apkZ, 0, sizeof(apkZ));
        if (!mz_zip_reader_init_file(&apkZ, srcPath.c_str(), 0)) return;
        int sceneIdx = mz_zip_reader_locate_file(&apkZ, "assets/scene.zip", nullptr, 0);
        if (sceneIdx < 0) { mz_zip_reader_end(&apkZ); return; }
        size_t szSz = 0;
        void* szD = mz_zip_reader_extract_to_heap(&apkZ, sceneIdx, &szSz, 0);
        mz_zip_reader_end(&apkZ);
        if (!szD) return;

        mz_zip_archive szZ;
        memset(&szZ, 0, sizeof(szZ));
        mz_zip_reader_init_mem(&szZ, szD, szSz, 0);

        // Scan scene.zip for RENDSHAD files — deduplicate by entry index
        {
            u32 totalFiles = mz_zip_reader_get_num_files(&szZ);
            for (u32 i = 0; i < totalFiles; ++i) {
                mz_zip_archive_file_stat fstat;
                if (!mz_zip_reader_file_stat(&szZ, i, &fstat)) continue;
                std::string fname(fstat.m_filename);
                // Match any RENDSHAD file in a "shaders/" dir. The basename is "shader"
                // in some envs (nuxd) but "shader_<suffix>" in others (haven2025 appends
                // a random hash, e.g. "shader_t2w9"). Match on the "/shaders/" path plus
                // a basename beginning with "shader" so it's env-agnostic.
                size_t slash = fname.find_last_of('/');
                std::string base = (slash == std::string::npos) ? fname : fname.substr(slash + 1);
                bool isShader = (fname.find("/shaders/") != std::string::npos &&
                                 base.rfind("shader", 0) == 0);
                if (!isShader) continue;
                size_t fsz = 0;
                void* fd = mz_zip_reader_extract_to_heap(&szZ, i, &fsz, 0);
                if (!fd) continue;
                std::vector<u8> shaderData((u8*)fd, (u8*)fd + fsz);
                std::vector<SpirvBlob> blobs;
                if (parseRendShad(shaderData, blobs)) {
                    fprintf(stderr, "  [SHADER] %s -> %zu blobs\n", fname.c_str(), blobs.size());
                    for (auto& b : blobs) { b.srcName = fname; shaders.push_back(std::move(b)); }
                }
                mz_free(fd);
            }
        }
        mz_zip_reader_end(&szZ);
        free(szD);
    };

    loadShadersFromApk(apkPath);
    // AUTO-DETECT V203/HSL: if the env ships its OWN RENDSHAD (the SHAD per-material shaders), it's a
    // V203 home -> use the per-material path (perMat) by DEFAULT so the STOCK render (no flags) is
    // faithful. V79 sources ship no RENDSHAD and keep the built-in shader. (HSR_NOPERMAT forces off.)
    bool envShippedRendShad = !shaders.empty();

    // The v200+ shared shaders (horizon_shared_shaders / renderer_module) are a SYSTEM
    // library — identical across every new env, NOT owned by any one env. When the input
    // ships no RENDSHAD (a raw V79 .gltf.ovrscene source, which we're backporting INTO the
    // new system), source the shaders from the generic system shader pack, never from a
    // specific env. Resolution order (all env-agnostic):
    //   1. $HSR_SHADER_APK            (explicit system-shader pack / any v200+ env)
    //   2. system_shaders.apk / .zip  next to the cwd or the renderer exe
    if (shaders.empty()) {
        std::vector<std::string> candidates;
        if (const char* e = std::getenv("HSR_SHADER_APK")) candidates.push_back(e);
        candidates.push_back("system_shaders.apk");
        candidates.push_back("system_shaders.zip");
        candidates.push_back("../system_shaders.apk");
        // Double-clicking the EXE sets no HSR_SHADER_APK, so auto-locate the HSR system shader
        // pack (haven2025 / nuxd = the port target for V79) at the usual repo-relative spots,
        // relative to either the repo root or the build dir the exe runs from. This is how the
        // "Render APK (drag here).bat" wires it; we replicate it so double-click also works.
        for (const char* base : { ".", "..", "../..", "../../..", "../../../.." }) {
            candidates.push_back(std::string(base) + "/Working (Current Env)/com_meta_shell_env_footprint_haven2025.apk");
            candidates.push_back(std::string(base) + "/Working (Current Env)/com_meta_environment_prod_nuxd.apk");
            candidates.push_back(std::string(base) + "/haven2025_base.apk");
        }
        // For a v200+ TARGET (backporting), prefer the real shared shaders if present.
        // BUT V79 sources (.gltf.ovrscene / .opa official homes) are rendered by libshell's
        // OWN built-in shaders — the SystemShell ModelLoader compiles a dynamic PBR shader
        // (ShaderCache.cpp / DynamicShaderPBR.cpp, "compiling PBR shader for featureMask"),
        // it does NOT pull shaders from the env APK. So for V79 we don't require any external
        // pack; we use the self-contained built-in shader below. Only probe the external pack
        // when this is NOT a V79 source.
        for (auto& c : candidates) {
            fprintf(stderr, "[MAIN] Input has no RENDSHAD — loading shaders from: %s\n", c.c_str());
            loadShadersFromApk(c);
            if (!shaders.empty()) break;
        }
        if (shaders.empty()) {
            // ── Built-in self-contained shader (the V79 "both sides" path) ──────────────
            // Mirrors how V79 libshell renders old homes: its own shader, not the env's.
            fprintf(stderr, "[MAIN] Using built-in self-contained shader (V79 path — like libshell's\n"
                            "       ModelLoader DynamicShaderPBR; no env-supplied RENDSHAD needed).\n");
            SpirvBlob bv; bv.stageType = 0; bv.srcName = "builtin://v79_universal";
            bv.code.assign(kUnivVertSpirv, kUnivVertSpirv + kUnivVertSize / sizeof(uint32_t));
            SpirvBlob bf; bf.stageType = 4; bf.srcName = "builtin://v79_universal";
            bf.code.assign(kUnivFragSpirv, kUnivFragSpirv + kUnivFragSize / sizeof(uint32_t));
            shaders.push_back(std::move(bv));
            shaders.push_back(std::move(bf));
        }
    }

    fprintf(stderr, "[MAIN] Total shader blobs: %zu\n", shaders.size());
    // Debug: dump blob info
    for (size_t si = 0; si < shaders.size(); ++si) {
        auto& s = shaders[si];
        fprintf(stderr, "  Blob %zu: %zu words, stageType=%u", si, s.code.size(), s.stageType);
        if (s.code.size() >= 5) {
            // Parse OpEntryPoint
            for (size_t i = 5; i < s.code.size() && i < 30; ) {
                u32 word = s.code[i];
                u32 op = word & 0xFFFF;
                u32 wc = word >> 16;
                if (wc == 0 || wc > 100) break;
                if (op == 15 && i + 1 < s.code.size()) {
                    u32 execModel = s.code[i+1];
                    const char* emName = "?";
                    if (execModel == 0) emName = "VERTEX";
                    else if (execModel == 4) emName = "FRAGMENT";
                    else if (execModel == 5) emName = "COMPUTE";
                    fprintf(stderr, " exec=%u(%s)", execModel, emName);
                    break;
                }
                i += wc;
            }
        }
        fprintf(stderr, "\n");
    }

    // Select shaders:
    //   vertSpirv        = smallest vertex blob   (unlit.surface shader, ~1815 words)
    //   fragSpirv        = smallest fragment blob  (unlit.surface shader, ~2517 words)
    //   skinnedVertSpirv = largest vertex blob    (unlitblendskinned shader, ~3224 words)
    //   skinnedFragSpirv = largest fragment blob  (unlitblendskinned shader, ~7416 words)
    static constexpr u32 SPIRV_MAGIC = 0x07230203u;
    std::vector<u32> vertSpirv, fragSpirv, skinnedVertSpirv, skinnedFragSpirv;
    std::string g_globalShaderPath;       // path of the chosen global shader (-> renderer matParams match)
    std::vector<u32> alphaTestFragSpirv;  // V79/OPA cutout discard frag (set below); empty otherwise

    // Determine each blob's stage (0=vert, 4=frag) once.
    auto stageOf = [&](const SpirvBlob& s) -> u32 {
        if (s.code.size() < 5 || s.code[0] != SPIRV_MAGIC) return 0xFFFFFFFFu;
        for (size_t i = 5; i < s.code.size() && i < 100; ) {
            u32 word = s.code[i], op = word & 0xFFFF, wc = word >> 16;
            if (wc == 0 || wc > 256) break;
            if (op == 15 && i + 1 < s.code.size()) return s.code[i+1];
            i += wc;
        }
        return 0xFFFFFFFFu;
    };
    // Does a fragment shader sample a texture? (has an OpTypeSampledImage / OpTypeImage)
    auto fragSamplesTexture = [&](const SpirvBlob& s) -> bool {
        for (size_t i = 5; i + 1 < s.code.size(); ) {
            u32 word = s.code[i], op = word & 0xFFFF, wc = word >> 16;
            if (wc == 0) break;
            if (op == 25 /*OpTypeImage*/ || op == 27 /*OpTypeSampledImage*/) return true;
            i += wc;
        }
        return false;
    };

    // CRITICAL: vertex+fragment must come from the SAME shader program, or the
    // descriptor layouts won't match and the GPU samples garbage (HAVEN rendered
    // pure yellow because the smallest-vert and smallest-frag were picked from two
    // DIFFERENT of its 14 shader files). Group blobs by source file, then choose a
    // self-consistent pair, preferring a plain textured "unlit" surface shader.
    // A surface shader file holds MANY (vert,frag) variant pairs (mono/multiview/fog/…),
    // emitted in interleaved order. Pairing the i-th vertex with the i-th fragment keeps
    // both stages in the SAME variant (matching descriptor + push-constant interfaces).
    // We then pick the variant with the LARGEST fragment — the fullest shading path
    // (the tiny fragments are depth/shadow prepass variants that output no color).
    struct Prog { std::vector<u32> vert, frag; std::string name; bool fragTex=false; };
    std::vector<Prog> progs;
    {
        struct FileBlobs { std::vector<std::vector<u32>> verts, frags; };
        std::unordered_map<std::string, FileBlobs> byFile;
        std::vector<std::string> order;
        for (auto& s : shaders) {
            u32 st = stageOf(s);
            auto it = byFile.find(s.srcName);
            if (it == byFile.end()) { byFile[s.srcName]; order.push_back(s.srcName); }
            if (st == 0) byFile[s.srcName].verts.push_back(s.code);
            else if (st == 4) byFile[s.srcName].frags.push_back(s.code);
        }
        for (auto& name : order) {
            auto& fb = byFile[name];
            if (fb.verts.empty() || fb.frags.empty()) continue;
            // Choose the pair (i-th vert, i-th frag) whose fragment is largest among the
            // pairs that have both stages.
            size_t nPair = std::min(fb.verts.size(), fb.frags.size());
            size_t best = 0; size_t bestSz = 0;
            for (size_t i = 0; i < nPair; ++i)
                if (fb.frags[i].size() > bestSz) { bestSz = fb.frags[i].size(); best = i; }
            Prog p;
            p.name = name;
            p.vert = fb.verts[best];
            p.frag = fb.frags[best];
            // does the chosen fragment sample a texture?
            for (size_t i = 5; i + 1 < p.frag.size(); ) {
                u32 op = p.frag[i] & 0xFFFF, wc = p.frag[i] >> 16; if (!wc) break;
                if (op == 25 || op == 27) { p.fragTex = true; break; }
                i += wc;
            }
            progs.push_back(std::move(p));
        }
    }

    // Score programs: a textured unlit/default surface shader is the best generic
    // choice for static geometry (no per-material binding logic yet).
    // Our pipeline binds ONE base-color texture (set2 bind1). So prefer the simplest
    // single-texture shader and AVOID ones needing extra bound textures we don't
    // supply (lightmap, rgbmasked, normal) — those sample an unbound image and come
    // out black. "default"/"unlituniform"/"unlit" (non-lightmap) are ideal.
    // Now that set2 is built by introspecting the chosen shader (lightmap/extra
    // textures get a 1x1 white fallback), prefer a STANDARD surface shader that has
    // matParams + a base-color texture. Avoid billboard (no matParams; special-case)
    // and the giant "default" lit shader (needs many lighting buffers).
    // Prefer the env's real PBR workhorse, isotropictiled.surface — the shader the vast
    // majority of Haven props' materials reference. We now feed it full set0/set1/set2
    // (synthesized lighting + per-material params/textures), so it renders faithfully.
    // Avoid variant shaders that need extra per-material handling we don't do yet
    // (rgbmasked/emissive/normaldirectional/vegetation) and the unlit family (wrong for props).
    const char* forceShader = std::getenv("HSR_FORCESHADER");     // substring -> force that shader
    auto score = [forceShader](const Prog& p) -> int {
        std::string n = p.name; for (auto& c : n) c = (char)tolower((unsigned char)c);
        int s = 0;
        if (forceShader && *forceShader && n.find(forceShader) != std::string::npos) s += 100000;
        if (p.fragTex) s += 100;                                   // must sample a texture
        if (n.find("isotropictiled") != std::string::npos) s += 300;  // the PBR workhorse
        // rgbmasked treats the base texture as a MASK (real colors come from per-material LayerRed/
        // LayerBlue constants we can't feed yet) -> renders washed-out gray. Penalize hard so that
        // when an env ships ONLY rgbmasked variants (the vistas), the plain UNLIT shader wins instead
        // and shows the base texture's real albedo (green grass / brown dirt) rather than gray. Plain
        // isotropictiled (haven) has no rgbmask penalty so it still keeps faithful PBR.
        if (n.find("rgbmasked") != std::string::npos) s -= 350;
        if (n.find("emissive")  != std::string::npos) s -= 40;
        if (n.find("unpacked")  != std::string::npos) s -= 30;
        if (n.find("normaldirectional") != std::string::npos) s -= 50;
        if (n.find("vegetation")!= std::string::npos || n.find("animvege") != std::string::npos) s -= 80;
        // INSTANCED variants carry a per-instance "instance" UBO (atlasCellIndex + hue/bright/saturate
        // variation) we can't populate per-instance -> washes albedo to gray. Prefer the non-instance
        // sibling (vista ships both 'isotropictiled...tangent' and '...tangentinstance').
        if (n.find("instance") != std::string::npos) s -= 40;
        if (n.find("billboard") != std::string::npos) s -= 200;
        if (n.find("default")   != std::string::npos) s -= 100;   // needs full light/IBL/shadow set
        // Special-purpose surfaces must never be the scene-wide shader (they only fit their own mat).
        if (n.find("fog")   != std::string::npos) s -= 200;
        if (n.find("water") != std::string::npos) s -= 200;
        if (n.find("matte") != std::string::npos) s -= 200;       // mattepainting backdrop
        if (n.find("glass") != std::string::npos || n.find("decal") != std::string::npos) s -= 200;
        if (n.find("skybox")!= std::string::npos) s -= 200;
        // unlit family: shows the base texture's real albedo flat. NOT ideal for lit props, but it's the
        // right FALLBACK when an env ships no plain isotropictiled (vistas) — beats washed-gray rgbmasked
        // and the special shaders, while plain isotropictiled (+300) still wins where present (haven).
        if (n.find("unlit")     != std::string::npos) s += 20;
        // v203 terrain is baked-LIGHTMAP lit. When the env ships a lightmap shader (e.g. oceanarium's
        // unlittiledlightmap), PREFER it: it samples the env's baked lightmap (loader already decodes
        // it) -> faithful baked lighting/shadows instead of flat unlit. Still below isotropictiled(400).
        if (n.find("lightmap")  != std::string::npos) s += 60;
        // The scene-wide (static) shader shouldn't be a skinned variant (it expects bone data static
        // meshes lack); skinned meshes get their own program (selected below by "skinned").
        if (n.find("skinned")   != std::string::npos) s -= 30;
        return s;
    };
    const Prog* best = nullptr; int bestScore = -1000000;
    for (auto& p : progs) { int sc = score(p); if (sc > bestScore) { bestScore = sc; best = &p; } }
    if (best) {
        vertSpirv = best->vert; fragSpirv = best->frag;
        g_globalShaderPath = best->name;            // so per-material constant blocks of the SAME shader apply
        fprintf(stderr, "[MAIN] Best env program: %s (score=%d, fragTex=%d)\n",
                best->name.c_str(), bestScore, (int)best->fragTex);
    }

    // NOTE: env shaders each declare their own descriptor layout which may not match
    // our fixed pipeline (causing wrong sampling). But they ARE valid SPIR-V that
    // creates pipelines successfully, so we use the env's best textured program.
    // (An earlier embedded "universal" shader failed pipeline creation and crashed.)

    // Skinned program: the one whose name contains "skinned" (nuxd), else none.
    for (auto& p : progs) {
        std::string n = p.name; for (auto& c : n) c = (char)tolower((unsigned char)c);
        if (n.find("skinned") != std::string::npos) {
            skinnedVertSpirv = p.vert; skinnedFragSpirv = p.frag; break;
        }
    }

    // ── V79 transparency fix (global, any V79 env — glTF .ovrscene AND .opa official homes) ──
    // Raw V79 envs render with our OWN self-contained unlit shader that outputs the base
    // texture's rgb AND its ALPHA — exactly like libshell's V79 ModelLoader path. EVERY
    // borrowed v200+ shader (isotropictiled/unlit/billboard) hardwires output opacity=1.0
    // (baseColor.a is METALLIC there), so transparent textures — Outer Wilds planet
    // billboards, SpongeBob jellyfish, foliage cutouts, spacestation stars/ui-rings — rendered
    // as opaque black squares. Our shader feeds SRC_ALPHA blending real per-texel alpha ->
    // clear backgrounds. It also un-darkens V79 (baked-lit textures shown unlit, not re-lit by
    // PBR's dim synthesized IBL). CPU-skinned/animated meshes stream world-space positions into
    // the VBO, so this plain pos/uv vertex shader handles them too (no in-shader skinning).
    if (isV79 || isOpa) {
        vertSpirv.assign(kV79VertSpirv, kV79VertSpirv + kV79VertSpirvSize / sizeof(uint32_t));
        fragSpirv.assign(kV79FragSpirv, kV79FragSpirv + kV79FragSpirvSize / sizeof(uint32_t));
        // Cutout variant (discards texels below alpha threshold) for AlphaTest materials so
        // flags/foliage/animals draw in the opaque pass and write depth (faithful to libshell).
        alphaTestFragSpirv.assign(kV79FragAlphaSpirv, kV79FragAlphaSpirv + kV79FragAlphaSpirvSize / sizeof(uint32_t));
        skinnedVertSpirv.clear(); skinnedFragSpirv.clear();
        fprintf(stderr, "[MAIN] %s env -> built-in unlit shader with texture-ALPHA output "
                        "(transparency fix; %zu vert / %zu frag words)\n",
                isOpa ? "V79 .opa" : "V79 glTF", vertSpirv.size(), fragSpirv.size());
    }

    if (vertSpirv.empty() || fragSpirv.empty()) {
        fprintf(stderr, "[MAIN] FATAL: No usable shader program found in APK\n");
        return 1;
    }
    fprintf(stderr, "[MAIN] Selected VERTEX(unlit): %zu words  FRAGMENT(unlit): %zu words\n",
            vertSpirv.size(), fragSpirv.size());
    if (!skinnedVertSpirv.empty())
        fprintf(stderr, "[MAIN] Selected VERTEX(skinned): %zu words  FRAGMENT(skinned): %zu words\n",
                skinnedVertSpirv.size(), skinnedFragSpirv.size());

    fprintf(stderr, "\n[MAIN] Scene: %zu meshes ready\n\n", sceneMeshes->size());

    // ── Step B: Init GLFW + Vulkan ─────────────────────────────
    glfwSetErrorCallback(errorCb);
    if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);  // always-on-top so external screenshot capture is reliable
    g_window = glfwCreateWindow(g_winW, g_winH,
        "HSR Renderer [Vulkan] — WASD=move drag=look  Tab/N=next mesh B=prev  F=wire  Esc=quit",
        nullptr, nullptr);
    if (!g_window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }

    glfwSetKeyCallback(g_window, keyCb);
    glfwSetMouseButtonCallback(g_window, mouseBtnCb);
    glfwSetCursorPosCallback(g_window, cursorCb);
    glfwSetScrollCallback(g_window, scrollCb);
    glfwSetFramebufferSizeCallback(g_window, fbSizeCb);
    glfwSetDropCallback(g_window, dropCb);   // drag an .apk onto the window -> reload that env

    // ── Step C: Init Vulkan renderer ───────────────────────────
    VkRenderer vkRenderer;
    vkRenderer.verbose = true;
    vkRenderer.debugMode = false;
    g_renderer = &vkRenderer;
    vkRenderer.alphaTestFragSpirv = std::move(alphaTestFragSpirv);  // enables the cutout pipeline (V79/OPA)
    vkRenderer.globalShaderPath = g_globalShaderPath;               // per-material matParams match gate
    // Per-material shaders (HSR_PERMAT): hand the renderer EVERY loaded shader so it can build a
    // program per distinct material shader and route each mesh to its own (faithful emissive/masked/vege).
    // DEFAULT ON for V203/HSL envs (they ship their own per-material RENDSHAD) so the stock render needs
    // no flags; HSR_PERMAT forces on for any env; HSR_NOPERMAT forces off.
    bool usePerMat = !std::getenv("HSR_NOPERMAT") && (std::getenv("HSR_PERMAT") || envShippedRendShad);
    if (usePerMat) {
        vkRenderer.perMat = true;
        for (auto& p : progs)
            vkRenderer.loadedShaders.push_back({ VkRenderer::surfaceName(p.name), p.vert, p.frag });
        fprintf(stderr, "[MAIN] per-material shaders ON (%zu programs)%s\n", progs.size(),
                envShippedRendShad ? " [auto: V203 env ships RENDSHAD]" : " [HSR_PERMAT]");
    }

    if (!vkRenderer.init(g_window, vertSpirv, fragSpirv, skinnedVertSpirv, skinnedFragSpirv)) {
        fprintf(stderr, "[MAIN] FATAL: Vulkan init failed\n");
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    // Hand the SpecIbl diffuse cubemap to the renderer BEFORE uploading (uploadMesh bakes it into the
    // per-vertex color of *_specibl meshes). HSR_NOIBL disables it for comparison.
    if (isOpa && opa.iblDiffuse.ok() && !std::getenv("HSR_NOIBL")) vkRenderer.iblDiffuse = opa.iblDiffuse;
    // SPECULAR cube (mip0) for the CPU per-vertex split-sum IBL of no-albedo metallic/gem shells
    // (divingHelmet, rubyGem). This is the FAITHFUL path: V79 has no per-pixel cube sampler (proven —
    // 0 samplerCube/reflect in libshell), so the IBL is reduced on the CPU per vertex. The reduction is
    // roughness-weighted (rough->collapses to the dim irradiance) so it CANNOT blow rough surfaces to
    // white the way the old matcap-at-normal guess did. HSR_NOIBL disables. HSR_NOSPECIBL = diffuse-only.
    if (isOpa && opa.iblSpecular.ok() && !std::getenv("HSR_NOIBL") && !std::getenv("HSR_NOSPECIBL")) vkRenderer.iblSpecular = opa.iblSpecular;
    // (Legacy GPU cube upload path — unused by the faithful CPU bake; kept behind HSR_SPECULAR.)
    if (isOpa && !opa.iblSpecularRaw.empty() && std::getenv("HSR_SPECULAR")) vkRenderer.setSpecularCubemap(opa.iblSpecularRaw);
    if (isV79 || isOpa) vkRenderer.ensureSpecCube();   // valid cube view for the V79 shader's cube slot

    // Upload meshes (HSR_MAXMESH / HSR_MINMESH env limit the range for crash bisection)
    fprintf(stderr, "\n[MAIN] Uploading %zu meshes to GPU...\n", sceneMeshes->size());
    int minMesh = 0, maxMesh = (int)sceneMeshes->size();
    if (const char* e = std::getenv("HSR_MINMESH")) minMesh = atoi(e);
    if (const char* e = std::getenv("HSR_MAXMESH")) maxMesh = atoi(e);
    for (int mi = 0; mi < (int)sceneMeshes->size(); ++mi) {
        if (mi < minMesh || mi >= maxMesh) continue;
        vkRenderer.uploadMesh((*sceneMeshes)[mi]);
    }
    fprintf(stderr, "[MAIN] GPU upload complete: %zu meshes\n\n",
            vkRenderer.gpuMeshes.size());

    // [DEBUG] HSR_TESTMOVE=<dx>: shift every mesh's model translation by +dx in X. If the scene
    // visibly moves, the worldFromModel push constant is live; if not, the shader ignores it.
    if (const char* tm = std::getenv("HSR_TESTMOVE")) {
        float d = (float)atof(tm);
        for (auto& gm : vkRenderer.gpuMeshes) gm.model[12] += d;
        fprintf(stderr, "[TESTMOVE] shifted all %zu models by x+=%.2f\n", vkRenderer.gpuMeshes.size(), d);
    }

    // ── Step D: Main loop ──────────────────────────────────────
    fprintf(stderr, "[MAIN] Render loop started — Esc to quit\n\n");
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frames = 0;
    auto fpsTime = lastTime;

    // Headless auto-capture: HSR_SHOT=path captures after warmup; HSR_SHOT_FRAMES
    // (default 150) controls when, HSR_SHOT_QUIT=1 exits after the shot.
    const char* shotPath   = std::getenv("HSR_SHOT");
    int  shotAtFrame = 150;
    if (const char* sf = std::getenv("HSR_SHOT_FRAMES")) { int v=atoi(sf); if (v>0) shotAtFrame=v; }
    bool shotQuit = std::getenv("HSR_SHOT_QUIT") != nullptr;
    if (const char* solo = std::getenv("HSR_SOLO")) { vkRenderer.soloMesh = atoi(solo); }
    if (const char* hide = std::getenv("HSR_HIDEMESH")) { vkRenderer.hideMesh = atoi(hide); }
    if (const char* hm = std::getenv("HSR_HIDEMAT")) { vkRenderer.hideMat = hm; }
    if (const char* sm = std::getenv("HSR_SOLOMAT")) { vkRenderer.soloMat = sm; }
    if (std::getenv("HSR_WIRE")) vkRenderer.wireframe = true;   // headless wireframe diagnostic
    // Camera override for headless captures: HSR_CAM="x,y,z,yawDeg,pitchDeg"
    if (const char* cs = std::getenv("HSR_CAM")) {
        float x,y,z,yd,pd;
        if (sscanf(cs, "%f,%f,%f,%f,%f", &x,&y,&z,&yd,&pd) == 5) {
            vkRenderer.cam.pos[0]=x; vkRenderer.cam.pos[1]=y; vkRenderer.cam.pos[2]=z;
            vkRenderer.cam.yaw = yd*3.14159265f/180.0f;
            vkRenderer.cam.pitch = pd*3.14159265f/180.0f;
        }
    }
    // HSR_SOLO auto-frame: when soloing a mesh with NO explicit HSR_CAM, park the camera right in
    // front of that mesh's world AABB so it FILLS the view — inspect each submesh up close.
    if (vkRenderer.soloMesh >= 0 && !std::getenv("HSR_CAM")
        && vkRenderer.soloMesh < (int)sceneMeshes->size()
        && vkRenderer.soloMesh < (int)vkRenderer.gpuMeshes.size()) {
        const auto& md = (*sceneMeshes)[vkRenderer.soloMesh];
        const float* M = vkRenderer.gpuMeshes[vkRenderer.soloMesh].model;   // per-mesh world transform
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
        size_t nv = md.positions.size()/3;
        for (size_t i=0;i<nv;i++){
            float lx=md.positions[i*3], ly=md.positions[i*3+1], lz=md.positions[i*3+2];
            float wx=M[0]*lx+M[4]*ly+M[8]*lz+M[12];
            float wy=M[1]*lx+M[5]*ly+M[9]*lz+M[13];
            float wz=M[2]*lx+M[6]*ly+M[10]*lz+M[14];
            if(wx<mn[0])mn[0]=wx; if(wx>mx[0])mx[0]=wx;
            if(wy<mn[1])mn[1]=wy; if(wy>mx[1])mx[1]=wy;
            if(wz<mn[2])mn[2]=wz; if(wz>mx[2])mx[2]=wz;
        }
        if (nv>0){
            float cx=(mn[0]+mx[0])*0.5f, cy=(mn[1]+mx[1])*0.5f, cz=(mn[2]+mx[2])*0.5f;
            float rx=mx[0]-mn[0], ry=mx[1]-mn[1], rz=mx[2]-mn[2];
            float radius=0.5f*sqrtf(rx*rx+ry*ry+rz*rz); if(radius<0.01f)radius=0.5f;
            float fov=vkRenderer.cam.fovDeg*3.14159265f/180.0f;
            float dist=radius/tanf(fov*0.5f)*1.5f;
            vkRenderer.cam.pos[0]=cx; vkRenderer.cam.pos[1]=cy; vkRenderer.cam.pos[2]=cz+dist;
            vkRenderer.cam.yaw=0.0f; vkRenderer.cam.pitch=0.0f;   // look -Z straight at the centroid
            fprintf(stderr,"[SOLO] auto-framed mesh %d center(%.2f,%.2f,%.2f) r=%.2f dist=%.2f\n",
                    vkRenderer.soloMesh,cx,cy,cz,radius,dist);
        }
    }
    long totalFrames = 0;
    bool shotDone = false;
    auto animStart = std::chrono::high_resolution_clock::now();
    // HSR_ANIMTIME=<sec> forces a fixed animation pose (deterministic capture / debugging).
    const char* animTimeEnv = std::getenv("HSR_ANIMTIME");
    float fixedAnimTime = animTimeEnv ? (float)atof(animTimeEnv) : -1.0f;

    // ── Ambient audio: loop the env's *.ogg (e.g. _BACKGROUND_LOOP.ogg) from the APK ──
    AudioPlayer g_audio;
    if (!std::getenv("HSR_NOAUDIO")) {
        std::vector<u8> ogg;
        mz_zip_archive az; memset(&az, 0, sizeof(az));
        if (mz_zip_reader_init_file(&az, apkPath.c_str(), 0)) {
            int si = mz_zip_reader_locate_file(&az, "assets/scene.zip", nullptr, 0);
            if (si >= 0) {
                size_t szN = 0; void* szD = mz_zip_reader_extract_to_heap(&az, si, &szN, 0);
                if (szD) {
                    mz_zip_archive sz; memset(&sz, 0, sizeof(sz));
                    if (mz_zip_reader_init_mem(&sz, szD, szN, 0)) {
                        u32 nf = mz_zip_reader_get_num_files(&sz);
                        for (u32 i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
                            if (!mz_zip_reader_file_stat(&sz, i, &st)) continue;
                            std::string fn(st.m_filename);
                            if (fn.size() >= 4 && fn.compare(fn.size()-4, 4, ".ogg") == 0) {
                                size_t on = 0; void* od = mz_zip_reader_extract_to_heap(&sz, i, &on, 0);
                                if (od) { ogg.assign((u8*)od, (u8*)od + on); mz_free(od); }
                                break; }
                        }
                        mz_zip_reader_end(&sz);
                    }
                    mz_free(szD);
                }
            }
            mz_zip_reader_end(&az);
        }
        if (!ogg.empty()) g_audio.start(ogg.data(), ogg.size());
        else fprintf(stderr, "[AUDIO] no .ogg in this env\n");
    }

    // ── Editor UI (Dear ImGui): outliner, move, focus, anim/audio control, save ──
    float animDur = isOpa ? opa.animDuration() : (isV79 ? gltf.animDuration : 0.0f);
    Editor editor;
    if (!std::getenv("HSR_NOUI"))   // HSR_NOUI = clean capture without the editor overlay
        editor.init(&vkRenderer, g_window, &g_audio, &g_animOverride, &g_animScrub, animDur);

    while (!glfwWindowShouldClose(g_window)) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        // V79 glTF skeletal animation: sample the clip and stream skinned positions into
        // each dynamic mesh's persistently-mapped VBO (vertex position is at offset 0).
        if (isV79 && gltf.hasAnimation()) {
            float at = g_animOverride ? g_animScrub : ((fixedAnimTime >= 0.f) ? fixedAnimTime : std::chrono::duration<float>(now - animStart).count());
            gltf.animate(at);
            for (size_t i = 0; i < gltf.meshes.size() && i < vkRenderer.gpuMeshes.size(); ++i) {
                auto& md = gltf.meshes[i];
                auto& gm = vkRenderer.gpuMeshes[i];
                if (!gm.dynamicVerts || !gm.vboMapped) continue;
                u8* base = reinterpret_cast<u8*>(gm.vboMapped);
                u32 nv = gm.dynVertCount;
                float cx=0,cy=0,cz=0; float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; u32 cnt=0;
                for (u32 v = 0; v < nv && (size_t)v*3+2 < md.positions.size(); ++v) {
                    float* p = reinterpret_cast<float*>(base + (size_t)v * gm.vboStride + gm.posOffset);
                    float X=md.positions[v*3], Y=md.positions[v*3+1], Z=md.positions[v*3+2];
                    p[0]=X; p[1]=Y; p[2]=Z;
                    cx+=X; cy+=Y; cz+=Z; ++cnt;
                    mn[0]=std::min(mn[0],X); mn[1]=std::min(mn[1],Y); mn[2]=std::min(mn[2],Z);
                    mx[0]=std::max(mx[0],X); mx[1]=std::max(mx[1],Y); mx[2]=std::max(mx[2],Z);
                }
                if (cnt) {   // gizmo/pick follow the animated mesh (skinned bind verts sit near origin)
                    gm.centroid[0]=cx/cnt; gm.centroid[1]=cy/cnt; gm.centroid[2]=cz/cnt;
                    gm.bbMin[0]=mn[0]; gm.bbMin[1]=mn[1]; gm.bbMin[2]=mn[2];
                    gm.bbMax[0]=mx[0]; gm.bbMax[1]=mx[1]; gm.bbMax[2]=mx[2];
                    if (gm.pickPos.size()==md.positions.size()) gm.pickPos = md.positions;
                }
            }
        }

        // V79 .opa node animation (sanim): the looping ui_ring / wire motion. Same dynamic-
        // vertex streaming path — animate() rewrites world positions, we push them to the VBO.
        if (isOpa && opa.hasAnimation()) {
            float at = g_animOverride ? g_animScrub : ((fixedAnimTime >= 0.f) ? fixedAnimTime : std::chrono::duration<float>(now - animStart).count());
            opa.animate(at);
            for (size_t i = 0; i < opa.meshes.size() && i < vkRenderer.gpuMeshes.size(); ++i) {
                auto& md = opa.meshes[i];
                auto& gm = vkRenderer.gpuMeshes[i];
                if (!gm.dynamicVerts || !gm.vboMapped) continue;
                // per-frame MaterialTint (UniformColor) — fog/dust/flicker opacity fade
                gm.curTint[0]=md.curTint[0]; gm.curTint[1]=md.curTint[1];
                gm.curTint[2]=md.curTint[2]; gm.curTint[3]=md.curTint[3];
                // keep the ray-pick geometry in sync with the animated positions
                if (gm.pickPos.size() == md.positions.size()) gm.pickPos = md.positions;
                u8* base = reinterpret_cast<u8*>(gm.vboMapped);
                u32 nv = gm.dynVertCount;
                // Re-derive the world centroid + AABB from the POSED positions so the editor gizmo and
                // ray-pick follow an animated/skinned mesh (skinned meshes keep LOCAL bind verts near
                // the origin at upload -> the gizmo sat at world 0,0,0 = "stalked the camera").
                float cx=0,cy=0,cz=0; float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; u32 cnt=0;
                for (u32 v = 0; v < nv && (size_t)v*3+2 < md.positions.size(); ++v) {
                    float* p = reinterpret_cast<float*>(base + (size_t)v * gm.vboStride + gm.posOffset);
                    float X=md.positions[v*3], Y=md.positions[v*3+1], Z=md.positions[v*3+2];
                    p[0]=X; p[1]=Y; p[2]=Z;
                    cx+=X; cy+=Y; cz+=Z; ++cnt;
                    mn[0]=std::min(mn[0],X); mn[1]=std::min(mn[1],Y); mn[2]=std::min(mn[2],Z);
                    mx[0]=std::max(mx[0],X); mx[1]=std::max(mx[1],Y); mx[2]=std::max(mx[2],Z);
                }
                if (cnt) {
                    gm.centroid[0]=cx/cnt; gm.centroid[1]=cy/cnt; gm.centroid[2]=cz/cnt;
                    gm.bbMin[0]=mn[0]; gm.bbMin[1]=mn[1]; gm.bbMin[2]=mn[2];
                    gm.bbMax[0]=mx[0]; gm.bbMax[1]=mx[1]; gm.bbMax[2]=mx[2];
                }
                // mat.sanim UV/flipbook animation: stream the (possibly transformed) UVs too.
                for (u32 v = 0; v < nv && (size_t)v*2+1 < md.uvs.size(); ++v) {
                    float* q = reinterpret_cast<float*>(base + (size_t)v * gm.vboStride + gm.uvOffset);
                    q[0] = md.uvs[v*2]; q[1] = md.uvs[v*2+1];
                }
                static int sdbg=-1; if(sdbg<0) sdbg=std::getenv("HSR_STREAMDBG")?1:0;
                if(sdbg && (i==137||i==148)) {
                    const float* vu = (const float*)(base + gm.uvOffset);
                    fprintf(stderr,"[STREAMDBG] i=%zu dyn=%d nv=%u uvOff=%u stride=%u uvsz=%zu md.uv0=(%.3f,%.3f) vbo.uv0=(%.3f,%.3f)\n",
                        i,(int)gm.dynamicVerts,nv,gm.uvOffset,gm.vboStride,md.uvs.size(),
                        md.uvs.size()>1?md.uvs[0]:-9.f,md.uvs.size()>1?md.uvs[1]:-9.f, vu[0],vu[1]);
                }
            }
        }

        // v203 HzAnim skeletal animation (nuxd & any RENDMESH env with a skeleton): CPU-skin into the
        // dynamic VBO each frame (same streaming path as OPA/glTF). Makes prism_wave/motes ripple.
        if (loader.hasAnimation()) {
            float at = g_animOverride ? g_animScrub : ((fixedAnimTime >= 0.f) ? fixedAnimTime : std::chrono::duration<float>(now - animStart).count());
            loader.animate(at);
            for (size_t i = 0; i < loader.meshes.size() && i < vkRenderer.gpuMeshes.size(); ++i) {
                auto& md = loader.meshes[i];
                auto& gm = vkRenderer.gpuMeshes[i];
                if (!gm.dynamicVerts || !gm.vboMapped) continue;
                if (gm.pickPos.size() == md.positions.size()) gm.pickPos = md.positions;
                u8* base = reinterpret_cast<u8*>(gm.vboMapped);
                u32 nv = gm.dynVertCount;
                float cx=0,cy=0,cz=0; float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; u32 cnt=0;
                for (u32 v = 0; v < nv && (size_t)v*3+2 < md.positions.size(); ++v) {
                    float* p = reinterpret_cast<float*>(base + (size_t)v * gm.vboStride + gm.posOffset);
                    float X=md.positions[v*3], Y=md.positions[v*3+1], Z=md.positions[v*3+2];
                    p[0]=X; p[1]=Y; p[2]=Z; cx+=X; cy+=Y; cz+=Z; ++cnt;
                    mn[0]=std::min(mn[0],X); mn[1]=std::min(mn[1],Y); mn[2]=std::min(mn[2],Z);
                    mx[0]=std::max(mx[0],X); mx[1]=std::max(mx[1],Y); mx[2]=std::max(mx[2],Z);
                }
                if (cnt) { gm.centroid[0]=cx/cnt; gm.centroid[1]=cy/cnt; gm.centroid[2]=cz/cnt;
                    gm.bbMin[0]=mn[0]; gm.bbMin[1]=mn[1]; gm.bbMin[2]=mn[2];
                    gm.bbMax[0]=mx[0]; gm.bbMax[1]=mx[1]; gm.bbMax[2]=mx[2]; }
            }
        }

        auto& cam = vkRenderer.cam;
        if (!uiWantsKeyboard()) {
        if (glfwGetKey(g_window, GLFW_KEY_W) == GLFW_PRESS) cam.moveForward(dt);
        if (glfwGetKey(g_window, GLFW_KEY_S) == GLFW_PRESS) cam.moveBack(dt);
        if (glfwGetKey(g_window, GLFW_KEY_D) == GLFW_PRESS) cam.moveRight(dt);
        if (glfwGetKey(g_window, GLFW_KEY_A) == GLFW_PRESS) cam.moveLeft(dt);
        if (glfwGetKey(g_window, GLFW_KEY_E) == GLFW_PRESS) cam.moveUp(dt);
        if (glfwGetKey(g_window, GLFW_KEY_Q) == GLFW_PRESS) cam.moveDown(dt);
        }

        vkRenderer.render();
        glfwPollEvents();

        // Click-to-select: cast a ray from the cursor and pick the nearest mesh's AABB.
        if (g_clickPick) {
            g_clickPick = false;
            if (!std::getenv("HSR_NOUI")) editor.pick(g_clickX, g_clickY);
        }
        // Right-click-to-menu: pick the mesh under the cursor and open its context menu.
        if (g_rightClick) {
            g_rightClick = false;
            if (!std::getenv("HSR_NOUI")) editor.pickForMenu(g_rightX, g_rightY);
        }

        // Drag-and-drop reload: an .apk was dropped -> relaunch on it (inherits HSR_* params) and exit.
        if (g_doReload) {
#ifdef _WIN32
            relaunchSelf(g_dropPath);
#endif
            break;
        }

        totalFrames++;
        if (shotPath && !shotDone && totalFrames >= shotAtFrame) {
            { FILE* tf=fopen("_main_trace.txt","a"); if(tf){fprintf(tf,"calling screenshot frame=%ld\n",totalFrames);fclose(tf);} }
            vkRenderer.screenshot(shotPath);
            { FILE* tf=fopen("_main_trace.txt","a"); if(tf){fprintf(tf,"screenshot returned\n");fclose(tf);} }
            shotDone = true;
            if (shotQuit) glfwSetWindowShouldClose(g_window, GLFW_TRUE);
        }

        frames++;
        auto fpsNow = std::chrono::high_resolution_clock::now();
        float fpsElapsed = std::chrono::duration<float>(fpsNow - fpsTime).count();
        if (fpsElapsed >= 5.0f) {
            fprintf(stderr, "[FPS] %.1f  pos=(%.1f,%.1f,%.1f)  yaw=%.0f pitch=%.0f speed=%.1f\n",
                frames/fpsElapsed, cam.pos[0],cam.pos[1],cam.pos[2],
                cam.yaw*57.3f, cam.pitch*57.3f, cam.speed);
            frames = 0;
            fpsTime = fpsNow;
        }
    }

    // Cleanup
    fprintf(stderr, "\n[MAIN] Shutting down...\n");
    if (!std::getenv("HSR_NOUI")) editor.shutdown();
    vkRenderer.cleanup();
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
