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
#include <thread>   // background scene-load worker (the window/UI stay live while parsing)

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
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // HWND of the main window for the GDI loading splash
#endif

#include "core/types.h"
#include "core/camera.h"
#include "core/load_progress.h"
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
#include "io/gltf_export.h"   // Blender round-trip: env -> glTF 2.0 project
#include "io/gltf_import.h"   // Blender round-trip: re-import an edited glTF project
#include "miniz.h"
#ifdef _WIN32
#include <windows.h>
// (the old GL drop-splash is gone — the drop zone + loading progress paint into the MAIN window via GDI)
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
// ── In-window loading / drop-zone painter (GDI) ────────────────────────────────────────────────────
// Paints directly onto the main (NO_API) window's HWND while the scene loads on a worker thread, or
// while waiting for an APK drop — the ONE window is alive and informative from the first moment
// (no separate drop popup, no white frozen rect). Double-buffered; GDI stops the instant Vulkan
// takes over presentation of the same window.
static void paintLoadSplash(HWND hwnd, const char* envName, bool waitingForDrop) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC wdc = GetDC(hwnd);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ obmp = SelectObject(dc, bmp);
    // Backdrop: vertical gradient, same palette as the editor theme.
    { TRIVERTEX v[2] = { { 0, 0, 0x1a00, 0x1d00, 0x2800, 0 }, { w, h, 0x0c00, 0x0d00, 0x1400, 0 } };
      GRADIENT_RECT gr = { 0, 1 }; GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V); }
    SetBkMode(dc, TRANSPARENT);
    int bigPx = h / 16 > 26 ? h / 16 : 26, smallPx = h / 32 > 15 ? h / 32 : 15;
    HFONT fBig = CreateFontA(-bigPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HFONT fSmall = CreateFontA(-smallPx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ ofont = SelectObject(dc, fBig);
    auto center = [&](int y, const char* s, COLORREF col) {
        SIZE sz; GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
        SetTextColor(dc, col); TextOutA(dc, (w - sz.cx) / 2, y, s, (int)strlen(s));
        return sz.cy;
    };
    if (waitingForDrop) {
        // Drop-zone state: dashed border + headline, integrated into the main window.
        int mx = w / 8, my = h / 5;
        HPEN pen = CreatePen(PS_DASH, 1, RGB(86, 132, 168)); HGDIOBJ open = SelectObject(dc, pen);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, mx, my, w - mx, h - my);
        SelectObject(dc, open); SelectObject(dc, obr); DeleteObject(pen);
        int ty = h / 2 - bigPx;
        ty += center(ty, "Drop an environment .apk here", RGB(214, 222, 235)) + 10;
        SelectObject(dc, fSmall);
        center(ty, "V79 / OPA / V203 / glTF project - the editor loads it right here", RGB(128, 140, 160));
    } else {
        const char* stage = g_loadProgress.stage.load(std::memory_order_relaxed);
        int cur = g_loadProgress.cur.load(std::memory_order_relaxed);
        int total = g_loadProgress.total.load(std::memory_order_relaxed);
        int y = h / 2 - bigPx * 2;
        char title[512]; snprintf(title, sizeof title, "Loading  %s", envName ? envName : "");
        y += center(y, title, RGB(214, 222, 235)) + bigPx / 2;
        SelectObject(dc, fSmall);
        char st[256];
        if (total > 0) snprintf(st, sizeof st, "%s   (%d / %d)", stage ? stage : "", cur, total);
        else           snprintf(st, sizeof st, "%s", stage ? stage : "");
        y += center(y, st, RGB(150, 164, 186)) + smallPx + 8;
        // Progress bar: determinate fill when total known, sweeping marquee otherwise.
        int bw = w / 2, bh = smallPx > 12 ? smallPx : 12, bx = (w - bw) / 2;
        RECT bar = { bx, y, bx + bw, y + bh };
        HBRUSH trough = CreateSolidBrush(RGB(34, 38, 52)); FillRect(dc, &bar, trough); DeleteObject(trough);
        HBRUSH fill = CreateSolidBrush(RGB(64, 156, 255));
        if (total > 0) {
            float f = (float)cur / (float)total; if (f < 0.f) f = 0.f; if (f > 1.f) f = 1.f;
            RECT fr = { bx, y, bx + (int)(bw * f), y + bh }; FillRect(dc, &fr, fill);
        } else {
            // marquee: a 25%-wide chunk sweeping left->right
            float t = (GetTickCount() % 1200) / 1200.f;
            int cw = bw / 4, cx0 = bx + (int)((bw + cw) * t) - cw;
            RECT fr = { cx0 < bx ? bx : cx0, y, (cx0 + cw > bx + bw) ? bx + bw : cx0 + cw, y + bh };
            if (fr.right > fr.left) FillRect(dc, &fr, fill);
        }
        DeleteObject(fill);
        HPEN bpen = CreatePen(PS_SOLID, 1, RGB(70, 78, 98)); HGDIOBJ open = SelectObject(dc, bpen);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom);
        SelectObject(dc, open); SelectObject(dc, obr); DeleteObject(bpen);
    }
    SelectObject(dc, ofont); DeleteObject(fBig); DeleteObject(fSmall);
    BitBlt(wdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(hwnd, wdc);
}
#endif

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
        if (g_editor && g_editor->ready && (g_editor->patchMode || g_editor->sliceGizmoOn)) { /* Esc cancels the active TOOL (editor.onKey handles it) - don't deselect/quit */ }
        else if (g_renderer && g_renderer->selectedMesh >= 0) {
            // Deselect first, exit on second Esc
            g_renderer->selectedMesh = -1;
            glfwSetWindowTitle(w, "HSR Renderer [Vulkan] - Tab=select mesh  F=wireframe  Esc=quit");
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
    // Init WinSock ONCE at startup so the editor's Quest-sync (editor.h sendQuestCmd -> 127.0.0.1:27042) works
    // in a normal session. Previously WSAStartup only ran inside hsrHttpServer() (HSR_LIVE mode), so in the plain
    // editor every socket() returned INVALID_SOCKET -> "Quest: ON" pause/scrub silently sent NOTHING to the bridge.
    { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
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
    fprintf(stderr, " HSR Renderer / Editor - libshell.so Vulkan replica\n");
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
    if (argc >= 2) apkPath = argv[1];

    // ── ONE window, alive from t=0 ─────────────────────────────────────────────────────────────
    // Interactive sessions create the REAL main window IMMEDIATELY — before any scene parsing —
    // and paint into it with GDI: a drop-zone when launched bare (the drag&drop is integrated, no
    // separate popup), then the live loading progress while the env parses on a worker thread.
    // Vulkan later takes over presentation of this same window. HSR_LIVE (MCP-driven) is ALSO
    // interactive: its window is visible, so it gets the same splash + streaming upload (live
    // commands are simply deferred until the last mesh lands). Only truly headless/scripted modes
    // (HSR_EXPORT / HSR_SHOT / HSR_BLENDER_EXPORT) keep the old synchronous flow.
    const bool interactive = !std::getenv("HSR_EXPORT") && !std::getenv("HSR_SHOT")
                          && !std::getenv("HSR_BLENDER_EXPORT");
    glfwSetErrorCallback(errorCb);
    if (interactive) {
        if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        // Don't STEAL FOCUS from whatever the user is doing - neither at creation nor when shown
        // later (drag-drop relaunches used to yank focus every time).
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        if (std::getenv("HSR_FLOATING")) glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        g_window = glfwCreateWindow(g_winW, g_winH, "HSR Renderer - Quest Home Editor", nullptr, nullptr);
        if (!g_window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }
        { GLFWimage icons[2]; std::vector<unsigned char> p48, p32; genEditorIcon(48,p48); genEditorIcon(32,p32);
          icons[0]={48,48,p48.data()}; icons[1]={32,32,p32.data()}; glfwSetWindowIcon(g_window, 2, icons); }
        glfwSetDropCallback(g_window, dropCb);
        if (apkPath.empty()) {
            // Launched bare: the main window IS the drop zone. Wait here until an .apk lands on it.
            glfwSetWindowTitle(g_window, "HSR Renderer - drag an environment .apk here");
            g_doReload = false; g_dropPath.clear();
#ifdef _WIN32
            HWND hwnd = glfwGetWin32Window(g_window);
#endif
            while (!glfwWindowShouldClose(g_window) && !g_doReload) {
#ifdef _WIN32
                paintLoadSplash(hwnd, nullptr, /*waitingForDrop=*/true);
#endif
                glfwWaitEventsTimeout(0.05);
            }
            if (!g_doReload || g_dropPath.empty()) { glfwDestroyWindow(g_window); glfwTerminate(); return 0; }
            apkPath = g_dropPath; g_doReload = false; g_dropPath.clear();
        }
    }
    fprintf(stderr, "[MAIN] APK: %s\n\n", apkPath.c_str());
    std::string envBaseName = apkPath;
    { size_t sl = envBaseName.find_last_of("/\\"); if (sl != std::string::npos) envBaseName = envBaseName.substr(sl + 1); }
    if (interactive) glfwSetWindowTitle(g_window, ("HSR Renderer - loading " + envBaseName + " ...").c_str());

    // ── IN-PLACE ENV RELOAD LOOP ────────────────────────────────────────────────────────────────
    // The editor can SWAP the loaded env WITHOUT restarting the process: cook -> "Preview cooked
    // (HSL)" renders the cooked APK in this SAME window, "Back to source" returns, and drag&drop
    // reloads in place too (no new process, no focus steal). Everything from scene load to the
    // render loop lives inside this loop; a swap tears down renderer+editor (the window survives)
    // and goes around again with the new path.
    const std::string sourceEnvPath = apkPath;   // the original source env ("Back to source" target)
    std::string nextEnv;                          // set by a swap request; empty = normal exit
    for (;;) {
    nextEnv.clear();
    g_loadProgress.done.store(false); g_loadProgress.failed.store(false);
    g_loadProgress.set("Reading APK...");
    g_animOverride = false; g_animScrub = 0.f;
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
    static std::vector<MeshData> blenderMeshes;   // Blender round-trip import target (stable address)
    bool isBlender = false, isV79 = false, isOpa = false;
    // Outputs of the shader extraction/selection (consumed by Vulkan init after the load completes).
    static constexpr u32 SPIRV_MAGIC = 0x07230203u;
    std::vector<u32> vertSpirv, fragSpirv, skinnedVertSpirv, skinnedFragSpirv;
    std::string g_globalShaderPath;       // path of the chosen global shader (-> renderer matParams match)
    std::vector<u32> alphaTestFragSpirv;  // V79/OPA cutout discard frag (set below); empty otherwise
    struct Prog { std::vector<u32> vert, frag; std::string name; bool fragTex=false; };
    std::vector<Prog> allVariants;   // EVERY (vert,frag) pair per surface — per-material picks the right one
    bool envShippedRendShad = false;

    // The ENTIRE CPU-side load (format detect + scene parse + texture decode + shader extraction and
    // selection). In interactive mode this runs on a WORKER thread while the main thread keeps the
    // window alive and paints the loading progress — the old flow blocked here for the whole parse,
    // which is exactly the "white screen" (no window, or a frozen unpainted one). Headless modes call
    // it synchronously, unchanged.
    auto loadSceneCPU = [&]() -> bool {
    g_loadProgress.set("Reading APK...");
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
    // Blender round-trip IMPORT: a plain .gltf/.glb (NOT a V79 .ovrscene) = a re-imported, Blender-edited project.
    // Load it into MeshData[] so the editor's HSL tools can tweak it further and re-cook to an APK.
    g_loadProgress.set("Parsing scene...");
    isBlender = gltfimport::isPlainGltf(apkPath) && gltfimport::importEnv(apkPath, blenderMeshes);
    isV79 = !isBlender && gltf.load(apkPath);
    if (isBlender) {
        fprintf(stderr, "[MAIN] Imported Blender glTF project - %zu meshes (editable + re-cookable)\n", blenderMeshes.size());
        sceneMeshes = &blenderMeshes;
    } else if (isV79) {
        fprintf(stderr, "[MAIN] Detected V79 .gltf.ovrscene env - %zu mesh primitives\n", gltf.meshes.size());
        sceneMeshes = &gltf.meshes;
    } else if ((isOpa = opa.load(apkPath))) {
        // V79 OLD OFFICIAL home: cooked .opa (faithful libshell reflection-format loader)
        fprintf(stderr, "[MAIN] Detected V79 .opa official home - %zu renderable submeshes\n", opa.meshes.size());
        sceneMeshes = &opa.meshes;
    } else {
        if (!loader.load(apkPath)) {
            fprintf(stderr, "\n[MAIN] FATAL: Scene load failed\n");
            return false;
        }
        // RENDER BOTH: merge the companion env (vista backdrop + haven2025 home) at the shared origin.
        if (!companionPath.empty()) {
            static SceneLoader companionStore; companionStore = SceneLoader{};   // fresh per load (in-place swaps re-enter here)
            SceneLoader& companion = companionStore;
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
    g_loadProgress.set("Loading shaders...");
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
    envShippedRendShad = !shaders.empty();

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
            fprintf(stderr, "[MAIN] Input has no RENDSHAD - loading shaders from: %s\n", c.c_str());
            loadShadersFromApk(c);
            if (!shaders.empty()) break;
        }
        if (shaders.empty()) {
            // ── Built-in self-contained shader (the V79 "both sides" path) ──────────────
            // Mirrors how V79 libshell renders old homes: its own shader, not the env's.
            fprintf(stderr, "[MAIN] Using built-in self-contained shader (V79 path - like libshell's\n"
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
    g_loadProgress.set("Selecting shaders...");

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
        return false;
    }
    fprintf(stderr, "[MAIN] Selected VERTEX(unlit): %zu words  FRAGMENT(unlit): %zu words\n",
            vertSpirv.size(), fragSpirv.size());
    if (!skinnedVertSpirv.empty())
        fprintf(stderr, "[MAIN] Selected VERTEX(skinned): %zu words  FRAGMENT(skinned): %zu words\n",
                skinnedVertSpirv.size(), skinnedFragSpirv.size());

    fprintf(stderr, "\n[MAIN] Scene: %zu meshes ready\n\n", sceneMeshes->size());
    g_loadProgress.set("Starting renderer...");
    return true;
    };   // end loadSceneCPU

    // Run the load: worker thread + live progress painting (interactive), else synchronous.
    bool loadOk = false;
    if (interactive) {
        std::thread loadThread([&]{ loadOk = loadSceneCPU(); g_loadProgress.done.store(true); });
#ifdef _WIN32
        HWND splashHwnd = glfwGetWin32Window(g_window);
#endif
        while (!g_loadProgress.done.load()) {
            glfwPollEvents();   // window stays responsive (move/resize/close) during the whole parse
#ifdef _WIN32
            paintLoadSplash(splashHwnd, envBaseName.c_str(), /*waitingForDrop=*/false);
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        loadThread.join();
        if (glfwWindowShouldClose(g_window)) { glfwDestroyWindow(g_window); glfwTerminate(); return 0; }
    } else {
        loadOk = loadSceneCPU();
    }
    if (!loadOk) {
        fprintf(stderr, "\n[MAIN] FATAL: scene load failed for %s\n", apkPath.c_str());
        return 1;
    }

    // ── Step B: Init GLFW + Vulkan ─────────────────────────────
    if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }   // no-op if already inited (interactive)
    // Title shows WHICH loader/format is active (V79 glTF / V79 OPA / HSL) + the env file name.
    std::string g_fmtName = isV79 ? "V79 glTF" : (isOpa ? "V79 OPA" : "HSL");
    std::string g_baseName = envBaseName;
    std::string g_title = "HSR Renderer [Vulkan]  -  " + g_fmtName + "  -  " + g_baseName +
                          "   |   WASD=move drag=look  Tab/N=next mesh B=prev  F=wire  Esc=quit";
    if (!g_window) {   // headless/scripted modes create the window only now (after the synchronous load)
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        // COOKING MODE (HSR_EXPORT) = console-only: hide the window so it doesn't pop a white "frozen" window with no
        // feedback. The cook prints [GLTF]/[COOK]/[EXPORT] progress to the console; HSR_EXPORT_QUIT exits when done.
        bool g_cookHeadless = std::getenv("HSR_EXPORT") != nullptr;
        if (g_cookHeadless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        // Always-on-top (GLFW_FLOATING) ONLY for capture modes (reliable external screenshots). The interactive editor
        // defaults OFF — an always-on-top editor window is annoying — and exposes a runtime "Always on top" toggle.
        else if (std::getenv("HSR_SHOT") || std::getenv("HSR_LIVE") || std::getenv("HSR_FLOATING")) glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
        g_window = glfwCreateWindow(g_winW, g_winH, g_title.c_str(), nullptr, nullptr);
        if (!g_window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }
        { GLFWimage icons[2]; std::vector<unsigned char> p48, p32; genEditorIcon(48,p48); genEditorIcon(32,p32);  // window/taskbar icon
          icons[0]={48,48,p48.data()}; icons[1]={32,32,p32.data()}; glfwSetWindowIcon(g_window, 2, icons); }
    } else {
        glfwSetWindowTitle(g_window, g_title.c_str());
    }

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
    // PROGRESSIVE upload (interactive): meshes are streamed to the GPU a time-slice per frame INSIDE the
    // render loop, so the window/UI are live immediately and the scene pops in as it uploads (with a
    // loading bar). Headless/scripted modes keep the blocking upload (captures expect the full scene).
    int uploadNext = 0;
    const int uploadEnd = (int)sceneMeshes->size();
    if (!interactive) {
        for (int mi = 0; mi < (int)sceneMeshes->size(); ++mi) {
            if (mi < minMesh || mi >= maxMesh) continue;
            vkRenderer.uploadMesh((*sceneMeshes)[mi]);
        }
        uploadNext = uploadEnd;
        fprintf(stderr, "[MAIN] GPU upload complete: %zu meshes\n\n",
                vkRenderer.gpuMeshes.size());
    }

    // [DEBUG] HSR_TESTMOVE=<dx>: shift every mesh's model translation by +dx in X. If the scene
    // visibly moves, the worldFromModel push constant is live; if not, the shader ignores it.
    if (const char* tm = std::getenv("HSR_TESTMOVE")) {
        float d = (float)atof(tm);
        for (auto& gm : vkRenderer.gpuMeshes) gm.model[12] += d;
        fprintf(stderr, "[TESTMOVE] shifted all %zu models by x+=%.2f\n", vkRenderer.gpuMeshes.size(), d);
    }

    // ── Step D: Main loop ──────────────────────────────────────
    fprintf(stderr, "[MAIN] Render loop started - Esc to quit\n\n");
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
        fprintf(stderr, "[LIVE] ready - HTTP control on :%d (curl --data 'cam=..\\nshot=path' 127.0.0.1:%d)\n", port, port);
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
    editor.sourceEnvPath = sourceEnvPath;   // "Back to source" target for the in-place cook-preview swap
    editor.setEnvAudio(ogg);            // the env's background loop -> cooked as an auto-start FMOD SoundAsset (editor can REPLACE/ADD/export it)
    // (editor.audio binds in editor.init — BEFORE loadProject, so a session AUDIOOVR restarts the preview;
    //  in headless (HSR_NOUI) it stays null, so a session AUDIOOVR swaps the cook bytes WITHOUT starting playback)
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
        else if (gltf.isNodeAnimated(meshIdx) && gltf.nodeAnimMoves(meshIdx) && !std::getenv("HSR_NOPULSE")) {   // nodeAnimMoves: skip CONSTANT tracks (no getTime shader -> static, not "breathing")
            // COMBINED T+R node anim (cyan gears/mills/boat/shadow cards, dome comets, rifthome leaves) → 2-joint
            // RIGID-HZANIM: the node's EXACT per-frame WORLD matrix on the device-proven skinned path. The fit chain
            // below is rotation-ONLY or translation-ONLY — it DROPPED the other channel for meshes that animate both
            // ("wrong animation on windmill, shadows, gears"). Pure-T / pure-R meshes return !ok() here and keep
            // their proven getTime shaders (no regression). Gated on HSR_HZANIM like every skinned cook path.
            if (std::getenv("HSR_HZANIM") && !std::getenv("HSR_NOGLTFRIGID")) {
                auto rg = gltf.extractNodeRigidHzAnim(meshIdx);
                if (rg.ok()) {
                    em.hzJointPos=std::move(rg.jointPos); em.hzJointQuat=std::move(rg.jointQuat); em.hzJointScale=std::move(rg.jointScale);
                    em.hzParents=std::move(rg.parents); em.hzBoneIdx=std::move(rg.boneIdx); em.hzBoneWgt=std::move(rg.boneWgt);
                    em.hzTrsLocal=std::move(rg.trsLocal); em.hzRestPos=std::move(rg.restPos);
                    em.hzJointCount=rg.jointCount; em.hzFrames=rg.frameCount; em.hzFps=rg.fps;
                    em.vatOffsets.clear(); em.vatFrames=0;
                    if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%d glTF T+R node anim -> RIGID-HZANIM (%d frames @ %.1ffps)\n", meshIdx, rg.frameCount, rg.fps);
                    if (gltf.isNodeAnimated(meshIdx) && std::getenv("HSR_VERBOSE")) gltf.dumpNodeAnimTrack(meshIdx);
                    return;
                }
            }
            // Y-ROTATION (Outer Wilds skybox/Interloper) takes precedence: a uniform node-spin -> getTime() Y-rotation
            // shader. Falls through to the wisp scale-pulse for non-rotation node anims (erebor flames have no rot channel).
            float rax[3] = {0,1,0}, rom = 0.f, rpiv[3] = {0,0,0}, ramp = 0.f, rper = 0.f; bool rosc = false;
            if (!std::getenv("HSR_NOROT") && gltf.extractNodeRotation(meshIdx, rax, rom, rpiv, rosc, ramp, rper)) {
                // FAST or LARGE "sway" -> EXACT ROTREPLAY (the OPA torch-flame fix, glTF side). The single-cosine
                // sway fit is wrong for (a) fast keyed flicker (fireplace particles) and (b) a MONOTONIC per-loop
                // rotation ramp (cyan gears: 63° in 8.3s, wraps on the gear symmetry) — the fit sees peak=63° and
                // plays a one-sided rock. Replay the node's actual relative quats instead (same getTime shader as
                // the aurora). Gentle slow ambient sways (foliage ±1-3°) keep the cheap fitted oscillate shader.
                bool exact = rosc && !std::getenv("HSR_NOROTREPLAY")
                           && (rper < 5.f || ramp > 0.15f || ramp < -0.15f);   // fast flicker OR >~8.6° swing
                if (exact) {
                    int RN = 128; if (const char* e2 = std::getenv("HSR_ROTKEYS")) { int c = atoi(e2); if (c >= 8) RN = c; }
                    float piv[3]; std::vector<float> quats; float loopSec = 0.f;
                    if (gltf.cookExtractRotationReplay(meshIdx, RN, piv, quats, loopSec) && (int)quats.size() == (RN+1)*4) {
                        em.rotReplay = true; em.rotReplayN = RN; em.rotLoopSec = loopSec; em.rotQuats = std::move(quats);
                        em.rotPivot[0]=piv[0]; em.rotPivot[1]=piv[1]; em.rotPivot[2]=piv[2];
                        em.rotAnim = true; em.rotAxis[0]=rax[0]; em.rotAxis[1]=rax[1]; em.rotAxis[2]=rax[2];
                        em.vatOffsets.clear(); em.vatFrames = 0;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%d glTF SWAY(amp=%.0fdeg per=%.1fs) -> ROTREPLAY %d keys loop=%.2fs (exact shape)\n", meshIdx, ramp*57.2958f, rper, RN, loopSec);
                    }
                }
                if (!em.rotReplay) {
                    em.rotAnim = true; em.rotOmega = rom; em.rotOsc = rosc; em.rotAmp = ramp; em.rotPeriod = rper;
                    em.rotAxis[0]=rax[0]; em.rotAxis[1]=rax[1]; em.rotAxis[2]=rax[2];
                    em.rotPivot[0]=rpiv[0]; em.rotPivot[1]=rpiv[1]; em.rotPivot[2]=rpiv[2];
                    em.vatOffsets.clear(); em.vatFrames = 0;   // rotation/sway -> getTime() Rodrigues shader, NOT VAT (vatBaker also ran on this node-anim mesh)
                }
            } else if (gltf.extractNodeTranslation(meshIdx, 16, em.transFrames, em.transLoop)) {   // GENERAL node-translation replay (sliding screens) — port ANY translation, not the wrong pulse default
                em.transAnim = true; em.transN = 16;
            } else if (!std::getenv("HSR_NOSCALE") && gltf.extractNodeScaleFrames(meshIdx, 16, em.scaleFrames, em.scaleLoop, em.scalePivot)) {
                // GENERAL node-SCALE "breathe" replay (Erebor's 12 wisps, NON-UNIFORM per-axis) -> shadergen::SCALE getTime()
                // shader. FAITHFUL per-axis amplitudes (the hand-rolled wispscale.surface couldn't). The pre-existing pose/
                // pulse path stays as the gated FALLBACK: HSR_NOSCALE skips this so a node still takes the old wispscale route.
                em.scaleAnim = true; em.scaleN = 16;
            } else if (std::getenv("HSR_POSEANIM") && gltf.extractNodeScaleAnim(meshIdx, em.poseStartScale, em.poseEndScale, em.poseDuration))
                em.poseAnim = true;
            else em.pulse = true;
        }
        if (gltf.isNodeAnimated(meshIdx) && std::getenv("HSR_VERBOSE")) gltf.dumpNodeAnimTrack(meshIdx);
        // FRAME COUNT must track the clip DURATION, not a fixed 64: snakeway's dragon "Action" is 31.2s — 64 frames =
        // ~2fps → the body-undulation is massively undersampled → "moves but not the original animation". Sample at a
        // real rate (~30fps) so the wave is captured. ACL compresses the extra frames cheaply. HSR_HZFPS overrides.
        // The HZAN clip has a HARD device size limit: a ~22 KB clip (256 frames) LOADS, ~65 KB (935 frames) throws
        // std::length_error on load. ⚠ That "size limit" was a MISDIAGNOSIS: the crash was the missing 5-byte
        // trailing channel-name table (IDA @0x1617f4c, fixed in hzAclEncode — "never a malformed clip"), and the
        // official oceanarium whale ships 240 KB / 197 KB HzAnim clips. NO CAP by default — FULL PORT, the whole
        // clip at ~30fps (snakeway's 31.2s dragon = real 30fps, was ~8fps). HSR_HZFPS / HSR_HZMAXFRAMES opt back in.
        { float hzfps = 30.f; if (const char* e2 = std::getenv("HSR_HZFPS")) { float v = (float)atof(e2); if (v >= 1.f) hzfps = v; }
          int cap = 0x7FFFFFFF; if (const char* e3 = std::getenv("HSR_HZMAXFRAMES")) { int v = atoi(e3); if (v >= 8) cap = v; }
          int nf = (gltf.animDuration > 0.f) ? (int)(gltf.animDuration * hzfps + 0.5f) : frames;
          if (nf < 64) nf = 64; if (nf > cap) nf = cap;   // floor 64 (short clips); cap only if HSR_HZMAXFRAMES set
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
    std::unordered_map<size_t, OpaLoader::FlipRec> g_opaFlip;   // mat.sanim ATLAS flipbooks (waterfall/stream/fog)
    std::set<int> g_syncNodes;   // declared HERE (outlives the lambda stored in editor.hzAnimExtractor); populated below
    // OPA VAT cook (underwater corals/seaweed/fish): bake the SOURCE VAT data (t_*_vatdata.exr.opa, played
    // live via vatRecs) into the cooker's device-proven useVat path (RGBA16 offset tex + vatunlitbasecolor —
    // the Erebor-wisp pipeline). Was never wired -> every VAT mesh cooked STATIC. HSR_NOVAT disables (same
    // switch as the loader's live VAT).
    if (isOpa && !std::getenv("HSR_NOVAT"))
        editor.vatBaker = [&opa](int meshIdx, int frames, int& nv){ return opa.bakeVAT(meshIdx, frames, nv); };
    if (isOpa && !std::getenv("HSR_NOROT")) {
        opa.cookExtractRotations(g_opaRot);
        opa.cookExtractUVScroll(g_opaUv);   // mat.sanim water/foam UV scrolls (continuous)
        opa.cookExtractFlipbook(g_opaFlip); // mat.sanim ATLAS flipbooks (cell-stepping waterfall/stream/fog)
        fprintf(stderr, "[OPA] cook anim: %zu spin/sway + %zu uv-scroll + %zu flipbook (of %zu meshes)\n", g_opaRot.size(), g_opaUv.size(), g_opaFlip.size(), opa.meshes.size());
        // SYNC NODES (the "train + steam detached" fix): the train STEAM is a flipbook -> it rides the getTime() TRANSLATE
        // clock. Its sibling the train BODY is a plain node mesh -> default useHz (animator clock) -> the two drift apart.
        // Collect the node (+ its direct parent = the vehicle group) of every flipbook that translates a LOT (a vehicle);
        // any node-rigid mesh on one of THOSE nodes is routed to the SAME getTime() TRANSLATE so body+steam stay attached.
        // Complex one-offs (comets/birds, on unrelated nodes) keep their faithful useHz baked path.
        for (size_t mi=0; mi<opa.meshes.size(); ++mi) { std::vector<float> mats; int mN=0; float mL=0.f;
            if (!opa.cookExtractUVMatrices((int)mi, 8, mats, mN, mL)) continue;   // only UV-animated cards (the train STEAM)
            std::vector<float> tof; float tl=0.f;
            if (opa.cookExtractNodeTranslateFrames((int)mi, 24, tof, tl)) {
                float md=0.f; for (float v : tof){ float a=std::fabs(v); if(a>md)md=a; }
                if (md > 60.f) { int n=opa.animNodeOf((int)mi); if (n>=0){ g_syncNodes.insert(n); int p=opa.animNodeParentOf(n); if(p>=0) g_syncNodes.insert(p); } } } }
        editor.hzAnimExtractor = [&g_opaRot,&g_opaUv,&g_opaFlip,&opa,&g_syncNodes](int meshIdx, int frames, hslcook::ExportMesh& em){
            (void)frames;
            // MaterialTint FULL-RGBA cycle (stinson fireworks flash colors / city window-light flicker) — filled
            // FIRST, independent of the motion routing below (shader branches chain a TINTREPLAY frag stage).
            // The baked COLOR_0 already carries the t=0 tint (animate(0) -> md.curTint folded into em.curTint), so
            // DIVIDE it back out — the replay table carries the full curve including frame 0 (no double-apply).
            // HSR_NOTINT disables.
            if (!std::getenv("HSR_NOTINT")) {
                std::vector<float> trgba; int tn = 0; float tloop = 0.f;
                if (opa.cookExtractTintRGBA(meshIdx, 512, trgba, tn, tloop) && tn >= 2) {
                    // SKINNED foliage (stinson zen tree materialsplit_*): the getTime TINTREPLAY frag stage does NOT
                    // reliably drive on the device's skinned-cutout pipeline (device showed the leaves PALE/untinted =
                    // stark white "shattered" cards, while the source live-tints them GREEN so they read as a canopy).
                    // Bake the tint STATICALLY into COLOR_0 instead — the base×COLOR0 unlit path always executes on
                    // device AND shows in the preview. em.curTint already carries the frame-0 tint (the loader's tint
                    // attach set md.curTint = the cycle's frame-0, which is the clean natural leaf GREEN — the cycle's
                    // resting color; the green→cyan→pink loop's MEAN is a muddy khaki that reads as dead foliage). So
                    // leave em.curTint as-is (frame-0 green) and do NOT flag tintAnim (no getTime replay for skins).
                    if (opa.isSkinnedMesh(meshIdx)) {
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%d MaterialTint (skinned) -> STATIC COLOR_0 tint=(%.2f,%.2f,%.2f,%.2f)\n", meshIdx, em.curTint[0],em.curTint[1],em.curTint[2],em.curTint[3]);
                    } else {
                        // undo the baked frame-0 tint exactly (em.curTint = md.curTint(frame0)·edit·light); a channel
                        // that STARTS at 0 (firework light OFF at t=0) is unrecoverable -> neutral 1 (the replay owns it)
                        for (int c = 0; c < 4; c++) { float f0 = trgba[c]; em.curTint[c] = (f0 > 1e-4f) ? em.curTint[c]/f0 : 1.0f; }
                        em.tintAnim = true; em.tintFrames = std::move(trgba); em.tintN = tn; em.tintLoop = tloop;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[COOK] m%d MaterialTint RGBA cycle %d frames loop=%.2fs -> TINTREPLAY\n", meshIdx, tn, tloop);
                    }
                }
            }
            // PURE CONTINUOUS SCROLL (waterfall / water / foam / stream: identity 2x2 + a small CONSTANT per-frame
            // translate) -> the SMOOTH getTime `uv += rate*time` scroll shader, NOT the per-frame matrix REPLAY below.
            // Replaying N discrete matrices SNAPS the UV each frame = CHOPPY, and the hard per-frame offset jump shows
            // a visible cell BORDER/seam on a scroll (rockquarry waterfalls: "choppy + I see borders"). rate*time is
            // continuous and seamless (texture REPEAT wrap hides the tile boundary). ONLY take this when NOTHING else
            // is layered — no tint-fade, no node translate, no rot/scale — so fog/dust/steam (which carry fade+motion)
            // keep the matrix-replay path unchanged. g_opaUv was filled by cookExtractUVScroll (uvScrollRate: identity
            // 2x2 + |avgDelta|<=0.04/frame). HSR_NOUVSCROLLFAST forces the old replay path.
            {
                auto uit = g_opaUv.find((size_t)meshIdx);
                // PURE CONTINUOUS SCROLL (waterfall/foam/stream): smooth getTime `uv += rate*time`. EXCLUDE nodes that
                // rotate/scale their GEOMETRY — the aurora's mesh is a WARPED surface that ROTATES ABOUT ITSELF (the
                // node rotation), and that geometry rotation BENDS the texture = the animated look (user). That is a
                // GEOMETRY animation (faithful per-frame node rotation via RIGID-HZANIM below), NOT a UV scroll/warp; a
                // smooth uv-scroll here would wrongly make it "slide/move" instead of the mesh rotating about itself.
                if (uit != g_opaUv.end() && !std::getenv("HSR_NOUVSCROLLFAST") && !opa.nodeAnimatesRotOrScale((int)meshIdx)) {
                    std::vector<float> fa; float floop=0.f;
                    bool hasFade = opa.cookExtractTintAlpha((int)meshIdx, fa, floop) && fa.size()>=2;
                    std::vector<float> tof; float tl=0.f; bool hasTrans=false;
                    if (opa.cookExtractNodeTranslateFrames((int)meshIdx, 24, tof, tl)) {
                        float md=0.f; for (float v : tof) { float a=std::fabs(v); if(a>md)md=a; } hasTrans = md > 1.f; }
                    if (!hasFade && !hasTrans) {
                        em.uvScroll=true; em.uvRate[0]=uit->second.first; em.uvRate[1]=uit->second.second;
                        em.flipbook=false; em.transAnim=false; em.fadeAnim=false; em.rotAnim=false;
                        em.vatOffsets.clear(); em.vatFrames=0; em.hzJointCount=0; em.hzFrames=0;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] m%d UV-SCROLL(smooth) rate=(%.4f,%.4f)/s -> no snap/borders\n", meshIdx, em.uvRate[0], em.uvRate[1]);
                        return;
                    }
                }
            }
            // MATERIAL UV ANIMATION — REPLAY THE DESKTOP'S EXACT DATA (the "stop guessing" fix). Any mesh whose mat.sanim
            // track animates (fog/dust/smoke/fire/steam) → copy its ACTUAL per-frame 2x3 UV matrices (the SAME values the
            // desktop's animate() plays) and replay them VERBATIM in a FRAGMENT getTime() shader — no derived cols/rows
            // grid to mis-guess, all frames (no flashing), full matrix (scroll + sprite-cell + 2x2 SCALE/dust all exact).
            // Takes priority over useHz (whose skinned shader froze the texture). If it also rides a BIG vehicle move
            // (train STEAM ~162u), layer a TRANSLATE under it; small ambient drift stays static (looked wrong as getTime).
            { std::vector<float> mats; int mN=0; float mLoop=0.f;
              if (opa.cookExtractUVMatrices((int)meshIdx, 256, mats, mN, mLoop)) {
                  // AURORA (pure-ROTATION card: node rotates, does NOT translate) -> ROTREPLAY getTime VERTEX angle-replay,
                  // routed HERE and UNGATED (a shader, NOT skeletal — must NOT depend on the animate-skinned/HSR_HZANIM
                  // toggle; else animSkinned=OFF cooks fall to the choppy FLIPBOOK-UVMATRIX texture-scroll = "rotating
                  // texture + choppy", the persistent Quest bug). Faithful non-uniform rotation, smooth, no animator LOD.
                  if (!std::getenv("HSR_NOROTREPLAY") && opa.nodeAnimatesRotOrScale((int)meshIdx)) {
                      std::vector<float> tck; float tl3=0.f; bool moves=false;
                      if (opa.cookExtractNodeTranslateFrames((int)meshIdx,24,tck,tl3)){ float mm=0.f; for(float v:tck){float a=std::fabs(v);if(a>mm)mm=a;} moves=mm>1.f; }
                      if (!moves) {
                          // AURORA = EXACTLY what OPA's animate() does: the node ROTATES the dome geometry (verified by the vertex
                          // test — v0 sweeps ~360°/loop about world-Y) AND the mat.sanim UVTransform COUNTER-SCROLLS the texture the
                          // matching amount, so the TEXTURE stays WORLD-STILL while the mesh spins under it (the curved dome bends the
                          // still texture). BOTH channels are required and CANCEL: geometry rotation alone locks the texture to the
                          // mesh so it spins (wrong); scroll alone slides the texture (wrong). Cook BOTH (natural directions, so they
                          // cancel like OPA): ROTREPLAY(vertex) + UVSCROLL(frag) via the cook's useRot chain.
                          int RN=128; if(const char*e=std::getenv("HSR_ROTKEYS")){int c=atoi(e);if(c>=8)RN=c;}
                          float pivot[3]; std::vector<float> quats; float loopSec=0.f;
                          if (opa.cookExtractRotationReplay((int)meshIdx,RN,pivot,quats,loopSec) && (int)quats.size()==(RN+1)*4) {
                              em.rotReplay=true; em.rotReplayN=RN; em.rotLoopSec=loopSec; em.rotQuats=std::move(quats);
                              em.rotPivot[0]=pivot[0]; em.rotPivot[1]=pivot[1]; em.rotPivot[2]=pivot[2];
                              em.rotAnim=true; em.flipbook=false; em.fadeAnim=false;
                              em.hzJointCount=0; em.hzFrames=0; em.vatOffsets.clear(); em.vatFrames=0;
                              // the UV counter-scroll (raw-matrix rate = what OPA replays) — makes the texture world-still
                              em.uvScroll=false; em.uvRate[0]=em.uvRate[1]=0.f;
                              if (mN>=2 && mLoop>1e-3f && (int)mats.size()>=mN*6) {
                                  const float* a=&mats[0]; const float* b=&mats[(size_t)(mN-1)*6];
                                  if (std::fabs(a[0]-1)<1e-3f&&std::fabs(a[4]-1)<1e-3f&&std::fabs(a[1])<1e-3f&&std::fabs(a[3])<1e-3f) {
                                      float span=mLoop*(float)(mN-1)/(float)mN;
                                      float ru=span>1e-4f?(b[2]-a[2])/span:0.f, rv=span>1e-4f?(b[5]-a[5])/span:0.f;
                                      if (ru!=0.f||rv!=0.f){ em.uvScroll=true; em.uvRate[0]=ru; em.uvRate[1]=rv; } } }
                              if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] m%d AURORA ROTREPLAY+UVSCROLL %d keys loop=%.1fs scroll=(%.4f,%.4f) (geometry spins, texture world-still)\n", meshIdx, RN, loopSec, em.uvRate[0],em.uvRate[1]);
                              return;
                          }
                      }
                  }
                  // KEEP the AUTHORED blend (set from md.useBlend/additive in buildExportMeshes): fog/steam are
                  // authored Transparent:true and stay BLEND, but the storybook River waterCards are authored
                  // OPAQUE (Transparent:false) — the old forced em.blend=true made them alpha-blend with NO depth
                  // write, so overlapping river cards z-fought/sorted wrong and composited darker than OPA
                  // ("not in sync, wrong placements, wrong lighting"). OPA renders them opaque with depth-write.
                  em.flipbook=true; em.flipOffset=true; em.uvScroll=false;
                  em.flipUVMats=std::move(mats); em.flipN=mN; em.flipLoop=mLoop;
                  em.flipCols=1; em.flipRows=1; em.flipFrames=mN; em.flipFps=30.f;
                  em.rotAnim=false; em.vatOffsets.clear(); em.vatFrames=0;
                  // OPACITY FADE (mat.sanim MaterialTint alpha) — replayed in the flipbook frag, SYNCED to the mat.sanim
                  // period (mLoop) as the UV so fade-in/out lines up exactly like V79. Shared by BOTH motion paths below.
                  em.fadeAnim=false;
                  if (!std::getenv("HSR_FLIPNOFADE")) { std::vector<float> fa; float floop=0.f;
                      if (opa.cookExtractTintAlpha((int)meshIdx, fa, floop) && fa.size()>=2) {
                          em.fadeAnim=true; em.fadeN=(int)fa.size(); em.fadeFrames=std::move(fa); em.fadeLoop = mLoop; } }
                  // ── MOTION (NO EXCLUSION) ────────────────────────────────────────────────────────────────────────
                  // An effect card whose node ROTATES or SCALES (fog_01 = full T+R+S) can't be faithfully ported by the
                  // getTime TRANSLATE shader alone (a hierarchical / far-pivot / rotating transform would FLING the card —
                  // verified: raw node scale 1.7x vs the render's ~1.2x net world size). Route it to RIGID-HZANIM (the
                  // EXACT per-frame WORLD matrix via a 2-joint skeleton = faithful T+R+S) AND keep the UV-flipbook + fade:
                  // the cook bakes those into the SKINNED material shader (fragment), skinning stays in the vertex. So
                  // T+R+S + UV + fade ALL cook together, synced (device: skeleton on the animator clock, UV/fade on the
                  // shader clock — both real-time). HSR_FLIPNOHZ forces the old translate-only path.
                  if (!std::getenv("HSR_FLIPNOHZ") && std::getenv("HSR_HZANIM") && opa.nodeAnimatesRotOrScale((int)meshIdx)) {   // gate on HSR_HZANIM (matches the cook's useHz); else keep the getTime path (no regression)
                      // (Pure-ROTATION cards e.g. the aurora already returned above via the ungated ROTREPLAY quaternion path.)
                      // This block is now only reached by TRANSLATE/SCALE effect cards (fog/dust): RIGID-HZANIM replays the
                      // exact per-frame T+R+S via a 2-joint skeleton AND keeps the UV flipbook + fade.
                      auto rg = opa.extractNodeRigidHzAnim((int)meshIdx, /*allowPureRotation=*/true);   // fog/dust (translate+R+S): RIGID replays exact T+R+S + keeps UV/fade
                      if (rg.ok()) {
                          em.hzJointPos=std::move(rg.jointPos); em.hzJointQuat=std::move(rg.jointQuat); em.hzJointScale=std::move(rg.jointScale);
                          em.hzParents=std::move(rg.parents); em.hzBoneIdx=std::move(rg.boneIdx); em.hzBoneWgt=std::move(rg.boneWgt);
                          em.hzTrsLocal=std::move(rg.trsLocal); em.hzRestPos=std::move(rg.restPos);
                          em.hzJointCount=rg.jointCount; em.hzFrames=rg.frameCount; em.hzFps=rg.fps;
                          em.transAnim=false;   // motion is in the skeleton; flipbook+fade ride the skinned material shader
                          return;
                      }
                  }
                  // Pure-translate card (or rigid extraction failed): the lighter getTime TRANSLATE + flipbook + fade path.
                  em.hzJointCount=0; em.hzFrames=0;
                  std::vector<float> tof; float tloop=0.f; em.transAnim=false;
                  if (!std::getenv("HSR_FLIPNOTRANS") && opa.cookExtractNodeTranslateFrames((int)meshIdx, 24, tof, tloop)) {   // NATURAL node period first (measure drift)
                      float md=0.f; for (size_t k=0;k<tof.size();++k){ float a=std::fabs(tof[k]); if(a>md)md=a; }
                      if (md > 60.f) {
                          // VEHICLE (train/mine-cart steam): keep the NATURAL node period so steam + body stay LOCKED
                          // (the body extracts the same node period at the g_syncNodes path). Forcing it to the steam's
                          // UV/puff mLoop made the steam race along the track and detach from the cart = the regression.
                          em.transAnim=true; em.transN=24; em.transFrames=std::move(tof); em.transLoop=tloop;
                      } else if (md > 1.f) {
                          // CONVEYOR card (storybook river waterCards: drift ~23u downstream then TELEPORT back upstream
                          // MID-loop): the 24-sample getTime TRANSLATE smears that instant teleport across a whole sample
                          // interval (17s/24 ≈ 0.7s) — the card visibly ZIPS back and TEARS the river ("not synced", the
                          // black sliver through the water). RIGID-HZANIM replays the EXACT per-frame node path at native
                          // resolution with the exclusive-endpoint wrap (instant seam — the cyberhome cars "speed backward"
                          // fix), and the UV flipbook + fade ride the skinned material shader exactly like the fog cards.
                          // VEHICLES (>60u) stay on the getTime TRANSLATE clock — their sibling BODY meshes sync to it.
                          if (!std::getenv("HSR_FLIPNOHZ") && std::getenv("HSR_HZANIM")) {
                              auto rg = opa.extractNodeRigidHzAnim((int)meshIdx);
                              if (rg.ok()) {
                                  em.hzJointPos=std::move(rg.jointPos); em.hzJointQuat=std::move(rg.jointQuat); em.hzJointScale=std::move(rg.jointScale);
                                  em.hzParents=std::move(rg.parents); em.hzBoneIdx=std::move(rg.boneIdx); em.hzBoneWgt=std::move(rg.boneWgt);
                                  em.hzTrsLocal=std::move(rg.trsLocal); em.hzRestPos=std::move(rg.restPos);
                                  em.hzJointCount=rg.jointCount; em.hzFrames=rg.frameCount; em.hzFps=rg.fps;
                                  em.transAnim=false;
                                  return;
                              }
                          }
                          // AMBIENT drift (fog/dust, rigid unavailable): lock the MOVEMENT to the mat.sanim period (mLoop)
                          // so translate + UV + fade stay synced (matches the render — one mat.sanim clock).
                          std::vector<float> tof2; float tl2=0.f;
                          if (opa.cookExtractNodeTranslateFrames((int)meshIdx, 24, tof2, tl2, mLoop)) {
                              em.transAnim=true; em.transN=24; em.transFrames=std::move(tof2); em.transLoop=tl2; }
                      } }
                  return;
              } }
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
            // ENORMOUS node "translation" -> BAKE STATIC (stinson city color_beam searchlights: the GEO_ANIM node
            // translates ~4700 UNITS, which on a small far-background beam is not a faithful entity slide — the source
            // node-anim is a deform/pivot that the rigid 1-joint port mis-reads as a whole-mesh SLIDE, so the beam ends
            // up flung "off place". Any per-mesh node translation this large (>1500u) is such a case: cook the mesh
            // STATIC at its bind pose (correct placement) rather than sliding it across the sky. Cars/train/comets move
            // far less and keep their rigid path. HSR_KEEPBIGSLIDE restores the old slide.
            // (color_beam searchlights fall through to the rigid 1-joint node replay below — it renders the additive
            //  GLOW visibly, unlike the VAT path whose vatunlitblend mis-samples the base texture = a solid white/
            //  invisible beam. The rigid replay reproduces the node's world transform per frame = moves like the OPA.)
            // TRAIN BODY (a node on a vehicle group that also carries a getTime flipbook STEAM): route to the SAME getTime
            // TRANSLATE clock as the steam so they stay attached (NOT useHz, whose animator clock drifts vs getTime).
            { int myNode = opa.animNodeOf(meshIdx);
              if (myNode>=0 && g_syncNodes.count(myNode)) { std::vector<float> tof; float tloop=0.f;
                  if (opa.cookExtractNodeTranslateFrames(meshIdx, 24, tof, tloop)) {
                      em.transAnim=true; em.transN=24; em.transFrames=std::move(tof); em.transLoop=tloop;
                      em.hzJointCount=0; em.hzFrames=0; em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                      return; } } }
            // NON-skinned node TRANSLATION (cars, comet STREAK) -> 1-joint RIGID HZANIM (DEFAULT, device-proven:
            // faithful arbitrary path replay of the ACTUAL per-frame positions incl. the comet's streak). This is
            // a real skinned mesh in the cooked env's skinned SET.
            // ⛔⛔ DO NOT reroute this to getTime TRANSLATE (removing it from the skinned set): the on-device bridge
            // + user PROVED that pulling these RIGID skeletons OUT of the cooked skinned set CORRUPTS the device
            // animator so EVERY skinned mesh (owl/chicken/horses) moves RANDOMLY. Keep node-translations as
            // RIGID-HZANIM. (Earlier "HzAnim skinning is dead/frozen" reading was WRONG HOOKS — env skinning is the
            // HSR MeshShellEnv/JointMatrices path, not the MHE sbSkinningMatrices my hooks watched.)
            // HSR_NODETRANSLATE = opt-in experiment to reroute to getTime TRANSLATE (BREAKS the animator — debug only).
            if (!std::getenv("HSR_NODETRANSLATE")) {
                auto rg = opa.extractNodeRigidHzAnim(meshIdx);
                if (rg.ok()) {
                    em.hzJointPos=std::move(rg.jointPos); em.hzJointQuat=std::move(rg.jointQuat); em.hzJointScale=std::move(rg.jointScale);
                    em.hzParents=std::move(rg.parents); em.hzBoneIdx=std::move(rg.boneIdx); em.hzBoneWgt=std::move(rg.boneWgt);
                    em.hzTrsLocal=std::move(rg.trsLocal); em.hzRestPos=std::move(rg.restPos);
                    em.hzJointCount=rg.jointCount; em.hzFrames=rg.frameCount; em.hzFps=rg.fps;
                    em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                    return;
                }
            } else {
                std::vector<float> tof; float tloop=0.f;
                if (opa.cookExtractNodeTranslateFrames(meshIdx, 64, tof, tloop)) {
                    float md=0.f; for (float v : tof){ float a=std::fabs(v); if(a>md)md=a; }
                    if (md > 1.f) {
                        em.transAnim=true; em.transN=64; em.transFrames=std::move(tof); em.transLoop=tloop;
                        em.hzJointCount=0; em.hzFrames=0; em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                        return;
                    }
                }
            }
            }   // end HSR_OPAHZ gate
            // NODE-SCALE "breathe" (NON-UNIFORM per-axis) -> shadergen::SCALE getTime() shader (faithful per-axis amplitudes).
            // Sits before the translation/spin paths so a pure scale-pulse takes the scale shader. HSR_NOSCALE skips it.
            if (!std::getenv("HSR_NOSCALE") && opa.cookExtractNodeScaleFrames(meshIdx, 16, em.scaleFrames, em.scaleLoop, em.scalePivot)) {
                em.scaleAnim=true; em.scaleN=16;
                em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                return;
            }
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
                // FAST ROCKING (torch flames: sub-5s keyed flicker) — the single-cosine SWAY fit is too crude for it:
                // the (amp/2)(1-cos) shader swings ONE-SIDED while the source rocks both ways, and the amplitude/axis
                // come from a probe fit of a 38-key flicker ("flame anim not correct"). REPLAY the node's EXACT
                // per-frame relative rotation instead — the same ROTREPLAY getTime vertex shader the aurora uses:
                // faithful rocking shape + phase, exact node-origin pivot. Slow ambient sways (foliage ~29s, ±1-3°)
                // keep the cheap fitted oscillate shader (looked right; replay would add shaders for no visible gain).
                bool fastOsc = r.isOsc && r.period > 1e-3f && r.period < 5.f && !std::getenv("HSR_NOROTREPLAY");
                if (fastOsc) {
                    int RN=128; if(const char*e=std::getenv("HSR_ROTKEYS")){int c=atoi(e);if(c>=8)RN=c;}
                    float pivot[3]; std::vector<float> quats; float loopSec=0.f;
                    if (opa.cookExtractRotationReplay(meshIdx,RN,pivot,quats,loopSec) && (int)quats.size()==(RN+1)*4) {
                        em.rotReplay=true; em.rotReplayN=RN; em.rotLoopSec=loopSec; em.rotQuats=std::move(quats);
                        em.rotPivot[0]=pivot[0]; em.rotPivot[1]=pivot[1]; em.rotPivot[2]=pivot[2];
                        em.rotAnim=true; em.vatOffsets.clear(); em.vatFrames=0;
                        if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[COOK] m%d FAST-SWAY -> ROTREPLAY %d keys loop=%.2fs (exact rocking, node pivot)\n", meshIdx, RN, loopSec);
                    }
                }
                if (!em.rotReplay) {
                    em.rotAnim=true; em.rotOmega=r.omega; em.rotOsc=r.isOsc; em.rotAmp=r.amp; em.rotPeriod=r.period;
                    em.rotAxis[0]=r.axis[0]; em.rotAxis[1]=r.axis[1]; em.rotAxis[2]=r.axis[2];
                    em.rotPivot[0]=r.pivot[0]; em.rotPivot[1]=r.pivot[1]; em.rotPivot[2]=r.pivot[2];
                    em.vatOffsets.clear(); em.vatFrames=0; }
                }
            auto fit = g_opaFlip.find((size_t)meshIdx);   // ATLAS flipbook (waterfall/stream/fog) -> OFFSET flipbook shader (frame-snap)
            if (fit != g_opaFlip.end()) {
                em.flipbook=true; em.flipOffset=true; em.blend=true;
                em.flipCols=fit->second.cols; em.flipRows=fit->second.rows;
                em.flipFrames=fit->second.frames; em.flipFps=fit->second.fps;
                em.rotAnim=false; em.uvScroll=false; em.vatOffsets.clear(); em.vatFrames=0;
                return;
            }
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
        if (uploadNext >= uploadEnd) {   // blocking-upload path: session restore right away (as before)
            editor.projectPath = apkPath;   // session saves/loads to <env>.hsledit
            editor.loadProject();           // auto-restore a prior session (transforms/renames/items) if one exists
        }
        // progressive path: projectPath/loadProject happen when the LAST mesh lands (the session applies
        // per-mesh transforms by gpuMeshes index, so every index must exist first) — see the render loop.
    }

    // One-shot headless re-cook: HSR_EXPORT exports the loaded (optionally edited) scene to an APK then,
    // with HSR_EXPORT_QUIT, exits — lets the editor's Export path run batch / from the command line.
    if (std::getenv("HSR_EXPORT")) {
        if (editor.projectPath.empty()) { editor.projectPath = apkPath; editor.loadProject(); }   // headless cook includes the saved session
        // Populate md.positions at the t=0 REST frame (the render loop hasn't run yet). REST (not mid-anim) is REQUIRED:
        // node-anim meshes are dynamicVerts so md.positions is what gets baked, and the getTime TRANSLATE/ROTATE shaders add
        // their offset RELATIVE TO t=0 — baking a mid-anim frame double-applied the motion (Star Trek screens "go beyond").
        // Skinned FLIPBOOKS still collapse here: at t=0 the loop's first cell is the only one ON (others scaled to 0).
        if (isV79 && gltf.hasAnimation()) gltf.animate(0.f);
        else if (isOpa) opa.animate(0.f);   // OPA SKINNED meshes: the skin loader stores PRE-skin (joint-local) basePos, so md.positions
                                            // only becomes the real bind geometry once the clip is evaluated. The render loop hasn't run
                                            // yet at cook time, so without this the cooked skinned verts collapse to a vertical streak
                                            // ("messed up on conversion" / spiky skin.opa). animate(0.f) bakes the t=0 bind pose into
                                            // md.positions (matches the glTF path above) -> correct STATIC geometry AND the bind verts the device re-skins from.
        editor.exportAPKSync();   // synchronous cook + auto-sign with a terminal progress bar
        if (std::getenv("HSR_EXPORT_QUIT")) return 0;
    }

    // One-shot Blender export: HSR_BLENDER_EXPORT writes the loaded env to a glTF 2.0 project (meshes + materials +
    // textures + per-mesh node transforms) under blender_export/<env>/ that opens directly in Blender, plus a
    // <env>.blendmeta.json sidecar (per-object src index + hstf components) for re-import & re-cook. HSR_EXPORT_QUIT exits.
    if (std::getenv("HSR_BLENDER_EXPORT") && sceneMeshes) {
        if (isV79 && gltf.hasAnimation()) gltf.animate(0.f);   // bake the t=0 rest frame (same as the APK cook path)
        else if (isOpa) opa.animate(0.f);
        std::string base = apkPath; size_t sl = base.find_last_of("/\\"); if (sl != std::string::npos) base = base.substr(sl+1);
        size_t dot = base.find_last_of('.'); if (dot != std::string::npos) base = base.substr(0, dot);
        std::string outDir = "blender_export/" + base;
        auto ems = editor.buildExportMeshes();   // FULL source: geometry + skins + skeletal clips + node anims + materials
        bool ok = !ems.empty() && gltfexport::exportEnvFull(ems, outDir, base, "");
        fprintf(stderr, "[BLENDER] %s -> %s/%s.gltf  (%zu meshes, skins+anims)\n", ok ? "exported" : "FAILED", outDir.c_str(), base.c_str(), ems.size());
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
        //   geometry flung far away that tears holes) | matinfo=i (shader/VAT/bones/blend flags)
        //   dump=i (FULL data-extract for ONE mesh: coordinates/shader/material/matParams/textures/
        //          animation/skeleton/components — the cooker ground-truth reference) | quit
#ifdef _WIN32
        if (liveMode && uploadNext >= uploadEnd) {   // DEFER live commands until the scene finished streaming in (a shot/listmesh mid-upload would see a partial scene)
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
                    else if (strncmp(ln, "at=", 3) == 0)          { float v = (float)atof(ln + 3);   // SCRUB anim time (seconds); at=-1 -> resume real-time
                        // PIN, don't just set: the editor timeline advances *animScrub every frame while
                        // animPlaying, so a bare g_animScrub write drifted away immediately (the reason
                        // bridge captures kept relaunching with HSR_ANIMTIME instead of scrubbing live).
                        if (v < 0.f) { g_animOverride = false; if (g_editor) g_editor->animPlaying = true; }
                        else { g_animOverride = true; g_animScrub = v; if (g_editor) g_editor->animPlaying = false; } }
                    else if (strncmp(ln, "fov=", 4) == 0)         C.fovDeg = (float)atof(ln + 4);
                    else if (strncmp(ln, "far=", 4) == 0)         C.farZ = (float)atof(ln + 4);
                    else if (strncmp(ln, "shot=", 5) == 0)        shotThis = ln + 5;
                    else if (strncmp(ln, "hidemesh=", 9) == 0)    vkRenderer.hideMesh = atoi(ln + 9);
                    else if (strncmp(ln, "solomesh=", 9) == 0)  { vkRenderer.soloMesh = atoi(ln + 9);
                        // solo AUTO-FOCUSES (same as the context menu): a soloed mesh you can't see is useless
                        if (vkRenderer.soloMesh >= 0 && g_editor && vkRenderer.soloMesh < (int)vkRenderer.gpuMeshes.size())
                            g_editor->focusMesh(vkRenderer.gpuMeshes[vkRenderer.soloMesh]); }
                    else if (strncmp(ln, "delmesh=", 8) == 0) {   // editor mesh DELETE toggle (drop from render + cook)
                        int mi=atoi(ln+8); if (mi>=0 && mi<(int)vkRenderer.gpuMeshes.size()) { vkRenderer.setDeleted(mi, !vkRenderer.isDeleted(mi));
                            snprintf(tmp,sizeof tmp,"mesh %d deleted=%d (deletedCount=%d)\n", mi, (int)vkRenderer.isDeleted(mi), vkRenderer.deletedCount()); out += tmp; } }
                    else if (strncmp(ln, "dupmesh=", 8) == 0) {   // editor mesh DUPLICATE (clone + offset)
                        int mi=atoi(ln+8); size_t before=vkRenderer.gpuMeshes.size();
                        editor.selectOne(mi); editor.duplicateSelected();
                        snprintf(tmp,sizeof tmp,"duplicated mesh %d -> now %zu meshes (was %zu)\n", mi, vkRenderer.gpuMeshes.size(), before); out += tmp; }
                    else if (strncmp(ln, "settex=", 7) == 0) {   // editor SET texture from a PNG/JPG (skybox/any mesh): settex=<mi>,<path>
                        const char* a=ln+7; const char* c=strchr(a,',');
                        if (c) { int mi=atoi(std::string(a,c-a).c_str()); bool ok=editor.setMeshTexture(mi, c+1);
                            snprintf(tmp,sizeof tmp,"settex %d -> %s\n", mi, ok?"ok":"FAILED"); out += tmp; } }
                    else if (strncmp(ln, "exporttex=", 10) == 0) {   // editor EXPORT a mesh's texture to PNG: exporttex=<mi>,<path>
                        const char* a=ln+10; const char* c=strchr(a,',');
                        if (c) { int mi=atoi(std::string(a,c-a).c_str()); bool ok=editor.exportMeshTexture(mi, c+1);
                            snprintf(tmp,sizeof tmp,"exporttex %d -> %s\n", mi, ok?"ok":"FAILED"); out += tmp; } }
                    else if (strncmp(ln, "skycolor=", 9) == 0) {   // editor GENERAL skybox color (any env): skycolor=<r>,<g>,<b> (0..1)
                        float rr,gg,bb; if (sscanf(ln+9,"%f,%f,%f",&rr,&gg,&bb)==3) { editor.setSkyColor(rr,gg,bb);
                            snprintf(tmp,sizeof tmp,"skycolor=(%.3f,%.3f,%.3f)\n",rr,gg,bb); out += tmp; } }
                    else if (strncmp(ln, "skyimage=", 9) == 0) {   // skybox from an IMAGE file (equirect): skyimage=<path>
                        bool ok=editor.setSkyImage(ln+9); snprintf(tmp,sizeof tmp,"skyimage -> %s\n", ok?"ok":"FAILED"); out += tmp; }
                    else if (strncmp(ln, "skytexmesh=", 11) == 0) {   // skybox from an EXISTING mesh TEXTURE: skytexmesh=<mi>
                        bool ok=editor.setSkyImageFromMesh(atoi(ln+11)); snprintf(tmp,sizeof tmp,"skytexmesh -> %s\n", ok?"ok":"FAILED"); out += tmp; }
                    else if (strncmp(ln, "audiofile=", 10) == 0) {   // REPLACE/ADD the background loop from a file (previews + cooks)
                        bool ok=editor.setAudioFromFile(ln+10); snprintf(tmp,sizeof tmp,"audiofile -> %s\n", ok?"ok":"FAILED"); out += tmp; }
                    else if (strncmp(ln, "audioexport", 11) == 0) {   // export the current loop next to the session in saved/
                        bool ok=editor.exportAudio(); snprintf(tmp,sizeof tmp,"audioexport -> %s\n", ok?"ok":"FAILED"); out += tmp; }
                    else if (strncmp(ln, "audiorevert", 11) == 0) {   // drop the override -> env's own theme
                        editor.clearAudioOverride(); out += "audiorevert -> ok\n"; }
                    else if (strncmp(ln, "exportsky=", 10) == 0) {   // export the current skybox texture: exportsky=<path>
                        bool ok=editor.exportSkyImage(ln+10); snprintf(tmp,sizeof tmp,"exportsky -> %s\n", ok?"ok":"FAILED"); out += tmp; }
                    else if (strncmp(ln, "movemesh=", 9) == 0) {   // live-edit: world-translate ONE mesh (test placement fixes without recompiling)
                        int mi; float dx, dy, dz;
                        if (sscanf(ln + 9, "%d,%f,%f,%f", &mi, &dx, &dy, &dz) == 4 && mi >= 0 && mi < (int)vkRenderer.gpuMeshes.size()) {
                            // Route through the EDITOR transform (editT + recomputeModel), NOT the raw model matrix:
                            // the raw-matrix poke left the Object panel stale, the next editor edit clobbered the move,
                            // and it never saved/cooked. Through editT the UI, .hsledit session and cook all see it.
                            auto& gm = vkRenderer.gpuMeshes[mi];
                            gm.editT[0] += dx; gm.editT[1] += dy; gm.editT[2] += dz;
                            if (g_editor) g_editor->recomputeModel(gm);
                            else { gm.model[12] += dx; gm.model[13] += dy; gm.model[14] += dz; }   // HSR_NOUI: no editor -> old direct poke
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
                    else if (strncmp(ln, "dump=", 5) == 0) {   // FULL data-extract for ONE mesh: material/shader/animation/skeleton/coordinates.
                        // Ground-truth reference to fix the cooker: dumps the RENDERER's processed data for any render mode (OPA/V203/glTF/V79).
                        int mi = atoi(ln + 5);
                        if (mi < 0 || mi >= (int)sceneMeshes->size() || mi >= (int)vkRenderer.gpuMeshes.size()) { out += "dump: bad mesh index\n"; }
                        else {
                        const auto& md = (*sceneMeshes)[mi]; const auto& gm = vkRenderer.gpuMeshes[mi]; const float* M = gm.model;
                        snprintf(tmp,sizeof tmp,"===== DUMP mesh [%d] '%s' =====\n", mi, md.name.c_str()); out += tmp;
                        snprintf(tmp,sizeof tmp,"  meshPath=%s\n", md.meshPath.c_str()); out += tmp;
                        // -- COORDINATES: processed world matrix (column-major), transform, world AABB --
                        snprintf(tmp,sizeof tmp,"[COORD] worldMatrix(col-major)=\n  [%.4f %.4f %.4f %.4f]\n  [%.4f %.4f %.4f %.4f]\n  [%.4f %.4f %.4f %.4f]\n  [%.4f %.4f %.4f %.4f]\n",
                            M[0],M[4],M[8],M[12], M[1],M[5],M[9],M[13], M[2],M[6],M[10],M[14], M[3],M[7],M[11],M[15]); out += tmp;
                        snprintf(tmp,sizeof tmp,"  worldPos=(%.3f,%.3f,%.3f) transform.pos=(%.3f,%.3f,%.3f) rot=(%.4f,%.4f,%.4f,%.4f) scale=(%.3f,%.3f,%.3f)\n",
                            M[12],M[13],M[14], md.transform.pos[0],md.transform.pos[1],md.transform.pos[2],
                            md.transform.rot[0],md.transform.rot[1],md.transform.rot[2],md.transform.rot[3], md.transform.scale[0],md.transform.scale[1],md.transform.scale[2]); out += tmp;
                        { float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f}; size_t nv=md.positions.size()/3;
                          for(size_t i=0;i<nv;i++){ float lx=md.positions[i*3],ly=md.positions[i*3+1],lz=md.positions[i*3+2];
                            float wx=M[0]*lx+M[4]*ly+M[8]*lz+M[12],wy=M[1]*lx+M[5]*ly+M[9]*lz+M[13],wz=M[2]*lx+M[6]*ly+M[10]*lz+M[14];
                            for(int k=0;k<3;k++){ float w=(k==0?wx:k==1?wy:wz); if(w<mn[k])mn[k]=w; if(w>mx[k])mx[k]=w; } }
                          if(nv) { snprintf(tmp,sizeof tmp,"  worldAABB min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)\n", mn[0],mn[1],mn[2],mx[0],mx[1],mx[2]); out+=tmp; } }
                        // PER-VERTEX world positions (quad orientation/curve): the ACTUAL posed geometry the renderer draws.
                        { size_t nv=md.positions.size()/3;
                          for(size_t vv=0; vv<nv && vv<8; vv++){ float lx=md.positions[vv*3],ly=md.positions[vv*3+1],lz=md.positions[vv*3+2];
                            float wx=M[0]*lx+M[4]*ly+M[8]*lz+M[12],wy=M[1]*lx+M[5]*ly+M[9]*lz+M[13],wz=M[2]*lx+M[6]*ly+M[10]*lz+M[14];
                            snprintf(tmp,sizeof tmp,"    v%zu world=(%.2f,%.2f,%.2f)\n",vv,wx,wy,wz); out+=tmp; } }
                        snprintf(tmp,sizeof tmp,"  nVerts=%u nIdx=%u vboStride=%u posOff=%u uvOff=%u\n", md.nVerts, md.nIdx, gm.vboStride, gm.posOffset, gm.uvOffset); out += tmp;
                        // -- SHADER --
                        snprintf(tmp,sizeof tmp,"[SHADER] path=%s\n  shaderIng=%llu progIdx=%d\n", md.shaderPath.c_str(), (unsigned long long)md.shaderIng, gm.progIdx); out += tmp;
                        // -- MATERIAL flags + tint --
                        snprintf(tmp,sizeof tmp,"[MATERIAL] blend=%d additive=%d alphaTest=%d doubleSided=%d tiled=%d iblLit=%d skybox=%d\n",
                            (int)md.useBlend,(int)md.additive,(int)md.alphaTest,(int)md.doubleSided,(int)md.tiled,(int)md.iblLit,(int)md.isSkybox); out += tmp;
                        snprintf(tmp,sizeof tmp,"  tint=(%.3f,%.3f,%.3f,%.3f) atlasCell=%.0f\n", md.tint[0],md.tint[1],md.tint[2],md.tint[3], md.atlasCellIndex); out += tmp;
                        // -- MATPARAMS: decode the cooked constant block by name-hash + float values --
                        if (!md.constParams.empty()) { out += "  matParams (nameHash : values):\n";
                          for (const auto& cp : md.constParams) { int nfl = (int)(cp.byteSize/4);
                            snprintf(tmp,sizeof tmp,"    %08x [%uB]:", cp.nameHash, cp.byteSize); out += tmp;
                            for (int f=0; f<nfl && f<8; f++) { float v=0; if ((size_t)cp.blobOffset+f*4+4<=md.matParamsBlob.size()) memcpy(&v,&md.matParamsBlob[cp.blobOffset+f*4],4);
                              snprintf(tmp,sizeof tmp," %.4f", v); out += tmp; } out += "\n"; } }
                        if (!md.matOverrides.empty()) { out += "  matOverrides (per-instance):\n";
                          for (const auto& mo : md.matOverrides) { snprintf(tmp,sizeof tmp,"    %s = (%.4f,%.4f,%.4f,%.4f)\n", mo.name.c_str(), mo.v[0],mo.v[1],mo.v[2],mo.v[3]); out += tmp; } }
                        // -- TEXTURES --
                        snprintf(tmp,sizeof tmp,"[TEX] base=%dx%d(%d) normal=%dx%d(%d) orm=%dx%d(%d) emissive=%dx%d(%d) lightmap=%dx%d(%d)\n",
                            md.texW,md.texH,(int)md.hasTexture, md.normalW,md.normalH,(int)md.hasNormal, md.ormW,md.ormH,(int)md.hasOrm,
                            md.emissiveW,md.emissiveH,(int)md.hasEmissive, md.lmW,md.lmH,(int)md.hasLightmap); out += tmp;
                        if (!md.ormTexName.empty()) { snprintf(tmp,sizeof tmp,"  ormTexName=%s (lightmap merge-group)\n", md.ormTexName.c_str()); out += tmp; }
                        // -- ANIMATION --
                        snprintf(tmp,sizeof tmp,"[ANIM] hasVat=%d vat=%ux%u(verts x frames) vatTrack=%.1f rate=%.3f timeOff=%.3f dynamicVerts=%d gltfMeshIdx=%d\n",
                            (int)md.hasVat, md.vatW, md.vatH, md.vatTrackIndex, md.vatRateFactor, md.vatTimeOffset, (int)md.dynamicVerts, md.gltfMeshIndex); out += tmp;
                        // -- SKELETON --
                        snprintf(tmp,sizeof tmp,"[SKEL] hasBones=%d isSkinned=%d bonePalette=%zu slots boneIdx=%zu boneWt=%zu\n",
                            (int)md.hasBones, (int)gm.isSkinned, md.bonePalette.size(), md.boneIndices.size()/4, md.boneWeights.size()/4); out += tmp;
                        if (!md.bonePalette.empty()) { out += "  palette jointHashes:";
                          for (size_t i=0;i<md.bonePalette.size() && i<24;i++){ snprintf(tmp,sizeof tmp," %08x", md.bonePalette[i]); out += tmp; } out += "\n"; }
                        // -- COOK-ANIM CLASSIFICATION (OPA): EXACTLY what device shader/anim this mesh cooks to.
                        //    Diagnoses "jumping / not showing": the first TRUE branch (in cook priority order) wins. --
                        if (isOpa) {
                            std::vector<float> cmats; int cmN=0; float cmL=0.f;
                            bool uvm = opa.cookExtractUVMatrices(mi, 256, cmats, cmN, cmL);
                            std::vector<float> ctof; float ctl=0.f;
                            bool tr = opa.cookExtractNodeTranslateFrames(mi, 24, ctof, ctl);
                            float tmd=0.f; for(float v:ctof){ float a=fabsf(v); if(a>tmd)tmd=a; }
                            bool hz = opa.extractHzAnim(mi).ok();
                            bool rg = opa.extractNodeRigidHzAnim(mi).ok();
                            std::vector<float> csf; float csl=0.f; float csp[3]={0,0,0};
                            bool sc = opa.cookExtractNodeScaleFrames(mi, 16, csf, csl, csp);
                            float ctd[3]={0,0,0}; bool nt = opa.extractNodeTranslate(mi, ctd);
                            int myNode = opa.animNodeOf(mi);
                            bool nodeTrans = std::getenv("HSR_NODETRANSLATE") != nullptr;   // opt-in reroute (breaks animator)
                            bool tt = tr && tmd > 1.f && nodeTrans;
                            // PURE CONTINUOUS SCROLL shortcut — MUST mirror hzAnimExtractor: a clean scroll (identity 2x2 +
                            // small const translate, no fade/motion) cooks to the SMOOTH uv+=rate*time shader, NOT the matrix
                            // replay (which snaps = choppy + border seams). Without this the dump mislabeled waterfalls as
                            // FLIPBOOK-UVMATRIX even though the cook now emits a smooth scroll.
                            bool pureScroll=false; float srU=0.f, srV=0.f;
                            if (!std::getenv("HSR_NOUVSCROLLFAST")) {
                                std::unordered_map<size_t,std::pair<float,float>> uvsMap; opa.cookExtractUVScroll(uvsMap);
                                if (uvsMap.count(mi) && !opa.nodeAnimatesRotOrScale(mi)) {
                                    std::vector<float> fa2; float fl2=0.f; bool hasFade2 = opa.cookExtractTintAlpha(mi,fa2,fl2)&&fa2.size()>=2;
                                    std::vector<float> tf2; float tl2=0.f; bool hasTr2=false;
                                    if (opa.cookExtractNodeTranslateFrames(mi,24,tf2,tl2)){ float md=0.f; for(float v:tf2){float a=fabsf(v);if(a>md)md=a;} hasTr2=md>1.f; }
                                    if (!hasFade2 && !hasTr2) { pureScroll=true; srU=uvsMap[mi].first; srV=uvsMap[mi].second; }
                                }
                            }
                            const char* winner = pureScroll ? "UV-SCROLL(smooth uv+=rate*time)" :
                                                  uvm ? "FLIPBOOK-UVMATRIX(frag getTime)" : hz ? "HZANIM-SKINNED" :
                                                  tt ? "TRANSLATE-getTime(node path; HSR_NODETRANSLATE)" :
                                                  rg ? "RIGID-HZANIM(node path)" : sc ? "SCALE-getTime" :
                                                  nt ? "POSE-TRANSLATE(node move)" : "STATIC(no anim)";
                            snprintf(tmp,sizeof tmp,"[COOK-ANIM] => %s   (node=%d)\n", winner, myNode); out += tmp;
                            if (pureScroll) { snprintf(tmp,sizeof tmp,"  uvScroll rate=(%.4f,%.4f) UV/s (continuous, REPEAT-wrap; no frame snap => smooth, no borders)\n", srU, srV); out += tmp; }
                            snprintf(tmp,sizeof tmp,"  uvMatrix=%d(N=%d loop=%.2fs) nodeTranslate=%d(maxDelta=%.2fu) hzSkinned=%d rigid=%d scale=%d poseTrans=%d(d=%.2f,%.2f,%.2f)\n",
                                (int)uvm,cmN,cmL, (int)tr,tmd, (int)hz,(int)rg,(int)sc,(int)nt, ctd[0],ctd[1],ctd[2]); out += tmp;
                            if (uvm && cmN>0) { out += "  UV matrices [a b c | d e f] (device replays these in FRAGMENT):\n";
                              for (int f=0; f<cmN && f<5 && (int)cmats.size()>=(f+1)*6; f++){
                                snprintf(tmp,sizeof tmp,"    f%d: %.4f %.4f %.4f | %.4f %.4f %.4f\n", f,
                                  cmats[f*6],cmats[f*6+1],cmats[f*6+2],cmats[f*6+3],cmats[f*6+4],cmats[f*6+5]); out += tmp; } }
                            if (tr && ctof.size()>=6) { int tn = (int)(ctof.size()/3);
                              float sx=ctof[(tn-1)*3]-ctof[0], sy=ctof[(tn-1)*3+1]-ctof[1], sz=ctof[(tn-1)*3+2]-ctof[2];
                              float seam=sqrtf(sx*sx+sy*sy+sz*sz), span=0.f;
                              for(int f=0;f<tn;f++){ float m=std::max({fabsf(ctof[f*3]),fabsf(ctof[f*3+1]),fabsf(ctof[f*3+2])}); if(m>span)span=m; }
                              bool looping = seam < 0.20f*span + 0.5f;
                              snprintf(tmp,sizeof tmp,"  node-translate: %s (seam=%.2f span=%.2f) => device %s\n",
                                looping?"CYCLIC-LOOP":"ONE-WAY", seam, span, looping?"WRAPS (smooth, no jump)":"HOLDS+teleport"); out += tmp;
                              for (int f=0; f<tn && f<12; f++){ snprintf(tmp,sizeof tmp,"    t%d: %.3f %.3f %.3f\n", f, ctof[f*3],ctof[f*3+1],ctof[f*3+2]); out += tmp; } }
                            // FADE (mat.sanim MaterialTint alpha) — the fog/dust fade-in/out baked into the frag getTime.
                            { std::vector<float> cfa; float cfl=0.f;
                              bool fade = opa.cookExtractTintAlpha(mi, cfa, cfl) && cfa.size()>=2;
                              if (fade) { float amn=1e9f,amx=-1e9f; for(float a:cfa){ if(a<amn)amn=a; if(a>amx)amx=a; }
                                snprintf(tmp,sizeof tmp,"  FADE(MaterialTint.a): N=%zu naturalLoop=%.2fs alpha[min=%.3f max=%.3f]  (cooked loop=UV %.2fs, synced) => baked into frag getTime\n",
                                  cfa.size(), cfl, amn, amx, cmL); out += tmp; }
                              else out += "  FADE: none\n"; }
                            // SCALE breathe (chained into the effect-card shader when present) — amplitude + loop.
                            { std::vector<float> csf2; float csl2=0.f; float csp2[3]={0,0,0};
                              bool scb = opa.cookExtractNodeScaleFrames(mi, 16, csf2, csl2, csp2);
                              if (scb) { float dev=0.f; for(float f:csf2){ float d=f>1.f?f-1.f:1.f-f; if(d>dev)dev=d; }
                                snprintf(tmp,sizeof tmp,"  SCALE(breathe): loop=%.2fs maxDev=%.3f pivot=(%.2f,%.2f,%.2f) => CHAINED into effect-card shader\n", csl2, dev, csp2[0],csp2[1],csp2[2]); out += tmp; }
                              else out += "  SCALE: none\n"; }
                        }
                        // -- COMPONENTS (all hstf components on the entity) --
                        if (!md.components.empty()) { snprintf(tmp,sizeof tmp,"[COMPONENTS] %zu:\n", md.components.size()); out += tmp;
                          for (const auto& c : md.components) { snprintf(tmp,sizeof tmp,"    %s v%d (%zu fields)\n", c.shortCls.c_str(), c.version, c.fields.size()); out += tmp; } }
                        out += "===== END DUMP =====\n";
                        }
                    }
                    else if (strncmp(ln, "hidemat=", 8) == 0)     vkRenderer.hideMat = ln + 8;   // std::string; empty = none (checked via .empty())
                    else if (strncmp(ln, "solomat=", 8) == 0)     vkRenderer.soloMat = ln + 8;
                    else if (strncmp(ln, "wire=", 5) == 0)        vkRenderer.wireframe = atoi(ln + 5) != 0;
                    else if (strncmp(ln, "clear", 5) == 0)      { vkRenderer.hideMesh = -1; vkRenderer.soloMesh = -1; vkRenderer.hideMat.clear(); vkRenderer.soloMat.clear(); vkRenderer.wireframe = false; }
                    // ── editor-feature parity (drive + VERIFY the same ops the UI has, via the MCP bridge) ──
                    else if (strncmp(ln, "dup=", 4) == 0)       { if (g_editor && g_editor->ready) { g_editor->selectOne(atoi(ln+4)); g_editor->duplicateSelected(); out += "dup ok, now " + std::to_string(vkRenderer.gpuMeshes.size()) + " meshes\n"; } }
                    else if (strncmp(ln, "del=", 4) == 0)       { if (g_editor && g_editor->ready) { g_editor->selectOne(atoi(ln+4)); g_editor->toggleDeleteSelected(); out += "del toggled\n"; } }
                    else if (strncmp(ln, "settex=", 7) == 0)    { // settex=<idx>,<imagePath>
                        const char* c = strchr(ln+7, ','); if (c && g_editor && g_editor->ready) { bool ok=g_editor->setMeshTexture(atoi(ln+7), std::string(c+1)); out += ok?"settex ok\n":"settex FAILED\n"; } }
                    else if (strncmp(ln, "tint=", 5) == 0)      { // tint=<idx>,r,g,b,a  (per-mesh material tint, live)
                        int mi; float tr,tg,tb,ta;
                        if (sscanf(ln+5, "%d,%f,%f,%f,%f", &mi,&tr,&tg,&tb,&ta)==5 && mi>=0 && mi<(int)vkRenderer.gpuMeshes.size()) {
                            auto& gm=vkRenderer.gpuMeshes[mi]; gm.editTint[0]=tr; gm.editTint[1]=tg; gm.editTint[2]=tb; gm.editTint[3]=ta;
                            if (g_editor) g_editor->matEdited.insert(mi); out += "tint ok\n"; } }
                    else if (strncmp(ln, "light=", 6) == 0)     { // light=r,g,b,a  (GLOBAL light manipulation, live)
                        float lr,lg,lb,la; if (sscanf(ln+6, "%f,%f,%f,%f", &lr,&lg,&lb,&la)==4) {
                            vkRenderer.lightMul[0]=lr; vkRenderer.lightMul[1]=lg; vkRenderer.lightMul[2]=lb; vkRenderer.lightMul[3]=la; out += "light ok\n"; } }
                    else if (strncmp(ln, "matflag=", 8) == 0)   { // matflag=<idx>,<blend>,<additive>,<alphaTest>,<cullBack>
                        int mi,fb,fa,ft,fc;
                        if (sscanf(ln+8, "%d,%d,%d,%d,%d", &mi,&fb,&fa,&ft,&fc)==5 && mi>=0 && mi<(int)vkRenderer.gpuMeshes.size()) {
                            auto& gm=vkRenderer.gpuMeshes[mi]; gm.useBlend=fb!=0; gm.additive=fa!=0; gm.alphaTest=ft!=0; gm.cullBack=fc!=0;
                            if (sceneMeshes && mi<(int)sceneMeshes->size()) { auto& md=(*sceneMeshes)[mi]; md.useBlend=gm.useBlend; md.additive=gm.additive; md.alphaTest=gm.alphaTest; md.doubleSided=!gm.cullBack; }
                            if (g_editor) g_editor->matEdited.insert(mi); out += "matflag ok\n"; } }
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
        // GATED on upload completion: animate() rewrites md.positions, and uploadMesh reads
        // md.positions — animating mid-stream would bake NOT-YET-UPLOADED meshes at a mid-anim
        // pose (and looks wrong: things moving while the scene is still popping in).
        if (isV79 && gltf.hasAnimation() && uploadNext >= uploadEnd) {
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
        // GATED on upload completion (same reason as the glTF block above).
        if (isOpa && opa.hasAnimation() && uploadNext >= uploadEnd) {
            float at = g_animOverride ? g_animScrub : ((fixedAnimTime >= 0.f) ? fixedAnimTime : std::chrono::duration<float>(now - animStart).count());
            opa.animate(at);
            for (size_t i = 0; i < opa.meshes.size() && i < vkRenderer.gpuMeshes.size(); ++i) {
                auto& md = opa.meshes[i];
                auto& gm = vkRenderer.gpuMeshes[i];
                if (std::getenv("HSR_FADEDBG") && md.name.find("dust")!=std::string::npos)
                    fprintf(stderr,"[FADEDBG] m%zu '%s' dyn=%d vbomap=%d md.curTint.a=%.3f gm.curTint.a=%.3f\n",
                        i, md.name.c_str(), (int)gm.dynamicVerts, (int)(gm.vboMapped!=nullptr), md.curTint[3], gm.curTint[3]);
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

        // PROGRESSIVE GPU UPLOAD: stream a ~20ms time-slice of meshes to the GPU each frame. The window +
        // editor UI render normally in between, so the scene visibly pops in mesh-by-mesh with a loading
        // bar instead of a white frozen window (the old flow blocked here for the entire upload).
        if (uploadNext < uploadEnd) {
            auto ut0 = std::chrono::high_resolution_clock::now();
            do {
                if (uploadNext >= minMesh && uploadNext < maxMesh)
                    vkRenderer.uploadMesh((*sceneMeshes)[uploadNext]);
                ++uploadNext;
            } while (uploadNext < uploadEnd &&
                     std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - ut0).count() < 20.f);
            editor.uploadCur = uploadNext; editor.uploadTotal = uploadEnd;   // editor draws the loading bar
            vkRenderer.farFitDone = false;   // re-fit the far plane to what has arrived so far
            if (uploadNext >= uploadEnd) {
                editor.uploadCur = editor.uploadTotal = 0;   // done -> hide the loading bar
                fprintf(stderr, "[MAIN] GPU upload complete: %zu meshes\n\n", vkRenderer.gpuMeshes.size());
                animStart = std::chrono::high_resolution_clock::now();   // animations START now (frozen during load)
                // Session restore now that every gpuMeshes index exists (it applies per-mesh transforms by index).
                if (!std::getenv("HSR_NOUI")) { editor.projectPath = apkPath; editor.loadProject(); }
            }
        }

        // Tie the cooked getTime() shader anims (rot/osc/wispscale/flipbook/VAT) + procedural motes/prism to the EDITOR
        // TIMELINE so pause/scrub/loop control EVERY animation (they were free-running on wall-clock = ignored the
        // timeline). g_animOverride is true while the editor drives the playhead; HSR_ANIMTIME freezes; else (pure
        // headless) <0 keeps the renderer's own wall-clock. FROZEN at t=0 while meshes are still uploading.
        vkRenderer.extAnimTime = (uploadNext < uploadEnd) ? 0.f
                               : g_animOverride ? g_animScrub
                               : ((fixedAnimTime >= 0.f) ? fixedAnimTime : -1.f);
        vkRenderer.render();
        glfwPollEvents();
        // (pick + the gizmo are handled immediately in editor.onMouseButton, using the viewport pane rect)

        // Drag-and-drop: a .hsrprefab SPAWNS into the current scene (no reload!); anything else swaps
        // the env IN PLACE (same window/process - no relaunch, no focus steal).
        if (g_doReload) {
            if (g_dropPath.size() > 10 && g_dropPath.substr(g_dropPath.size()-10) == ".hsrprefab") {
                if (!std::getenv("HSR_NOUI") && editor.ready) editor.spawnPrefab(g_dropPath);
                g_doReload = false; g_dropPath.clear();
            }
            else if (interactive) { nextEnv = g_dropPath; g_doReload = false; g_dropPath.clear(); break; }
#ifdef _WIN32
            relaunchSelf(g_dropPath);
#endif
            break;
        }
        // Editor-driven swap (Cook panel: "Preview cooked (HSL)" / "Back to source") - same in-place reload.
        if (!editor.swapTo.empty()) {
            if (interactive) { nextEnv = editor.swapTo; editor.swapTo.clear(); break; }
            editor.swapTo.clear();
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
    fprintf(stderr, "\n[MAIN] Shutting down%s...\n", nextEnv.empty() ? "" : " (in-place env swap)");
    if (!std::getenv("HSR_NOUI")) editor.shutdown();
    vkRenderer.cleanup();
    if (!nextEnv.empty() && interactive) {
        // IN-PLACE SWAP: same window, fresh renderer/editor/scene on the new env. The GDI loading
        // splash takes over the window again while the new env parses on the worker thread.
        apkPath = nextEnv;
        envBaseName = apkPath;
        { size_t sl = envBaseName.find_last_of("/\\"); if (sl != std::string::npos) envBaseName = envBaseName.substr(sl + 1); }
        glfwSetWindowTitle(g_window, ("HSR Renderer - loading " + envBaseName + " ...").c_str());
        fprintf(stderr, "[MAIN] IN-PLACE ENV SWAP -> %s\n\n", apkPath.c_str());
        continue;
    }
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
    }   // end IN-PLACE ENV RELOAD LOOP
}
