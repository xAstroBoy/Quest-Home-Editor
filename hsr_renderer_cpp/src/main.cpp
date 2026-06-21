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
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
// Symbolized crash handler: on a segfault, walk the faulting thread's stack and print
// function + file:line for each frame (needs the PDB from /Zi /DEBUG). Writes to stderr
// (→ _live.log) AND _crash.txt so a background/headless crash is never silent.
static LONG WINAPI hsrCrashHandler(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    FILE* cf = fopen("_crash.txt", "w");
    auto emit = [&](const char* fmt, auto... a) {
        fprintf(stderr, fmt, a...); if (cf) fprintf(cf, fmt, a...);
    };
    emit("\n[CRASH] code=0x%08lx addr=%p\n",
         (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
        emit("[CRASH] access violation %s address %p\n",
             ep->ExceptionRecord->ExceptionInformation[0] ? "WRITING" : "READING",
             (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    char symbuf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (!sf.AddrPC.Offset) break;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
        DWORD64 disp = 0; const char* name = "(?)";
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) name = sym->Name;
        IMAGEHLP_LINE64 ln; ln.SizeOfStruct = sizeof(ln); DWORD ld = 0;
        if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &ln)) {
            const char* fn = strrchr(ln.FileName, '\\'); fn = fn ? fn + 1 : ln.FileName;
            emit("  #%-2d %s  (%s:%lu)\n", i, name, fn, (unsigned long)ln.LineNumber);
        } else {
            emit("  #%-2d %s +0x%llx\n", i, name, (unsigned long long)disp);
        }
    }
    if (cf) fclose(cf);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate after reporting
}

// ── HSR_LIVE HTTP control server ───────────────────────────────────────────────────────────────
// A tiny localhost HTTP server so the renderer loads ONCE and is driven live (no relaunch, no files).
// POST the command block (newline-separated, same syntax as the comments in the render loop) in the
// request body; the response body carries the result (farscan/listmesh dumps, or "ok"/"shot <path>").
// The socket thread only enqueues raw text under a mutex; ALL renderer/Vulkan state is touched solely
// by the main thread, which drains the queue each frame — so this never races the GPU.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
struct LiveCmd { std::string text, result; std::atomic<bool> done{false}; };
static std::mutex          g_liveMx;
static std::deque<LiveCmd*> g_liveQ;
std::atomic<bool>          g_audioMuted{false};   // PC preview-audio mute: the editor's "Play preview audio" toggle binds here; audio.h's data callback reads it (defined non-static so editor.h/audio.h externs resolve)
static void hsrHttpServer(int port) {
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) { fprintf(stderr, "[HTTP] WSAStartup failed\n"); return; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof yes);
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
        fprintf(stderr, "[HTTP] bind/listen failed on port %d (err %d)\n", port, WSAGetLastError()); return;
    }
    fprintf(stderr, "[HTTP] live control on http://127.0.0.1:%d  (POST commands in body)\n", port);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::string req; char rb[4096]; int n;
        while ((n = recv(c, rb, sizeof rb, 0)) > 0) {
            req.append(rb, n);
            size_t hp = req.find("\r\n\r\n");
            if (hp != std::string::npos) {
                size_t clp = req.find("Content-Length:");
                int cl = (clp != std::string::npos) ? atoi(req.c_str() + clp + 15) : 0;
                size_t have = req.size() - (hp + 4);
                while ((int)have < cl) { n = recv(c, rb, sizeof rb, 0); if (n <= 0) break; req.append(rb, n); have += n; }
                break;
            }
        }
        std::string cmds; size_t hp = req.find("\r\n\r\n");
        if (hp != std::string::npos && hp + 4 < req.size()) cmds = req.substr(hp + 4);
        LiveCmd lc; lc.text = cmds;
        { std::lock_guard<std::mutex> g(g_liveMx); g_liveQ.push_back(&lc); }
        while (!lc.done.load(std::memory_order_acquire)) Sleep(1);
        std::string body = lc.result.empty() ? "ok\n" : lc.result;
        char hdr[160]; int hl = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        send(c, hdr, hl, 0); send(c, body.data(), (int)body.size(), 0);
        closesocket(c);
    }
}
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "core/types.h"
#include "core/camera.h"
#include "loaders/asmh_parser.h"
#include "loaders/rendmesh_parser.h"
#include "loaders/rendtxtr_parser.h"
#include "loaders/matlmatl_parser.h"
#include "loaders/hstf_parser.h"
#include "loaders/rendshad_parser.h"
#include "render/universal_shader.h"
#include "core/audio.h"
#include "core/audio_convert.h"
#include "render/v79_shader.h"
#include "render/vk_renderer.h"
#include "loaders/scene_loader.h"
#include "loaders/gltf_loader.h"
#include "loaders/opa_loader.h"
#include "core/audio.h"
#include "ui/editor.h"
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
static Editor*         g_editor = nullptr;      // the custom-UI editor (defined in editor.h above); callbacks route input here

// Procedural editor icon: a dark disc with the R/G/B move-gizmo motif (matches the editor's gizmo). No asset file.
static void genEditorIcon(int S, std::vector<unsigned char>& px) {
    px.assign((size_t)S*S*4, 0); float c=(S-1)*0.5f, R=S*0.47f;
    auto set=[&](int x,int y,int r,int g,int b,int a){ if(x<0||y<0||x>=S||y>=S)return; size_t i=((size_t)y*S+x)*4; px[i]=(unsigned char)r;px[i+1]=(unsigned char)g;px[i+2]=(unsigned char)b;px[i+3]=(unsigned char)a; };
    for (int y=0;y<S;y++) for (int x=0;x<S;x++){ float dx=x-c,dy=y-c; if (dx*dx+dy*dy<=R*R) set(x,y,44,46,54,255); }
    auto axis=[&](float ang,float L,int r,int g,int b){ float dx=std::sin(ang),dy=-std::cos(ang); for(float t=0;t<=L;t+=0.5f){ float xx=c+dx*t,yy=c+dy*t; for(int oy=-1;oy<=1;oy++)for(int ox=-1;ox<=1;ox++) set((int)(xx+0.5f)+ox,(int)(yy+0.5f)+oy,r,g,b,255);} for(int oy=-2;oy<=2;oy++)for(int ox=-2;ox<=2;ox++) set((int)(c+dx*L+0.5f)+ox,(int)(c+dy*L+0.5f)+oy,r,g,b,255); };
    float L=S*0.34f; axis(1.5708f,L,232,72,72); axis(0.f,L,96,210,96); axis(3.6651f,L,80,130,245);  // X red, Y green, Z blue
}

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
    if (g_editor && g_editor->ready) g_editor->onKey(key, act, mods);
    if (g_editor && g_editor->ready && g_editor->wantsKeyboard()) return;  // typing in a UI text field
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

static bool uiWantsMouse()    { return g_editor && g_editor->ready && g_editor->wantsMouse(); }
static bool uiWantsKeyboard() { return g_editor && g_editor->ready && g_editor->wantsKeyboard(); }

// The editor owns pick + the gizmo (in onMouseButton). Here we only manage the fly-cam look-drag: a left
// press that DIDN'T land on a panel/gizmo (uiWantsMouse) begins a look; the editor decides click-vs-drag.
static void mouseBtnCb(GLFWwindow* w, int btn, int act, int mods) {
    if (g_editor && g_editor->ready) g_editor->onMouseButton(btn, act, mods);
    if (btn == GLFW_MOUSE_BUTTON_LEFT) {
        if (act == GLFW_PRESS) { g_mouseDown = !uiWantsMouse(); glfwGetCursorPos(w, &g_mx, &g_my); }
        else if (act == GLFW_RELEASE) g_mouseDown = false;
    }
}

static void cursorCb(GLFWwindow* w, double x, double y) {
    if (g_editor && g_editor->ready) g_editor->onCursorPos(x, y);
    if (g_mouseDown && !uiWantsMouse() && g_renderer)        // look-drag only over the viewport
        g_renderer->cam.rotate((float)(x - g_mx), (float)(y - g_my));
    g_mx = x; g_my = y;
}

static void scrollCb(GLFWwindow* w, double xo, double yo) {
    if (g_editor && g_editor->ready) g_editor->onScroll(xo, yo);
    if (uiWantsMouse()) return;                              // scrolling a panel, not the fly-cam
    if (g_renderer) g_renderer->cam.adjustSpeed(yo > 0 ? 1.25f : 0.8f);
}
static void charCb(GLFWwindow* w, unsigned int cp) { if (g_editor && g_editor->ready) g_editor->onChar(cp); }

static void fbSizeCb(GLFWwindow* w, int w_, int h_) {
    g_winW = w_; g_winH = h_;
    if (g_renderer) g_renderer->framebufferResized = true;
}

static void errorCb(int err, const char* desc) {
    fprintf(stderr, "[GLFW] Err %d: %s\n", err, desc);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(hsrCrashHandler);   // segfault → symbolized stack in stderr + _crash.txt
#endif
    // Record the exe's own directory so APK signing can find/auto-create the Android build-tools + debug keystore
    // right beside the exe (a machine with no Android SDK just needs the tools dropped next to the exe).
#ifdef _WIN32
    { char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
      std::string p(exe, (n>0&&n<MAX_PATH)?(size_t)n:0); size_t s = p.find_last_of("\\/");
      if (s != std::string::npos) AppConfig::s_exeDir = p.substr(0, s); }
#else
    if (argc > 0 && argv[0]) { std::error_code ec; std::string p = std::filesystem::absolute(argv[0], ec).string();
      size_t s = p.find_last_of('/'); if (s != std::string::npos) AppConfig::s_exeDir = p.substr(0, s); }
#endif
    fprintf(stderr, "========================================================\n");
    fprintf(stderr, " HSR Renderer / Editor — libshell.so Vulkan replica\n");
    fprintf(stderr, " Drag an .apk onto the window to load it\n");
    fprintf(stderr, "========================================================\n\n");
    if (!std::getenv("HSR_NO_TOOLCHECK")) hslcook::reportToolchain();   // startup readiness of the signing toolchain
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

    // ── standalone APK signer: `hsr_renderer --sign <foo.apk> [more.apk ...]` ──────────────────────────────
    // Signs an ALREADY-BUILT APK (e.g. a shared/unsigned cooked home someone sent you) in place -> <name>_signed.apk,
    // so it installs without INSTALL_PARSE_FAILED_NO_CERTIFICATES. NO re-cook needed. Same auto-detected build-tools +
    // auto-generated debug keystore as the cooker (drop apksigner+zipalign beside the exe if you have no Android SDK).
    if (argc >= 2 && (std::string(argv[1]) == "--sign" || std::string(argv[1]) == "-s")) {
        if (argc < 3) { fprintf(stderr, "usage: hsr_renderer --sign <apk> [more.apk ...]\n"); return 2; }
        int fails = 0;
        for (int i = 2; i < argc; ++i) {
            std::string in = argv[i];
            std::string out = (in.size() > 4 && in.substr(in.size() - 4) == ".apk")
                                ? in.substr(0, in.size() - 4) + "_signed.apk" : in + "_signed.apk";
            fprintf(stderr, "\n[SIGN] %s\n", in.c_str());
            bool ok = hslcook::signApk(in, out, [](float f, const char* s){ fprintf(stderr, "  [%3d%%] %s\n", (int)(f * 100.f), s); });
            if (ok) fprintf(stderr, "[SIGN]  OK -> %s\n", out.c_str());
            else  { fprintf(stderr, "[SIGN]  FAILED: %s\n", in.c_str()); ++fails; }
        }
        return fails ? 1 : 0;
    }

    // `hsr_renderer --restore-haven` puts the ORIGINAL Meta Haven 2025 back from the auto-backup the cooker made
    // (folder "Haven2025_Backup" beside the exe) before it installed a spoof, then relaunches the shell.
    if (argc >= 2 && std::string(argv[1]) == "--restore-haven") {
        return Editor::cliRestoreHaven();
    }

    // `hsr_renderer --fetch-tools` pre-downloads the Android signing toolchain (Google build-tools + a Temurin JRE
    // if no Java) right beside the exe, so later --sign / Cook works on a clean machine with no SDK and no JDK.
    if (argc >= 2 && std::string(argv[1]) == "--fetch-tools") {
        auto p = [](float f, const char* s){ fprintf(stderr, "  [%3d%%] %s\n", (int)(f * 100.f), s); };
        std::string bt = hslcook::downloadBuildTools(p);
        if (bt.empty()) fprintf(stderr, "[TOOLS] build-tools: FAILED (need curl + network)\n");
        else            fprintf(stderr, "[TOOLS] build-tools -> %s\n", bt.c_str());
        std::string jh = hslcook::ensureJava(p);
        fprintf(stderr, "[TOOLS] java -> %s\n", jh.empty() ? "(already on PATH)" : jh.c_str());
        return bt.empty() ? 1 : 0;
    }

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
    // NOTE: a single per-env lightmap exposure is WRONG for calming — it mixes INTERIOR home meshes (floor/walls,
    // need ~2.6) with OUTDOOR vista meshes (ground/boulders, want ~1.4). Lowering it globally under-exposed the
    // home floor/walls (dark/wrong). Keep the interior-tuned default (g_lmExposure=2.6); a faithful per-MESH
    // exposure (home vs vista) is the real fix. HSR_LMEXP still overrides globally for manual A/B.

    // ── Step A: Load scene ─────────────────────────────────────
    // Detect format: a raw V79 env ships a *.gltf.ovrscene (glTF 2.0 + ASTC KTX); the new
    // HSR/Haven format ships RENDMESH/MATLMATL/RENDSHAD. Try V79 (glTF) first, else HSR.
    SceneLoader loader;
    // Per-asset logging floods stderr (hundreds of lines) — and stderr is slow on Windows, so it noticeably
    // drags out load time. Default OFF; set HSR_VERBOSE=1 to re-enable the full asset trace for debugging.
    loader.verbose = (std::getenv("HSR_VERBOSE") != nullptr);
    GltfLoader gltf;
    OpaLoader opa;
    std::vector<MeshData>* sceneMeshes = nullptr;
    // Companion env to render alongside (merged at the shared origin). Explicit via HSR_BACKDROP, or AUTO:
    // a "vista_" env is a BACKGROUND scenery for the haven2025 home, so auto-load haven2025 from the same
    // folder and render the home inside the vista (one command: just open the vista APK).
    std::string companionPath;
    if (const char* bd = std::getenv("HSR_BACKDROP")) companionPath = bd;
    else if (apkPath.find("vista_") != std::string::npos) {
        size_t sl = apkPath.find_last_of("/\\");
        companionPath = (sl == std::string::npos ? std::string() : apkPath.substr(0, sl + 1)) + "haven2025.apk";
        fprintf(stderr, "[MAIN] vista_ env -> auto-companion home: %s\n", companionPath.c_str());
    }
    else if (apkPath.find("haven2025") != std::string::npos && !std::getenv("HSR_NOVISTA")) {
        // haven2025 is the HOME ONLY (levels = home_3d_props/staticarch_shell; NO vista level). On device
        // it is LIT + backdropped BY a vista (the dark void = the missing vista). Loaded alone it's flat
        // with a void sky. Auto-companion a default vista so it renders as intended ("lit from the vista").
        // Pick the first com_meta_shell_env_vista_*.apk that exists next to it (prefer calming). HSR_BACKDROP
        // overrides the vista; HSR_NOVISTA forces the bare home.
        size_t sl = apkPath.find_last_of("/\\");
        std::string dir = (sl == std::string::npos ? std::string() : apkPath.substr(0, sl + 1));
        const char* vistas[] = {"com_meta_shell_env_vista_calming.apk","com_meta_shell_env_vista_central.apk",
                                "com_meta_shell_env_vista_focused.apk","com_meta_shell_env_vista_oceanarium.apk"};
        for (const char* v : vistas) { std::string p = dir + v; FILE* f = fopen(p.c_str(),"rb"); if (f) { fclose(f); companionPath = p; break; } }
        if (!companionPath.empty()) fprintf(stderr, "[MAIN] haven2025 home -> auto-companion vista: %s\n", companionPath.c_str());
    }
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
        // RENDER BOTH: merge the companion env (vista backdrop + haven2025 home) at the shared origin.
        if (!companionPath.empty()) {
            static SceneLoader companion;
            companion.verbose = loader.verbose;
            fprintf(stderr, "[MAIN] Loading companion env: %s\n", companionPath.c_str());
            if (companion.load(companionPath)) {
                size_t before = loader.meshes.size();
                for (auto& m : companion.meshes) loader.meshes.push_back(std::move(m));
                fprintf(stderr, "[MAIN] Companion merged: +%zu meshes (total %zu)\n",
                        loader.meshes.size() - before, loader.meshes.size());
                // FAITHFUL CROSS-LOADER LIGHTMAPS: the vista ships the override HSTFs (GUID->lightmap); the
                // home ships the USD templates (GUID->mesh name) + the room meshes. Combine via the GUID chain
                // so each mesh gets its REAL baked lightmap (incl shared merge-group atlases) = the device look.
                loader.applyLightmapOverrides(loader.meshes, &companion);
            } else fprintf(stderr, "[MAIN] Companion load FAILED: %s\n", companionPath.c_str());
        }
        if (companionPath.empty()) loader.applyLightmapOverrides(loader.meshes);   // single env (no companion)
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
    if (!companionPath.empty()) loadShadersFromApk(companionPath);  // companion's shaders too (both envs render)
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
    std::vector<Prog> allVariants;   // EVERY (vert,frag) pair per surface — per-material picks the right one
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
            auto fragSamplesTex = [](const std::vector<u32>& frag) -> bool {
                for (size_t i = 5; i + 1 < frag.size(); ) {
                    u32 op = frag[i] & 0xFFFF, wc = frag[i] >> 16; if (!wc) break;
                    if (op == 25 || op == 27) return true;
                    i += wc;
                }
                return false;
            };
            size_t best = 0; size_t bestSz = 0;
            for (size_t i = 0; i < nPair; ++i) {
                if (fb.frags[i].size() > bestSz) { bestSz = fb.frags[i].size(); best = i; }
                // keep EVERY variant for the per-material path (so it can pick the one whose samplers
                // match the material's textureParameters slots — the builder otherwise grabs the largest
                // übershader variant, which uses samplers/inputs we don't feed -> washed/flat).
                Prog pv; pv.name = name; pv.vert = fb.verts[i]; pv.frag = fb.frags[i];
                pv.fragTex = fragSamplesTex(fb.frags[i]); allVariants.push_back(std::move(pv));
            }
            Prog p;
            p.name = name;
            p.vert = fb.verts[best];
            p.frag = fb.frags[best];
            p.fragTex = fragSamplesTex(p.frag);
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
    // COOKING MODE (HSR_EXPORT) = console-only: hide the window so it doesn't pop a white "frozen" window with no
    // feedback. The cook prints [GLTF]/[COOK]/[EXPORT] progress to the console; HSR_EXPORT_QUIT exits when done.
    bool g_cookHeadless = std::getenv("HSR_EXPORT") != nullptr;
    if (g_cookHeadless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    else glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);  // always-on-top so external screenshot capture is reliable
    // Title shows WHICH loader/format is active (V79 glTF / V79 OPA / HSL) + the env file name.
    std::string g_fmtName = isV79 ? "V79 glTF" : (isOpa ? "V79 OPA" : "HSL");
    std::string g_baseName = apkPath; { size_t sl = g_baseName.find_last_of("/\\"); if (sl != std::string::npos) g_baseName = g_baseName.substr(sl + 1); }
    std::string g_title = "HSR Renderer [Vulkan]  —  " + g_fmtName + "  —  " + g_baseName +
                          "   |   WASD=move drag=look  Tab/N=next mesh B=prev  F=wire  Esc=quit";
    g_window = glfwCreateWindow(g_winW, g_winH, g_title.c_str(), nullptr, nullptr);
    if (!g_window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }
    { GLFWimage icons[2]; std::vector<unsigned char> p48, p32; genEditorIcon(48,p48); genEditorIcon(32,p32);  // window/taskbar icon
      icons[0]={48,48,p48.data()}; icons[1]={32,32,p32.data()}; glfwSetWindowIcon(g_window, 2, icons); }

    glfwSetKeyCallback(g_window, keyCb);
    glfwSetMouseButtonCallback(g_window, mouseBtnCb);
    glfwSetCursorPosCallback(g_window, cursorCb);
    glfwSetScrollCallback(g_window, scrollCb);
    glfwSetCharCallback(g_window, charCb);    // text input for the editor's fields (package name, search)
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
        for (auto& p : allVariants)
            vkRenderer.loadedShaders.push_back({ VkRenderer::surfaceName(p.name), p.vert, p.frag });
        fprintf(stderr, "[MAIN] per-material shaders ON (%zu variants)%s\n", allVariants.size(),
                envShippedRendShad ? " [auto: V203 env ships RENDSHAD]" : " [HSR_PERMAT]");
    }

    // ── Load the env's REAL lightprobe ambient from its .lprb (FlatBuffer, magic "LPRB"). field2 = the merged
    // global SH; field2[0..2] = L00 RGB. The PBR shaders' lightprobesParams DC = ambientRGB*3.5449, so the
    // device ambient radiance = L00/3.5449. Using it (instead of the synthesized warm-white + 1.8x boost) stops
    // the ~4x over-bright wash that turned the greenish ground white. Reversed from libshell LightprobeNetwork /
    // the lightprobesParamsTag(L00..L22) shader layout. Must run BEFORE vkRenderer.init() (it uses ambientRGB).
    if (!std::getenv("HSR_NOENVAMB")) {
        mz_zip_archive aZ; memset(&aZ,0,sizeof aZ);
        if (mz_zip_reader_init_file(&aZ, apkPath.c_str(), 0)) {
            int si = mz_zip_reader_locate_file(&aZ, "assets/scene.zip", nullptr, 0);
            size_t szSz=0; void* szD = si>=0 ? mz_zip_reader_extract_to_heap(&aZ, si, &szSz, 0) : nullptr;
            mz_zip_reader_end(&aZ);
            if (szD) {
                mz_zip_archive sZ; memset(&sZ,0,sizeof sZ);
                if (mz_zip_reader_init_mem(&sZ, szD, szSz, 0)) {
                    mz_uint nf = mz_zip_reader_get_num_files(&sZ);
                    for (mz_uint i=0;i<nf;i++){
                        mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&sZ,i,&st)) continue;
                        if (!strstr(st.m_filename, "lightprobes_merged.lprb")) continue;
                        size_t fsz=0; void* fd = mz_zip_reader_extract_to_heap(&sZ, i, &fsz, 0);
                        if (fd && fsz>80) {
                            const unsigned char* d=(const unsigned char*)fd;
                            if (memcmp(d+4,"LPRB",4)==0) {
                                uint32_t root=*(const uint32_t*)d;
                                if ((size_t)root+4<=fsz) {
                                    uint32_t vt=root-(uint32_t)(*(const int32_t*)(d+root));
                                    if ((size_t)vt+10<=fsz && *(const uint16_t*)(d+vt)>=10) {
                                        uint16_t f2voff=*(const uint16_t*)(d+vt+8);   // field2 = vtable slot 2
                                        if (f2voff && (size_t)root+f2voff+4<=fsz) {
                                            uint32_t f2abs=root+f2voff;
                                            uint32_t vecPos=f2abs + *(const uint32_t*)(d+f2abs);
                                            if ((size_t)vecPos+16<=fsz && *(const uint32_t*)(d+vecPos)>=3) {
                                                const float* sh=(const float*)(d+vecPos+4);
                                                vkRenderer.ambientRGB[0]=sh[0]/3.5449f;
                                                vkRenderer.ambientRGB[1]=sh[1]/3.5449f;
                                                vkRenderer.ambientRGB[2]=sh[2]/3.5449f;
                                                vkRenderer.hasEnvAmbient=true;
                                                fprintf(stderr, "[MAIN] env lightprobe ambient = (%.3f,%.3f,%.3f) from %s\n",
                                                        vkRenderer.ambientRGB[0],vkRenderer.ambientRGB[1],vkRenderer.ambientRGB[2], st.m_filename);
                                            }
                                        }
                                    }
                                }
                            }
                            mz_free(fd);
                        }
                        break;
                    }
                    mz_zip_reader_end(&sZ);
                }
                mz_free(szD);
            }
        }
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
    // V203 skybox-IBL (gated HSR_SKYIBL): feed the captured EQUIRECT reflectionMap panorama into the SpecIbl
    // per-vertex bake (equirect -> cube via ibl::equirectToCubemap). Off by default -> no regression to envs
    // that already render via baked lightmaps. Must precede the mesh upload (the bake runs in uploadMesh).
    if (std::getenv("HSR_SKYIBL") && !loader.iblEquirectRGBA8.empty())
        vkRenderer.setIblEquirectRGBA8(loader.iblEquirectRGBA8.data(), loader.iblEqW, loader.iblEqH);

    // Upload meshes (HSR_MAXMESH / HSR_MINMESH env limit the range for crash bisection)
    // Device-faithful clip planes from the env's space.hstf (HSR_CLIP toggles them on in fitFarToScene).
    vkRenderer.setSceneClip(loader.sceneNearClip, loader.sceneFarClip);
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
    if (const char* sel = std::getenv("HSR_SELECT")) { vkRenderer.selectedMesh = atoi(sel); }  // headless test of selection-highlight (Pass 3)
    if (std::getenv("HSR_SHOWOVERLAY")) { vkRenderer.showNavmesh = vkRenderer.showCollision = vkRenderer.showSpawn = true; }  // headless overlay test
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
    // ── Live control mode (HSR_LIVE): load the scene ONCE, then drive camera / visibility /
    // screenshots by writing lines to a control file (no relaunch, no mesh reload). One persistent
    // instance serves every camera angle + shot. Commands (one per line in the control file):
    //   cam=x,y,z,yawDeg,pitchDeg | shot=path.png | hidemesh=<idx> | solomesh=<idx>
    //   hidemat=<substr> | solomat=<substr> | wire=0/1 | clear | quit
    // After each shot the renderer writes _live_ack.txt so the driver knows the PNG is ready.
    const char* liveEnv  = std::getenv("HSR_LIVE");
    bool        liveMode = liveEnv != nullptr;
    std::string pendingShot, pendingShotOut;
#ifdef _WIN32
    LiveCmd*    pendingShotCmd = nullptr;   // the command whose shot completes after the next render
#endif
    if (liveMode) {
        shotQuit = false; shotPath = nullptr;   // never auto-quit / one-shot in live mode
#ifdef _WIN32
        int port = atoi(liveEnv); if (port <= 1) port = 8777;
        std::thread(hsrHttpServer, port).detach();
        fprintf(stderr, "[LIVE] ready — HTTP control on :%d (curl --data 'cam=..\\nshot=path' 127.0.0.1:%d)\n", port, port);
#endif
    }
    auto animStart = std::chrono::high_resolution_clock::now();
    // HSR_ANIMTIME=<sec> forces a fixed animation pose (deterministic capture / debugging).
    const char* animTimeEnv = std::getenv("HSR_ANIMTIME");
    float fixedAnimTime = animTimeEnv ? (float)atof(animTimeEnv) : -1.0f;

    // ── Ambient audio: loop the env's theme from the APK. Accepts ANY common container (ogg/wav/mp3/flac): the
    //    desktop preview decodes it universally (audioconv) and the cook ships it as a SoundAsset — FMOD-native
    //    containers raw, anything else transcoded to WAV. This is the "audio conversion" path. ──
    AudioPlayer g_audio;
    std::vector<u8> audioRaw;   // the env's theme as found in the APK, raw bytes (whatever container)
    {
        static const char* AUD_EXT[] = { ".ogg", ".wav", ".mp3", ".flac" };
        mz_zip_archive az; memset(&az, 0, sizeof(az));
        if (mz_zip_reader_init_file(&az, apkPath.c_str(), 0)) {
            int si = mz_zip_reader_locate_file(&az, "assets/scene.zip", nullptr, 0);
            if (si >= 0) {
                size_t szN = 0; void* szD = mz_zip_reader_extract_to_heap(&az, si, &szN, 0);
                if (szD) {
                    mz_zip_archive sz; memset(&sz, 0, sizeof(sz));
                    if (mz_zip_reader_init_mem(&sz, szD, szN, 0)) {
                        u32 nf = mz_zip_reader_get_num_files(&sz);
                        for (u32 i = 0; i < nf && audioRaw.empty(); ++i) { mz_zip_archive_file_stat st;
                            if (!mz_zip_reader_file_stat(&sz, i, &st)) continue;
                            std::string fn(st.m_filename);
                            for (const char* ext : AUD_EXT) {
                                size_t el = strlen(ext);
                                if (fn.size() >= el && fn.compare(fn.size()-el, el, ext) == 0) {
                                    size_t on = 0; void* od = mz_zip_reader_extract_to_heap(&sz, i, &on, 0);
                                    if (od) { audioRaw.assign((u8*)od, (u8*)od + on); mz_free(od); }
                                    break; }
                            }
                        }
                        mz_zip_reader_end(&sz);
                    }
                    mz_free(szD);
                }
            }
            mz_zip_reader_end(&az);
        }
    }
    std::vector<u8> ogg;   // the bytes the cook actually ships (FMOD-native container raw, or transcoded WAV)
    if (!audioRaw.empty()) {
        const char* fmt = audioconv::sniff(audioRaw.data(), audioRaw.size());
        audioconv::Pcm pcm; std::string aerr;
        bool dec = audioconv::decode(audioRaw.data(), audioRaw.size(), pcm, &aerr);
        if (dec && !std::getenv("HSR_NOAUDIO")) g_audio.startPCM(pcm.samples.data(), pcm.frames(), pcm.channels, pcm.sampleRate);
        else if (!dec) fprintf(stderr, "[AUDIO] decode failed (%s): %s\n", fmt, aerr.c_str());
        if (audioconv::fmodNative(fmt)) {
            ogg = audioRaw;   // FMOD reads ogg/wav/mp3 directly -> ship raw (compact)
            fprintf(stderr, "[AUDIO] theme '%s' (%d Hz %d ch) -> shipped raw\n", fmt, pcm.sampleRate, pcm.channels);
        } else if (dec) {
            ogg = audioconv::toWav(pcm);   // e.g. flac -> WAV for the device's FMOD
            fprintf(stderr, "[AUDIO] theme '%s' (%d Hz %d ch) -> converted to WAV for cook (%zu KB)\n", fmt, pcm.sampleRate, pcm.channels, ogg.size()/1024);
        }
    } else {
        fprintf(stderr, "[AUDIO] no audio (ogg/wav/mp3/flac) in this env\n");
    }

    // ── Editor UI (Dear ImGui): outliner, move, focus, anim/audio control, save ──
    // V203 envs animate via getTime()-driven shaders (VAT creatures, UV scroll, material/flipbook anims) + HZANIM
    // clips — all continuous, no single global clip duration. animDur=0 froze the editor timeline (playhead never
    // advances -> *animScrub stuck at 0 -> every time-driven anim FROZEN at t=0 in the editor). Give v203 a 60s
    // looping timeline so the playhead advances + the anims play/scrub (headless render uses real elapsed time, so
    // it's unaffected). OPA/V79 keep their real clip duration.
    float animDur = isOpa ? opa.animDuration() : (isV79 ? gltf.animDuration : 60.0f);
    Editor editor;
    g_editor = &editor;            // expose to the GLFW input callbacks
    editor.r = &vkRenderer;             // bind the renderer up-front so Export works even when the UI is skipped (HSR_NOUI)
    editor.sceneMeshes = sceneMeshes;   // CPU geometry/textures for the "Export APK" cooker (parallel to gpuMeshes)
    editor.bgOgg = ogg;                 // the env's background loop -> cooked as an auto-start FMOD SoundAsset
    // Auto-import the V79 env's assets/markup.json (portal->Spawn, seat-hotspots->Chair, others->Hotspot) as editable items
    if (!apkPath.empty()) {
        mz_zip_archive mkz; memset(&mkz, 0, sizeof(mkz));
        if (mz_zip_reader_init_file(&mkz, apkPath.c_str(), 0)) {
            int mi = mz_zip_reader_locate_file(&mkz, "assets/markup.json", nullptr, 0);
            if (mi >= 0) { size_t msz=0; void* md=mz_zip_reader_extract_to_heap(&mkz, mi, &msz, 0); if (md) { editor.importMarkup(std::string((char*)md, msz)); mz_free(md); } }
            mz_zip_reader_end(&mkz);
        }
    }
    // (navmesh is MANUAL: multi-select the meshes you want walkable -> "+ Add" -> Navmesh (from selection) -> Cook)
    // VAT (vertex-animation) bakes the per-frame V79 node deformation into a shader-sampled offset texture.
    // ⛔ DEVICE-PROVEN 2026-06-10: the VAT EXPORT path does NOT render on the Quest (the cooked VAT meshes are
    // invisible in-headset — the erebor wisp sparkles vanished when VAT was default-on), whereas the poseAnim path
    // (ShellPoseAnimationComponent + unlitblend transparent material) renders + fades correctly. So VAT export is
    // OPT-IN ONLY (HSR_VAT) until the on-device VAT entity setup is reversed; the DEFAULT wisp port = poseAnim.
    if (isV79 && std::getenv("HSR_VAT")) editor.vatBaker = [&gltf](int meshIdx, int frames, int& nv){ return gltf.bakeVAT(meshIdx, frames, nv); };
    if (isV79) editor.hzAnimExtractor = [&gltf](int meshIdx, int frames, hslcook::ExportMesh& em){   // HZANIM skeletal port
        // FLIPBOOK: an animated flat material (node-animated, no skin) -> route to the GPU flipbook shader (no skeleton).
        // HSR_FLIPGRID=CxR sets the spritesheet grid (default 11x11 from the template).
        if (std::getenv("HSR_FLIPBOOK") && gltf.isNodeAnimated(meshIdx)) {
            em.flipbook = true;
            if (const char* g = std::getenv("HSR_FLIPGRID")) { int c=0,r=0; if (sscanf(g,"%dx%d",&c,&r)==2){ em.flipCols=c; em.flipRows=r; } }
        }
        // V203 wisp port. ⛔ DEVICE-PROVEN 2026-06-10: the getTime() PULSE shader is the DEFAULT — it self-loops off
        // globalUniforms.time and fades the wisp's opacity+brightness CONTINUOUSLY ("slow fade in/out"), on the shipped
        // unlitblend base that renders. ShellPoseAnimationComponent is a 2-keyframe ONE-SHOT (sub_14651D8) that plays
        // once then HOLDS static ("visible but not moving"), so it's opt-in only (HSR_POSEANIM). HSR_NOPULSE = off.
        else if (gltf.isNodeAnimated(meshIdx) && !std::getenv("HSR_NOPULSE")) {
            // Y-ROTATION (Outer Wilds skybox/Interloper) takes precedence: a uniform node-spin -> getTime() Y-rotation
            // shader. Falls through to the wisp scale-pulse for non-rotation node anims (erebor flames have no rot channel).
            float rax[3] = {0,1,0}, rom = 0.f, rpiv[3] = {0,0,0}, ramp = 0.f, rper = 0.f; bool rosc = false;
            if (!std::getenv("HSR_NOROT") && gltf.extractNodeRotation(meshIdx, rax, rom, rpiv, rosc, ramp, rper)) {
                em.rotAnim = true; em.rotOmega = rom; em.rotOsc = rosc; em.rotAmp = ramp; em.rotPeriod = rper;
                em.rotAxis[0]=rax[0]; em.rotAxis[1]=rax[1]; em.rotAxis[2]=rax[2];
                em.rotPivot[0]=rpiv[0]; em.rotPivot[1]=rpiv[1]; em.rotPivot[2]=rpiv[2];
                em.vatOffsets.clear(); em.vatFrames = 0;   // rotation/sway -> getTime() Rodrigues shader, NOT VAT (vatBaker also ran on this node-anim mesh)
            } else if (std::getenv("HSR_POSEANIM") && gltf.extractNodeScaleAnim(meshIdx, em.poseStartScale, em.poseEndScale, em.poseDuration))
                em.poseAnim = true;
            else em.pulse = true;
        }
        if (gltf.isNodeAnimated(meshIdx) && std::getenv("HSR_VERBOSE")) gltf.dumpNodeAnimTrack(meshIdx);
        // FRAME COUNT must track the clip DURATION, not a fixed 64: snakeway's dragon "Action" is 31.2s — 64 frames =
        // ~2fps → the body-undulation is massively undersampled → "moves but not the original animation". Sample at a
        // real rate (~30fps) so the wave is captured. ACL compresses the extra frames cheaply. HSR_HZFPS overrides.
        // The HZAN clip has a HARD device size limit: a ~22 KB clip (256 frames) LOADS, ~65 KB (935 frames) throws
        // std::length_error on load (the device reads the clip block size into a SIGNED 16-bit field → >32767 goes
        // negative → bad string/vector resize). So CAP the frame count (not a fixed 64 — that under-samples a 31 s clip
        // to ~2 fps = "moves but not the original animation"). 256 frames is device-confirmed safe; for snakeway's 31 s
        // dragon that's ~8 fps (4× smoother than 64). HSR_HZFPS / HSR_HZMAXFRAMES override.
        { float hzfps = 30.f; if (const char* e2 = std::getenv("HSR_HZFPS")) { float v = (float)atof(e2); if (v >= 1.f) hzfps = v; }
          int cap = 256; if (const char* e3 = std::getenv("HSR_HZMAXFRAMES")) { int v = atoi(e3); if (v >= 8) cap = v; }
          int nf = (gltf.animDuration > 0.f) ? (int)(gltf.animDuration * hzfps + 0.5f) : frames;
          if (nf < 64) nf = 64; if (nf > cap) nf = cap;   // floor 64 (short clips); cap to keep the ACL clip under the device size limit
          frames = nf; }
        auto e = gltf.extractHzAnim(meshIdx, frames);
        if (e.ok()) { em.hzJointPos=std::move(e.jointPos); em.hzJointQuat=std::move(e.jointQuat); em.hzJointScale=std::move(e.jointScale);
                      em.hzParents=std::move(e.parents); em.hzBoneIdx=std::move(e.boneIdx); em.hzBoneWgt=std::move(e.boneWgt);
                      em.hzTrsLocal=std::move(e.trsLocal); em.hzRestPos=std::move(e.restPos); em.hzJointCount=e.jointCount; em.hzFrames=e.frameCount; em.hzFps=e.fps; }
    };
    // ── OPA node-animation port: batch-fit every animated OPA mesh to a spin/sway and feed the SAME getTime()
    //    Rodrigues shader path the glTF rotations use (node TRANSFORM anims are the bulk of OPA motion). ──
    std::unordered_map<size_t, noderot::Result> g_opaRot;
    std::unordered_map<size_t, std::pair<float,float>> g_opaUv;
    if (isOpa && !std::getenv("HSR_NOROT")) {
        opa.cookExtractRotations(g_opaRot);
        opa.cookExtractUVScroll(g_opaUv);   // mat.sanim water/foam UV scrolls
        fprintf(stderr, "[OPA] cook anim: %zu spin/sway + %zu uv-scroll (of %zu meshes)\n", g_opaRot.size(), g_opaUv.size(), opa.meshes.size());
        editor.hzAnimExtractor = [&g_opaRot,&g_opaUv,&opa](int meshIdx, int frames, hslcook::ExportMesh& em){
            (void)frames;
            // OPA skeletal/rigid HZANIM port — DEFAULT ON (faithful animation). Was gated after an early cooked APK
            // crashed, but the incredibles skinned fix ([[project_hsr_skinned_rendmesh_skinblock]]) made HZANIM stable
            // on device (cyberhome: loads, no crash, ErrorNotReady only transient). Opt-out via HSR_NOOPAHZ.
            if (!std::getenv("HSR_NOOPAHZ")) {
            // SKINNED HZANIM (door/discs/screens — ALL skinned meshes). Faithful hierarchical → HZAN:SKEL + ACL clip.
            auto e = opa.extractHzAnim(meshIdx);
            if (e.ok()) {
                em.hzJointPos=std::move(e.jointPos); em.hzJointQuat=std::move(e.jointQuat); em.hzJointScale=std::move(e.jointScale);
                em.hzParents=std::move(e.parents); em.hzBoneIdx=std::move(e.boneIdx); em.hzBoneWgt=std::move(e.boneWgt);
                em.hzTrsLocal=std::move(e.trsLocal); em.hzRestPos=std::move(e.restPos);
                em.hzJointCount=e.jointCount; em.hzFrames=e.frameCount; em.hzFps=e.fps;
                em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                return;
            }
            // NON-skinned node TRANSLATION (cars/train) -> 1-joint RIGID HZANIM (faithful arbitrary path). Returns
            // !ok() for pure spins (no translation), which fall through to the lighter getTime() Rodrigues path.
            auto rg = opa.extractNodeRigidHzAnim(meshIdx);
            if (rg.ok()) {
                em.hzJointPos=std::move(rg.jointPos); em.hzJointQuat=std::move(rg.jointQuat); em.hzJointScale=std::move(rg.jointScale);
                em.hzParents=std::move(rg.parents); em.hzBoneIdx=std::move(rg.boneIdx); em.hzBoneWgt=std::move(rg.boneWgt);
                em.hzTrsLocal=std::move(rg.trsLocal); em.hzRestPos=std::move(rg.restPos);
                em.hzJointCount=rg.jointCount; em.hzFrames=rg.frameCount; em.hzFps=rg.fps;
                em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                return;
            }
            }   // end HSR_OPAHZ gate
            // CARS/TRAIN: node TRANSLATION -> ShellPoseAnimationComponent (the FAITHFUL, device-proven node-anim port;
            // NO skin, so MeshDefinition::fix can't reject it like the 1-joint rigid did). Mesh stays static; the entity
            // pose lerps rest -> rest+delta over the clip. Pure spins (no translation) fall through to the spin shader.
            { float tdelta[3];
              if (opa.extractNodeTranslate(meshIdx, tdelta)) {
                  em.poseAnim=true; em.poseDuration = opa.animDuration()>0.f ? opa.animDuration() : 2.f;
                  em.poseTransDelta[0]=tdelta[0]; em.poseTransDelta[1]=tdelta[1]; em.poseTransDelta[2]=tdelta[2];
                  em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                  return;
              } }
            auto it = g_opaRot.find((size_t)meshIdx);
            if (it != g_opaRot.end()) { const noderot::Result& r = it->second;
                em.rotAnim=true; em.rotOmega=r.omega; em.rotOsc=r.isOsc; em.rotAmp=r.amp; em.rotPeriod=r.period;
                em.rotAxis[0]=r.axis[0]; em.rotAxis[1]=r.axis[1]; em.rotAxis[2]=r.axis[2];
                em.rotPivot[0]=r.pivot[0]; em.rotPivot[1]=r.pivot[1]; em.rotPivot[2]=r.pivot[2];
                em.vatOffsets.clear(); em.vatFrames=0; }
            auto uit = g_opaUv.find((size_t)meshIdx);   // UV scroll (cooker prioritizes rotation over scroll if both)
            if (uit != g_opaUv.end()) { em.uvScroll=true; em.uvRate[0]=uit->second.first; em.uvRate[1]=uit->second.second; }
        };
    }
    // Cook package = the LOADED env's OWN package (read from its AndroidManifest), not a hardcoded one.
    {
        mz_zip_archive mz; memset(&mz, 0, sizeof mz);
        if (mz_zip_reader_init_file(&mz, apkPath.c_str(), 0)) {
            int mi = mz_zip_reader_locate_file(&mz, "AndroidManifest.xml", nullptr, 0);
            if (mi >= 0) { size_t msz=0; void* md=mz_zip_reader_extract_to_heap(&mz, mi, &msz, 0);
                if (md) { std::vector<uint8_t> ax((uint8_t*)md,(uint8_t*)md+msz); std::string pkg=hslcook::readAxmlPackage(ax);
                    if (!pkg.empty()) { editor.cookPkg = pkg; fprintf(stderr,"[MAIN] env package from manifest: %s (cook target)\n", pkg.c_str()); }
                    mz_free(md); } }
            mz_zip_reader_end(&mz);
        }
    }
    if (!std::getenv("HSR_NOUI")) {  // HSR_NOUI = clean capture without the editor overlay
        editor.init(&vkRenderer, g_window, &g_audio, &g_animOverride, &g_animScrub, animDur);
        editor.projectPath = apkPath;   // session saves/loads to <env>.hsledit
        editor.loadProject();           // auto-restore a prior session (transforms/renames/items) if one exists
    }

    // One-shot headless re-cook: HSR_EXPORT exports the loaded (optionally edited) scene to an APK then,
    // with HSR_EXPORT_QUIT, exits — lets the editor's Export path run batch / from the command line.
    if (std::getenv("HSR_EXPORT")) {
        if (editor.projectPath.empty()) { editor.projectPath = apkPath; editor.loadProject(); }   // headless cook includes the saved session
        editor.exportAPKSync();   // synchronous cook + auto-sign with a terminal progress bar
        if (std::getenv("HSR_EXPORT_QUIT")) return 0;
    }

    while (!glfwWindowShouldClose(g_window)) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        // ── Live control: drain HTTP-queued command batches. Camera/visibility apply immediately;
        // a shot is deferred to AFTER this frame renders (so it reflects the new camera) and then the
        // command's HTTP response is completed. farscan/listmesh write their dump into the response.
        // Commands (one per body line): cam=x,y,z,yaw,pitch | move=dx,dy,dz | fov=deg | far=val
        //   bg=r,g,b (clear colour; bg=1,1,1 = WHITE sky to expose ground HOLES) | wire=0/1
        //   hidemesh=i | solomesh=i | hidemat=sub | solomat=sub | clear | shot=path.png
        //   listmesh=sub | farscan[=thr] (meshes with a vertex |world coord|>thr — finds mis-decoded
        //   geometry flung far away that tears holes) | quit
#ifdef _WIN32
        if (liveMode) {
            std::vector<LiveCmd*> batch;
            { std::lock_guard<std::mutex> g(g_liveMx); while (!g_liveQ.empty()) { batch.push_back(g_liveQ.front()); g_liveQ.pop_front(); } }
            for (LiveCmd* lc : batch) {
                std::vector<char> mb(lc->text.begin(), lc->text.end()); mb.push_back(0);
                std::string out; std::string shotThis;
                auto& C = vkRenderer.cam;
                for (char* ln = strtok(mb.data(), "\r\n"); ln; ln = strtok(nullptr, "\r\n")) {
                    while (*ln == ' ' || *ln == '\t') ++ln;
                    if (!*ln || *ln == '#') continue;
                    float x, y, z, yd, pd, r, g, b; char tmp[256];
                    if (sscanf(ln, "cam=%f,%f,%f,%f,%f", &x, &y, &z, &yd, &pd) == 5) {
                        C.pos[0] = x; C.pos[1] = y; C.pos[2] = z;
                        C.yaw = yd * 3.14159265f / 180.0f; C.pitch = pd * 3.14159265f / 180.0f;
                    } else if (sscanf(ln, "move=%f,%f,%f", &x, &y, &z) == 3) { C.pos[0]+=x; C.pos[1]+=y; C.pos[2]+=z; }
                    else if (sscanf(ln, "bg=%f,%f,%f", &r, &g, &b) == 3) { vkRenderer.clearRGB[0]=r; vkRenderer.clearRGB[1]=g; vkRenderer.clearRGB[2]=b; }
                    else if (strncmp(ln, "fov=", 4) == 0)         C.fovDeg = (float)atof(ln + 4);
                    else if (strncmp(ln, "far=", 4) == 0)         C.farZ = (float)atof(ln + 4);
                    else if (strncmp(ln, "shot=", 5) == 0)        shotThis = ln + 5;
                    else if (strncmp(ln, "hidemesh=", 9) == 0)    vkRenderer.hideMesh = atoi(ln + 9);
                    else if (strncmp(ln, "solomesh=", 9) == 0)    vkRenderer.soloMesh = atoi(ln + 9);
                    else if (strncmp(ln, "movemesh=", 9) == 0) {   // live-edit: world-translate ONE mesh (test placement fixes without recompiling)
                        int mi; float dx, dy, dz;
                        if (sscanf(ln + 9, "%d,%f,%f,%f", &mi, &dx, &dy, &dz) == 4 && mi >= 0 && mi < (int)vkRenderer.gpuMeshes.size()) {
                            vkRenderer.gpuMeshes[mi].model[12] += dx; vkRenderer.gpuMeshes[mi].model[13] += dy; vkRenderer.gpuMeshes[mi].model[14] += dz;
                            snprintf(tmp, sizeof tmp, "moved mesh %d by (%.2f,%.2f,%.2f)\n", mi, dx, dy, dz); out += tmp;
                        }
                    }
                    else if (strncmp(ln, "frame=", 6) == 0) {   // auto-frame the camera on mesh idx's world AABB (same as HSR_SOLO auto-frame)
                        int mi = atoi(ln + 6);
                        if (mi >= 0 && mi < (int)sceneMeshes->size() && mi < (int)vkRenderer.gpuMeshes.size()) {
                            const auto& md = (*sceneMeshes)[mi]; const float* M = vkRenderer.gpuMeshes[mi].model;
                            float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; size_t nv=md.positions.size()/3;
                            for (size_t i=0;i<nv;i++){
                                float lx=md.positions[i*3],ly=md.positions[i*3+1],lz=md.positions[i*3+2];
                                float wx=M[0]*lx+M[4]*ly+M[8]*lz+M[12], wy=M[1]*lx+M[5]*ly+M[9]*lz+M[13], wz=M[2]*lx+M[6]*ly+M[10]*lz+M[14];
                                if(wx<mn[0])mn[0]=wx; if(wx>mx[0])mx[0]=wx; if(wy<mn[1])mn[1]=wy; if(wy>mx[1])mx[1]=wy; if(wz<mn[2])mn[2]=wz; if(wz>mx[2])mx[2]=wz;
                            }
                            if (nv>0){
                                float cx=(mn[0]+mx[0])*0.5f, cy=(mn[1]+mx[1])*0.5f, cz=(mn[2]+mx[2])*0.5f;
                                float rx=mx[0]-mn[0],ry=mx[1]-mn[1],rz=mx[2]-mn[2]; float radius=0.5f*sqrtf(rx*rx+ry*ry+rz*rz); if(radius<0.01f)radius=0.5f;
                                float fov=C.fovDeg*3.14159265f/180.0f; float dist=radius/tanf(fov*0.5f)*1.5f;
                                C.pos[0]=cx; C.pos[1]=cy; C.pos[2]=cz+dist; C.yaw=0.0f; C.pitch=0.0f;
                                snprintf(tmp,sizeof tmp,"framed mesh %d c=(%.2f,%.2f,%.2f) r=%.2f dist=%.2f\n", mi,cx,cy,cz,radius,dist); out+=tmp;
                            } else { snprintf(tmp,sizeof tmp,"mesh %d has no verts\n", mi); out+=tmp; }
                        }
                    }
                    else if (strncmp(ln, "matinfo=", 8) == 0) {   // dump a mesh's shader + VAT/bones/blend flags (diagnose fog/waterfall/plant/flock)
                        int mi = atoi(ln + 8);
                        if (mi >= 0 && mi < (int)sceneMeshes->size() && mi < (int)vkRenderer.gpuMeshes.size()) {
                            const auto& md = (*sceneMeshes)[mi]; const auto& gm = vkRenderer.gpuMeshes[mi];
                            snprintf(tmp,sizeof tmp,"[MATINFO %d] '%s' shader=%s\n", mi, md.name.c_str(), md.shaderPath.c_str()); out += tmp;
                            snprintf(tmp,sizeof tmp,"  hasVat=%d hasBones=%d isSkinned=%d tiled=%d tex=%dx%d mdBlend=%d gmBlend=%d add=%d progIdx=%d stride=%u uvOff=%u\n",
                                (int)md.hasVat,(int)md.hasBones,(int)gm.isSkinned,(int)md.tiled,md.texW,md.texH,(int)md.useBlend,(int)gm.useBlend,(int)gm.additive,gm.progIdx,gm.vboStride,gm.uvOffset); out += tmp;
                        }
                    }
                    else if (strncmp(ln, "hidemat=", 8) == 0)     vkRenderer.hideMat = ln + 8;   // std::string; empty = none (checked via .empty())
                    else if (strncmp(ln, "solomat=", 8) == 0)     vkRenderer.soloMat = ln + 8;
                    else if (strncmp(ln, "wire=", 5) == 0)        vkRenderer.wireframe = atoi(ln + 5) != 0;
                    else if (strncmp(ln, "clear", 5) == 0)      { vkRenderer.hideMesh = -1; vkRenderer.soloMesh = -1; vkRenderer.hideMat.clear(); vkRenderer.soloMat.clear(); vkRenderer.wireframe = false; }
                    else if (strncmp(ln, "listmesh=", 9) == 0) {
                        const char* sub = ln + 9; size_t N = sceneMeshes->size();
                        snprintf(tmp,sizeof tmp,"[LISTMESH] '%s':\n",sub); out += tmp;
                        for (size_t mi = 0; mi < N && mi < vkRenderer.gpuMeshes.size(); ++mi) {
                            const auto& md = (*sceneMeshes)[mi];
                            if (md.name.find(sub) == std::string::npos) continue;
                            const float* M = vkRenderer.gpuMeshes[mi].model;
                            snprintf(tmp,sizeof tmp,"  [%zu] %s  nV=%zu  modelPos=(%.2f,%.2f,%.2f)\n",
                                    mi, md.name.c_str(), md.positions.size()/3, M[12], M[13], M[14]); out += tmp;
                        }
                    }
                    else if (strncmp(ln, "farscan", 7) == 0) {
                        float thr = 1000.0f; sscanf(ln + 7, "=%f", &thr);
                        size_t N = std::min(sceneMeshes->size(), vkRenderer.gpuMeshes.size());
                        snprintf(tmp,sizeof tmp,"[FARSCAN] meshes with a vertex |world coord| > %.0f (of %zu):\n", thr, N); out += tmp;
                        for (size_t mi = 0; mi < N; ++mi) {
                            const auto& md = (*sceneMeshes)[mi]; const float* M = vkRenderer.gpuMeshes[mi].model;
                            size_t nv = md.positions.size()/3; if (!nv) continue;
                            float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}, maxabs=0; size_t farv=0;
                            for (size_t i=0;i<nv;i++){
                                float lx=md.positions[i*3],ly=md.positions[i*3+1],lz=md.positions[i*3+2];
                                float wx=M[0]*lx+M[4]*ly+M[8]*lz+M[12];
                                float wy=M[1]*lx+M[5]*ly+M[9]*lz+M[13];
                                float wz=M[2]*lx+M[6]*ly+M[10]*lz+M[14];
                                mn[0]=std::min(mn[0],wx);mx[0]=std::max(mx[0],wx);
                                mn[1]=std::min(mn[1],wy);mx[1]=std::max(mx[1],wy);
                                mn[2]=std::min(mn[2],wz);mx[2]=std::max(mx[2],wz);
                                float aa=fabsf(wx); aa=std::max(aa,fabsf(wy)); aa=std::max(aa,fabsf(wz));
                                if(aa>maxabs)maxabs=aa; if(aa>thr)farv++;
                            }
                            if (maxabs > thr) {
                                snprintf(tmp,sizeof tmp,"  [%zu] %s  maxabs=%.0f farV=%zu/%zu  wAABB X[%.0f,%.0f] Y[%.0f,%.0f] Z[%.0f,%.0f]\n",
                                        mi, md.name.c_str(), maxabs, farv, nv, mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]); out += tmp;
                            }
                        }
                        out += "[FARSCAN] done\n";
                    }
                    else if (strncmp(ln, "quit", 4) == 0)         glfwSetWindowShouldClose(g_window, GLFW_TRUE);
                }
                if (shotThis.empty()) { lc->result = out.empty() ? "ok\n" : out; lc->done.store(true, std::memory_order_release); }
                else { pendingShot = shotThis; pendingShotOut = out; pendingShotCmd = lc; }  // finished after render (below)
            }
        }
#endif

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
        // (pick + the gizmo are handled immediately in editor.onMouseButton, using the viewport pane rect)

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
        // Live-mode capture: the camera was applied at the top of THIS frame, so the just-rendered
        // image reflects it. Write the PNG, then complete the HTTP response for that command.
        if (liveMode && !pendingShot.empty()) {
            vkRenderer.screenshot(pendingShot.c_str());
            fprintf(stderr, "[LIVE] shot -> %s\n", pendingShot.c_str());
#ifdef _WIN32
            if (pendingShotCmd) {
                pendingShotCmd->result = pendingShotOut + "shot " + pendingShot + "\n";
                pendingShotCmd->done.store(true, std::memory_order_release);
                pendingShotCmd = nullptr;
            }
#endif
            pendingShot.clear(); pendingShotOut.clear();
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
