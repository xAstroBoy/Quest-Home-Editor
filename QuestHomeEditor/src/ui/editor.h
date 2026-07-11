#pragma once
// ── In-window EDITOR — a custom, from-scratch Blender-style C++ UI (NO Dear ImGui) ──────────────────────────
// Drawn through the renderer's own Vulkan via the src/ui/ toolkit (ui_font/ui_draw/ui_core): a tiled Blender
// layout (header + 3D Viewport pane + Outliner + Properties tabs + Timeline), dark Blender-faithful theme,
// click-select + a custom move/rotate/scale gizmo with undo, and the porting tools embedded as Properties
// panels — including a threaded Cook/Export with a live progress bar and one-click APK auto-signing.
// Decoupled from the renderer via its overlayBegin/overlayDraw hooks + uiViewportRect (the 3D scissor pane).
#include "render/vk_renderer.h"
#include "core/audio.h"
#include "core/audio_convert.h"      // decode ANY ogg/wav/mp3/flac for the audio REPLACE/ADD/export UI
#include "core/camera.h"
#include "core/scene_items.h"
#include "cook/hsl_cooker.h"
#include "io/gltf_export.h"          // Blender round-trip: env -> glTF 2.0 project
#include "ui/ui_core.h"
#include "stb_image.h"               // decode PNG/JPG for "set mesh/skybox texture from image"
#include "stb_image_write.h"         // encode PNG for "export mesh/skybox texture"
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>           // MUST precede windows.h (else windows.h pulls winsock v1 -> redefinition); for the Quest-bridge mirror socket
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <shobjidl.h>            // IFileOpenDialog — the full Explorer folder picker for the Blender export path
#else
  // POSIX sockets for the Quest-bridge mirror (same BSD API; alias the few Winsock-isms we use)
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/time.h>
  typedef int SOCKET;
  #ifndef INVALID_SOCKET
  #define INVALID_SOCKET (-1)
  #endif
  #define closesocket ::close
#endif
// Socket recv/send timeout in ms — Winsock takes a DWORD, POSIX a struct timeval.
static inline void hsrSockTimeoutMs(SOCKET s, int ms) {
#ifdef _WIN32
    DWORD to = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
#else
    struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof tv);
#endif
}
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <deque>
#include <fstream>
#include <filesystem>
#include <set>
#include <map>
#include <array>
#include <tuple>
#include <climits>

struct Editor {
    // ── bindings ──
    VkRenderer*  r = nullptr;
    AudioPlayer* audio = nullptr;
    GLFWwindow*  win = nullptr;
    GLFWcursor*  curH = nullptr; GLFWcursor* curV = nullptr; int wantCursor = 0;   // splitter resize cursors (1=H,2=V)
    std::vector<std::pair<std::string,std::string>> adbDeviceList;                 // {serial, "Model (serial)"} for the device picker
    bool adbDevScanned = false;                                                    // auto-scan devices once when the Cook panel opens
    bool ready = false;
    std::string projectPath;           // the loaded env path; the editor session saves/loads to <projectPath>.hsledit
    bool*  animOverride = nullptr;     // main's loop reads these through pointers (timeline scrub)
    float* animScrub    = nullptr;
    float  animDuration = 0.0f;
    bool   animPlaying  = true;        // the editor OWNS the playback clock -> the timeline playhead advances LIVE
    // ── QUEST TIMELINE MIRROR: pause/scrub/speed here also drive the on-device bridge (adb forward tcp:27042)
    //    so the headset freezes/scrubs to the SAME instant. Play/Pause -> `world 1/0`; slider -> `world set <t>`.
    bool   syncQuest   = false;
    float  questSpeed  = 1.0f;
    float  lastSentT   = -1.f;
    bool   lastSentPlay = true;
    double lastSendMs  = 0.0;
    float  questTimeNow = -1.f;         // live device globalUniforms.time (buf+596), polled from the bridge `gtime`
    double lastQueryMs  = 0.0;
    // Fire-and-forget a bridge command to 127.0.0.1:27042 (adb forward). Detached so the UI never stalls.
    void sendQuestCmd(std::string cmd) {
        std::thread([cmd]{
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0); if (s == INVALID_SOCKET) return;
            hsrSockTimeoutMs(s, 250);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(27042); a.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (connect(s, (sockaddr*)&a, sizeof a) == 0) { std::string c = cmd + "\n"; send(s, c.c_str(), (int)c.size(), 0); char rb[64]; recv(s, rb, sizeof rb, 0); }
            closesocket(s);
        }).detach();
    }
    // Poll the device's LIVE clock so the timeline can show "desktop t  vs  Quest t" side-by-side (verify sync).
    // Detached (300ms timeout); parses `gtime` reply "time(buf+596)=<sec>" into questTimeNow.
    void pollQuestTime() {
        std::thread([this]{
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0); if (s == INVALID_SOCKET) return;
            hsrSockTimeoutMs(s, 300);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(27042); a.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
                const char* c = "gtime\n"; send(s, c, 6, 0);
                char rb[512]; int n = recv(s, rb, sizeof rb - 1, 0);
                if (n > 0) { rb[n] = 0; const char* p = strstr(rb, "buf+596)="); if (p) questTimeNow = (float)atof(p + 9); }
            }
            closesocket(s);
        }).detach();
    }
    // ── QUEST PLAYER CONTROL: live-track the headset player + drag it around (with gravity OFF so it
    //    holds height instead of falling+resetting). Drives the AIO bridge: `nogravity`/`warp`/`rot`/`playerpos`.
    bool   trackPlayer   = false;        // poll playerpos + show the player marker/gizmo in the viewport
    bool   playerNoGrav  = false;        // "No gravity" toggle (mirrors the device `nogravity` flag)
    bool   playerDrag    = false;        // dragging the player marker (XZ ground / height / yaw)
    int    playerDragMode= 0;            // 0=XZ ground, 1=height (Y), 2=yaw
    float  pdMx0=0, pdMy0=0, pdY0=0, pdYaw0=0;   // drag anchors (mouse + value at press)
    float  gzPlayerC[2]={0,0}, gzPlayerUp[2]={0,0}, gzPlayerYawTip[2]={0,0}; bool gzPlayerVis=false;   // cached screen handles
    float  questPlayerPos[3]  = {0,0,0}; // device player FEET (world)
    float  questPlayerHead[3] = {0,0,0}; // device head/eye (world)
    float  questPlayerYaw = 0.f;         // commanded facing yaw (radians)
    float  questPlayerFace = 0.f;        // DEVICE head-forward heading (radians) — what the player is actually looking at
    bool   questPlayerFaceOk = false;    // face= was reported (env rendering)
    int    questPlayerDev = -1;          // device nograv state readback (-1 unknown, 0/1)
    // ── PERSISTENT player-control channel (the "gizmo laggy" fix): ONE kept-open socket + a worker thread.
    //    The UI queues warp/rot into pcQ (same-verb spam coalesces); the worker streams them + polls playerpos over the
    //    same socket at ~60 Hz = NO per-drag connect/recv round-trip. Reconnects if the socket drops. ──
    std::thread pcThread; std::atomic<bool> pcRun{false};
    std::mutex  pcMx; std::vector<std::string> pcQ;   // pending commands; consecutive SAME-verb entries coalesce (drag spam)
    void ensureAdbForward() {
#ifdef _WIN32
        system("adb forward tcp:27042 tcp:27042 >nul 2>&1");
#else
        system("adb forward tcp:27042 tcp:27042 >/dev/null 2>&1");
#endif
    }
    // Queue a device command. Consecutive commands with the SAME verb (e.g. a burst of drag "warp"s or "rot"s)
    // coalesce to the latest so a fast drag doesn't back up the socket; DISTINCT verbs (warp then rot, or
    // nogravity then warp) are ALL preserved — that's the "teleport doesn't always work" fix: a Teleport-to-cam
    // that queues warp+rot no longer has the rot clobber the warp before the worker sends it.
    void pcQueue(const std::string& c){
        std::lock_guard<std::mutex> l(pcMx);
        auto verb=[](const std::string& s){ size_t sp=s.find(' '); return sp==std::string::npos? s : s.substr(0,sp); };
        if (!pcQ.empty() && verb(pcQ.back())==verb(c)) pcQ.back()=c; else pcQ.push_back(c);
    }
    static int pcRecvLine(SOCKET s, char* buf, int cap){ int n=0; char ch;
        while (n<cap-1){ int r=recv(s,&ch,1,0); if(r<=0) return n>0?n:-1; if(ch=='\n') break; buf[n++]=ch; } buf[n]=0; return n; }
    void parsePlayerpos(const char* rb){
        // DON'T clobber the feet position while the user is dragging the gizmo — the ~6Hz device readback lags
        // the drag, so overwriting questPlayerPos every poll made the marker snap BACK to the device pos each
        // 160ms = "the gizmo won't follow the cursor as I hold it". The drag is authoritative; on release the
        // poll resumes tracking. (head/face/nograv still update — only the dragged feet pos is held.)
        const char* pf=strstr(rb,"feet="); if (pf && !playerDrag){ float x,y,z; if(sscanf(pf+5,"%f,%f,%f",&x,&y,&z)==3){ questPlayerPos[0]=x; questPlayerPos[1]=y; questPlayerPos[2]=z; } }
        const char* ph=strstr(rb,"head="); if (ph){ float x,y,z; if(sscanf(ph+5,"%f,%f,%f",&x,&y,&z)==3){ questPlayerHead[0]=x; questPlayerHead[1]=y; questPlayerHead[2]=z; } }
        const char* pc=strstr(rb,"face="); if (pc){ float d; if(sscanf(pc+5,"%f",&d)==1){ questPlayerFace=d*0.0174533f; questPlayerFaceOk=true; } }
        const char* pn=strstr(rb,"nograv="); if (pn) questPlayerDev = atoi(pn+7);
    }
    void startPlayerChannel(){
        if (pcRun.exchange(true)) return;
        ensureAdbForward();
        pcThread = std::thread([this]{
            SOCKET s=INVALID_SOCKET; double lastPoll=0; char rb[512];
            while (pcRun.load()){
                if (s==INVALID_SOCKET){ s=socket(AF_INET,SOCK_STREAM,0);
                    if (s!=INVALID_SOCKET){ hsrSockTimeoutMs(s,400);
                        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(27042); a.sin_addr.s_addr=inet_addr("127.0.0.1");
                        if (connect(s,(sockaddr*)&a,sizeof a)!=0){ closesocket(s); s=INVALID_SOCKET; } }
                    if (s==INVALID_SOCKET){ std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; } }
                // 1) flush ALL pending commands in order (distinct verbs preserved; same-verb spam pre-coalesced)
                std::vector<std::string> batch; { std::lock_guard<std::mutex> l(pcMx); batch.swap(pcQ); }
                bool sockDead=false;
                for (auto& cmd : batch){ std::string c=cmd+"\n";
                    if (send(s,c.c_str(),(int)c.size(),0)<=0 || pcRecvLine(s,rb,sizeof rb)<0){ closesocket(s); s=INVALID_SOCKET; sockDead=true; break; } }
                if (sockDead) continue;
                // 2) poll the pose ~6 Hz on the SAME socket
                double now=nowMs(); if (now-lastPoll>160.0){ lastPoll=now;
                    if (send(s,"playerpos\n",10,0)<=0){ closesocket(s); s=INVALID_SOCKET; continue; }
                    if (pcRecvLine(s,rb,sizeof rb)<0){ closesocket(s); s=INVALID_SOCKET; continue; }
                    parsePlayerpos(rb); }
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
            if (s!=INVALID_SOCKET) closesocket(s);
        });
    }
    void stopPlayerChannel(){ if (!pcRun.exchange(false)) return; if (pcThread.joinable()) pcThread.join(); }
    // legacy one-shot poll (used on first show before the channel starts)
    void pollQuestPlayer(){ ensureAdbForward(); startPlayerChannel(); }
    // Send the device player to a world pos (gravity-defying when playerNoGrav) — `warp x y z yaw`.
    void sendPlayerWarp(float x, float y, float z) {
        startPlayerChannel();
        char b[96]; snprintf(b,sizeof b,"warp %.3f %.3f %.3f %.4f", x,y,z, questPlayerYaw);
        pcQueue(b); questPlayerPos[0]=x; questPlayerPos[1]=y; questPlayerPos[2]=z;
    }
    void sendPlayerYaw(float yaw) { startPlayerChannel(); questPlayerYaw=yaw; char b[48]; snprintf(b,sizeof b,"rot %.4f", yaw); pcQueue(b); }
    void sendPlayerNoGravity(bool on) { startPlayerChannel(); playerNoGrav=on; pcQueue(on?"nogravity 1":"nogravity 0"); }
    double nowMs() { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::vector<MeshData>* sceneMeshes = nullptr;                                  // CPU geometry for Cook
    std::function<std::vector<float>(int,int,int&)> vatBaker;                      // V79 VAT bake hook
    std::function<void(int,int,hslcook::ExportMesh&)> hzAnimExtractor;             // V79 HZANIM skeletal hook
    std::function<std::string(int)> animSourceProbe;                               // ANIM-AUDIT: which SOURCE anim channels touch mesh i ("" = none) — per-format (main.cpp), INDEPENDENT of the cook extractors' gates so silent drops show up
    std::function<bool(int,int,int,std::vector<float>&,int&,float&)> flipbookSequencer;   // FLIPBOOK observed cell order: (meshIdx, cols, rows) -> per-frame 2x3 UV mats + N + loopSec (the row-major @global-fps ASSUMPTION was wrong for TV/torches/fires)
    std::vector<uint8_t> bgOgg;                                                    // env background loop -> FMOD asset
    // ── BACKGROUND AUDIO cooker (view / REPLACE / ADD / export / revert) ─────────────────────────────────────
    // bgOgg = what the cook SHIPS (FMOD SND asset, auto-start loop entity at spawn). envOgg keeps the env's
    // ORIGINAL theme so a replacement is revertible. Any ogg/wav/mp3/flac works: FMOD-native containers ship
    // raw, others transcode to WAV (audioconv — the loader's exact rules). Replacing restarts the desktop
    // preview loop too (WYSIWYG). The override file path persists in the session (AUDIOOVR).
    std::vector<uint8_t> envOgg;   // the env's own theme as loaded (empty = env ships no audio)
    std::string audioOvrPath;      // user override source file ("" = env's own audio)
    std::string audioInfo;         // cached UI line: "ogg 44100 Hz 2 ch 63.2s (1.2 MB)"
    void setEnvAudio(const std::vector<uint8_t>& raw){ envOgg=raw; if(audioOvrPath.empty()){ bgOgg=raw; refreshAudioInfo(); } }
    void refreshAudioInfo(){
        audioInfo.clear(); if(bgOgg.empty()) return;
        const char* fmt=audioconv::sniff(bgOgg.data(),bgOgg.size());
        audioconv::Pcm pcm; char b[128];
        if(audioconv::decode(bgOgg.data(),bgOgg.size(),pcm)){
            float secs = pcm.sampleRate>0 ? pcm.frames()/(float)pcm.sampleRate : 0.f;
            snprintf(b,sizeof b,"%s  %d Hz  %d ch  %.1fs  (%.2f MB)",fmt,pcm.sampleRate,pcm.channels,secs,bgOgg.size()/1048576.0);
        } else snprintf(b,sizeof b,"%s (%.2f MB) - decode failed",fmt,bgOgg.size()/1048576.0);
        audioInfo=b;
    }
    void restartAudioPreview(){
        if(!audio) return; if(audio->ok) audio->stop();
        if(bgOgg.empty()) return;
        audioconv::Pcm pcm;
        if(audioconv::decode(bgOgg.data(),bgOgg.size(),pcm)) audio->startPCM(pcm.samples.data(),pcm.frames(),pcm.channels,pcm.sampleRate);
    }
    bool setAudioFromFile(const std::string& path){
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ setStatus("audio FAILED (open): "+path); return false; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        std::vector<uint8_t> raw((size_t)(n>0?n:0)); if(n>0){ size_t rd=fread(raw.data(),1,(size_t)n,f); (void)rd; } fclose(f);
        const char* fmt=audioconv::sniff(raw.data(),raw.size());
        audioconv::Pcm pcm; std::string err;
        if(!audioconv::decode(raw.data(),raw.size(),pcm,&err)){ setStatus("audio decode FAILED ("+std::string(fmt)+"): "+err+" - use OGG / WAV / MP3 / FLAC"); return false; }
        bgOgg = audioconv::fmodNative(fmt) ? std::move(raw) : audioconv::toWav(pcm);   // raw when FMOD reads it natively (compact), else WAV
        audioOvrPath=path; refreshAudioInfo();
        if(audio){ if(audio->ok) audio->stop(); audio->startPCM(pcm.samples.data(),pcm.frames(),pcm.channels,pcm.sampleRate); }
        setStatus("Audio set from "+path+" - previews now, ships in the cook as the background loop");
        return true;
    }
    void clearAudioOverride(){
        if(audioOvrPath.empty()) return;
        audioOvrPath.clear(); bgOgg=envOgg; refreshAudioInfo(); restartAudioPreview();
        setStatus(envOgg.empty()?"Audio override cleared - this env ships no own audio (silent)":"Audio override cleared - env's own theme restored");
    }
    bool exportAudio(){
        if(bgOgg.empty()){ setStatus("no audio to export"); return false; }
        const char* fmt=audioconv::sniff(bgOgg.data(),bgOgg.size()); if(!strcmp(fmt,"?")) fmt="bin";
        namespace fs=std::filesystem; std::error_code ec;
        fs::path dir=fs::path(saveTargetFile()).parent_path(); fs::create_directories(dir,ec);
        std::string out=(dir/(projectBase()+"_audio."+fmt)).string();
        FILE* f=fopen(out.c_str(),"wb"); if(!f){ setStatus("audio export FAILED: "+out); return false; }
        fwrite(bgOgg.data(),1,bgOgg.size(),f); fclose(f);
        setStatus("Exported audio -> "+out); return true;
    }

    // ── UI toolkit ──
    ui::Font font, mono; ui::UIDraw uiDraw; ui::Context cx; ui::DrawList dl;
    int fbW = 0, fbH = 0; float uiScale = 1.f; double lastT = 0.0;
    float dpiScale = 1.f;                 // OS content-scale (DPI), captured once; uiScale = dpiScale * window-fit factor
    float baseRowH=0, baseHeaderH=0, basePad=0, baseIndent=0, baseTimelineH=0;   // UNSCALED theme metrics (re-derived on resize)
    static constexpr float baseFontPx = 15.f;

    // ── selection / outliner ──
    int  selected = -1;            // the ACTIVE object (gizmo origin, properties); -1 = none
    std::vector<int> sel;          // the full selection set (multi-select); `selected` is its active member
    bool showLocal = false;
    bool inSel(int i) const { for (int s : sel) if (s==i) return true; return false; }
    // Selecting a MESH must FULLY drop any scene-ITEM selection (the two are mutually exclusive: the gizmo/inspector
    // test selItem>=0 first). Clearing only selItems while leaving the ACTIVE selItem set left a stale item bound, so
    // a newly-clicked mesh "wouldn't sync" (gizmo/props stuck on the old item). Clear selItem too.
    void selectOne(int i){ sel.clear(); if (i>=0) sel.push_back(i); selected=i; r->selectedMesh=i; selItems.clear(); selItem=-1; }
    void toggleSel(int i){ if (i<0) return; selItems.clear(); selItem=-1; for (size_t k=0;k<sel.size();++k) if (sel[k]==i){ sel.erase(sel.begin()+k); selected = sel.empty()?-1:sel.back(); r->selectedMesh=selected; return; } sel.push_back(i); selected=i; r->selectedMesh=i; }
    void deselectAll(){ sel.clear(); selected=-1; r->selectedMesh=-1; }

    // ── mesh DELETE (toggle) — drop the selected mesh(es) from the render AND the cook (non-destructive: the mesh
    //    stays in gpuMeshes so indices/undo stay valid; persisted as a DELETED line in the .hsledit). Del again restores.
    void toggleDeleteSelected(){
        if (sel.empty() || !r) return;
        // Del only ever DELETES live meshes — already-deleted ones in the selection (stale sel, old
        // sessions) are ignored, never flipped back alive. Restoring is Ctrl+Z's job exclusively;
        // "Del again restores" resurrected ghosts whenever a selection tool grabbed a deleted mesh.
        std::vector<int> live; for (int m:sel) if (!r->isDeleted((size_t)m)) live.push_back(m);
        if (live.empty()) { deselectAll(); return; }
        for (int m:live) r->setDeleted((size_t)m, true);
        pushDeleteUndo(live, true);     // the edit HISTORY: Ctrl+Z is the ONLY way back
        // Deleting a CREATED mesh (cut/fuse/patch result, idx >= baseMeshCount) must re-write the .geom
        // sidecar: the autosave gates the sidecar on geomDirty, and a STALE sidecar still holds the mesh
        // ALIVE while its session DELETED line is ignored on load (geomAuth) -> "deleted cut mesh shows
        // back up on reload" (bluehillgoldmine zombie).
        if (baseMeshCount>=0) for (int m:live) if (m>=baseMeshCount) { geomDirty=true; break; }
        bakeNavmeshes(items);           // colliders/navmeshes sourcing these meshes drop them immediately
        size_t n = live.size();
        deselectAll();                  // deleted = GONE (no list row, no pick, no gizmo)
        setStatus("Deleted "+std::to_string(n)+" mesh(es) - gone from render + list + cook (only Ctrl+Z restores)");
    }
    // ── ANIMATED-mesh EYE (Ctrl+H / View checkbox): one click hides EVERYTHING that animates —
    //    skinned rigs, node anims, UV-scroll/flipbook cards, tint-flicker particles, VAT creatures
    //    (every mat.sanim path sets dynamicVerts, so the coverage is total) — so static geometry can
    //    be edited without the moving stuff in the way. Remembers exactly which meshes IT hid and
    //    restores only those on toggle-off (manual hides untouched).
    std::vector<int> animEyeHidden; bool animEyeOn=false;
    bool meshIsAnimated(int i) const {
        if (!r || i<0 || i>=(int)r->gpuMeshes.size()) return false;
        const VkGpuMesh& gm=r->gpuMeshes[(size_t)i];
        if (gm.isSkinned || gm.dynamicVerts) return true;
        if (sceneMeshes && i<(int)sceneMeshes->size()){
            const MeshData& md=(*sceneMeshes)[(size_t)i];
            if (md.hasVat || md.dynamicVerts || md.hasBones || md.animatedTintAlpha) return true; }
        return false;
    }
    void toggleAnimatedEye(){
        if (!r) return;
        if (!animEyeOn){
            animEyeHidden.clear();
            for (int i=0;i<(int)r->gpuMeshes.size();++i)
                if (meshIsAnimated(i) && !r->isDeleted((size_t)i) && !r->isHidden((size_t)i)){ r->setHidden((size_t)i,true); animEyeHidden.push_back(i); }
            animEyeOn=true;
            setStatus("Hid "+std::to_string(animEyeHidden.size())+" ANIMATED mesh(es) - skinned/flipbooks/UV-scroll/particles/VAT (Ctrl+H shows them again)");
        } else {
            for (int i : animEyeHidden) if (i>=0 && i<(int)r->gpuMeshes.size()) r->setHidden((size_t)i,false);
            setStatus("Restored "+std::to_string(animEyeHidden.size())+" animated mesh(es)");
            animEyeHidden.clear(); animEyeOn=false;
        }
    }
    // ── mesh DUPLICATE (Ctrl+D) — clone the selected mesh(es) into a fresh GPU mesh (its own buffers) offset in place,
    //    so the copy can be moved/edited/cooked independently WITHOUT re-authoring geometry. Appends to sceneMeshes +
    //    gpuMeshes; buildExportMeshes ships every mesh, so the clone cooks too.
    void duplicateSelected(){
        if (sel.empty() || !r || !sceneMeshes) return;
        std::vector<int> src=sel; deselectAll(); std::vector<int> made;
        for (int s : src) {
            if (s<0 || s>=(int)sceneMeshes->size() || s>=(int)r->gpuMeshes.size()) continue;
            MeshData cp = (*sceneMeshes)[(size_t)s];          // clone CPU mesh (geometry + textures + material flags) — SAME name (hidden edit log tracks it)
            sceneMeshes->push_back(cp);
            size_t ni = r->gpuMeshes.size();
            meshEditLog[(int)ni] = "copy";
            r->uploadMesh(sceneMeshes->back());               // creates gpuMeshes[ni] with its own GPU buffers
            if (r->gpuMeshes.size() != ni+1) continue;        // upload failed -> skip
            VkGpuMesh& ng = r->gpuMeshes[ni];
            const VkGpuMesh& og = r->gpuMeshes[(size_t)s];    // (both refs taken AFTER the push -> valid)
            memcpy(ng.editT, og.editT, sizeof ng.editT); memcpy(ng.editR, og.editR, sizeof ng.editR); memcpy(ng.editS, og.editS, sizeof ng.editS);
            // NO offset: the copy lands EXACTLY on the original (move it yourself) — the shift annoyed more than it helped
            recomputeModel(ng);
            made.push_back((int)ni);
        }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false);   // keep the hide/delete bitsets sized to the grown list
        r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        if (!made.empty()) pushDeleteUndo(made, false);       // duplicates are part of the HISTORY: Ctrl+Z removes them
        geomDirty = true;
        sel=made; selected=made.empty()?-1:made.back(); r->selectedMesh=selected; selItem=-1;
        scrollToSel = true;                                   // outliner jumps to the new clones (they append at the END)
        setStatus("Duplicated "+std::to_string(made.size())+" mesh(es) - move/edit/cook independently (Ctrl+Z removes)");
    }

    // ── mesh EXTEND (geometry "stretch"): grow NEW triangles off the mesh's OPEN boundary edges to fill
    //    holes/blanks. New verts copy their source vert's UV, so the texture CONTINUES from the border
    //    (edge texels stretch across the new strip - no re-unwrap needed). Non-destructive: the extended
    //    mesh is appended as a fresh mesh (+ cooks), the original is soft-DELETED (Ctrl+Z restores it).
    //    dir: 0 = down (-Y, floor gaps), 1 = outward (away from centroid, horizontal), 2 = up (+Y).
    void extendMeshBoundary(int mi, float dist, int dir){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];   // clone; we append the modified copy
        size_t nv = md.positions.size()/3;
        if (nv < 3 || md.indices.size() < 3) { setStatus("extend: mesh has no editable geometry"); return; }
        // boundary edges = used by exactly ONE triangle
        std::map<std::pair<uint32_t,uint32_t>,int> ec;
        auto key=[&](uint32_t a, uint32_t b){ return a<b?std::make_pair(a,b):std::make_pair(b,a); };
        for (size_t k=0;k+2<md.indices.size();k+=3){ uint32_t a=md.indices[k],b=md.indices[k+1],c=md.indices[k+2];
            ec[key(a,b)]++; ec[key(b,c)]++; ec[key(c,a)]++; }
        float cx0=0,cy0=0,cz0=0; for(size_t v2=0;v2<nv;++v2){ cx0+=md.positions[v2*3]; cy0+=md.positions[v2*3+1]; cz0+=md.positions[v2*3+2]; }
        cx0/=nv; cy0/=nv; cz0/=nv;
        std::map<uint32_t,uint32_t> cloneOf;   // src vert -> its extruded twin
        auto extrude=[&](uint32_t v2)->uint32_t{
            auto it=cloneOf.find(v2); if (it!=cloneOf.end()) return it->second;
            float px=md.positions[v2*3], py=md.positions[v2*3+1], pz=md.positions[v2*3+2];
            float ox=0, oy=0, oz=0;
            if (dir==0) oy=-dist; else if (dir==2) oy=dist;
            else { float dx=px-cx0, dz=pz-cz0, l=std::sqrt(dx*dx+dz*dz); if (l<1e-4f){dx=1;dz=0;l=1;} ox=dx/l*dist; oz=dz/l*dist; }
            uint32_t nvi=(uint32_t)(md.positions.size()/3);
            md.positions.push_back(px+ox); md.positions.push_back(py+oy); md.positions.push_back(pz+oz);
            if (md.uvs.size() >= nv*2)     { md.uvs.push_back(md.uvs[v2*2]); md.uvs.push_back(md.uvs[v2*2+1]); }           // CONTINUE the border UV
            if (md.uvs2.size() >= nv*2)    { md.uvs2.push_back(md.uvs2[v2*2]); md.uvs2.push_back(md.uvs2[v2*2+1]); }
            cloneOf[v2]=nvi; return nvi;
        };
        int added=0;
        for (size_t k=0;k+2<md.indices.size() && added<200000;k+=3){ uint32_t tri[3]={md.indices[k],md.indices[k+1],md.indices[k+2]};
            for (int e=0;e<3;e++){ uint32_t a=tri[e], b=tri[(e+1)%3];
                if (ec[key(a,b)]!=1) continue;                       // interior edge
                uint32_t a2=extrude(a), b2=extrude(b);
                md.indices.push_back(a); md.indices.push_back(b);  md.indices.push_back(b2);
                md.indices.push_back(a); md.indices.push_back(b2); md.indices.push_back(a2);
                added+=2; } }
        if (!added) { setStatus("extend: mesh is CLOSED (no boundary edges) - nothing to grow"); return; }
        if (commitGeomEdit(mi, std::move(md), "extend"))
            setStatus("Extended "+std::to_string(added)+" tris off the boundary (texture continued) - Ctrl+Z reverts");
    }

    // ── mesh SEAL HOLES: find closed BOUNDARY LOOPS (holes in the triangulation - "wireframe looks fuller
    //    than the surface") and FAN-FILL each with real triangles around the loop's centroid. UVs/uv2 are
    //    averaged from the rim, so the texture blends across the patch. Same non-destructive append+history
    //    flow as extendMeshBoundary. Run on every selected mesh via the context menu.
    // Collect every CLOSED boundary loop (edges used by exactly one triangle, chained into rings).
    // Shared by the seal tools AND the visual HOLE INSPECTOR overlay.
    // WELDS VERTICES BY POSITION first: baked env meshes (and fuse results) duplicate vertices along
    // UV/normal seams, so without welding nearly EVERY seam edge looked like a boundary — 58 phantom
    // "holes" on a floor while the REAL rim (crossing seam-split verts) never chained into one loop.
    static void collectBoundaryLoops(const MeshData& md, std::vector<std::vector<uint32_t>>& loops){
        loops.clear();
        if (md.indices.size() < 3) return;
        size_t nv = md.positions.size()/3;
        std::vector<uint32_t> weld(nv);
        { std::map<std::tuple<long long,long long,long long>, uint32_t> grid;   // 0.5mm quantized position -> first vert there
          for (size_t v2=0; v2<nv; ++v2) {
              auto k = std::make_tuple((long long)std::llround(md.positions[v2*3]  *2000.0),
                                       (long long)std::llround(md.positions[v2*3+1]*2000.0),
                                       (long long)std::llround(md.positions[v2*3+2]*2000.0));
              auto it = grid.find(k);
              if (it == grid.end()) { grid.emplace(k, (uint32_t)v2); weld[v2] = (uint32_t)v2; }
              else weld[v2] = it->second;
          } }
        std::map<std::pair<uint32_t,uint32_t>,int> ec;
        auto key=[&](uint32_t a,uint32_t b){ return a<b?std::make_pair(a,b):std::make_pair(b,a); };
        for (size_t k=0;k+2<md.indices.size();k+=3){
            uint32_t a=weld[md.indices[k]], b=weld[md.indices[k+1]], c=weld[md.indices[k+2]];
            if (a==b || b==c || c==a) continue;              // degenerate after weld
            ec[key(a,b)]++; ec[key(b,c)]++; ec[key(c,a)]++; }
        std::multimap<uint32_t,uint32_t> adj;   // undirected boundary adjacency
        for (auto& kv : ec) if (kv.second==1) { adj.insert({kv.first.first,kv.first.second}); adj.insert({kv.first.second,kv.first.first}); }
        std::set<std::pair<uint32_t,uint32_t>> used;
        std::vector<uint32_t> loop;
        for (auto& st : adj) {
            if (used.count(key(st.first,st.second))) continue;
            loop.clear(); uint32_t start=st.first, cur=st.second, prev=start; loop.push_back(start);
            used.insert(key(start,cur));
            bool closed=false;
            for (int guard=0; guard<100000; ++guard) {
                loop.push_back(cur);
                if (cur==start) { closed=true; break; }
                uint32_t next=UINT32_MAX;
                auto rng=adj.equal_range(cur);
                for (auto it2=rng.first; it2!=rng.second; ++it2)
                    if (it2->second!=prev && !used.count(key(cur,it2->second))) { next=it2->second; break; }
                if (next==UINT32_MAX) break;
                used.insert(key(cur,next)); prev=cur; cur=next;
            }
            if (!closed || loop.size()<4 || loop.size()>20000) continue;   // loop includes start twice at the end
            loop.pop_back();                                               // drop the repeated start
            loops.push_back(loop);
        }
    }
    // Seal holes. targetLoop >= 0 = fill EXACTLY that boundary loop (the hole inspector's click-to-fill —
    // no guessing); nearWorldPt = fill the loop nearest that world point ("Seal hole HERE" right-click);
    // neither = fill all EXCEPT the longest loop (an open sheet's outer perimeter is a loop too).
    void sealMeshHoles(int mi, const float* nearWorldPt=nullptr, int targetLoop=-1){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        size_t nv = md.positions.size()/3;
        if (nv < 3 || md.indices.size() < 3) return;
        std::vector<std::vector<uint32_t>> loops;
        collectBoundaryLoops(md, loops);
        if (loops.empty()) { setStatus("seal: no closed boundary loops on '"+md.name+"'"); return; }
        // ── PASS 2: choose WHICH loops are actual holes ──
        // nearWorldPt (right-click "Seal hole HERE"): ONLY the loop nearest the clicked point.
        // Otherwise: fill everything EXCEPT the longest loop — an open sheet's OUTER PERIMETER is also
        // a closed boundary loop, and capping it was the "falsely generates" bug. (A watertight-minus-
        // holes mesh has no perimeter; then the longest loop is simply the biggest hole and stays open —
        // use "Seal hole HERE" to target it explicitly.)
        const float* M = r->gpuMeshes[(size_t)mi].model;
        std::vector<char> fill(loops.size(), 1);
        if (targetLoop >= 0 && targetLoop < (int)loops.size()) {   // hole-inspector click: EXACTLY this loop
            std::fill(fill.begin(), fill.end(), 0); fill[targetLoop] = 1;
        } else if (nearWorldPt) {
            std::fill(fill.begin(), fill.end(), 0);
            int bestL=-1; float bestD=1e30f;
            for (size_t li=0; li<loops.size(); ++li) {
                float cx0=0,cy0=0,cz0=0; for (uint32_t v2 : loops[li]) { const float* p=&md.positions[v2*3];
                    cx0+=M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]; cy0+=M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]; cz0+=M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]; }
                float n2=(float)loops[li].size(); cx0/=n2; cy0/=n2; cz0/=n2;
                float dx=cx0-nearWorldPt[0], dy=cy0-nearWorldPt[1], dz=cz0-nearWorldPt[2], d=dx*dx+dy*dy+dz*dz;
                if (d<bestD){ bestD=d; bestL=(int)li; }
            }
            if (bestL>=0) fill[bestL]=1;
        } else if (loops.size()>1) {
            size_t longest=0; for (size_t li=1; li<loops.size(); ++li) if (loops[li].size()>loops[longest].size()) longest=li;
            fill[longest]=0;   // the outer perimeter stays open
        }
        // ── PASS 3: fan-fill the chosen loops (rim UVs averaged into the center vert) ──
        int holes=0, added=0;
        bool hasUV=md.uvs.size()>=nv*2, hasUV2=md.uvs2.size()>=nv*2;
        for (size_t li=0; li<loops.size(); ++li) {
            if (!fill[li]) continue;
            const auto& L = loops[li]; size_t n = L.size();
            float px=0,py=0,pz=0,u0=0,v0=0,u1=0,v1=0;
            for (uint32_t v2 : L){ px+=md.positions[v2*3]; py+=md.positions[v2*3+1]; pz+=md.positions[v2*3+2];
                if (hasUV){ u0+=md.uvs[v2*2]; v0+=md.uvs[v2*2+1]; } if (hasUV2){ u1+=md.uvs2[v2*2]; v1+=md.uvs2[v2*2+1]; } }
            uint32_t cvi=(uint32_t)(md.positions.size()/3);
            md.positions.push_back(px/n); md.positions.push_back(py/n); md.positions.push_back(pz/n);
            if (hasUV) { md.uvs.push_back(u0/n);  md.uvs.push_back(v0/n); }
            if (hasUV2){ md.uvs2.push_back(u1/n); md.uvs2.push_back(v1/n); }
            for (size_t i2=0;i2<n;++i2){ uint32_t a=L[i2], b=L[(i2+1)%n];
                md.indices.push_back(a); md.indices.push_back(b); md.indices.push_back(cvi); ++added; }
            ++holes;
        }
        if (!holes) { setStatus("seal: only the outer perimeter found - right-click NEXT TO the hole and use 'Seal hole HERE'"); return; }
        if (commitGeomEdit(mi, std::move(md), nearWorldPt?"sealHere":"seal"))
            setStatus("Sealed "+std::to_string(holes)+" hole(s) with "+std::to_string(added)+" tris"
                      +(nearWorldPt?"" : " (outer perimeter left open)")+" - Ctrl+Z reverts");
    }

    // ── HOLE INSPECTOR: detect the selected mesh's boundary loops and show each as a NUMBERED orange
    //    outline in the viewport; click a marker (or a panel Fill button) to seal EXACTLY that hole.
    //    No guessing. Cache invalidated on selection change / any geometry edit.
    bool showHoles = false;
    int  holesMesh = -1;                              // which mesh the cache belongs to (-1 = stale)
    std::vector<std::vector<uint32_t>> holeLoops;     // boundary loops of holesMesh
    std::vector<std::array<float,3>> holeCenterL;     // cached MODEL-space loop centroids (world = M * this, cheap per frame)
    void refreshHoles(){
        holesMesh = -1; holeLoops.clear(); holeCenterL.clear();
        if (!r || !sceneMeshes || selected<0 || selected>=(int)sceneMeshes->size()) return;
        const MeshData& md=(*sceneMeshes)[(size_t)selected];
        collectBoundaryLoops(md, holeLoops);
        holeCenterL.resize(holeLoops.size());
        for (size_t li=0; li<holeLoops.size(); ++li){ float cx0=0,cy0=0,cz0=0;
            for (uint32_t v2 : holeLoops[li]){ cx0+=md.positions[v2*3]; cy0+=md.positions[v2*3+1]; cz0+=md.positions[v2*3+2]; }
            float n2=(float)holeLoops[li].size(); holeCenterL[li]={cx0/n2, cy0/n2, cz0/n2}; }
        holesMesh = selected;
    }
    void drawHoleOverlay(){
        if (!showHoles || selected<0 || !r || selected>=(int)r->gpuMeshes.size()) return;
        if (holesMesh != selected) refreshHoles();
        if (holeLoops.empty()) return;
        auto& th=cx.th;
        const MeshData& md=(*sceneMeshes)[(size_t)selected]; const float* M=r->gpuMeshes[(size_t)selected].model;
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        size_t maxLoops = std::min(holeLoops.size(), (size_t)128);   // sanity cap (post-weld counts are small anyway)
        for (size_t li=0; li<maxLoops; ++li) {
            const auto& L = holeLoops[li];
            size_t step = L.size()>160 ? L.size()/160 : 1;           // DECIMATED rim (projecting 20k rim verts per frame was the lag)
            float prevS[2]; bool prevOk=false;
            for (size_t i2=0;i2<=L.size();i2+=step){
                uint32_t v2 = L[i2 % L.size()]; const float* p=&md.positions[v2*3];
                float wp[3]={ M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12], M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13], M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14] };
                float sx,sy; bool ok=worldToScreen(wp,sx,sy);
                if (ok && prevOk) dl.line(prevS[0],prevS[1],sx,sy, ui::rgba(255,160,40), 2.5f);   // orange rim
                prevS[0]=sx; prevS[1]=sy; prevOk=ok;
            }
            const auto& cl = holeCenterL[li];
            float cw[3]={ M[0]*cl[0]+M[4]*cl[1]+M[8]*cl[2]+M[12], M[1]*cl[0]+M[5]*cl[1]+M[9]*cl[2]+M[13], M[2]*cl[0]+M[6]*cl[1]+M[10]*cl[2]+M[14] };
            float ms[2];
            if (worldToScreen(cw, ms[0], ms[1])) {
                bool hv = std::fabs(cx.in.mx-ms[0])<14*uiScale && std::fabs(cx.in.my-ms[1])<14*uiScale;
                float hs=9*uiScale;
                dl.rect(ms[0]-hs, ms[1]-hs, hs*2, hs*2, hv?ui::rgba(255,235,80,230):ui::rgba(255,160,40,200));
                dl.border(ms[0]-hs, ms[1]-hs, hs*2, hs*2, ui::rgba(20,20,20), 1.5f);
                char nb[16]; snprintf(nb,sizeof nb,"%d",(int)li+1);
                cx.textAligned(ms[0]-hs, ms[1]-hs-16*uiScale, hs*2+20*uiScale, 14*uiScale, nb, ui::rgba(255,200,90), 0);
                if (hv && cx.in.pressed[0] && !ctxOpen && !addMenuOpen) {   // CLICK the marker = fill THIS hole
                    sealMeshHoles(selected, nullptr, (int)li);
                    dl.popClip(); return;                                   // geometry changed - stop this frame
                }
            }
        }
        dl.popClip();
    }

    // ── PATCH TOOL ("paint where to seal"): YOU click the corner points around a gap in the viewport
    //    (pins land on the geometry you click), Enter builds a clean fan patch through those points as
    //    its OWN mesh - selected mesh's texture, flat planar UVs (no radial smearing), no dependence on
    //    the broken triangulation at all. Undoable (Ctrl+Z removes), persists via the .geom sidecar.
    bool patchMode = false;
    bool pinIsCut  = false;                       // pins define a CUT REGION instead of a patch (same click-the-corners UX)
    bool pinIsKnife= false;                       // pins define a KNIFE PATH: split ALONG the clicked polyline (nothing deleted)
    std::vector<std::array<float,3>> patchPts;
    std::vector<std::array<u8,4>>    patchCols;   // color sampled from the CLICKED surface at each pin (auto-matching texture)
    // ── CUT REGION by pins: drop every triangle of the SELECTION whose centroid falls inside the
    //    clicked polygon (projected on the pins' plane, +-3m). Texture/UVs untouched - "cut with the
    //    same texture and all". Non-destructive per mesh (Ctrl+Z reverts each).
    void cutRegionByPins(){
        // 2 PINS = SPLIT LINE: draw a line across the mesh -> split the selection in two along the
        // vertical plane through it (texture untouched). 3+ pins = polygon cut-out.
        if (patchPts.size()==2){
            float d[3]={patchPts[1][0]-patchPts[0][0], patchPts[1][1]-patchPts[0][1], patchPts[1][2]-patchPts[0][2]};
            float dl2=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
            if (dl2<1e-4f){ setStatus("split line: the two points are on top of each other"); return; }
            d[0]/=dl2; d[1]/=dl2; d[2]/=dl2;
            int up = std::fabs(d[0])<std::fabs(d[1]) ? (std::fabs(d[0])<std::fabs(d[2])?0:2) : (std::fabs(d[1])<std::fabs(d[2])?1:2);
            float u[3]={0,0,0}; u[up]=1.f;                                     // world axis most perpendicular to the line
            float pn[3]={ d[1]*u[2]-d[2]*u[1], d[2]*u[0]-d[0]*u[2], d[0]*u[1]-d[1]*u[0] };
            float nl=std::sqrt(pn[0]*pn[0]+pn[1]*pn[1]+pn[2]*pn[2]); if (nl<1e-6f){ setStatus("split line: degenerate plane"); return; }
            pn[0]/=nl; pn[1]/=nl; pn[2]/=nl;
            float pd = pn[0]*patchPts[0][0]+pn[1]*patchPts[0][1]+pn[2]*patchPts[0][2];
            forEachSelMesh([&](int m){ sliceMeshByPlane(m, pn, pd); });
            patchPts.clear(); patchCols.clear(); patchMode=false; pinIsCut=false;
            return;
        }
        if (patchPts.size()<3){ setStatus("cut: 2 pins = SPLIT along the line, 3+ pins = cut the polygon out"); return; }
        size_t n=patchPts.size();
        float nx=0,ny=0,nz=0;
        for (size_t i2=0;i2<n;++i2){ auto& a=patchPts[i2]; auto& b=patchPts[(i2+1)%n];
            nx+=(a[1]-b[1])*(a[2]+b[2]); ny+=(a[2]-b[2])*(a[0]+b[0]); nz+=(a[0]-b[0])*(a[1]+b[1]); }
        int dom = std::fabs(nx)>std::fabs(ny) ? (std::fabs(nx)>std::fabs(nz)?0:2) : (std::fabs(ny)>std::fabs(nz)?1:2);
        int ua=(dom+1)%3, va=(dom+2)%3;
        // ── 3D depth window: distance measured along the pins' TRUE best-fit plane normal (a slanted
        //    click on a dune/wall gets a slanted cut volume, not a world-axis slab), and the depth is
        //    the Cut depth setting (0 = auto: 3m + however much the pins themselves spread off-plane).
        float nrm[3]={nx,ny,nz}; float nl2=std::sqrt(nx*nx+ny*ny+nz*nz);
        if (nl2>1e-9f){ nrm[0]/=nl2; nrm[1]/=nl2; nrm[2]/=nl2; } else { nrm[0]=nrm[1]=nrm[2]=0; nrm[dom]=1.f; }
        float planeD=0; for (auto& p : patchPts) planeD += nrm[0]*p[0]+nrm[1]*p[1]+nrm[2]*p[2]; planeD/=(float)n;
        float spread=0; for (auto& p : patchPts) spread=std::max(spread, std::fabs(nrm[0]*p[0]+nrm[1]*p[1]+nrm[2]*p[2]-planeD));
        float depthWin = knifeCutDepth>0.001f ? knifeCutDepth : (3.f+spread);
        std::vector<std::array<float,2>> poly(n);
        for (size_t i2=0;i2<n;++i2) poly[i2]={patchPts[i2][ua], patchPts[i2][va]};
        auto inPoly=[&](float u0,float v0)->bool{ bool in=false;
            for (size_t i2=0,j=n-1;i2<n;j=i2++){
                if (((poly[i2][1]>v0)!=(poly[j][1]>v0)) &&
                    (u0 < (poly[j][0]-poly[i2][0])*(v0-poly[i2][1])/(poly[j][1]-poly[i2][1])+poly[i2][0])) in=!in; }
            return in; };
        int totalCut=0;
        forEachSelMesh([&](int mi){
            if (mi<0||mi>=(int)sceneMeshes->size()||mi>=(int)r->gpuMeshes.size()) return;
            MeshData md=(*sceneMeshes)[(size_t)mi];
            const float* M=r->gpuMeshes[(size_t)mi].model;
            std::vector<u32> keep; keep.reserve(md.indices.size());
            int cut=0;
            for (size_t k=0;k+2<md.indices.size();k+=3){
                float c[3]={0,0,0};
                for (int e=0;e<3;e++){ const float* p=&md.positions[md.indices[k+e]*3];
                    c[0]+=M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]; c[1]+=M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]; c[2]+=M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]; }
                c[0]/=3; c[1]/=3; c[2]/=3;
                if (std::fabs(nrm[0]*c[0]+nrm[1]*c[1]+nrm[2]*c[2]-planeD)<=depthWin && inPoly(c[ua],c[va])) { ++cut; continue; }
                keep.push_back(md.indices[k]); keep.push_back(md.indices[k+1]); keep.push_back(md.indices[k+2]);
            }
            if (!cut) return;
            md.indices=std::move(keep); totalCut+=cut;
            commitGeomEdit(mi, std::move(md), "cutRegion");
        });
        patchPts.clear(); patchCols.clear(); patchMode=false; pinIsCut=false;
        setStatus(totalCut ? ("Cut "+std::to_string(totalCut)+" tris inside the clicked region, depth "+std::to_string(depthWin).substr(0,4)+"m (texture untouched) - Ctrl+Z reverts")
                           : "cut region: nothing inside the polygon on the selected mesh(es) - raise Cut depth if the target sits deeper");
    }
    // Raycast a screen point against ONE mesh and return hit point + the SURFACE COLOR there
    // (barycentric-interpolated UV -> bilinear sample of that mesh's texture).
    bool screenRayHitColor(double mx, double my, int meshIdx, float outP[3], u8 outCol[4]){
        outCol[0]=outCol[1]=outCol[2]=180; outCol[3]=255;
        if(!r||!sceneMeshes||meshIdx<0||meshIdx>=(int)r->gpuMeshes.size()) return false;
        float W=(float)rcViewport.extent.width,H=(float)rcViewport.extent.height; if(W<=0||H<=0)return false;
        float vp[16]; mat4mul(r->cam.proj,r->cam.view,vp); float inv[16]; if(!invertMat4(vp,inv))return false;
        float ndcx=2.f*((float)mx-rcViewport.offset.x)/W-1.f, ndcy=2.f*((float)my-rcViewport.offset.y)/H-1.f;
        float O[3],Fp[3]; unproject(inv,ndcx,ndcy,1.f,O); unproject(inv,ndcx,ndcy,0.f,Fp);
        float D[3]={Fp[0]-O[0],Fp[1]-O[1],Fp[2]-O[2]}; float dl_=std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]); if(dl_<1e-6f)return false; D[0]/=dl_;D[1]/=dl_;D[2]/=dl_;
        auto& gm=r->gpuMeshes[meshIdx]; const auto& P=gm.pickPos; const auto& I=gm.pickIdx; if(P.size()<9||I.size()<3)return false;
        const MeshData& md=(*sceneMeshes)[(size_t)meshIdx];
        float bestT=1e30f; bool hit=false; uint32_t hA=0,hB=0,hC=0; float hP[3]={0,0,0};
        for(size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
            if((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size())continue;
            float w0[3],w1[3],w2[3]; xformPoint(gm.model,&P[a*3],w0); xformPoint(gm.model,&P[b*3],w1); xformPoint(gm.model,&P[c*3],w2);
            float t; if(rayTri(O,D,w0,w1,w2,t)&&t<bestT){ bestT=t; hit=true; hA=a; hB=b; hC=c;
                hP[0]=O[0]+D[0]*t; hP[1]=O[1]+D[1]*t; hP[2]=O[2]+D[2]*t; } }
        if(!hit) return false;
        outP[0]=hP[0]; outP[1]=hP[1]; outP[2]=hP[2];
        // barycentric of the hit inside its triangle (world space) -> interpolated UV -> texture sample
        size_t nvm = md.positions.size()/3;
        if (!md.texRGBA.empty() && md.uvs.size()>=nvm*2 && hA<nvm && hB<nvm && hC<nvm) {
            float w0[3],w1[3],w2[3];
            xformPoint(gm.model,&P[hA*3],w0); xformPoint(gm.model,&P[hB*3],w1); xformPoint(gm.model,&P[hC*3],w2);
            float v0[3]={w1[0]-w0[0],w1[1]-w0[1],w1[2]-w0[2]}, v1[3]={w2[0]-w0[0],w2[1]-w0[1],w2[2]-w0[2]}, v2[3]={hP[0]-w0[0],hP[1]-w0[1],hP[2]-w0[2]};
            float d00=v0[0]*v0[0]+v0[1]*v0[1]+v0[2]*v0[2], d01=v0[0]*v1[0]+v0[1]*v1[1]+v0[2]*v1[2];
            float d11=v1[0]*v1[0]+v1[1]*v1[1]+v1[2]*v1[2], d20=v2[0]*v0[0]+v2[1]*v0[1]+v2[2]*v0[2], d21=v2[0]*v1[0]+v2[1]*v1[1]+v2[2]*v1[2];
            float den=d00*d11-d01*d01;
            if (std::fabs(den)>1e-12f){
                float bv=(d11*d20-d01*d21)/den, bw=(d00*d21-d01*d20)/den, bu=1.f-bv-bw;
                float uu=bu*md.uvs[hA*2]+bv*md.uvs[hB*2]+bw*md.uvs[hC*2];
                float vv=bu*md.uvs[hA*2+1]+bv*md.uvs[hB*2+1]+bw*md.uvs[hC*2+1];
                uu-=std::floor(uu); vv-=std::floor(vv);
                int W2=(int)md.texW,H2=(int)md.texH;
                float fx=uu*W2-0.5f, fy=vv*H2-0.5f; int x0=(int)std::floor(fx), y0=(int)std::floor(fy); float tx=fx-x0, ty=fy-y0;
                auto S=[&](int px,int py,int ch)->float{ px=std::clamp(px,0,W2-1); py=std::clamp(py,0,H2-1); return (float)md.texRGBA[((size_t)py*W2+px)*4+ch]; };
                for (int ch=0;ch<4;++ch){ float a2=S(x0,y0,ch)*(1-tx)+S(x0+1,y0,ch)*tx, b2=S(x0,y0+1,ch)*(1-tx)+S(x0+1,y0+1,ch)*tx;
                    outCol[ch]=(u8)std::clamp(a2*(1-ty)+b2*ty,0.f,255.f); }
            }
        }
        return true;
    }
    void drawPatchOverlay(){
        if (!patchMode) return;
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        cx.textAligned((float)rcViewport.offset.x, (float)rcViewport.offset.y+26*uiScale, (float)rcViewport.extent.width, 18*uiScale,
                       pinIsKnife ? "KNIFE PATH - click points ALONG the cut line (corners allowed), ENTER cuts, ESC cancels"
                                  : pinIsBend ? "BEND/STRETCH - click the grab point, set radius+offset in Geometry, ENTER applies"
                                 : pinIsCut ? "CUT REGION - 2 pins = SPLIT along the line, 3+ = cut the polygon out; ENTER applies"
                                : "PATCH TOOL - click the gap's corners in order (3+), ENTER builds, ESC cancels", ui::rgba(120,230,140), 1);
        if (!sel.empty())
            cx.textAligned((float)rcViewport.offset.x, (float)rcViewport.offset.y+44*uiScale, (float)rcViewport.extent.width, 16*uiScale,
                           "pins locked to the SELECTED mesh(es) - clicks on other meshes are ignored", ui::rgba(160,255,180,200), 1);
        float prev[2]; bool prevOk=false;
        for (size_t i2=0;i2<patchPts.size();++i2){
            float w[3]={patchPts[i2][0],patchPts[i2][1],patchPts[i2][2]}; float sx,sy;
            if (worldToScreen(w,sx,sy)){
                dl.rect(sx-5*uiScale,sy-5*uiScale,10*uiScale,10*uiScale, ui::rgba(120,230,140));
                char nb[8]; snprintf(nb,sizeof nb,"%d",(int)i2+1);
                cx.textAligned(sx+7*uiScale,sy-8*uiScale,20*uiScale,14*uiScale,nb,ui::rgba(160,255,180),0);
                if (prevOk) dl.line(prev[0],prev[1],sx,sy, ui::rgba(120,230,140), 2.f);
                prev[0]=sx; prev[1]=sy; prevOk=true;
            }
        }
        if (!pinIsKnife && patchPts.size()>2){ float w0[3]={patchPts[0][0],patchPts[0][1],patchPts[0][2]}; float sx,sy;   // knife path = OPEN line, no closing edge
            if (prevOk && worldToScreen(w0,sx,sy)) dl.line(prev[0],prev[1],sx,sy, ui::rgba(120,230,140,140), 1.5f); }
        dl.popClip();
    }
    // A patch click: raycast the cursor, drop a pin at the hit AND remember the SURFACE COLOR under
    // the click (the patch texture is auto-generated from these samples). With a SELECTION the ray
    // tests ONLY the selected mesh(es) — nearest hit among them wins — so pins land on the TARGETED
    // mesh even when bystanders sit in front or a backdrop shows through the gap being patched
    // (clicking a gap corner used to pin the sky dome 500m behind it). No selection = any mesh.
    bool patchClick(double mx, double my){
        float p[3]; u8 col[4];
        if (!sel.empty()){
            float bestD=1e30f, bp[3]={0,0,0}; u8 bc[4]={0,0,0,0}; bool got=false;
            for (int m : sel){
                if (m<0 || !r || m>=(int)r->gpuMeshes.size() || r->isDeleted((size_t)m) || r->isHidden((size_t)m)) continue;
                float q[3]; u8 c2[4];
                if (!screenRayHitColor(mx,my,m,q,c2)) continue;
                float dx=q[0]-r->cam.pos[0], dy=q[1]-r->cam.pos[1], dz=q[2]-r->cam.pos[2];
                float d=dx*dx+dy*dy+dz*dz;
                if (d<bestD){ bestD=d; memcpy(bp,q,12); memcpy(bc,c2,4); got=true; }
            }
            if (!got){ setStatus("pin: click ON the selected mesh(es) - pins are locked to the selection (deselect to pin on anything)"); return false; }
            memcpy(p,bp,12); memcpy(col,bc,4);
        } else {
            int hit = pickIndex(mx,my); if (hit<0) return false;
            if (!screenRayHitColor(mx,my,hit,p,col)) return false;
        }
        patchPts.push_back({p[0],p[1],p[2]});
        patchCols.push_back({col[0],col[1],col[2],col[3]});
        setStatus("patch: point "+std::to_string(patchPts.size())+(sel.empty()?"":" (on selection)")+" set (Enter builds with 3+, Esc cancels)");
        return true;
    }
    void buildPatch(){
        if (patchPts.size()<3){ setStatus("patch: click at least 3 corner points first"); patchMode=false; patchPts.clear(); patchCols.clear(); return; }
        if (!r || !sceneMeshes) return;
        MeshData md;                                      // FRESH mesh - nothing inherited, nothing stale
        md.name = "patch";
        md.doubleSided = true;                            // show from both sides regardless of click order
        md.useBlend = false; md.additive = false; md.alphaTest = false;
        md.transform = Transform{}; md.hasWorldMatrix = true;
        static const float I16[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(md.worldMatrix, I16, sizeof I16);
        size_t n = patchPts.size();
        // dominant plane (Newell normal) -> 2D coords for UVs + the generated texture
        float nx=0,ny=0,nz=0;
        for (size_t i2=0;i2<n;++i2){ auto& a=patchPts[i2]; auto& b=patchPts[(i2+1)%n];
            nx+=(a[1]-b[1])*(a[2]+b[2]); ny+=(a[2]-b[2])*(a[0]+b[0]); nz+=(a[0]-b[0])*(a[1]+b[1]); }
        int dom = std::fabs(nx)>std::fabs(ny) ? (std::fabs(nx)>std::fabs(nz)?0:2) : (std::fabs(ny)>std::fabs(nz)?1:2);
        int ua=(dom+1)%3, va=(dom+2)%3;
        float mnu=1e30f,mxu=-1e30f,mnv=1e30f,mxv=-1e30f;
        for (auto& p : patchPts){ mnu=std::min(mnu,p[ua]); mxu=std::max(mxu,p[ua]); mnv=std::min(mnv,p[va]); mxv=std::max(mxv,p[va]); }
        float su=(mxu-mnu)>1e-4f?(mxu-mnu):1.f, sv=(mxv-mnv)>1e-4f?(mxv-mnv):1.f;
        // ── AUTO-GENERATED TEXTURE (v2 - a REAL MAP): every texel PROJECTS onto the surrounding
        //    geometry along the patch plane (fused meshes included) and samples that triangle's texture
        //    through its OWN UVs (atlas-safe). Texels over the actual gap (nothing to sample) get the
        //    pin-color IDW blend. Result: the sheet shows the true surrounding imagery with a smooth
        //    fill in the hole = ready-made AI inpainting template (Export PNG -> AI -> Set texture).
        const int TS = 512;
        md.texRGBA.assign((size_t)TS*TS*4, 255); md.texW=TS; md.texH=TS; md.hasTexture=true;
        float patchDom=0; for (auto& p : patchPts) patchDom += p[dom]; patchDom/=(float)n;
        float pinDmn=1e30f, pinDmx=-1e30f; for (auto& p : patchPts){ pinDmn=std::min(pinDmn,p[dom]); pinDmx=std::max(pinDmx,p[dom]); }
        float domPad = std::max(3.f, 0.75f*(pinDmx-pinDmn));   // curved rims (dune/dome sides) need a TALLER sampling window than flat gaps
        // gather candidate triangles: any visible mesh whose bounds overlap the patch region (in the
        // plane) and sit inside the pins' height window along the dominant axis; bucket on a 2D grid.
        // With a SELECTION only the TARGETED meshes are sampled (focus: a tree standing in the region
        // must not bend the patch or leak into its texture).
        struct CTri { int mesh; uint32_t a,b,c; float au,av,bu,bv,cu,cv; float domAvg; float ad,bd,cd; };
        std::vector<CTri> ctris;
        std::map<std::pair<int,int>, std::vector<int>> cgrid;   // 1/32-of-patch cells
        float cellU = su/32.f, cellV = sv/32.f;
        for (int mi2=0; mi2<(int)r->gpuMeshes.size(); ++mi2){
            if (r->isHidden(mi2) || r->isDeleted(mi2)) continue;
            if (!sel.empty() && std::find(sel.begin(),sel.end(),mi2)==sel.end()) continue;   // locked to the selection
            const auto& gm2=r->gpuMeshes[mi2]; const MeshData& m2=(*sceneMeshes)[(size_t)mi2];
            if (gm2.isSkinned || gm2.dynamicVerts) continue;
            float mn3[3],mx3[3]; worldAabb(const_cast<VkGpuMesh&>(gm2),mn3,mx3);
            if (mx3[ua]<mnu||mn3[ua]>mxu||mx3[va]<mnv||mn3[va]>mxv) continue;
            if (mx3[dom]<pinDmn-domPad||mn3[dom]>pinDmx+domPad) continue;
            size_t nvm=m2.positions.size()/3; if (m2.uvs.size()<nvm*2 || m2.texRGBA.empty()) continue;
            const auto& P2=gm2.pickPos; const auto& I2=gm2.pickIdx; if (P2.size()<9||I2.size()<3) continue;
            for (size_t k=0;k+2<I2.size();k+=3){ uint32_t a=I2[k],b=I2[k+1],c=I2[k+2];
                if ((size_t)a*3+2>=P2.size()||(size_t)b*3+2>=P2.size()||(size_t)c*3+2>=P2.size()) continue;
                if (a>=nvm||b>=nvm||c>=nvm) continue;
                float wa[3],wb[3],wc[3]; xformPoint(gm2.model,&P2[a*3],wa); xformPoint(gm2.model,&P2[b*3],wb); xformPoint(gm2.model,&P2[c*3],wc);
                float dAvg=(wa[dom]+wb[dom]+wc[dom])/3.f;
                if (dAvg<pinDmn-domPad||dAvg>pinDmx+domPad) continue;
                CTri ct{mi2,a,b,c, wa[ua],wa[va], wb[ua],wb[va], wc[ua],wc[va], dAvg, wa[dom],wb[dom],wc[dom]};
                float tmnu=std::min({ct.au,ct.bu,ct.cu}), tmxu=std::max({ct.au,ct.bu,ct.cu});
                float tmnv=std::min({ct.av,ct.bv,ct.cv}), tmxv=std::max({ct.av,ct.bv,ct.cv});
                if (tmxu<mnu||tmnu>mxu||tmxv<mnv||tmnv>mxv) continue;
                int id=(int)ctris.size(); ctris.push_back(ct);
                for (int gu=(int)std::floor((tmnu-mnu)/cellU); gu<=(int)std::floor((tmxu-mnu)/cellU); ++gu)
                    for (int gv=(int)std::floor((tmnv-mnv)/cellV); gv<=(int)std::floor((tmxv-mnv)/cellV); ++gv)
                        cgrid[{gu,gv}].push_back(id);
            }
        }
        std::vector<std::array<float,2>> pinUV(n);
        for (size_t i2=0;i2<n;++i2) pinUV[i2] = { (patchPts[i2][ua]-mnu)/su, (patchPts[i2][va]-mnv)/sv };
        size_t baked=0;
        for (int ty2=0; ty2<TS; ++ty2) for (int tx2=0; tx2<TS; ++tx2){
            float u01=(tx2+0.5f)/TS, v01=(ty2+0.5f)/TS;
            float wu=mnu+u01*su, wv=mnv+v01*sv;               // texel's position in the patch plane
            u8* px=&md.texRGBA[((size_t)ty2*TS+tx2)*4];
            // find the surrounding triangle covering this texel (nearest to the patch plane wins)
            int bestT=-1; float bestDd=1e30f;
            auto itc=cgrid.find({(int)std::floor((wu-mnu)/cellU),(int)std::floor((wv-mnv)/cellV)});
            if (itc!=cgrid.end()) for (int ti2 : itc->second){ const CTri& T=ctris[ti2];
                float d0=(T.bu-T.au)*(wv-T.av)-(T.bv-T.av)*(wu-T.au);
                float d1=(T.cu-T.bu)*(wv-T.bv)-(T.cv-T.bv)*(wu-T.bu);
                float d2=(T.au-T.cu)*(wv-T.cv)-(T.av-T.cv)*(wu-T.cu);
                bool neg=(d0<1e-7f)&&(d1<1e-7f)&&(d2<1e-7f), pos=(d0>-1e-7f)&&(d1>-1e-7f)&&(d2>-1e-7f);
                if (!(neg||pos)) continue;
                float dd=std::fabs(T.domAvg-patchDom);
                if (dd<bestDd){ bestDd=dd; bestT=ti2; }
            }
            if (bestT>=0){
                const CTri& T=ctris[bestT]; const MeshData& m2=(*sceneMeshes)[(size_t)T.mesh];
                // 2D barycentric in the plane -> interpolated UV -> bilinear texture sample
                float den=(T.bv-T.cv)*(T.au-T.cu)+(T.cu-T.bu)*(T.av-T.cv);
                if (std::fabs(den)>1e-12f){
                    float l0=((T.bv-T.cv)*(wu-T.cu)+(T.cu-T.bu)*(wv-T.cv))/den;
                    float l1=((T.cv-T.av)*(wu-T.cu)+(T.au-T.cu)*(wv-T.cv))/den;
                    float l2=1.f-l0-l1;
                    float uu=l0*m2.uvs[T.a*2]+l1*m2.uvs[T.b*2]+l2*m2.uvs[T.c*2];
                    float vv=l0*m2.uvs[T.a*2+1]+l1*m2.uvs[T.b*2+1]+l2*m2.uvs[T.c*2+1];
                    uu-=std::floor(uu); vv-=std::floor(vv);
                    int W2=(int)m2.texW,H2=(int)m2.texH;
                    float fx=uu*W2-0.5f, fy=vv*H2-0.5f; int x0=(int)std::floor(fx), y0=(int)std::floor(fy); float tfx=fx-x0, tfy=fy-y0;
                    auto S=[&](int sx2,int sy2,int ch)->float{ sx2=std::clamp(sx2,0,W2-1); sy2=std::clamp(sy2,0,H2-1); return (float)m2.texRGBA[((size_t)sy2*W2+sx2)*4+ch]; };
                    for (int ch=0;ch<3;++ch){ float a2=S(x0,y0,ch)*(1-tfx)+S(x0+1,y0,ch)*tfx, b2=S(x0,y0+1,ch)*(1-tfx)+S(x0+1,y0+1,ch)*tfx;
                        px[ch]=(u8)std::clamp(a2*(1-tfy)+b2*tfy,0.f,255.f); }
                    px[3]=255; ++baked; continue;
                }
            }
            // over the actual GAP: smooth pin-color blend (AI inpaints this region beautifully)
            float wsum=0, acc[3]={0,0,0};
            for (size_t i2=0;i2<n;++i2){
                float du=u01-pinUV[i2][0], dv=v01-pinUV[i2][1];
                float w2 = 1.f/(du*du+dv*dv+1e-4f);
                wsum+=w2; for (int ch=0;ch<3;++ch) acc[ch]+=w2*(float)patchCols[i2][ch];
            }
            for (int ch=0;ch<3;++ch) px[ch]=(u8)std::clamp(acc[ch]/wsum, 0.f, 255.f);
            px[3]=255;
        }
        fprintf(stderr,"[EDIT] patch bake: %zu/%d texels sampled from surrounding geometry, rest = gap blend\n", baked, TS*TS);
        // ── CURVED GEOMETRY (v2): the old single-center FAN was always a FLAT plate — patching a dune
        //    side / dome / anything with height+depth sliced straight through the terrain. The patch is
        //    now a SUBDIVIDED disk (rim edges split, rings marching to the centroid) and EVERY generated
        //    vertex is PROJECTED onto the surrounding geometry along the dominant axis — the exact same
        //    triangles the texture bake samples, so the sheet follows the actual triangle directions:
        //    curvature, slope, height. Points over the true gap blend the pin heights (IDW). Pins stay
        //    EXACTLY where they were clicked.
        float cen[3]={0,0,0}; for (auto& p : patchPts){ cen[0]+=p[0]; cen[1]+=p[1]; cen[2]+=p[2]; }
        cen[0]/=n; cen[1]/=n; cen[2]/=n;
        auto pinDomIDW=[&](float wu, float wv)->float{                        // gap fallback: blend of the pins' heights
            float wsum=0, acc=0;
            for (size_t i2=0;i2<n;++i2){ float du=wu-patchPts[i2][ua], dv=wv-patchPts[i2][va];
                float w2=1.f/(du*du+dv*dv+1e-6f); wsum+=w2; acc+=w2*patchPts[i2][dom]; }
            return acc/wsum; };
        auto surfDomAt=[&](float wu, float wv, float est, float& outD)->bool{ // surviving-surface height at a plane point
            auto itc=cgrid.find({(int)std::floor((wu-mnu)/cellU),(int)std::floor((wv-mnv)/cellV)});
            if (itc==cgrid.end()) return false;
            bool got=false; float best=1e30f;
            for (int ti2 : itc->second){ const CTri& T=ctris[ti2];
                float den=(T.bv-T.cv)*(T.au-T.cu)+(T.cu-T.bu)*(T.av-T.cv);
                if (std::fabs(den)<1e-12f) continue;
                float l0=((T.bv-T.cv)*(wu-T.cu)+(T.cu-T.bu)*(wv-T.cv))/den;
                float l1=((T.cv-T.av)*(wu-T.cu)+(T.au-T.cu)*(wv-T.cv))/den;
                float l2=1.f-l0-l1;
                if (l0<-1e-4f||l1<-1e-4f||l2<-1e-4f) continue;                // not inside this triangle
                float d2=l0*T.ad+l1*T.bd+l2*T.cd;                             // interpolated surface height
                float dd=std::fabs(d2-est);
                if (dd<best){ best=dd; outD=d2; got=true; }                   // the sheet CLOSEST to the estimate wins
            }
            return got && best<=domPad; };
        auto solveDom=[&](float q[3], bool exactPin){
            if (exactPin) return;                                             // clicked pins = ground truth
            float est=pinDomIDW(q[ua],q[va]); float d2;
            q[dom] = surfDomAt(q[ua],q[va],est,d2) ? d2 : est; };
        auto pushV=[&](const float p[3]){ md.positions.push_back(p[0]); md.positions.push_back(p[1]); md.positions.push_back(p[2]);
            md.uvs.push_back((p[ua]-mnu)/su); md.uvs.push_back((p[va]-mnv)/sv); };
        const int SB = std::max(1,(int)std::ceil(24.f/(float)n));             // rim segments per clicked edge (~24 rim verts)
        const int MR = (int)n*SB;                                             // rim vertex count
        const int RG = 6;                                                     // rings rim -> center
        for (int r2=0; r2<RG; ++r2){
            float fr=(float)r2/(float)RG;
            for (int j=0;j<MR;++j){
                int pi2=j/SB, ps=j%SB; float ft=(float)ps/(float)SB;
                const auto& A=patchPts[(size_t)pi2]; const auto& B=patchPts[(size_t)((pi2+1)%n)];
                float rq[3]={A[0]+(B[0]-A[0])*ft, A[1]+(B[1]-A[1])*ft, A[2]+(B[2]-A[2])*ft};
                float q[3]={rq[0]+(cen[0]-rq[0])*fr, rq[1]+(cen[1]-rq[1])*fr, rq[2]+(cen[2]-rq[2])*fr};
                solveDom(q, r2==0 && ps==0);
                pushV(q);
            }
        }
        float cq[3]={cen[0],cen[1],cen[2]}; solveDom(cq,false); pushV(cq);
        uint32_t ci=(uint32_t)((size_t)MR*RG);
        for (int r2=0; r2<RG-1; ++r2) for (int j=0;j<MR;++j){ int j1=(j+1)%MR;
            uint32_t a0=(uint32_t)(r2*MR+j), b0=(uint32_t)(r2*MR+j1), a1=(uint32_t)((r2+1)*MR+j), b1=(uint32_t)((r2+1)*MR+j1);
            md.indices.push_back(a0); md.indices.push_back(b0); md.indices.push_back(a1);
            md.indices.push_back(b0); md.indices.push_back(b1); md.indices.push_back(a1);
        }
        for (int j=0;j<MR;++j){ int j1=(j+1)%MR;
            md.indices.push_back((uint32_t)((RG-1)*MR+j)); md.indices.push_back((uint32_t)((RG-1)*MR+j1)); md.indices.push_back(ci); }
        md.nVerts=(u32)(md.positions.size()/3); md.nIdx=(u32)md.indices.size();
        sceneMeshes->push_back(std::move(md));
        size_t ni=r->gpuMeshes.size();
        r->uploadMesh(sceneMeshes->back());
        if (r->gpuMeshes.size()!=ni+1){ setStatus("patch: GPU upload failed"); sceneMeshes->pop_back(); return; }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        pushDeleteUndo({(int)ni}, false);                 // Ctrl+Z removes the patch
        meshEditLog[(int)ni]="patch("+std::to_string(n)+"pts)";
        geomDirty=true; holesMesh=-1;
        selectOne((int)ni); scrollToSel=true;
        patchPts.clear(); patchCols.clear(); patchMode=false;
        setStatus("Patch built - follows the surrounding surface curvature; texture auto-blended (Export PNG copies a ready ChatGPT inpaint prompt -> Set texture)");
    }

    // ── BEND / STRETCH ("grab" deform): click a point on the mesh (1 pin in bend mode), set radius +
    //    offset, Apply -> vertices within the radius move by offset * smooth cosine falloff (full at
    //    the pin, zero at the rim). Pulls dents/hills/bends into the geometry - texture follows the
    //    verts (UVs unchanged). Repeatable; Ctrl+Z reverts each apply.
    bool  pinIsBend = false;
    float bendRadius = 2.f, bendOff[3] = {0.f, 0.5f, 0.f};
    void bendAtPin(){
        if (patchPts.empty()){ setStatus("bend: click a point on the mesh first"); return; }
        const auto& pin = patchPts.back();
        int applied=0;
        forEachSelMesh([&](int mi){
            if (mi<0||mi>=(int)sceneMeshes->size()||mi>=(int)r->gpuMeshes.size()) return;
            MeshData md=(*sceneMeshes)[(size_t)mi];
            const float* M=r->gpuMeshes[(size_t)mi].model;
            float inv[16]; if (!invertMat4(const_cast<float*>(M), inv)) return;   // pull the world offset into MODEL space
            size_t nv=md.positions.size()/3; int moved=0;
            // pin + offset in model space (positions are model-space; gm.model re-applies on draw)
            float pinL[3]={ inv[0]*pin[0]+inv[4]*pin[1]+inv[8]*pin[2]+inv[12],
                            inv[1]*pin[0]+inv[5]*pin[1]+inv[9]*pin[2]+inv[13],
                            inv[2]*pin[0]+inv[6]*pin[1]+inv[10]*pin[2]+inv[14] };
            float offL[3]={ inv[0]*bendOff[0]+inv[4]*bendOff[1]+inv[8]*bendOff[2],
                            inv[1]*bendOff[0]+inv[5]*bendOff[1]+inv[9]*bendOff[2],
                            inv[2]*bendOff[0]+inv[6]*bendOff[1]+inv[10]*bendOff[2] };
            float R = bendRadius>0.01f?bendRadius:0.01f;
            for (size_t v2=0;v2<nv;++v2){
                float dx=md.positions[v2*3]-pinL[0], dy=md.positions[v2*3+1]-pinL[1], dz=md.positions[v2*3+2]-pinL[2];
                float dist=std::sqrt(dx*dx+dy*dy+dz*dz);
                if (dist>=R) continue;
                float w2 = 0.5f+0.5f*std::cos(3.14159265f*dist/R);   // smooth falloff (1 at pin -> 0 at rim)
                md.positions[v2*3]+=offL[0]*w2; md.positions[v2*3+1]+=offL[1]*w2; md.positions[v2*3+2]+=offL[2]*w2;
                ++moved;
            }
            if (!moved) return;
            if (commitGeomEdit(mi, std::move(md), "bend")) ++applied;
        });
        setStatus(applied ? ("Bent/stretched "+std::to_string(applied)+" mesh(es) around the pin (repeat to sculpt more; Ctrl+Z reverts)")
                          : "bend: no vertices inside the radius on the selection - click closer or raise the radius");
    }

    // ── FUSE + REBUILD SURFACE: the PROPER merge for stacked/overlapping sheets (floors). Instead of
    //    concatenating triangles, the selection is RE-BUILT from scratch:
    //      1. project everything onto the dominant plane and rasterize the union FOOTPRINT (256 grid),
    //      2. per cell keep the TOP surface height (under-layers are gone - no overlaps, no z-fight),
    //      3. retriangulate as a clean uniform grid (two tris/cell, single layer, watertight interior),
    //      4. BAKE the appearance into ONE 1024 texture by sampling each covering triangle through its
    //         own UVs (atlas-safe) -> texture preserved, UVs = flat 0..1 sheet.
    //    Sources go to history (one Ctrl+Z restores them and removes the rebuild).
    void rebuildSurface(){
        if (!r || !sceneMeshes || sel.empty()) { setStatus("rebuild: select the meshes to merge first"); return; }
        std::vector<int> src; int skipped=0;
        for (int s : sel) { if (s<0||s>=(int)sceneMeshes->size()||s>=(int)r->gpuMeshes.size()||r->isDeleted((size_t)s)) continue;
            if (r->gpuMeshes[s].isSkinned || r->gpuMeshes[s].dynamicVerts) { ++skipped; continue; }
            src.push_back(s); }
        if (src.empty()) { setStatus("rebuild: no static meshes in the selection"); return; }
        // world triangles of every source + area-weighted normal
        struct RTri { int mesh; uint32_t a,b,c; float wa[3],wb[3],wc[3]; };
        std::vector<RTri> tris;
        float nx=0,ny=0,nz=0;
        for (int s : src){ const MeshData& md=(*sceneMeshes)[(size_t)s]; const float* M=r->gpuMeshes[(size_t)s].model;
            size_t nvm=md.positions.size()/3;
            for (size_t k=0;k+2<md.indices.size();k+=3){ uint32_t a=md.indices[k],b=md.indices[k+1],c=md.indices[k+2];
                if (a>=nvm||b>=nvm||c>=nvm) continue;
                RTri t; t.mesh=s; t.a=a; t.b=b; t.c=c;
                auto X=[&](uint32_t v2, float o[3]){ const float* p=&md.positions[v2*3];
                    o[0]=M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]; o[1]=M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]; o[2]=M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]; };
                X(a,t.wa); X(b,t.wb); X(c,t.wc);
                float e1[3]={t.wb[0]-t.wa[0],t.wb[1]-t.wa[1],t.wb[2]-t.wa[2]}, e2[3]={t.wc[0]-t.wa[0],t.wc[1]-t.wa[1],t.wc[2]-t.wa[2]};
                nx+=e1[1]*e2[2]-e1[2]*e2[1]; ny+=e1[2]*e2[0]-e1[0]*e2[2]; nz+=e1[0]*e2[1]-e1[1]*e2[0];
                tris.push_back(t);
            } }
        if (tris.empty()) { setStatus("rebuild: no triangles found"); return; }
        int dom = std::fabs(nx)>std::fabs(ny) ? (std::fabs(nx)>std::fabs(nz)?0:2) : (std::fabs(ny)>std::fabs(nz)?1:2);
        int ua=(dom+1)%3, va=(dom+2)%3;
        float mnu=1e30f,mxu=-1e30f,mnv=1e30f,mxv=-1e30f;
        for (auto& t : tris) for (const float* w : {t.wa,t.wb,t.wc}){ mnu=std::min(mnu,w[ua]); mxu=std::max(mxu,w[ua]); mnv=std::min(mnv,w[va]); mxv=std::max(mxv,w[va]); }
        float su=(mxu-mnu)>1e-3f?(mxu-mnu):1.f, sv=(mxv-mnv)>1e-3f?(mxv-mnv):1.f;
        const int G=256, T=1024;
        float cellU=su/G, cellV=sv/G;
        // bucket triangles over the G-grid (by 2D bbox)
        std::vector<std::vector<int>> bucket((size_t)G*G);
        for (int ti2=0; ti2<(int)tris.size(); ++ti2){ auto& t=tris[ti2];
            float tmnu=std::min({t.wa[ua],t.wb[ua],t.wc[ua]}), tmxu=std::max({t.wa[ua],t.wb[ua],t.wc[ua]});
            float tmnv=std::min({t.wa[va],t.wb[va],t.wc[va]}), tmxv=std::max({t.wa[va],t.wb[va],t.wc[va]});
            int cu0=std::clamp((int)((tmnu-mnu)/cellU),0,G-1), cu1=std::clamp((int)((tmxu-mnu)/cellU),0,G-1);
            int cv0=std::clamp((int)((tmnv-mnv)/cellV),0,G-1), cv1=std::clamp((int)((tmxv-mnv)/cellV),0,G-1);
            for (int cv=cv0;cv<=cv1;++cv) for (int cu=cu0;cu<=cu1;++cu) bucket[(size_t)cv*G+cu].push_back(ti2);
        }
        // covering triangle at a plane point: TOP surface wins (max dom coordinate)
        auto coverAt=[&](float u0,float v0, int& outTri, float& outDom)->bool{
            int cu=std::clamp((int)((u0-mnu)/cellU),0,G-1), cv=std::clamp((int)((v0-mnv)/cellV),0,G-1);
            outTri=-1; outDom=-1e30f;
            for (int ti2 : bucket[(size_t)cv*G+cu]){ const RTri& t=tris[ti2];
                float au=t.wa[ua],av=t.wa[va], bu=t.wb[ua],bv=t.wb[va], cu2=t.wc[ua],cv2=t.wc[va];
                float den=(bv-cv2)*(au-cu2)+(cu2-bu)*(av-cv2);
                if (std::fabs(den)<1e-12f) continue;
                float l0=((bv-cv2)*(u0-cu2)+(cu2-bu)*(v0-cv2))/den;
                float l1=((cv2-av)*(u0-cu2)+(au-cu2)*(v0-cv2))/den;
                float l2=1.f-l0-l1;
                if (l0<-1e-4f||l1<-1e-4f||l2<-1e-4f) continue;
                float dcoord=l0*t.wa[dom]+l1*t.wb[dom]+l2*t.wc[dom];
                if (dcoord>outDom){ outDom=dcoord; outTri=ti2; }
            }
            return outTri>=0;
        };
        // occupancy + top height per cell
        std::vector<char>  occ((size_t)G*G, 0);
        std::vector<float> hgt((size_t)G*G, 0.f);
        int occCount=0;
        for (int cv=0;cv<G;++cv) for (int cu=0;cu<G;++cu){
            float u0=mnu+(cu+0.5f)*cellU, v0=mnv+(cv+0.5f)*cellV;
            int t2; float d2;
            if (coverAt(u0,v0,t2,d2)){ occ[(size_t)cv*G+cu]=1; hgt[(size_t)cv*G+cu]=d2; ++occCount; }
        }
        if (!occCount){ setStatus("rebuild: footprint rasterization found nothing"); return; }
        // BAKE the texture: every texel samples the TOP covering triangle through ITS OWN UVs
        MeshData out;
        out.name="rebuilt"; out.doubleSided=true;
        out.transform=Transform{}; out.hasWorldMatrix=true;
        static const float I16[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(out.worldMatrix, I16, sizeof I16);
        out.texRGBA.assign((size_t)T*T*4, 255); out.texW=T; out.texH=T; out.hasTexture=true;
        for (int ty2=0;ty2<T;++ty2) for (int tx2=0;tx2<T;++tx2){
            float u0=mnu+((tx2+0.5f)/T)*su, v0=mnv+((ty2+0.5f)/T)*sv;
            int t2; float d2;
            u8* px=&out.texRGBA[((size_t)ty2*T+tx2)*4];
            if (!coverAt(u0,v0,t2,d2)) { px[3]=255; continue; }
            const RTri& t=tris[t2]; const MeshData& m2=(*sceneMeshes)[(size_t)t.mesh];
            size_t nvm=m2.positions.size()/3;
            if (m2.uvs.size()<nvm*2 || m2.texRGBA.empty()) { px[0]=px[1]=px[2]=170; continue; }
            float au=t.wa[ua],av=t.wa[va], bu=t.wb[ua],bv=t.wb[va], cu2=t.wc[ua],cv2=t.wc[va];
            float den=(bv-cv2)*(au-cu2)+(cu2-bu)*(av-cv2); if (std::fabs(den)<1e-12f) continue;
            float l0=((bv-cv2)*(u0-cu2)+(cu2-bu)*(v0-cv2))/den;
            float l1=((cv2-av)*(u0-cu2)+(au-cu2)*(v0-cv2))/den;
            float l2=1.f-l0-l1;
            float uu=l0*m2.uvs[t.a*2]+l1*m2.uvs[t.b*2]+l2*m2.uvs[t.c*2];
            float vv=l0*m2.uvs[t.a*2+1]+l1*m2.uvs[t.b*2+1]+l2*m2.uvs[t.c*2+1];
            uu-=std::floor(uu); vv-=std::floor(vv);
            int W2=(int)m2.texW,H2=(int)m2.texH;
            float fx=uu*W2-0.5f, fy=vv*H2-0.5f; int x0=(int)std::floor(fx), y0=(int)std::floor(fy); float tfx=fx-x0, tfy=fy-y0;
            auto S=[&](int sx2,int sy2,int ch)->float{ sx2=std::clamp(sx2,0,W2-1); sy2=std::clamp(sy2,0,H2-1); return (float)m2.texRGBA[((size_t)sy2*W2+sx2)*4+ch]; };
            for (int ch=0;ch<3;++ch){ float a2=S(x0,y0,ch)*(1-tfx)+S(x0+1,y0,ch)*tfx, b2=S(x0,y0+1,ch)*(1-tfx)+S(x0+1,y0+1,ch)*tfx;
                px[ch]=(u8)std::clamp(a2*(1-tfy)+b2*tfy,0.f,255.f); }
            px[3]=255;
        }
        // RETRIANGULATE like a real author would: TRACE the footprint's boundary CONTOURS (outer
        // outline + hole loops) off the occupancy grid, SIMPLIFY them (Douglas-Peucker), then
        // EAR-CLIP the polygon-with-holes. Result: big, well-shaped triangles that follow the actual
        // silhouette - like the source mesh's own triangulation, not a grid, not slivers.
        std::vector<float> cornerH((size_t)(G+1)*(G+1), 0.f);
        for (int cv=0; cv<=G; ++cv) for (int cu=0; cu<=G; ++cu){
            float a=0; int c2=0;
            for (int oy=-1;oy<=0;++oy) for (int ox=-1;ox<=0;++ox){ int xx=cu+ox, yy=cv+oy;
                if (xx<0||yy<0||xx>=G||yy>=G||!occ[(size_t)yy*G+xx]) continue; a+=hgt[(size_t)yy*G+xx]; ++c2; }
            cornerH[(size_t)cv*(G+1)+cu] = c2? a/c2 : 0.f;
        }
        auto emitVert=[&](float gu, float gv)->uint32_t{           // grid coords -> world vert + uv
            uint32_t id=(uint32_t)(out.positions.size()/3);
            float uu2=mnu+gu*cellU, vv2=mnv+gv*cellV;
            int iu=std::clamp((int)std::lround(gu),0,G), iv=std::clamp((int)std::lround(gv),0,G);
            float p[3]; p[ua]=uu2; p[va]=vv2; p[dom]=cornerH[(size_t)iv*(G+1)+iu];
            out.positions.push_back(p[0]); out.positions.push_back(p[1]); out.positions.push_back(p[2]);
            out.uvs.push_back((uu2-mnu)/su); out.uvs.push_back((vv2-mnv)/sv);
            return id;
        };
        int triCount=0;
        {
            // 1) boundary SEGMENT SOUP: for each occupied cell side facing empty space, a directed unit
            //    segment with the interior on its LEFT; chain segments into closed loops.
            typedef std::pair<int,int> GP;
            std::multimap<GP,GP> segs;
            auto empt=[&](int cu,int cv){ return cu<0||cv<0||cu>=G||cv>=G||!occ[(size_t)cv*G+cu]; };
            for (int cv=0;cv<G;++cv) for (int cu=0;cu<G;++cu){ if (!occ[(size_t)cv*G+cu]) continue;
                if (empt(cu,cv-1)) segs.insert({{cu,cv},{cu+1,cv}});
                if (empt(cu+1,cv)) segs.insert({{cu+1,cv},{cu+1,cv+1}});
                if (empt(cu,cv+1)) segs.insert({{cu+1,cv+1},{cu,cv+1}});
                if (empt(cu-1,cv)) segs.insert({{cu,cv+1},{cu,cv}});
            }
            std::vector<std::vector<GP>> loops2;
            while (!segs.empty()){
                auto it=segs.begin(); GP start=it->first, cur=it->second; segs.erase(it);
                std::vector<GP> lp{start};
                for (int guard=0; guard<4*G*G; ++guard){
                    lp.push_back(cur);
                    if (cur==start) break;
                    auto rng=segs.equal_range(cur);
                    if (rng.first==rng.second){ lp.clear(); break; }
                    GP nxt = rng.first->second; segs.erase(rng.first);   // (multi-branch corners: any exit works)
                    cur = nxt;
                }
                if (lp.size()>3 && lp.front()==lp.back()){ lp.pop_back(); loops2.push_back(std::move(lp)); }
            }
            // 2) simplify each loop (Douglas-Peucker, closed; 1.5-cell tolerance keeps the silhouette)
            auto dpSimp=[&](const std::vector<GP>& in)->std::vector<std::array<float,2>>{
                std::vector<std::array<float,2>> P; P.reserve(in.size());
                for (auto& g : in) P.push_back({(float)g.first,(float)g.second});
                if (P.size()<8) return P;
                std::vector<char> keep(P.size(),0); keep[0]=1;
                std::function<void(int,int)> rec=[&](int a2,int b2){
                    if (b2-a2<2) return;
                    float ax=P[a2][0],ay=P[a2][1],bx=P[b2%P.size()][0],by=P[b2%P.size()][1];
                    float dx=bx-ax,dy=by-ay,len=std::sqrt(dx*dx+dy*dy); if(len<1e-6f){len=1;}
                    int wi=-1; float wd=0;
                    for (int i2=a2+1;i2<b2;++i2){ float d2=std::fabs((P[i2][0]-ax)*dy-(P[i2][1]-ay)*dx)/len;
                        if (d2>wd){ wd=d2; wi=i2; } }
                    if (wi>=0 && wd>1.5f){ keep[wi]=1; rec(a2,wi); rec(wi,b2); }
                };
                int half=(int)P.size()/2; keep[half]=1;
                rec(0,half); rec(half,(int)P.size());
                std::vector<std::array<float,2>> outP;
                for (size_t i2=0;i2<P.size();++i2) if (keep[i2]) outP.push_back(P[i2]);
                return outP;
            };
            struct Poly { std::vector<std::array<float,2>> pts; float area; };
            std::vector<Poly> outers, holes2;
            for (auto& lp : loops2){
                auto sp=dpSimp(lp); if (sp.size()<3) continue;
                float ar=0; for (size_t i2=0;i2<sp.size();++i2){ auto& a2=sp[i2]; auto& b2=sp[(i2+1)%sp.size()];
                    ar += a2[0]*b2[1]-b2[0]*a2[1]; }
                ar*=0.5f;
                if (ar>1.f) outers.push_back({sp,ar}); else if (ar<-1.f) holes2.push_back({sp,ar});
            }
            // 3) per OUTER: bridge its holes in (rightmost-vertex ray split), then EAR-CLIP
            auto inPoly=[&](const std::vector<std::array<float,2>>& pp, float u0, float v0)->bool{
                bool in=false; size_t np=pp.size();
                for (size_t i2=0,j=np-1;i2<np;j=i2++){
                    if (((pp[i2][1]>v0)!=(pp[j][1]>v0)) &&
                        (u0 < (pp[j][0]-pp[i2][0])*(v0-pp[i2][1])/(pp[j][1]-pp[i2][1])+pp[i2][0])) in=!in; }
                return in; };
            for (auto& op : outers){
                std::vector<std::array<float,2>> poly = op.pts;   // CCW
                // holes inside this outer, rightmost first
                std::vector<const Poly*> myHoles;
                for (auto& hp : holes2) if (inPoly(op.pts, hp.pts[0][0], hp.pts[0][1])) myHoles.push_back(&hp);
                std::sort(myHoles.begin(), myHoles.end(), [](const Poly* a2, const Poly* b2){
                    float ma=-1e30f, mb=-1e30f; for (auto& p2 : a2->pts) ma=std::max(ma,p2[0]); for (auto& p2 : b2->pts) mb=std::max(mb,p2[0]);
                    return ma>mb; });
                for (const Poly* hp : myHoles){
                    size_t hm=0; for (size_t i2=1;i2<hp->pts.size();++i2) if (hp->pts[i2][0]>hp->pts[hm][0]) hm=i2;
                    float hu=hp->pts[hm][0], hv2=hp->pts[hm][1];
                    // nearest polygon vertex to the RIGHT of the hole's rightmost point (simple, robust enough on grid contours)
                    int best=-1; float bd=1e30f;
                    for (size_t i2=0;i2<poly.size();++i2){ if (poly[i2][0] < hu-0.01f) continue;
                        float d2=(poly[i2][0]-hu)*(poly[i2][0]-hu)+(poly[i2][1]-hv2)*(poly[i2][1]-hv2);
                        if (d2<bd){ bd=d2; best=(int)i2; } }
                    if (best<0) continue;
                    std::vector<std::array<float,2>> merged; merged.reserve(poly.size()+hp->pts.size()+2);
                    for (int i2=0;i2<=best;++i2) merged.push_back(poly[i2]);
                    for (size_t k=0;k<=hp->pts.size();++k) merged.push_back(hp->pts[(hm+k)%hp->pts.size()]);   // hole loop (CW) + back to hm
                    for (size_t i2=best;i2<poly.size();++i2) merged.push_back(poly[i2]);
                    poly=std::move(merged);
                }
                // ear clipping (CCW polygon)
                std::vector<int> idx(poly.size()); for (size_t i2=0;i2<poly.size();++i2) idx[i2]=(int)i2;
                std::vector<uint32_t> vid(poly.size(), UINT32_MAX);
                auto V2=[&](int i2)->uint32_t{ if (vid[idx[i2]]==UINT32_MAX) vid[idx[i2]]=emitVert(poly[idx[i2]][0], poly[idx[i2]][1]); return vid[idx[i2]]; };
                int guard=(int)poly.size()*(int)poly.size()+16;
                while (idx.size()>3 && guard-->0){
                    bool clipped=false;
                    for (size_t i2=0;i2<idx.size();++i2){
                        int ia=idx[(i2+idx.size()-1)%idx.size()], ib=idx[i2], ic=idx[(i2+1)%idx.size()];
                        auto& A=poly[ia]; auto& B=poly[ib]; auto& C=poly[ic];
                        float cr=(B[0]-A[0])*(C[1]-A[1])-(B[1]-A[1])*(C[0]-A[0]);
                        if (cr<=1e-7f) continue;                     // reflex/degenerate
                        bool ear=true;
                        for (int j : idx){ if (j==ia||j==ib||j==ic) continue;
                            float d0=(B[0]-A[0])*(poly[j][1]-A[1])-(B[1]-A[1])*(poly[j][0]-A[0]);
                            float d1=(C[0]-B[0])*(poly[j][1]-B[1])-(C[1]-B[1])*(poly[j][0]-B[0]);
                            float d2=(A[0]-C[0])*(poly[j][1]-C[1])-(A[1]-C[1])*(poly[j][0]-C[0]);
                            if (d0>=-1e-7f&&d1>=-1e-7f&&d2>=-1e-7f){ ear=false; break; } }
                        if (!ear) continue;
                        uint32_t va2=V2((int)((i2+idx.size()-1)%idx.size())), vb2=V2((int)i2), vc2=V2((int)((i2+1)%idx.size()));
                        out.indices.push_back(va2); out.indices.push_back(vb2); out.indices.push_back(vc2); ++triCount;
                        idx.erase(idx.begin()+i2); clipped=true; break;
                    }
                    if (!clipped) break;                             // stuck (degenerate input) - keep what we have
                }
                if (idx.size()==3){ uint32_t va2=V2(0), vb2=V2(1), vc2=V2(2);
                    out.indices.push_back(va2); out.indices.push_back(vb2); out.indices.push_back(vc2); ++triCount; }
            }
        }
        if (!triCount){ setStatus("rebuild: contour triangulation failed (degenerate footprint)"); return; }
        fprintf(stderr,"[EDIT] rebuild: %d occupied cells -> contour ear-clip = %d tris, %zu verts (author-grade)\n",
                occCount, triCount, out.positions.size()/3);
        out.nVerts=(u32)(out.positions.size()/3); out.nIdx=(u32)out.indices.size();
        sceneMeshes->push_back(std::move(out));
        size_t ni=r->gpuMeshes.size();
        r->uploadMesh(sceneMeshes->back());
        if (r->gpuMeshes.size()!=ni+1){ setStatus("rebuild: GPU upload failed"); sceneMeshes->pop_back(); return; }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        std::vector<int> hist=src; std::vector<uint8_t> histA(src.size(),1);
        for (int s : src) r->setDeleted((size_t)s, true);
        hist.push_back((int)ni); histA.push_back(0);
        pushDeleteUndo(hist, histA);
        meshEditLog[(int)ni] = "rebuild(" + std::to_string(src.size()) + ")";
        geomDirty=true; holesMesh=-1;
        selectOne((int)ni); scrollToSel=true;
        setStatus("REBUILT "+std::to_string(src.size())+" mesh(es) as ONE clean surface: "+std::to_string(triCount)
                  +" author-grade triangles (contour ear-clip), texture baked 1024"
                  +(skipped?" - skipped "+std::to_string(skipped)+" animated":"")+" - Ctrl+Z restores the originals");
    }

    // ── PREFABS: save the selection as a reusable asset (prefabs/<name>.hsrprefab - geometry world-
    //    baked relative to the selection center + texture as PNG), then SPAWN copies anywhere: from
    //    the Scene tab list, or by DRAG & DROPPING the .hsrprefab file into the window. Spawns land
    //    where you're looking, are undoable, persist via the .geom sidecar, and cook like any mesh.
    std::vector<std::string> prefabList; double prefabListAt = 0.0;
    void refreshPrefabList(){
        prefabList.clear();
        std::error_code ec; std::filesystem::create_directories("prefabs", ec);
        for (auto& de : std::filesystem::directory_iterator("prefabs", ec))
            if (de.path().extension()==".hsrprefab") prefabList.push_back(de.path().string());
        std::sort(prefabList.begin(), prefabList.end());
    }
    void savePrefabFromSelection(){
        if (!r || !sceneMeshes || sel.empty()) { setStatus("prefab: select the mesh(es) to save first"); return; }
        std::vector<int> src;
        for (int s : sel) if (s>=0 && s<(int)sceneMeshes->size() && s<(int)r->gpuMeshes.size() && !r->isDeleted((size_t)s)
                              && !r->gpuMeshes[s].isSkinned && !r->gpuMeshes[s].dynamicVerts) src.push_back(s);
        if (src.empty()) { setStatus("prefab: no static meshes in the selection"); return; }
        // anchor = selection center (world) so spawns place naturally where you aim
        float cen[3]={0,0,0}; int cn=0;
        for (int s : src){ const auto& gm=r->gpuMeshes[(size_t)s]; cen[0]+=gm.centroid[0]+gm.editT[0]; cen[1]+=gm.centroid[1]+gm.editT[1]; cen[2]+=gm.centroid[2]+gm.editT[2]; ++cn; }
        cen[0]/=cn; cen[1]/=cn; cen[2]/=cn;
        std::error_code ec; std::filesystem::create_directories("prefabs", ec);
        std::string base; for (char c : r->gpuMeshes[(size_t)src[0]].name) base += (isalnum((unsigned char)c)||c=='_'||c=='-')?c:'_';
        if (base.size()>40) base.resize(40);
        std::string gp; for (int k=1;k<1000;++k){ gp = "prefabs/"+base+(k>1?("_"+std::to_string(k)):"")+".hsrprefab";
            if (!std::filesystem::exists(gp, ec)) break; }
        FILE* f=fopen(gp.c_str(),"wb"); if(!f){ setStatus("prefab: can't write "+gp); return; }
        auto w32=[&](u32 v){ fwrite(&v,4,1,f); };
        auto wstr=[&](const std::string& s2){ w32((u32)s2.size()); fwrite(s2.data(),1,s2.size(),f); };
        fwrite("HSRPFAB1",8,1,f); w32((u32)src.size());
        for (int s : src){
            const MeshData& md=(*sceneMeshes)[(size_t)s]; const float* M=r->gpuMeshes[(size_t)s].model;
            wstr(md.name);
            u8 flags[4]={ md.useBlend?(u8)1:(u8)0, md.additive?(u8)1:(u8)0, md.alphaTest?(u8)1:(u8)0, md.doubleSided?(u8)1:(u8)0 };
            fwrite(flags,1,4,f);
            w32(md.texW); w32(md.texH);
            std::vector<u8> png;
            if (!md.texRGBA.empty() && md.texW>0)
                stbi_write_png_to_func([](void* ctx, void* d, int n2){ auto* v=(std::vector<u8>*)ctx; v->insert(v->end(),(u8*)d,(u8*)d+n2); },
                                       &png, md.texW, md.texH, 4, md.texRGBA.data(), md.texW*4);
            w32((u32)png.size()); if(!png.empty()) fwrite(png.data(),1,png.size(),f);
            // world-baked positions RELATIVE to the anchor
            size_t nvm=md.positions.size()/3;
            std::vector<float> rel; rel.reserve(nvm*3);
            for (size_t v2=0;v2<nvm;++v2){ const float* p=&md.positions[v2*3];
                rel.push_back(M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]-cen[0]);
                rel.push_back(M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]-cen[1]);
                rel.push_back(M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]-cen[2]); }
            w32((u32)rel.size());        fwrite(rel.data(),4,rel.size(),f);
            w32((u32)md.uvs.size());     if(!md.uvs.empty())  fwrite(md.uvs.data(),4,md.uvs.size(),f);
            w32((u32)md.indices.size()); if(!md.indices.empty()) fwrite(md.indices.data(),4,md.indices.size(),f);
        }
        fclose(f);
        refreshPrefabList();
        std::string abs = std::filesystem::absolute(gp, ec).string(); for (char& c : abs) if (c=='/') c='\\';
        setStatus("PREFAB saved: "+abs+"  - spawn it from Scene > Prefabs, or drag the file into the window");
    }
    void spawnPrefab(const std::string& path){
        if (!r || !sceneMeshes) return;
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ setStatus("prefab: can't open "+path); return; }
        char magic[8]; if (fread(magic,1,8,f)!=8 || memcmp(magic,"HSRPFAB1",8)!=0){ fclose(f); setStatus("prefab: bad file (not a .hsrprefab)"); return; }
        auto r32=[&]()->u32{ u32 v=0; fread(&v,4,1,f); return v; };
        auto rstr=[&]()->std::string{ u32 n2=r32(); std::string s2(n2,'\0'); if(n2) fread(&s2[0],1,n2,f); return s2; };
        // spawn where you're LOOKING (surface hit), else 4m in front of the camera
        float anchor[3];
        if (!cameraForwardHit(anchor)) { Camera& c=r->cam; float cp=std::cos(c.pitch);
            anchor[0]=c.pos[0]+std::sin(c.yaw)*cp*4.f; anchor[1]=c.pos[1]+std::sin(c.pitch)*4.f; anchor[2]=c.pos[2]-std::cos(c.yaw)*cp*4.f; }
        u32 count=r32(); std::vector<int> made;
        static const float I16[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (u32 gi=0; gi<count && gi<256; ++gi){
            MeshData md;
            md.name = rstr();
            u8 flags[4]; fread(flags,1,4,f);
            md.useBlend=flags[0]!=0; md.additive=flags[1]!=0; md.alphaTest=flags[2]!=0; md.doubleSided=flags[3]!=0;
            md.texW=r32(); md.texH=r32();
            u32 pn=r32();
            if (pn){ std::vector<u8> png(pn); fread(png.data(),1,pn,f);
                int w2=0,h2=0,n3=0; unsigned char* px=stbi_load_from_memory(png.data(),(int)pn,&w2,&h2,&n3,4);
                if (px){ md.texRGBA.assign(px,px+(size_t)w2*h2*4); md.texW=(u32)w2; md.texH=(u32)h2; md.hasTexture=true; stbi_image_free(px); } }
            u32 n2; n2=r32(); md.positions.resize(n2); if(n2) fread(md.positions.data(),4,n2,f);
            n2=r32(); md.uvs.resize(n2); if(n2) fread(md.uvs.data(),4,n2,f);
            n2=r32(); md.indices.resize(n2); if(n2) fread(md.indices.data(),4,n2,f);
            for (size_t v2=0;v2+2<md.positions.size();v2+=3){ md.positions[v2]+=anchor[0]; md.positions[v2+1]+=anchor[1]; md.positions[v2+2]+=anchor[2]; }
            md.transform=Transform{}; md.hasWorldMatrix=true; memcpy(md.worldMatrix,I16,sizeof I16);
            md.nVerts=(u32)(md.positions.size()/3); md.nIdx=(u32)md.indices.size();
            if (md.nVerts<3 || md.nIdx<3) continue;
            sceneMeshes->push_back(std::move(md));
            size_t ni=r->gpuMeshes.size();
            r->uploadMesh(sceneMeshes->back());
            if (r->gpuMeshes.size()!=ni+1){ sceneMeshes->pop_back(); continue; }
            meshEditLog[(int)ni]="prefab"; made.push_back((int)ni);
        }
        fclose(f);
        if (made.empty()) { setStatus("prefab: nothing spawned (empty/corrupt file?)"); return; }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        pushDeleteUndo(made, false);                          // Ctrl+Z removes the spawn
        geomDirty=true;
        sel=made; selected=made.back(); r->selectedMesh=selected; selItem=-1; scrollToSel=true;
        std::string bn=path; size_t sl=bn.find_last_of("/\\"); if (sl!=std::string::npos) bn=bn.substr(sl+1);
        setStatus("Spawned prefab '"+bn+"' ("+std::to_string(made.size())+" mesh(es)) where you were looking - move it with the gizmo (Ctrl+Z removes)");
    }

    // ── RE-UV FLAT: give the mesh ONE continuous planar UV sheet (0..1 across its bounds, dominant
    //    plane by area-weighted normal). After fusing floor+patch their UVs disagree (atlas cells vs
    //    0..1 sheet) so a Set texture shows at different scales - Re-UV first and the texture spans
    //    ONCE, seamlessly, across everything. Pairs with: Fuse -> Re-UV flat -> Set texture (AI image).
    void reUVFlat(int mi){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        size_t nv = md.positions.size()/3;
        if (nv<3) return;
        // area-weighted normal over all triangles -> dominant projection plane
        float nx=0,ny=0,nz=0;
        for (size_t k=0;k+2<md.indices.size();k+=3){
            const float* A=&md.positions[md.indices[k]*3]; const float* B=&md.positions[md.indices[k+1]*3]; const float* C=&md.positions[md.indices[k+2]*3];
            float e1[3]={B[0]-A[0],B[1]-A[1],B[2]-A[2]}, e2[3]={C[0]-A[0],C[1]-A[1],C[2]-A[2]};
            nx+=e1[1]*e2[2]-e1[2]*e2[1]; ny+=e1[2]*e2[0]-e1[0]*e2[2]; nz+=e1[0]*e2[1]-e1[1]*e2[0];
        }
        int dom = std::fabs(nx)>std::fabs(ny) ? (std::fabs(nx)>std::fabs(nz)?0:2) : (std::fabs(ny)>std::fabs(nz)?1:2);
        int ua=(dom+1)%3, va=(dom+2)%3;
        float mnu=1e30f,mxu=-1e30f,mnv=1e30f,mxv=-1e30f;
        for (size_t v2=0;v2<nv;++v2){ float a=md.positions[v2*3+ua], b=md.positions[v2*3+va];
            mnu=std::min(mnu,a); mxu=std::max(mxu,a); mnv=std::min(mnv,b); mxv=std::max(mxv,b); }
        float su=(mxu-mnu)>1e-4f?(mxu-mnu):1.f, sv=(mxv-mnv)>1e-4f?(mxv-mnv):1.f;
        md.uvs.resize(nv*2);
        for (size_t v2=0;v2<nv;++v2){ md.uvs[v2*2]=(md.positions[v2*3+ua]-mnu)/su; md.uvs[v2*2+1]=(md.positions[v2*3+va]-mnv)/sv; }
        md.uvs2.clear(); md.hasLightmap=false; md.lmRGBA.clear();   // old lightmap unwrap no longer applies
        md.astcRaw.clear();
        if (commitGeomEdit(mi, std::move(md), "reUV"))
            setStatus("Re-UV'd to ONE flat 0..1 sheet - Set texture now spans the whole surface once (Ctrl+Z reverts)");
    }

    // ── SLICE by an ARBITRARY world plane (n . p = pd): every triangle goes LEFT or RIGHT by world
    //    centroid (triangles are not split at the plane - fine for prop separation). Both halves keep
    //    the texture/UVs; original goes to history (one Ctrl+Z un-slices). Used by the axis buttons
    //    AND the 2-pin cut line ("draw a line -> split in two").
    void sliceMeshByPlane(int mi, const float pn[3], float pd){
        // plane -> MODEL-space per-vertex signed distance -> the shared fence splitter below
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        const MeshData& src=(*sceneMeshes)[(size_t)mi];
        const float* M=r->gpuMeshes[(size_t)mi].model;
        size_t nv=src.positions.size()/3;
        if (nv<3 || src.indices.size()<6) return;
        float A2 = pn[0]*M[0] + pn[1]*M[1]  + pn[2]*M[2];
        float B2 = pn[0]*M[4] + pn[1]*M[5]  + pn[2]*M[6];
        float C2 = pn[0]*M[8] + pn[1]*M[9]  + pn[2]*M[10];
        float E2 = pn[0]*M[12]+ pn[1]*M[13] + pn[2]*M[14] - pd;
        std::vector<float> sdv(nv);
        for (size_t v2=0;v2<nv;++v2){ const float* p=&src.positions[v2*3]; sdv[v2]=A2*p[0]+B2*p[1]+C2*p[2]+E2; }
        sliceMeshBySD(mi, sdv, "slice");
    }
    // ── generalized splitter: split a mesh in two along the ZERO SET of a per-vertex signed distance
    //    field — a single plane (line knife / slice gizmo) or the KNIFE PATH's piecewise polyline fence.
    //    Crossing triangles are clipped EXACTLY at the sd=0 crossings (positions + UVs lerped), both
    //    halves become their own meshes, the original goes to history.
    bool sliceMeshBySD(int mi, const std::vector<float>& sdv, const char* what){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return false;
        const MeshData& src=(*sceneMeshes)[(size_t)mi];
        size_t nv=src.positions.size()/3;
        if (nv<3 || src.indices.size()<6 || sdv.size()<nv) return false;
        MeshData half[2]; std::map<uint32_t,uint32_t> rm[2];
        bool hasUV=src.uvs.size()>=nv*2, hasUV2=src.uvs2.size()>=nv*2, hasCol=src.colors.size()>=nv*4;
        for (int h2=0;h2<2;++h2){ half[h2]=src;
            half[h2].positions.clear(); half[h2].uvs.clear(); half[h2].uvs2.clear(); half[h2].indices.clear();
            half[h2].colors.clear(); half[h2].uvs3.clear(); half[h2].uvs4.clear();
            half[h2].boneIndices.clear(); half[h2].boneWeights.clear(); half[h2].hasBones=false; }
        // ── TRUE PLANE CLIPPING (the ACCURATE slice): triangles crossing the plane are SPLIT at it.
        //    New vertices land EXACTLY on the cut (positions + UVs lerped by the crossing fraction),
        //    so the edge is a perfectly straight line and the texture is continuous across it.
        (void)hasCol;   // per-vertex colors are dropped by the halves (can't survive re-indexing cleanly)
        auto SD=[&](uint32_t v2)->float{ return sdv[v2]; };
        std::map<std::pair<uint32_t,uint32_t>,uint32_t> cutRm[2];   // shared cut-verts per half (edge-keyed - watertight cut line)
        auto origV=[&](int h2, uint32_t v2)->uint32_t{
            auto& pt=half[h2]; auto& rmm=rm[h2];
            auto it=rmm.find(v2); if (it!=rmm.end()) return it->second;
            uint32_t nvi=(uint32_t)(pt.positions.size()/3);
            pt.positions.push_back(src.positions[v2*3]); pt.positions.push_back(src.positions[v2*3+1]); pt.positions.push_back(src.positions[v2*3+2]);
            if (hasUV) { pt.uvs.push_back(src.uvs[v2*2]);  pt.uvs.push_back(src.uvs[v2*2+1]); }
            if (hasUV2){ pt.uvs2.push_back(src.uvs2[v2*2]); pt.uvs2.push_back(src.uvs2[v2*2+1]); }
            rmm[v2]=nvi; return nvi;
        };
        auto cutV=[&](int h2, uint32_t va, uint32_t vb)->uint32_t{
            auto key = va<vb?std::make_pair(va,vb):std::make_pair(vb,va);
            auto& crm=cutRm[h2];
            auto it=crm.find(key); if (it!=crm.end()) return it->second;
            float sa=SD(va), sb=SD(vb); float t=sa/(sa-sb);          // crossing fraction along the edge
            t=std::clamp(t,0.f,1.f);
            auto& pt=half[h2];
            uint32_t nvi=(uint32_t)(pt.positions.size()/3);
            for (int c2=0;c2<3;++c2) pt.positions.push_back(src.positions[va*3+c2]+(src.positions[vb*3+c2]-src.positions[va*3+c2])*t);
            if (hasUV)  for (int c2=0;c2<2;++c2) pt.uvs.push_back(src.uvs[va*2+c2]+(src.uvs[vb*2+c2]-src.uvs[va*2+c2])*t);
            if (hasUV2) for (int c2=0;c2<2;++c2) pt.uvs2.push_back(src.uvs2[va*2+c2]+(src.uvs2[vb*2+c2]-src.uvs2[va*2+c2])*t);
            crm[key]=nvi; return nvi;
        };
        const float EPS=1e-5f;
        for (size_t k=0;k+2<src.indices.size();k+=3){
            uint32_t tri[3]={src.indices[k],src.indices[k+1],src.indices[k+2]};
            float sd[3]={SD(tri[0]),SD(tri[1]),SD(tri[2])};
            bool anyNeg=sd[0]<-EPS||sd[1]<-EPS||sd[2]<-EPS, anyPos=sd[0]>EPS||sd[1]>EPS||sd[2]>EPS;
            if (!anyPos){ // whole tri -> half 0
                auto& pt=half[0]; for (int e=0;e<3;++e) pt.indices.push_back(origV(0,tri[e])); continue; }
            if (!anyNeg){ // whole tri -> half 1
                auto& pt=half[1]; for (int e=0;e<3;++e) pt.indices.push_back(origV(1,tri[e])); continue; }
            // CROSSING: Sutherland-Hodgman clip against the plane, once per side
            for (int h2=0;h2<2;++h2){
                float sign = h2==0 ? -1.f : 1.f;                     // keep sd*sign >= 0
                uint32_t poly[4]; int pn2=0;
                for (int e=0;e<3;++e){
                    uint32_t va=tri[e], vb=tri[(e+1)%3];
                    float da=sd[e]*sign, db=sd[(e+1)%3]*sign;
                    if (da>=-EPS) poly[pn2++]=origV(h2,va);
                    if ((da>EPS&&db<-EPS)||(da<-EPS&&db>EPS)) poly[pn2++]=cutV(h2,va,vb);
                }
                auto& pt=half[h2];
                for (int e=2;e<pn2;++e){ pt.indices.push_back(poly[0]); pt.indices.push_back(poly[e-1]); pt.indices.push_back(poly[e]); }
            }
        }
        if (half[0].indices.empty() || half[1].indices.empty()) { setStatus(std::string(what)+": the cut didn't cross this mesh (everything on one side)"); return false; }
        const VkGpuMesh og = r->gpuMeshes[(size_t)mi];
        std::vector<int> hist{mi}; std::vector<uint8_t> histA{1};
        int made=0;
        for (int h2=0;h2<2;++h2){
            half[h2].nVerts=(u32)(half[h2].positions.size()/3); half[h2].nIdx=(u32)half[h2].indices.size();
            sceneMeshes->push_back(std::move(half[h2]));
            size_t ni=r->gpuMeshes.size();
            r->uploadMesh(sceneMeshes->back());
            if (r->gpuMeshes.size()!=ni+1){ sceneMeshes->pop_back(); continue; }
            VkGpuMesh& ng=r->gpuMeshes[ni];
            memcpy(ng.editT,og.editT,sizeof ng.editT); memcpy(ng.editR,og.editR,sizeof ng.editR); memcpy(ng.editS,og.editS,sizeof ng.editS);
            memcpy(ng.editTint,og.editTint,sizeof ng.editTint); recomputeModel(ng);
            hist.push_back((int)ni); histA.push_back(0);
            meshEditLog[(int)ni] = (meshEditLog.count(mi)?meshEditLog[mi]+"+":std::string()) + what;
            ++made;
        }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        r->setDeleted((size_t)mi, true); pushDeleteUndo(hist, histA);
        remapMeshComponents(mi, std::vector<int>(hist.begin()+1, hist.end()));   // colliders follow the halves
        // SELECT the pieces (was deselectAll): a knife/slice on a mesh whose halves stay exactly in place is
        // visually a NO-OP ("tried cutting a sky cloud, it didn't even cut it") — highlighting the two new
        // halves is the visible proof the cut happened, and move/Del acts on them immediately.
        sel.clear(); selItems.clear();
        for (size_t k=1;k<hist.size();++k) sel.push_back(hist[k]);
        selected = sel.empty() ? -1 : sel.front(); r->selectedMesh = selected;
        holesMesh=-1; geomDirty = true;
        setStatus("Cut in two ("+std::to_string(made)+" parts, texture kept, both SELECTED) - move/delete each; Ctrl+Z un-cuts");
        return made>0;
    }
    // axis slice = plane through the mesh's world center, normal along the axis
    void sliceMesh(int mi, int axis){
        if (!r || mi<0 || mi>=(int)r->gpuMeshes.size()) return;
        float mn[3],mx[3]; worldAabb(r->gpuMeshes[(size_t)mi],mn,mx);
        float pn[3]={0,0,0}; pn[axis]=1.f;
        sliceMeshByPlane(mi, pn, (mn[axis]+mx[axis])*0.5f);
    }

    // ── KNIFE: draw a plain 2D LINE across the screen - on release the selection is cut along it
    //    (the cutting plane goes through the camera and the drawn line, so the cut lands EXACTLY
    //    under the line you see). No 3D widget, no gizmo - just drag a line over the mesh.
    bool  knifeOn = false, knifeDrag = false;
    bool  knifeBox = false;        // KNIFE BOX sub-mode: drag a RECTANGLE = 3D cut (frustum + optional depth)
    float knifeCutDepth = 0.f;     // box-cut depth from the FIRST surface hit; 0 = cut THROUGH everything
    float knifeA[2] = {0,0};
    void startKnife(){
        // no selection needed - the knife cuts whatever mesh the line is DRAWN OVER; with a selection
        // it cuts the SELECTED mesh(es) the plane crosses (occluders in front can't steal the cut)
        patchMode=false; pinIsCut=false; pinIsBend=false; pinIsKnife=false; sliceGizmoOn=false; patchPts.clear(); patchCols.clear();
        knifeOn=true; knifeDrag=false; knifeBox=false;
        setStatus(sel.empty() ? "KNIFE: drag a straight line ACROSS a mesh - release cuts that mesh exactly under the line (Esc cancels)"
                              : "KNIFE: drag a line - release cuts the SELECTED mesh(es) along it, even behind occluders (Esc cancels)");
    }
    void startKnifeBox(){
        patchMode=false; pinIsCut=false; pinIsBend=false; pinIsKnife=false; sliceGizmoOn=false; patchPts.clear(); patchCols.clear();
        knifeOn=true; knifeDrag=false; knifeBox=true;
        char b2[160]; snprintf(b2,sizeof b2, "KNIFE BOX: drag a RECTANGLE - release splits the %s into INSIDE + OUTSIDE pieces (depth %.2f%s; Esc cancels)",
                               sel.empty()?"mesh under it":"SELECTED mesh(es)", knifeCutDepth, knifeCutDepth>0.001f?"m from the first surface":" = through");
        setStatus(b2);
    }
    void applyKnife(float ax, float ay, float bx, float by){
        knifeOn=false; knifeDrag=false;
        float dx2=bx-ax, dy2=by-ay;
        if (dx2*dx2+dy2*dy2 < 100.f) { setStatus("knife: line too short - drag a longer stroke"); return; }
        // plane through the CAMERA EYE and the two unprojected rays of the line endpoints
        float W=(float)rcViewport.extent.width, H=(float)rcViewport.extent.height; if (W<=0||H<=0) return;
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp); float inv[16]; if (!invertMat4(vp, inv)) return;
        auto ray=[&](float sx, float sy, float d[3]){
            float ndcx=2.f*(sx-rcViewport.offset.x)/W-1.f, ndcy=2.f*(sy-rcViewport.offset.y)/H-1.f;
            float O2[3],F2[3]; unproject(inv,ndcx,ndcy,1.f,O2); unproject(inv,ndcx,ndcy,0.f,F2);
            d[0]=F2[0]-O2[0]; d[1]=F2[1]-O2[1]; d[2]=F2[2]-O2[2];
            float l=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); if (l>1e-6f){ d[0]/=l; d[1]/=l; d[2]/=l; } };
        float dA[3], dB[3]; ray(ax,ay,dA); ray(bx,by,dB);
        float n[3]={ dA[1]*dB[2]-dA[2]*dB[1], dA[2]*dB[0]-dA[0]*dB[2], dA[0]*dB[1]-dA[1]*dB[0] };
        float nl=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nl<1e-6f) { setStatus("knife: degenerate line"); return; }
        n[0]/=nl; n[1]/=nl; n[2]/=nl;
        float pd = n[0]*r->cam.pos[0]+n[1]*r->cam.pos[1]+n[2]*r->cam.pos[2];
        // Cut targets: with a SELECTION the stroke only defines the CUTTING PLANE — every selected mesh
        // the plane actually crosses gets cut, even when it sits partially BEHIND other meshes (the old
        // screen-pick gate let occluders steal the hit = "the line didn't cross the SELECTED mesh" on a
        // mesh plainly under the stroke). With NO selection it cuts just the one mesh the stroke is
        // mostly drawn over (what you see, not everything hidden along the ray path).
        std::set<int> targets;
        if (!sel.empty()) {
            for (int s2 : sel) {
                if (s2<0 || s2>=(int)r->gpuMeshes.size() || r->isDeleted((size_t)s2)) continue;
                float mn3[3],mx3[3]; worldAabb(r->gpuMeshes[(size_t)s2],mn3,mx3);
                float c[3]={(mn3[0]+mx3[0])*0.5f,(mn3[1]+mx3[1])*0.5f,(mn3[2]+mx3[2])*0.5f};
                float e[3]={(mx3[0]-mn3[0])*0.5f,(mx3[1]-mn3[1])*0.5f,(mx3[2]-mn3[2])*0.5f};
                float dist=n[0]*c[0]+n[1]*c[1]+n[2]*c[2]-pd;                                  // center to plane
                float rad =std::fabs(n[0])*e[0]+std::fabs(n[1])*e[1]+std::fabs(n[2])*e[2];    // AABB extent along the normal
                if (std::fabs(dist)<=rad) targets.insert(s2);   // the plane crosses this mesh -> cut it
            }
            if (targets.empty()) {
                // The stroke plane misses every SELECTED mesh (stale leftover selection / line drawn over
                // something else): fall back to cutting the mesh actually UNDER the line — never a silent
                // no-op (the "tried cutting a sky cloud, it didn't even cut it" trap: an old selection ate
                // the stroke and only a status line said so).
                std::map<int,int> hits;
                for (int i2=0;i2<=24;++i2){ float t=(float)i2/24.f;
                    int hit = pickIndex(ax+(bx-ax)*t, ay+(by-ay)*t);
                    if (hit>=0) hits[hit]++; }
                int best=-1, bestN=0; for (auto& kv : hits) if (kv.second>bestN){ best=kv.first; bestN=kv.second; }
                if (best<0) { setStatus("knife: the cut plane misses the SELECTED mesh(es) and no mesh is under the line"); return; }
                targets.insert(best);
            }
        } else {
            // no selection: cut the ONE mesh the stroke is mostly over (most stroke samples), not every
            // neighbor the ends of the line graze; 24 samples so thin/narrow meshes register too
            std::map<int,int> hits;
            for (int i2=0;i2<=24;++i2){ float t=(float)i2/24.f;
                int hit = pickIndex(ax+(bx-ax)*t, ay+(by-ay)*t);
                if (hit>=0) hits[hit]++; }
            if (hits.empty()) { setStatus("knife: the line didn't cross any mesh"); return; }
            int best=-1, bestN=0; for (auto& kv : hits) if (kv.second>bestN){ best=kv.first; bestN=kv.second; }
            targets.insert(best);
        }
        int done=0;
        for (int m : targets) { sliceMeshByPlane(m, n, pd); ++done; }   // EXACT triangle-splitting cut
        setStatus("Knife cut "+std::to_string(done)+" mesh(es) "+(sel.empty()?"under the line":"(the selection)")+" - Ctrl+Z un-cuts");
    }
    // ── 3D BOX CUT core: partition ONE mesh into the part INSIDE a convex volume (4 frustum planes
    //    from the dragged rectangle + optional near/far depth planes) and the part OUTSIDE. Triangles
    //    crossing the boundary are clipped EXACTLY (positions + UVs lerped at the crossing), so the cut
    //    walls are perfectly straight and the texture continuous. Both parts become their own meshes
    //    (same material/texture, edit transform carried); the original goes to history (Ctrl+Z).
    //    Planes are WORLD space {nx,ny,nz,d}, normal pointing INTO the volume: inside = n.p - d >= 0.
    bool boxCutMesh(int mi, const float (*planes)[4], int nPlanes, int& insideIdx){
        insideIdx=-1;
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return false;
        const MeshData& src=(*sceneMeshes)[(size_t)mi];
        const VkGpuMesh& gm=r->gpuMeshes[(size_t)mi];
        if (gm.isSkinned || gm.dynamicVerts) { setStatus("box cut: skinned/animated meshes can't be cut"); return false; }
        const float* M=gm.model;
        size_t nv=src.positions.size()/3;
        if (nv<3 || src.indices.size()<3) return false;
        bool hasUV=src.uvs.size()>=nv*2, hasUV2=src.uvs2.size()>=nv*2;
        // planes -> MODEL space: f(p) = A*x+B*y+C*z+E == n.p_world - d
        std::vector<std::array<float,4>> lp((size_t)nPlanes);
        for (int i=0;i<nPlanes;++i){ const float* P=planes[i];
            lp[(size_t)i][0]=P[0]*M[0] +P[1]*M[1] +P[2]*M[2];
            lp[(size_t)i][1]=P[0]*M[4] +P[1]*M[5] +P[2]*M[6];
            lp[(size_t)i][2]=P[0]*M[8] +P[1]*M[9] +P[2]*M[10];
            lp[(size_t)i][3]=P[0]*M[12]+P[1]*M[13]+P[2]*M[14]-P[3]; }
        struct CV { float p[3], uv[2], uv2[2]; };
        auto sdOf=[&](const CV& v, const std::array<float,4>& q){ return q[0]*v.p[0]+q[1]*v.p[1]+q[2]*v.p[2]+q[3]; };
        auto lerpCV=[&](const CV& a, const CV& b, float t){ CV o;
            for (int c2=0;c2<3;++c2) o.p[c2]=a.p[c2]+(b.p[c2]-a.p[c2])*t;
            for (int c2=0;c2<2;++c2){ o.uv[c2]=a.uv[c2]+(b.uv[c2]-a.uv[c2])*t; o.uv2[c2]=a.uv2[c2]+(b.uv2[c2]-a.uv2[c2])*t; }
            return o; };
        const float EPS=1e-5f;
        auto clip=[&](const std::vector<CV>& in2, const std::array<float,4>& q, float sign, std::vector<CV>& out2){
            out2.clear(); size_t m=in2.size();
            for (size_t e=0;e<m;++e){ const CV& a=in2[e]; const CV& b=in2[(e+1)%m];
                float da=sdOf(a,q)*sign, db=sdOf(b,q)*sign;
                if (da>=-EPS) out2.push_back(a);
                if ((da>EPS&&db<-EPS)||(da<-EPS&&db>EPS)) out2.push_back(lerpCV(a,b,da/(da-db))); } };
        MeshData piece[2];   // [0]=OUTSIDE  [1]=INSIDE
        for (int h2=0;h2<2;++h2){ piece[h2]=src;
            piece[h2].positions.clear(); piece[h2].uvs.clear(); piece[h2].uvs2.clear(); piece[h2].indices.clear();
            piece[h2].colors.clear(); piece[h2].uvs3.clear(); piece[h2].uvs4.clear();
            piece[h2].boneIndices.clear(); piece[h2].boneWeights.clear(); piece[h2].hasBones=false; }
        std::map<std::tuple<int,int,int,int,int>,uint32_t> vmap[2];   // bit-exact weld so the cut stays watertight
        auto emitPoly=[&](int h2, const std::vector<CV>& poly){
            if (poly.size()<3) return;
            auto& pt=piece[h2]; auto& vm=vmap[h2];
            auto vidx=[&](const CV& v)->uint32_t{
                auto bi=[&](float f){ int i2; memcpy(&i2,&f,4); return i2; };
                auto key=std::make_tuple(bi(v.p[0]),bi(v.p[1]),bi(v.p[2]),bi(v.uv[0]),bi(v.uv[1]));
                auto it=vm.find(key); if (it!=vm.end()) return it->second;
                uint32_t nvi=(uint32_t)(pt.positions.size()/3);
                pt.positions.push_back(v.p[0]); pt.positions.push_back(v.p[1]); pt.positions.push_back(v.p[2]);
                if (hasUV) { pt.uvs.push_back(v.uv[0]);  pt.uvs.push_back(v.uv[1]); }
                if (hasUV2){ pt.uvs2.push_back(v.uv2[0]); pt.uvs2.push_back(v.uv2[1]); }
                vm.emplace(key,nvi); return nvi; };
            uint32_t v0=vidx(poly[0]);
            for (size_t e=2;e<poly.size();++e){ pt.indices.push_back(v0); pt.indices.push_back(vidx(poly[e-1])); pt.indices.push_back(vidx(poly[e])); } };
        std::vector<CV> cur, nxt, outPart;
        for (size_t k=0;k+2<src.indices.size();k+=3){
            cur.clear();
            for (int e=0;e<3;++e){ uint32_t v2=src.indices[k+e]; CV c{};
                memcpy(c.p,&src.positions[v2*3],12);
                if (hasUV) { c.uv[0]=src.uvs[v2*2];   c.uv[1]=src.uvs[v2*2+1]; }
                if (hasUV2){ c.uv2[0]=src.uvs2[v2*2]; c.uv2[1]=src.uvs2[v2*2+1]; }
                cur.push_back(c); }
            bool alive=true;
            for (int i=0;i<nPlanes && alive;++i){
                clip(cur, lp[(size_t)i], -1.f, outPart);   // slab OUTSIDE plane i (still inside planes 0..i-1)
                emitPoly(0, outPart);
                clip(cur, lp[(size_t)i], +1.f, nxt);       // continue clipping with the inside remainder
                cur.swap(nxt);
                alive = cur.size()>=3;
            }
            if (alive) emitPoly(1, cur);
        }
        if (piece[1].indices.empty()){ setStatus("box cut: the box missed the mesh"); return false; }
        if (piece[0].indices.empty()){ setStatus("box cut: the WHOLE mesh is inside the box - nothing to cut"); return false; }
        const VkGpuMesh og=gm;   // copy - gpuMeshes reallocates as pieces upload
        std::vector<int> hist{mi}; std::vector<uint8_t> histA{1};
        int made=0;
        for (int h2=0;h2<2;++h2){
            piece[h2].nVerts=(u32)(piece[h2].positions.size()/3); piece[h2].nIdx=(u32)piece[h2].indices.size();
            sceneMeshes->push_back(std::move(piece[h2]));
            size_t ni=r->gpuMeshes.size();
            r->uploadMesh(sceneMeshes->back());
            if (r->gpuMeshes.size()!=ni+1){ sceneMeshes->pop_back(); continue; }
            VkGpuMesh& ng=r->gpuMeshes[ni];
            memcpy(ng.editT,og.editT,sizeof ng.editT); memcpy(ng.editR,og.editR,sizeof ng.editR); memcpy(ng.editS,og.editS,sizeof ng.editS);
            memcpy(ng.editTint,og.editTint,sizeof ng.editTint); recomputeModel(ng);
            hist.push_back((int)ni); histA.push_back(0);
            meshEditLog[(int)ni] = (meshEditLog.count(mi)?meshEditLog[mi]+"+":std::string()) + (h2?"boxcut-in":"boxcut-out");
            if (h2) insideIdx=(int)ni;
            ++made;
        }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        r->setDeleted((size_t)mi, true); pushDeleteUndo(hist, histA);
        remapMeshComponents(mi, std::vector<int>(hist.begin()+1, hist.end()));   // colliders follow inside+outside pieces
        holesMesh=-1; geomDirty=true;
        return made==2;
    }
    // ── KNIFE PATH apply ("a free 3D knife set by LINES"): the clicked pins form a POLYLINE; each
    //    segment + the camera eye defines a cutting plane, so the cut lands exactly under the drawn
    //    path - but unlike the straight line knife the path can have corners and hug what you traced.
    //    Every mesh vertex takes the signed distance of the polyline segment NEAREST to it (3D), and
    //    the generalized splitter cuts along that piecewise fence. Targets = the SELECTION (pins are
    //    already selection-locked); with no selection, the mesh(es) the pins actually sit on. Nothing
    //    is deleted - the targets split in two along the line (Ctrl+Z per mesh un-cuts).
    void applyKnifePath(){
        if (!r || patchPts.size()<2){ setStatus("knife path: click at least 2 points along the cut line first (Enter cuts)"); return; }
        float O[3]={r->cam.pos[0],r->cam.pos[1],r->cam.pos[2]};
        struct Seg { float n[3], d; float a[3], b[3]; };
        std::vector<Seg> segs;
        for (size_t i2=0;i2+1<patchPts.size();++i2){
            const auto& A=patchPts[i2]; const auto& B=patchPts[i2+1];
            float u[3]={A[0]-O[0],A[1]-O[1],A[2]-O[2]}, v[3]={B[0]-O[0],B[1]-O[1],B[2]-O[2]};
            float n2[3]={u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
            float l=std::sqrt(n2[0]*n2[0]+n2[1]*n2[1]+n2[2]*n2[2]); if (l<1e-9f) continue;
            n2[0]/=l; n2[1]/=l; n2[2]/=l;
            if (!segs.empty()){ const Seg& pr=segs.back();          // consistent LEFT/RIGHT along the whole path
                if (n2[0]*pr.n[0]+n2[1]*pr.n[1]+n2[2]*pr.n[2] < 0){ n2[0]=-n2[0]; n2[1]=-n2[1]; n2[2]=-n2[2]; } }
            Seg s; memcpy(s.n,n2,12); s.d=n2[0]*O[0]+n2[1]*O[1]+n2[2]*O[2];
            memcpy(s.a,A.data(),12); memcpy(s.b,B.data(),12);
            segs.push_back(s);
        }
        if (segs.empty()){ setStatus("knife path: the clicked points are on top of each other"); return; }
        std::vector<int> targets;
        if (!sel.empty()) targets=sel;
        else { std::set<int> t2;                                    // no selection: the meshes the pins sit on
               for (auto& p : patchPts){ float w2[3]={p[0],p[1],p[2]}; float sx,sy;
                   if (worldToScreen(w2,sx,sy)){ int hit=pickIndex(sx,sy); if (hit>=0) t2.insert(hit); } }
               targets.assign(t2.begin(),t2.end()); }
        if (targets.empty()){ setStatus("knife path: no target - select the mesh(es) or pin ON a mesh"); return; }
        auto segDist2=[&](const Seg& s, const float p[3])->float{   // squared point-to-SEGMENT distance (3D)
            float ab[3]={s.b[0]-s.a[0],s.b[1]-s.a[1],s.b[2]-s.a[2]};
            float ap[3]={p[0]-s.a[0],p[1]-s.a[1],p[2]-s.a[2]};
            float ll=ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2];
            float t=ll>1e-12f? std::clamp((ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])/ll,0.f,1.f) : 0.f;
            float q[3]={s.a[0]+ab[0]*t-p[0], s.a[1]+ab[1]*t-p[1], s.a[2]+ab[2]*t-p[2]};
            return q[0]*q[0]+q[1]*q[1]+q[2]*q[2]; };
        int done=0;
        for (int m : targets){
            if (m<0||m>=(int)sceneMeshes->size()||m>=(int)r->gpuMeshes.size()||r->isDeleted((size_t)m)) continue;
            if (r->gpuMeshes[(size_t)m].isSkinned || r->gpuMeshes[(size_t)m].dynamicVerts) continue;
            const MeshData& md=(*sceneMeshes)[(size_t)m]; const float* M=r->gpuMeshes[(size_t)m].model;
            size_t nv=md.positions.size()/3; if (nv<3) continue;
            std::vector<float> sdv(nv);
            for (size_t v2=0;v2<nv;++v2){
                const float* p=&md.positions[v2*3];
                float wp[3]={M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12],
                             M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13],
                             M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]};
                size_t best=0; float bestD=1e30f;
                for (size_t s2=0;s2<segs.size();++s2){ float d2=segDist2(segs[s2],wp); if (d2<bestD){ bestD=d2; best=s2; } }
                const Seg& S=segs[best];
                sdv[v2]=S.n[0]*wp[0]+S.n[1]*wp[1]+S.n[2]*wp[2]-S.d;
            }
            if (sliceMeshBySD(m, sdv, "knifepath")) ++done;
        }
        patchPts.clear(); patchCols.clear(); patchMode=false; pinIsKnife=false;
        setStatus(done ? ("KNIFE PATH cut "+std::to_string(done)+" mesh(es) along the "+std::to_string(segs.size())+"-segment line - Ctrl+Z un-cuts")
                       : "knife path: the line didn't cross the target mesh(es)");
    }
    // ── KNIFE BOX apply: the dragged rectangle -> 4 inward frustum planes through the camera (+ near/far
    //    depth planes when Cut depth > 0, measured from the FIRST surface the rectangle's rays hit). The
    //    cut is fully 3D-bounded: width and height by the rectangle, depth by the depth value. Targets =
    //    the SELECTED mesh(es) the volume touches; with no selection, the one mesh under the rectangle.
    void applyKnifeBox(float ax, float ay, float bx, float by){
        knifeOn=false; knifeDrag=false; knifeBox=false;
        float x0=std::min(ax,bx), x1=std::max(ax,bx), y0=std::min(ay,by), y1=std::max(ay,by);
        if (x1-x0<8.f || y1-y0<8.f){ setStatus("box cut: rectangle too small - drag a bigger box"); return; }
        float W=(float)rcViewport.extent.width, H=(float)rcViewport.extent.height; if (W<=0||H<=0) return;
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp); float inv[16]; if (!invertMat4(vp, inv)) return;
        auto ray=[&](float sx, float sy, float d[3]){
            float ndcx=2.f*(sx-rcViewport.offset.x)/W-1.f, ndcy=2.f*(sy-rcViewport.offset.y)/H-1.f;
            float O2[3],F2[3]; unproject(inv,ndcx,ndcy,1.f,O2); unproject(inv,ndcx,ndcy,0.f,F2);
            d[0]=F2[0]-O2[0]; d[1]=F2[1]-O2[1]; d[2]=F2[2]-O2[2];
            float l=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); if (l>1e-6f){ d[0]/=l; d[1]/=l; d[2]/=l; } };
        float dTL[3],dTR[3],dBR[3],dBL[3],dC[3];
        ray(x0,y0,dTL); ray(x1,y0,dTR); ray(x1,y1,dBR); ray(x0,y1,dBL);
        ray((x0+x1)*0.5f,(y0+y1)*0.5f,dC);
        float eye[3]={r->cam.pos[0],r->cam.pos[1],r->cam.pos[2]};
        float planes[6][4]; int np=0;
        auto side=[&](const float* dA, const float* dB){
            float n2[3]={dA[1]*dB[2]-dA[2]*dB[1], dA[2]*dB[0]-dA[0]*dB[2], dA[0]*dB[1]-dA[1]*dB[0]};
            float l=std::sqrt(n2[0]*n2[0]+n2[1]*n2[1]+n2[2]*n2[2]); if (l<1e-9f) return;
            n2[0]/=l; n2[1]/=l; n2[2]/=l;
            if (n2[0]*dC[0]+n2[1]*dC[1]+n2[2]*dC[2] < 0){ n2[0]=-n2[0]; n2[1]=-n2[1]; n2[2]=-n2[2]; }   // point INWARD
            planes[np][0]=n2[0]; planes[np][1]=n2[1]; planes[np][2]=n2[2];
            planes[np][3]=n2[0]*eye[0]+n2[1]*eye[1]+n2[2]*eye[2]; ++np; };
        side(dTL,dTR); side(dTR,dBR); side(dBR,dBL); side(dBL,dTL);
        if (np<4){ setStatus("box cut: degenerate rectangle"); return; }
        // targets: SELECTION first (never the rest of the world); no selection = mesh under the rectangle
        std::set<int> targets;
        if (!sel.empty()){
            for (int s2 : sel){
                if (s2<0 || s2>=(int)r->gpuMeshes.size() || r->isDeleted((size_t)s2)) continue;
                float mn3[3],mx3[3]; worldAabb(r->gpuMeshes[(size_t)s2],mn3,mx3);
                bool outAny=false;
                for (int i=0;i<np && !outAny;++i){ const float* P=planes[i];
                    float px=P[0]>0?mx3[0]:mn3[0], py=P[1]>0?mx3[1]:mn3[1], pz=P[2]>0?mx3[2]:mn3[2];   // most-inside corner
                    if (P[0]*px+P[1]*py+P[2]*pz < P[3]) outAny=true; }
                if (!outAny) targets.insert(s2);
            }
            if (targets.empty()){ setStatus("box cut: the rectangle misses the SELECTED mesh(es)"); return; }
        } else {
            std::map<int,int> hits;
            for (int gy=0;gy<5;++gy) for (int gx=0;gx<5;++gx){
                int hit=pickIndex(x0+(x1-x0)*((float)gx+0.5f)/5.f, y0+(y1-y0)*((float)gy+0.5f)/5.f);
                if (hit>=0) hits[hit]++; }
            if (hits.empty()){ setStatus("box cut: no mesh under the rectangle"); return; }
            int best=-1,bestN=0; for (auto& kv:hits) if (kv.second>bestN){ best=kv.first; bestN=kv.second; }
            targets.insert(best);
        }
        // depth limit: near/far planes measured from the FIRST surface the rectangle's rays hit ON THE
        // TARGETS (so an occluder can't set the depth origin). 0 = cut through everything.
        if (knifeCutDepth>0.001f){
            float tNear=1e30f; float p[3]; u8 col[4];
            for (int gy=0;gy<5;++gy) for (int gx=0;gx<5;++gx){
                double sx=x0+(x1-x0)*((double)gx+0.5)/5.0, sy=y0+(y1-y0)*((double)gy+0.5)/5.0;
                for (int m : targets)
                    if (screenRayHitColor(sx,sy,m,p,col)){
                        float s=dC[0]*(p[0]-eye[0])+dC[1]*(p[1]-eye[1])+dC[2]*(p[2]-eye[2]);
                        if (s>0.f) tNear=std::min(tNear,s); }
            }
            if (tNear>=1e29f){ setStatus("box cut: no target surface under the rectangle to measure depth from"); return; }
            float e0=dC[0]*eye[0]+dC[1]*eye[1]+dC[2]*eye[2];
            planes[np][0]= dC[0]; planes[np][1]= dC[1]; planes[np][2]= dC[2];
            planes[np][3]= e0 + std::max(0.f, tNear-0.01f); ++np;                        // near: just before the first surface
            planes[np][0]=-dC[0]; planes[np][1]=-dC[1]; planes[np][2]=-dC[2];
            planes[np][3]=-(e0 + tNear + knifeCutDepth); ++np;                           // far: first surface + depth
        }
        int done=0; std::vector<int> insides;
        for (int m : targets){ int in2=-1; if (boxCutMesh(m, planes, np, in2)){ ++done; if (in2>=0) insides.push_back(in2); } }
        if (!done) return;                                        // per-mesh status already set
        selectOne(insides.empty()?-1:insides[0]);
        for (size_t i2=1;i2<insides.size();++i2) toggleSel(insides[(size_t)i2]);
        scrollToSel=true;
        char b2[200]; snprintf(b2,sizeof b2,"3D BOX CUT: %d mesh(es) split into INSIDE + OUTSIDE pieces (depth %s) - inside piece selected: Del removes it, Ctrl+Z un-cuts",
                               done, knifeCutDepth>0.001f?(std::to_string(knifeCutDepth).substr(0,4)+"m from the surface").c_str():"THROUGH");
        setStatus(b2);
    }
    void drawKnifeOverlay(){
        if (!knifeOn) return;
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        char hint[180];
        if (knifeBox) snprintf(hint,sizeof hint,"KNIFE BOX (3D) - drag a RECTANGLE over the %s, release = cut INSIDE piece out (depth %s; Esc cancels)",
                               sel.empty()?"mesh":"SELECTED mesh(es)", knifeCutDepth>0.001f?(std::to_string(knifeCutDepth).substr(0,4)+"m").c_str():"through");
        else snprintf(hint,sizeof hint,"%s", sel.empty() ? "KNIFE - drag a line across the mesh, release = cut (Esc cancels)"
                                                         : "KNIFE - drag a line, release = cut the SELECTED mesh(es) along it (Esc cancels)");
        cx.textAligned((float)rcViewport.offset.x, (float)rcViewport.offset.y+26*uiScale, (float)rcViewport.extent.width, 18*uiScale,
                       hint, ui::rgba(255,240,90), 1);
        if (knifeDrag){
            if (knifeBox){   // rectangle preview: 4 edges, black outline + yellow line
                float rx0=std::min(knifeA[0],cx.in.mx), rx1=std::max(knifeA[0],cx.in.mx);
                float ry0=std::min(knifeA[1],cx.in.my), ry1=std::max(knifeA[1],cx.in.my);
                auto edge=[&](float a0,float b0,float a1,float b1){
                    dl.line(a0,b0,a1,b1, ui::rgba(0,0,0,180), 5.f);
                    dl.line(a0,b0,a1,b1, ui::rgba(255,240,90), 2.5f); };
                edge(rx0,ry0,rx1,ry0); edge(rx1,ry0,rx1,ry1); edge(rx1,ry1,rx0,ry1); edge(rx0,ry1,rx0,ry0);
            } else {
                dl.line(knifeA[0],knifeA[1], cx.in.mx,cx.in.my, ui::rgba(0,0,0,180), 5.f);      // outline for contrast
                dl.line(knifeA[0],knifeA[1], cx.in.mx,cx.in.my, ui::rgba(255,240,90), 2.5f);    // THE line
                float hs2=5*uiScale;
                dl.rect(knifeA[0]-hs2,knifeA[1]-hs2,hs2*2,hs2*2, ui::rgba(255,240,90));
                dl.rect(cx.in.mx-hs2,cx.in.my-hs2,hs2*2,hs2*2, ui::rgba(255,240,90));
            }
        }
        dl.popClip();
    }

    // ── SLICE GIZMO: a VISIBLE cutting plane you place with the normal move/rotate gizmo. The exact
    //    cut line is highlighted LIVE on the mesh as you drag; Enter cuts (true triangle-splitting).
    //    Move = slide the plane, Rotate = turn it; works on the whole selection.
    bool  sliceGizmoOn = false;
    float slicePos[3] = {0,0,0};
    float sliceQuat[4] = {0,0,0,1};                        // plane normal = quat * world X
    void sliceNormal(float n[3]) const { float x[3]={1,0,0}; const_cast<Editor*>(this)->quatRotVec(sliceQuat, x, n); }
    void startSliceGizmo(const float* atWorld /*nullable*/){
        if (selected<0 || !r || selected>=(int)r->gpuMeshes.size()) { setStatus("slice gizmo: select the mesh(es) to cut first"); return; }
        patchMode=false; pinIsCut=false; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
        sliceGizmoOn = true;
        sliceQuat[0]=sliceQuat[1]=sliceQuat[2]=0; sliceQuat[3]=1;
        if (atWorld){ memcpy(slicePos, atWorld, 12); }
        else { float mn[3],mx[3]; worldAabb(r->gpuMeshes[(size_t)selected],mn,mx);
               slicePos[0]=(mn[0]+mx[0])*0.5f; slicePos[1]=(mn[1]+mx[1])*0.5f; slicePos[2]=(mn[2]+mx[2])*0.5f; }
        gizmoOp = 0;
        setStatus("SLICE GIZMO: drag the gizmo to place the plane (Move slides, Rotate turns), watch the highlighted cut line - ENTER cuts, ESC cancels");
    }
    void applySliceGizmo(){
        if (!sliceGizmoOn) return;
        float n[3]; sliceNormal(n);
        float pd = n[0]*slicePos[0]+n[1]*slicePos[1]+n[2]*slicePos[2];
        forEachSelMesh([&](int m){ sliceMeshByPlane(m, n, pd); });
        sliceGizmoOn = false;
    }
    void drawSliceOverlay(){
        if (!sliceGizmoOn || !r || selected<0 || selected>=(int)r->gpuMeshes.size()) return;
        float n[3]; sliceNormal(n);
        float pd = n[0]*slicePos[0]+n[1]*slicePos[1]+n[2]*slicePos[2];
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        cx.textAligned((float)rcViewport.offset.x, (float)rcViewport.offset.y+26*uiScale, (float)rcViewport.extent.width, 18*uiScale,
                       "SLICE GIZMO - Move/Rotate the plane, the glowing line is the exact cut - ENTER cuts, ESC cancels", ui::rgba(255,120,120), 1);
        // ── THE CUT FENCE: a big, IMPOSSIBLE-TO-MISS square frame with a grid, chunky handles and a
        //    direction arrow. Every line is drawn in SEGMENTS so it stays visible even when parts sit
        //    off-screen or the plane is viewed edge-on (the old quad vanished if ANY corner failed
        //    to project = "invisible line").
        { float mn[3],mx[3]; worldAabb(r->gpuMeshes[(size_t)selected],mn,mx);
          float s = 0.75f*std::sqrt((mx[0]-mn[0])*(mx[0]-mn[0])+(mx[1]-mn[1])*(mx[1]-mn[1])+(mx[2]-mn[2])*(mx[2]-mn[2])) + 0.5f;
          float t1[3], t2[3], n2[3]; { float a[3]={0,1,0}, b[3]={0,0,1}; quatRotVec(sliceQuat,a,t1); quatRotVec(sliceQuat,b,t2); }
          sliceNormal(n2);
          auto W3=[&](float a1, float a2v, float o[3]){ for (int k=0;k<3;++k) o[k]=slicePos[k]+t1[k]*a1+t2[k]*a2v; };
          auto segLine=[&](const float A3[3], const float B3[3], uint32_t col, float thick){
              const int SEG=12; float pa[2]; bool paOk=false;
              for (int i2=0;i2<=SEG;++i2){ float t=(float)i2/SEG;
                  float wp[3]={A3[0]+(B3[0]-A3[0])*t, A3[1]+(B3[1]-A3[1])*t, A3[2]+(B3[2]-A3[2])*t};
                  float pb[2]; bool ok=worldToScreen(wp,pb[0],pb[1]);
                  if (ok && paOk) dl.line(pa[0],pa[1],pb[0],pb[1],col,thick);
                  pa[0]=pb[0]; pa[1]=pb[1]; paOk=ok; } };
          const uint32_t FRAME=ui::rgba(255,80,80), GRID=ui::rgba(255,110,110,120);
          // grid (a FENCE - reads clearly even edge-on)
          const int GN=6;
          for (int i2=0;i2<=GN;++i2){ float f=-s+2.f*s*i2/GN;
              float A3[3],B3[3];
              W3(f,-s,A3); W3(f,s,B3); segLine(A3,B3, i2==0||i2==GN?FRAME:GRID, i2==0||i2==GN?4.f:1.5f);
              W3(-s,f,A3); W3(s,f,B3); segLine(A3,B3, i2==0||i2==GN?FRAME:GRID, i2==0||i2==GN?4.f:1.5f); }
          // chunky corner + edge-midpoint handles
          for (int i2=0;i2<8;++i2){ static const float HS[8][2]={{-1,-1},{1,-1},{1,1},{-1,1},{0,-1},{1,0},{0,1},{-1,0}};
              float hp[3]; W3(HS[i2][0]*s, HS[i2][1]*s, hp); float sc2[2];
              if (worldToScreen(hp, sc2[0], sc2[1])){ float hs2=7*uiScale;
                  dl.rect(sc2[0]-hs2, sc2[1]-hs2, hs2*2, hs2*2, FRAME);
                  dl.border(sc2[0]-hs2, sc2[1]-hs2, hs2*2, hs2*2, ui::rgba(30,10,10), 1.5f); } }
          // DIRECTION ARROW along the normal (which side is which)
          { float tip[3]={slicePos[0]+n2[0]*s*0.45f, slicePos[1]+n2[1]*s*0.45f, slicePos[2]+n2[2]*s*0.45f};
            segLine(slicePos, tip, ui::rgba(255,200,80), 4.f);
            float ts2[2]; if (worldToScreen(tip, ts2[0], ts2[1])){ float hs2=8*uiScale;
                dl.triangle(ts2[0]-hs2,ts2[1]+hs2, ts2[0]+hs2,ts2[1]+hs2, ts2[0],ts2[1]-hs2, ui::rgba(255,200,80)); } } }
        // LIVE CUT LINE: every selected mesh's triangle edges crossing the plane, highlighted
        int segs=0;
        std::vector<int> targets = sel.empty()? std::vector<int>{selected} : sel;
        for (int m : targets){
            if (m<0||m>=(int)sceneMeshes->size()||m>=(int)r->gpuMeshes.size()||r->isDeleted((size_t)m)) continue;
            const MeshData& md=(*sceneMeshes)[(size_t)m]; const float* M=r->gpuMeshes[(size_t)m].model;
            float A2=n[0]*M[0]+n[1]*M[1]+n[2]*M[2], B2=n[0]*M[4]+n[1]*M[5]+n[2]*M[6];
            float C2=n[0]*M[8]+n[1]*M[9]+n[2]*M[10], E2=n[0]*M[12]+n[1]*M[13]+n[2]*M[14]-pd;
            auto SD2=[&](uint32_t v2)->float{ const float* p=&md.positions[v2*3]; return A2*p[0]+B2*p[1]+C2*p[2]+E2; };
            for (size_t k=0;k+2<md.indices.size() && segs<3000;k+=3){
                uint32_t tri[3]={md.indices[k],md.indices[k+1],md.indices[k+2]};
                float sd[3]={SD2(tri[0]),SD2(tri[1]),SD2(tri[2])};
                float ip[2][3]; int ipn=0;
                for (int e=0;e<3 && ipn<2;++e){ float da=sd[e], db=sd[(e+1)%3];
                    if ((da>0&&db<0)||(da<0&&db>0)){ float t=da/(da-db);
                        const float* pa=&md.positions[tri[e]*3]; const float* pb=&md.positions[tri[(e+1)%3]*3];
                        float lp[3]={pa[0]+(pb[0]-pa[0])*t, pa[1]+(pb[1]-pa[1])*t, pa[2]+(pb[2]-pa[2])*t};
                        ip[ipn][0]=M[0]*lp[0]+M[4]*lp[1]+M[8]*lp[2]+M[12];
                        ip[ipn][1]=M[1]*lp[0]+M[5]*lp[1]+M[9]*lp[2]+M[13];
                        ip[ipn][2]=M[2]*lp[0]+M[6]*lp[1]+M[10]*lp[2]+M[14]; ++ipn; } }
                if (ipn==2){ float s0[2],s1[2];
                    if (worldToScreen(ip[0],s0[0],s0[1]) && worldToScreen(ip[1],s1[0],s1[1]))
                        { dl.line(s0[0],s0[1],s1[0],s1[1], ui::rgba(255,240,90), 5.f); ++segs; } }
            }
        }
        dl.popClip();
    }

    // ── mesh MIRROR: flip the geometry across an axis (about its own local center) so it FACES THE
    //    OTHER SIDE. Triangle winding is reversed (a mirror inverts orientation) so faces stay correct;
    //    the texture flips with the geometry. axis: 0=X 1=Y 2=Z. Non-destructive (Ctrl+Z reverts).
    void mirrorMesh(int mi, int axis){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        size_t nv = md.positions.size()/3;
        if (nv<3) return;
        float lo=1e30f, hi=-1e30f;
        for (size_t v2=0;v2<nv;++v2){ float p=md.positions[v2*3+axis]; lo=std::min(lo,p); hi=std::max(hi,p); }
        float c = (lo+hi)*0.5f;
        for (size_t v2=0;v2<nv;++v2) md.positions[v2*3+axis] = 2.f*c - md.positions[v2*3+axis];
        for (size_t k=0;k+2<md.indices.size();k+=3) std::swap(md.indices[k+1], md.indices[k+2]);   // fix winding
        const char* an[3]={"X","Y","Z"};
        if (commitGeomEdit(mi, std::move(md), "mirror"))
            setStatus(std::string("Mirrored across ")+an[axis]+" (faces the other side now) - Ctrl+Z reverts");
    }

    // ── mesh ROTATE (baked): TRUE rotation of the geometry about its own center axis — no reflection,
    //    winding/orientation preserved. axis 0=X 1=Y 2=Z; deg any multiple of 90 (90/180/270). Repeat
    //    presses stack. (Arbitrary angles = the Rotate gizmo; this BAKES the turn into the vertices.)
    void rotateMeshGeom(int mi, int axis, int deg){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        size_t nv = md.positions.size()/3;
        if (nv<3) return;
        int u=(axis+1)%3, v=(axis+2)%3;                       // the plane the rotation spins in
        float lo0=1e30f,hi0=-1e30f,lo1=1e30f,hi1=-1e30f;
        for (size_t k=0;k<nv;++k){ float a=md.positions[k*3+u], b=md.positions[k*3+v];
            lo0=std::min(lo0,a); hi0=std::max(hi0,a); lo1=std::min(lo1,b); hi1=std::max(hi1,b); }
        float cu=(lo0+hi0)*0.5f, cv=(lo1+hi1)*0.5f;
        float rad=(float)deg*3.14159265f/180.f, c=std::cos(rad), s=std::sin(rad);
        if (deg%90==0){ int q=((deg/90)%4+4)%4; const float qc[4]={1,0,-1,0}, qs[4]={0,1,0,-1}; c=qc[q]; s=qs[q]; }   // exact quarter turns
        for (size_t k=0;k<nv;++k){ float a=md.positions[k*3+u]-cu, b=md.positions[k*3+v]-cv;
            md.positions[k*3+u]=cu + a*c - b*s; md.positions[k*3+v]=cv + a*s + b*c; }
        const char* an[3]={"X","Y","Z"};
        char what[24]; snprintf(what,sizeof what,"rot%s%d",an[axis],deg);
        if (commitGeomEdit(mi, std::move(md), what))
            setStatus(std::string("Rotated ")+std::to_string(deg)+" deg about "+an[axis]+" center - repeat to stack, Ctrl+Z reverts");
    }
    void rotateMesh180(int mi){ rotateMeshGeom(mi, 1, 180); }

    // ── DOME PATTERN ANALYSIS: characterize an existing dome's tessellation (so we can EXTEND its own
    //    triangle pattern instead of replacing it). Reports: sphere center/radius, ring latitudes + per-ring
    //    segment counts, azimuth coverage, edge-length stats, and open boundary loops. Data-only (no edits).
    std::string analyzeDomePattern(int mi){
        std::string o;
        char b[256];
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()) return "pattern: bad mesh index\n";
        const MeshData& md=(*sceneMeshes)[(size_t)mi];
        size_t nv=md.positions.size()/3; size_t ntri=md.indices.size()/3;
        snprintf(b,sizeof b,"=== PATTERN mesh[%d] '%s'  nVerts=%zu nTris=%zu ===\n", mi, md.name.c_str(), nv, ntri); o+=b;
        if (nv<3||md.indices.size()<3) return o+"  (no geometry)\n";
        // center = bbox center; radius = mean dist
        double c[3]={0,0,0}; float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f};
        for(size_t v=0;v<nv;v++)for(int k=0;k<3;k++){ float p=md.positions[v*3+k]; mn[k]=std::min(mn[k],p); mx[k]=std::max(mx[k],p);}
        for(int k=0;k<3;k++) c[k]=(mn[k]+mx[k])*0.5;
        double rsum=0,rmin=1e30,rmax=0;
        std::vector<double> vt(nv),vph(nv);
        for(size_t v=0;v<nv;v++){ double dx=md.positions[v*3]-c[0],dy=md.positions[v*3+1]-c[1],dz=md.positions[v*3+2]-c[2];
            double l=std::sqrt(dx*dx+dy*dy+dz*dz); rsum+=l; rmin=std::min(rmin,l); rmax=std::max(rmax,l);
            double ll=l<1e-6?1:l; vt[v]=std::acos(std::clamp(dy/ll,-1.0,1.0)); vph[v]=std::atan2(dz,dx); }
        double rmean=rsum/nv;
        snprintf(b,sizeof b,"  bboxCenter=(%.2f,%.2f,%.2f)  radius mean=%.2f min=%.2f max=%.2f (spread=%.1f%%)\n",
                 c[0],c[1],c[2], rmean,rmin,rmax, (rmax-rmin)/std::max(1e-6,rmean)*100.0); o+=b;
        // LEAST-SQUARES SPHERE FIT (the TRUE sphere the cap lies on) + residual spread from THAT center
        { double A[4][4]={{0}}, B4[4]={0};
          for(size_t v=0;v<nv;v++){ double px=md.positions[v*3],py=md.positions[v*3+1],pz=md.positions[v*3+2];
              double row[4]={2*px,2*py,2*pz,1.0}, rhs=px*px+py*py+pz*pz;
              for(int i=0;i<4;i++){ for(int j=0;j<4;j++)A[i][j]+=row[i]*row[j]; B4[i]+=row[i]*rhs; } }
          double M2[4][5]; for(int i=0;i<4;i++){for(int j=0;j<4;j++)M2[i][j]=A[i][j]; M2[i][4]=B4[i];}
          bool ok=true; for(int cc=0;cc<4;cc++){ int piv=cc; for(int rr=cc+1;rr<4;rr++) if(std::fabs(M2[rr][cc])>std::fabs(M2[piv][cc]))piv=rr;
              if(std::fabs(M2[piv][cc])<1e-12){ok=false;break;} for(int j=0;j<5;j++)std::swap(M2[cc][j],M2[piv][j]);
              for(int rr=0;rr<4;rr++){ if(rr==cc)continue; double f=M2[rr][cc]/M2[cc][cc]; for(int j=cc;j<5;j++)M2[rr][j]-=f*M2[cc][j]; } }
          if(ok){ double s[4]; for(int i=0;i<4;i++)s[i]=M2[i][4]/M2[i][i];
              double Rf=std::sqrt(std::max(0.0,s[3]+s[0]*s[0]+s[1]*s[1]+s[2]*s[2]));
              double fmn=1e30,fmx=0,frms=0; for(size_t v=0;v<nv;v++){ double dx=md.positions[v*3]-s[0],dy=md.positions[v*3+1]-s[1],dz=md.positions[v*3+2]-s[2];
                  double l=std::sqrt(dx*dx+dy*dy+dz*dz); fmn=std::min(fmn,l); fmx=std::max(fmx,l); frms+=(l-Rf)*(l-Rf); } frms=std::sqrt(frms/nv);
              snprintf(b,sizeof b,"  LS-FIT sphere center=(%.2f,%.2f,%.2f) R=%.2f  |v-C|:[%.1f..%.1f]  RMS residual=%.2f (%.1f%% of R)\n",
                       s[0],s[1],s[2],Rf, fmn,fmx, frms, frms/std::max(1e-6,Rf)*100.0); o+=b; }
          else o+="  LS-FIT sphere: FAILED (degenerate)\n"; }
        // RINGS: sort polar angles, split into clusters where the gap exceeds a threshold
        std::vector<double> ts=vt; std::sort(ts.begin(),ts.end());
        double span=ts.back()-ts.front(); double thr=std::max(0.02, span/200.0);
        std::vector<std::pair<double,int>> rings;  // (mean latitude, count)
        { double acc=ts[0]; int cnt=1;
          for(size_t i=1;i<ts.size();++i){ if(ts[i]-ts[i-1]>thr){ rings.push_back({acc/cnt, cnt}); acc=0; cnt=0; } acc+=ts[i]; cnt++; }
          rings.push_back({acc/cnt,cnt}); }
        snprintf(b,sizeof b,"  polar t range=[%.1f°..%.1f°]  detected %zu rings (gap thr=%.3f rad):\n",
                 ts.front()*57.2958, ts.back()*57.2958, rings.size(), thr); o+=b;
        // per-ring azimuth coverage: assign each vert to nearest ring, measure azimuth span + typical step
        for(size_t ri=0; ri<rings.size() && ri<40; ++ri){ double rt=rings[ri].first;
            std::vector<double> az;
            for(size_t v=0;v<nv;v++) if(std::fabs(vt[v]-rt) <= thr) az.push_back(vph[v]);
            std::sort(az.begin(),az.end());
            double aspan = az.empty()?0 : (az.back()-az.front());
            // median azimuth gap
            double medstep=0; if(az.size()>1){ std::vector<double> g; for(size_t i=1;i<az.size();++i)g.push_back(az[i]-az[i-1]); std::sort(g.begin(),g.end()); medstep=g[g.size()/2]; }
            int segFull = medstep>1e-4 ? (int)std::llround(2*3.14159265/medstep) : 0;
            snprintf(b,sizeof b,"    ring %2zu  t=%6.1f°  verts=%3zu  azSpan=%6.1f°  medStep=%5.2f°  ->fullSegs~%d\n",
                     ri, rt*57.2958, az.size(), aspan*57.2958, medstep*57.2958, segFull); o+=b;
        }
        // EDGE lengths (welded) + BOUNDARY loops
        std::vector<std::vector<uint32_t>> loops; collectBoundaryLoops(md, loops);
        snprintf(b,sizeof b,"  boundary loops (holes/rims): %zu\n", loops.size()); o+=b;
        for(size_t li=0; li<loops.size() && li<12; ++li){ snprintf(b,sizeof b,"    loop %2zu: %zu edges\n", li, loops[li].size()); o+=b; }
        return o;
    }

    // ── COMPLETE DOME: regenerate the sky dome as ONE UNIFORM sphere. The partial mesh (e.g.
    //    bluehillsgoldmine VE_WESTERN sky_MTL, a perfect sphere CAP) is fitted to its sphere, then the WHOLE
    //    thing is rebuilt as an ICOSPHERE — geodesic, uniform near-equilateral triangles EVERYWHERE, no pole
    //    slivers, top identical to bottom. Its texture is re-baked into a standard equirect map (real imagery
    //    rasterized to its true direction, empty parts push-pull inpainted) and the icosphere gets equirect
    //    UVs, so it stays textured. Non-destructive (original soft-deleted; Ctrl+Z reverts).
    void completeDome(int mi, bool fullSphere=false){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        size_t nv = md.positions.size()/3;
        if (nv < 8 || md.indices.size() < 3) { setStatus("dome: mesh has no editable geometry"); return; }
        const double PI = 3.14159265358979323846;
        bool hasUV = md.uvs.size() >= nv*2;

        // ── 1. least-squares SPHERE fit → centre C, radius R (V79 sky domes fit at 0.0% residual) ──
        double C[3]={0,0,0}, R=0;
        { double A[4][4]={{0}}, B4[4]={0};
          for(size_t v=0;v<nv;v++){ double px=md.positions[v*3],py=md.positions[v*3+1],pz=md.positions[v*3+2];
              double row[4]={2*px,2*py,2*pz,1.0}, rhs=px*px+py*py+pz*pz;
              for(int i=0;i<4;i++){ for(int j=0;j<4;j++)A[i][j]+=row[i]*row[j]; B4[i]+=row[i]*rhs; } }
          double M2[4][5]; for(int i=0;i<4;i++){for(int j=0;j<4;j++)M2[i][j]=A[i][j]; M2[i][4]=B4[i];}
          bool ok=true; for(int cc=0;cc<4;cc++){ int piv=cc; for(int rr=cc+1;rr<4;rr++) if(std::fabs(M2[rr][cc])>std::fabs(M2[piv][cc]))piv=rr;
              if(std::fabs(M2[piv][cc])<1e-12){ok=false;break;} for(int j=0;j<5;j++)std::swap(M2[cc][j],M2[piv][j]);
              for(int rr=0;rr<4;rr++){ if(rr==cc)continue; double f=M2[rr][cc]/M2[cc][cc]; for(int j=cc;j<5;j++)M2[rr][j]-=f*M2[cc][j]; } }
          if(!ok){ setStatus("dome: sphere fit failed"); return; }
          double s[4]; for(int i=0;i<4;i++)s[i]=M2[i][4]/M2[i][i];
          C[0]=s[0]; C[1]=s[1]; C[2]=s[2]; R=std::sqrt(std::max(0.0,s[3]+s[0]*s[0]+s[1]*s[1]+s[2]*s[2])); }
        if (!(R>1e-3)) { setStatus("dome: degenerate sphere"); return; }

        // ── 2. bake the original texture into a STANDARD equirect map (u=azimuth/2π, v=polar/π) ──
        bool bakeTex = hasUV && md.hasTexture && !md.texRGBA.empty() && md.texW>0 && md.texH>0
                       && md.texRGBA.size() >= (size_t)md.texW*md.texH*4;
        std::vector<u8> canvas; const int TW=2048, TH=1024;
        if (bakeTex) {
            std::vector<double> vt(nv), vph(nv);
            for (size_t v=0; v<nv; ++v){ double dx=md.positions[v*3]-C[0],dy=md.positions[v*3+1]-C[1],dz=md.positions[v*3+2]-C[2];
                double l=std::sqrt(dx*dx+dy*dy+dz*dz); if(l<1e-6)l=1; vt[v]=std::acos(std::clamp(dy/l,-1.0,1.0)); vph[v]=std::atan2(dz,dx); }
            int SW=(int)md.texW, SH=(int)md.texH; const std::vector<u8>& src=md.texRGBA;
            auto sampleSrc=[&](double u,double vv,u8 out[3]){ u-=std::floor(u); vv-=std::floor(vv); double fx=u*SW-0.5,fy=vv*SH-0.5;
                int x0=(int)std::floor(fx),y0=(int)std::floor(fy); double tx=fx-x0,ty=fy-y0;
                auto S=[&](int x,int y,int c)->double{ x=std::clamp(x,0,SW-1); y=std::clamp(y,0,SH-1); return (double)src[((size_t)y*SW+x)*4+c]; };
                for(int c=0;c<3;++c){ double a=S(x0,y0,c)*(1-tx)+S(x0+1,y0,c)*tx,b=S(x0,y0+1,c)*(1-tx)+S(x0+1,y0+1,c)*tx; out[c]=(u8)std::clamp(a*(1-ty)+b*ty,0.0,255.0); } };
            canvas.assign((size_t)TW*TH*3,0); std::vector<u8> mask((size_t)TW*TH,0);
            auto edge=[](double ax,double ay,double bx,double by,double px,double py){ return (bx-ax)*(py-ay)-(by-ay)*(px-ax); };
            for (size_t k=0;k+2<md.indices.size();k+=3){ uint32_t id[3]={md.indices[k],md.indices[k+1],md.indices[k+2]};
                if(id[0]>=nv||id[1]>=nv||id[2]>=nv) continue;
                double eu[3],ev[3],ou[3],ov[3];
                for(int c=0;c<3;c++){ eu[c]=(vph[id[c]]+PI)/(2*PI); ev[c]=std::clamp(vt[id[c]]/PI,0.0,1.0); ou[c]=md.uvs[id[c]*2]; ov[c]=md.uvs[id[c]*2+1]; }
                double mnu=std::min({eu[0],eu[1],eu[2]}), mxu=std::max({eu[0],eu[1],eu[2]}); if(mxu-mnu>0.5) for(int c=0;c<3;c++) if(eu[c]<0.5)eu[c]+=1.0;
                double px[3],py[3]; for(int c=0;c<3;c++){ px[c]=eu[c]*TW; py[c]=ev[c]*TH; }
                double area=edge(px[0],py[0],px[1],py[1],px[2],py[2]); if(std::fabs(area)<1e-9)continue;
                int x0=(int)std::floor(std::min({px[0],px[1],px[2]})),x1=(int)std::ceil(std::max({px[0],px[1],px[2]}));
                int y0=std::clamp((int)std::floor(std::min({py[0],py[1],py[2]})),0,TH-1),y1=std::clamp((int)std::ceil(std::max({py[0],py[1],py[2]})),0,TH-1);
                for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){ double sx=x+0.5,sy=y+0.5;
                    double w0=edge(px[1],py[1],px[2],py[2],sx,sy)/area,w1=edge(px[2],py[2],px[0],py[0],sx,sy)/area,w2=1.0-w0-w1;
                    if(w0<-1e-4||w1<-1e-4||w2<-1e-4)continue;
                    double ux=w0*ou[0]+w1*ou[1]+w2*ou[2],vx=w0*ov[0]+w1*ov[1]+w2*ov[2];
                    int xx=((x%TW)+TW)%TW; size_t pi=(size_t)y*TW+xx; sampleSrc(ux,vx,&canvas[pi*3]); mask[pi]=1; } }
            // push-pull inpaint the uncovered region so it's never blank
            std::vector<std::vector<float>> LC,LW2; std::vector<int> LWd,LHt;
            { std::vector<float> c((size_t)TW*TH*3,0),w((size_t)TW*TH,0);
              for(size_t i=0;i<(size_t)TW*TH;i++) if(mask[i]){ c[i*3]=canvas[i*3]/255.f;c[i*3+1]=canvas[i*3+1]/255.f;c[i*3+2]=canvas[i*3+2]/255.f;w[i]=1.f; }
              LC.push_back(std::move(c)); LW2.push_back(std::move(w)); LWd.push_back(TW); LHt.push_back(TH); }
            while(LWd.back()>1||LHt.back()>1){ int cw=LWd.back(),ch=LHt.back(),pw=std::max(1,cw/2),ph=std::max(1,ch/2);
                const auto& cc=LC.back(); const auto& cw2=LW2.back(); std::vector<float> c((size_t)pw*ph*3,0),w((size_t)pw*ph,0);
                for(int y=0;y<ph;y++)for(int x=0;x<pw;x++){ float sw=0,sc[3]={0,0,0};
                    for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){ int sx=std::min(x*2+dx,cw-1),sy=std::min(y*2+dy,ch-1); size_t si=(size_t)sy*cw+sx; float ww=cw2[si]; sw+=ww; for(int t=0;t<3;t++)sc[t]+=cc[si*3+t]*ww; }
                    size_t pi=(size_t)y*pw+x; if(sw>0)for(int t=0;t<3;t++)c[pi*3+t]=sc[t]/sw; w[pi]=std::min(1.f,sw*0.25f); }
                LC.push_back(std::move(c)); LW2.push_back(std::move(w)); LWd.push_back(pw); LHt.push_back(ph); }
            for(int l=(int)LC.size()-2;l>=0;l--){ int cw=LWd[l],ch=LHt[l],pw=LWd[l+1]; auto& cc=LC[l]; auto& cw2=LW2[l]; const auto& pc=LC[l+1]; const auto& pw2=LW2[l+1];
                for(int y=0;y<ch;y++)for(int x=0;x<cw;x++){ size_t ci=(size_t)y*cw+x; float ownW=cw2[ci]; if(ownW>=0.999f)continue;
                    int qx=std::min(x/2,pw-1),qy=std::min(y/2,LHt[l+1]-1); size_t pi=(size_t)qy*pw+qx; if(pw2[pi]<=0)continue;
                    for(int t=0;t<3;t++)cc[ci*3+t]=cc[ci*3+t]*ownW+pc[pi*3+t]*(1.f-ownW); cw2[ci]=1.f; } }
            const auto& c0=LC[0]; for(size_t i=0;i<(size_t)TW*TH;i++) if(!mask[i]) for(int t=0;t<3;t++) canvas[i*3+t]=(u8)std::clamp(c0[i*3+t]*255.f,0.f,255.f);
        }

        // ── 3. build a UNIFORM ICOSPHERE (subdivided icosahedron) → the new, consistent geometry ──
        const double gt=(1.0+std::sqrt(5.0))/2.0;
        std::vector<std::array<double,3>> P = {
            {-1,gt,0},{1,gt,0},{-1,-gt,0},{1,-gt,0}, {0,-1,gt},{0,1,gt},{0,-1,-gt},{0,1,-gt}, {gt,0,-1},{gt,0,1},{-gt,0,-1},{-gt,0,1} };
        for (auto& p : P){ double l=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]); p[0]/=l;p[1]/=l;p[2]/=l; }
        std::vector<std::array<uint32_t,3>> F = {
            {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11}, {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
            {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9}, {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
        const int SUB = 5;   // 20*4^5 = 20480 tris (matches the game's own full dome density)
        std::map<uint64_t,uint32_t> midCache;
        auto midpoint=[&](uint32_t a, uint32_t b)->uint32_t{
            uint64_t key = a<b ? ((uint64_t)a<<32|b) : ((uint64_t)b<<32|a);
            auto it=midCache.find(key); if(it!=midCache.end()) return it->second;
            std::array<double,3> m={ (P[a][0]+P[b][0])*0.5,(P[a][1]+P[b][1])*0.5,(P[a][2]+P[b][2])*0.5 };
            double l=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); m[0]/=l;m[1]/=l;m[2]/=l;
            uint32_t idx=(uint32_t)P.size(); P.push_back(m); midCache[key]=idx; return idx; };
        for (int s=0;s<SUB;++s){ std::vector<std::array<uint32_t,3>> F2; F2.reserve(F.size()*4);
            for (auto& f : F){ uint32_t a=midpoint(f[0],f[1]), b=midpoint(f[1],f[2]), c=midpoint(f[2],f[0]);
                F2.push_back({f[0],a,c}); F2.push_back({f[1],b,a}); F2.push_back({f[2],c,b}); F2.push_back({a,b,c}); }
            F.swap(F2); }

        // ── 4. write the icosphere into md: positions on the fitted sphere, equirect UVs, seam-split ──
        md.positions.clear(); md.uvs.clear(); md.indices.clear();
        md.colors.clear(); md.uvs2.clear(); md.uvs3.clear(); md.uvs4.clear();
        md.positions.reserve(P.size()*3); md.uvs.reserve(P.size()*2);
        std::vector<double> U(P.size());
        for (size_t i=0;i<P.size();++i){ const auto& d=P[i];
            md.positions.push_back((float)(C[0]+R*d[0])); md.positions.push_back((float)(C[1]+R*d[1])); md.positions.push_back((float)(C[2]+R*d[2]));
            double u=(std::atan2(d[2],d[0])+PI)/(2*PI), v=std::acos(std::clamp(d[1],-1.0,1.0))/PI;
            U[i]=u; md.uvs.push_back((float)u); md.uvs.push_back((float)v); }
        // seam fix: a triangle spanning the u=0/1 wrap gets its low-u corners duplicated at u+1 (no smear)
        std::map<uint32_t,uint32_t> seamDup;
        auto seamVert=[&](uint32_t v)->uint32_t{ auto it=seamDup.find(v); if(it!=seamDup.end())return it->second;
            uint32_t idx=(uint32_t)(md.positions.size()/3);
            md.positions.push_back(md.positions[v*3]); md.positions.push_back(md.positions[v*3+1]); md.positions.push_back(md.positions[v*3+2]);
            md.uvs.push_back((float)(U[v]+1.0)); md.uvs.push_back(md.uvs[v*2+1]); seamDup[v]=idx; return idx; };
        for (auto& f : F){ uint32_t a=f[0],b=f[1],c=f[2];
            double mn=std::min({U[a],U[b],U[c]}), mx=std::max({U[a],U[b],U[c]});
            if (mx-mn>0.5){ if(U[a]<0.5)a=seamVert(a); if(U[b]<0.5)b=seamVert(b); if(U[c]<0.5)c=seamVert(c); }
            md.indices.push_back(a); md.indices.push_back(b); md.indices.push_back(c); }
        md.doubleSided = true;   // sky dome viewed from inside
        if (bakeTex){ std::vector<u8> rgba((size_t)TW*TH*4,255);
            for(size_t i=0;i<(size_t)TW*TH;i++){ rgba[i*4]=canvas[i*3]; rgba[i*4+1]=canvas[i*3+1]; rgba[i*4+2]=canvas[i*3+2]; }
            md.texRGBA.swap(rgba); md.texW=TW; md.texH=TH; md.hasTexture=true; md.srcAstc.clear(); md.srcAstcBw=md.srcAstcBh=md.srcAstcMips=0; }

        size_t tris=F.size();
        if (commitGeomEdit(mi, std::move(md), fullSphere?"completeSphere":"completeDome"))
            setStatus("Regenerated a UNIFORM icosphere ("+std::to_string(tris)+" even tris, no poles)"
                      +std::string(bakeTex?" + equirect sky map":"")+" - Ctrl+Z reverts");
    }

    // ── FUSE the multi-selection into ONE mesh: every source's CURRENT world transform is baked into
    //    the vertices, geometry concatenated, and EVERY SOURCE KEEPS ITS OWN TEXTURE — the textures are
    //    packed into ONE ATLAS (identical textures share a cell) and each source's UVs are remapped into
    //    its cell. Sources go to history (ONE Ctrl+Z un-fuses). Skinned/animated sources are skipped.
    //    Caveat: TILED UVs (outside 0..1) are wrapped into the cell — heavy tiling shows seams.
    // Fuse keeps the sources' ORIGINAL triangle pattern by default (nothing re-guessed); this opt-in
    // additionally drops later-source triangles fully covered by an earlier coplanar sheet (z-fight
    // cleanup for stacked flat floors — leave OFF for curved/bent geometry and patches).
    bool fuseDropCovered = false;
    void fuseSelectedMeshes(){
        if (!r || !sceneMeshes || sel.size()<2) { setStatus("fuse: select 2+ meshes first (Ctrl+click)"); return; }
        std::vector<int> src; int skippedAnim=0;
        for (int s : sel) { if (s<0||s>=(int)sceneMeshes->size()||s>=(int)r->gpuMeshes.size()||r->isDeleted((size_t)s)) continue;
            if (r->gpuMeshes[s].isSkinned || r->gpuMeshes[s].dynamicVerts) { ++skippedAnim; continue; }
            src.push_back(s); }
        if (src.size()<2) { setStatus("fuse: need 2+ STATIC meshes (skinned/animated can't fuse)"); return; }
        int base = src[0];
        // ── TEXTURE ATLAS: dedup source textures by content, pack unique ones into a grid ──
        static const u8 WHITE1[4] = {255,255,255,255};
        struct SrcTex { const u8* d; int w, h; };
        std::vector<SrcTex> cells; std::map<unsigned long long,int> cellByHash;
        std::vector<int> cellOf(src.size());
        for (size_t i2=0;i2<src.size();++i2){ const MeshData& md=(*sceneMeshes)[(size_t)src[i2]];
            SrcTex st{WHITE1,1,1};
            if (!md.texRGBA.empty() && md.texW>0 && md.texH>0 && md.texRGBA.size()>=(size_t)md.texW*md.texH*4)
                st = SrcTex{ md.texRGBA.data(), (int)md.texW, (int)md.texH };
            unsigned long long hh = VkRenderer::texHash(st.d, (size_t)st.w*st.h*4, (u32)st.w, (u32)st.h, 0);
            auto it=cellByHash.find(hh);
            if (it==cellByHash.end()){ cellByHash[hh]=(int)cells.size(); cellOf[i2]=(int)cells.size(); cells.push_back(st); }
            else cellOf[i2]=it->second;
        }
        int nCells=(int)cells.size();
        int grid=1; while (grid*grid<nCells) ++grid;
        int cellW=1, cellH=1; for (auto& c : cells){ cellW=std::max(cellW,c.w); cellH=std::max(cellH,c.h); }
        const int MAXA=4096;                                   // cap the atlas (downscale cells to fit)
        if (grid*cellW>MAXA) cellW=MAXA/grid; if (grid*cellH>MAXA) cellH=MAXA/grid;
        if (cellW<1) cellW=1; if (cellH<1) cellH=1;
        int aw=grid*cellW, ah=grid*cellH;
        std::vector<u8> atlas; bool useAtlas = nCells>1;
        if (useAtlas) {
            atlas.assign((size_t)aw*ah*4, 255);
            for (int ci=0; ci<nCells; ++ci) {                  // bilinear-resample each unique texture into its cell
                const SrcTex& st=cells[ci]; int ox=(ci%grid)*cellW, oy=(ci/grid)*cellH;
                for (int y2=0;y2<cellH;++y2) for (int x2=0;x2<cellW;++x2) {
                    float fx=(x2+0.5f)/cellW*st.w-0.5f, fy=(y2+0.5f)/cellH*st.h-0.5f;
                    int x0=(int)std::floor(fx), y0=(int)std::floor(fy);
                    float tx=fx-x0, ty=fy-y0;
                    auto S=[&](int px,int py,int ch){ px=std::clamp(px,0,st.w-1); py=std::clamp(py,0,st.h-1); return (float)st.d[((size_t)py*st.w+px)*4+ch]; };
                    u8* o=&atlas[((size_t)(oy+y2)*aw+(ox+x2))*4];
                    for (int ch=0;ch<4;++ch){
                        float v0=S(x0,y0,ch)*(1-tx)+S(x0+1,y0,ch)*tx, v1=S(x0,y0+1,ch)*(1-tx)+S(x0+1,y0+1,ch)*tx;
                        o[ch]=(u8)std::clamp(v0*(1-ty)+v1*ty, 0.f, 255.f); }
                }
            }
        }
        MeshData out = (*sceneMeshes)[(size_t)base];          // material FLAGS from the active-first mesh
        out.positions.clear(); out.uvs.clear(); out.uvs2.clear(); out.indices.clear();
        out.transform = Transform{};                          // world already baked into the verts below
        out.hasWorldMatrix = true;
        static const float I16[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(out.worldMatrix, I16, sizeof I16);
        if (useAtlas) {
            out.texRGBA = std::move(atlas); out.texW=(u32)aw; out.texH=(u32)ah; out.hasTexture=true;
            out.astcRaw.clear();                              // the compressed source no longer matches — force the RGBA path
            out.hasLightmap=false; out.lmRGBA.clear();        // per-source role maps can't ride the atlas UVs
            out.hasNormal=false; out.normalRGBA.clear();
            out.hasOrm=false; out.ormRGBA.clear();
            out.hasEmissive=false; out.emissiveRGBA.clear();
        }
        // per-vertex side arrays copied from the base would MISALIGN with the concatenated verts — drop them
        out.colors.clear(); out.uvs3.clear(); out.uvs4.clear();
        out.boneIndices.clear(); out.boneWeights.clear(); out.hasBones=false;
        float insetU = useAtlas ? 2.0f/(float)cellW : 0.f;    // guard band so mips don't bleed neighbor cells
        float insetV = useAtlas ? 2.0f/(float)cellH : 0.f;
        std::vector<int> triOrd;                              // per-triangle source ordinal (selection order)
        for (size_t i2=0;i2<src.size();++i2) {
            int s = src[i2];
            const MeshData& md=(*sceneMeshes)[(size_t)s]; const VkGpuMesh& gm=r->gpuMeshes[(size_t)s];
            const float* M=gm.model;
            int cell=cellOf[i2]; float cgx=(float)(cell%grid), cgy=(float)(cell/grid);
            u32 vb=(u32)(out.positions.size()/3);
            size_t nv=md.positions.size()/3;
            bool srcUV = md.uvs.size()>=nv*2;
            for (size_t k2=0;k2<nv;++k2){ const float* p=&md.positions[k2*3];
                out.positions.push_back(M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]);
                out.positions.push_back(M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]);
                out.positions.push_back(M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]);
                float u0 = srcUV ? md.uvs[k2*2] : 0.f, v0 = srcUV ? md.uvs[k2*2+1] : 0.f;
                if (useAtlas) {
                    u0 -= std::floor(u0); v0 -= std::floor(v0);                     // wrap tiling into the cell
                    u0 = (cgx + insetU + u0*(1.f-2.f*insetU)) / (float)grid;        // remap into THIS source's cell
                    v0 = (cgy + insetV + v0*(1.f-2.f*insetV)) / (float)grid;
                }
                out.uvs.push_back(u0); out.uvs.push_back(v0);
            }
            for (u32 ix : md.indices) out.indices.push_back(vb+ix);
            triOrd.resize(out.indices.size()/3, (int)i2);   // tag every tri with its source ordinal (earlier selection wins overlaps)
        }
        // ── Z-FIGHT ELIMINATION ────────────────────────────────────────────────────────────────────
        // Pass A: weld by position, drop degenerate + exact SAME-WINDING duplicate triangles (reversed
        // duplicates = intentional double-siding, always kept). The fused mesh otherwise keeps the
        // sources' ORIGINAL triangle pattern verbatim.
        // Pass B (OPT-IN via fuseDropCovered): COPLANAR COVERAGE — a triangle from a LATER-selected
        // source that lies in (within ~8mm of) an EARLIER source's plane AND whose surface is fully
        // covered by that source's triangles is REDUNDANT SHEET - dropped. First-selected sheet wins.
        // Kills stacked floorGroup01/02/03 shimmer without full CSG (partial overlaps keep both).
        {
            size_t fnv = out.positions.size()/3;
            std::vector<uint32_t> weld(fnv);
            { std::map<std::tuple<long long,long long,long long>, uint32_t> wgrid;
              for (size_t v2=0; v2<fnv; ++v2) {
                  auto k = std::make_tuple((long long)std::llround(out.positions[v2*3]  *2000.0),
                                           (long long)std::llround(out.positions[v2*3+1]*2000.0),
                                           (long long)std::llround(out.positions[v2*3+2]*2000.0));
                  auto it=wgrid.find(k);
                  if (it==wgrid.end()) { wgrid.emplace(k,(uint32_t)v2); weld[v2]=(uint32_t)v2; } else weld[v2]=it->second;
              } }
            // Pass A — duplicate key preserves WINDING (rotation-canonical, smallest index first).
            // Sorting the indices treated a REVERSED-winding copy as "the same triangle" and dropped
            // it — but these envs emulate double-siding with reversed duplicates, so fusing punched
            // back-face holes into perfectly good meshes.
            std::set<std::tuple<uint32_t,uint32_t,uint32_t>> seen;
            std::vector<u32> keep; keep.reserve(out.indices.size());
            std::vector<int> keepOrd;
            size_t dropped=0;
            for (size_t k=0;k+2<out.indices.size();k+=3){
                uint32_t a=weld[out.indices[k]], b=weld[out.indices[k+1]], c=weld[out.indices[k+2]];
                if (a==b||b==c||c==a) { ++dropped; continue; }                     // degenerate
                uint32_t s0=a,s1=b,s2=c;                                           // rotate so the smallest LEADS, order kept
                if (b<=a&&b<=c){ s0=b;s1=c;s2=a; } else if (c<=a&&c<=b){ s0=c;s1=a;s2=b; }
                if (!seen.insert(std::make_tuple(s0,s1,s2)).second) { ++dropped; continue; }   // exact SAME-winding duplicate
                keep.push_back(out.indices[k]); keep.push_back(out.indices[k+1]); keep.push_back(out.indices[k+2]);
                keepOrd.push_back(k/3 < triOrd.size() ? triOrd[k/3] : 0);
            }
            // Pass B: coplanar-coverage removal (later source loses where an earlier one already has
            // surface). OPT-IN (fuseDropCovered, Geometry-panel toggle): by default fuse keeps the
            // ORIGINAL triangle pattern verbatim — the coverage guess ate bent/curved sheets bucketed
            // as "coplanar" and re-opened patched holes ("fuse generates more holes"). Turn it ON only
            // for genuinely stacked flat sheets (floorGroup z-fight).
            size_t nT = keep.size()/3, coplDropped=0;
            std::vector<char> triDead(nT, 0);
            auto V=[&](size_t t,int e)->const float*{ return &out.positions[keep[t*3+e]*3]; };
            if (fuseDropCovered) {
            struct TriP { float n[3]; float d; int dom; float mn[2], mx[2]; };
            std::vector<TriP> tp(nT);
            std::map<std::tuple<int,int,int>, std::vector<int>> planeBuckets;      // quantized canonical normal -> tris
            auto uvOf=[&](const TriP& P, const float* p, float& u, float& v){     // project onto the plane's dominant 2D
                int a2=(P.dom+1)%3, b2=(P.dom+2)%3; u=p[a2]; v=p[b2]; };
            for (size_t t=0;t<nT;++t){
                const float* A=V(t,0); const float* B=V(t,1); const float* C=V(t,2);
                float e1[3]={B[0]-A[0],B[1]-A[1],B[2]-A[2]}, e2[3]={C[0]-A[0],C[1]-A[1],C[2]-A[2]};
                float n[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
                float l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                if (l<1e-9f){ tp[t].dom=-1; continue; }
                n[0]/=l; n[1]/=l; n[2]/=l;
                int dom = std::fabs(n[0])>std::fabs(n[1]) ? (std::fabs(n[0])>std::fabs(n[2])?0:2) : (std::fabs(n[1])>std::fabs(n[2])?1:2);
                if (n[dom]<0){ n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; }               // canonical facing (opposite-winding sheets fight too)
                TriP& P=tp[t]; memcpy(P.n,n,12); P.d=n[0]*A[0]+n[1]*A[1]+n[2]*A[2]; P.dom=dom;
                float u,v; uvOf(P,A,u,v); P.mn[0]=P.mx[0]=u; P.mn[1]=P.mx[1]=v;
                for (int e=1;e<3;++e){ uvOf(P,V(t,e),u,v); P.mn[0]=std::min(P.mn[0],u); P.mx[0]=std::max(P.mx[0],u); P.mn[1]=std::min(P.mn[1],v); P.mx[1]=std::max(P.mx[1],v); }
                planeBuckets[std::make_tuple((int)std::lround(n[0]*24.f),(int)std::lround(n[1]*24.f),(int)std::lround(n[2]*24.f))].push_back((int)t);
            }
            // editor-built PATCH sources are NEVER dropped as "redundant sheet": the user built them
            // specifically to FILL a hole — the old coverage test (4 samples only) kept declaring
            // rim-straddling patch triangles fully covered and fusing re-opened the very hole.
            std::vector<char> ordIsPatch(src.size(), 0);
            for (size_t o2=0;o2<src.size();++o2){ const std::string& nm=(*sceneMeshes)[(size_t)src[o2]].name;
                if (nm=="patch"||nm.rfind("patch",0)==0) ordIsPatch[o2]=1; }
            const float PLANE_EPS = 0.008f;                                        // 8mm sheet-offset tolerance
            for (auto& kb : planeBuckets) {
                auto& tris = kb.second;
                if (tris.size()<2) continue;
                // 2D grid inside the bucket for fast coverage lookups
                std::map<std::pair<int,int>, std::vector<int>> cell;               // 0.5m cells over dominant-plane coords
                auto cellOf2=[&](float u,float v){ return std::make_pair((int)std::floor(u*2.f),(int)std::floor(v*2.f)); };
                for (int t : tris){ if (tp[t].dom<0) continue;
                    for (int cu=(int)std::floor(tp[t].mn[0]*2.f); cu<=(int)std::floor(tp[t].mx[0]*2.f); ++cu)
                        for (int cv=(int)std::floor(tp[t].mn[1]*2.f); cv<=(int)std::floor(tp[t].mx[1]*2.f); ++cv)
                            cell[{cu,cv}].push_back(t); }
                for (int t : tris) {
                    if (tp[t].dom<0 || keepOrd[t]==0) continue;                    // the FIRST source never loses
                    if (keepOrd[t]<(int)ordIsPatch.size() && ordIsPatch[keepOrd[t]]) continue;   // patches never lose
                    // sample points: centroid + verts shrunk 8% toward centroid + edge midpoints shrunk
                    // 8% (7 samples - 4 missed partial overlaps and dropped hole-spanning triangles)
                    const float* A=V(t,0); const float* B=V(t,1); const float* C=V(t,2);
                    float cen[3]={(A[0]+B[0]+C[0])/3,(A[1]+B[1]+C[1])/3,(A[2]+B[2]+C[2])/3};
                    float smp[7][3]; memcpy(smp[0],cen,12);
                    for (int e=0;e<3;++e){ const float* P0=V(t,e);
                        for (int ax2=0;ax2<3;++ax2) smp[e+1][ax2]=P0[ax2]+(cen[ax2]-P0[ax2])*0.08f; }
                    for (int e=0;e<3;++e){ const float* P0=V(t,e); const float* P1=V(t,(e+1)%3);
                        for (int ax2=0;ax2<3;++ax2){ float m2=(P0[ax2]+P1[ax2])*0.5f; smp[e+4][ax2]=m2+(cen[ax2]-m2)*0.08f; } }
                    bool allCovered=true;
                    for (int si2=0; si2<7 && allCovered; ++si2) {
                        float su,sv; uvOf(tp[t], smp[si2], su, sv);
                        auto itc = cell.find(cellOf2(su,sv));
                        bool cov=false;
                        if (itc!=cell.end()) for (int c2 : itc->second) {
                            if (triDead[c2] || keepOrd[c2] >= keepOrd[t] || tp[c2].dom<0) continue;   // only EARLIER live sources cover
                            if (std::fabs(tp[c2].n[0]*smp[si2][0]+tp[c2].n[1]*smp[si2][1]+tp[c2].n[2]*smp[si2][2]-tp[c2].d) > PLANE_EPS) continue;
                            float au,av,bu,bv,cu2,cv2; uvOf(tp[c2],V(c2,0),au,av); uvOf(tp[c2],V(c2,1),bu,bv); uvOf(tp[c2],V(c2,2),cu2,cv2);
                            float d0=(bu-au)*(sv-av)-(bv-av)*(su-au);
                            float d1=(cu2-bu)*(sv-bv)-(cv2-bv)*(su-bu);
                            float d2=(au-cu2)*(sv-cv2)-(av-cv2)*(su-cu2);
                            bool neg=(d0<1e-6f)&&(d1<1e-6f)&&(d2<1e-6f), pos=(d0>-1e-6f)&&(d1>-1e-6f)&&(d2>-1e-6f);
                            if (neg||pos){ cov=true; break; }
                        }
                        allCovered = cov;
                    }
                    if (allCovered) { triDead[t]=1; ++coplDropped; }
                }
            }
            }   // fuseDropCovered
            std::vector<u32> fin; fin.reserve(keep.size());
            for (size_t t=0;t<nT;++t){ if (triDead[t]) continue; fin.push_back(keep[t*3]); fin.push_back(keep[t*3+1]); fin.push_back(keep[t*3+2]); }
            if (dropped||coplDropped) fprintf(stderr,"[EDIT] fuse: dropped %zu duplicate/degenerate + %zu COPLANAR-COVERED tris (z-fight elimination)\n", dropped, coplDropped);
            out.indices = std::move(fin);
        }
        out.nVerts=(u32)(out.positions.size()/3); out.nIdx=(u32)out.indices.size();   // uploadMesh trusts the EXPLICIT counts
        sceneMeshes->push_back(std::move(out));
        size_t ni=r->gpuMeshes.size();
        r->uploadMesh(sceneMeshes->back());
        if (r->gpuMeshes.size()!=ni+1){ setStatus("fuse: GPU upload failed"); sceneMeshes->pop_back(); return; }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        std::vector<int> hist=src; std::vector<uint8_t> histA(src.size(),1);
        for (int s : src) r->setDeleted((size_t)s, true);
        hist.push_back((int)ni); histA.push_back(0);
        pushDeleteUndo(hist, histA);                          // ONE Ctrl+Z un-fuses (sources back, fused gone)
        for (int s : src) remapMeshComponents(s, {(int)ni});  // colliders on any source follow the fused mesh
        meshEditLog[(int)ni] = "fuse(" + std::to_string(src.size()) + ")";
        geomDirty = true; holesMesh = -1;
        selectOne((int)ni); scrollToSel=true;
        setStatus("Fused "+std::to_string(src.size())+" meshes into one - "
                  +(nCells>1 ? (std::to_string(nCells)+" textures packed into a "+std::to_string(aw)+"x"+std::to_string(ah)+" atlas (UVs remapped)")
                             : std::string("shared texture kept as-is"))
                  +(skippedAnim? " - skipped "+std::to_string(skippedAnim)+" animated" : "")+" - Ctrl+Z un-fuses");
    }

    // HIDDEN per-mesh edit log: geometry ops NEVER rename the mesh (the outliner keeps the original name);
    // what happened is tracked here and shown only as a dim "edits: ..." line in the Object tab.
    std::map<int,std::string> meshEditLog;
    // ── GEOMETRY PERSISTENCE: meshes CREATED by the editor (fuse/cut/seal/extend/split/slice/duplicate
    //    results) exist only in RAM — without this sidecar they VANISHED on reload ("modified meshes are
    //    not saved"). Saved as <session>.geom next to the .hsledit; restored BEFORE the session lines
    //    apply so every index matches. Textures stored as PNG (the fused atlas compresses well).
    bool geomDirty = false;
    int  baseMeshCount = -1;   // how many meshes the ENV itself loaded (anything above = editor-created)
    std::thread geomThread; std::atomic<bool> geomSaving{false};
    bool geomAuth = false;   // a v2 .geom was loaded: it is authoritative for created meshes (idx >= baseMeshCount) - stale session lines must not override it   // sidecar writes run OFF the UI thread (PNG-encoding a fused 4K atlas froze the app for seconds)
    void saveGeomSidecar(const std::string& sessionFile){
        if (!r || !sceneMeshes || baseMeshCount < 0) return;
        // NEVER skip the write: the session .hsledit is written unconditionally and its MESH/DELETED lines
        // index INTO these meshes — a dropped sidecar write leaves a stale .geom whose mesh count no longer
        // matches the session, so on reload every index lands on the WRONG mesh (soft-deleted cut originals
        // come back = the "ghost meshes"). The previous write runs off-thread; joining it here is instant
        // in practice (it has had the whole autosave interval to finish).
        if (geomThread.joinable()) geomThread.join();
        std::string gp = sessionFile + ".geom";
        // SNAPSHOT the extra meshes fast (bulk vector copies), then encode + write on a WORKER thread.
        struct GeoSnap { std::string name; u8 flags[4]; u8 del; float eT[3],eR[4],eS[3],eTint[4]; u32 texW,texH; std::vector<u8> texRGBA;
                         std::vector<float> pos,uv,uv2; std::vector<u32> idx; std::string log; };
        auto snap = std::make_shared<std::vector<GeoSnap>>();
        for (int i=baseMeshCount; i<(int)sceneMeshes->size(); ++i){
            // DELETED created meshes are NOT persisted: the undo history dies with the session, so after a
            // restart they are unreachable garbage — yet every geometry op's superseded original was being
            // saved forever WITH its 4K PNG (treehouse's sidecar hit 402MB / 414 records, the write took so
            // long the process died mid-file = truncated sidecar = load crash).
            if (r->isDeleted((size_t)i)) continue;
            const MeshData& md=(*sceneMeshes)[(size_t)i];
            GeoSnap g; g.name=md.name;
            g.flags[0]=md.useBlend?1:0; g.flags[1]=md.additive?1:0; g.flags[2]=md.alphaTest?1:0; g.flags[3]=md.doubleSided?1:0;
            // v2 state byte: bit0 = deleted (always 0 now — deleted records aren't written; kept for
            // compatibility with the first v2 files), bit1 = collision-excluded (walk-through)
            g.del = noColMeshes.count(i)?2:0;
            if ((size_t)i < r->gpuMeshes.size()) { const VkGpuMesh& gm=r->gpuMeshes[(size_t)i];
                memcpy(g.eT,gm.editT,sizeof g.eT); memcpy(g.eR,gm.editR,sizeof g.eR);
                memcpy(g.eS,gm.editS,sizeof g.eS); memcpy(g.eTint,gm.editTint,sizeof g.eTint); }
            else { g.eT[0]=g.eT[1]=g.eT[2]=0; g.eR[0]=g.eR[1]=g.eR[2]=0; g.eR[3]=1; g.eS[0]=g.eS[1]=g.eS[2]=1; g.eTint[0]=g.eTint[1]=g.eTint[2]=g.eTint[3]=1; }
            g.texW=md.texW; g.texH=md.texH; g.texRGBA=md.texRGBA;
            g.pos=md.positions; g.uv=md.uvs; g.uv2=md.uvs2; g.idx=md.indices;
            auto it=meshEditLog.find(i); if (it!=meshEditLog.end()) g.log=it->second;
            snap->push_back(std::move(g));
        }
        if (snap->empty()) { std::error_code ec; std::filesystem::remove(gp, ec); geomDirty=false; return; }
        geomDirty = false; geomSaving.store(true);
        geomThread = std::thread([this, gp, snap]{
            // write to a TEMP file + atomic swap: a process killed mid-write (this is a multi-second job for
            // big scenes) must never leave a half-written .geom behind — that truncated file crashed loads.
            std::string tp = gp + ".tmp";
            FILE* f = fopen(tp.c_str(), "wb");
            if (f) {
                auto w32=[&](u32 v){ fwrite(&v,4,1,f); };
                auto wstr=[&](const std::string& s){ w32((u32)s.size()); fwrite(s.data(),1,s.size(),f); };
                fwrite("HSRGEOM2",8,1,f); w32((u32)snap->size());
                // slice/copy families all share ONE source texture — encode + store each distinct texture
                // ONCE; later records write the 0xFFFFFFFE marker + the record index that owns the pixels.
                std::unordered_map<unsigned long long,u32> texSeen;
                for (u32 ri=0; ri<(u32)snap->size(); ++ri){
                    auto& g = (*snap)[ri];
                    wstr(g.name); fwrite(g.flags,1,4,f);
                    fwrite(&g.del,1,1,f);
                    fwrite(g.eT,4,3,f); fwrite(g.eR,4,4,f); fwrite(g.eS,4,3,f); fwrite(g.eTint,4,4,f);
                    w32(g.texW); w32(g.texH);
                    if (g.texRGBA.empty() || !g.texW) { w32(0); }
                    else {
                        unsigned long long hh=1469598103934665603ull;
                        for (size_t k=0;k<g.texRGBA.size();k+=7) hh=(hh^g.texRGBA[k])*1099511628211ull;   // sampled FNV — 4K RGBA hashes fast
                        hh ^= (unsigned long long)g.texRGBA.size()*2654435761ull ^ ((unsigned long long)g.texW<<32);
                        auto it2=texSeen.find(hh);
                        if (it2!=texSeen.end()) { w32(0xFFFFFFFEu); w32(it2->second); }
                        else {
                            std::vector<u8> png;   // texture -> PNG in memory (the slow part - off-thread)
                            stbi_write_png_to_func([](void* ctx, void* d, int n){ auto* v=(std::vector<u8>*)ctx; v->insert(v->end(),(u8*)d,(u8*)d+n); },
                                                   &png, g.texW, g.texH, 4, g.texRGBA.data(), g.texW*4);
                            w32((u32)png.size()); if(!png.empty()) fwrite(png.data(),1,png.size(),f);
                            texSeen.emplace(hh, ri);
                        }
                    }
                    w32((u32)g.pos.size()); if(!g.pos.empty()) fwrite(g.pos.data(),4,g.pos.size(),f);
                    w32((u32)g.uv.size());  if(!g.uv.empty())  fwrite(g.uv.data(),4,g.uv.size(),f);
                    w32((u32)g.uv2.size()); if(!g.uv2.empty()) fwrite(g.uv2.data(),4,g.uv2.size(),f);
                    w32((u32)g.idx.size()); if(!g.idx.empty()) fwrite(g.idx.data(),4,g.idx.size(),f);
                    wstr(g.log);
                }
                fclose(f);
                std::error_code ec; std::filesystem::remove(gp, ec);   // Windows rename won't overwrite
                std::filesystem::rename(tp, gp, ec);
                if (ec) fprintf(stderr,"[EDIT] geometry sidecar swap FAILED: %s\n", ec.message().c_str());
                else fprintf(stderr,"[EDIT] geometry sidecar saved: %zu live editor-created mesh(es) -> %s\n", snap->size(), gp.c_str());
            }
            geomSaving.store(false);
        });
    }
    void loadGeomSidecar(const std::string& sessionFile){
        if (!r || !sceneMeshes) return;
        std::string gp = sessionFile + ".geom";
        FILE* f = fopen(gp.c_str(), "rb"); if (!f) return;
        char magic[8]; if (fread(magic,1,8,f)!=8 || memcmp(magic,"HSRGEOM",7)!=0 || (magic[7]!='1' && magic[7]!='2')){ fclose(f); return; }
        bool v2 = magic[7]=='2';   // v2 records carry deleted/nocol + editT/R/S/Tint (self-contained; no session-index dependence)
        // EVERY read is checked: a process killed mid-write used to leave a truncated file whose zero-filled
        // tail decoded as "empty mesh" records — uploadMesh(0 verts) crashed inside the driver. Any short or
        // insane read now stops the restore cleanly and keeps everything already loaded.
        bool ok=true;
        auto rd =[&](void* d, size_t n){ if (ok && fread(d,1,n,f)!=n) ok=false; };
        auto r32=[&]()->u32{ u32 v=0; rd(&v,4); return v; };
        auto rstr=[&]()->std::string{ u32 n=r32(); if(n>1u<<20){ ok=false; return {}; } std::string s(n,'\0'); if(n) rd(&s[0],n); return s; };
        u32 count=r32(); int restored=0;
        std::vector<int> recScene;   // file record -> sceneMeshes/gpu index (-1 = skipped/failed) for texture dedup refs
        for (u32 gi=0; gi<count && gi<4096 && ok; ++gi){
            MeshData md;
            md.name = rstr();
            u8 flags[4]={0,0,0,0}; rd(flags,4);
            md.useBlend=flags[0]!=0; md.additive=flags[1]!=0; md.alphaTest=flags[2]!=0; md.doubleSided=flags[3]!=0;
            u8 del=0; float eT[3]={0,0,0}, eR[4]={0,0,0,1}, eS[3]={1,1,1}, eTint[4]={1,1,1,1};
            if (v2){ rd(&del,1); rd(eT,12); rd(eR,16); rd(eS,12); rd(eTint,16); }
            md.texW=r32(); md.texH=r32();
            u32 pn=r32();
            int texFrom=-1;
            if (pn==0xFFFFFFFEu){   // dedup marker: pixels live in an earlier record
                u32 ref=r32(); if (ref<recScene.size()) texFrom=recScene[ref];
            } else if (pn>0x10000000u){ ok=false; }   // >256MB "PNG" = garbage/truncation
            else if (pn){ std::vector<u8> png(pn); rd(png.data(),pn);
                if (ok){ int w2=0,h2=0,n2=0; unsigned char* px=stbi_load_from_memory(png.data(),(int)pn,&w2,&h2,&n2,4);
                         if (px){ md.texRGBA.assign(px,px+(size_t)w2*h2*4); md.texW=(u32)w2; md.texH=(u32)h2; md.hasTexture=true; stbi_image_free(px); } } }
            auto rArrF=[&](std::vector<float>& v){ u32 n=r32(); if(n>100000000u){ ok=false; return; } v.resize(n); if(n) rd(v.data(),(size_t)n*4); };
            rArrF(md.positions); rArrF(md.uvs); rArrF(md.uvs2);
            { u32 n=r32(); if(n>100000000u) ok=false; else { md.indices.resize(n); if(n) rd(md.indices.data(),(size_t)n*4); } }
            std::string log = rstr();
            if (!ok){ fprintf(stderr,"[EDIT] geometry sidecar TRUNCATED at record %u/%u - ignoring the rest (%s)\n", gi, count, gp.c_str()); break; }
            if (texFrom>=0 && texFrom<(int)sceneMeshes->size()){ const MeshData& src=(*sceneMeshes)[(size_t)texFrom];
                md.texRGBA=src.texRGBA; md.texW=src.texW; md.texH=src.texH; md.hasTexture=src.hasTexture; }
            // old v2 files still contain soft-deleted records; they're unreachable (undo died with the
            // session) — parse past them but never upload. Degenerate/empty geometry is skipped too.
            if ((del&1) || md.positions.size()<9 || md.indices.size()<3){ recScene.push_back(-1); continue; }
            md.nVerts=(u32)(md.positions.size()/3); md.nIdx=(u32)md.indices.size();
            md.transform = Transform{}; md.hasWorldMatrix=true;
            static const float I16[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            memcpy(md.worldMatrix, I16, sizeof I16);
            sceneMeshes->push_back(std::move(md));
            size_t ni=r->gpuMeshes.size();
            r->uploadMesh(sceneMeshes->back());
            if (r->gpuMeshes.size()!=ni+1){ sceneMeshes->pop_back(); recScene.push_back(-1); continue; }
            if (v2){   // self-contained restore: transform + tint + collision-exclude (no session-line dependence)
                VkGpuMesh& ng=r->gpuMeshes[ni];
                memcpy(ng.editT,eT,sizeof eT); memcpy(ng.editR,eR,sizeof eR);
                memcpy(ng.editS,eS,sizeof eS); memcpy(ng.editTint,eTint,sizeof eTint);
                recomputeModel(ng);
                if (del&2) noColMeshes.insert((int)ni);
            }
            if (!log.empty()) meshEditLog[(int)ni]=log;
            recScene.push_back((int)ni);
            ++restored;
        }
        fclose(f);
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        if (v2) geomAuth = true;   // GEOM2 is AUTHORITATIVE for created meshes: session MESH/DELETED/MATF/
                                   // TEXOVR/NOCOL lines with idx >= baseMeshCount are stale (the compacted
                                   // sidecar re-orders live meshes) and must not be applied over it.
        if (restored) fprintf(stderr,"[EDIT] geometry sidecar restored %d editor-created mesh(es) from %s\n", restored, gp.c_str());
    }

    // shared tail of the geometry tools: append the edited clone (SAME name), carry the transform/tint,
    // soft-delete the original into the undo history, select the new mesh, log the edit invisibly.
    bool commitGeomEdit(int mi, MeshData&& md, const char* what){
        // uploadMesh trusts the EXPLICIT counts, not the array sizes — a stale nVerts/nIdx from the source
        // mesh made grown/carved results upload the WRONG vertex count = garbage/invisible geometry (the
        // treehouse fuse/seal bug). Sync them to what the arrays actually hold now.
        md.nVerts = (u32)(md.positions.size()/3);
        md.nIdx   = (u32)md.indices.size();
        sceneMeshes->push_back(std::move(md));
        size_t ni=r->gpuMeshes.size();
        r->uploadMesh(sceneMeshes->back());
        if (r->gpuMeshes.size()!=ni+1){ setStatus(std::string(what)+": GPU upload failed"); sceneMeshes->pop_back(); return false; }
        VkGpuMesh& ng=r->gpuMeshes[ni]; const VkGpuMesh& og=r->gpuMeshes[(size_t)mi];
        memcpy(ng.editT,og.editT,sizeof ng.editT); memcpy(ng.editR,og.editR,sizeof ng.editR); memcpy(ng.editS,og.editS,sizeof ng.editS);
        memcpy(ng.editTint,og.editTint,sizeof ng.editTint); recomputeModel(ng);
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        r->setDeleted((size_t)mi, true);
        pushDeleteUndo({mi,(int)ni}, std::vector<uint8_t>{1,0});   // ONE history entry: Ctrl+Z restores the original AND removes the result
        remapMeshComponents(mi, {(int)ni});                        // colliders/navmeshes on this mesh follow the edit
        { auto it=meshEditLog.find(mi); std::string prev = it!=meshEditLog.end()? it->second+"+" : std::string();
          meshEditLog[(int)ni] = prev + what; }
        selectOne((int)ni);
        scrollToSel = true;   // the result appends at the END of the outliner — jump to it
        holesMesh = -1;       // hole-inspector cache is stale after any geometry change
        geomDirty = true;     // geometry edits persist via the .geom sidecar on save/auto-save
        return true;
    }
    // Run a single-mesh geometry op over the WHOLE selection (each op selects its result; afterwards the
    // multi-selection is the set of results).
    void forEachSelMesh(const std::function<void(int)>& op){
        std::vector<int> src = sel.empty() ? (selected>=0 ? std::vector<int>{selected} : std::vector<int>{}) : sel;
        std::vector<int> results;
        for (int s : src) { if (s<0 || (r && r->isDeleted((size_t)s))) continue; op(s); if (selected>=0) results.push_back(selected); }
        if (results.size()>1) { sel=results; selected=results.back(); r->selectedMesh=selected; }
    }

    // ── mesh CUT HOLE: delete every triangle whose centroid lies within `radius` of the WORLD point
    //    (the spot you right-clicked). Makes doorways/entrances: the render gap opens AND the cooked
    //    collision follows the new geometry (re-bake), so you can walk through. Non-destructive.
    void cutMeshHole(int mi, const float wp[3], float radius){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData md = (*sceneMeshes)[(size_t)mi];
        if (md.indices.size() < 3) return;
        const float* M = r->gpuMeshes[(size_t)mi].model;
        float r2 = radius*radius;
        std::vector<u32> keep; keep.reserve(md.indices.size());
        int cut=0;
        auto W=[&](uint32_t v2, float o[3]){ const float* p=&md.positions[v2*3];
            o[0]=M[0]*p[0]+M[4]*p[1]+M[8]*p[2]+M[12]; o[1]=M[1]*p[0]+M[5]*p[1]+M[9]*p[2]+M[13]; o[2]=M[2]*p[0]+M[6]*p[1]+M[10]*p[2]+M[14]; };
        for (size_t k=0;k+2<md.indices.size();k+=3){
            uint32_t a=md.indices[k],b=md.indices[k+1],c=md.indices[k+2];
            float wa[3],wb[3],wc[3]; W(a,wa); W(b,wb); W(c,wc);
            float cx0=(wa[0]+wb[0]+wc[0])/3-wp[0], cy0=(wa[1]+wb[1]+wc[1])/3-wp[1], cz0=(wa[2]+wb[2]+wc[2])/3-wp[2];
            if (cx0*cx0+cy0*cy0+cz0*cz0 <= r2) { ++cut; continue; }   // inside the hole -> drop
            keep.push_back(a); keep.push_back(b); keep.push_back(c);
        }
        if (!cut) { setStatus("cut: no triangles within "+std::to_string(radius)+"m of the clicked point"); return; }
        md.indices = std::move(keep);
        if (commitGeomEdit(mi, std::move(md), "cut"))
            setStatus("Cut "+std::to_string(cut)+" tris (r="+std::to_string(radius)+"m) - re-bake/collision follows; original in history (Ctrl+Z)");
    }

    // ── mesh SPLIT: separate into CONNECTED PIECES. Connectivity is by vertex POSITION (quantized
    //    weld), NOT raw index sharing: meshes that duplicate vertices per triangle / per UV seam
    //    (most fused or game-ripped geometry) used to explode into "every triangle its own piece"
    //    (max-256 bail) or split along texture seams instead of the real islands. Degenerate
    //    (zero-area) triangles never glue two islands together (triangle-strip stitches did — many
    //    visually separate items came back as "ONE connected piece"). Each piece becomes its own
    //    mesh (same texture/material) so parts move/delete/collision-exclude independently.
    void splitMeshParts(int mi){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        const MeshData& src = (*sceneMeshes)[(size_t)mi];
        size_t nv = src.positions.size()/3;
        if (nv<3 || src.indices.size()<3) return;
        // 1) weld vertices by quantized position (epsilon scales with the mesh so huge terrains and
        //    tiny props both weld correctly) — duplicated seam/soup verts land on one weld node
        float wmn[3]={1e30f,1e30f,1e30f}, wmx[3]={-1e30f,-1e30f,-1e30f};
        for (size_t v2=0; v2<nv; ++v2) for (int a2=0;a2<3;++a2){ float p=src.positions[v2*3+a2]; wmn[a2]=std::min(wmn[a2],p); wmx[a2]=std::max(wmx[a2],p); }
        float wdiag=std::sqrt((wmx[0]-wmn[0])*(wmx[0]-wmn[0])+(wmx[1]-wmn[1])*(wmx[1]-wmn[1])+(wmx[2]-wmn[2])*(wmx[2]-wmn[2]));
        double weps=std::max(wdiag*1e-5f, 1e-6f);
        std::vector<uint32_t> weld(nv);
        { std::map<std::tuple<long long,long long,long long>,uint32_t> wcell;
          for (size_t v2=0; v2<nv; ++v2){
              auto key=std::make_tuple((long long)std::llround(src.positions[v2*3]  /weps),
                                       (long long)std::llround(src.positions[v2*3+1]/weps),
                                       (long long)std::llround(src.positions[v2*3+2]/weps));
              auto it=wcell.find(key);
              if (it!=wcell.end()) weld[v2]=it->second; else { weld[v2]=(uint32_t)v2; wcell.emplace(key,(uint32_t)v2); }
          } }
        // 2) union-find over the WELDED ids; degenerate tris (repeated welded vert = zero area) are
        //    skipped so strip stitches can't bridge islands
        std::vector<uint32_t> parent(nv); for (size_t i2=0;i2<nv;++i2) parent[i2]=(uint32_t)i2;
        std::function<uint32_t(uint32_t)> find = [&](uint32_t v2)->uint32_t{ while (parent[v2]!=v2){ parent[v2]=parent[parent[v2]]; v2=parent[v2]; } return v2; };
        for (size_t k=0;k+2<src.indices.size();k+=3){ uint32_t a=find(weld[src.indices[k]]),b=find(weld[src.indices[k+1]]),c=find(weld[src.indices[k+2]]);
            if (a==b||b==c||c==a) continue;   // degenerate/stitch: keep the tri, never glue through it
            parent[b]=a; parent[c]=a; }
        std::map<uint32_t,int> compOf; int nComp=0;
        for (size_t k=0;k<src.indices.size();k+=3){ uint32_t rt=find(weld[src.indices[k]]); if(!compOf.count(rt)) compOf[rt]=nComp++; }
        if (nComp<=1) { setStatus("split: mesh is ONE connected piece (by position) - use Cut region / Knife to divide it manually"); return; }
        if (nComp>256){ setStatus("split: "+std::to_string(nComp)+" pieces is too many (max 256) - Cut region a chunk out first"); return; }
        bool hasUV=src.uvs.size()>=nv*2, hasUV2=src.uvs2.size()>=nv*2, hasCol=src.colors.size()>=nv*4;
        std::vector<MeshData> parts((size_t)nComp);
        std::vector<std::map<uint32_t,uint32_t>> remap((size_t)nComp);
        for (int c2=0;c2<nComp;++c2){ parts[c2]=src;
            parts[c2].positions.clear(); parts[c2].uvs.clear(); parts[c2].uvs2.clear(); parts[c2].indices.clear();
            parts[c2].colors.clear(); parts[c2].uvs3.clear(); parts[c2].uvs4.clear();               // remapped below / dropped (stale copies would misalign)
            parts[c2].boneIndices.clear(); parts[c2].boneWeights.clear(); parts[c2].hasBones=false;
            // TEXTURE SHARING: every piece shares the ONE source texture/lightmap. Copying the source's (up to
            // 16MB) texRGBA + lightmap into each of up-to-256 pieces exploded memory = the split CRASH. FREE the
            // per-part copies and point texShareSrc at the source (kept in sceneMeshes, soft-deleted below) — the
            // renderer + cook read the bytes from there. dims/flags stay (copied) so uploadMesh sizes correctly.
            parts[c2].texShareSrc = mi;
            std::vector<u8>().swap(parts[c2].texRGBA);      std::vector<u8>().swap(parts[c2].astcRaw);
            std::vector<u8>().swap(parts[c2].srcAstc);      std::vector<u8>().swap(parts[c2].normalRGBA);
            std::vector<u8>().swap(parts[c2].ormRGBA);      std::vector<u8>().swap(parts[c2].emissiveRGBA);
            std::vector<u8>().swap(parts[c2].lmRGBA);       std::vector<u8>().swap(parts[c2].vatRaw);
            /* parts KEEP the original name (the [idx] suffix in the outliner disambiguates) */ }
        for (size_t k=0;k+2<src.indices.size();k+=3){
            int c2 = compOf[find(weld[src.indices[k]])]; auto& pt=parts[c2]; auto& rm=remap[c2];
            for (int e=0;e<3;e++){ uint32_t v2=src.indices[k+e];
                auto it=rm.find(v2); uint32_t nvi;
                if (it!=rm.end()) nvi=it->second;
                else { nvi=(uint32_t)(pt.positions.size()/3);
                       pt.positions.push_back(src.positions[v2*3]); pt.positions.push_back(src.positions[v2*3+1]); pt.positions.push_back(src.positions[v2*3+2]);
                       if (hasUV){ pt.uvs.push_back(src.uvs[v2*2]); pt.uvs.push_back(src.uvs[v2*2+1]); }
                       if (hasUV2){ pt.uvs2.push_back(src.uvs2[v2*2]); pt.uvs2.push_back(src.uvs2[v2*2+1]); }
                       if (hasCol){ for (int cc=0;cc<4;++cc) pt.colors.push_back(src.colors[v2*4+cc]); }   // vertex colors REMAPPED, not copied wholesale
                       rm[v2]=nvi; }
                pt.indices.push_back(nvi); } }
        for (auto& pt : parts) { pt.nVerts=(u32)(pt.positions.size()/3); pt.nIdx=(u32)pt.indices.size(); }   // uploadMesh trusts the explicit counts
        // append every piece (carry transform/tint), then soft-delete the original (ONE history entry
        // covering original + every part - Ctrl+Z removes the parts AND restores the original)
        const VkGpuMesh og = r->gpuMeshes[(size_t)mi];   // copy - gpuMeshes reallocates as parts upload
        int made=0;
        std::vector<int> hist{mi}; std::vector<uint8_t> histA{1};
        for (auto& pt : parts) {
            sceneMeshes->push_back(std::move(pt));
            size_t ni=r->gpuMeshes.size();
            r->uploadMesh(sceneMeshes->back());
            if (r->gpuMeshes.size()!=ni+1){ sceneMeshes->pop_back(); continue; }
            VkGpuMesh& ng=r->gpuMeshes[ni];
            memcpy(ng.editT,og.editT,sizeof ng.editT); memcpy(ng.editR,og.editR,sizeof ng.editR); memcpy(ng.editS,og.editS,sizeof ng.editS);
            memcpy(ng.editTint,og.editTint,sizeof ng.editTint); recomputeModel(ng); ++made;
            hist.push_back((int)ni); histA.push_back(0);
        }
        r->hiddenMeshes.resize(r->gpuMeshes.size(), false); r->deletedMeshes.resize(r->gpuMeshes.size(), false);
        r->setDeleted((size_t)mi, true); pushDeleteUndo(hist, histA);
        remapMeshComponents(mi, std::vector<int>(hist.begin()+1, hist.end()));   // colliders follow every piece
        deselectAll(); geomDirty = true; holesMesh = -1;
        setStatus("Split into "+std::to_string(made)+" pieces - move/delete/collision-exclude them independently (Ctrl+Z reverts)");
    }

    // ── TEXTURE edit: set a mesh's (e.g. the SKYBOX dome's) base texture from a PNG/JPG image, and export it back ──
    // The decoded RGBA replaces md.texRGBA (what the COOK ships) AND live-re-uploads the GPU texture (preview). The
    // source path is remembered (texOverride) so it re-applies on session load and cooks from a fresh open.
    std::map<int,std::string> texOverride;
    bool setMeshTexture(int mi, const std::string& path){
        if(!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()){ setStatus("set texture: bad mesh "+std::to_string(mi)); return false; }
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ setStatus("texture load FAILED (open): "+path); return false; }
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        std::vector<uint8_t> bytes((size_t)(sz>0?sz:0)); if(sz>0){ size_t rd=fread(bytes.data(),1,(size_t)sz,f); (void)rd; } fclose(f);
        int w=0,h=0,n=0; unsigned char* px=stbi_load_from_memory(bytes.data(),(int)bytes.size(),&w,&h,&n,4);
        if(!px){ setStatus("texture decode FAILED (PNG/JPG only): "+path); return false; }
        backupTexture(mi);   // "Undo texture" can take this back
        MeshData& md=(*sceneMeshes)[mi];
        md.texRGBA.assign(px, px+(size_t)w*h*4); md.texW=w; md.texH=h; md.hasTexture=true;
        stbi_image_free(px);
        bool ok = r->replaceMeshTexture((size_t)mi, *sceneMeshes);   // live preview re-upload + descriptor rebind
        texOverride[mi]=path;
        setStatus(ok ? ("Set texture ["+std::to_string(mi)+"] "+std::to_string(w)+"x"+std::to_string(h)+" from "+path+"  (previews + cooks)")
                     : ("Texture set on CPU (will cook) but preview re-upload failed: "+path));
        return true;
    }
    // ── AI texture prompt: every exported PNG comes with a ready-to-paste ChatGPT prompt (copied to
    //    the CLIPBOARD + written as <png>_PROMPT.txt beside the file). The guardrails keep the AI's
    //    result mappable straight back onto the mesh via Set texture: exact size, nothing moved or
    //    re-framed, palette kept, edges still tile, alpha preserved. Patch meshes get an INPAINT
    //    prompt instead (the smooth pin-color blend region IS the hole to repaint).
    std::string aiTexturePrompt(const MeshData& md) const {
        bool isPatch = md.name.rfind("patch",0)==0;
        char head[176]; snprintf(head,sizeof head,
            "I'm attaching a %dx%d PNG texture from a 3D game environment%s.\n\n",
            (int)md.texW,(int)md.texH, isPatch?" (an inpainting template)":"");
        std::string p(head);
        if (isPatch)
            p += "It was auto-baked around a HOLE in the scene: the detailed imagery is real surrounding "
                 "surface sampled from the 3D scene, and the smooth/blurry gradient region is a placeholder "
                 "covering the actual gap.\n"
                 "TASK: repaint ONLY the smooth gradient region with plausible detail that seamlessly "
                 "continues the surrounding imagery - match its colors, texture grain, lighting direction "
                 "and scale. Leave every already-detailed pixel exactly as it is.\n\n";
        else
            p += "TASK: improve this texture - increase detail and sharpness, remove compression artifacts, "
                 "noise and blur, and make surfaces read cleanly, while keeping the SAME look so it still "
                 "matches the rest of the scene.\n\n";
        p += "HARD RULES (the image is UV-mapped onto a 3D mesh, so pixel POSITIONS matter):\n"
             "1. Same resolution and aspect ratio as the original (an exact 2x upscale is also fine).\n"
             "2. Do NOT move, crop, rotate, mirror, re-center or re-frame anything; no added borders, "
             "margins, vignettes, watermarks or text.\n"
             "3. Keep the original color palette and overall brightness - no artistic re-grade.\n"
             "4. If the texture tiles, keep the edges wrap-compatible: the left edge must still continue "
             "into the right edge, and the top into the bottom.\n"
             "5. Preserve transparency exactly: fully transparent areas stay fully transparent, hard "
             "alpha cut-out edges stay hard.\n"
             "6. If you see separate islands (a texture ATLAS), improve each island in place - never "
             "paint across island boundaries.\n"
             "7. Return the result as a downloadable PNG (no JPEG re-compression).\n";
        return p;
    }
    void writeAiPromptSidecar(const MeshData& md, const std::string& pngPath){
        std::string ptxt = aiTexturePrompt(md);
        size_t dot = pngPath.find_last_of('.');
        std::string pp = (dot==std::string::npos ? pngPath : pngPath.substr(0,dot)) + "_PROMPT.txt";
        if (FILE* pf=fopen(pp.c_str(),"wb")){ fwrite(ptxt.data(),1,ptxt.size(),pf); fclose(pf); }
        if (win) glfwSetClipboardString(win, ptxt.c_str());
    }
    bool exportMeshTexture(int mi, const std::string& path){
        if(!sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()){ setStatus("export texture: bad mesh "+std::to_string(mi)); return false; }
        const MeshData& md=(*sceneMeshes)[mi];
        if(!md.hasTexture || md.texRGBA.size()<(size_t)md.texW*md.texH*4 || md.texW<1||md.texH<1){ setStatus("export texture: mesh "+std::to_string(mi)+" has none"); return false; }
        int ok=stbi_write_png(path.c_str(), md.texW, md.texH, 4, md.texRGBA.data(), md.texW*4);
        if (ok) writeAiPromptSidecar(md, path);
        setStatus(ok ? ("Exported texture ["+std::to_string(mi)+"] "+std::to_string(md.texW)+"x"+std::to_string(md.texH)+" -> "+path+"  |  ChatGPT prompt copied to clipboard (+_PROMPT.txt)")
                     : ("texture export FAILED: "+path));
        return ok!=0;
    }
    // re-apply saved texture overrides after a session load (so the swapped skybox/texture previews + cooks)
    void applyTexOverrides(){ auto ov=texOverride; for(auto& kv:ov) setMeshTexture(kv.first, kv.second); }

    // ── GENERAL SKYBOX color (works for ANY env): drives the renderer BACKGROUND (clearRGB) for the live preview and
    //    persists in the session. skyColor[0] < 0 => "unset" (keep the env's own sky). The cook target is a
    //    SkyboxPlatformComponent whose colorTexture is a solid-color (or image) texture — see project_hsr_v205_skybox_system.
    float skyColor[3] = {-1.f,-1.f,-1.f};
    bool skyColorSet() const { return skyColor[0] >= 0.f; }
    // Set ONLY the background/skybox color (clearRGB). Does NOT touch any env mesh — the env's own dome/geometry
    // stays exactly as authored; the skybox color is the SEPARATE background layer behind everything (cook target =
    // a SkyboxPlatformComponent, see project_hsr_v205_skybox_system). General: applies to ANY env's background.
    void setSkyColor(float rr,float gg,float bb){
        skyColor[0]=rr; skyColor[1]=gg; skyColor[2]=bb;
        if(r){ r->clearRGB[0]=rr; r->clearRGB[1]=gg; r->clearRGB[2]=bb; }
        char b[112]; snprintf(b,sizeof b,"Skybox/background color = (%.3f, %.3f, %.3f) - background layer only, env meshes untouched",rr,gg,bb);
        setStatus(b);
    }
    void clearSkyColor(){ skyColor[0]=skyColor[1]=skyColor[2]=-1.f; setStatus("Skybox color cleared"); }
    // Optional SKYBOX IMAGE (equirect panorama) — cooked as the texture on a large inward sky sphere (general, any env).
    std::vector<uint8_t> skyImageRGBA; int skyImageW=0, skyImageH=0; std::string skyImagePath;
    bool setSkyImage(const std::string& path){
        FILE* f=fopen(path.c_str(),"rb"); if(!f){ setStatus("skybox image FAILED (open): "+path); return false; }
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        std::vector<uint8_t> bytes((size_t)(sz>0?sz:0)); if(sz>0){ size_t rd=fread(bytes.data(),1,(size_t)sz,f); (void)rd; } fclose(f);
        int w=0,h=0,n=0; unsigned char* px=stbi_load_from_memory(bytes.data(),(int)bytes.size(),&w,&h,&n,4);
        if(!px){ setStatus("skybox image decode FAILED (PNG/JPG): "+path); return false; }
        skyImageRGBA.assign(px,px+(size_t)w*h*4); skyImageW=w; skyImageH=h; stbi_image_free(px); skyImagePath=path;
        previewSkybox();
        setStatus("Skybox image set "+std::to_string(w)+"x"+std::to_string(h)+" (equirect) - previews + cooks onto a sky sphere");
        return true;
    }
    void clearSkyImage(){ skyImageRGBA.clear(); skyImageW=skyImageH=0; skyImagePath.clear(); previewSkybox(); setStatus("Skybox image cleared"); }
    // Skybox from an EXISTING TEXTURE — reuse any mesh's texture (e.g. the env's own sky dome, mesh 7) as the skybox,
    // no external file needed. This is the "skybox could be a TEXTURE" path (vs an imported image or a flat color).
    bool setSkyImageFromMesh(int mi){
        if(!sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()){ setStatus("skybox-from-texture: bad mesh "+std::to_string(mi)); return false; }
        const MeshData& md=(*sceneMeshes)[mi];
        if(!md.hasTexture || md.texRGBA.size()<(size_t)md.texW*md.texH*4 || md.texW<1||md.texH<1){ setStatus("mesh "+std::to_string(mi)+" has no texture to use as skybox"); return false; }
        skyImageRGBA=md.texRGBA; skyImageW=md.texW; skyImageH=md.texH; skyImagePath="mesh:"+std::to_string(mi);
        previewSkybox();
        setStatus("Skybox = texture of mesh ["+std::to_string(mi)+"] "+std::to_string(md.texW)+"x"+std::to_string(md.texH)+" - previews + cooks onto a sky sphere");
        return true;
    }
    // Export the CURRENT skybox texture (imported image / reused texture) to a PNG. "export skybox texture if any."
    bool exportSkyImage(const std::string& path){
        if(skyImageRGBA.size()<(size_t)skyImageW*skyImageH*4 || skyImageW<1||skyImageH<1){ setStatus("no skybox texture to export"); return false; }
        int ok=stbi_write_png(path.c_str(), skyImageW, skyImageH, 4, skyImageRGBA.data(), skyImageW*4);
        setStatus(ok?("Exported skybox texture "+std::to_string(skyImageW)+"x"+std::to_string(skyImageH)+" -> "+path):("skybox export FAILED: "+path));
        return ok!=0;
    }
    bool skyboxSet() const { return skyColorSet() || !skyImageRGBA.empty(); }
    // Build the GENERAL skybox mesh (a large inward-facing UV sphere with the solid color / equirect image) as an
    // ExportMesh — appended to the cook so it ships as a NORMAL far mesh (behind everything; the 150000 farClip covers
    // it; avoids the SkyboxPlatformComponent cubemap-black issue). Renders in preview too (it's a normal mesh).
    void appendSkyboxMesh(std::vector<hslcook::ExportMesh>& ems){
        if(!skyboxSet()) return;
        using namespace hslcook; ExportMesh sky; sky.name="generated_skybox"; sky.doubleSided=true;
        const int LAT=24, LON=48; const float R=8000.f; const float PI=3.14159265358979f;
        for(int y=0;y<=LAT;y++){ float v=(float)y/LAT, th=v*PI;
            for(int x=0;x<=LON;x++){ float u=(float)x/LON, ph=u*2.f*PI;
                sky.positions.push_back(sinf(th)*cosf(ph)*R); sky.positions.push_back(cosf(th)*R); sky.positions.push_back(sinf(th)*sinf(ph)*R);
                sky.uvs.push_back(u); sky.uvs.push_back(v); } }
        for(int y=0;y<LAT;y++) for(int x=0;x<LON;x++){ uint32_t a=y*(LON+1)+x,b=a+1,c=a+(LON+1),d=c+1;
            sky.indices.push_back(a); sky.indices.push_back(b); sky.indices.push_back(c);
            sky.indices.push_back(b); sky.indices.push_back(d); sky.indices.push_back(c); }
        if(!skyImageRGBA.empty()){ sky.rgba=skyImageRGBA; sky.w=skyImageW; sky.h=skyImageH; }
        else { const int N=8; sky.w=N; sky.h=N; sky.rgba.resize((size_t)N*N*4);
            uint8_t cr=(uint8_t)(skyColor[0]*255.f+0.5f),cg=(uint8_t)(skyColor[1]*255.f+0.5f),cb=(uint8_t)(skyColor[2]*255.f+0.5f);
            for(int p=0;p<N*N;p++){ sky.rgba[p*4]=cr; sky.rgba[p*4+1]=cg; sky.rgba[p*4+2]=cb; sky.rgba[p*4+3]=255; } }
        ems.push_back(std::move(sky));
    }
    // ── LIVE PREVIEW of the skybox image/texture: upload the sky sphere as a real gpuMesh so it shows in the viewport
    //    (a flat color already previews via clearRGB; an image/texture needs the sphere). Re-set just swaps the texture.
    int skyPreviewMesh=-1;   // gpuMesh index of the live skybox sphere (-1 = none); when >=0 the cook uses IT (no double-emit)
    void previewSkybox(){
        if(!r || !sceneMeshes) return;
        if(skyImageRGBA.empty()){ if(skyPreviewMesh>=0) r->setDeleted((size_t)skyPreviewMesh,true); return; }   // image/texture only; color = clearRGB
        // (re)build the texture; geometry is fixed so re-set just replaces the texture on the existing sphere
        if(skyPreviewMesh>=0 && skyPreviewMesh<(int)sceneMeshes->size()){
            MeshData& pm=(*sceneMeshes)[(size_t)skyPreviewMesh];
            pm.texRGBA=skyImageRGBA; pm.texW=skyImageW; pm.texH=skyImageH; pm.hasTexture=true;
            r->setDeleted((size_t)skyPreviewMesh,false);
            r->replaceMeshTexture((size_t)skyPreviewMesh, *sceneMeshes);
            return;
        }
        if(sceneMeshes->empty()) return;
        // CLONE a working static mesh as the template (fully-initialized fields), then OVERWRITE geometry+texture.
        // (A hand-built MeshData crashed uploadMesh — a field it dereferences was empty; cloning avoids that, like dupmesh.)
        MeshData md=(*sceneMeshes)[0];
        md.name="generated_skybox"; md.doubleSided=true; md.hasTexture=true;
        md.colors.clear(); md.uvs2.clear(); md.normalRGBA.clear(); md.hasNormal=false; md.lmRGBA.clear(); md.hasLightmap=false;
        md.emissiveRGBA.clear(); md.hasEmissive=false; md.astcRaw.clear(); md.vatRaw.clear(); md.hasVat=false;
        md.boneIndices.clear(); md.boneWeights.clear(); md.hasBones=false; md.bonePalette.clear();
        md.useBlend=false; md.additive=false; md.alphaTest=false; md.dynamicVerts=false;
        md.positions.clear(); md.uvs.clear(); md.indices.clear();
        const int LAT=24,LON=48; const float R=8000.f, PI=3.14159265358979f;
        for(int y=0;y<=LAT;y++){ float v=(float)y/LAT, th=v*PI; for(int x=0;x<=LON;x++){ float u=(float)x/LON, ph=u*2*PI;
            md.positions.push_back(sinf(th)*cosf(ph)*R); md.positions.push_back(cosf(th)*R); md.positions.push_back(sinf(th)*sinf(ph)*R);
            md.uvs.push_back(u); md.uvs.push_back(v); } }
        for(int y=0;y<LAT;y++) for(int x=0;x<LON;x++){ uint32_t a=y*(LON+1)+x,b=a+1,c=a+(LON+1),d=c+1;
            md.indices.push_back(a); md.indices.push_back(b); md.indices.push_back(c);
            md.indices.push_back(b); md.indices.push_back(d); md.indices.push_back(c); }
        md.texRGBA=skyImageRGBA; md.texW=skyImageW; md.texH=skyImageH;
        sceneMeshes->push_back(md); skyPreviewMesh=(int)r->gpuMeshes.size();
        r->uploadMesh(sceneMeshes->back());
        r->hiddenMeshes.resize(r->gpuMeshes.size(),false); r->deletedMeshes.resize(r->gpuMeshes.size(),false);
    }
    // ── per-mesh "skybox backdrop" marks: cooked as SkyboxPlatformComponent (far-clip-EXEMPT, escapes the PortalStereoCamera
    //    far=5000 clip). Set via right-click; multi-select applies to the whole selection. [[project_hsr_eye_subcamera_farclip]]
    std::vector<int> skyboxMeshes;
    bool isSkyboxMesh(int i) const { for (int s:skyboxMeshes) if(s==i) return true; return false; }
    void toggleSkybox(int i){ if(i<0)return; for(size_t k=0;k<skyboxMeshes.size();++k) if(skyboxMeshes[k]==i){ skyboxMeshes.erase(skyboxMeshes.begin()+k); return; } skyboxMeshes.push_back(i); }
    // ── rubber-band box select (Ctrl/Shift + left-drag in the viewport; plain left-drag stays camera-look) ──
    bool boxSel=false; float boxX0=0,boxY0=0;
    int  outlinerDragRow=-1;       // outliner drag-select: row where the left-drag started (-1 = none)
    bool outlinerDragAdd=false;    // drag-select adds (Ctrl/Shift) vs replaces
    static bool isBackdrop(const std::string& n){ auto h=n; for (auto& c:h) c=(char)tolower(c); return h.find("sky")!=std::string::npos||h.find("backdrop")!=std::string::npos||h.find("skybox")!=std::string::npos; }
    char search[96] = "";
    bool prevKeyA=false, prevKeyC=false;   // Ctrl+A (select-all-filtered) / Ctrl+C (copy names) edge-detect
    float outlinerScroll = 0.f, propScroll = 0.f;
    bool  scrollToSel = false;
    bool  didAutoSel = false;      // auto-select a centered object once (frame 1)
    // right-click context menu
    bool  ctxOpen = false; float ctxX = 0, ctxY = 0; int ctxMesh = -1;
    int   ctxItem = -1;   // >=0: the context menu targets THIS scene item (outliner right-click), not a mesh
    float ctxRX = 0, ctxRY = 0, ctxRW = 0, ctxRH = 0;   // its on-screen rect (so clicks route to it)
    int   ctxSub = -1; float ctxSubY = 0;                 // open submenu index (-1 none) + its anchor row Y
    float ctxR2X = 0, ctxR2Y = 0, ctxR2W = 0, ctxR2H = 0; // the submenu panel's rect (0-wide when closed)
    bool  insideCtx(float x, float y) const { return (x>=ctxRX && y>=ctxRY && x<ctxRX+ctxRW && y<ctxRY+ctxRH)
                                                  || (ctxR2W>0 && x>=ctxR2X && y>=ctxR2Y && x<ctxR2X+ctxR2W && y<ctxR2Y+ctxR2H); }
    // outliner section collapse (click the SCENE ITEMS / MESHES headers)
    bool  itemsCollapsed = false, meshesCollapsed = false;
    // progressive GPU upload progress (main sets these while streaming; the viewport draws a loading bar)
    int   uploadCur = 0, uploadTotal = 0;
    bool  showKeybinds = false;        // F1: the all-shortcuts overlay
    std::vector<int> meshClipboard;    // Ctrl+C on meshes stores the selection; Ctrl+V pastes independent clones
    std::vector<int> selItems;         // scene-item MULTI-selection (list ops: hide/space/Del/Remove); selItem = active
    // IN-PLACE env swap (NO process restart): setting swapTo makes main tear down the scene and load this
    // path in the SAME window - cook preview (any source -> its cooked HSL APK) and back again.
    std::string swapTo;
    std::string sourceEnvPath;         // the ORIGINAL env this session started from ("Back to source")
    float extendDist = 1.0f;           // manual extend distance (Object tab "Geometry" section)
    float cutRadius  = 1.0f;           // manual cut-hole radius (Object tab "Geometry" section)
    std::set<int> noColMeshes;         // meshes EXCLUDED from collision (walk-through); persisted as NOCOL
    float ctxHitP[3] = {0,0,0}; bool ctxHitValid = false;   // world point under the cursor when the ctx menu opened (cut-hole target)
    bool  popupAtePress = false;   // a popup consumed the left PRESS -> its RELEASE must not pick a mesh behind the menu

    // ── scene items (spawn/chair/collider/navmesh/wall/hotspot — addable/removable/positionable/cookable) ──
    std::vector<sitem::Item> items;
    std::vector<int> animColliders;   // mesh indices marked as ANIMATED colliders (same-entity kinematic collider in the cook)
    // ── player simulator (walk the env in-editor to test navmesh/spawn/floor; the cam is glued to the walkable surface) ──
    bool playSim = false; float pVelY = 0.f;
    std::vector<float> simV; std::vector<uint32_t> simI;   // cached walkable triangles for the sim
    int  selItem = -1;             // index into items (-1 = none; mesh selection active instead)
    float lastItemClickT = -1.f;   // for double-click-to-focus detection (single click = select only, no camera teleport)
    bool alwaysOnTop = false;       // window always-on-top toggle (OFF by default — see the "Pin" pill in the viewport header)
    bool chairTiltLock = true;      // chairs rotate YAW-only (stay upright); the gizmo won't tip them over. Toggle in chair props.
    bool xrayMesh = false;          // "X-ray" overlay: draw the SELECTED mesh(es) wireframe ALWAYS-on-top so you can see
                                    // exactly where the real mesh is vs a collider box (the boxes draw on top, so from some
                                    // camera angles a box LOOKS aligned with its mesh but isn't — this removes that ambiguity).
    bool showItems = true;         // item markers visible (the things you add are always shown)
    bool showFarClip = false;      // viewport overlay: draw the device far-clip (PortalStereoCamera far=5000) boundary sphere
    bool showType[sitem::TYPE_COUNT] = { true,true,true,true,true,true,true };   // per-Meta-component visibility toggles
    bool showMeshCol = false;      // DEDICATED viewport toggle for ADDED mesh colliders (markers + gizmo). DEFAULT OFF so a freshly-loaded env (which restores saved per-mesh colliders) isn't cluttered with red collider overlays on launch — toggle "Collision" on to edit them. (Adding a navmesh flips it on so you see what you just placed.)
    // distinct marker colours (deliberately AVOID the gizmo's R/G/B axes): cyan / orange / magenta / teal / purple / yellow
    uint32_t typeColor(int t, bool seld) const {
        switch (t) {
          case sitem::SPAWN:     return seld?ui::rgba(120,240,255):ui::rgba(0,200,235);    // cyan
          case sitem::CHAIR:     return seld?ui::rgba(255,185,95):ui::rgba(255,150,40);    // orange
          case sitem::BOXCOL:    return seld?ui::rgba(255,130,220):ui::rgba(235,80,190);   // magenta
          case sitem::NAVMESH:   return seld?ui::rgba(120,255,130):ui::rgba(60,225,80);     // green (haven2025 navmesh)
          case sitem::WALLPLACE: return seld?ui::rgba(200,140,255):ui::rgba(170,90,240);   // purple
          case sitem::HOTSPOT:   return seld?ui::rgba(255,235,120):ui::rgba(240,210,60);   // yellow
          case sitem::BOUNDARY:  return seld?ui::rgba(255,120,110):ui::rgba(235,70,60);    // red (kill floor)
        } return ui::rgba(220,220,220);
    }
    bool addMenuOpen = false; float addMenuX = 0, addMenuY = 0;
    bool langMenuOpen = false; float langBtnX=0, langBtnY=0, langBtnW=0, langBtnH=0, langMenuW=0, langMenuH=0;   // language dropdown (GitHub #10)
    // Raycast straight DOWN through the scene and snap pos.y onto the floor beneath it, so a freshly-added ground
    // item (spawn/chair/box) lands ON the floor — not floating at the height an overview camera happened to be.
    void dropToFloor(float pos[3]){
        float bestY=-1e30f; bool hit=false;
        for (int mi=0; mi<(int)r->gpuMeshes.size(); ++mi){
            if (r->isHidden(mi) || isBackdrop(r->gpuMeshes[mi].name)) continue;
            auto& gm=r->gpuMeshes[mi]; const auto& P=gm.pickPos; const auto& I=gm.pickIdx;
            if (P.size()<9||I.size()<3) continue;
            for (size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size()) continue;
                float pa[3],pb[3],pc[3]; xformPoint(gm.model,&P[a*3],pa); xformPoint(gm.model,&P[b*3],pb); xformPoint(gm.model,&P[c*3],pc);
                float d=(pb[2]-pc[2])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[2]-pc[2]); if (std::fabs(d)<1e-9f) continue;
                float u=((pb[2]-pc[2])*(pos[0]-pc[0])+(pc[0]-pb[0])*(pos[2]-pc[2]))/d;
                float v=((pc[2]-pa[2])*(pos[0]-pc[0])+(pa[0]-pc[0])*(pos[2]-pc[2]))/d;
                float w=1.f-u-v; if (u<-0.02f||v<-0.02f||w<-0.02f) continue;        // pos XZ inside this tri
                float y=u*pa[1]+v*pb[1]+w*pc[1];
                if (y < pos[1]-0.3f && y>bestY){ bestY=y; hit=true; }                // highest surface BELOW the item (excludes a ceiling near/above it)
            }
        }
        if (hit) pos[1]=bestY;
    }
    void addItem(int type) {
        if (type==sitem::NAVMESH) { addNavmesh(sel.empty()?1:2); return; }   // navmesh has its own mode-aware path
        pushItemUndo(items);   // undoable add
        sitem::Item it; it.type=type; it.name=std::string(sitem::typeName(type))+" "+std::to_string(items.size()+1);
        float hp[3]; bool closeHit=false;
        // Drop it where you're LOOKING only if that surface is reasonably CLOSE. A far hit (you were looking at the
        // vista/backdrop) or nothing in view -> a SET distance ahead, so objects never land out in the void
        // (ToastConcern: a chair spawned at z=-92 because the forward ray hit a distant mesh).
        if (cameraForwardHit(hp)) { float dx=hp[0]-r->cam.pos[0],dy=hp[1]-r->cam.pos[1],dz=hp[2]-r->cam.pos[2];
                                    closeHit = (dx*dx+dy*dy+dz*dz) <= 15.f*15.f; }
        if (closeHit) { it.pos[0]=hp[0]; it.pos[1]=hp[1]; it.pos[2]=hp[2]; }
        else { float cp=std::cos(r->cam.pitch); float fwd[3]={std::sin(r->cam.yaw)*cp, std::sin(r->cam.pitch), -std::cos(r->cam.yaw)*cp};
               it.pos[0]=r->cam.pos[0]+fwd[0]*3.f; it.pos[1]=r->cam.pos[1]+fwd[1]*3.f; it.pos[2]=r->cam.pos[2]+fwd[2]*3.f; }  // 3 m ahead of the camera
        if (type==sitem::SPAWN||type==sitem::CHAIR||type==sitem::BOXCOL) dropToFloor(it.pos);   // land ground items ON the floor (not floating at overview-camera height)
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; tab=TAB_OBJECT; propScroll=0.f;   // reset scroll: the +Add buttons sit at the BOTTOM of the long Scene panel, so without this the new item's editor rendered scrolled off-screen ("had to switch tabs to see it")
    }
    // Create a Meta Component DERIVED from a mesh's world geometry (right-click -> Make ...). Each type is fitted to the
    // mesh so it's immediately solid/usable instead of a default placeholder you then hand-size.
    void addItemFromMesh(int type, int meshIdx){
        if (meshIdx<0 || meshIdx>=(int)r->gpuMeshes.size()) return;
        pushItemUndo(items);
        float mn[3],mx[3]; worldAabb(r->gpuMeshes[meshIdx],mn,mx);
        float C[3]={(mn[0]+mx[0])*0.5f,(mn[1]+mx[1])*0.5f,(mn[2]+mx[2])*0.5f};
        float H[3]={std::max(0.02f,(mx[0]-mn[0])*0.5f),std::max(0.02f,(mx[1]-mn[1])*0.5f),std::max(0.02f,(mx[2]-mn[2])*0.5f)};
        sitem::Item it; it.type=type; it.name=std::string(sitem::typeName(type))+" "+std::to_string(items.size()+1);
        it.pos[0]=C[0]; it.pos[1]=C[1]; it.pos[2]=C[2];
        switch(type){
          case sitem::BOXCOL:                                       // a box collider that wraps the whole mesh
            it.half[0]=H[0]; it.half[1]=H[1]; it.half[2]=H[2]; break;
          case sitem::WALLPLACE: {
            // FIT THE WALL SNUGLY TO THE MESH ITSELF — never move it to the camera. IDA (V205.2):
            // horizon::hpi::WallPlacementComponent is a UserComponent with only propRank/propMaxWidth/propMaxHeight; its
            // facing is purely the ENTITY TRANSFORM. Walls stand vertical, so keep world-up and solve the horizontal
            // orientation from a 2D PCA of the mesh's footprint in XZ: the major axis = the wall's LENGTH, its
            // perpendicular = the facing normal. pos = mesh CENTER, propW/H = the mesh's OWN extents. (The camera only
            // picks WHICH of the two faces the props live on — the position never leaves the mesh.)
            auto& gm=r->gpuMeshes[meshIdx]; const auto& Pv=gm.pickPos; int nv=0; double sx=0,sz=0;
            for(size_t k=0;k+2<Pv.size();k+=3){ float w[3]; xformPoint(gm.model,&Pv[k],w); sx+=w[0]; sz+=w[2]; ++nv; }
            if(nv>0){
                double mxc=sx/nv, mzc=sz/nv, a=0,b=0,cc=0;
                for(size_t k=0;k+2<Pv.size();k+=3){ float w[3]; xformPoint(gm.model,&Pv[k],w); double dx=w[0]-mxc,dz=w[2]-mzc; a+=dx*dx; b+=dx*dz; cc+=dz*dz; }
                double theta=0.5*std::atan2(2.0*b, a-cc);              // major-axis (wall length) angle in XZ
                double ux=std::cos(theta), uz=std::sin(theta), nx=-uz, nz=ux;   // length dir u, facing normal n (perp)
                double hl=0; for(size_t k=0;k+2<Pv.size();k+=3){ float w[3]; xformPoint(gm.model,&Pv[k],w); double dx=w[0]-mxc,dz=w[2]-mzc; double pl=std::fabs(dx*ux+dz*uz); if(pl>hl)hl=pl; }
                double cdx=r->cam.pos[0]-mxc, cdz=r->cam.pos[2]-mzc; if(nx*cdx+nz*cdz<0){ nx=-nx; nz=-nz; }   // props face the player; pos unchanged
                it.pos[0]=(float)mxc; it.pos[1]=C[1]; it.pos[2]=(float)mzc;       // SNUG: dead-center of the mesh
                it.rot[0]=0.f; it.rot[1]=(float)(std::atan2(nx,-nz)*57.29577951); it.rot[2]=0.f;
                it.propW=std::max(0.2f,(float)(2.0*hl)); it.propH=std::max(0.2f,2*H[1]);
            } else { it.propW=std::max(0.2f,std::max(2*H[0],2*H[2])); it.propH=std::max(0.2f,2*H[1]); }   // no verts -> AABB fallback
            break; }
          case sitem::SPAWN:   it.pos[1]=mx[1]; it.allowStart=true; it.isLocal=true; break;   // stand on top of the mesh
          case sitem::CHAIR:   it.pos[1]=mx[1]; break;                                        // seat on top
          case sitem::BOUNDARY:it.pos[1]=mn[1]; break;                                        // kill-floor plane at the mesh base
          default: break;                                                                    // HOTSPOT etc. -> mesh center
        }
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; tab=TAB_OBJECT; propScroll=0.f;
        setStatus(std::string("Created ")+sitem::typeName(type)+" from '"+r->gpuMeshes[meshIdx].name+"'");
    }
    void deleteSelItem() { if (selItem>=0 && selItem<(int)items.size()) { pushItemUndo(items); items.erase(items.begin()+selItem); selItem=-1; } }
    // Import a V79 env's assets/markup.json Locators as scene items: portal->Spawn, seat-hotspots->Chair,
    // other hotspots/mirrors/curios->Hotspot. position/rotation + avatar_position(->exit)/avatar_rotation(->facing).
    int importMarkup(const std::string& json) {
        auto getStr=[&](size_t blk, const char* key, std::string& out){ std::string k=std::string("\"")+key+"\""; size_t i=json.find(k,blk); if(i==std::string::npos) return; i=json.find(':',i+k.size()); if(i==std::string::npos) return; size_t q=json.find('"',i); if(q==std::string::npos) return; size_t e=json.find('"',q+1); if(e!=std::string::npos) out=json.substr(q+1,e-q-1); };
        auto getVec=[&](size_t blk, const char* key, float v[3]){ std::string k=std::string("\"")+key+"\""; size_t i=json.find(k,blk); if(i==std::string::npos) return; i=json.find('[',i); if(i!=std::string::npos) sscanf(json.c_str()+i,"[ %f , %f , %f",&v[0],&v[1],&v[2]); };
        int added=0; size_t p=0;
        while ((p=json.find("\"Locator\"",p))!=std::string::npos) {
            size_t blk=p; p+=9;
            std::string type,name; getStr(blk,"type",type); getStr(blk,"name",name);
            float pos[3]={0,0,0},rot[3]={0,0,0},ap[3]={0,0,0},ar[3]={0,0,0};
            getVec(blk,"position",pos); getVec(blk,"rotation",rot); getVec(blk,"avatar_position",ap); getVec(blk,"avatar_rotation",ar);
            sitem::Item it; it.name = name.empty()?type:name; it.pos[0]=pos[0]; it.pos[1]=pos[1]; it.pos[2]=pos[2];
            std::string ln=name; for(auto& c:ln) c=(char)tolower(c);
            bool seat = ln.find("seat")!=std::string::npos||ln.find("couch")!=std::string::npos||ln.find("chair")!=std::string::npos||ln.find("sofa")!=std::string::npos||ln.find("stool")!=std::string::npos;
            if (type=="portal") { it.type=sitem::SPAWN; it.allowStart=true; it.isLocal=true; for(int k=0;k<3;k++) it.rot[k]=ar[k]; }
            else if (seat)      { it.type=sitem::CHAIR; for(int k=0;k<3;k++){ it.rot[k]=rot[k]; it.exitPos[k]=ap[k]; } }
            else                { it.type=sitem::HOTSPOT; for(int k=0;k<3;k++) it.rot[k]=rot[k]; }
            items.push_back(it); ++added;
        }
        if (added) fprintf(stderr, "[EDITOR] imported %d V79 markup locators -> scene items\n", added);
        return added;
    }
    // ── PROJECT SAVE / LOAD — persist every editor change so a session survives a close/rebuild ──
    // The .hsledit session lives in a "saved/" folder. It must be found no matter the working directory / how the exe
    // was launched (the old CWD-relative "saved/" broke when launched from the taskbar). So we look in a "saved/" subfolder
    // at EVERY ancestor of the env (nearest first) — finds the repo-level saved/ that already holds your sessions.
    std::string sessionPath;   // the .hsledit path load/save actually used this session (so save round-trips to it)
    // A cooked APK is "<stem>_Rooted-System.apk" / "<stem>_NoRoot-Spoof.apk" / "<stem>_cooked.apk". Strip the cook
    // suffix so the session round-trips to the SOURCE env's "<stem>.apk.hsledit" — loading the COOKED APK then shows
    // the same scene ITEMS (spawn/navmesh/colliders) the cook baked in. (The cook re-orders meshes, so the cooked APK
    // can't replay the session's index-based MESH transforms — loadProject skips those when the env is cooked.)
    static std::string stripCookSuffix(const std::string& fname) {
        size_t dot = fname.rfind(".apk");
        std::string stem = (dot==std::string::npos)?fname:fname.substr(0,dot);
        std::string ext  = (dot==std::string::npos)?std::string():fname.substr(dot);
        for (const std::string suf : {"_Rooted-System","_NoRoot-Spoof","_cooked"})
            if (stem.size()>=suf.size() && stem.compare(stem.size()-suf.size(),suf.size(),suf)==0) { stem.resize(stem.size()-suf.size()); break; }
        return stem + ext;
    }
    std::string rawBase() const {
        if (projectPath.empty()) return "editor_project";
        size_t sl=projectPath.find_last_of("/\\"); return (sl==std::string::npos)?projectPath:projectPath.substr(sl+1);
    }
    bool envIsCookedApk() const { std::string r=rawBase(); return r!=stripCookSuffix(r); }   // loaded a cook OUTPUT (re-ordered meshes)
    std::string projectBase() const {
        if (projectPath.empty()) return "editor_project";
        return stripCookSuffix(rawBase());
    }
    // Candidate session-FILE paths, nearest-to-the-env first. NOTE: pure path math — no directory is created here (the old
    // create_directories side-effect made an empty co-located saved/ that then SHADOWED the real repo-level one).
    std::vector<std::string> projectCandidates() const {
        std::vector<std::string> c; namespace fs = std::filesystem; std::error_code ec;
        std::string base = projectBase() + ".hsledit";
        if (!projectPath.empty()) {
            fs::path env = fs::absolute(fs::path(projectPath), ec);
            for (fs::path d = env.parent_path(); ; ) { c.push_back((d / "saved" / base).string());
                fs::path up = d.parent_path(); if (up == d || up.empty()) break; d = up; }
            c.push_back(projectPath + ".hsledit");          // legacy: next to the env file
        }
        c.push_back("saved/" + base);                       // legacy: CWD-relative saved/
        return c;
    }
    // Where saveProject writes: the file we loaded (round-trip), else the first EXISTING saved/ folder up the tree, else
    // co-locate a new saved/ next to the env (created here, on demand).
    std::string saveTargetFile() {
        if (!sessionPath.empty()) return sessionPath;
        namespace fs = std::filesystem; std::error_code ec; auto cands = projectCandidates();
        for (auto& c : cands) { if (fs::is_directory(fs::path(c).parent_path(), ec)) return c; }
        if (!cands.empty()) { fs::create_directories(fs::path(cands[0]).parent_path(), ec); return cands[0]; }
        std::filesystem::create_directories("saved", ec); return "saved/" + projectBase() + ".hsledit";
    }
    static std::string qstr(const std::string& s){ std::string o="\""; for(char c:s){ if(c=='"'||c=='\\') o+='\\'; o+=c; } o+='"'; return o; }
    static std::vector<std::string> tokenize(const std::string& line){
        std::vector<std::string> t; size_t i=0;
        while(i<line.size()){
            while(i<line.size()&&(line[i]==' '||line[i]=='\t'||line[i]=='\r'))i++;
            if(i>=line.size())break;
            if(line[i]=='"'){ std::string s; i++; while(i<line.size()&&line[i]!='"'){ if(line[i]=='\\'&&i+1<line.size())i++; s+=line[i++]; } if(i<line.size())i++; t.push_back(s); }
            else { size_t j=i; while(j<line.size()&&line[j]!=' '&&line[j]!='\t'&&line[j]!='\r')j++; t.push_back(line.substr(i,j-i)); i=j; }
        }
        return t;
    }
    // Serialize the editor session (.hsledit text). Used by saveProject (to disk) AND embedded into the cooked APK
    // (assets/_editor_session.hsledit) so an ORPHAN cooked APK — one whose source saved/<env>.hsledit is gone — still
    // round-trips its scene items when reloaded (loadProject extracts + parses it cooked-mode). [[the cook embeds this]]
    std::string serializeSession(){
        std::string s; char b[640];
        s += "HSLEDIT 2\n";
        snprintf(b,sizeof b,"CAM %.4f %.4f %.4f %.5f %.5f\n", r->cam.pos[0],r->cam.pos[1],r->cam.pos[2], r->cam.yaw, r->cam.pitch); s+=b;
        snprintf(b,sizeof b,"CFG %d %.3f %.3f %.3f %.1f %.4f %.0f %d %.0f %d %d %d %d %d %d %d %.4f %.4f %.4f\n",
            cfgFog?1:0, cfgFogColor[0],cfgFogColor[1],cfgFogColor[2], cfgFogStart, cfgFogDensity, cfgFar,
            skybox?1:0, skyboxDist, noCull?1:0, solidCollision?1:0, animSkinned?1:0, cookAudio?1:0, previewAudio?1:0, voxelSolid?1:0,
            bgColorSet?1:0, bgColor[0], bgColor[1], bgColor[2]); s+=b;
        for(int i=0;i<(int)r->gpuMeshes.size();++i){ auto& gm=r->gpuMeshes[i];
            snprintf(b,sizeof b,"MESH %d %s %.5f %.5f %.5f %.6f %.6f %.6f %.6f %.5f %.5f %.5f %d\n", i, qstr(gm.name).c_str(),
                gm.editT[0],gm.editT[1],gm.editT[2], gm.editR[0],gm.editR[1],gm.editR[2],gm.editR[3],
                gm.editS[0],gm.editS[1],gm.editS[2], r->isHidden(i)?1:0); s+=b; }
        for(auto& it:items){
            snprintf(b,sizeof b,"ITEM %d %s %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d %d %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d %d",
                it.type, qstr(it.name).c_str(), it.pos[0],it.pos[1],it.pos[2], it.rot[0],it.rot[1],it.rot[2],
                it.scale[0],it.scale[1],it.scale[2], it.allowStart?1:0, it.isLocal?1:0,
                it.exitPos[0],it.exitPos[1],it.exitPos[2], it.half[0],it.half[1],it.half[2], it.propW,it.propH,
                it.navMode, (int)it.srcMeshes.size()); s+=b;
            for(int m:it.srcMeshes){ snprintf(b,sizeof b," %d", m); s+=b; }
            snprintf(b,sizeof b," %.4f %.4f", it.iconY, it.iconScale); s+=b;   // trailing optional fields (old loaders ignore extras)
            s+="\n";
        }
        if(!animColliders.empty()){ s+="COLLIDERS "+std::to_string(animColliders.size()); for(int m:animColliders){ snprintf(b,sizeof b," %d",m); s+=b; } s+="\n"; }
        if(!skyboxMeshes.empty()){ s+="SKYBOX "+std::to_string(skyboxMeshes.size()); for(int m:skyboxMeshes){ snprintf(b,sizeof b," %d",m); s+=b; } s+="\n"; }
        if(r->deletedCount()>0){ s+="DELETED "+std::to_string(r->deletedCount()); for(int i=0;i<(int)r->gpuMeshes.size();++i) if(r->isDeleted(i)){ snprintf(b,sizeof b," %d",i); s+=b; } s+="\n"; }
        for(auto& kv:texOverride){ s += "TEXOVR "+std::to_string(kv.first)+" "+kv.second+"\n"; }   // swapped mesh/skybox textures (re-loaded from the image path on open)
        if(skyColorSet()){ snprintf(b,sizeof b,"SKYCOL %.4f %.4f %.4f\n", skyColor[0],skyColor[1],skyColor[2]); s+=b; }   // general skybox background color
        if(!skyImagePath.empty()){ s += "SKYIMG "+skyImagePath+"\n"; }   // skybox image file path OR "mesh:N" (reused texture)
        if(!audioOvrPath.empty()){ s += "AUDIOOVR "+audioOvrPath+"\n"; }   // replaced/added background audio (re-loaded from the file on open)
        { int n=0; for(auto& it:items) if(it.hidden) n++; if(n){ s+="IHIDE "+std::to_string(n); for(int i=0;i<(int)items.size();++i) if(items[i].hidden){ snprintf(b,sizeof b," %d",i); s+=b; } s+="\n"; } }
        // global light manipulation (only when edited off default)
        if (r->lightMul[0]!=1.f||r->lightMul[1]!=1.f||r->lightMul[2]!=1.f||r->lightMul[3]!=1.f){
            snprintf(b,sizeof b,"LIGHT %.4f %.4f %.4f %.4f\n", r->lightMul[0],r->lightMul[1],r->lightMul[2],r->lightMul[3]); s+=b; }
        // per-mesh material edits (tint + flags) — MATF idx blend additive alphaTest cullBack tintR G B A
        for (int i : matEdited) { if (i<0||i>=(int)r->gpuMeshes.size()) continue; auto& gm=r->gpuMeshes[i];
            snprintf(b,sizeof b,"MATF %d %d %d %d %d %.4f %.4f %.4f %.4f\n", i,
                gm.useBlend?1:0, gm.additive?1:0, gm.alphaTest?1:0, gm.cullBack?1:0,
                gm.editTint[0],gm.editTint[1],gm.editTint[2],gm.editTint[3]); s+=b; }
        // collision-excluded meshes (walk-through)
        if (!noColMeshes.empty()) { s+="NOCOL "+std::to_string(noColMeshes.size());
            for (int m : noColMeshes) { snprintf(b,sizeof b," %d",m); s+=b; } s+="\n"; }
        return s;
    }
    void saveProject(){
        std::string target=saveTargetFile();
        std::string s=serializeSession();
        FILE* f=fopen(target.c_str(),"wb"); if(!f){ setStatus("SAVE FAILED: "+target); return; }
        sessionPath=target;   // future saves/loads round-trip to here
        fwrite(s.data(),1,s.size(),f); fclose(f);
        saveGeomSidecar(target);          // editor-created meshes (fuse/cut/seal/... results) persist beside the session
        lastSessionSnap = std::move(s);   // auto-save dirty baseline = what was just saved
        { std::error_code ec; std::filesystem::remove(target + ".autosave", ec); }   // state is properly saved -> the crash-recovery autosave is obsolete
        recoverOffer = false;
        setStatus("Saved -> "+target);
    }
    // Read assets/_editor_session.hsledit that the cook EMBEDDED inside this APK — so an ORPHAN cooked APK (its source
    // saved/<env>.hsledit is gone, e.g. copied to another machine) still round-trips its scene items. projectPath = the APK.
    std::string extractEmbeddedSession(){
        if (projectPath.size() < 4 || projectPath.substr(projectPath.size()-4) != ".apk") return {};
        mz_zip_archive z; memset(&z, 0, sizeof z);
        if (!mz_zip_reader_init_file(&z, projectPath.c_str(), 0)) return {};
        std::string out; int idx = mz_zip_reader_locate_file(&z, "assets/_editor_session.hsledit", nullptr, 0);
        if (idx >= 0) { size_t sz=0; void* d=mz_zip_reader_extract_to_heap(&z, idx, &sz, 0); if(d){ out.assign((const char*)d, sz); mz_free(d); } }
        mz_zip_reader_end(&z);
        return out;
    }
    void loadProject(){
        // Try every "saved/<env>.hsledit" up the env's directory tree (nearest first) — checks the FILE itself, so an
        // empty co-located saved/ never shadows the real one. First hit wins and becomes the round-trip target.
        FILE* f=nullptr; std::string used;
        for (auto& c : projectCandidates()) { f=fopen(c.c_str(),"rb"); if(f){ used=c; break; } }
        std::string all;
        if(f){ sessionPath=used; fprintf(stderr,"[EDIT] loaded session: %s\n", used.c_str());
               fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ all.resize(n); fread(&all[0],1,n,f);} fclose(f); }
        else { all = extractEmbeddedSession();   // ORPHAN cooked APK: no saved/<env>.hsledit on disk -> read the session the cook EMBEDDED inside the APK ("extract em")
               if(all.empty()){ fprintf(stderr,"[EDIT] no saved session (disk) and no _editor_session.hsledit embedded in the APK\n");
                                loadGeomSidecar(saveTargetFile());   // a .geom can exist without a .hsledit (autosave-only runs) — created meshes still restore
                                checkAutosaveRecovery(); return; }   // a crash before the FIRST save still leaves an .autosave -> offer it
               sessionPath.clear(); fprintf(stderr,"[EDIT] loaded EMBEDDED session from cooked APK (%zu bytes)\n", all.size()); }
        // Restore editor-CREATED meshes (fuse/cut/seal/split/slice/duplicate results) BEFORE the session
        // lines apply — their MESH/DELETED/MATF indices assume those meshes exist.
        if (!sessionPath.empty()) loadGeomSidecar(sessionPath);
        else loadGeomSidecar(saveTargetFile());   // embedded-session path: sidecar may still sit at the default spot
        int meshN=0, itemN=0; applySessionText(all, meshN, itemN);
        didAutoSel = true;   // a session was restored -> don't let frame-1 auto-focus clobber the restored camera
        applyTexOverrides();   // re-load any swapped mesh/skybox textures from their image paths (preview + cook)
        lastSessionSnap = serializeSession();   // auto-save dirty baseline = the state just restored (don't re-write an unchanged session)
        setStatus("Loaded "+std::to_string(meshN)+" mesh edits + "+std::to_string(itemN)+" items from "+sessionPath);
        checkAutosaveRecovery();   // an .autosave NEWER than the session = edits lost to a crash/close -> offer to restore them
    }
    // Parse + apply a .hsledit session text (shared by loadProject and the auto-save recovery banner).
    void applySessionText(const std::string& all, int& meshN, int& itemN){
        items.clear(); selItem=-1; deselectAll(); animColliders.clear(); skyboxMeshes.clear();
        matEdited.clear(); for(int k=0;k<4;k++) r->lightMul[k]=1.f;   // reset light/material/collision edits; LIGHT/MATF/NOCOL lines re-apply
        { // created-mesh collision-excludes came from the GEOM2 sidecar (loaded BEFORE this) — keep those, reset the rest
          std::set<int> keep; for (int m : noColMeshes) if (geomAuth && m>=baseMeshCount) keep.insert(m);
          noColMeshes = std::move(keep); }
        clearAudioOverride();   // reset to the env's own theme; an AUDIOOVR line below re-applies the replacement (no-op when none was active)
        const bool cooked = envIsCookedApk();   // a cook OUTPUT: take the session's scene ITEMS but NOT its index-based MESH transforms (already baked into the cook) — re-derive the navmesh from THIS env's ground by name
        size_t p=0; meshN=0; itemN=0;
        while(p<all.size()){
            size_t e=all.find('\n',p); std::string line=all.substr(p, e==std::string::npos?std::string::npos:e-p); p=(e==std::string::npos)?all.size():e+1;
            auto t=tokenize(line); if(t.empty()) continue;
            if(t[0]=="CAM" && t.size()>=6){ r->cam.pos[0]=(float)atof(t[1].c_str()); r->cam.pos[1]=(float)atof(t[2].c_str()); r->cam.pos[2]=(float)atof(t[3].c_str()); r->cam.yaw=(float)atof(t[4].c_str()); r->cam.pitch=(float)atof(t[5].c_str()); }
            else if(t[0]=="CFG" && t.size()>=15){ cfgFog=atoi(t[1].c_str())!=0; cfgFogColor[0]=(float)atof(t[2].c_str()); cfgFogColor[1]=(float)atof(t[3].c_str()); cfgFogColor[2]=(float)atof(t[4].c_str()); cfgFogStart=(float)atof(t[5].c_str()); cfgFogDensity=(float)atof(t[6].c_str()); cfgFar=(float)atof(t[7].c_str()); skybox=atoi(t[8].c_str())!=0; skyboxDist=(float)atof(t[9].c_str()); noCull=atoi(t[10].c_str())!=0; solidCollision=atoi(t[11].c_str())!=0; prevSolidCol=solidCollision; animSkinned=atoi(t[12].c_str())!=0; cookAudio=atoi(t[13].c_str())!=0; previewAudio=atoi(t[14].c_str())!=0; /* t[15]=voxelSolid: NOT loaded — stays at the reverted default (false); old sessions saved 1 and would re-wall rooms */ if(t.size()>=20){ bgColorSet=atoi(t[16].c_str())!=0; bgColor[0]=(float)atof(t[17].c_str()); bgColor[1]=(float)atof(t[18].c_str()); bgColor[2]=(float)atof(t[19].c_str()); if(bgColorSet&&r){ r->clearRGB[0]=bgColor[0]; r->clearRGB[1]=bgColor[1]; r->clearRGB[2]=bgColor[2]; } } g_audioMuted.store(!previewAudio, std::memory_order_relaxed); }
            else if(t[0]=="MESH" && t.size()>=14 && !cooked){ int idx=atoi(t[1].c_str()); if(geomAuth && idx>=baseMeshCount) continue;   /* GEOM2 owns created meshes; compacted sidecar re-orders them = stale indices */ if(idx>=0&&idx<(int)r->gpuMeshes.size()){ auto& gm=r->gpuMeshes[idx];
                gm.name=t[2]; gm.editT[0]=(float)atof(t[3].c_str()); gm.editT[1]=(float)atof(t[4].c_str()); gm.editT[2]=(float)atof(t[5].c_str());
                gm.editR[0]=(float)atof(t[6].c_str()); gm.editR[1]=(float)atof(t[7].c_str()); gm.editR[2]=(float)atof(t[8].c_str()); gm.editR[3]=(float)atof(t[9].c_str());
                gm.editS[0]=(float)atof(t[10].c_str()); gm.editS[1]=(float)atof(t[11].c_str()); gm.editS[2]=(float)atof(t[12].c_str());
                r->setHidden(idx, atoi(t[13].c_str())!=0); recomputeModel(gm); meshN++; } }
            else if(t[0]=="ITEM" && t.size()>=24){ sitem::Item it; it.type=atoi(t[1].c_str()); it.name=t[2];
                it.pos[0]=(float)atof(t[3].c_str()); it.pos[1]=(float)atof(t[4].c_str()); it.pos[2]=(float)atof(t[5].c_str());
                it.rot[0]=(float)atof(t[6].c_str()); it.rot[1]=(float)atof(t[7].c_str()); it.rot[2]=(float)atof(t[8].c_str());
                it.scale[0]=(float)atof(t[9].c_str()); it.scale[1]=(float)atof(t[10].c_str()); it.scale[2]=(float)atof(t[11].c_str());
                it.allowStart=atoi(t[12].c_str())!=0; it.isLocal=atoi(t[13].c_str())!=0;
                it.exitPos[0]=(float)atof(t[14].c_str()); it.exitPos[1]=(float)atof(t[15].c_str()); it.exitPos[2]=(float)atof(t[16].c_str());
                it.half[0]=(float)atof(t[17].c_str()); it.half[1]=(float)atof(t[18].c_str()); it.half[2]=(float)atof(t[19].c_str());
                it.propW=(float)atof(t[20].c_str()); it.propH=(float)atof(t[21].c_str()); it.navMode=atoi(t[22].c_str());
                int nsrc=atoi(t[23].c_str()); for(int k=0;k<nsrc && 24+k<(int)t.size();++k) it.srcMeshes.push_back(atoi(t[24+k].c_str()));
                if (24+nsrc   < (int)t.size()) it.iconY     = (float)atof(t[24+nsrc].c_str());     // optional (newer sessions)
                if (24+nsrc+1 < (int)t.size()) it.iconScale = (float)atof(t[24+nsrc+1].c_str());
                if(it.type==sitem::NAVMESH){ if(cooked){ it.srcMeshes.clear(); fillGroundMeshes(it.srcMeshes); } bakeNavGeometry(it); }   // cooked: re-pick ground by NAME (the cook re-ordered meshes -> the saved indices point at the wrong meshes)
                items.push_back(std::move(it)); itemN++; }
            else if(t[0]=="COLLIDERS" && !cooked){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k) animColliders.push_back(atoi(t[2+k].c_str())); }
            else if(t[0]=="SKYBOX" && !cooked){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k) skyboxMeshes.push_back(atoi(t[2+k].c_str())); }   // restore far-backdrop skybox marks
            else if(t[0]=="IHIDE"){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k){ int idx=atoi(t[2+k].c_str()); if(idx>=0&&idx<(int)items.size()) items[idx].hidden=true; } }   // restore per-item visibility eyes
            else if(t[0]=="DELETED"){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k){ int idx=atoi(t[2+k].c_str()); if(idx>=0 && !(geomAuth && idx>=baseMeshCount)) r->setDeleted(idx,true); } }   // restore editor mesh deletions (dropped from render + cook)
            else if(t[0]=="TEXOVR" && (int)t.size()>=3){ int mi=atoi(t[1].c_str()); std::string p=t[2]; for(size_t k=3;k<t.size();++k) p+=" "+t[k]; if(mi>=0 && !(geomAuth && mi>=baseMeshCount)) texOverride[mi]=p; }   // remember swapped textures; applyTexOverrides() re-loads them below
            else if(t[0]=="SKYCOL" && (int)t.size()>=4){ setSkyColor((float)atof(t[1].c_str()),(float)atof(t[2].c_str()),(float)atof(t[3].c_str())); }   // general skybox color -> drives clearRGB
            else if(t[0]=="SKYIMG" && (int)t.size()>=2){ std::string p=t[1]; for(size_t k=2;k<t.size();++k) p+=" "+t[k]; if(p.rfind("mesh:",0)==0) setSkyImageFromMesh(atoi(p.c_str()+5)); else setSkyImage(p); }   // restore skybox image/texture
            else if(t[0]=="AUDIOOVR" && (int)t.size()>=2){ std::string p=t[1]; for(size_t k=2;k<t.size();++k) p+=" "+t[k]; setAudioFromFile(p); }   // restore the replaced/added background audio
            else if(t[0]=="LIGHT" && t.size()>=5){ for(int k=0;k<4;k++) r->lightMul[k]=(float)atof(t[1+k].c_str()); }   // global light manipulation
            else if(t[0]=="NOCOL" && !cooked){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k){ int idx=atoi(t[2+k].c_str()); if(!(geomAuth && idx>=baseMeshCount)) noColMeshes.insert(idx); } }   // collision-excluded meshes
            else if(t[0]=="MATF" && t.size()>=10 && !cooked){ int mi=atoi(t[1].c_str());   // per-mesh material flags + tint
                if (mi>=0 && mi<(int)r->gpuMeshes.size() && !(geomAuth && mi>=baseMeshCount)){ auto& gm=r->gpuMeshes[mi];
                    gm.useBlend=atoi(t[2].c_str())!=0; gm.additive=atoi(t[3].c_str())!=0;
                    gm.alphaTest=atoi(t[4].c_str())!=0; gm.cullBack=atoi(t[5].c_str())!=0;
                    for(int k=0;k<4;k++) gm.editTint[k]=(float)atof(t[6+k].c_str());
                    if (sceneMeshes && mi<(int)sceneMeshes->size()){ auto& md=(*sceneMeshes)[mi];   // cook ships the same flags
                        md.useBlend=gm.useBlend; md.additive=gm.additive; md.alphaTest=gm.alphaTest; md.doubleSided=!gm.cullBack; }
                    matEdited.insert(mi); } }
        }
    }
    // ── AUTO-SAVE (Phase-1 crash resistance): every autoSaveIntervalS seconds, if the session text changed since the
    //    last save/auto-save, write it to "<session>.hsledit.autosave". saveProject deletes the .autosave (state is now
    //    properly saved); loadProject offers a RECOVERY banner when an .autosave is NEWER than the saved session (= the
    //    editor crashed/closed with unsaved edits). Toggle lives in the Scene tab. ──
    bool   autoSaveOn = true;
    float  autoSaveIntervalS = 30.f;
    double lastAutoSaveT = 0.0;
    std::string lastSessionSnap;                 // last text persisted (save OR auto-save) — the dirty check
    bool recoverOffer = false; std::string recoverPath;
    std::chrono::steady_clock::time_point recoverOfferAt{};   // when the recovery banner appeared (auto-hides after 5s)
    void autoSaveTick(double now){
        if (!autoSaveOn || cooking || !r || r->gpuMeshes.empty()) return;
        if (projectPath.empty() || (uploadTotal > 0 && uploadCur < uploadTotal)) return;   // still streaming meshes in (projectPath binds on completion) — nothing valid to snapshot yet
        if (lastAutoSaveT == 0.0) { lastAutoSaveT = now;               // first tick arms the timer + captures the dirty
            if (lastSessionSnap.empty()) lastSessionSnap = serializeSession(); return; }   // baseline (no-session envs: don't autosave an untouched scene)
        if (now - lastAutoSaveT < autoSaveIntervalS) return;
        lastAutoSaveT = now;
        std::string s = serializeSession();
        if (s == lastSessionSnap) return;                              // nothing changed -> don't churn the disk
        std::string ap = saveTargetFile() + ".autosave";
        FILE* f=fopen(ap.c_str(),"wb"); if(!f) return;
        fwrite(s.data(),1,s.size(),f); fclose(f);
        if (geomDirty) saveGeomSidecar(saveTargetFile());   // crash-safe: created meshes persist with the autosave
        lastSessionSnap = std::move(s);
        fprintf(stderr,"[EDIT] auto-saved -> %s\n", ap.c_str());
    }
    void checkAutosaveRecovery(){
        namespace fs=std::filesystem; std::error_code ec;
        std::string ap = saveTargetFile() + ".autosave";
        if (!fs::exists(ap, ec)) return;
        if (!sessionPath.empty()) {   // stale autosave (session was saved AFTER it) -> silently clean it up
            auto at = fs::last_write_time(ap, ec);
            std::error_code ec2; auto st = fs::last_write_time(sessionPath, ec2);
            if (!ec && !ec2 && at <= st) { fs::remove(ap, ec); return; }
        }
        recoverPath = ap; recoverOffer = true; recoverOfferAt = std::chrono::steady_clock::now();
        fprintf(stderr,"[EDIT] auto-save recovery available: %s\n", ap.c_str());
    }
    void restoreAutosave(){
        FILE* f=fopen(recoverPath.c_str(),"rb"); if(!f){ recoverOffer=false; return; }
        std::string all; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ all.resize(n); fread(&all[0],1,n,f);} fclose(f);
        // editor-created meshes FIRST (the autosave's MESH/DELETED indices assume they exist) — the
        // sidecar lives at the SESSION path (autosaves share it); skip if already restored this run.
        if (sceneMeshes && (int)sceneMeshes->size() <= baseMeshCount) loadGeomSidecar(saveTargetFile());
        int meshN=0, itemN=0; applySessionText(all, meshN, itemN);
        didAutoSel = true; applyTexOverrides();
        lastSessionSnap = std::move(all); recoverOffer = false;
        setStatus("Recovered auto-save ("+std::to_string(meshN)+" mesh edits + "+std::to_string(itemN)+" items) - Save to keep it");
    }
    void dismissAutosave(){ std::error_code ec; std::filesystem::remove(recoverPath, ec); recoverOffer=false; }
    // V79 stores NO navmesh file — the LocomotionSystem generates it from the walkable ground geometry at runtime.
    // So auto-add a NAVMESH item sourced from the env's ground/floor/terrain meshes (the faithful re-creation; the
    // cook PhysX-cooks them into a V203 ColliderMesh). Returns the mesh count (0 = none found -> user picks manually).
    // Pick the env's walkable ground/floor meshes by NAME (index-independent), so it works on the SOURCE env or a
    // cook OUTPUT (whose meshes are re-ordered) alike — used by autoNavmeshFromGround + the cooked-APK navmesh re-derive.
    void fillGroundMeshes(std::vector<int>& out) const {
        static const char* kw[] = { "ground","floor","terrain","mainground","lakeshore","walk","path","road","plane","sidewalk","tile" };
        for (int i=0;i<(int)r->gpuMeshes.size();++i){
            if (r->isHidden(i) || isBackdrop(r->gpuMeshes[i].name)) continue;
            std::string n=r->gpuMeshes[i].name; for (auto& c:n) c=(char)tolower(c);
            for (const char* k : kw) if (n.find(k)!=std::string::npos) { out.push_back(i); break; }
        }
    }
    int autoNavmeshFromGround() {
        sitem::Item nv; nv.type=sitem::NAVMESH; nv.name="Navmesh (V79 ground)";
        fillGroundMeshes(nv.srcMeshes);
        if (nv.srcMeshes.empty()) return 0;
        items.push_back(nv);
        fprintf(stderr, "[EDITOR] auto-navmesh from %d V79 walkable ground meshes\n", (int)nv.srcMeshes.size());
        return (int)nv.srcMeshes.size();
    }
    int  localSpawnCount() const { int n=0; for (auto& it:items) if (it.type==sitem::SPAWN && it.allowStart && it.isLocal) ++n; return n; }

    // ── undo/redo ──
    struct Xform { float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; };
    struct UndoOp { std::vector<int> m; std::vector<Xform> b, a;       // mesh-transform op (multi-mesh = one drag = one op)
                    bool isItems=false; std::vector<sitem::Item> itemsState;      // OR a scene-items snapshot (add/delete/move/paste)
                    bool isDelete=false; std::vector<int> delM; std::vector<uint8_t> delA; };  // OR mesh delete/restore states (delA[i]=deleted AFTER the op)
    std::vector<UndoOp> undoStack, redoStack;
    // mesh DELETE/RESTORE (and duplicate/geometry-tool results) are part of the edit history: Ctrl+Z
    // restores deleted meshes AND removes clones an op created; Ctrl+Y re-applies. Per-mesh states so a
    // combined op ("original deleted + clone added") round-trips exactly.
    void pushDeleteUndo(const std::vector<int>& m, const std::vector<uint8_t>& after){
        UndoOp op; op.isDelete=true; op.delM=m; op.delA=after;
        undoStack.push_back(std::move(op)); redoStack.clear(); if (undoStack.size()>256) undoStack.erase(undoStack.begin());
    }
    void pushDeleteUndo(const std::vector<int>& m, bool nowDeleted){
        pushDeleteUndo(m, std::vector<uint8_t>(m.size(), nowDeleted?1:0));
    }
    sitem::Item clipboardItem; bool hasClipboard=false;   // copy/paste one scene item
    std::vector<sitem::Item> itemsBeforeDrag;             // snapshot captured when an item gizmo-drag starts (pushed on release)
    // snapshot the items vector BEFORE a mutation so Ctrl+Z can restore it (swap-based: the op holds the other state).
    void pushItemUndo(std::vector<sitem::Item> before){ UndoOp op; op.isItems=true; op.itemsState=std::move(before);
        undoStack.push_back(std::move(op)); redoStack.clear(); if (undoStack.size()>256) undoStack.erase(undoStack.begin()); }
    bool editing = false; int editMesh = -1; Xform editBefore;

    // ── gizmo ──
    int  gizmoOp = 0;            // 0=move 1=rotate 2=scale
    bool gizmoLocal = true;      // local vs world axes
    int  gizmoAxis = -1;         // axis being dragged (0..2), -1 = none
    bool lockAxis[3] = {false,false,false};   // per-axis gizmo lock (viewport X/Y/Z pills, Shift+X/Y/Z): a locked axis draws dimmed and can't be grabbed
    bool gizmoDrag = false; std::vector<int> gizmoSel; std::vector<Xform> gizmoBeforeV;
    bool gzVisible = false; float gzOrigin[2]={0,0}, gzTip[3][2]={{0,0},{0,0},{0,0}};
    float gzAxisW[3][3];         // cached world-space axis directions
    float gzAxisFace[3]={1,1,1}; // sign of each axis vs the view dir (rotate-drag handedness)
    float gzRing[3][33][2];      // cached projected rotation-ring points (per axis) for hit-test

    // ── layout (draggable ratios) ──
    float rightRatio = 0.235f, outlinerRatio = 0.45f, timelineH = 80.f;
    bool panelLeft = false;   // dock the tool column on the LEFT instead of the right
    bool dockDragging = false;   // Photoshop-style: grabbed the tool-panel grip → drag to re-dock LEFT/RIGHT
    VkRect2D rcHeader{}, rcViewport{}, rcOutliner{}, rcProps{}, rcTimeline{};
    int dragSplit = 0;           // 1=right border 2=outliner/props border 3=timeline border
    // A home is "loaded" once meshes exist. Empty session (Logcat-only) hides the timeline + dock grip.
    bool homeLoaded() const { return r && !r->gpuMeshes.empty(); }

    // ── properties tabs ──
    enum { TAB_OBJECT, TAB_SCENE, TAB_MATERIAL, TAB_ANIM, TAB_PHYSICS, TAB_COOK, TAB_LOGCAT };
    int tab = TAB_OBJECT;

    // ── cook (threaded) ──
    std::thread cookThread; std::atomic<bool> cooking{false}; std::atomic<float> cookProg{0.f};
    std::mutex statusMx; std::string cookStage = "idle", cookStatus;
    std::string cookPkg = "com.environment.outerwilds";
    bool autoSign = true, spoofHaven = true;
    bool cookAudio = true;             // DEFAULT ON: bake the env's background audio loop into the cooked APK. Toggle off = silent home.
    bool cookAutoFloor = true;         // DEFAULT ON: when NO Navmesh item exists, the cook generates a walkable floor (ColliderBox grid / disk). OFF = ship ZERO generated collision (the "invisible wall I never placed").
    bool killVistas = true;            // DEFAULT ON (unrooted spoof): after the spoof install, replace EVERY installed
                                       // com.meta.shell.env.vista.* package with an INVISIBLE (0-entity) environment, so
                                       // any vista the boot resolver force-pairs renders NOTHING. Only touches /data
                                       // (updatable) vista packages; a non-removable system vista is reported, not forced.
                                       // [[project_hsr_unrooted_footprint_vista_fix]]
    bool cookWarnOpen = false;         // pre-cook "no collision" confirmation modal is showing (blocks the frame's other input while up)
    // A cook has collision the player can stand on IF: the auto-floor is on, OR the user placed a Navmesh (walkable
    // mesh) or a Box collider. WITHOUT any of these the port ships zero collision — the player falls through forever
    // and respawns at the void floor. This gates the pre-cook warning + is the truth the wiki/README point at.
    bool envHasCollision() const {
        if (cookAutoFloor) return true;
        for (const auto& it : items) if (it.type == sitem::NAVMESH || it.type == sitem::BOXCOL) return true;
        return false;
    }
    bool previewAudio = true;          // DEFAULT ON: play the env's background loop HERE on the PC while previewing. Toggle off = mute desktop playback (drives g_audioMuted).
    bool solidCollision = true;        // DEFAULT ON: cook a REAL double-sided trimesh collider (floor+walls+columns, haven2025 SEBD format). Off = floor-only ColliderBox grid.
    bool prevSolidCol = true;          // tracks solidCollision so the navmesh gizmo re-bakes (walls appear/vanish in the preview) when it's toggled.
    bool voxelSolid = false;           // DEFAULT OFF (reverted): thick all-orientation voxel ColliderBoxes (voxelSolidBoxes)
                                       // gave walls real VOLUME so a teleported head can't clip a paper-thin wall — BUT thickening
                                       // every navmesh tri inward walled off room interiors ("not allowing me to walk inside rooms").
                                       // Default collision = thin per-triangle floor boxes + wall slabs (walkable). Opt-in escape
                                       // hatch for the thin-wall clip case; drives HSR_VOXSOLID in runCook.
    bool installAfterCook = true;      // DEFAULT ON: cook -> sign -> install to the headset. The installer auto-detects
                                       // adb root: ROOT -> install the UNSPOOFED own-package APK (+ auto-select it);
                                       // NO root -> back up the real haven2025, then install the haven2025 SPOOF.
    int shellRestart = 0;              // after-install vrshell restart (was FORCED, now an option): 0=Auto — restart only
                                       // when UNROOTED (rooted headsets hot-swap via the environment_selected IPC, no kill
                                       // needed); 1=Always; 2=Never. Cook-panel cycle button.
    std::string adbSerial, wifiIp;     // device serial ("" = default); wifiIp -> "adb connect" for wireless adb
    bool loadDiag = true;              // after an unrooted install+reload, capture the no-root env-load logcat and show WHY it was accepted/rejected
    // ── Logcat page state (live, NO-ROOT vrshell log viewer) ────────────────────────────────────────────────────
    struct LogLine { std::string t; uint8_t sev; bool env; };   // sev: 0=V/D 1=I 2=W 3=E ; env=env-load line (highlight/filter)
    std::thread logThread; std::atomic<bool> logRun{false};
    std::atomic<int> logMode{1};       // display filter: 0=env-load only, 1=all vrshell (--pid capture), 2=EVERYTHING (all logcat)
    std::mutex logMx; std::deque<LogLine> logBuf; std::string logLastRaw; std::string logLastTs;
    float logScroll = 0.f; bool logStick = true;   // logScroll = pixel offset; logStick = auto-scroll to newest
    bool autoLogStarted = false; std::atomic<bool> logVerboseSet{false};   // auto-start + one-time debug.logLevel=Verbose
    // adb auto-grab: if no working adb is found at runtime, download Google's platform-tools in the background.
    std::thread adbDlThread; std::atomic<bool> adbDling{false}; std::atomic<bool> adbDlDone{false};
    std::string adbDlStatus; std::mutex adbDlMx; bool adbAutoTried = false;
    void ensureAdbAsync(){
        if (adbDling.load() || adbAutoTried || adbWorks()) return;   // EXTERNAL adb already works -> use it, done.
        adbAutoTried = true;
#ifdef _WIN32
        // OFFLINE fallback: adb is EMBEDDED in the exe (ADBWINZIP resource). Extract it beside the app (instant, no
        // network) before trying a download. So: external adb -> embedded adb -> download.
        { std::string a = extractEmbeddedAdb(); if (!a.empty() && adbWorks(true)) return; }
#endif
        adbDling.store(true);
        { std::lock_guard<std::mutex> lk(adbDlMx); adbDlStatus = "Fetching adb (Google platform-tools, ~15 MB, one time)..."; }
        if (adbDlThread.joinable()) adbDlThread.join();
        adbDlThread = std::thread([this]{
            auto prog=[this](float, const char* s){ std::lock_guard<std::mutex> lk(adbDlMx); adbDlStatus = s; };
            std::string adb = hslcook::downloadPlatformTools(prog);
            { std::lock_guard<std::mutex> lk(adbDlMx);
              adbDlStatus = adb.empty() ? "adb download FAILED - check your connection, or Browse for adb.exe" : "adb ready"; }
            adbDling.store(false); adbDlDone.store(true);   // UI thread rechecks adbWorks() on adbDlDone
        });
    }
    std::thread restoreThread; std::atomic<bool> restoring{false};   // "Restore original Haven 2025" button (runs off the UI thread)
    std::thread javaThread; std::atomic<int> javaState{0};           // proactive Java auto-install: 0=installing, 1=ready, 2=failed
    bool animSkinned = true;   // HZANIM skinned + 1-joint rigid clips (door/discs/screens/cars/train/sphere). DEFAULT ON:
                               // the incredibles fixed-type-targetId fix ([[project_hsr_skinned_rendmesh_skinblock]]) made
                               // the clip cook stable on device (loads, no std::length_error). Opt-out: toggle / HSR_NOHZ.
    bool noCull = true;        // DEFAULT ON: emit scene-spanning bounds so V205's frustum/occlusion/CLOD/size culler never
                               // drops a mesh = V79-style "draw everything" (old homes had NO env culler). Fixes cooked-home
                               // clipping/disappearing; trades the Quest's culling perf for full visibility. -> HSR_NOCULL
    bool skybox = false;       // DEFAULT OFF (camera-locked, fragile): route the FAR backdrop (centroid > skyboxDist) to the SkyboxPlatformComponent
                               // pass — depth-clamped, EXEMPT from the shell's hard PortalStereoCamera far=5000 clip (device-
                               // proven: official homes/vistas escape the 5000 clip ONLY this way, NOT via a bigger far). The
                               // backdrop becomes camera-locked (fine at km range). -> HSR_SKYBOX_DIST. [[project_hsr_eye_subcamera_farclip]]
    float skyboxDist = 1500.f; // meters: meshes whose centroid is farther than this from origin -> skybox (else normal walkable mesh)
    // ── HSL render config: previewed live (WYSIWYG) AND emitted into the cook's ScenePlatformComponent ──
    bool  cfgFog = false;             // distance fog: show in preview + ship in cook (fogColor/fogStart/fogDensity)
    float cfgFogColor[3] = {0.05f, 0.06f, 0.09f};
    float cfgFogStart = 20.f;         // meters where fog begins
    float cfgFogDensity = 0.015f;     // distance-fog density
    float cfgFar = 150000.f;          // ScenePlatformComponent farClippingPlane (device extends from its 5000m default)

    // ── i18n (GitHub #10): UI language + CJK-aware font atlas ────────────────────────────────────────────────
    std::vector<unsigned> langCps;   // non-ASCII codepoints the active language needs (empty for English)
    // Bake the UI font at `px`, including exactly the CJK glyphs the active language uses (English = ASCII-only).
    void reloadUIFont(float px) {
        langCps.clear();
        if (i18n::g_lang != i18n::EN) i18n::collectExtraCodepoints(langCps);
        loadUIFont(font, px, false, langCps.empty() ? nullptr : &langCps);
        mono = font;   // one shared atlas — a stale mono copy would index the re-baked atlas with old UVs
    }
    static std::string langCfgPath() { return AppConfig::exeRel("hsr_ui_lang.txt"); }
    void loadLangPref() { FILE* f=fopen(langCfgPath().c_str(),"r"); if(!f) return; int l=0; if(fscanf(f,"%d",&l)==1 && l>=0 && l<i18n::LANG_COUNT) i18n::g_lang=l; fclose(f); }
    void saveLangPref() { FILE* f=fopen(langCfgPath().c_str(),"w"); if(!f) return; fprintf(f,"%d\n", i18n::g_lang); fclose(f); }
    void setLanguage(int l) {
        if (l<0 || l>=i18n::LANG_COUNT || l==i18n::g_lang) return;
        i18n::g_lang = l; saveLangPref();
        reloadUIFont(baseFontPx * uiScale); uiDraw.reloadFont();   // re-bake with/without the CJK glyph set, re-upload the atlas
        setStatus(std::string("UI language: ") + i18n::langName(l));
    }
    void cycleLanguage() { setLanguage((i18n::g_lang + 1) % i18n::LANG_COUNT); }
    bool insideLangMenu(float mx, float my) const {
        if (!langMenuOpen) return false;
        return mx >= langBtnX && my >= langBtnY && mx < langBtnX + langMenuW && my < langBtnY + langBtnH + langMenuH;
    }
    // Dropdown listing every language (native name), drawn on top at frame end. Click one to switch.
    void drawLangMenu() {
        if (!langMenuOpen) return;
        auto& th = cx.th; size_t n; auto* L = i18n::langs(n);
        float rh = th.rowH + 2*uiScale, pad = 4*uiScale;
        float w = langBtnW; for (size_t i=0;i<n;i++){ float lw=dl.textW(L[i].name)+24*uiScale; if(lw>w)w=lw; }
        langMenuW = w; langMenuH = n*rh + 2*pad;
        float x = langBtnX, y = langBtnY + langBtnH;
        dl.rect(x, y, w, langMenuH, th.panelBg); dl.border(x, y, w, langMenuH, th.accent);
        for (size_t i=0;i<n;i++) {
            float ry = y + pad + i*rh; bool cur = ((int)i==i18n::g_lang);
            bool hv = cx.hover(x, ry, w, rh);
            if (cur) dl.rect(x, ry, w, rh, th.rowSel); else if (hv) dl.rect(x, ry, w, rh, th.rowHover);
            cx.textAligned(x+10*uiScale, ry, w-14*uiScale, rh, L[i].name, cur?th.textSel:th.text, 0);
            if (hv && cx.in.pressed[0]) { langMenuOpen=false; setLanguage((int)i); }
        }
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  INIT / SHUTDOWN
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void init(VkRenderer* renderer, GLFWwindow* window, AudioPlayer* a, bool* animOver, float* animSc, float animDur) {
        r = renderer; win = window; audio = a;
        animOverride = animOver; animScrub = animSc; animDuration = animDur;
        float xs = 1.f, ys = 1.f; glfwGetWindowContentScale(window, &xs, &ys);
        dpiScale = (xs > 0.5f) ? xs : 1.f; uiScale = dpiScale;
        loadLangPref();                                  // restore the saved UI language BEFORE the first font bake (CJK atlas)
        reloadUIFont(baseFontPx * uiScale);
        mono = font;   // the UI pipeline binds ONE atlas (the main font); "mono" MUST share it or its glyph UVs index garbage
        // NO-GPU COOK MODE: no device -> no UI atlas/pipeline (uiDraw's first call writes a GPU
        // texture through r->device = null-deref). The headless cook never draws the overlay.
        if (!r->gpuLess && r->device != VK_NULL_HANDLE) uiDraw.init(r, &font);
        cx.font = &font; cx.mono = &font;
        // text-field clipboard (Ctrl+C/X/V) -> the OS clipboard via GLFW
        cx.setClip = [this](const char* s){ if (win && s) glfwSetClipboardString(win, s); };
        cx.getClip = [this]() -> std::string { const char* c = win ? glfwGetClipboardString(win) : nullptr; return c ? c : ""; };
        // capture UNSCALED theme metrics, then scale for DPI; recomputeUiScale() re-derives them from these on every resize
        baseRowH=cx.th.rowH; baseHeaderH=cx.th.headerH; basePad=cx.th.pad; baseIndent=cx.th.indent; baseTimelineH=timelineH;
        cx.th.rowH *= uiScale; cx.th.headerH *= uiScale; cx.th.pad *= uiScale; cx.th.indent *= uiScale;
        timelineH *= uiScale;
        r->overlayBegin = [this]() { this->buildFrame(); };
        r->overlayDraw  = [this](VkCommandBuffer cmd) { uiDraw.record(cmd, dl); };
        baseMeshCount = sceneMeshes ? (int)sceneMeshes->size() : 0;   // anything appended past this = editor-created (persisted via the .geom sidecar)
        // (auto-select of a centered, in-front object happens on frame 1 in buildFrame, when the camera matrices are valid)
        // Proactively AUTO-INSTALL Java if not present (Temurin JRE beside the exe), in the background, so the first
        // cook isn't blocked on a ~40MB download. apksigner/keytool need it; a no-op if java is already on PATH.
        javaThread = std::thread([this]{
            std::string jh = hslcook::ensureJava([](float,const char*){});
#ifdef _WIN32
            bool onPath = (system("java -version >NUL 2>&1")==0);
#else
            bool onPath = (system("java -version >/dev/null 2>&1")==0);
#endif
            javaState.store((!jh.empty() || onPath) ? 1 : 2);
        });
        ready = true;
    }
    void shutdown() {
        stopPlayerChannel();   // stop the persistent Quest player-control worker
        if (cookThread.joinable()) cookThread.join();
        if (restoreThread.joinable()) restoreThread.join();
        if (javaThread.joinable()) javaThread.join();
        if (geomThread.joinable()) geomThread.join();
        if (adbDlThread.joinable()) adbDlThread.join();
        if (ready) { vkDeviceWaitIdle(r->device); uiDraw.destroy(); ready = false; }
    }
    ~Editor() { stopLogStream(); if (cookThread.joinable()) cookThread.join(); if (restoreThread.joinable()) restoreThread.join(); if (javaThread.joinable()) javaThread.join(); if (geomThread.joinable()) geomThread.join(); if (adbDlThread.joinable()) adbDlThread.join(); }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  INPUT  (GLFW callbacks in main route here; we accumulate into cx.in, cleared at end of buildFrame)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void onCursorPos(double x, double y) {
        float sx = uiScale, fx = (float)x * (fbW>0&&winW()>0 ? (float)fbW/winW() : 1.f);
        float fy = (float)y * (fbH>0&&winH()>0 ? (float)fbH/winH() : 1.f);
        cx.in.dmx += fx - cx.in.mx; cx.in.dmy += fy - cx.in.my;
        cx.in.mx = fx; cx.in.my = fy; (void)sx;
    }
    void onMouseButton(int button, int action, int mods) {
        int b = button==GLFW_MOUSE_BUTTON_LEFT?0 : button==GLFW_MOUSE_BUTTON_RIGHT?1 : button==GLFW_MOUSE_BUTTON_MIDDLE?2 : -1;
        if (b < 0) return;
        cx.in.shift = (mods&GLFW_MOD_SHIFT)!=0; cx.in.ctrl=(mods&GLFW_MOD_CONTROL)!=0; cx.in.alt=(mods&GLFW_MOD_ALT)!=0;
        if (b == 0 && (ctxOpen || addMenuOpen || langMenuOpen)) {   // an open popup owns the mouse
            if (action == GLFW_PRESS) { cx.in.down[0]=true; cx.in.pressed[0]=true; cx.in.pressX[0]=cx.in.mx; cx.in.pressY[0]=cx.in.my; popupAtePress=true; if (!insideCtx(cx.in.mx,cx.in.my) && !insideLangMenu(cx.in.mx,cx.in.my)) { ctxOpen=false; addMenuOpen=false; langMenuOpen=false; } }
            else { cx.in.down[0]=false; cx.in.released[0]=true; }
            return;                                             // (item action is performed in drawContextMenu / drawAddMenu / drawLangMenu)
        }
        if (knifeOn && b == 0 && action == GLFW_PRESS && inRect(rcViewport, cx.in.mx, cx.in.my)) {
            knifeA[0]=(float)cx.in.mx; knifeA[1]=(float)cx.in.my; knifeDrag=true;   // KNIFE stroke start
            cx.in.down[0]=true; cx.in.pressed[0]=true; return;
        }
        if (playSim) {   // WALK MODE: the fly-cam owns the mouse-look; no pick / gizmo / exit-drag
            if (action==GLFW_PRESS){ cx.in.down[b]=true; cx.in.pressed[b]=true; } else { cx.in.down[b]=false; cx.in.released[b]=true; }
            return;
        }
        bool in3D = inRect(rcViewport, cx.in.mx, cx.in.my) && cx.in.my > rcViewport.offset.y + 22*uiScale;   // exclude the header strip (pills/toggles)
        if (action == GLFW_PRESS) {
            cx.in.down[b] = true; cx.in.pressed[b] = true; cx.in.pressX[b]=cx.in.mx; cx.in.pressY[b]=cx.in.my;
            if (b==0 && in3D && exitHVis && !editExit && selItem>=0 && selItem<(int)items.size() && items[selItem].type==sitem::CHAIR) {   // chair exit handle (square drag; off when the gizmo edits it)
                float dx=cx.in.mx-exitHS[0], dy=cx.in.my-exitHS[1]; if (dx*dx+dy*dy < 196.f*uiScale*uiScale) exitDrag=true;
            }
            if (b==0 && in3D && trackPlayer && gzPlayerVis && !exitDrag) {   // Quest player marker handles take priority when tracking
                int ph = playerGizmoHit(cx.in.mx, cx.in.my);
                if (ph>=0) { playerDrag=true; playerDragMode=ph; pdMx0=(float)cx.in.mx; pdMy0=(float)cx.in.my; pdY0=questPlayerPos[1]; pdYaw0=questPlayerYaw; }
            }
            if (b == 0 && gzVisible && in3D && !exitDrag && !playerDrag) {     // gizmo handle hit-test (cached screen positions)
                int hit = gizmoHitTest(cx.in.mx, cx.in.my);
                if (hit >= 0) {
                    if (sliceGizmoOn && selItem<0) { gizmoDrag=true; gizmoAxis=hit; gizmoSel.clear(); }          // dragging the SLICE PLANE (no undo capture needed)
                    else if (selItem>=0) { gizmoDrag=true; gizmoAxis=hit; gizmoSel.clear(); itemsBeforeDrag=items; }   // scene-item drag (snapshot for undo)
                    else if (!sel.empty()) { gizmoDrag=true; gizmoAxis=hit; gizmoSel=sel; gizmoBeforeV.clear(); for (int m:sel) gizmoBeforeV.push_back(captureX(r->gpuMeshes[m])); }
                }
            }
            // Ctrl/Shift + left-drag in empty viewport = rubber-band box multi-select (plain left-drag stays camera-look)
            if (b==0 && in3D && !gizmoDrag && !exitDrag && (cx.in.ctrl||cx.in.shift)) { boxSel=true; boxX0=(float)cx.in.mx; boxY0=(float)cx.in.my; }
        } else if (action == GLFW_RELEASE) {
            cx.in.down[b] = false; cx.in.released[b] = true;
            bool wasPlayer = playerDrag; if (b==0) playerDrag=false;   // player-marker drag: consume the release (no mesh pick)
            bool wasExit = exitDrag; if (b==0) exitDrag=false;
            if (b==0 && wasPlayer) return;   // don't fall through to gizmo/pick handling
            bool wasBox = (b==0)&&boxSel; if (b==0) boxSel=false;
            if (b == 0 && gizmoDrag) {
                if (gizmoSel.empty() && !itemsBeforeDrag.empty()) { pushItemUndo(std::move(itemsBeforeDrag)); itemsBeforeDrag.clear(); }   // item drag -> item undo
                else { std::vector<Xform> after; for (int m:gizmoSel) after.push_back(captureX(r->gpuMeshes[m])); pushUndo(gizmoSel, gizmoBeforeV, after); }
                gizmoDrag=false; gizmoAxis=-1; }
            else if (b == 0 && popupAtePress) { popupAtePress = false; }   // the press went to a MENU: its release must NOT pick a mesh behind the popup
            else if (b == 0 && knifeOn && knifeDrag) {   // KNIFE: release = cut (line = plane, box = 3D volume)
                knifeDrag=false;
                if (knifeBox) applyKnifeBox(knifeA[0], knifeA[1], (float)cx.in.mx, (float)cx.in.my);
                else          applyKnife(knifeA[0], knifeA[1], (float)cx.in.mx, (float)cx.in.my);
            }
            else if (b == 0 && in3D && patchMode) {   // PATCH TOOL: clicks drop pins, never select/deselect
                float dx=cx.in.mx-(float)cx.in.pressX[0], dy=cx.in.my-(float)cx.in.pressY[0];
                if (dx*dx+dy*dy < 25.f*uiScale*uiScale) patchClick(cx.in.mx, cx.in.my);
            }
            else if (b == 0 && in3D && !wasExit) {   // a click (pick) OR a Ctrl/Shift box-drag (rubber-band multi-select)
                float dx=cx.in.mx-(float)cx.in.pressX[0], dy=cx.in.my-(float)cx.in.pressY[0];
                if (wasBox && dx*dx+dy*dy >= 25.f*uiScale*uiScale) boxSelectRect(boxX0, boxY0, (float)cx.in.mx, (float)cx.in.my, cx.in.ctrl);   // Ctrl box = toggle/unpick, Shift box = add
                else if (dx*dx+dy*dy < 25.f*uiScale*uiScale) pick(cx.in.mx, cx.in.my, cx.in.shift||cx.in.ctrl);   // Ctrl or Shift = add to multi-selection
            }
            else if (b == 1 && in3D) {   // right-click (no drag) = object context menu
                float dx=cx.in.mx-(float)cx.in.pressX[1], dy=cx.in.my-(float)cx.in.pressY[1];
                if (dx*dx+dy*dy < 25.f*uiScale*uiScale) {
                    int it = pickItem(cx.in.mx, cx.in.my);
                    if (it>=0) { selItem=it; deselectAll();                          // scene-item marker -> its own context menu (focus/hide/duplicate/remove)
                        ctxItem=it; ctxMesh=-1; ctxOpen=true; ctxSub=-1; ctxX=cx.in.mx; ctxY=cx.in.my; }
                    else { int hit = pickIndex(cx.in.mx, cx.in.my);
                        if (hit>=0) {
                            if (!inSel(hit)) selectOne(hit);                        // right-clicked an UNSELECTED mesh -> select just it;
                            // ELSE: keep the existing multi-selection so the menu can act on ALL of them
                            selItem=-1; ctxMesh=hit; ctxItem=-1; ctxOpen=true; ctxSub=-1; ctxX=cx.in.mx; ctxY=cx.in.my;
                            float hn[3]; ctxHitValid = screenRayHitMesh(cx.in.mx, cx.in.my, hit, ctxHitP, hn);   // exact surface point (cut-hole target)
                        }
                    }
                }
            }
        }
    }
    void onScroll(double dx, double dy) { cx.in.wheel += (float)dy; }
    void onChar(unsigned cp) { if (cp >= 32 && cp < 0x10000 && cp < 256) cx.in.text.push_back((char)cp); }
    void onKey(int key, int action, int mods) {
        cx.in.shift=(mods&GLFW_MOD_SHIFT)!=0; cx.in.ctrl=(mods&GLFW_MOD_CONTROL)!=0; cx.in.alt=(mods&GLFW_MOD_ALT)!=0;
        bool press = (action==GLFW_PRESS || action==GLFW_REPEAT);
        if (key>=0 && key<400) { cx.in.keyDown[key] = (action!=GLFW_RELEASE); if (press) cx.in.keyRepeat[key]=true; }
        if (action != GLFW_PRESS) return;
        if (cx.kbFocus) return;                                 // typing in a field: don't trigger shortcuts
        // (gizmo mode = the viewport Move/Rotate/Scale pills, so G/R/S stay free for the WASD fly-cam)
        if ((mods&GLFW_MOD_CONTROL) && key=='Z') { if (mods&GLFW_MOD_SHIFT) doRedo(); else doUndo(); }
        if ((mods&GLFW_MOD_CONTROL) && key=='Y') doRedo();
        if ((mods&GLFW_MOD_CONTROL) && key=='S') saveProject();   // persist the session
        if ((mods&GLFW_MOD_CONTROL) && key=='L') loadProject();   // restore the session
        if ((mods&GLFW_MOD_CONTROL) && key=='C' && selItem>=0 && selItem<(int)items.size()) { clipboardItem=items[selItem]; hasClipboard=true; }   // copy item
        if ((mods&GLFW_MOD_CONTROL) && key=='V' && hasClipboard) {   // paste a copy (offset so it's visible), undoable, and select it
            pushItemUndo(items); sitem::Item it=clipboardItem; it.pos[0]+=0.5f; it.pos[2]+=0.5f; /*name kept*/
            deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; }
        if ((mods&GLFW_MOD_CONTROL) && key=='D' && selItem>=0 && selItem<(int)items.size()) {   // Ctrl+D = duplicate in place+offset
            pushItemUndo(items); sitem::Item it=items[selItem]; it.pos[0]+=0.5f; it.pos[2]+=0.5f; /*name kept*/
            deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; }
        if (key=='P' && !(mods&GLFW_MOD_CONTROL)) { if(playSim) stopSim(); else startSim(); }   // toggle WALK mode (player sim)
        if ((mods&GLFW_MOD_SHIFT) && !(mods&GLFW_MOD_CONTROL)) {   // Shift+X/Y/Z = toggle the per-axis gizmo lock (viewport pills)
            if (key=='X') lockAxis[0]=!lockAxis[0];
            if (key=='Y') lockAxis[1]=!lockAxis[1];
            if (key=='Z') lockAxis[2]=!lockAxis[2];
        }
        if (key==GLFW_KEY_DELETE) {   // Del: remove selected scene item(s), else delete/restore selected mesh(es)
            if (!selItems.empty() && selItems.size()>1) {   // MULTI item delete (one undo snapshot)
                pushItemUndo(items);
                std::vector<int> del=selItems; std::sort(del.rbegin(), del.rend());
                for (int i : del) if (i>=0 && i<(int)items.size()) items.erase(items.begin()+i);
                selItems.clear(); selItem=-1;
            }
            else if (selItem>=0) deleteSelItem();
            else if (!sel.empty()) toggleDeleteSelected();
        }
        if ((mods&GLFW_MOD_CONTROL) && key=='D' && selItem<0 && !sel.empty()) duplicateSelected();   // Ctrl+D on meshes = duplicate (clone + offset), editable independently
        if ((mods&GLFW_MOD_CONTROL) && key=='C' && selItem<0 && !sel.empty()) { meshClipboard=sel; setStatus("Copied "+std::to_string(sel.size())+" mesh(es) - Ctrl+V pastes clones"); }   // mesh COPY (names already went to the OS clipboard above)
        if ((mods&GLFW_MOD_CONTROL) && key=='V' && selItem<0 && !meshClipboard.empty()) {            // mesh PASTE = clone the copied set
            sel.clear(); for (int m : meshClipboard) if (m>=0 && m<(int)r->gpuMeshes.size()) sel.push_back(m);
            if (!sel.empty()) { selected=sel.back(); r->selectedMesh=selected; duplicateSelected(); }
        }
        if ((mods&GLFW_MOD_CONTROL) && key=='A') {   // Ctrl+A = select ALL: scene ITEMS when an item is active, else all meshes
            if (selItem>=0 || !selItems.empty()) {
                selItems.clear();
                for (int i2=0;i2<(int)items.size();++i2) selItems.push_back(i2);
                selItem = selItems.empty()?-1:selItems.back(); deselectAll();
                setStatus("Selected ALL "+std::to_string(selItems.size())+" scene items (Del removes, Space hides)");
            } else if (r) {
                sel.clear();
                for (int i2=0;i2<(int)r->gpuMeshes.size();++i2) if (!r->isDeleted((size_t)i2) && !r->isHidden((size_t)i2)) sel.push_back(i2);
                selected = sel.empty()?-1:sel.back(); r->selectedMesh=selected; selItems.clear(); selItem=-1;
                setStatus("Selected ALL "+std::to_string(sel.size())+" meshes (click a scene item first to Ctrl+A the items instead)");
            }
        }
        if ((mods&GLFW_MOD_CONTROL) && key=='H') toggleAnimatedEye();   // Ctrl+H = hide/show EVERYTHING animated (cards/flipbooks/particles/skinned/VAT)
        if (key==GLFW_KEY_F1) showKeybinds = !showKeybinds;   // all-shortcuts overlay
        if (patchMode) {   // PIN TOOLS (patch / cut region / bend / knife path): Enter = apply, Esc = cancel
            if (key==GLFW_KEY_ENTER) { if (pinIsKnife) applyKnifePath(); else if (pinIsBend) bendAtPin(); else if (pinIsCut) cutRegionByPins(); else buildPatch(); }
            if (key==GLFW_KEY_ESCAPE) { patchMode=false; pinIsCut=false; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear(); setStatus("pin tool cancelled"); }
        }
        if (sliceGizmoOn) {   // SLICE GIZMO: Enter = cut along the placed plane, Esc = cancel
            if (key==GLFW_KEY_ENTER) applySliceGizmo();
            if (key==GLFW_KEY_ESCAPE) { sliceGizmoOn=false; setStatus("slice gizmo cancelled"); }
        }
        if (knifeOn && key==GLFW_KEY_ESCAPE) { knifeOn=false; knifeDrag=false; knifeBox=false; setStatus("knife cancelled"); }
        if (key==GLFW_KEY_SPACE && !playSim) {   // SPACE = toggle visibility of the selection (meshes or scene items)
            if (!selItems.empty()) { bool anyVis=false; for (int i:selItems) if (i>=0&&i<(int)items.size()&&!items[i].hidden) anyVis=true;
                                     for (int i:selItems) if (i>=0&&i<(int)items.size()) items[i].hidden = anyVis; }
            else if (selItem>=0 && selItem<(int)items.size()) items[selItem].hidden = !items[selItem].hidden;
            else if (!sel.empty() && r) {
                bool anyVis=false; for (int m:sel) if (!r->isHidden((size_t)m)) anyVis=true;   // mixed state -> hide all first
                for (int m:sel) r->setHidden((size_t)m, anyVis);
                setStatus((anyVis?"Hid ":"Showed ")+std::to_string(sel.size())+" mesh(es)  [Space toggles]");
            }
        }
    }
    // ── F1 keybinds overlay: every shortcut in one panel ──
    void drawKeybindsPanel() {
        if (!showKeybinds) return;
        auto& th=cx.th; float rh=th.rowH;
        static const char* rows[][2] = {
            {"WASD + QE",          "fly camera (drag = look, scroll = speed)"},
            {"F (hover a row)",    "focus / frame the object"},
            {"Double-click row",   "focus / frame the object"},
            {"Click / Ctrl+Click", "select / add to multi-selection"},
            {"Shift+Drag rows",    "drag-paint multi-select"},
            {"Ctrl+A",             "select all (filtered, visible)"},
            {"Space",              "toggle visibility of the selection"},
            {"Ctrl+C / Ctrl+V",    "copy / paste (clone) meshes or scene items"},
            {"Ctrl+D",             "duplicate selection"},
            {"Del",                "delete / restore selection (undoable)"},
            {"Ctrl+Z / Ctrl+Y",    "undo / redo (transforms, items, deletes)"},
            {"Move/Rotate/Scale",  "gizmo mode pills (Scale = stretch per-axis)"},
            {"Shift+X / Y / Z",    "lock a gizmo axis"},
            {"Right-click",        "context menu (viewport or outliner rows)"},
            {"P",                  "walk the env (player simulator)"},
            {"Ctrl+S / Ctrl+L",    "save / load the session"},
            {"Tab / N / B",        "cycle mesh selection"},
            {"F1",                 "toggle this panel"},
        };
        int n = (int)(sizeof rows / sizeof rows[0]);
        float w = 560*uiScale, h = (n+2)*rh + 16*uiScale;
        float x = (fbW-w)*0.5f, y = (fbH-h)*0.5f;
        dl.rect(x,y,w,h, ui::rgba(24,26,33,242)); dl.border(x,y,w,h, th.accent);
        cx.textAligned(x, y+4*uiScale, w, rh, "KEYBINDS   (F1 or click to close)", th.text, 1);
        float ry = y + rh + 10*uiScale;
        for (int i=0;i<n;++i, ry+=rh) {
            cx.textAligned(x+16*uiScale,  ry, 170*uiScale, rh, rows[i][0], th.accentHot, 0);
            cx.textAligned(x+195*uiScale, ry, w-205*uiScale, rh, rows[i][1], th.text, 0);
        }
        if (cx.in.pressed[0] && cx.hover(x,y,w,h)) showKeybinds=false;
    }
    int winW() const { int w=0,h=0; glfwGetWindowSize(win,&w,&h); return w; }
    int winH() const { int w=0,h=0; glfwGetWindowSize(win,&w,&h); return h; }
    // main gates camera/WASD on these
    bool wantsMouse() const { return ctxOpen || knifeOn || cx.active!=0 || cx.kbFocus!=0 || gizmoDrag || exitDrag || playerDrag || boxSel || dragSplit!=0 || dockDragging || !inRect(rcViewport, cx.in.mx, cx.in.my); }
    bool wantsKeyboard() const { return cx.kbFocus != 0; }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  FRAME  (overlayBegin: layout -> set viewport pane -> build the DrawList; overlayDraw records it)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    // Responsive UI: recompute uiScale from the viewport each frame. dpiScale keeps physical size right; the window-fit
    // factor (logical height vs a 900px baseline, clamped) upscales on big windows / downscales on small ones. The font
    // atlas re-bake+upload is gated on the integer pixel size, so a resize-drag triggers only a handful of reloads.
    void recomputeUiScale() {
        if (fbH <= 0) return;
        float logicalH = (dpiScale > 0.1f) ? (float)fbH / dpiScale : (float)fbH;
        // The DPI scale already gives the right PHYSICAL size, so only DOWNSCALE when the window is small (so the UI
        // fits); never upscale past the DPI size — the old 1.7x cap made the UI huge on big/4K monitors ("too upscaled").
        float target   = dpiScale * std::clamp(logicalH / 1080.f, 0.72f, 1.0f);
        if (std::fabs(target - uiScale) < 0.01f) return;
        uiScale = target;
        cx.th.rowH=baseRowH*uiScale; cx.th.headerH=baseHeaderH*uiScale; cx.th.pad=basePad*uiScale; cx.th.indent=baseIndent*uiScale;
        timelineH = baseTimelineH*uiScale;
        int px = (int)(baseFontPx*uiScale + 0.5f);
        if (px >= 8 && px != (int)(font.pixelHeight + 0.5f)) {
            reloadUIFont((float)px); uiDraw.reloadFont();
            mono = font;   // keep "mono" on the SAME re-baked atlas — a stale copy indexes the new atlas
                           // with the OLD glyph UVs = the garbled component-class rows in the Scene tab
        }
    }
    void buildFrame() {
        if (!ready) return;
        fbW = (int)r->swapchainExtent.width; fbH = (int)r->swapchainExtent.height;
        recomputeUiScale();                                      // responsive: adapt uiScale + font + theme to the viewport
        // Recompute the view matrix NOW (WASD already moved the camera this frame) so the overlay's worldToScreen
        // matches the meshes the renderer draws later this frame. Without this the overlay uses last frame's view,
        // so the selection wireframe drifts off the mesh during camera motion and snaps back when you stop.
        if (r) r->cam.updateView();
        double now = glfwGetTime(); float dt=(float)(now-lastT); cx.t = (float)now; lastT = now;
        if (dt<0.f || dt>0.25f) dt=0.f;                              // ignore the first frame / hitches
        if (animScrub && animOverride && animDuration>0.f) {         // the editor drives playback -> LIVE playhead
            *animOverride = true;                                    //   main loop always renders at *animScrub
            if (animPlaying) { *animScrub += dt; if (*animScrub >= animDuration) *animScrub = fmodf(*animScrub, animDuration); }
        }
        layout();
        if (!didAutoSel && !r->gpuMeshes.empty()) {             // frame 1: spawn at the env's DEFAULT spawn (nothing auto-selected)
            didAutoSel = true;
            // If the user placed/imported a local spawn point, start the camera there (facing it). OTHERWISE leave the
            // camera at the loader's default view (Camera default = {0,1.6,0} looking forward = the env's default spawn) —
            // do NOT auto-select the first mesh or reframe onto it ("Planet" at an angle).
            for (auto& it:items) if (it.type==sitem::SPAWN && it.allowStart){ float q[4]; eulerToQuat(it.rot,q); float f[3]={0,0,-1},o[3]; quatRotVec(q,f,o);
                r->cam.pos[0]=it.pos[0]; r->cam.pos[1]=it.pos[1]+1.6f; r->cam.pos[2]=it.pos[2]; r->cam.yaw=std::atan2(o[0],-o[2]); r->cam.pitch=0; break; }
        }
        if (playSim) simulatePlayer(dt);                        // walk mode: glue the cam to the walkable surface
        // ── WYSIWYG scene config, EVERY frame (was Cook-tab-only, so fog/far reverted when the tab closed):
        //    the live preview always shows the SAME fog + far-clip the cook ships. HSR_CLIP (device-clip replica
        //    diagnostics) keeps ownership of farZ when set.
        { float fcol[4]={cfgFogColor[0],cfgFogColor[1],cfgFogColor[2],1.f};
          r->setSceneFog(fcol, cfgFogStart, cfgFog?cfgFogDensity:0.f, 0.f, 1500.f);
          static const bool devClip = std::getenv("HSR_CLIP")!=nullptr;
          if (!devClip) { float f=cfgFar; if (f < r->cam.nearZ+1.f) f = r->cam.nearZ+1.f; r->cam.farZ = f; } }
        r->uiViewportRect = rcViewport;                         // the 3D scene scissors to the Viewport pane
        dl.begin(fbW, fbH, &font, uiDraw.whiteU, uiDraw.whiteV);
        cx.dl = &dl; cx.hot = 0;                                // hot recomputed each frame
        // MODAL INPUT LOCK: while the pre-cook warning is up, park the mouse off-screen so NOTHING under the scrim
        // hovers/clicks (immediate-mode widgets gate all interaction on hover). Restored just before the modal draws.
        float savedMx=cx.in.mx, savedMy=cx.in.my;
        if (cookWarnOpen) { cx.in.mx = cx.in.my = -1e5f; }
        // NOTE: do NOT fill the whole window — the 3D scene shows through rcViewport; each panel paints its
        // own opaque background and the layout tiles the rest, so the viewport pane stays the live 3D view.
        drawViewportOverlay();
        drawItems();                                            // spawn/chair/collider/wall/hotspot/navmesh markers
        if (xrayMesh) drawSelectedMeshWire();                   // X-ray: selected mesh wireframe over the boxes (alignment)
        if (hasRespawn) {                                       // visualize the respawn kill-floor: a red grid at respawnY around you
            VkRect2D vp=rcViewport; dl.pushClip((float)vp.offset.x,(float)vp.offset.y,(float)vp.extent.width,(float)vp.extent.height);
            float cx0=r->cam.pos[0], cz0=r->cam.pos[2], S=60.f; uint32_t col=ui::rgba(255,80,80,150);
            for (int g=-6; g<=6; ++g){ float t=g/6.f*S;
                float a1[3]={cx0-S,respawnY,cz0+t}, a2[3]={cx0+S,respawnY,cz0+t}, b1[3]={cx0+t,respawnY,cz0-S}, b2[3]={cx0+t,respawnY,cz0+S};
                float sa[2],sb[2]; if(worldToScreen(a1,sa[0],sa[1])&&worldToScreen(a2,sb[0],sb[1])) dl.line(sa[0],sa[1],sb[0],sb[1],col,1.f);
                if(worldToScreen(b1,sa[0],sa[1])&&worldToScreen(b2,sb[0],sb[1])) dl.line(sa[0],sa[1],sb[0],sb[1],col,1.f); }
            dl.popClip();
        }
        if (exitDrag && cx.in.down[0] && selItem>=0 && selItem<(int)items.size()) {   // drag the chair exit handle (XZ at its current height)
            auto& it=items[selItem]; float planeY=it.pos[1]+it.exitPos[1], g[3];       // keep exitPos[1] (height = numeric box, drag/type)
            if (screenToGround(cx.in.mx,cx.in.my,planeY,g)) { it.exitPos[0]=g[0]-it.pos[0]; it.exitPos[2]=g[2]-it.pos[2]; }
        }
        updatePlayerDrag();      // apply a live player-marker drag (before drawing so the marker follows the cursor)
        drawGizmo();
        drawPlayerGizmo();       // live Quest player marker + drag handles (Track Player)
        if (boxSel) {   // rubber-band selection rectangle (Ctrl/Shift + left-drag)
            VkRect2D vp=rcViewport; dl.pushClip((float)vp.offset.x,(float)vp.offset.y,(float)vp.extent.width,(float)vp.extent.height);
            float x0=std::min(boxX0,(float)cx.in.mx), y0=std::min(boxY0,(float)cx.in.my), x1=std::max(boxX0,(float)cx.in.mx), y1=std::max(boxY0,(float)cx.in.my);
            dl.rect(x0,y0,x1-x0,y1-y0, ui::rgba(90,160,255,40)); dl.border(x0,y0,x1-x0,y1-y0, ui::rgba(120,180,255,220));
            dl.popClip();
        }
        drawHeader();
        drawRecoverBanner();                                    // crash-recovery offer strip (under the header)
        autoSaveTick(now);                                      // periodic dirty-checked session autosave
        drawOutliner();
        drawProperties();
        if (homeLoaded()) drawTimeline();          // no timeline strip on the empty (Logcat-only) session
        wantCursor = 0;
        drawSplitters();
        // apply the resize cursor when hovering/dragging a splitter (create the standard cursors lazily)
        if (win) {
            if (!curH) curH = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
            if (!curV) curV = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
            glfwSetCursor(win, wantCursor==1 ? curH : wantCursor==2 ? curV : nullptr);
        }
        drawDockDrag();                                         // Photoshop-style re-dock drop zones (while grip is dragged)
        // EMPTY SESSION (no home loaded): an ANIMATED "drag a file here" drop-zone instead of a dark void.
        if (r && r->gpuMeshes.empty()) {
            VkRect2D v=rcViewport; float hdr=26*uiScale;
            float vx=(float)v.offset.x, vy=(float)v.offset.y+hdr, vw=(float)v.extent.width, vh=(float)v.extent.height-hdr;
            auto& th=cx.th; auto* d2=cx.dl; float t=(float)glfwGetTime();
            float pulse=0.5f+0.5f*std::sin(t*2.0f);            // 0..1 breathing
            float bob=std::sin(t*2.6f)*7.f*uiScale;            // arrow bob
            d2->pushClip(vx,vy,vw,vh);
            d2->rect(vx,vy,vw,vh, ui::rgba(28,30,38));
            float cyc=vy+vh*0.5f, midx=vx+vw*0.5f;
            // marching-ants dashed drop-zone box
            float bw2=std::min(vw*0.60f, 540.f*uiScale), bh2=std::min(vh*0.52f, 260.f*uiScale);
            float bx2=midx-bw2*0.5f, by2=cyc-bh2*0.5f, thk=2.f*uiScale;
            uint32_t bcol=ui::rgba(64,140,235,(int)(55+120*pulse));
            float dash=12*uiScale, dgap=9*uiScale, period=dash+dgap, off=std::fmod(t*36.f*uiScale, period);
            for(float p=bx2-off; p<bx2+bw2; p+=period){ float a=std::max(p,bx2), b=std::min(p+dash,bx2+bw2); if(b>a){ d2->rect(a,by2,b-a,thk,bcol); d2->rect(a,by2+bh2-thk,b-a,thk,bcol);} }
            for(float p=by2-off; p<by2+bh2; p+=period){ float a=std::max(p,by2), b=std::min(p+dash,by2+bh2); if(b>a){ d2->rect(bx2,a,thk,b-a,bcol); d2->rect(bx2+bw2-thk,a,thk,b-a,bcol);} }
            // bobbing down-chevron (two beams forming a V)
            float chx=midx, chy=cyc-58*uiScale+bob, cs=13*uiScale, ct=3.f*uiScale;
            uint32_t chc=ui::rgba(96,170,250,(int)(150+95*pulse));
            for(float k=0;k<=cs;k+=1.f){ d2->rect(chx-cs+k-ct*0.5f, chy+k-ct*0.5f, ct, ct, chc); d2->rect(chx+cs-k-ct*0.5f, chy+k-ct*0.5f, ct, ct, chc); }
            // pulsing text
            cx.textAligned(vx, cyc-16*uiScale, vw, 30*uiScale, "Drag a home here to start", ui::withA(th.text,(int)(165+90*pulse)), 1);
            cx.textAligned(vx, cyc+18*uiScale, vw, 20*uiScale, "Supported:  .apk   .gltf   .glb   .gltf.ovrscene   .opa", th.textDim, 1);
            cx.textAligned(vx, cyc+bh2*0.5f+8*uiScale, vw, 18*uiScale, "- or open the Logcat tab to diagnose a Quest (no root needed) -", th.textDim, 1);
            d2->popClip();
        }
        drawContextMenu();                                      // floating; drawn last = on top
        drawAddMenu();
        drawHoleOverlay();                                      // numbered orange hole outlines - CLICK a marker to seal that hole
        drawPatchOverlay();                                     // patch tool: pins + outline while clicking corners
        drawSliceOverlay();                                     // slice gizmo: plane quad + LIVE glowing cut line
        drawKnifeOverlay();                                     // knife: the 2D stroke line while dragging
        drawKeybindsPanel();                                    // F1 overlay: every shortcut in one place
        if (cookWarnOpen) { cx.in.mx=savedMx; cx.in.my=savedMy; drawCookWarn(); }   // restore mouse for the modal only
        drawLangMenu();                                         // language dropdown (on top of the UI, under tooltips)
        cx.drawTooltip();                                       // deferred hover tooltips — drawn ABOVE everything
        cx.in.newFrame();                                       // consume per-frame input edges/deltas
    }

    // ── tiled Blender layout: header strip on top, timeline strip on bottom, a right column (outliner over
    //    properties), and the 3D viewport filling the remaining left/center. Borders are draggable. ──
    void layout() {
        float W=(float)fbW, H=(float)fbH, hH=cx.th.headerH, tH=homeLoaded()?timelineH:0.f;   // no timeline until a home loads
        float rightW = std::clamp(W*rightRatio, 220.f*uiScale, W*0.75f);   // up to 75% wide (log viewer / wide panels)
        float midY = hH, midH = H - hH - tH;
        float oH = std::clamp(midH*outlinerRatio, 80.f*uiScale, midH-80.f*uiScale);
        rcHeader   = {{0,0},{(uint32_t)W,(uint32_t)hH}};
        float panelX = panelLeft ? 0.f : (W-rightW);         // tool column on the LEFT or RIGHT
        float viewX  = panelLeft ? rightW : 0.f;
        rcViewport = {{(int)viewX,(int)midY},{(uint32_t)(W-rightW),(uint32_t)midH}};
        rcOutliner = {{(int)panelX,(int)midY},{(uint32_t)rightW,(uint32_t)oH}};
        rcProps    = {{(int)panelX,(int)(midY+oH)},{(uint32_t)rightW,(uint32_t)(midH-oH)}};
        rcTimeline = {{0,(int)(H-tH)},{(uint32_t)W,(uint32_t)tH}};
    }
    void drawSplitters() {
        // vertical: viewport|tool column (boundary = panel's viewport-facing edge, left OR right dock)
        float bx = panelLeft ? (float)(rcOutliner.offset.x + rcOutliner.extent.width) : (float)rcOutliner.offset.x;
        float by=(float)rcHeader.extent.height, bh=(float)(rcViewport.extent.height);
        handleSplit(1, bx-3, by, 6, bh, true);
        // horizontal: outliner|properties
        float hy = (float)rcProps.offset.y;
        handleSplit(2, (float)rcOutliner.offset.x, hy-3, (float)rcOutliner.extent.width, 6, false);
        // horizontal: middle|timeline (only when the timeline is shown)
        if (homeLoaded()) handleSplit(3, 0, (float)rcTimeline.offset.y-3, (float)fbW, 6, false);
        // thin separator lines
        dl.rect(bx-1, by, 1, bh, cx.th.splitLine);
        dl.rect((float)rcProps.offset.x, hy-1, (float)rcProps.extent.width, 1, cx.th.splitLine);
        if (homeLoaded()) dl.rect(0, (float)rcTimeline.offset.y-1, (float)fbW, 1, cx.th.splitLine);
    }
    void handleSplit(int id, float x, float y, float w, float h, bool vertical) {
        bool hv = cx.hover(x,y,w,h);
        if (hv || dragSplit==id) wantCursor = vertical ? 1 : 2;   // show the resize cursor on hover/drag
        if (hv && cx.in.pressed[0]) dragSplit = id;
        if (dragSplit == id) {
            if (cx.in.down[0]) {
                if (id==1) rightRatio = std::clamp((panelLeft ? cx.in.mx : (fbW - cx.in.mx))/(float)fbW, 0.12f, 0.75f);
                else if (id==2) { float midH=(float)rcViewport.extent.height; outlinerRatio = std::clamp((cx.in.my - rcHeader.extent.height)/midH, 0.12f, 0.88f); }
                else if (id==3) timelineH = std::clamp((float)fbH - cx.in.my, 36.f, (float)fbH*0.5f);
            } else dragSplit = 0;
        }
    }

    // Photoshop-style panel re-dock: while the tab-strip grip is held, highlight the LEFT/RIGHT drop column
    // (whichever half the cursor is over) and dock there on release. Drawn over the viewport, under menus/tooltips.
    void drawDockDrag() {
        if (!dockDragging) return;
        auto& th=cx.th;
        bool left = cx.in.mx < fbW*0.5f;
        float colW = std::clamp((float)fbW*rightRatio, 220.f*uiScale, (float)fbW*0.75f);
        float hy=(float)rcHeader.extent.height, hh=(float)fbH - hy - (homeLoaded()?timelineH:0.f);
        float hx = left ? 0.f : (float)fbW-colW;
        dl.rect(hx,hy,colW,hh, ui::rgba(64,140,235,55));
        dl.border(hx,hy,colW,hh, ui::rgba(96,170,250,225));
        cx.textAligned(hx, hy+hh*0.5f-10*uiScale, colW, 20*uiScale, left?"Dock LEFT":"Dock RIGHT", th.textSel, 1);
        // a little chip riding the cursor
        float cw2=100*uiScale, ch2=20*uiScale, cxp=std::min(cx.in.mx+12,(float)fbW-cw2), cyp=std::min(cx.in.my+12,(float)fbH-ch2);
        dl.rect(cxp,cyp,cw2,ch2, ui::rgba(30,32,40,238)); dl.border(cxp,cyp,cw2,ch2, th.accent);
        cx.textAligned(cxp,cyp,cw2,ch2,"Tool panel",th.text,1);
        if (!cx.in.down[0]) { panelLeft = left; dockDragging=false; }   // dropped
    }

    void drawTimeline() {
        if (!homeLoaded()) return;
        auto& th=cx.th; VkRect2D a=rcTimeline;
        float x=(float)a.offset.x, y=(float)a.offset.y, w=(float)a.extent.width, h=(float)a.extent.height;
        dl.rect(x,y,w,h, th.headerBg);
        float hh=20*uiScale;
        cx.textAligned(x+8*uiScale, y, 120*uiScale, hh, "Timeline", th.textDim, 0);
        if (!animOverride || !animScrub) { cx.textAligned(x+8*uiScale, y+hh, 320*uiScale, h-hh, "(no animation in this scene)", th.textDim, 0); return; }
        float bx=x+8*uiScale, by=y+hh+6*uiScale, bw=58*uiScale, bh=std::max(16.f, h-hh-12*uiScale);
        float dur=animDuration>0?animDuration:1.f, frac=std::clamp(*animScrub/dur, 0.f, 1.f);
        // Quest mirror helpers: PAUSE/scrub -> pin the headset on the SAME timeline phase; PLAY -> run at speed.
        auto qPin  = [&]{ if (syncQuest) {   // pin BOTH device clocks to this timeline instant
            sendQuestCmd("world setphase "+std::to_string(std::clamp(*animScrub/dur,0.f,1.f)));   // HzAnim (comet/birds)
            sendQuestCmd("world settime "+std::to_string(*animScrub)); } };                        // getTime shaders (flipbooks/train/uvscroll)
        auto qPlay = [&]{ if (syncQuest) sendQuestCmd("world "+std::to_string(questSpeed)); };
        if (cx.button(ui::hashId("tlplay"), bx, by, bw, bh, animPlaying?"Pause":"Play")) {
            animPlaying = !animPlaying;
            if (animPlaying) qPlay(); else qPin();   // pause here -> headset FREEZES on this exact instant
        }
        // ── →Quest mirror toggle: pause/scrub/speed also drive the headset (world cmd over the adb-forwarded bridge)
        float qbx=bx+bw+6*uiScale, qbw=70*uiScale;
        if (cx.button(ui::hashId("tlquest"), qbx, by, qbw, bh, syncQuest?"Quest: ON":"Quest: off")) {
            syncQuest = !syncQuest;
            if (syncQuest) {
#ifdef _WIN32
#ifdef _WIN32
                system("adb forward tcp:27042 tcp:27042 >nul 2>&1");   // ensure the bridge port is reachable
#else
                system("adb forward tcp:27042 tcp:27042 >/dev/null 2>&1");
#endif
#endif
                if (animPlaying) qPlay(); else qPin();
            } else sendQuestCmd("world off");
        }
        float tx=qbx+qbw+10*uiScale, tw=std::max(20.f, w-(tx-x)-120*uiScale), ty=by+bh*0.5f-3*uiScale;
        dl.rect(tx,ty,tw,6*uiScale, th.field); dl.border(tx,ty,tw,6*uiScale, th.border);
        dl.rect(tx,ty,tw*frac,6*uiScale, th.accent);
        dl.rect(tx+tw*frac-3*uiScale, by, 6*uiScale, bh, th.textSel);             // playhead (advances live while playing)
        if (cx.hover(tx,by,tw,bh) && cx.in.down[0]) { *animScrub = std::clamp((cx.in.mx-tx)/tw,0.f,1.f)*dur; *animOverride=true; animPlaying=false;   // scrub = pause+seek
            if (syncQuest) { double now=nowMs(); if (now-lastSendMs>66.0) { lastSendMs=now; qPin(); } } }   // headset jumps to the same time (throttled)
        // Live device-clock read-back so you can SEE the desktop time vs the Quest time (confirm they match).
        // Right-aligned across the WHOLE header width (minus the "Timeline" title) so the number never clips.
        if (syncQuest) { double now=nowMs(); if (now-lastQueryMs>250.0) { lastQueryMs=now; pollQuestTime(); } }
        char b[128];
        if (syncQuest) {
            bool matched = questTimeNow>=0.f && fabsf(questTimeNow-*animScrub)<0.5f;   // after settime, device == desktop
            if (questTimeNow<0.f) snprintf(b,sizeof b,"set %.2f/%.0fs  Quest=? (no reply)", *animScrub, dur);
            else snprintf(b,sizeof b,"set %.2f/%.0fs  Quest=%.2fs %s", *animScrub, dur, questTimeNow,
                          matched?"[SYNC]":(animPlaying?"[play]":"[pin]"));
        } else snprintf(b,sizeof b,"%.2f / %.2fs", *animScrub, dur);
        cx.textAligned(x+128*uiScale, y, w-136*uiScale, hh, b, th.textDim, 2);
    }

    // Floating context menu for a SCENE ITEM (right-click on its marker or its outliner row):
    // focus / hide / duplicate / remove — so colliders & co. are removable without hunting the 'x'.
    void drawItemContextMenu() {
        if (ctxItem < 0 || ctxItem >= (int)items.size()) { ctxOpen=false; ctxItem=-1; return; }
        auto& th=cx.th; auto& it=items[ctxItem];
        // acts on the WHOLE item multi-selection when the clicked row is part of it
        bool multiHit=false; for (int s:selItems) if (s==ctxItem) multiHit=true;
        std::vector<int> tgt = (multiHit && selItems.size()>1) ? selItems : std::vector<int>{ctxItem};
        std::string suf = tgt.size()>1 ? (" ("+std::to_string(tgt.size())+")") : std::string();
        std::vector<std::pair<std::string,int>> rows = {
            { it.name + "   " + sitem::typeName(it.type) + suf, -1 },
            { "Focus / teleport", 0 },
            { std::string(it.hidden ? "Show" : "Hide")+suf+"  (Space)", 1 },
            { std::string("Duplicate")+suf, 2 },
            { std::string("Remove")+suf, 3 } };
        float rh=th.rowH+2*uiScale, w=200*uiScale, h=rows.size()*rh+6*uiScale;
        float x=ctxX, y=ctxY; if (x+w>fbW) x=fbW-w; if (y+h>fbH) y=fbH-h; if (x<0)x=0; if (y<0)y=0;
        ctxRX=x; ctxRY=y; ctxRW=w; ctxRH=h;
        dl.rect(x,y,w,h, th.panelBg); dl.border(x,y,w,h, th.border);
        dl.pushClip(x,y,w,h);
        float ry=y+3*uiScale;
        for (auto& rw : rows) {
            if (rw.second<0) { cx.textAligned(x+8*uiScale,ry,w-10*uiScale,rh, rw.first.c_str(), th.textDim, 0); dl.rect(x+4*uiScale,ry+rh-1,w-8*uiScale,1,th.border); ry+=rh; continue; }
            bool hv=cx.hover(x,ry,w,rh);
            if (hv) dl.rect(x+1,ry,w-2,rh, th.accent);
            cx.textAligned(x+12*uiScale,ry,w-14*uiScale,rh, rw.first.c_str(), hv?th.textSel:th.text, 0);
            if (hv && cx.in.pressed[0]) {
                int a=rw.second;
                if      (a==0) focusItem(ctxItem);
                else if (a==1) { bool anyVis=false; for (int t:tgt) if (t<(int)items.size() && !items[t].hidden) anyVis=true;
                                 for (int t:tgt) if (t<(int)items.size()) items[t].hidden = anyVis; }
                else if (a==2) { pushItemUndo(items);
                                 for (int t:tgt) if (t<(int)items.size()) { sitem::Item cp=items[t]; cp.pos[0]+=0.5f; cp.pos[2]+=0.5f; /*name kept*/ items.push_back(std::move(cp)); }
                                 selItem=(int)items.size()-1; selItems.clear(); deselectAll(); }
                else if (a==3) { pushItemUndo(items); std::vector<int> del=tgt; std::sort(del.rbegin(), del.rend());
                                 for (int t:del) if (t<(int)items.size()) items.erase(items.begin()+t);
                                 selItem=-1; selItems.clear(); }
                ctxOpen=false; ctxItem=-1;
                dl.popClip(); return;   // items may have been mutated — stop iterating this frame
            }
            ry+=rh;
        }
        dl.popClip();
    }

    // Floating object context menu (right-click in the viewport). Items act on ctxMesh.
    void drawContextMenu() {
        if (!ctxOpen) { ctxR2W = 0; return; }
        if (ctxItem >= 0) { ctxR2W = 0; drawItemContextMenu(); return; }   // scene-item variant (outliner/marker right-click)
        if (ctxMesh<0 || ctxMesh>=(int)r->gpuMeshes.size()) { ctxOpen=false; ctxR2W=0; return; }
        auto& th=cx.th; VkGpuMesh& gm=r->gpuMeshes[ctxMesh];
        bool soloed=(r->soloMesh==ctxMesh), hidden=r->isHidden(ctxMesh);
        std::vector<int> tg = (inSel(ctxMesh) && sel.size()>1) ? sel : std::vector<int>{ctxMesh};   // context actions cover the WHOLE multi-selection
        std::string suf = tg.size()>1 ? (" ("+std::to_string(tg.size())+")") : std::string();
        int nHidden=0; for (int i=0;i<(int)r->gpuMeshes.size();++i) if (r->isHidden(i)) nHidden++;
        std::string colLbl = gm.dynamicVerts ? (isAnimCollider(ctxMesh) ? "Remove Animated Collider" : "Animated Collider (follows anim)")
                                              : (std::string("Make Mesh Collider (exact)")+suf);
        // ── GROUPED menu: short top level + hover submenus (a: action id, sub: submenu index, -1 = none) ──
        struct MRow { std::string t; int a; int sub; };
        std::vector<MRow> subCut = {
            {"KNIFE: draw a line, cut under it",45,-1},                       // 2D stroke, release = exact cut under the line
            {"Slice gizmo (place the cut plane)",44,-1},                      // gizmo-placed plane + live cut line, Enter cuts
            {"Slice in half X",35,-1},{"Slice in half Y",36,-1},{"Slice in half Z",37,-1},
            {"Cut region (pins: 2=split, 3+=cut out)",40,-1},
            {std::string("Cut hole HERE (r=")+std::to_string((int)(cutRadius*100)/100.f).substr(0,4)+"m)",22,-1},
            {"Split into connected pieces",23,-1} };
        std::vector<MRow> subGeo = {
            {"Stretch / scale (per-axis gizmo)",18,-1},
            {"Extend edges DOWN 1m (fill gaps)",19,-1},
            {"Extend edges OUTWARD 1m (fill gaps)",20,-1},
            {std::string(showHoles?"Hide hole inspector":"SHOW HOLES (click a marker to seal)"),33,-1},
            {"Seal hole HERE (nearest to click)",34,-1},
            {std::string("Seal ALL holes (keeps perimeter)")+suf,21,-1},
            {std::string(patchMode?"Patch tool: CANCEL":"Patch tool (click gap corners, Enter)"),38,-1},
            {"Bend/stretch tool (click grab point)",41,-1},
            {"Mirror X",24,-1},{"Mirror Y",25,-1},{"Mirror Z",26,-1},
            {"Rotate Y +90 (repeat to stack)",28,-1},{"Rotate Y 180",29,-1},
            {"Rotate X +90",30,-1},{"Rotate Z +90",31,-1},
            {std::string("Complete DOME (partial sky -> full 360°)")+suf,46,-1},
            {std::string("Complete SPHERE (360° enclosed, closes bottom)")+suf,47,-1},
            {std::string("Fuse selection into ONE mesh")+suf,32,-1},
            {std::string("Fuse + REBUILD surface")+suf+" (clean retri)",42,-1},
            {std::string("Re-UV flat (texture spans ONCE)")+suf,39,-1} };
        std::vector<MRow> subCmp = {
            {colLbl,5,-1},
            {"Mesh Collider (static, exact tris)",16,-1},
            {std::string("REMOVE mesh collider")+suf+" ("+std::to_string(boundMeshColliders(tg))+" bound)",48,-1},
            {"Navmesh (walkable scene)",17,-1},
            {"Box Collider (wrap mesh)",10,-1},
            {"Wall (faces camera)",11,-1},
            {"Spawn Point (on top)",12,-1},
            {"Chair / Seat (on top)",13,-1},
            {"Locomotion Hotspot",14,-1},
            {"Kill Floor / Boundary",15,-1} };
        std::vector<MRow> main = {
            {gm.name, -1, -1},
            {"Focus / teleport",0,-1},
            {soloed?"Unsolo":"Solo only",1,-1},
            {std::string(hidden?"Unhide":"Hide")+suf+"  (Space)",2,-1},
            {std::string("Duplicate")+suf+"  (Ctrl+D)",8,-1},
            {std::string("Delete")+suf+"  (Del)",9,-1},
            {std::string("Save as PREFAB")+suf,43,-1},                        // reusable asset: Scene > Prefabs spawns it, or drag the file in
            {"Cut & slice",-2,0},
            {"Geometry / repair",-2,1},
            {"Make component",-2,2},
            {std::string(noColMeshes.count(ctxMesh)?"Collision: INCLUDE again":"Collision: EXCLUDE (walk-through)")+suf,27,-1},
            {std::string(isSkyboxMesh(ctxMesh)?"Unmark skybox backdrop":"Make skybox backdrop")+suf,6,-1},
            {std::string("Reset transform")+suf,3,-1},
            {"Copy name",4,-1} };
        if (nHidden>0) main.push_back({std::string("Unhide ALL (")+std::to_string(nHidden)+")",7,-1});
        // The FULL action set (shared by main rows and submenu rows).
        auto runAct=[&](int a){
                if (a==0) focusMesh(gm);
                else if (a==1) { r->soloMesh = soloed?-1:ctxMesh; if (!soloed) focusMesh(gm); }   // solo AUTO-FOCUSES (a solo you can't see is useless)
                else if (a==2) { for (int t:tg) r->setHidden(t, !hidden); }                       // hide/unhide ALL selected
                else if (a==3) { std::vector<int> mm; std::vector<Xform> bb,aa;                    // reset ALL selected (single undo)
                    for (int t:tg){ auto& g=r->gpuMeshes[t]; mm.push_back(t); bb.push_back(captureX(g));
                        g.editT[0]=g.editT[1]=g.editT[2]=0; g.editR[0]=g.editR[1]=g.editR[2]=0; g.editR[3]=1; g.editS[0]=g.editS[1]=g.editS[2]=1; recomputeModel(g); aa.push_back(captureX(g)); }
                    pushUndo(mm,bb,aa); }
                else if (a==4) glfwSetClipboardString(win, gm.name.c_str());
                else if (a==5) { for (int t:tg) addMeshCollider(t); }                             // collider on ALL selected
                else if (a==6) {   // mark/unmark skybox backdrop on ALL selected
                    bool makeSky = !isSkyboxMesh(ctxMesh);   // base the whole batch on the clicked mesh's new state (consistent toggle)
                    for (int t : tg) { if (isSkyboxMesh(t) != makeSky) toggleSkybox(t); }
                    setStatus(std::string(makeSky?"Marked ":"Unmarked ")+std::to_string(tg.size())+" mesh(es) as skybox backdrop (far-clip-exempt)");
                }
                else if (a==7) { for (int i=0;i<(int)r->gpuMeshes.size();++i) r->setHidden(i,false); setStatus("Unhid ALL meshes"); }   // unhide everything
                else if (a==8) { if (!inSel(ctxMesh)) selectOne(ctxMesh); duplicateSelected(); }   // clone selection (independent copy, cooks too)
                else if (a==9) { if (!inSel(ctxMesh)) selectOne(ctxMesh); toggleDeleteSelected(); }  // drop from render + cook (Ctrl+Z restores)
                else if (a==18) { if (!inSel(ctxMesh)) selectOne(ctxMesh); gizmoOp=2; tab=TAB_OBJECT;   // STRETCH: per-axis Scale gizmo + the Object tab's Scale fields
                                  setStatus("Scale gizmo ON - drag a box handle to stretch per-axis, or the CENTER square for uniform (all 3 axes) - or type in Object > Scale"); }
                // GEOMETRY ops apply to the WHOLE multi-selection (each source -> its own result, all re-selected)
                else if (a==19) forEachSelMesh([&](int m){ extendMeshBoundary(m, extendDist, 0); });   // grow DOWN off open edges
                else if (a==20) forEachSelMesh([&](int m){ extendMeshBoundary(m, extendDist, 1); });   // grow OUTWARD (radial)
                else if (a==21) forEachSelMesh([&](int m){ sealMeshHoles(m); });
                else if (a==22) { if (ctxHitValid) forEachSelMesh([&](int m){ cutMeshHole(m, ctxHitP, cutRadius); });   // carve at the clicked spot
                                  else setStatus("cut: no surface point under the cursor (right-click ON the mesh)"); }
                else if (a==23) forEachSelMesh([&](int m){ splitMeshParts(m); });
                else if (a==24) forEachSelMesh([&](int m){ mirrorMesh(m, 0); });
                else if (a==25) forEachSelMesh([&](int m){ mirrorMesh(m, 1); });
                else if (a==26) forEachSelMesh([&](int m){ mirrorMesh(m, 2); });
                else if (a==28) forEachSelMesh([&](int m){ rotateMeshGeom(m, 1, 90); });
                else if (a==29) forEachSelMesh([&](int m){ rotateMeshGeom(m, 1, 180); });
                else if (a==30) forEachSelMesh([&](int m){ rotateMeshGeom(m, 0, 90); });
                else if (a==31) forEachSelMesh([&](int m){ rotateMeshGeom(m, 2, 90); });
                else if (a==32) fuseSelectedMeshes();
                else if (a==42) rebuildSurface();
                else if (a==43) savePrefabFromSelection();
                else if (a==33) { showHoles=!showHoles; if (showHoles){ refreshHoles(); setStatus(std::to_string(holeLoops.size())+" boundary loop(s) outlined - CLICK an orange marker to seal that hole"); } }
                else if (a==34) { if (ctxHitValid) sealMeshHoles(ctxMesh, ctxHitP);
                                  else setStatus("seal: right-click ON the mesh near the hole"); }
                else if (a==35) forEachSelMesh([&](int m){ sliceMesh(m, 0); });
                else if (a==36) forEachSelMesh([&](int m){ sliceMesh(m, 1); });
                else if (a==37) forEachSelMesh([&](int m){ sliceMesh(m, 2); });
                else if (a==44) startSliceGizmo(ctxHitValid?ctxHitP:nullptr);
                else if (a==45) startKnife();
                else if (a==46) forEachSelMesh([&](int m){ completeDome(m, false); });  // partial sky dome -> full 360° dome (to the rim)
                else if (a==47) forEachSelMesh([&](int m){ completeDome(m, true);  });  // -> fully enclosed 360° sphere (closes the bottom)
                else if (a==38) { patchMode=!patchMode; pinIsCut=false; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
                                  if (patchMode) setStatus("PATCH: click the gap's corner points in the viewport (3+), Enter builds, Esc cancels"); }
                else if (a==39) forEachSelMesh([&](int m){ reUVFlat(m); });
                else if (a==40) { patchMode=true; pinIsCut=true; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
                                  setStatus("CUT: 2 pins = SPLIT along the line, 3+ pins = cut the polygon out; Enter applies"); }
                else if (a==41) { patchMode=true; pinIsBend=true; pinIsCut=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
                                  setStatus("BEND: click the grab point, set Radius + Offset (Object > Geometry), Enter deforms"); }
                else if (a==27) { bool exc = !noColMeshes.count(ctxMesh);      // collision walk-through toggle (whole selection)
                                  for (int t2:tg) { if (exc) noColMeshes.insert(t2); else noColMeshes.erase(t2); }
                                  bakeNavmeshes(this->items);                  // preview + cook collision re-bake immediately ("items" here = the scene items)
                                  setStatus(exc ? "Collision EXCLUDED - you can walk through these (cook follows)"
                                                : "Collision INCLUDED again"); }
                // "Make component from mesh" — BULK: one component per mesh in the whole selection (tg), not just the clicked one.
                else if (a==10) for(int t:tg) addItemFromMesh(sitem::BOXCOL, t);
                else if (a==11) for(int t:tg) addItemFromMesh(sitem::WALLPLACE, t);
                else if (a==12) for(int t:tg) addItemFromMesh(sitem::SPAWN, t);
                else if (a==13) for(int t:tg) addItemFromMesh(sitem::CHAIR, t);
                else if (a==14) for(int t:tg) addItemFromMesh(sitem::HOTSPOT, t);
                else if (a==15) for(int t:tg) addItemFromMesh(sitem::BOUNDARY, t);
                else if (a==16) for(int t:tg) addMeshCollider(t, true);   // STATIC mesh collider (exact tris) on EACH selected mesh
                else if (a==48) { int nrm=removeMeshColliders(tg);        // take the collider component OFF the mesh(es)
                                  setStatus(nrm ? ("Removed "+std::to_string(nrm)+" mesh collider component(s) - Ctrl+Z restores")
                                                : "no mesh collider is bound to this mesh"); }
                else if (a==17) addNavmesh(1);                            // generate the walkable scene navmesh (smart)
                ctxOpen=false; ctxSub=-1;
        };
        // AUTO-FIT width: each panel is as wide as its longest label (no more truncated rows).
        float rh=th.rowH+2*uiScale;
        auto fitW=[&](const std::vector<MRow>& rows){ float mw=120*uiScale;
            for (auto& m2 : rows){ float tw=dl.textW(m2.t.c_str())+28*uiScale + (m2.sub>=0?16*uiScale:0); if (tw>mw) mw=tw; }
            return std::min(mw, fbW*0.6f); };
        float w=fitW(main), h=main.size()*rh+6*uiScale;
        float x=ctxX, y=ctxY; if (x+w>fbW) x=fbW-w; if (y+h>fbH) y=fbH-h; if (x<0)x=0; if (y<0)y=0;
        ctxRX=x; ctxRY=y; ctxRW=w; ctxRH=h; ctxR2W=0;
        // draws one panel; returns nothing, but handles hover (submenu open/close) + clicks
        auto drawPanel=[&](float px, float py, float pw, const std::vector<MRow>& rows, bool isSub){
            float ph=rows.size()*rh+6*uiScale;
            dl.rect(px,py,pw,ph, th.panelBg); dl.border(px,py,pw,ph, th.border);
            dl.pushClip(px,py,pw,ph);
            float ry=py+3*uiScale;
            for (auto& it : rows) {
                if (it.a==-1 && it.sub<0) { cx.textAligned(px+8*uiScale,ry,pw-10*uiScale,rh, it.t.c_str(), th.textDim, 0); dl.rect(px+4*uiScale,ry+rh-1,pw-8*uiScale,1,th.border); ry+=rh; continue; }
                bool hv=cx.hover(px,ry,pw,rh);
                bool open=(!isSub && it.sub>=0 && ctxSub==it.sub);
                if (hv || open) dl.rect(px+1,ry,pw-2,rh, th.accent);
                cx.textAligned(px+12*uiScale,ry,pw-14*uiScale,rh, it.t.c_str(), (hv||open)?th.textSel:th.text, 0);
                if (it.sub>=0) cx.textAligned(px,ry,pw-8*uiScale,rh, ">", (hv||open)?th.textSel:th.textDim, 2);   // submenu arrow
                if (hv && !isSub) {   // hovering a MAIN row: open its submenu / close a stale one
                    if (it.sub>=0) { if (ctxSub!=it.sub){ ctxSub=it.sub; ctxSubY=ry; } }
                    else ctxSub=-1;
                }
                if (hv && cx.in.pressed[0]) { if (it.sub>=0) { ctxSub=it.sub; ctxSubY=ry; } else if (it.a>=0) runAct(it.a); }
                ry+=rh;
            }
            dl.popClip();
        };
        drawPanel(x,y,w,main,false);
        if (ctxOpen && ctxSub>=0) {   // submenu panel beside the parent row (flips LEFT when the screen edge is near)
            const std::vector<MRow>& rows2 = ctxSub==0?subCut : ctxSub==1?subGeo : subCmp;
            float w2=fitW(rows2), h2=rows2.size()*rh+6*uiScale;
            float x2 = (x+w+w2<=fbW) ? x+w-2*uiScale : x-w2+2*uiScale;
            float y2 = ctxSubY; if (y2+h2>fbH) y2=fbH-h2; if (y2<0) y2=0; if (x2<0) x2=0;
            ctxR2X=x2; ctxR2Y=y2; ctxR2W=w2; ctxR2H=h2;
            drawPanel(x2,y2,w2,rows2,true);
        }
    }

    void record(VkCommandBuffer cmd) { uiDraw.record(cmd, dl); }

    // ── Pre-cook "no collision" warning modal. Fires from startCook() when envHasCollision()==false: no auto-floor,
    //    no Navmesh, no Box collider = the port has nothing to stand on. Centered card over a dim scrim; the caller
    //    parks the mouse off-screen so only THESE buttons are live. "Cook anyway" proceeds via startCookNow(). ──
    void drawCookWarn() {
        auto& th = cx.th; float W=(float)fbW, H=(float)fbH;
        dl.rect(0,0,W,H, ui::rgba(0,0,0,150));                                   // dim everything behind
        float bw = std::min(480.f*uiScale, W-40*uiScale), bh = 214.f*uiScale;
        float bx = (W-bw)*0.5f, by = (H-bh)*0.5f, pad = 16.f*uiScale, rh = th.rowH;
        dl.rect(bx,by,bw,bh, th.panelBg); dl.border(bx,by,bw,bh, th.border);
        dl.rect(bx,by,bw,3*uiScale, ui::rgba(240,180,60));                       // amber warning stripe
        float y = by + pad + 4*uiScale;
        cx.textAligned(bx+pad, y, bw-2*pad, rh+2*uiScale, "No collision in this environment", ui::rgba(245,190,70), 0);
        y += rh + 10*uiScale;
        const char* lines[] = {
            "Nothing to stand on: no Navmesh, no Box collider, and",
            "Auto-floor collision is off. The player will fall through",
            "the floor and respawn forever.",
            "",
            "Add a Navmesh or Box collider (Scene tab -> + Add), or turn",
            "on \"Auto floor collision\" in the Cook tab.",
        };
        for (const char* ln : lines) { cx.textAligned(bx+pad, y, bw-2*pad, rh, ln, th.textDim, 0); y += rh*0.92f; }
        float bwid = (bw-2*pad-10*uiScale)/2, bhh = rh+8*uiScale, byb = by+bh-pad-bhh;
        if (cx.button(ui::hashId("cwcancel"), bx+pad, byb, bwid, bhh, "Cancel") || cx.in.keyRepeat[ui::KEY_ESCAPE])
            cookWarnOpen=false;
        if (cx.button(ui::hashId("cwgo"), bx+pad+bwid+10*uiScale, byb, bwid, bhh, "Cook anyway", true)) {
            cookWarnOpen=false; startCookNow();
        }
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  SPACES
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void drawHeader() {
        auto& th = cx.th; float h = (float)rcHeader.extent.height, W=(float)fbW, pad=8*uiScale;
        dl.rect(0,0,W,h, th.headerBg);
        dl.rect(0,h-1,W,1, th.splitLine);
        cx.textAligned(pad, 0, 200*uiScale, h, "V79  Quest Home  Editor", th.text, 0);
        // menu strip (visual; functional menus land in cleanup phase)
        const char* menus[] = {"File","Edit","Object","View"};
        const char* menuTips[] = {
            "File actions - Save / Load a session, Export or Import a glTF, Cook to APK.\n(Use the Save / Load / Cook buttons on the right for now.)",
            "Edit actions - Undo / Redo, and the per-object edits in the Object tab.",
            "Object actions - add / duplicate / delete, and transform the selected mesh\n(see the Object and Geometry tabs).",
            "View options - camera + viewport display (grid, gizmos, far-clip overlay)."};
        float mx = 220*uiScale;
        for (int mi2=0; mi2<4; ++mi2) { const char* m=menus[mi2]; float w = dl.textW(i18n::tr(m))+18*uiScale;
            cx.button(ui::hashId(m), mx, 3*uiScale, w, h-6*uiScale, m); cx.tip(mx, 3*uiScale, w, h-6*uiScale, menuTips[mi2]); mx += w+2*uiScale; }
        // language selector (GitHub #10): click for a dropdown of all languages; persisted + re-bakes the atlas
        { const char* ll = i18n::langName(i18n::g_lang); float w = dl.textW(ll)+26*uiScale; if (w < 64*uiScale) w = 64*uiScale;
          float lx = mx + 10*uiScale; langBtnX=lx; langBtnY=3*uiScale; langBtnW=w; langBtnH=h-6*uiScale;
          if (cx.button(ui::hashId("hdrlang"), lx, langBtnY, w, langBtnH, ll)) { langMenuOpen=!langMenuOpen; ctxOpen=false; addMenuOpen=false; }
          cx.tip(lx, langBtnY, w, langBtnH, "UI language"); }
        // right side: Save / Load (persist the session) + Cook quick-button + progress
        float bw = 96*uiScale, bh = h-8*uiScale, bx = W - bw - pad;
        if (cooking) { cx.progressBar(bx-150*uiScale, 4*uiScale, 150*uiScale+bw, bh, cookProg.load(), stageStr().c_str()); }
        else {
            if (cx.button(ui::hashId("hdrcook"), bx, 4*uiScale, bw, bh, "Cook APK", true)) { tab=TAB_COOK; startCook(); }
            cx.tip(bx, 4*uiScale, bw, bh, "Cook the loaded home into an installable Quest APK (and install it\nif Install is on). Opens the Cook tab. Same as the Cook button there.");
            float sw=56*uiScale;
            if (cx.button(ui::hashId("hdrload"), bx-sw-4*uiScale, 4*uiScale, sw, bh, "Load")) loadProject();
            cx.tip(bx-sw-4*uiScale, 4*uiScale, sw, bh, "Load a saved session (.hsledit) or a cooked APK back into the editor.\nRestores your edits, selection and cook settings.");
            if (cx.button(ui::hashId("hdrsave"), bx-2*sw-8*uiScale, 4*uiScale, sw, bh, "Save")) saveProject();
            cx.tip(bx-2*sw-8*uiScale, 4*uiScale, sw, bh, "Save this session to a .hsledit file (Ctrl+S) so your edits persist\nand you can reopen exactly where you left off.");
            // Blender / glTF round-trip: export the home to a glTF project, edit it in Blender (or any 3D tool),
            // then import the edited glTF/glb back in. Labels say plainly which way each button goes.
            float blw = 104*uiScale;
            float bex = bx-2*sw-12*uiScale-blw;
            if (cx.button(ui::hashId("hdrblend"),  bex, 4*uiScale, blw, bh, "Export glTF")) exportBlender();
            cx.tip(bex, 4*uiScale, blw, bh, "Export this home to a glTF 2.0 project (meshes + materials + textures\n+ transforms) that opens in Blender or any 3D tool for editing.\nEdit it, then use \"Import glTF\" to bring it back and cook it.");
            if (cx.button(ui::hashId("hdrimport"), bex-blw-4*uiScale, 4*uiScale, blw, bh, "Import glTF")) importBlender();
            cx.tip(bex-blw-4*uiScale, 4*uiScale, blw, bh, "Import a glTF/glb you edited (e.g. in Blender) as a fresh session,\nso you can preview and cook the edited home. Pairs with \"Export glTF\".");
            { const char* s = autoSaveOn ? "Auto-save: On" : "Auto-save: Off"; float aw = dl.textW(s)+14*uiScale;   // crash-resistance: periodic .autosave + recovery on reopen
              float ax = bex-2*blw-8*uiScale-aw; float ay0=4*uiScale;
              if (cx.tab(ui::hashId("hdrautosave"), ax, ay0, aw, bh, s, autoSaveOn)) autoSaveOn = !autoSaveOn;
              cx.tip(ax, ay0, aw, bh, "Auto-save the session to <env>.hsledit.autosave every 30s\n(only when something changed). If the editor crashes or closes\nwith unsaved edits, reopening offers to restore them.\nSave (Ctrl+S) still writes the real .hsledit."); }
        }
    }

    // Crash-recovery strip: an .autosave newer than the saved session was found on load — offer Restore / Dismiss.
    void drawRecoverBanner() {
        if (!recoverOffer) return;
        // Auto-hide the nag after 5s (non-destructive: leaves the .autosave on disk, just stops nagging).
        if (std::chrono::duration<float>(std::chrono::steady_clock::now() - recoverOfferAt).count() > 5.0f) { recoverOffer = false; return; }
        auto& th = cx.th; float y = (float)rcHeader.extent.height, h = 26*uiScale;
        float W = (float)rcViewport.extent.width;   // span the viewport pane; the right column stays usable
        dl.rect(0, y, W, h, ui::rgba(115,85,20,240)); dl.rect(0, y+h-1, W, 1, th.splitLine);
        cx.textAligned(8*uiScale, y, W-190*uiScale, h, "Auto-save found (newer than your last save) - restore the recovered session?", ui::rgba(255,232,170), 0);
        float bw = 84*uiScale, bh = h-6*uiScale;
        if (cx.button(ui::hashId("asrest"), W-2*bw-16*uiScale, y+3*uiScale, bw, bh, "Restore", true)) restoreAutosave();
        if (cx.button(ui::hashId("asdism"), W-bw-8*uiScale,    y+3*uiScale, bw, bh, "Dismiss")) dismissAutosave();
    }

    void drawViewportOverlay() {
        auto& th = cx.th; VkRect2D v = rcViewport;
        dl.pushClip((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,(float)v.extent.height);
        // little viewport header bar
        float bh = 20*uiScale;
        dl.rect((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,bh, ui::withA(th.headerBg,210));
        cx.textAligned(v.offset.x+8*uiScale, v.offset.y, 200*uiScale, bh, "Viewport", th.textDim, 0);
        // transform-mode pills (G/R/S) + axis space
        const char* ops[]={"Move","Rotate","Scale"};
        const char* opTips[]={"Move (G) - drag the gizmo arrows to translate the selected mesh.",
                              "Rotate (R) - drag the gizmo rings to rotate the selected mesh.",
                              "Scale (S) - drag the gizmo handles to resize the selected mesh."};
        float px=v.offset.x+90*uiScale;
        for (int i=0;i<3;i++){ float w=dl.textW(ops[i])+14*uiScale; if (cx.tab(ui::hashId(2000+i, 7), px, v.offset.y, w, bh, ops[i], gizmoOp==i)) gizmoOp=i; cx.tip(px, (float)v.offset.y, w, bh, opTips[i]); px+=w; }
        { const char* s = gizmoLocal?"Local":"World"; float w=dl.textW(s)+14*uiScale; if (cx.tab(ui::hashId("axspace"), px+6*uiScale, v.offset.y, w, bh, s, false)) gizmoLocal=!gizmoLocal; cx.tip(px+6*uiScale, (float)v.offset.y, w, bh, "Gizmo orientation: World (axis-aligned) or Local (the mesh's own\naxes). Click to toggle."); px+=w+6*uiScale; }
        // per-axis gizmo locks (Shift+X/Y/Z): a lit pill = that axis is LOCKED (dimmed on the gizmo, can't be grabbed)
        { const char* axn[3]={"X","Y","Z"}; px+=4*uiScale;
          for (int k=0;k<3;k++){ float w=dl.textW(axn[k])+10*uiScale;
              if (cx.tab(ui::hashId(2300+k, 11), px, v.offset.y, w, bh, axn[k], lockAxis[k])) lockAxis[k]=!lockAxis[k];
              cx.tip(px, (float)v.offset.y, w, bh, k==0?"Lock the X axis (Shift+X): the gizmo ignores it while lit.":k==1?"Lock the Y axis (Shift+Y): the gizmo ignores it while lit.":"Lock the Z axis (Shift+Z): the gizmo ignores it while lit.");
              px+=w+2*uiScale; } px+=4*uiScale; }
        { const char* s = playSim?"Stop (P)":"Walk (P)"; float w=dl.textW(s)+16*uiScale; if (cx.tab(ui::hashId("walksim"), px+10*uiScale, v.offset.y, w, bh, s, playSim)) { if(playSim) stopSim(); else startSim(); } cx.tip(px+10*uiScale, (float)v.offset.y, w, bh, "Walk the home in first person (P): WASD + mouse to move on the\ncollision/navmesh - test that you can stand and don't fall through.\nPress P again to exit."); px+=w+16*uiScale; }
        // camera fly speed (drag or type) — in the header strip so it never fires a viewport pick
        { cx.textAligned(px, v.offset.y, 34*uiScale, bh, "Spd", th.textDim, 0); cx.dragFloat(ui::hashId("camspd"), px+34*uiScale, v.offset.y+1*uiScale, 54*uiScale, bh-2*uiScale, r->cam.speed, 0.1f, "%.1f"); px+=92*uiScale; }
        // PC PREVIEW AUDIO — front-and-center toggle (was buried in cook options). Mutes/unmutes the desktop background loop.
        { const char* s = previewAudio?"Audio: On":"Audio: Off"; float w=dl.textW(s)+16*uiScale;   // ASCII — the UI font has no emoji glyphs (the speaker emoji rendered as "????")
          if (cx.tab(ui::hashId("hdraudio"), px+8*uiScale, v.offset.y, w, bh, s, previewAudio)) { previewAudio=!previewAudio; g_audioMuted.store(!previewAudio, std::memory_order_relaxed); } cx.tip(px+8*uiScale, (float)v.offset.y, w, bh, "Play the home's background audio loop here on the PC (what will ship\nin the cook). Click to mute/unmute the preview."); px+=w+8*uiScale; }
        // ALWAYS-ON-TOP toggle (window is NOT pinned by default now). "Pin" = keep the editor above other windows.
        { const char* s = alwaysOnTop?"Pin: On":"Pin: Off"; float w=dl.textW(s)+16*uiScale;
          if (cx.tab(ui::hashId("hdrpin"), px+8*uiScale, v.offset.y, w, bh, s, alwaysOnTop)) { alwaysOnTop=!alwaysOnTop; if(win) glfwSetWindowAttrib(win, GLFW_FLOATING, alwaysOnTop?GLFW_TRUE:GLFW_FALSE); } cx.tip(px+8*uiScale, (float)v.offset.y, w, bh, "Keep the editor window ABOVE other windows (handy next to Blender\nor the headset mirror). Click to toggle."); px+=w+8*uiScale; }
        // X-RAY: selected mesh wireframe over the boxes (align colliders to meshes without camera-angle ambiguity)
        { const char* s = "X-ray"; float w=dl.textW(s)+16*uiScale;
          if (cx.tab(ui::hashId("hdrxray"), px+8*uiScale, v.offset.y, w, bh, s, xrayMesh)) xrayMesh=!xrayMesh; cx.tip(px+8*uiScale, (float)v.offset.y, w, bh, "X-ray: draw the selected mesh as a wireframe over everything, so you\ncan line up box colliders with the mesh regardless of camera angle."); px+=w+8*uiScale; }
        if (playSim) cx.textAligned(v.offset.x+8*uiScale, v.offset.y+v.extent.height-40*uiScale, v.extent.width-16, 18*uiScale, "WALK MODE - WASD+mouse to walk the navmesh, P to exit", ui::rgba(120,230,140), 0);
        // (overlay/gizmo toggles moved to their single home: the Scene tab "Gizmos / overlays" section —
        //  they used to duplicate here in the viewport header, which read as a confusing second selector)
        // stats (bottom-left)
        char st[128]; snprintf(st,sizeof st,"%zu objects   sel: %s", r->gpuMeshes.size(), selected>=0?r->gpuMeshes[selected].name.c_str():"-");
        cx.textAligned(v.offset.x+8*uiScale, v.offset.y+v.extent.height-20*uiScale, v.extent.width-16, 18*uiScale, st, th.textDim, 0);
        // GPU-upload loading bar (progressive streaming): centered at the bottom until every mesh has landed.
        if (uploadTotal > 0 && uploadCur < uploadTotal) {
            float bw = std::min(420.f*uiScale, v.extent.width*0.55f), bh2 = 9*uiScale;
            float bx = v.offset.x + (v.extent.width-bw)*0.5f, by = v.offset.y + v.extent.height - 44*uiScale;
            char lb[96]; snprintf(lb,sizeof lb,"Loading meshes onto GPU   %d / %d", uploadCur, uploadTotal);
            cx.textAligned(bx, by-20*uiScale, bw, 18*uiScale, lb, th.text, 1);
            dl.rect(bx, by, bw, bh2, ui::rgba(28,32,44));
            dl.rect(bx, by, bw*(float)uploadCur/(float)uploadTotal, bh2, th.accent);
            dl.border(bx, by, bw, bh2, th.border);
        }
        dl.popClip();
    }

    void drawOutliner() {
        auto& th = cx.th; VkRect2D a = rcOutliner;
        float x=(float)a.offset.x, y=(float)a.offset.y, w=(float)a.extent.width, h=(float)a.extent.height;
        dl.rect(x,y,w,h, th.panelBg);
        float hh = 22*uiScale;
        dl.rect(x,y,w,hh, th.headerBg);
        cx.textAligned(x+8*uiScale,y,70*uiScale,hh,"Outliner",th.text,0);
        // "+ Add" -> the Meta-component menu (a way to spawn each entity)
        float addW=44*uiScale, addX=x+66*uiScale;
        if (cx.button(ui::hashId("oadd"), addX, y+3*uiScale, addW, hh-6*uiScale, "+ Add", true)) { addMenuOpen=true; ctxOpen=false; addMenuX=addX; addMenuY=y+hh; }
        // ── dedicated, full-width SEARCH ROW (was a tiny unlabeled box in the header — easy to miss) ──
        float sy = y+hh;
        dl.rect(x, sy, w, hh, th.headerBg);
        cx.textAligned(x+8*uiScale, sy, 46*uiScale, hh, "Search", th.textDim, 0);
        std::string sb = search;
        float sfx = x+52*uiScale, sfw = std::max(60.f*uiScale, w-52*uiScale-8*uiScale);
        cx.textField(ui::hashId("osearch"), sfx, sy+3*uiScale, sfw, hh-6*uiScale, sb);
        strncpy(search, sb.c_str(), sizeof(search)-1); search[sizeof(search)-1]=0;
        if (!search[0]) cx.textAligned(sfx+7*uiScale, sy, sfw, hh, "filter meshes by name...", ui::rgba(120,120,128), 0);
        // Ctrl+A = select ALL meshes matching the current filter; Ctrl+C = copy the selected mesh names
        // (one per line) to the system clipboard. Edge-detected so each fires once per keypress.
        if (win) {
            bool ctrl = glfwGetKey(win,GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS || glfwGetKey(win,GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
            bool kA = glfwGetKey(win,GLFW_KEY_A)==GLFW_PRESS, kC = glfwGetKey(win,GLFW_KEY_C)==GLFW_PRESS;
            // ⛔ When ANY input field has keyboard focus, Ctrl+A / Ctrl+C belong to the TEXT (handled in ui_core editKeys),
            // never the scene select-all/copy. (Keep prevKey* fresh so there's no stray edge-trigger when the field blurs.)
            if (cx.kbFocus) { prevKeyA=kA; prevKeyC=kC; }
            else {
            auto matchesFilter=[&](int i){ if(!search[0]) return true; std::string ln=r->gpuMeshes[i].name,ls=search; for(char&c:ln)c=(char)tolower((unsigned char)c); for(char&c:ls)c=(char)tolower((unsigned char)c); return ln.find(ls)!=std::string::npos; };
            if (ctrl && kA && !prevKeyA) {
                sel.clear();
                for (int i=0;i<(int)r->gpuMeshes.size();++i) if (matchesFilter(i) && !r->isHidden(i) && !r->isDeleted(i)) sel.push_back(i);   // VISIBLE only (hidden + deleted excluded)
                selected = sel.empty()?-1:sel.back(); r->selectedMesh=selected; selItem=-1;
            }
            if (ctrl && kC && !prevKeyC && !sel.empty()) {
                std::string clip; for (int m:sel) if (m>=0&&m<(int)r->gpuMeshes.size()) { clip+=r->gpuMeshes[m].name; clip+="  ["+std::to_string(m)+"]\n"; }
                glfwSetClipboardString(win, clip.c_str());
            }
            prevKeyA=kA; prevKeyC=kC;
            }
        }
        // scrollable content: SCENE ITEMS (the things you add) then MESHES  (header row + search row = 2*hh)
        float listY = y+2*hh, listH = h-2*hh, rowH = th.rowH;
        dl.pushClip(x, listY, w, listH);
        if (cx.hover(x,listY,w,listH) && cx.in.wheel!=0) outlinerScroll -= cx.in.wheel*rowH*3;
        int nItems=(int)items.size(), nMesh=(int)r->gpuMeshes.size();
        // FILTERED count for the scroll extent: with a search active only matching rows take space, so the scroll must
        // clamp to the FILTERED height — else a stale scroll offset (from before the search) leaves the packed matches
        // scrolled ABOVE the viewport => "search shows nothing" (the bug). Lowercase substring = case-insensitive.
        // The search now covers SCENE ITEMS too (name + type, e.g. "collider" lists every collider).
        std::string lsq = search; for (char& c : lsq) c=(char)tolower((unsigned char)c);
        auto meshMatches=[&](int i){ if(!search[0]) return true; std::string ln=r->gpuMeshes[i].name; for(char&c:ln)c=(char)tolower((unsigned char)c); return ln.find(lsq)!=std::string::npos; };
        auto itemMatches=[&](int i){ if(!search[0]) return true;
            std::string ln=items[i].name; ln+=' '; ln+=sitem::typeName(items[i].type);
            for(char&c:ln)c=(char)tolower((unsigned char)c); return ln.find(lsq)!=std::string::npos; };
        // DELETED meshes are GONE from the list entirely (no [DEL] rows) — only Ctrl+Z resurrects them.
        auto meshRowVisible=[&](int i){ return !r->isDeleted(i) && meshMatches(i); };
        int nMeshShown = 0; for (int i=0;i<nMesh;i++) if (meshRowVisible(i)) nMeshShown++;
        int nItemsShown = nItems;
        if (search[0]) { nItemsShown=0; for (int i=0;i<nItems;i++) if (itemMatches(i)) nItemsShown++; }
        float total = (nItems?(1+(itemsCollapsed?0:nItemsShown))*rowH:0) + (1+(meshesCollapsed?0:nMeshShown))*rowH;
        // SCROLL-TO-SELECTION (duplicates/geometry results land at the END of the list — jump there so
        // the new row is visible instead of "where did my duped mesh go?!")
        if (scrollToSel && selected>=0 && selected<nMesh && !meshesCollapsed) {
            float ty = (nItems?(1+(itemsCollapsed?0:nItemsShown))*rowH:0) + rowH;   // rows before the mesh section
            for (int i2=0;i2<selected;++i2) if (meshRowVisible(i2)) ty+=rowH;
            outlinerScroll = std::clamp(ty - listH*0.5f, 0.f, std::max(0.f, total-listH));
            scrollToSel = false;
        }
        outlinerScroll = std::clamp(outlinerScroll, 0.f, std::max(0.f, total-listH));
        float ry = listY - outlinerScroll;
        auto onScreen=[&](float yy){ return yy+rowH>listY && yy<listY+listH; };
        // Collapsible section header: "v NAME (count)" / "> NAME (count)" — click toggles.
        auto sectionHeader=[&](const char* name, int count, bool& collapsed)->void{
            if (onScreen(ry)) {
                bool hv = cx.hover(x,ry,w,rowH);
                if (hv) dl.rect(x,ry,w,rowH,th.rowHover);
                char hd[96]; snprintf(hd,sizeof hd,"%s %s  (%d)", collapsed?">":"v", name, count);
                cx.textAligned(x+6*uiScale,ry,w,rowH,hd,th.textDim,0);
                if (hv && cx.in.pressed[0] && !addMenuOpen && !ctxOpen) collapsed = !collapsed;
            }
            ry+=rowH;
        };
        if (nItems) {
            sectionHeader("SCENE ITEMS", nItemsShown, itemsCollapsed);
            if (!itemsCollapsed) for (int i=0;i<nItems;++i) { if (!itemMatches(i)) continue;
                if (!onScreen(ry)) { ry+=rowH; continue; }
                auto& it=items[i];
                bool inMulti=false; for (int s:selItems) if (s==i) inMulti=true;
                bool seld=(i==selItem)||inMulti;
                if (seld) dl.rect(x,ry,w,rowH,th.rowSel); else if (cx.hover(x,ry,w,rowH)) dl.rect(x,ry,w,rowH,th.rowHover);
                // VISIBILITY EYE (same as the mesh rows) — hides this item's marker + gizmo.
                bool vis=!it.hidden; bool eyeClicked = eyeToggle(x+12*uiScale, ry+rowH*0.5f, vis); if (eyeClicked) it.hidden=!vis;
                char lbl[180]; snprintf(lbl,sizeof lbl,"%s   %s", it.name.c_str(), sitem::typeName(it.type));
                cx.textAligned(x+28*uiScale,ry,w-48*uiScale,rowH,lbl, it.hidden?th.textDim:(seld?th.textSel:th.text), 0);
                float ex=x+w-15*uiScale; bool dh=cx.hover(ex-3*uiScale,ry,18*uiScale,rowH);
                cx.textAligned(ex,ry,14*uiScale,rowH,"x", dh?ui::rgba(255,120,120):th.textDim, 0);
                // RIGHT-CLICK -> the scene-item context menu (focus/hide/duplicate/REMOVE — acts on the whole multi-selection)
                if (!addMenuOpen && !ctxOpen && cx.hover(x,ry,w,rowH) && cx.in.pressed[1]) {
                    if (!inMulti) { selItems.assign(1,i); selItem=i; deselectAll(); }
                    ctxItem=i; ctxMesh=-1; ctxOpen=true; ctxSub=-1; ctxX=cx.in.mx; ctxY=cx.in.my;
                }
                if (!addMenuOpen && !ctxOpen && !eyeClicked && cx.hover(x,ry,w,rowH) && cx.in.pressed[0]) { if (dh) { pushItemUndo(items); items.erase(items.begin()+i); selItem=-1; selItems.clear(); dl.popClip(); return; }
                    bool dbl = (selItem==i) && (cx.t - lastItemClickT < 0.35f);
                    if (cx.in.shift||cx.in.ctrl) {   // MULTI-select scene items (Ctrl/Shift+click, like mesh rows)
                        bool was=false; for (size_t k=0;k<selItems.size();++k) if (selItems[k]==i){ selItems.erase(selItems.begin()+k); was=true; break; }
                        if (!was) selItems.push_back(i);
                        selItem = selItems.empty()?-1:selItems.back(); deselectAll();
                    } else { selItems.assign(1,i); selItem=i; deselectAll(); }
                    lastItemClickT=cx.t;
                    if (dbl) focusItem(i); }   // SELECT only (no teleport); DOUBLE-click (or the Focus button) frames it
                ry+=rowH;
            }
        }
        sectionHeader("MESHES", nMeshShown, meshesCollapsed);
        if (!meshesCollapsed) for (int i=0;i<nMesh;i++) {
            // FILTER FIRST: a non-matching row takes NO space (so matches pack from the top and the scroll extent above
            // matches the visible list). DELETED meshes take no row at all - they're gone (Ctrl+Z restores).
            if (!meshRowVisible(i)) continue;
            if (!onScreen(ry)) { ry+=rowH; continue; }
            auto& gm = r->gpuMeshes[i];
            bool vis = !r->isHidden(i);
            auto rr = cx.treeRow(ui::hashId(3000+i,11), x, ry, w, rowH, (gm.name+"  ["+std::to_string(i)+"]"+(isSkyboxMesh(i)?"  *SKY*":"")).c_str(), 0, false, false, inSel(i), vis);
            if (addMenuOpen || ctxOpen) rr = ui::Context::RowResult{};   // an open menu overlays the list -> ignore row clicks under it
            if (rr.clicked) { selItem=-1; if (cx.in.shift||cx.in.ctrl) toggleSel(i); else selectOne(i); }   // Ctrl/Shift = add to multi-selection
            if (rr.toggledVis) r->setHidden(i, vis);
            if (rr.clicked && cx.in.pressed[0] && (cx.t - lastClickT < 0.3f) && lastClickIdx==i) focusMesh(gm);
            if (rr.clicked) { lastClickT = cx.t; lastClickIdx = i; }
            // RIGHT-CLICK on the row -> the SAME mesh context menu as the viewport (hide/duplicate/delete/colliders/...)
            if (!addMenuOpen && !ctxOpen && cx.hover(x,ry,w,rowH) && cx.in.pressed[1]) {
                if (!inSel(i)) selectOne(i);
                selItem=-1; ctxMesh=i; ctxItem=-1; ctxOpen=true; ctxSub=-1; ctxX=cx.in.mx; ctxY=cx.in.my;
            }
            // DRAG-PAINT multi-select: press a row, then drag over more rows (mouse held) to add them to the selection
            if (!ctxOpen && cx.in.pressed[0] && cx.hover(x,ry,w,rowH)) outlinerDragRow=i;
            else if (!ctxOpen && cx.in.down[0] && outlinerDragRow>=0 && i!=outlinerDragRow && cx.hover(x,ry,w,rowH) && !inSel(i)) { sel.push_back(i); selected=i; r->selectedMesh=i; selItem=-1; }
            ry += rowH;   // advance ONLY for shown (matching, on-screen) rows
        }
        if (!cx.in.down[0]) outlinerDragRow=-1;   // end the drag-paint when the button is released
        dl.popClip();
    }
    // Floating "Add Component" menu — each entry shows the FRIENDLY name + the REAL Meta component class; click = spawn it.
    void drawAddMenu() {
        if (!addMenuOpen) return;
        auto& th=cx.th; int types[]={sitem::SPAWN,sitem::CHAIR,sitem::BOXCOL,sitem::NAVMESH,sitem::WALLPLACE,sitem::HOTSPOT,sitem::BOUNDARY}; int n=7;
        float rh=(th.rowH+8*uiScale), w=300*uiScale, hh=n*rh+8*uiScale;
        float x=addMenuX, y=addMenuY; if (x+w>fbW) x=fbW-w-2; if (y+hh>fbH) y=fbH-hh-2;
        ctxRX=x; ctxRY=y; ctxRW=w; ctxRH=hh;   // route clicks (reuse the ctx-menu capture path)
        dl.rect(x,y,w,hh,th.panelBg); dl.border(x,y,w,hh,th.border);
        dl.pushClip(x,y,w,hh); float ry=y+4*uiScale;
        for (int i=0;i<n;++i, ry+=rh) {
            bool hv=cx.hover(x,ry,w,rh); if (hv) dl.rect(x+1,ry,w-2,rh,th.accent);
            cx.textAligned(x+12*uiScale,ry+2*uiScale,w-16*uiScale,th.rowH, sitem::typeName(types[i]), hv?th.textSel:th.text, 0);
            cx.textAligned(x+12*uiScale,ry+th.rowH-1*uiScale,w-16*uiScale,th.rowH-2*uiScale, sitem::metaName(types[i]), hv?ui::withA(th.textSel,180):th.textDim, 0, mono.ok?&mono:&font);
            if (hv && cx.in.pressed[0]) { addItem(types[i]); addMenuOpen=false; }
        }
        dl.popClip();
    }
    float lastClickT = -1.f; int lastClickIdx = -1;

    // labelled row of N drag fields over a plain float[] (scene items)
    bool vecRowF(const char* lbl, float* v, int n, float speed, float x, float& y, float w){
        auto& th=cx.th; float rh=th.rowH, lw=78*uiScale, fw=(w-lw)/n - 2*uiScale; bool ch=false;
        cx.label(x,y,lw,rh,lbl,th.textDim);
        for (int k=0;k<n;k++) if (cx.dragFloat(ui::hashId(6000+k, ui::hashId(lbl)), x+lw+k*(fw+2*uiScale), y, fw, rh, v[k], speed)) ch=true;
        y+=rh+2*uiScale; return ch;
    }
    // Properties for the selected scene item: name + Meta class + transform + the type-specific fields + Delete.
    void drawItemProps(float x, float y, float w){
        auto& th=cx.th; auto& it=items[selItem]; float rh=th.rowH;
        if (it.type != sitem::CHAIR) editExit=false;   // exit-gizmo mode only applies to chairs
        cx.label(x,y,42*uiScale,rh,"Name",th.textDim);
        cx.textField(ui::hashId(8200u+(unsigned)selItem, 9u), x+44*uiScale, y, w-44*uiScale, rh, it.name); y+=rh;   // rename the item
        cx.label(x,y,w,rh, sitem::metaName(it.type), th.textDim); y+=rh+4*uiScale;
        vecRowF("Position", it.pos, 3, 0.01f, x, y, w);
        vecRowF("Rotation", it.rot, 3, 0.5f, x, y, w);     // euler degrees (the Move/Rotate/Scale gizmo edits these)
        { float qd[4]; eulerToQuat(it.rot,qd); float rh2=th.rowH, lw=70*uiScale, fw=(w-lw)/4 - 2*uiScale; bool ch=false;   // EDITABLE quaternion X Y Z W (synced to euler)
          cx.label(x,y,lw,rh2,"Quat",th.textDim);
          for (int k=0;k<4;k++) if (cx.dragFloat(ui::hashId(6200u+(unsigned)k,7u), x+lw+k*(fw+2*uiScale), y, fw, rh2, qd[k], 0.01f)) ch=true;
          if (ch){ normalizeQuat(qd); quatToEuler(qd,it.rot); } y+=rh2+2*uiScale; }
        vecRowF("Scale", it.scale, 3, 0.01f, x, y, w);
        // UNIFORM scale: drag ONE field to shrink/grow all three axes at once (keeps the X:Y:Z ratio).
        { float rh2=th.rowH, lw=78*uiScale, fw=w-lw-2*uiScale;
          cx.label(x,y,lw,rh2,"Scale (all)",th.textDim);
          float base=(it.scale[0]+it.scale[1]+it.scale[2])/3.f, uni=base;
          if (cx.dragFloat(ui::hashId(6400u+(unsigned)selItem,7u), x+lw, y, fw, rh2, uni, 0.01f) && base>1e-6f){
              float f=uni/base; if (f>1e-4f) for(int k=0;k<3;k++) it.scale[k]*=f; }
          y+=rh2+2*uiScale; }
        // Quick "zero all rotations": reset this item's euler (X/Y/Z) to 0 in one click.
        if (cx.button(ui::hashId(8300u+(unsigned)selItem, 7u), x, y, 120*uiScale, rh, "Zero rotation")) { pushItemUndo(items); it.rot[0]=it.rot[1]=it.rot[2]=0.f; }
        y+=rh+4*uiScale;
        switch (it.type){
          case sitem::SPAWN: { cx.checkbox(ui::hashId("spstart"), x, y, "allowStart (this is a player start)", it.allowStart); y+=rh;
                               cx.checkbox(ui::hashId("splocal"), x, y, "local (else: remote players)", it.isLocal); y+=rh; break; }
          case sitem::CHAIR: {
                               cx.checkbox(ui::hashId("chtilt"), x, y, "Tilt lock (stay upright - yaw only)", chairTiltLock); y+=rh+2*uiScale;
                               if (chairTiltLock && (it.rot[0]!=0.f||it.rot[2]!=0.f)) { it.rot[0]=0.f; it.rot[2]=0.f; }   // snap upright while locked
                               vecRowF("Exit pos", it.exitPos, 3, 0.01f, x, y, w); cx.label(x,y,w,rh,"(where the avatar stands up)",th.textDim); y+=rh;
                               if (cx.button(ui::hashId("exitgiz"), x, y, w, rh, editExit?"Editing EXIT with gizmo (click to stop)":"Move exit point with the GIZMO", editExit)) editExit=!editExit;
                               y+=rh+2*uiScale;
                               if (cx.button(ui::hashId("exitcam"), x, y, w, rh, "Set exit point to camera position")) {
                                   it.exitPos[0]=r->cam.pos[0]-it.pos[0]; it.exitPos[1]=(r->cam.pos[1]-1.6f)-it.pos[1]; it.exitPos[2]=r->cam.pos[2]-it.pos[2]; }
                               y+=rh+2*uiScale;
                               // the shell chair button rides the ChairIcon child: height digs it out of buried
                               // geometry, scale fixes the "tiny dot" (haven chairs ride transform scale ~2.45)
                               cx.label(x,y,92*uiScale,rh,"Icon height",th.textDim); cx.dragFloat(ui::hashId("chicy"),x+94*uiScale,y,w-94*uiScale,rh,it.iconY,0.01f); y+=rh+2*uiScale;
                               cx.label(x,y,92*uiScale,rh,"Icon scale",th.textDim);  cx.dragFloat(ui::hashId("chics"),x+94*uiScale,y,w-94*uiScale,rh,it.iconScale,0.01f); y+=rh+2*uiScale;
                               break; }
          case sitem::BOXCOL: { vecRowF("Half size", it.half, 3, 0.01f, x, y, w); cx.label(x,y,w,rh,"(invisible wall / path blocker)",th.textDim); y+=rh; break; }
          case sitem::WALLPLACE: { cx.label(x,y,78*uiScale,rh,"Max W",th.textDim); cx.dragFloat(ui::hashId("wpw"),x+80*uiScale,y,w-80*uiScale,rh,it.propW,0.01f); y+=rh+2*uiScale;
                                   cx.label(x,y,78*uiScale,rh,"Max H",th.textDim); cx.dragFloat(ui::hashId("wph"),x+80*uiScale,y,w-80*uiScale,rh,it.propH,0.01f); y+=rh+2*uiScale;
                                   // FACING: the props hang on the side the viewport arrow points at (the entity's forward).
                                   // The auto-fit guesses that side from where the camera stood at creation — when it guesses
                                   // wrong ("loads on the wrong side") this flips it without hand-editing rotation numbers.
                                   if (cx.button(ui::hashId("wpflip"), x, y, w, rh, "Flip facing (props on the OTHER side)")) {
                                       pushItemUndo(items);
                                       it.rot[1] += 180.f; while (it.rot[1] > 180.f) it.rot[1] -= 360.f; while (it.rot[1] < -180.f) it.rot[1] += 360.f;
                                       setStatus("Wall placement flipped - the viewport arrow shows the props side");
                                   } y+=rh;
                                   cx.label(x,y,w,rh,"(arrow in the viewport = props side)",th.textDim); y+=rh; break; }
          case sitem::NAVMESH: {
                cx.label(x,y,w,rh,"Build mode",th.textDim); y+=rh;
                const char* modes[3]={"Flat","Smart","Selection"}; float bw=(w-4*uiScale)/3;
                for (int mm=0;mm<3;mm++){ if (cx.button(ui::hashId(7700u+mm,3u), x+mm*(bw+2*uiScale), y, bw, rh, modes[mm], it.navMode==mm)) { it.navMode=mm; bakeNavGeometry(it); } }
                y+=rh+4*uiScale;
                char b[110]; snprintf(b,sizeof b,"%s  -  %d tris / %d verts", it.navMode==0?"flat ground plane":it.navMode==1?"smart: walkable faces":"from selected meshes", (int)(it.navIdx.size()/3), (int)(it.navVerts.size()/3));
                cx.label(x,y,w,rh,b,th.textDim); y+=rh;
                if (it.navMode==2 && cx.button(ui::hashId("nvre"), x, y, 180*uiScale, rh, "Use current selection")) { it.srcMeshes=sel; bakeNavGeometry(it); }
                else if (it.navMode!=2 && cx.button(ui::hashId("nvrb"), x, y, 180*uiScale, rh, "Rebuild preview")) bakeNavGeometry(it);
                y+=rh+2*uiScale;
                if (cx.button(ui::hashId("navsolo"), x, y, w, rh, r->hideAllGeom?"Show meshes again":"Solo view (hide all meshes)", r->hideAllGeom)) { r->hideAllGeom=!r->hideAllGeom; if(r->hideAllGeom) focusItem(selItem); }
                y+=rh+2*uiScale;
                if (cx.button(ui::hashId("navslope"),  x, y, w, rh, "Slope colors (flat green -> steep red)", navColorBySlope)) navColorBySlope=!navColorBySlope;
                y+=rh+2*uiScale;
                if (cx.button(ui::hashId("navsmooth"), x, y, w, rh, "Smooth collision (averaged normals)", navSmooth)) navSmooth=!navSmooth;
                y+=rh+2*uiScale;
                if (cx.button(ui::hashId("navdbgcl"),  x, y, w, rh, "Render navmesh on device (debug clone)", navDebugClone)) navDebugClone=!navDebugClone;
                y+=rh+2*uiScale;
                cx.label(x,y,w,rh*0.9f,"(toggle the Navmesh eye to view it)",th.textDim); y+=rh; break; }
          case sitem::HOTSPOT: break;
          case sitem::BOUNDARY: { cx.label(x,y,w,rh,"Plane normal (UnitAxis)",th.textDim); y+=rh;
                                  if (cx.button(ui::hashId("bdax"), x, y, 160*uiScale, rh, sitem::unitAxisName(it.axis))) it.axis=(it.axis+1)%6;
                                  y+=rh+2*uiScale; cx.label(x,y,w,rh*0.9f,"(PositiveY = floor; Position.Y = kill height)",th.textDim); y+=rh; break; }
        }
        y+=8*uiScale;
        // QUICK PLACE: drop the item where the camera stands/looks (foot level + camera facing). Great for spawns/chairs.
        if (cx.button(ui::hashId("itcam"), x, y, w, rh, "Move here (camera position + rotation)")) {
            it.pos[0]=r->cam.pos[0]; it.pos[1]=r->cam.pos[1]-1.6f; it.pos[2]=r->cam.pos[2]; cameraEuler(it.rot);   // full yaw+pitch
        }
        y+=rh+6*uiScale;
        if (cx.button(ui::hashId("itfocus"), x, y, 80*uiScale, rh, "Focus")) focusItem(selItem);
        if (cx.button(ui::hashId("itdel"), x+86*uiScale, y, 90*uiScale, rh, "Delete")) deleteSelItem();
    }
    void focusOnPoint(const float p[3]){ Camera& c=r->cam; c.pos[0]=p[0]; c.pos[1]=p[1]+1.0f; c.pos[2]=p[2]+3.0f; float dy=p[1]-c.pos[1],dz=p[2]-c.pos[2],L=std::sqrt(dy*dy+dz*dz); if(L<1e-4f)L=1; c.yaw=0; c.pitch=std::asin(dy/L); }
    // FULL camera orientation (yaw + pitch, no roll) as the item's euler — so "place as camera" matches where you LOOK,
    // not just a flat yaw. Builds a -Z-forward look-rotation toward the camera forward, then converts to euler degrees.
    void cameraEuler(float e[3]){
        float cp=std::cos(r->cam.pitch);
        float F[3]={ std::sin(r->cam.yaw)*cp, std::sin(r->cam.pitch), -std::cos(r->cam.yaw)*cp };
        float fl=std::sqrt(F[0]*F[0]+F[1]*F[1]+F[2]*F[2]); if(fl<1e-6f){ e[0]=e[1]=e[2]=0; return; } for(int k=0;k<3;k++) F[k]/=fl;
        float z[3]={-F[0],-F[1],-F[2]};                                   // local +Z (forward = -Z)
        float up[3]={0,1,0};
        float x[3]={ up[1]*z[2]-up[2]*z[1], up[2]*z[0]-up[0]*z[2], up[0]*z[1]-up[1]*z[0] };
        float xl=std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);
        if(xl<1e-4f){ up[0]=0;up[1]=0;up[2]=1; x[0]=up[1]*z[2]-up[2]*z[1]; x[1]=up[2]*z[0]-up[0]*z[2]; x[2]=up[0]*z[1]-up[1]*z[0]; xl=std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); }
        for(int k=0;k<3;k++) x[k]/=xl;
        float yv[3]={ z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0] };
        float m00=x[0],m10=x[1],m20=x[2], m01=yv[0],m11=yv[1],m21=yv[2], m02=z[0],m12=z[1],m22=z[2];
        float tr=m00+m11+m22, q[4];
        if(tr>0){ float s=std::sqrt(tr+1.f)*2.f; q[3]=0.25f*s; q[0]=(m21-m12)/s; q[1]=(m02-m20)/s; q[2]=(m10-m01)/s; }
        else if(m00>m11&&m00>m22){ float s=std::sqrt(1.f+m00-m11-m22)*2.f; q[3]=(m21-m12)/s; q[0]=0.25f*s; q[1]=(m01+m10)/s; q[2]=(m02+m20)/s; }
        else if(m11>m22){ float s=std::sqrt(1.f+m11-m00-m22)*2.f; q[3]=(m02-m20)/s; q[0]=(m01+m10)/s; q[1]=0.25f*s; q[2]=(m12+m21)/s; }
        else { float s=std::sqrt(1.f+m22-m00-m11)*2.f; q[3]=(m10-m01)/s; q[0]=(m02+m20)/s; q[1]=(m12+m21)/s; q[2]=0.25f*s; }
        normalizeQuat(q); quatToEuler(q,e);
    }
    // The world point a scene-item's marker/label/pick/focus uses. A NAVMESH item lives at origin (its verts are
    // world-space), so use its baked geometry's AABB CENTRE instead — otherwise the marker/focus point at (0,0,0)
    // far from the actual navmesh (the Snake Way "out of bounds" marker).
    // the item's transform matrix (pos + euler rot + scale) — same T·R·S the cook's transformComp applies on device
    void itemTRS(const sitem::Item& it, float m[16]){ float q[4]; eulerToQuat(it.rot,q); buildTRS(it.pos, q, it.scale, m); }
    void itemMarkerPos(const sitem::Item& it, float out[3]){
        if (it.type==sitem::NAVMESH && it.navVerts.size()>=3){
            float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
            for(size_t v=0;v+2<it.navVerts.size();v+=3) for(int k=0;k<3;k++){ float p=it.navVerts[v+k]; if(p<mn[k])mn[k]=p; if(p>mx[k])mx[k]=p; }
            float c[3]={0.5f*(mn[0]+mx[0]),0.5f*(mn[1]+mx[1]),0.5f*(mn[2]+mx[2])}, M[16]; itemTRS(it,M); xformPoint(M,c,out); return;   // transformed centre = where the gizmo sits
        }
        out[0]=it.pos[0]; out[1]=it.pos[1]; out[2]=it.pos[2];
    }
    void focusItem(int i){ if(i<0||i>=(int)items.size())return; float p[3]; itemMarkerPos(items[i],p); focusOnPoint(p); }

    // Blender-style visibility EYE toggle: open almond+pupil = shown, eyelid line = hidden.
    bool eyeToggle(float ex, float ey, bool& v){
        float r=6*uiScale; bool hv=cx.hover(ex-r-3*uiScale, ey-r-3*uiScale, (r+3)*2, (r+3)*2);
        uint32_t c = v ? cx.th.text : cx.th.textDim;
        if (v){ float px=ex-r,pu=ey,pl=ey;
            for (int s=1;s<=10;s++){ float t=s/10.f, ax=ex-r+2*r*t, dy=r*0.6f*sinf(3.14159265f*t); dl.line(px,pu,ax,ey-dy,c,1.3f); dl.line(px,pl,ax,ey+dy,c,1.3f); px=ax;pu=ey-dy;pl=ey+dy; }
            dl.rect(ex-2*uiScale,ey-2*uiScale,4*uiScale,4*uiScale,c);
        } else dl.line(ex-r,ey,ex+r,ey,c,1.6f);
        if (hv && cx.in.pressed[0]){ v=!v; return true; }
        return false;
    }
    // The Meta-Component manager: per haven component type — colour swatch, EYE visibility toggle, count, + Add, Meta class.
    void drawScenePanel(float x, float y, float w){
        auto& th=cx.th; float rh=th.rowH;
        // ── BACKGROUND COLOR ("the dark one"): recolor the DARK void behind everything (r->clearRGB). Live +
        //    persisted. This is the pure viewport background — distinct from the "Skybox / Background" spawner
        //    below (which builds a sky sphere / cooks a skybox). Drag R/G/B or pick a preset. ──
        cx.label(x,y,w,rh,"Background color  (the dark void)",th.text); y+=rh+2*uiScale;
        { float y0=y; float* bg = r ? r->clearRGB : bgColor;
          cx.swatch(ui::hashId("bgsw"), x, y+2*uiScale, 40*uiScale, rh-4*uiScale, bg[0],bg[1],bg[2]);
          char cc[40]; snprintf(cc,sizeof cc,"%.2f, %.2f, %.2f", bg[0],bg[1],bg[2]);
          cx.textAligned(x+46*uiScale, y, w-46*uiScale, rh, cc, th.textDim, 0);
          cx.tip(x,y0,w,rh,"The dark background behind the whole scene (the viewport clear color).\nDrag R/G/B or click a preset to recolor it live. Persisted in the session."); y+=rh+2*uiScale;
          const char* chn[3]={"R","G","B"}; const uint32_t chc[3]={ui::rgba(220,70,70),ui::rgba(70,200,90),ui::rgba(80,140,235)};
          for (int ci=0; ci<3; ci++){
              cx.label(x+8*uiScale,y,18*uiScale,rh,chn[ci],th.textDim);
              float sv=bg[ci];
              if (cx.slider(ui::hashId(6500u+ci,11u), x+30*uiScale, y, w-90*uiScale, rh, sv, 0.f,1.f, chc[ci])) { if(r) r->clearRGB[ci]=sv; bgColor[ci]=sv; bgColorSet=true; }
              char vb[16]; snprintf(vb,sizeof vb,"%.2f",bg[ci]); cx.textAligned(x+w-56*uiScale,y,52*uiScale,rh,vb,th.text,2);
              y+=rh+2*uiScale;
          }
          struct BP{ const char* n; float r,g,b; };
          static const BP bpre[]={{"Black",0,0,0},{"White",1,1,1},{"Grey",0.5f,0.5f,0.5f},{"Sky",0.35f,0.55f,0.9f},{"Night",0.02f,0.03f,0.06f},{"Sunset",0.95f,0.5f,0.25f}};
          float bw=(w-5*4*uiScale)/6.f;
          for (int p=0;p<6;p++){ float bx=x+p*(bw+4*uiScale);
            if (cx.button(ui::hashId(6600u+p,11u), bx, y, bw, rh, bpre[p].n)) { for(int k=0;k<3;k++){ if(r) r->clearRGB[k]=(&bpre[p].r)[k]; bgColor[k]=(&bpre[p].r)[k]; } bgColorSet=true; } }
          y+=rh+6*uiScale; }
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        // ── PREFABS: reusable assets - save a selection (right-click > Save as PREFAB), spawn copies
        //    here or by DRAGGING the .hsrprefab file into the window. Spawns land where you look. ──
        cx.label(x,y,w,rh,"Prefabs",th.text); y+=rh+2*uiScale;
        { double nowT = glfwGetTime();
          if (prefabList.empty() && nowT-prefabListAt>3.0) { refreshPrefabList(); prefabListAt=nowT; }
          // the make-a-prefab entry point, front and center (also in right-click > Save as PREFAB)
          if (!sel.empty()) { if (cx.button(ui::hashId("pfsave"), x, y, w, rh, ("SAVE selection as prefab ("+std::to_string(sel.size())+" mesh)").c_str())) { savePrefabFromSelection(); refreshPrefabList(); } }
          else cx.label(x,y,w,rh,"(select mesh(es) -> save them as a prefab)",th.textDim);
          y+=rh+4*uiScale;
          if (cx.button(ui::hashId("pfrefresh"), x, y, 90*uiScale, rh, "Refresh")) refreshPrefabList();
          char pc[40]; snprintf(pc,sizeof pc,"%d prefab(s)",(int)prefabList.size());
          cx.label(x+96*uiScale,y,w-96*uiScale,rh,pc,th.textDim); y+=rh+2*uiScale;
          int shown=0;
          for (auto& pf : prefabList){ if (shown++>=8){ cx.label(x,y,w,rh*0.9f,"(more in prefabs/ - drag any in)",th.textDim); y+=rh*0.9f; break; }
              std::string bn=pf; size_t sl2=bn.find_last_of("/\\"); if (sl2!=std::string::npos) bn=bn.substr(sl2+1);
              if (bn.size()>28) bn=bn.substr(0,25)+"...";
              if (cx.button(ui::hashId(9700u+(unsigned)shown,23u), x, y, w-64*uiScale, rh, bn.c_str())) spawnPrefab(pf);
              cx.textAligned(x+w-60*uiScale,y,58*uiScale,rh,"spawn",th.textDim,0);
              y+=rh+2*uiScale; }
          cx.label(x,y,w,rh*0.9f,"(drop a .hsrprefab file into the window = spawn)",th.textDim); y+=rh+4*uiScale; }
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        // ── GIZMOS / OVERLAYS — single home for the viewport overlay toggles (they used to sit in the
        //    viewport header AND partly here = the confusing duplicate row; now this tab owns them). ──
        cx.label(x,y,w,rh,"Gizmos / overlays",th.text); y+=rh+2*uiScale;
        cx.checkbox(ui::hashId("gzNav"),  x, y, "Navmesh (walkable preview)", r->showNavmesh); y+=rh;
        cx.checkbox(ui::hashId("gzCol"),  x, y, "Colliders / walls", r->showCollision); y+=rh;
        cx.checkbox(ui::hashId("gzSpn"),  x, y, "Spawn markers", r->showSpawn); y+=rh;
        cx.checkbox(ui::hashId("gzFar"),  x, y, "Far-clip dome (device 5000m)", showFarClip); y+=rh;
        cx.checkbox(ui::hashId("gzMCol"), x, y, "Mesh collision (exact tris)", showMeshCol); y+=rh;
        { bool ae=animEyeOn;
          cx.checkbox(ui::hashId("gzAnim"), x, y, "Hide ANIMATED meshes (Ctrl+H)", ae);
          if (ae!=animEyeOn) toggleAnimatedEye(); }
        y+=rh+6*uiScale;
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        // ── PLAYER (QUEST) — live-track the headset player + drag its marker to teleport it on-device. Turn
        //    "No gravity" ON so the player HOLDS the dragged height instead of falling + resetting to spawn
        //    (drives the AIO bridge: nogravity / warp / rot). Needs the patched vrshell + adb forward. ──
        cx.label(x,y,w,rh,"Player (Quest)",th.text); y+=rh+2*uiScale;
        { bool tp0=trackPlayer;
          cx.checkbox(ui::hashId("plTrack"), x, y, "Track player  (live marker + drag)", trackPlayer); y+=rh;
          if (trackPlayer!=tp0) { if (trackPlayer) startPlayerChannel(); else stopPlayerChannel(); }
          bool ng0=playerNoGrav;
          cx.checkbox(ui::hashId("plNoGrav"), x, y, "No gravity  (hold height, no fall/reset)", playerNoGrav); y+=rh;
          if (playerNoGrav!=ng0) sendPlayerNoGravity(playerNoGrav);   // ensures the channel + adb-forward on its own
          char pb[110];
          snprintf(pb,sizeof pb,"pos %.2f, %.2f, %.2f", questPlayerPos[0],questPlayerPos[1],questPlayerPos[2]);
          cx.label(x,y,w,rh*0.9f,pb,th.textDim); y+=rh*0.9f;
          snprintf(pb,sizeof pb,"looking %.0f deg   device no-gravity: %s",
                   (questPlayerFaceOk?questPlayerFace:questPlayerYaw)*57.2958f,
                   questPlayerDev<0?"?":(questPlayerDev?"ON":"off"));
          cx.label(x,y,w,rh*0.9f,pb,th.textDim); y+=rh+2*uiScale;
          float bw2=(w-8*uiScale)/2;
          if (cx.button(ui::hashId("plToCam"), x, y, bw2, rh, "Teleport to camera", true)) {
              trackPlayer=true;
              questPlayerPos[0]=r->cam.pos[0]; questPlayerPos[1]=r->cam.pos[1]-1.6f; questPlayerPos[2]=r->cam.pos[2];   // feet ~1.6m below the eye
              questPlayerYaw=r->cam.yaw;
              sendPlayerWarp(questPlayerPos[0],questPlayerPos[1],questPlayerPos[2]); }
          if (cx.button(ui::hashId("plReset"), x+bw2+6*uiScale, y, bw2, rh, "Warp to spawn")) { trackPlayer=true; questPlayerYaw=0.f; sendPlayerWarp(0,0,0); }
          y+=rh+6*uiScale;
        }
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        // ── LIGHTING — global light manipulation (multiplies every draw's color; live, persisted, ships in session) ──
        cx.label(x,y,w,rh,"Lighting  (global, live)",th.text); y+=rh+2*uiScale;
        { float lw=26*uiScale, fw=(w-lw*4)/4 - 2*uiScale; const char* nm[4]={"R","G","B","A"};
          for (int k=0;k<4;k++){ float fx=x+k*(lw+fw+2*uiScale);
              cx.label(fx,y,lw,rh,nm[k],th.textDim);
              cx.dragFloat(ui::hashId(9200u+(unsigned)k,17u), fx+lw, y, fw, rh, r->lightMul[k], 0.005f, "%.2f"); }
          y+=rh+2*uiScale;
          float bw2=(w-8*uiScale)/2;
          if (cx.button(ui::hashId("litreset"), x, y, bw2, rh, "Reset lighting")) { for(int k=0;k<4;k++) r->lightMul[k]=1.f; }
          if (cx.button(ui::hashId("litnight"), x+bw2+6*uiScale, y, bw2, rh, "Night preview")) { r->lightMul[0]=0.35f; r->lightMul[1]=0.40f; r->lightMul[2]=0.55f; r->lightMul[3]=1.f; }
          y+=rh+8*uiScale; }
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        cx.label(x,y,w,rh,"Meta Components",th.text); y+=rh;
        cx.label(x,y,w,rh*0.9f,"eye = show markers     Clear = remove all of a type     + Add = spawn one",th.textDim); y+=rh+4*uiScale;
        int types[]={sitem::SPAWN,sitem::CHAIR,sitem::BOXCOL,sitem::NAVMESH,sitem::WALLPLACE,sitem::HOTSPOT,sitem::BOUNDARY};
        for (int t : types){
            int cnt=0; for (auto& it:items) if (it.type==t) cnt++;
            dl.rect(x, y+3*uiScale, 10*uiScale, rh-6*uiScale, typeColor(t,true));               // colour key
            eyeToggle(x+26*uiScale, y+rh*0.5f, showType[t]);                                    // EYE visibility toggle
            char lbl[80]; snprintf(lbl,sizeof lbl,"%s  (%d)", sitem::typeName(t), cnt);
            cx.label(x+40*uiScale, y, w-158*uiScale, rh, lbl, th.text);
            if (cnt>0 && cx.button(ui::hashId(7200u+t, 9u), x+w-110*uiScale, y+1*uiScale, 52*uiScale, rh-2*uiScale, "Clear", false)){
                pushItemUndo(items);                                                            // one Ctrl+Z restores the whole type
                for (int i2=(int)items.size()-1;i2>=0;--i2) if (items[(size_t)i2].type==t) items.erase(items.begin()+i2);
                selItem=-1; selItems.clear();
                setStatus(std::string("Cleared ")+std::to_string(cnt)+" "+sitem::typeName(t)+" item(s) - Ctrl+Z restores");
            }
            if (cx.button(ui::hashId(7100u+t, 7u), x+w-54*uiScale, y+1*uiScale, 52*uiScale, rh-2*uiScale, "+ Add", true)) addItem(t);
            y+=rh+1*uiScale;
            cx.textAligned(x+40*uiScale, y, w-44*uiScale, rh*0.85f, sitem::metaName(t), th.textDim, 0, mono.ok?&mono:&font); y+=rh*0.85f+6*uiScale;
        }
        // ── one-click NUKE: wipe every component/scene item in the env (undoable) ──
        { int total=(int)items.size();
          char cb3[48]; snprintf(cb3,sizeof cb3,"CLEAR ALL scene items (%d)",total);
          if (cx.button(ui::hashId("clearitems"), x, y, w, rh, cb3, false) && total>0){
              pushItemUndo(items);
              items.clear(); selItem=-1; selItems.clear();
              setStatus("Cleared ALL "+std::to_string(total)+" scene item(s)/component(s) - ONE Ctrl+Z restores everything");
          }
          y+=rh+4*uiScale; }
        // ── SKYBOX / BACKGROUND — the UI for the (long-existing) skybox backend: solid color, equirect image,
        //    or any mesh's texture. Previews live (clearRGB / sky sphere) AND cooks as the SkyboxPlatformComponent. ──
        y+=6*uiScale; dl.rect(x,y,w,1,th.splitLine); y+=8*uiScale;
        cx.label(x,y,w,rh,"Skybox / Background",th.text); y+=rh+2*uiScale;
        float y0;
        // ── COLOR PICKER: live swatch + R/G/B sliders + presets. Editing ANY slider applies immediately
        //    (setSkyColor drives the viewport clearRGB live + persists); the swatch previews the current pick.
        //    skyPick holds the working color so the sliders have something to drag even before it's "set".
        if (skyColorSet()) { skyPick[0]=skyColor[0]; skyPick[1]=skyColor[1]; skyPick[2]=skyColor[2]; }
        y0=y; cx.label(x,y,86*uiScale,rh,"Color",th.textDim);
        cx.swatch(ui::hashId("skysw"), x+88*uiScale, y+2*uiScale, 40*uiScale, rh-4*uiScale, skyPick[0],skyPick[1],skyPick[2]);
        { char cc[40]; snprintf(cc,sizeof cc,"%.2f, %.2f, %.2f", skyPick[0],skyPick[1],skyPick[2]);
          cx.textAligned(x+134*uiScale, y, w-134*uiScale, rh, skyColorSet()?cc:"(env's own sky)", th.textDim, 0); }
        cx.tip(x,y0,w,rh,"Solid background/skybox color. Drag the R/G/B sliders (or a preset)\nto set it - previews live in the viewport AND cooks as a\nSkyboxPlatformComponent solid-color texture. Env meshes untouched."); y+=rh+2*uiScale;
        const char* chn[3]={"R","G","B"}; const uint32_t chc[3]={ui::rgba(220,70,70),ui::rgba(70,200,90),ui::rgba(80,140,235)};
        for (int ci=0; ci<3; ci++){
            cx.label(x+8*uiScale,y,18*uiScale,rh,chn[ci],th.textDim);
            float sv=skyPick[ci];
            if (cx.slider(ui::hashId(6300u+ci,7u), x+30*uiScale, y, w-90*uiScale, rh, sv, 0.f,1.f, chc[ci])) { skyPick[ci]=sv; setSkyColor(skyPick[0],skyPick[1],skyPick[2]); }
            char vb[16]; snprintf(vb,sizeof vb,"%.2f",skyPick[ci]); cx.textAligned(x+w-56*uiScale,y,52*uiScale,rh,vb,th.text,2);
            y+=rh+2*uiScale;
        }
        // preset swatches (one click applies) + Clear
        struct SP{ const char* n; float r,g,b; };
        static const SP presets[]={{"Black",0,0,0},{"White",1,1,1},{"Grey",0.5f,0.5f,0.5f},{"Sky",0.35f,0.55f,0.9f},{"Night",0.02f,0.03f,0.06f},{"Sunset",0.95f,0.5f,0.25f}};
        { float bw=(w-5*4*uiScale)/6.f; y0=y;
          for (int p=0;p<6;p++){ float bx=x+p*(bw+4*uiScale);
            if (cx.button(ui::hashId(6400u+p,7u), bx, y, bw, rh, presets[p].n)) { skyPick[0]=presets[p].r; skyPick[1]=presets[p].g; skyPick[2]=presets[p].b; setSkyColor(skyPick[0],skyPick[1],skyPick[2]); } }
          cx.tip(x,y0,w,rh,"Quick presets. Black = a pure black void background (good for\nspace/theatre envs)."); y+=rh+4*uiScale; }
        if (cx.button(ui::hashId("skycolcl"), x, y, 90*uiScale, rh, "Clear color")) { clearSkyColor(); }
        y+=rh+4*uiScale;
        // image: path + Browse/Set/Clear (equirect panorama PNG/JPG -> inward sky sphere)
        y0=y; cx.label(x,y,86*uiScale,rh,"Image",th.textDim);
        cx.textField(ui::hashId("skyimgf"), x+88*uiScale, y, w-88*uiScale-110*uiScale, rh, skyImgUI);
        if (cx.button(ui::hashId("skyimgbr"), x+w-108*uiScale, y, 52*uiScale, rh, "Browse")) {
            std::string p = pickFileWin32(L"Choose a skybox panorama image (PNG / JPG, equirect)", L"Images (*.png;*.jpg;*.jpeg)", L"*.png;*.jpg;*.jpeg");
            if (!p.empty()) { skyImgUI = p; setSkyImage(p); } }
        if (cx.button(ui::hashId("skyimgcl"), x+w-52*uiScale, y, 52*uiScale, rh, "Clear")) { clearSkyImage(); skyImgUI.clear(); }
        cx.tip(x,y0,w,rh,"Equirect panorama image (PNG/JPG). Previews + cooks onto a large\ninward-facing sky sphere behind everything. Browse picks a file;\nor type/paste a path and press Set."); y+=rh+2*uiScale;
        if (cx.button(ui::hashId("skyimgset"), x+88*uiScale, y, 52*uiScale, rh, "Set") && !skyImgUI.empty()) setSkyImage(skyImgUI);
        // reuse ANY selected mesh's texture as the skybox (no external file needed)
        { bool have = selected>=0 && sceneMeshes && selected<(int)sceneMeshes->size();
          y0=y; if (cx.button(ui::hashId("skyfromsel"), x+88*uiScale+56*uiScale, y, w-88*uiScale-56*uiScale, rh,
                have?"Use selected mesh's texture":"Use selected mesh's texture (select a mesh first)") && have) setSkyImageFromMesh(selected);
          cx.tip(x,y0,w,rh,"Reuse the SELECTED mesh's texture as the skybox (e.g. the env's\nown sky dome texture) - no external image file needed."); }
        y+=rh+4*uiScale;
        // current state line
        { std::string cur = "current: ";
          if (!skyImagePath.empty()) cur += (skyImagePath.rfind("mesh:",0)==0 ? ("texture of mesh "+skyImagePath.substr(5)) : skyImagePath) + (" ("+std::to_string(skyImageW)+"x"+std::to_string(skyImageH)+")");
          else if (skyColorSet()) { char b[48]; snprintf(b,sizeof b,"solid color %.2f,%.2f,%.2f",skyColor[0],skyColor[1],skyColor[2]); cur += b; }
          else cur += "none (env's own background)";
          cx.textAligned(x, y, w, rh*0.9f, cur.c_str(), th.textDim, 0); y+=rh; }
    }
    std::string skyColUI, skyImgUI;   // Scene-tab skybox field buffers (UI state only; the applied state lives in skyColor/skyImagePath)
    float skyPick[3] = {0.f,0.f,0.f};  // color-picker working RGB (mirrors skyColor when set; lets the sliders drag before "set")
    // ── VIEWPORT BACKGROUND color ("the dark one"): the DARK void behind everything = the renderer clear color
    //    (r->clearRGB, default black). This is the PURE editor background — NOT the skybox spawner (which builds a
    //    sky sphere / cooks a SkyboxPlatformComponent). Live + persisted (CFG). bgColorSet=false -> keep default black.
    float bgColor[3] = {0.f,0.f,0.f};
    bool  bgColorSet = false;

    void drawProperties() {
        auto& th = cx.th; VkRect2D a = rcProps;
        float x=(float)a.offset.x, y=(float)a.offset.y, w=(float)a.extent.width, h=(float)a.extent.height;
        dl.rect(x,y,w,h, th.panelBg);
        // tab strip
        float th_h = 22*uiScale; const char* tabs[]={"Object","Scene","Material","Anim","Physics","Cook","Logcat"};
        const char* tabTips[]={
            "Object - transform the selected mesh (move / rotate / scale), rename,\nhide, duplicate or delete it.",
            "Scene - the Meta component manager: background/skybox, fog, colliders,\nspawn, audio; add navmeshes / box colliders.",
            "Material - the selected mesh's material: base color, texture, blend mode,\ntransparency, emissive, double-sided.",
            "Anim - playback + per-clip settings for animated meshes (loop, speed);\nscrub with the timeline at the bottom.",
            "Physics - collision for the home: auto floor, real trimesh collider,\nnavmeshes and box colliders (so you don't fall through).",
            "Cook - turn this home into an installable Quest APK: signing, spoof,\ncollision, fog, far-clip, and Install-over-adb settings.",
            "Logcat - read WHY a cooked home loaded or fell back on the headset,\nover adb with NO root. Works even with no home loaded."};
        // Photoshop-style dock grip: DRAG it to re-dock the tool panel LEFT/RIGHT (only once a home is loaded;
        // on the empty Logcat-only session there's no grip - it was easy to misclick and pointless there).
        float dbw = homeLoaded() ? 30*uiScale : 0.f;
        if (homeLoaded()) {
            bool hv = cx.hover(x, y, dbw, th_h);
            dl.rect(x, y, dbw, th_h, (hv||dockDragging)?th.widgetHot:th.headerBg);
            uint32_t gc = th.textDim;                                   // 2x3 grip dots
            for (int gx=0;gx<2;gx++) for (int gy=0;gy<3;gy++)
                dl.rect(x+dbw*0.5f-3*uiScale+gx*6*uiScale, y+th_h*0.5f-6*uiScale+gy*6*uiScale, 2*uiScale, 2*uiScale, gc);
            if (hv && cx.in.pressed[0]) dockDragging=true;
            cx.tip(x, y, dbw, th_h, "Grip - DRAG to dock the tool panel on the LEFT or RIGHT side\n(like Photoshop). Drag the divider between the panel and the\nviewport to resize it (up to 75% wide).");
        }
        float tx=x+dbw, tw=(w-dbw)/7.f;
        for (int i=0;i<7;i++){ if (cx.tab(ui::hashId(4000+i,13), tx, y, tw, th_h, tabs[i], tab==i)) { tab=i; propScroll=0.f; } cx.tip(tx, y, tw, th_h, tabTips[i]); tx+=tw; }
        dl.rect(x,y+th_h,w,1,th.splitLine);
        // Logcat page owns its whole area (its own scroll + live stream); handle it BEFORE the shared propScroll logic.
        if (tab==TAB_LOGCAT) { drawLogcatPanel(x, y+th_h+1, w, h-th_h-1); return; }
        // SCROLLABLE content: the tool panels outgrew the window - mouse wheel over the panel scrolls,
        // the offset applies to every tab's content, clamped generously (tabs report no exact height).
        if (cx.hover(x, y+th_h, w, h-th_h) && cx.in.wheel!=0) propScroll -= cx.in.wheel * th.rowH * 3.f;
        propScroll = std::clamp(propScroll, 0.f, 1800.f*uiScale);
        float cy = y+th_h+6*uiScale - propScroll, cx0 = x+8*uiScale, cw = w-16*uiScale;
        dl.pushClip(x, y+th_h, w, h-th_h);
        if (propScroll > 0.5f)   // scroll indicator
            cx.textAligned(x+w-40*uiScale, y+th_h+2*uiScale, 36*uiScale, 14*uiScale, "^^^", th.textDim, 2);
        if (tab==TAB_SCENE) { drawScenePanel(cx0, cy, cw); dl.popClip(); return; }   // the Meta-component manager (toggles + Add)
        if (selItem>=0 && selItem<(int)items.size() && tab!=TAB_COOK) { drawItemProps(cx0, cy, cw); dl.popClip(); return; }
        if (selected<0 || selected>=(int)r->gpuMeshes.size()) {
            if (tab==TAB_COOK) drawCookPanel(cx0, cy, cw);
            else cx.label(cx0, cy, cw, th.rowH, "(no selection)", th.textDim);
            dl.popClip(); return;
        }
        VkGpuMesh& gm = r->gpuMeshes[selected];
        if (tab==TAB_OBJECT)   drawObjectTab(gm, cx0, cy, cw);
        else if (tab==TAB_MATERIAL) drawMaterialTab(gm, cx0, cy, cw);
        else if (tab==TAB_ANIM)     drawAnimTab(cx0, cy, cw);
        else if (tab==TAB_PHYSICS)  drawPhysicsTab(cx0, cy, cw);
        else if (tab==TAB_COOK)     drawCookPanel(cx0, cy, cw);
        dl.popClip();
    }

    // a labelled row of N drag fields; records one undo per drag
    void vecRow(VkGpuMesh& gm, const char* lbl, float* v, int n, float speed, float x, float& y, float w, bool isEuler=false, bool isQuat=false) {
        auto& th=cx.th; float rh=th.rowH, lw=70*uiScale, fw=(w-lw)/n - 2*uiScale;
        cx.label(x,y,lw,rh,lbl,th.textDim);
        bool changed=false;
        for (int k=0;k<n;k++){ if (cx.dragFloat(ui::hashId(5000+k, ui::hashId(lbl)), x+lw+k*(fw+2*uiScale), y, fw, rh, v[k], speed)) changed=true; }
        if (changed) {
            if (!editing){ editing=true; editMesh=selected; editBefore=captureX(gm); }
            if (isEuler) eulerToQuat(v, gm.editR);
            if (isQuat)  normalizeQuat(gm.editR);
            recomputeModel(gm);
        }
        y += rh+2*uiScale;
    }
    void drawObjectTab(VkGpuMesh& gm, float x, float y, float w) {
        auto& th=cx.th;
        cx.label(x,y,42*uiScale,th.rowH,"Name",th.textDim);
        cx.textField(ui::hashId(8000u+(unsigned)selected, 9u), x+44*uiScale, y, w-44*uiScale, th.rowH, gm.name); y+=th.rowH+4*uiScale;   // rename the mesh
        vecRow(gm,"Position", gm.editT, 3, 0.01f, x, y, w);
        float e[3]; quatToEuler(gm.editR, e);
        vecRow(gm,"Rotation", e, 3, 0.5f, x, y, w, true);             // euler degrees
        vecRow(gm,"Quat", gm.editR, 4, 0.01f, x, y, w, false, true);  // raw quaternion X Y Z W (kept in sync)
        vecRow(gm,"Scale", gm.editS, 3, 0.01f, x, y, w);
        // UNIFORM scale: drag ONE field to shrink/grow all three axes at once (keeps the X:Y:Z ratio).
        // ("i can't shrink all 3 x y z scale in one go" — the per-axis Scale row needed three separate drags.)
        { auto& th2=cx.th; float rh=th2.rowH, lw=70*uiScale, fw=w-lw-2*uiScale;
          cx.label(x,y,lw,rh,"Scale (all)",th2.textDim);
          float base=(gm.editS[0]+gm.editS[1]+gm.editS[2])/3.f, uni=base;
          if (cx.dragFloat(ui::hashId("uniscale"), x+lw, y, fw, rh, uni, 0.01f) && base>1e-6f) {
              float f=uni/base; if (f>1e-4f){
                  if (!editing){ editing=true; editMesh=selected; editBefore=captureX(gm); }
                  for(int k=0;k<3;k++) gm.editS[k]*=f; recomputeModel(gm); }
          }
          cx.tip(x,y,w,rh,"Drag to scale X, Y and Z together (uniform), keeping their ratio.\nUse the Scale row above to change a single axis.");
          y+=rh+2*uiScale; }
        if (editing && cx.in.released[0]) endEdit(gm);
        if (cx.button(ui::hashId("resetx"), x, y, 120*uiScale, th.rowH, "Reset Transform")) {
            Xform b=captureX(gm); gm.editT[0]=gm.editT[1]=gm.editT[2]=0; gm.editR[0]=gm.editR[1]=gm.editR[2]=0; gm.editR[3]=1; gm.editS[0]=gm.editS[1]=gm.editS[2]=1; recomputeModel(gm); pushUndo(selected,b,captureX(gm));
        }
        if (cx.button(ui::hashId("focusx"), x+126*uiScale, y, 80*uiScale, th.rowH, "Focus")) focusMesh(gm);
        y += th.rowH+4*uiScale;
        // Quick "zero all rotations" (rotation only — keeps position/scale) over the WHOLE selection, one undo step.
        { std::string zl = sel.size()>1 ? ("Zero rotation ("+std::to_string(sel.size())+")") : std::string("Zero rotation");
          if (cx.button(ui::hashId("zerorot"), x, y, 206*uiScale, th.rowH, zl.c_str())) {
              std::vector<int> src = sel.empty()? std::vector<int>{selected} : sel, ms;
              std::vector<Xform> bs, as;
              for (int m : src){ if(m<0||m>=(int)r->gpuMeshes.size()) continue; auto& g2=r->gpuMeshes[m];
                  ms.push_back(m); bs.push_back(captureX(g2)); g2.editR[0]=g2.editR[1]=g2.editR[2]=0.f; g2.editR[3]=1.f; recomputeModel(g2); as.push_back(captureX(g2)); }
              if (!ms.empty()) pushUndo(ms, bs, as);
          }
          y += th.rowH+8*uiScale; }
        // PREFAB entry point right where a selected mesh's properties live (also: right-click > Save as
        // PREFAB, and the Scene tab's Prefabs section — the user could never find it buried in the old menu)
        { std::string pfl = sel.size()>1 ? ("Save as PREFAB ("+std::to_string(sel.size())+" meshes)") : std::string("Save as PREFAB");
          if (cx.button(ui::hashId("mkprefab"), x, y, 206*uiScale, th.rowH, pfl.c_str())) savePrefabFromSelection(); }
        y += th.rowH+8*uiScale;
        // ── GEOMETRY: manual extend/seal (the context menu has 1-click presets; this is the dialed version).
        //    Both AUTO-ADJUST THE TEXTURE: extend continues the border UVs, seal blends the rim UVs. ──
        dl.rect(x,y,w,1,th.splitLine); y+=5*uiScale;
        cx.label(x,y,w,th.rowH,"Geometry (fill gaps/holes)",th.text); y+=th.rowH+2*uiScale;
        cx.label(x,y,70*uiScale,th.rowH,"Distance",th.textDim);
        cx.dragFloat(ui::hashId("extdist"), x+72*uiScale, y, 80*uiScale, th.rowH, extendDist, 0.01f, "%.2f"); y+=th.rowH+3*uiScale;
        // every button below runs over the WHOLE multi-selection
        { float bw3=(w-10*uiScale)/3;
          if (cx.button(ui::hashId("extdn"),  x,                 y, bw3, th.rowH, "Extend Down"))    forEachSelMesh([&](int m){ extendMeshBoundary(m, extendDist, 0); });
          if (cx.button(ui::hashId("extout"), x+bw3+5*uiScale,   y, bw3, th.rowH, "Extend Out"))     forEachSelMesh([&](int m){ extendMeshBoundary(m, extendDist, 1); });
          if (cx.button(ui::hashId("extup"),  x+2*(bw3+5*uiScale), y, bw3, th.rowH, "Extend Up"))    forEachSelMesh([&](int m){ extendMeshBoundary(m, extendDist, 2); });
          y+=th.rowH+3*uiScale; }
        { float bw2=(w-6*uiScale)/2;   // HOLE INSPECTOR: see every gap as a numbered outline, click a marker to seal it
          char hb2[48]; snprintf(hb2,sizeof hb2, showHoles?"Holes: %d shown (click to seal)":"Show holes (inspector)", (int)holeLoops.size());
          if (cx.button(ui::hashId("holeshow"), x, y, bw2, th.rowH, hb2, showHoles)) { showHoles=!showHoles; if (showHoles) refreshHoles(); }
          if (cx.button(ui::hashId("sealhx"), x+bw2+6*uiScale, y, bw2, th.rowH, "Seal ALL holes")) forEachSelMesh([&](int m){ sealMeshHoles(m); });
          y += th.rowH+3*uiScale; }
        { float bw2=(w-6*uiScale)/2;    // PATCH TOOL: paint EXACTLY where geometry goes (gap corners -> Enter)
          char pb2[48]; snprintf(pb2,sizeof pb2, patchMode?"Patching: %d pts (Enter)":"Patch tool (click corners)",(int)patchPts.size());
          if (cx.button(ui::hashId("patchtg"), x, y, bw2, th.rowH, pb2, patchMode&&!pinIsCut)) { patchMode=!(patchMode&&!pinIsCut&&!pinIsBend&&!pinIsKnife); pinIsCut=false; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
              if (patchMode) setStatus("PATCH: click the gap's corner points in the viewport (3+), Enter builds, Esc cancels"); }
          if (cx.button(ui::hashId("patchgo"), x+bw2+6*uiScale, y, bw2, th.rowH, "Build patch (Enter)", false) && patchMode) buildPatch();
          y += th.rowH+3*uiScale; }
        if (cx.button(ui::hashId("reuvflat"), x, y, w, th.rowH, "Re-UV flat (Set texture spans once)")) forEachSelMesh([&](int m){ reUVFlat(m); });
        y += th.rowH+3*uiScale;
        { char cb2[52]; snprintf(cb2,sizeof cb2, (patchMode&&pinIsCut)?"Cutting: %d pts (Enter)":"Cut region tool (click corners)",(int)patchPts.size());
          if (cx.button(ui::hashId("cutreg"), x, y, w, th.rowH, cb2, patchMode&&pinIsCut)) {
              patchMode=!(patchMode&&pinIsCut); pinIsCut=patchMode; pinIsBend=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
              if (patchMode) setStatus("CUT: 2 pins = SPLIT along the line, 3+ pins = cut the polygon out of the SELECTION; Enter applies"); }
          y += th.rowH+3*uiScale; }
        // BEND/STRETCH: grab-deform with falloff (click a point, set radius + offset, Apply)
        { char bb2[52]; snprintf(bb2,sizeof bb2, (patchMode&&pinIsBend)?"Bending: pin set (Enter)":"Bend/stretch tool (click grab point)");
          if (cx.button(ui::hashId("bendtg"), x, y, w, th.rowH, bb2, patchMode&&pinIsBend)) {
              patchMode=!(patchMode&&pinIsBend); pinIsBend=patchMode; pinIsCut=false; pinIsKnife=false; patchPts.clear(); patchCols.clear();
              if (patchMode) setStatus("BEND: click the grab point on the mesh, set Radius + Offset below, Enter (or Apply) deforms"); }
          y += th.rowH+3*uiScale;
          if (patchMode && pinIsBend) {
              cx.label(x,y,54*uiScale,th.rowH,"Radius",th.textDim);
              cx.dragFloat(ui::hashId("bendrad"), x+56*uiScale, y, 70*uiScale, th.rowH, bendRadius, 0.02f, "%.2f"); y+=th.rowH+2*uiScale;
              float lw2=24*uiScale, fw2=(w-lw2*3)/3-4*uiScale; const char* bn[3]={"X","Y","Z"};
              for (int k=0;k<3;k++){ float fx=x+k*(lw2+fw2+4*uiScale);
                  cx.label(fx,y,lw2,th.rowH,bn[k],th.textDim);
                  cx.dragFloat(ui::hashId(9600u+(unsigned)k,19u), fx+lw2, y, fw2, th.rowH, bendOff[k], 0.01f, "%.2f"); }
              y+=th.rowH+2*uiScale;
              if (cx.button(ui::hashId("bendgo"), x, y, w, th.rowH, "Apply bend (Enter)")) bendAtPin();
              y+=th.rowH+3*uiScale;
          } }
        { char kb2[52]; snprintf(kb2,sizeof kb2, (knifeOn&&!knifeBox)?"KNIFE active - drag a line over the mesh":"KNIFE: cut by drawing a LINE");
          if (cx.button(ui::hashId("knife"), x, y, w, th.rowH, kb2, knifeOn&&!knifeBox)) { if (knifeOn&&!knifeBox){ knifeOn=false; knifeDrag=false; } else startKnife(); }
          y += th.rowH+3*uiScale; }
        { char kp2[64]; snprintf(kp2,sizeof kp2, (patchMode&&pinIsKnife)?"KNIFE PATH: %d pts (Enter cuts)":"KNIFE PATH: cut along clicked points",(int)patchPts.size());
          if (cx.button(ui::hashId("knifepath"), x, y, w, th.rowH, kp2, patchMode&&pinIsKnife)) {
              bool on = !(patchMode&&pinIsKnife);
              patchMode=on; pinIsKnife=on; pinIsCut=false; pinIsBend=false; patchPts.clear(); patchCols.clear();
              knifeOn=false; knifeBox=false; knifeDrag=false; sliceGizmoOn=false;
              if (on) setStatus("KNIFE PATH: click 2+ points along the cut line (corners allowed, pins lock to the SELECTION), ENTER cuts, ESC cancels"); }
          y += th.rowH+3*uiScale; }
        { char kb3[64]; snprintf(kb3,sizeof kb3, (knifeOn&&knifeBox)?"KNIFE BOX active - drag a rectangle":"KNIFE BOX: 3D cut (drag a RECTANGLE)");
          if (cx.button(ui::hashId("knifebox"), x, y, w, th.rowH, kb3, knifeOn&&knifeBox)) { if (knifeOn&&knifeBox){ knifeOn=false; knifeBox=false; knifeDrag=false; } else startKnifeBox(); }
          y += th.rowH+2*uiScale;
          cx.label(x,y,80*uiScale,th.rowH,"Cut depth",th.textDim);
          cx.dragFloat(ui::hashId("kboxdep"), x+82*uiScale, y, 70*uiScale, th.rowH, knifeCutDepth, 0.01f, "%.2f");
          cx.label(x+156*uiScale,y,w-156*uiScale,th.rowH,"m from surface (0 = through)",th.textDim);
          if (knifeCutDepth<0.f) knifeCutDepth=0.f;
          y += th.rowH+3*uiScale; }
        { char sb2[52]; snprintf(sb2,sizeof sb2, sliceGizmoOn?"SLICE GIZMO active (Enter cuts)":"Slice GIZMO (place the cut visually)");
          if (cx.button(ui::hashId("slcgiz"), x, y, w, th.rowH, sb2, sliceGizmoOn)) { if (sliceGizmoOn) sliceGizmoOn=false; else startSliceGizmo(nullptr); }
          y += th.rowH+3*uiScale; }
        { float bw3=(w-10*uiScale)/3;   // SLICE: separate the mesh into two halves along a center plane
          if (cx.button(ui::hashId("slcx"), x,                   y, bw3, th.rowH, "Slice X")) forEachSelMesh([&](int m){ sliceMesh(m, 0); });
          if (cx.button(ui::hashId("slcy"), x+bw3+5*uiScale,     y, bw3, th.rowH, "Slice Y")) forEachSelMesh([&](int m){ sliceMesh(m, 1); });
          if (cx.button(ui::hashId("slcz"), x+2*(bw3+5*uiScale), y, bw3, th.rowH, "Slice Z")) forEachSelMesh([&](int m){ sliceMesh(m, 2); });
          y += th.rowH+3*uiScale; }
        cx.label(x,y,70*uiScale,th.rowH,"Cut radius",th.textDim);
        cx.dragFloat(ui::hashId("cutrad"), x+72*uiScale, y, 80*uiScale, th.rowH, cutRadius, 0.01f, "%.2f"); y+=th.rowH+3*uiScale;
        cx.label(x,y,w,th.rowH*0.9f,"(cut: RIGHT-CLICK the exact spot -> 'Cut hole HERE')",th.textDim); y+=th.rowH*0.9f+2*uiScale;
        { float bw3=(w-10*uiScale)/3;
          if (cx.button(ui::hashId("mirx"), x,                   y, bw3, th.rowH, "Mirror X")) forEachSelMesh([&](int m){ mirrorMesh(m, 0); });
          if (cx.button(ui::hashId("miry"), x+bw3+5*uiScale,     y, bw3, th.rowH, "Mirror Y")) forEachSelMesh([&](int m){ mirrorMesh(m, 1); });
          if (cx.button(ui::hashId("mirz"), x+2*(bw3+5*uiScale), y, bw3, th.rowH, "Mirror Z")) forEachSelMesh([&](int m){ mirrorMesh(m, 2); });
          y+=th.rowH+3*uiScale; }
        { float bw3=(w-10*uiScale)/3;   // TRUE rotations about the mesh's own center (repeat to stack: 90 -> 180 -> 270)
          if (cx.button(ui::hashId("rox90"), x,                   y, bw3, th.rowH, "Rot X +90")) forEachSelMesh([&](int m){ rotateMeshGeom(m, 0, 90); });
          if (cx.button(ui::hashId("roy90"), x+bw3+5*uiScale,     y, bw3, th.rowH, "Rot Y +90")) forEachSelMesh([&](int m){ rotateMeshGeom(m, 1, 90); });
          if (cx.button(ui::hashId("roz90"), x+2*(bw3+5*uiScale), y, bw3, th.rowH, "Rot Z +90")) forEachSelMesh([&](int m){ rotateMeshGeom(m, 2, 90); });
          y+=th.rowH+3*uiScale; }
        { float bw2=(w-6*uiScale)/2;
          if (cx.button(ui::hashId("splitp"), x, y, bw2, th.rowH, "Split pieces")) forEachSelMesh([&](int m){ splitMeshParts(m); });
          char fb2[40]; snprintf(fb2,sizeof fb2,"Fuse selection (%d)",(int)sel.size());
          if (cx.button(ui::hashId("fusesel"), x+bw2+6*uiScale, y, bw2, th.rowH, fb2, sel.size()>1)) fuseSelectedMeshes();
          y+=th.rowH+3*uiScale;
          if (cx.button(ui::hashId("fusezf"), x, y, w, th.rowH,
                        fuseDropCovered ? "Fuse cleanup: drop overlapped coplanar tris (ON)"
                                        : "Fuse: keep EVERY original triangle (cleanup OFF)", fuseDropCovered)) {
              fuseDropCovered=!fuseDropCovered;
              setStatus(fuseDropCovered ? "Fuse will drop later-source triangles fully covered by an earlier coplanar sheet (stacked-floor z-fight cleanup)"
                                        : "Fuse keeps the sources' original triangle pattern verbatim (safest for curved/bent geometry + patches)");
          }
          y+=th.rowH+3*uiScale;
          char rb2[52]; snprintf(rb2,sizeof rb2,"Fuse + REBUILD surface (%d) - clean retriangulate",(int)sel.size());
          if (cx.button(ui::hashId("rebuilds"), x, y, w, th.rowH, rb2, !sel.empty())) rebuildSurface();
          y+=th.rowH+3*uiScale;
          bool exc = noColMeshes.count(selected)!=0;
          if (cx.button(ui::hashId("nocol"), x, y, w, th.rowH, exc?"Collision: OFF (walk-through)":"Collision: ON", exc)) {
              bool nowExc = !exc;   // apply to the WHOLE selection
              for (int m : sel) { if (nowExc) noColMeshes.insert(m); else noColMeshes.erase(m); }
              if (sel.empty() && selected>=0) { if (nowExc) noColMeshes.insert(selected); else noColMeshes.erase(selected); }
              bakeNavmeshes(items);
          }
          y+=th.rowH+3*uiScale;
          { std::vector<int> tgc = sel.empty() ? (selected>=0?std::vector<int>{selected}:std::vector<int>{}) : sel;
            int nb = boundMeshColliders(tgc);
            char rb3[64]; snprintf(rb3,sizeof rb3,"Remove mesh collider (%d bound)",nb);
            if (cx.button(ui::hashId("rmcol"), x, y, w, th.rowH, rb3, false) && nb>0) {
                int nrm=removeMeshColliders(tgc);
                setStatus(nrm ? ("Removed "+std::to_string(nrm)+" mesh collider component(s) - Ctrl+Z restores")
                              : "no mesh collider is bound to the selection");
            } }
          y+=th.rowH+8*uiScale; }
        // hidden edit log (names never change; this is the only place edits show)
        { auto it=meshEditLog.find(selected);
          if (it!=meshEditLog.end()) { std::string el = "edits: "+it->second;
              cx.label(x,y,w,th.rowH*0.9f, el.c_str(), th.textDim); y+=th.rowH*0.9f+2*uiScale; } }
        char b[96]; snprintf(b,sizeof b,"indices: %u    centroid %.1f %.1f %.1f", gm.nIdx, gm.centroid[0],gm.centroid[1],gm.centroid[2]);
        cx.label(x,y,w,th.rowH,b,th.textDim); y+=th.rowH;
        snprintf(b,sizeof b,"skinned: %s   dynamic: %s", gm.isSkinned?"yes":"no", gm.dynamicVerts?"yes":"no");
        cx.label(x,y,w,th.rowH,b,th.textDim); y+=th.rowH;
        // Skybox backdrop toggle — cooks this mesh as a SkyboxPlatformComponent (far-clip-EXEMPT, escapes PortalStereoCamera
        // far=5000). Reflects/sets the per-mesh mark; if several meshes are selected it applies to the whole selection.
        { bool sky = isSkyboxMesh(selected);
          cx.checkbox(ui::hashId("meshsky"), x, y, "Skybox backdrop (escapes 5000 far-clip)", sky);
          if (sky != isSkyboxMesh(selected)) {
              std::vector<int> tg=(sel.size()>1)?sel:std::vector<int>{selected};
              for(int t:tg){ if(isSkyboxMesh(t)!=sky) toggleSkybox(t); }
              setStatus(std::string(sky?"Marked ":"Unmarked ")+std::to_string(tg.size())+" mesh(es) as skybox backdrop");
          }
          y+=th.rowH+6*uiScale; }
        drawComponentsSection(gm, x, y, w);   // generic inspect/edit of ALL hstf components on this entity
    }
    // GENERIC component inspector — lists EVERY hstf component on the selected entity + every field, editable.
    // Covers all 30 v203 component classes (and any extras) with no per-component code. Field meanings:
    // V203_COMPONENTS_IDA.md (IDA-decoded). bool->toggle, number->drag, else->text. Edits write the value back
    // into the captured field (live inspector state; cook/export write-back is the next hop).
    void drawComponentsSection(VkGpuMesh& gm, float x, float& y, float w) {
        auto& th=cx.th;
        if (gm.components.empty()) { cx.label(x,y,w,th.rowH,"(no components captured)",th.textDim); y+=th.rowH; return; }
        char hb[64]; snprintf(hb,sizeof hb,"Components (%zu)", gm.components.size());
        y += 6*uiScale; cx.label(x,y,w,th.rowH,hb,th.text); y+=th.rowH+2*uiScale;
        float lw = 0.52f*w;
        for (size_t ci=0; ci<gm.components.size(); ++ci) {
            auto& ec = gm.components[ci];
            char cb[160]; snprintf(cb,sizeof cb,"%s  v%d", ec.shortCls.c_str(), ec.version);
            dl.rect(x, y, w, th.rowH, th.splitLine);
            cx.label(x+4*uiScale,y,w,th.rowH,cb,th.text); y+=th.rowH+1*uiScale;
            for (size_t fi=0; fi<ec.fields.size(); ++fi) {
                auto& f = ec.fields[fi];
                cx.label(x+10*uiScale, y, lw-10*uiScale, th.rowH, f.first.c_str(), th.textDim);
                unsigned id = ui::hashId((unsigned)(ci*1009u+fi+90001u), 11u);
                const std::string& s = f.second;
                bool isBool = (s=="true"||s=="false");
                bool isNum  = !s.empty() && s.find_first_not_of("0123456789.eE+-")==std::string::npos
                              && s.find_first_of("0123456789")!=std::string::npos;
                if (isBool) {
                    bool bv = (s=="true");
                    if (cx.checkbox(id, x+lw, y, "", bv)) f.second = bv?"true":"false";
                } else if (isNum) {
                    float fv=(float)atof(s.c_str());
                    if (cx.dragFloat(id, x+lw, y, w-lw, th.rowH, fv, 0.01f, "%g")) { char nb[40]; snprintf(nb,sizeof nb,"%g",fv); f.second=nb; }
                } else {
                    cx.textField(id, x+lw, y, w-lw, th.rowH, f.second);
                }
                y += th.rowH+1*uiScale;
            }
            y += 3*uiScale;
        }
    }
    // ── MATERIAL EDITOR: live-editable flags + tint, texture swap/export, shader inspector.
    // Flag edits write BOTH the GPU mesh (live preview) AND the CPU MeshData (so the cook ships them);
    // tint rides the per-draw color push constant (live). All persisted in the .hsledit session.
    void drawMaterialTab(VkGpuMesh& gm, float x, float y, float w) {
        auto& th=cx.th; float rh=th.rowH; char b[192];
        MeshData* md = (sceneMeshes && selected>=0 && selected<(int)sceneMeshes->size()) ? &(*sceneMeshes)[selected] : nullptr;
        // ── SHADER INSPECTOR ─────────────────────────────────────────────
        cx.label(x,y,w,rh,"Shader",th.text); y+=rh;
        { std::string sn = (gm.progIdx>=0 && gm.progIdx<(int)r->programs.size())
                             ? r->programs[gm.progIdx].surface
                             : (r->globalShaderPath.empty() ? std::string("built-in V79 unlit (texture RGBA)") : r->globalShaderPath);
          size_t sl=sn.find_last_of('/'); if (sl!=std::string::npos) sn=sn.substr(sl+1);
          snprintf(b,sizeof b,"  %s%s", sn.c_str(), gm.isSkinned?"  [skinned]":""); cx.label(x,y,w,rh,b,th.textDim); y+=rh;
          snprintf(b,sizeof b,"  progIdx %d   stride %u   idx %u", gm.progIdx, gm.vboStride, gm.nIdx);
          cx.label(x,y,w,rh,b,th.textDim); y+=rh+4*uiScale; }
        dl.rect(x,y,w,1,th.splitLine); y+=5*uiScale;
        // ── FLAGS (live + cook) ──────────────────────────────────────────
        cx.label(x,y,w,rh,"Material flags  (live + cook)",th.text); y+=rh+2*uiScale;
        bool ch=false, v;
        v=gm.useBlend;  if (cx.checkbox(ui::hashId("mfblend"), x, y, "Alpha blend (transparent)", v)) { gm.useBlend=v;  if(md) md->useBlend=v;  ch=true; } y+=rh;
        v=gm.additive;  if (cx.checkbox(ui::hashId("mfadd"),   x, y, "Additive (glow / god-rays)", v)) { gm.additive=v;  if(md) md->additive=v;  ch=true; } y+=rh;
        v=gm.alphaTest; if (cx.checkbox(ui::hashId("mfatest"), x, y, "Alpha test (cutout, depth-writes)", v)) { gm.alphaTest=v; if(md) md->alphaTest=v; ch=true; } y+=rh;
        v=gm.cullBack;  if (cx.checkbox(ui::hashId("mfcull"),  x, y, "Back-face cull (single-sided)", v)) { gm.cullBack=v;  if(md) md->doubleSided=!v; ch=true; } y+=rh+4*uiScale;
        if (ch) matEdited.insert(selected);   // session: persist a MATF line for this mesh
        // ── TINT (live, rides the color push constant) ───────────────────
        cx.label(x,y,w,rh,"Tint  (r g b a, live)",th.text); y+=rh+2*uiScale;
        { float lw=26*uiScale, fw=(w-lw*4)/4 - 2*uiScale; const char* nm[4]={"R","G","B","A"}; bool tch=false;
          for (int k=0;k<4;k++){ float fx=x+k*(lw+fw+2*uiScale);
              cx.label(fx,y,lw,rh,nm[k],th.textDim);
              if (cx.dragFloat(ui::hashId(9100u+(unsigned)k,13u), fx+lw, y, fw, rh, gm.editTint[k], 0.005f, "%.2f")) tch=true; }
          y+=rh+2*uiScale;
          if (cx.button(ui::hashId("tintreset"), x, y, 120*uiScale, rh, "Reset tint")) { for(int k=0;k<4;k++) gm.editTint[k]=1.f; tch=true; }
          if (tch) matEdited.insert(selected);
          y+=rh+4*uiScale; }
        dl.rect(x,y,w,1,th.splitLine); y+=5*uiScale;
        // ── TEXTURE (swap / export) ──────────────────────────────────────
        cx.label(x,y,w,rh,"Base texture",th.text); y+=rh;
        snprintf(b,sizeof b,"  %dx%d%s", md?md->texW:0, md?md->texH:0, texOverride.count(selected)?"  (REPLACED)":"");
        cx.label(x,y,w,rh,b,th.textDim); y+=rh+2*uiScale;
        { float bw2=(w-8*uiScale)/2;
          if (cx.button(ui::hashId("mtset"), x, y, bw2, rh+2*uiScale, "Set texture...", true)) {
              std::string p=pickFileWin32(L"Choose texture (PNG / JPG)", L"Images (*.png;*.jpg;*.jpeg)", L"*.png;*.jpg;*.jpeg");
              if (!p.empty()) setMeshTexture(selected, p);
          }
          if (cx.button(ui::hashId("mtexp"), x+bw2+6*uiScale, y, bw2, rh+2*uiScale, "Export PNG") && md && !md->texRGBA.empty()) {
              std::error_code ec; std::filesystem::create_directories("saved", ec);   // export failed silently when saved/ didn't exist
              std::string safe = "saved/"; for (char c : gm.name) safe += (c=='/'||c=='\\'||c==':')?'_':c; safe += "_tex.png";
              if (stbi_write_png(safe.c_str(), md->texW, md->texH, 4, md->texRGBA.data(), md->texW*4)) {
                  writeAiPromptSidecar(*md, safe);   // ChatGPT prompt -> clipboard + _PROMPT.txt beside the PNG
                  std::string abs = std::filesystem::absolute(safe, ec).string();
#ifdef _WIN32
                  for (char& c : abs) if (c=='/') c='\\';
                  setStatus("Exported texture -> "+abs+"  |  ChatGPT prompt COPIED to clipboard (paste it with the image; also saved as _PROMPT.txt)");
                  system(("explorer /select,\""+abs+"\"").c_str());   // SHOW the user exactly where it landed
#else
                  setStatus("Exported texture -> "+abs+"  |  ChatGPT prompt COPIED to clipboard (paste it with the image; also saved as _PROMPT.txt)");
                  std::string dir = abs; size_t sl2 = dir.find_last_of('/'); if (sl2 != std::string::npos) dir = dir.substr(0, sl2);
  #ifdef __APPLE__
                  system(("open \""+dir+"\" >/dev/null 2>&1 &").c_str());
  #else
                  system(("xdg-open \""+dir+"\" >/dev/null 2>&1 &").c_str());
  #endif
#endif
              } else setStatus("Texture export FAILED (disk full / path?): "+safe);
          }
          y+=rh+8*uiScale; }
        // ── TEXTURE ADJUST (bake brightness/saturation/hue into the texture; previews live + ships in the cook) ──
        cx.label(x,y,w,rh,"Adjust texture  (bakes in, live)",th.text); y+=rh+2*uiScale;
        { float lw=32*uiScale, fw=(w-lw*3)/3 - 4*uiScale;
          cx.label(x,y,lw,rh,"Brt",th.textDim);  cx.dragFloat(ui::hashId("txbrt"), x+lw, y, fw, rh, texBright, 0.005f, "%.2f");
          cx.label(x+lw+fw+4*uiScale,y,lw,rh,"Sat",th.textDim); cx.dragFloat(ui::hashId("txsat"), x+2*lw+fw+4*uiScale, y, fw, rh, texSat, 0.005f, "%.2f");
          cx.label(x+2*(lw+fw+4*uiScale),y,lw,rh,"Hue",th.textDim); cx.dragFloat(ui::hashId("txhue"), x+3*lw+2*fw+8*uiScale, y, fw, rh, texHueDeg, 0.5f, "%.0f");
          y+=rh+3*uiScale;
          if (cx.button(ui::hashId("txapply"), x, y, w, rh, "Apply adjust (bakes into the texture)")) applyTextureAdjust(selected);
          y+=rh+4*uiScale; }
        // ── DE-SHADOW: lift the lightmap shading BAKED into the texture (large-scale luminance flatten) ──
        { cx.label(x,y,64*uiScale,rh,"Shadow",th.textDim);
          cx.dragFloat(ui::hashId("dshstr"), x+66*uiScale, y, 70*uiScale, rh, deshadowStr, 0.005f, "%.2f");
          if (cx.button(ui::hashId("dshgo"), x+142*uiScale, y, w-142*uiScale, rh, "Remove baked shadows")) forEachSelMesh([&](int m){ removeBakedShadows(m); });
          y+=rh+3*uiScale;
          if (cx.button(ui::hashId("txundo"), x, y, w, rh, "Undo texture (restore before last texture op)")) undoTexture(selected);
          y+=rh+4*uiScale; }
        // ── TEXTURE GENERATOR (procedural replacements; previews live + ships in the cook) ──
        cx.label(x,y,w,rh,"Generate texture",th.text); y+=rh+2*uiScale;
        { float bw3=(w-10*uiScale)/3;
          if (cx.button(ui::hashId("tgsolid"), x,                   y, bw3, rh, "Solid (tint)")) generateTexture(selected, 0);
          if (cx.button(ui::hashId("tgcheck"), x+bw3+5*uiScale,     y, bw3, rh, "Checker"))      generateTexture(selected, 1);
          if (cx.button(ui::hashId("tgnoise"), x+2*(bw3+5*uiScale), y, bw3, rh, "Noise"))        generateTexture(selected, 2);
          y+=rh+2*uiScale;
          cx.label(x,y,w,rh*0.9f,"(Solid uses the Tint RGB above as the color)",th.textDim); y+=rh*0.9f+4*uiScale; }
        // ── SHADER DECOMPILER + COMPILER (full in-editor round trip) ───────────────────
        // Decompile: active program SPIR-V -> GLSL (tools/spirv-cross), opened for editing.
        // Compile+Apply: edited GLSL -> SPIR-V (tools/glslangValidator) -> pipelines HOT-RELOADED live.
        { float bw2=(w-8*uiScale)/2;
          if (cx.button(ui::hashId("shdec"), x, y+4*uiScale, bw2, rh+2*uiScale, "Decompile -> GLSL", true)) decompileShaderFor(selected);
          if (cx.button(ui::hashId("shcmp"), x+bw2+6*uiScale, y+4*uiScale, bw2, rh+2*uiScale, "Compile + APPLY", true)) compileApplyShaderFor(selected);
          y+=rh+10*uiScale;
          if (cx.button(ui::hashId("solox"), x, y, bw2, rh, r->soloMesh==selected?"Unsolo":"Solo only")) {
              bool was = (r->soloMesh==selected); r->soloMesh = was?-1:selected;
              if (!was && selected>=0 && selected<(int)r->gpuMeshes.size()) focusMesh(r->gpuMeshes[selected]);   // solo auto-focuses
          }
          y+=rh+4*uiScale;
          cx.label(x,y,w,rh*0.9f,"Decompile writes saved/shaders/<name>.{vert,frag}.glsl and",th.textDim); y+=rh*0.85f;
          cx.label(x,y,w,rh*0.9f,"opens them; edit, then Compile+APPLY hot-reloads them LIVE.",th.textDim); }
    }
    // Compile the (edited) GLSL for this mesh's shader back to SPIR-V with the vendored glslangValidator
    // and HOT-RELOAD the pipelines live (r->reloadShaderFor). Compile errors land in the status line.
    void compileApplyShaderFor(int mi) {
        if (!r || mi<0 || mi>=(int)r->gpuMeshes.size()) return;
        auto& gm = r->gpuMeshes[mi];
        std::string sn = "builtin_v79_unlit";
        if (gm.progIdx>=0 && gm.progIdx<(int)r->programs.size()) sn = r->programs[gm.progIdx].surface;
        std::string safe; for (char c : sn) safe += (isalnum((unsigned char)c)||c=='_'||c=='-')?c:'_';
        std::string vg="saved/shaders/"+safe+".vert.glsl", fg="saved/shaders/"+safe+".frag.glsl";
        if (!std::filesystem::exists(vg) || !std::filesystem::exists(fg)) { setStatus("Compile: run 'Decompile -> GLSL' first ("+vg+" missing)"); return; }
#ifdef _WIN32
        std::string gv = AppConfig::s_exeDir + "\\..\\tools\\glslangValidator.exe";
        if (FILE* t=fopen(gv.c_str(),"rb")) fclose(t); else gv = "tools/glslangValidator.exe";
        const std::string shPfx = "cmd /C ";   // Windows system() = cmd /c <str>, which strips ONE outer quote pair — wrap explicitly
#else
        std::string gv = AppConfig::exeRel("../tools/glslangValidator");
        if (FILE* t=fopen(gv.c_str(),"rb")) fclose(t); else if (FILE* t2=fopen("tools/glslangValidator","rb")) { fclose(t2); gv="tools/glslangValidator"; } else gv = "glslangValidator";   // PATH
        const std::string shPfx = "";
#endif
        std::string vs="saved/shaders/"+safe+".vert.edit.spv", fs="saved/shaders/"+safe+".frag.edit.spv", lg="saved/shaders/_compile.log";
        std::error_code ec; std::filesystem::remove(vs,ec); std::filesystem::remove(fs,ec);
        system((shPfx+"\""+gv+"\" -V -S vert \""+vg+"\" -o \""+vs+"\" > \""+lg+"\" 2>&1").c_str());
        system((shPfx+"\""+gv+"\" -V -S frag \""+fg+"\" -o \""+fs+"\" >> \""+lg+"\" 2>&1").c_str());
        auto rd=[&](const std::string& p, std::vector<u32>& out)->bool{ FILE* f=fopen(p.c_str(),"rb"); if(!f) return false;
            fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
            if (n<20 || (n&3)){ fclose(f); return false; } out.resize((size_t)n/4); fread(out.data(),4,out.size(),f); fclose(f); return true; };
        std::vector<u32> vspv, fspv;
        if (!rd(vs,vspv) || !rd(fs,fspv)) {
            std::string err; FILE* f=fopen(lg.c_str(),"rb");   // surface the FIRST validator error
            if (f){ char buf[512]; size_t n2=fread(buf,1,sizeof buf-1,f); buf[n2]=0; fclose(f);
                    for (char* c=buf;*c;++c) if(*c=='\r'||*c=='\n') *c=' '; err=buf; }
            setStatus("Shader compile FAILED: "+err.substr(0,220));
            return;
        }
        bool ok = r->reloadShaderFor(mi, vspv, fspv);
        setStatus(ok ? ("Shader '"+sn+"' compiled + HOT-RELOADED - the change is LIVE in the viewport")
                     : "Shader compiled but pipeline rebuild FAILED (kept old shader) - check bindings match");
    }
    // Dump the mesh's ACTIVE shader program (per-material or global) to saved/shaders/ as .spv, then
    // run the vendored tools/spirv-cross.exe to emit readable GLSL beside it and open the folder.
    // Recompile path: edit the GLSL -> tools/glsl_to_surface.py (glslangValidator) -> cookable .surface.
    void decompileShaderFor(int mi) {
        if (!r || mi<0 || mi>=(int)r->gpuMeshes.size()) return;
        auto& gm = r->gpuMeshes[mi];
        const std::vector<u32>* vs = &r->vertSpirv; const std::vector<u32>* fs = &r->fragSpirv;
        std::string sn = "builtin_v79_unlit";
        if (gm.progIdx>=0 && gm.progIdx<(int)r->programs.size()) {
            sn = r->programs[gm.progIdx].surface;
            for (auto& ls : r->loadedShaders) if (ls.surface==sn && !ls.vert.empty() && !ls.frag.empty()) { vs=&ls.vert; fs=&ls.frag; break; }
        }
        std::string safe; for (char c : sn) safe += (isalnum((unsigned char)c)||c=='_'||c=='-')?c:'_';
        std::filesystem::create_directories("saved/shaders");
        std::string vp = "saved/shaders/"+safe+".vert.spv", fp = "saved/shaders/"+safe+".frag.spv";
        auto wr=[&](const std::string& p, const std::vector<u32>& d){ FILE* f=fopen(p.c_str(),"wb"); if(f){ fwrite(d.data(),4,d.size(),f); fclose(f);} };
        wr(vp,*vs); wr(fp,*fs);
#ifdef _WIN32
        std::string sc = AppConfig::s_exeDir + "\\..\\tools\\spirv-cross.exe";
        if (FILE* t=fopen(sc.c_str(),"rb")) fclose(t); else sc = "tools/spirv-cross.exe";   // repo-root fallback
        std::string cmd = "\""+sc+"\" --version 450 \""+vp+"\" --output \"saved/shaders/"+safe+".vert.glsl\""
                          " & \""+sc+"\" --version 450 \""+fp+"\" --output \"saved/shaders/"+safe+".frag.glsl\"";
        system(("cmd /C "+cmd+" >NUL 2>&1").c_str());
        // open both GLSL files for editing right away (default editor), + the folder for context
        system(("cmd /C start \"\" notepad \"saved\\shaders\\"+safe+".vert.glsl\" >NUL 2>&1").c_str());
        system(("cmd /C start \"\" notepad \"saved\\shaders\\"+safe+".frag.glsl\" >NUL 2>&1").c_str());
#else
        std::string sc = AppConfig::exeRel("../tools/spirv-cross");
        if (FILE* t=fopen(sc.c_str(),"rb")) fclose(t); else if (FILE* t2=fopen("tools/spirv-cross","rb")) { fclose(t2); sc="tools/spirv-cross"; } else sc = "spirv-cross";   // PATH
        system(("\""+sc+"\" --version 450 \""+vp+"\" --output \"saved/shaders/"+safe+".vert.glsl\" >/dev/null 2>&1"
                " ; \""+sc+"\" --version 450 \""+fp+"\" --output \"saved/shaders/"+safe+".frag.glsl\" >/dev/null 2>&1").c_str());
#ifdef __APPLE__
        const char* opener = "open";
#else
        const char* opener = "xdg-open";
#endif
        system((std::string(opener)+" \"saved/shaders/"+safe+".vert.glsl\" >/dev/null 2>&1 &").c_str());
        system((std::string(opener)+" \"saved/shaders/"+safe+".frag.glsl\" >/dev/null 2>&1 &").c_str());
#endif
        setStatus("Shader '"+sn+"' -> saved/shaders/"+safe+".{vert,frag}.glsl OPENED - edit, then 'Compile + APPLY' hot-reloads it live");
    }
    std::set<int> matEdited;   // meshes whose material flags/tint were edited (session MATF/TINT lines)
    float texBright = 1.f, texSat = 1.f, texHueDeg = 0.f;   // Material tab texture-adjust fields (baked on Apply)
    float deshadowStr = 0.8f;                               // "Remove baked shadows" strength (0..1)

    // ── ONE-STEP TEXTURE UNDO: every texture-modifying op (adjust / de-shadow / generate / set) backs
    //    the previous pixels up first; "Undo texture" restores them. A ruined texture is never final.
    struct TexBackup { u32 w=0,h=0; std::vector<u8> rgba; };
    std::map<int,TexBackup> texBackup;
    void backupTexture(int mi){
        if (!sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()) return;
        const MeshData& md=(*sceneMeshes)[(size_t)mi];
        if (md.texRGBA.empty()) return;
        texBackup[mi] = TexBackup{ md.texW, md.texH, md.texRGBA };
    }
    void undoTexture(int mi){
        auto it=texBackup.find(mi);
        if (it==texBackup.end() || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()){ setStatus("undo texture: no backup for this mesh"); return; }
        MeshData& md=(*sceneMeshes)[(size_t)mi];
        std::swap(md.texW, it->second.w); std::swap(md.texH, it->second.h); std::swap(md.texRGBA, it->second.rgba);   // swap = redo-able
        md.astcRaw.clear();
        r->replaceMeshTexture((size_t)mi, *sceneMeshes);
        setStatus("Texture restored from backup (press again to swap back)");
    }

    // ── REMOVE BAKED SHADOWS: envs bake lightmap shading INTO the texture; this flattens the LARGE-
    //    SCALE luminance (divide by a heavily blurred luminance map, normalized to the image mean) so
    //    shadows/gradients lift while the fine texture detail stays. strength = lerp toward the result.
    void removeBakedShadows(int mi){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()) return;
        MeshData& md = (*sceneMeshes)[(size_t)mi];
        int W=(int)md.texW, H=(int)md.texH;
        if (md.texRGBA.empty() || W<4 || H<4) { setStatus("de-shadow: mesh has no texture"); return; }
        backupTexture(mi);   // "Undo texture" can always take this back
        // coarse luminance field: downsample to <=64 wide, two box-blur passes, bilinear upsample on read.
        // NEAR-WHITE texels (atlas PADDING) and near-black voids are EXCLUDED from the statistics —
        // a fused atlas's white padding used to drag the mean up and blow the whole floor to white.
        int dw = std::min(64, W), dh = std::min(64, H);
        std::vector<float> lum((size_t)dw*dh, 0.f);
        for (int y2=0;y2<dh;++y2) for (int x2=0;x2<dw;++x2){
            int sx0=x2*W/dw, sx1=std::max(sx0+1,(x2+1)*W/dw), sy0=y2*H/dh, sy1=std::max(sy0+1,(y2+1)*H/dh);
            double acc=0; int cnt=0;
            for (int sy=sy0; sy<sy1; sy+=std::max(1,(sy1-sy0)/4)) for (int sx=sx0; sx<sx1; sx+=std::max(1,(sx1-sx0)/4)){
                const u8* p=&md.texRGBA[((size_t)sy*W+sx)*4];
                double l = 0.299*p[0]+0.587*p[1]+0.114*p[2];
                if (l > 245.0 || l < 6.0) continue;                 // padding / void - not real surface
                acc += l; ++cnt; }
            lum[(size_t)y2*dw+x2] = cnt? (float)(acc/cnt/255.0) : -1.f;   // -1 = no data (filled below)
        }
        // fill no-data cells from neighbors so padding regions don't distort the field
        for (int pass=0; pass<3; ++pass){ std::vector<float> tmp=lum; bool any=false;
            for (int y2=0;y2<dh;++y2) for (int x2=0;x2<dw;++x2){ if (tmp[(size_t)y2*dw+x2]>=0) continue;
                float a=0; int c2=0;
                for (int oy=-1;oy<=1;++oy) for (int ox=-1;ox<=1;++ox){ int xx=x2+ox, yy=y2+oy;
                    if (xx<0||yy<0||xx>=dw||yy>=dh) continue; float v=tmp[(size_t)yy*dw+xx]; if (v>=0){ a+=v; ++c2; } }
                if (c2){ lum[(size_t)y2*dw+x2]=a/c2; any=true; } }
            if (!any) break; }
        for (auto& v : lum) if (v<0) v=0.5f;
        for (int pass=0; pass<2; ++pass){ std::vector<float> tmp=lum;
            for (int y2=0;y2<dh;++y2) for (int x2=0;x2<dw;++x2){ float a=0; int c2=0;
                for (int oy=-2;oy<=2;++oy) for (int ox=-2;ox<=2;++ox){ int xx=x2+ox, yy=y2+oy;
                    if (xx<0||yy<0||xx>=dw||yy>=dh) continue; a+=tmp[(size_t)yy*dw+xx]; ++c2; }
                lum[(size_t)y2*dw+x2]=a/c2; } }
        double mean=0; for (float v : lum) mean+=v; mean/= (double)lum.size(); if (mean<0.05) mean=0.05;
        float st = std::clamp(deshadowStr, 0.f, 1.f);
        for (int y2=0;y2<H;++y2) for (int x2=0;x2<W;++x2){
            u8* px=&md.texRGBA[((size_t)y2*W+x2)*4];
            double lpix = 0.299*px[0]+0.587*px[1]+0.114*px[2];
            if (lpix > 245.0 || lpix < 6.0) continue;             // don't touch padding/void texels
            float fu=(x2+0.5f)/W*dw-0.5f, fv=(y2+0.5f)/H*dh-0.5f;
            int u0=(int)std::floor(fu), v0=(int)std::floor(fv); float tu=fu-u0, tv=fv-v0;
            auto L=[&](int a,int b2)->float{ a=std::clamp(a,0,dw-1); b2=std::clamp(b2,0,dh-1); return lum[(size_t)b2*dw+a]; };
            float bl = (L(u0,v0)*(1-tu)+L(u0+1,v0)*tu)*(1-tv) + (L(u0,v0+1)*(1-tu)+L(u0+1,v0+1)*tu)*tv;
            float f = (float)mean / std::max(bl, 0.04f);
            f = std::clamp(f, 0.6f, 1.8f);                        // NEVER blow out to white / crush to black
            f = 1.f + (f-1.f)*st;                                 // strength lerp
            for (int ch=0;ch<3;++ch) px[ch]=(u8)std::clamp(px[ch]*f, 0.f, 255.f);
        }
        md.astcRaw.clear();
        r->replaceMeshTexture((size_t)mi, *sceneMeshes);
        setStatus("Baked shadows lifted (strength "+std::to_string(st).substr(0,4)+") - previews now, ships in the cook. Repeat to flatten more; Set texture restores.");
    }

    // ── TEXTURE ADJUST: bake brightness / saturation / hue-rotate into md.texRGBA, live re-upload.
    //    (Baked = it SHIPS in the cook; the Tint above is the non-destructive alternative.)
    void applyTextureAdjust(int mi){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size()) return;
        MeshData& md = (*sceneMeshes)[(size_t)mi];
        if (md.texRGBA.empty()) { setStatus("adjust: mesh has no texture"); return; }
        backupTexture(mi);   // "Undo texture" can take this back
        float rad = texHueDeg*3.14159265f/180.f, cH=std::cos(rad), sH=std::sin(rad);
        // hue rotation about the grey axis (standard YIQ-ish matrix), then saturation lerp, then brightness
        float m[9] = { 0.213f+cH*0.787f-sH*0.213f, 0.715f-cH*0.715f-sH*0.715f, 0.072f-cH*0.072f+sH*0.928f,
                       0.213f-cH*0.213f+sH*0.143f, 0.715f+cH*0.285f+sH*0.140f, 0.072f-cH*0.072f-sH*0.283f,
                       0.213f-cH*0.213f-sH*0.787f, 0.715f-cH*0.715f+sH*0.715f, 0.072f+cH*0.928f+sH*0.072f };
        for (size_t i2=0;i2+3<md.texRGBA.size();i2+=4){
            float R=md.texRGBA[i2]/255.f, G=md.texRGBA[i2+1]/255.f, B=md.texRGBA[i2+2]/255.f;
            float hr=m[0]*R+m[1]*G+m[2]*B, hg=m[3]*R+m[4]*G+m[5]*B, hb=m[6]*R+m[7]*G+m[8]*B;
            float grey = 0.299f*hr+0.587f*hg+0.114f*hb;
            hr = grey + (hr-grey)*texSat; hg = grey + (hg-grey)*texSat; hb = grey + (hb-grey)*texSat;
            auto q=[&](float v){ v*=texBright; return (u8)std::clamp(v*255.f+0.5f, 0.f, 255.f); };
            md.texRGBA[i2]=q(hr); md.texRGBA[i2+1]=q(hg); md.texRGBA[i2+2]=q(hb);
        }
        md.astcRaw.clear();                                   // compressed source no longer matches
        r->replaceMeshTexture((size_t)mi, *sceneMeshes);      // live re-upload
        setStatus("Texture adjusted (brt "+std::to_string(texBright).substr(0,4)+" sat "+std::to_string(texSat).substr(0,4)
                  +" hue "+std::to_string((int)texHueDeg)+") - baked, previews now, ships in the cook");
        texBright=1.f; texSat=1.f; texHueDeg=0.f;             // adjustments are relative - reset for the next round
    }
    // ── TEXTURE GENERATOR: replace the mesh's texture procedurally. kind 0=solid(editTint RGB) 1=checker 2=value noise.
    void generateTexture(int mi, int kind){
        if (!r || !sceneMeshes || mi<0 || mi>=(int)sceneMeshes->size() || mi>=(int)r->gpuMeshes.size()) return;
        MeshData& md = (*sceneMeshes)[(size_t)mi];
        backupTexture(mi);   // "Undo texture" can take this back
        const int S = 512;
        md.texRGBA.assign((size_t)S*S*4, 255); md.texW=S; md.texH=S; md.hasTexture=true; md.astcRaw.clear();
        const float* t = r->gpuMeshes[(size_t)mi].editTint;
        auto q=[&](float v){ return (u8)std::clamp(v*255.f+0.5f, 0.f, 255.f); };
        for (int y2=0;y2<S;++y2) for (int x2=0;x2<S;++x2){
            u8* p=&md.texRGBA[((size_t)y2*S+x2)*4];
            if (kind==0){ p[0]=q(t[0]); p[1]=q(t[1]); p[2]=q(t[2]); }
            else if (kind==1){ bool a2=((x2/32)+(y2/32))&1; u8 v=a2?200:72; p[0]=p[1]=p[2]=v; }
            else { u32 h=(u32)(x2*374761393u + y2*668265263u); h=(h^(h>>13))*1274126177u; h^=h>>16;
                   u8 v=(u8)(96 + (h%128)); p[0]=p[1]=p[2]=v; }
        }
        r->replaceMeshTexture((size_t)mi, *sceneMeshes);
        setStatus(std::string("Generated ")+(kind==0?"SOLID (tint color)":kind==1?"CHECKER":"NOISE")+" 512x512 texture - previews now, ships in the cook");
    }
    void drawAnimTab(float x, float y, float w) {
        auto& th=cx.th;
        if (!animOverride || !animScrub) { cx.label(x,y,w,th.rowH,"(no animation plumbed)",th.textDim); return; }
        cx.checkbox(ui::hashId("animplay"), x, y, "Playing (live playhead)", animPlaying); y+=th.rowH+4*uiScale;
        cx.label(x,y,70*uiScale,th.rowH,"Time",th.textDim);
        float dur = animDuration>0?animDuration:1.f; float t=*animScrub;
        if (cx.dragFloat(ui::hashId("animt"), x+72*uiScale, y, w-72*uiScale, th.rowH, t, dur*0.005f, "%.2f")) { *animScrub = std::clamp(t,0.f,dur); *animOverride=true; animPlaying=false; }
        y+=th.rowH+6*uiScale;
        char b[64]; snprintf(b,sizeof b,"duration: %.2fs", animDuration); cx.label(x,y,w,th.rowH,b,th.textDim);
    }
    void drawPhysicsTab(float x, float y, float w) {
        auto& th=cx.th; float rh=th.rowH;
        // ── Player simulator + respawn kill-floor ──
        cx.label(x,y,w,rh,"Player simulator",th.text); y+=rh+2*uiScale;
        if (cx.button(ui::hashId("simgo"), x, y, w, rh+2*uiScale, playSim?"Stop walking  (P)":"Walk the env  (P)", playSim)) { if(playSim) stopSim(); else startSim(); } y+=rh+6*uiScale;
        cx.checkbox(ui::hashId("rsen"), x, y, "Respawn kill-floor (fall below = respawn)", hasRespawn); y+=rh+2*uiScale;
        cx.label(x,y,90*uiScale,rh,"Respawn Y",th.textDim); cx.dragFloat(ui::hashId("rsy"), x+92*uiScale, y, w-92*uiScale, rh, respawnY, 0.05f); y+=rh+10*uiScale;
        dl.rect(x,y,w,1,th.splitLine); y+=6*uiScale;
        cx.label(x,y,w,rh,"Navmesh / walkable collider",th.text); y+=rh+2*uiScale;
        cx.label(x,y,w,rh*0.9f,"Cooks to a Meta ColliderMesh (PhysX SEBD).",th.textDim); y+=rh*0.85f;
        cx.label(x,y,w,rh*0.9f,"Toggle the Navmesh eye (Scene tab) to see it.",th.textDim); y+=rh+8*uiScale;
        cx.label(x,y,w,rh,"Add a navmesh:",th.text); y+=rh+3*uiScale;
        if (cx.button(ui::hashId("navflat"),  x, y, w, rh+2*uiScale, "Flat  -  one ground plane")) addNavmesh(0); y+=rh+8*uiScale;
        if (cx.button(ui::hashId("navsmart"), x, y, w, rh+2*uiScale, "Smart  -  walkable faces of the scene")) addNavmesh(1); y+=rh+8*uiScale;
        { bool hasSel=!sel.empty(); char b[64]; snprintf(b,sizeof b,"From selection  -  %d mesh%s", (int)sel.size(), sel.size()==1?"":"es");
          if (cx.button(ui::hashId("navsel"), x, y, w, rh+2*uiScale, b, hasSel) && hasSel) addNavmesh(2); } y+=rh+12*uiScale;
        int n=0; for (auto& it:items) if (it.type==sitem::NAVMESH) n++;
        char hb[48]; snprintf(hb,sizeof hb,"Navmeshes in scene: %d", n); cx.label(x,y,w,rh,hb,th.text); y+=rh+3*uiScale;
        for (int i=0;i<(int)items.size();++i){ auto& it=items[i]; if(it.type!=sitem::NAVMESH) continue;
            bool seld=(i==selItem); char b[110]; snprintf(b,sizeof b,"%s  [%d tris]", it.name.c_str(), (int)(it.navIdx.size()/3));
            bool vis=!it.hidden; if (eyeToggle(x+9*uiScale, y+rh*0.5f, vis)) it.hidden=!vis;   // per-item visibility eye (marker + gizmo)
            if (cx.button(ui::hashId(7800u+(unsigned)i, 5u), x+22*uiScale, y, w-80*uiScale, rh, b, seld)) { deselectAll(); selItem=i; tab=TAB_OBJECT; }
            if (cx.button(ui::hashId(7900u+(unsigned)i, 5u), x+w-56*uiScale, y, 54*uiScale, rh, "Delete")) { items.erase(items.begin()+i); if(selItem==i)selItem=-1; else if(selItem>i)selItem--; --i; }
            y+=rh+2*uiScale;
        }
    }

    // ── Cook / Export panel: package name, auto-sign + spoof toggles, Cook button, live progress, status ──
    void drawCookPanel(float x, float y, float w) {
        auto& th=cx.th;
        if (adbDlDone.exchange(false)) { adbWorks(true); adbDevScanned=false; }
        ensureAdbAsync();                    // installing needs adb — auto-grab it (once, background) if missing
        float y0;
        cx.label(x,y,w,th.rowH,"Cook to bootable Quest APK",th.text); y+=th.rowH+6*uiScale;
        y0=y; cx.label(x,y,90*uiScale,th.rowH,"Package",th.textDim);
        cx.textField(ui::hashId("cookpkg"), x+92*uiScale, y, w-92*uiScale, th.rowH, cookPkg);
        cx.tip(x,y0,w,th.rowH,"Android package id for the UNSPOOFED (rooted) APK,\ne.g. com.environment.outerwilds.\nThe haven2025 spoof always uses Meta's haven2025\npackage and ignores this field."); y+=th.rowH+4*uiScale;
        y0=y; cx.checkbox(ui::hashId("autosign"), x, y, "Auto-sign (zipalign + apksigner)", autoSign);
        cx.tip(x,y0,w,th.rowH,"Sign the APKs so the Quest will install them (unsigned ->\nINSTALL_PARSE_FAILED_NO_CERTIFICATES). Build-tools are\nauto-detected, or auto-downloaded beside the exe on first\nuse (pre-fetch with --fetch-tools). Keep this ON."); y+=th.rowH;
        { int js=javaState.load(); const char* jt = js==1 ? "Java: ready (auto-installed if it was missing)" : js==2 ? "Java: auto-install failed - retries on cook, or install a JDK" : "Java: installing runtime in background...";
          cx.label(x+14*uiScale, y, w, th.rowH, jt, js==2?ui::rgba(230,160,80):th.textDim); y+=th.rowH; }
        y0=y; cx.checkbox(ui::hashId("spoof"), x, y, "Emit haven2025 spoof (no-root install)", spoofHaven);
        cx.tip(x,y0,w,th.rowH,"Also build <env>_NoRoot-Spoof.apk, which masquerades as\nMeta's haven2025 home. This is the ONLY way to install on a\nNON-rooted Quest: it replaces haven2025, then you pick\n\"Haven 2025\" in the home menu. Keep this ON."); y+=th.rowH;
        y0=y; cx.checkbox(ui::hashId("hzanim"), x, y, "Animate skinned meshes (HZANIM - EXPERIMENTAL)", animSkinned);
        cx.tip(x,y0,w,th.rowH,"Emit skeletal animation for skinned meshes (clouds/koi/\ndroids). EXPERIMENTAL: the clip cook can still crash the\nenvironment on the device. Leave OFF unless testing."); y+=th.rowH+6*uiScale;
        y0=y; cx.checkbox(ui::hashId("nocull"), x, y, "Draw everything - disable culling (fixes clipping, V79-style)", noCull);
        cx.tip(x,y0,w,th.rowH,"Emit scene-spanning bounds so the V205 shell NEVER culls/clips\nyour meshes (frustum + Hi-Z occlusion + distance + CLOD budget).\nThe old V79 shell had NO environment culler, so this matches how\nold homes looked. Geometry still sits at its real position; only\nculling is defeated. Trades the Quest's culling perf for full\nvisibility. Keep ON if cooked homes clip / disappear at distance."); y+=th.rowH+6*uiScale;
        y0=y; cx.checkbox(ui::hashId("cookaudio"), x, y, "Ship background audio loop", cookAudio);
        y+=cx.th.rowH+2*uiScale; cx.checkbox(ui::hashId("cookautofloor"), x, y, "Auto floor collision (no Navmesh item)", cookAutoFloor);
        cx.tip(x,y0,w,th.rowH,"Bake the environment's background audio loop into the cooked APK\n(FMOD asset placed at the spawn). Turn OFF for a silent home."); y+=th.rowH+2*uiScale;
        y0=y; cx.checkbox(ui::hashId("killvistas"), x, y, "Neutralize vistas (install an invisible vista over each)", killVistas);
        cx.tip(x,y0,w,th.rowH,"After installing the spoof, replace EVERY installed vista package\n(com.meta.shell.env.vista.*) with an INVISIBLE 0-mesh environment.\nSo if the shell force-pairs a vista behind your home, it renders\nNOTHING. Only /data (updatable) vistas can be replaced without root;\na system-app vista is reported and left as-is. Keep ON if a vista\nstill shows behind your ported home."); y+=th.rowH+2*uiScale;
        // ── the AUDIO COOKER UI: shows what will ship + Replace/Add/Export/Revert (backend above: setAudioFromFile etc.) ──
        { if(audioInfo.empty() && !bgOgg.empty()) refreshAudioInfo();
          std::string al = bgOgg.empty() ? "  audio: none - Add one below"
                                         : ("  audio: "+audioInfo+(audioOvrPath.empty()?"  (env's own)":"  (REPLACED)"));
          cx.label(x+14*uiScale, y, w-14*uiScale, th.rowH*0.9f, al.c_str(), th.textDim); y+=th.rowH*0.95f;
          float bw3=(w-14*uiScale-8*uiScale)/3.f, bx3=x+14*uiScale;
          y0=y;
          if (cx.button(ui::hashId("audrepl"), bx3, y, bw3, th.rowH, bgOgg.empty()?"Add audio...":"Replace...", true)) {
              std::string p=pickFileWin32(L"Choose background audio (OGG / WAV / MP3 / FLAC)", L"Audio (*.ogg;*.wav;*.mp3;*.flac)", L"*.ogg;*.wav;*.mp3;*.flac");
              if(!p.empty() && setAudioFromFile(p)) cookAudio=true;   // adding/replacing audio implies you want it shipped
          }
          if (cx.button(ui::hashId("audexp"), bx3+bw3+4*uiScale, y, bw3, th.rowH, "Export")) exportAudio();
          if (cx.button(ui::hashId("audrev"), bx3+2*(bw3+4*uiScale), y, bw3, th.rowH, "Revert to env")) clearAudioOverride();
          cx.tip(x,y0,w,th.rowH,"Replace (or ADD, for a silent env) the background loop from ANY\nogg/wav/mp3/flac file - previews immediately on the PC and ships\nin the cook (FMOD-native formats raw; flac transcodes to WAV).\nExport writes the current loop next to the session in saved/.\nRevert restores the env's own theme. Persists in the session."); y+=th.rowH+6*uiScale;
        }
        // (PC preview-audio toggle moved to the viewport header strip — always visible, not buried here.)
        y0=y; cx.checkbox(ui::hashId("solidcol"), x, y, "Solid wall collision (trimesh)", solidCollision);
        cx.tip(x,y0,w,th.rowH,"Cook a REAL double-sided triangle-mesh collider for the whole env -\nwalk on floors AND get blocked by walls/columns, enter rooms through\ndoorways (haven2025's cooked-PhysX SEBD format, device-verified).\nOFF = a floor-only ColliderBox grid (walkable but you phase walls)."); y+=th.rowH+6*uiScale;
        if (solidCollision != prevSolidCol) { prevSolidCol = solidCollision; bakeNavmeshes(items); }   // re-bake so the gizmo shows floor+walls (on) / floor-only (off)
        // (thick all-sides voxel collision is REVERTED to OFF — it walled off room interiors; HSR_VOXSOLID re-enables it)
        g_audioMuted.store(!previewAudio, std::memory_order_relaxed);   // bind the toggle to the live audio-callback mute flag
        y0=y; cx.checkbox(ui::hashId("skybox"), x, y, "Far backdrop -> skybox (escapes the 5000 far-clip dome)", skybox);
        cx.tip(x,y0,w,th.rowH,"Route distant geometry (centroid > the meters below) to the\nSkyboxPlatformComponent pass, which is EXEMPT from the shell's\nhard PortalStereoCamera far=5000 clip (the black dome locked to\nyour head). This is the ONLY way official homes/vistas show km-\ndistant scenery - they skybox it, they do NOT use a bigger far.\nThe backdrop becomes camera-locked (no walk-up parallax), which\nis imperceptible at km range. Near/mid geometry stays walkable."); y+=th.rowH+2*uiScale;
        if (skybox) { y0=y; cx.label(x,y,150*uiScale,th.rowH,"  skybox distance (m)",th.textDim);
            char sdb[32]; snprintf(sdb,sizeof sdb,"%.0f",skyboxDist); std::string sds=sdb;
            cx.textField(ui::hashId("skyboxdist"), x+152*uiScale, y, w-152*uiScale, th.rowH, sds);
            skyboxDist=(float)atof(sds.c_str()); if(skyboxDist<1.f)skyboxDist=1500.f;
            cx.tip(x,y0,w,th.rowH,"Meshes whose centroid is farther than this from the origin are\ndrawn as skybox (camera-locked, far-clip-exempt). Lower = more\ngeometry skyboxed. cyberhome's city skyline sits ~3-7 km out."); y+=th.rowH+6*uiScale; }
        else y+=4*uiScale;
        // ── HSL render config (distance fog + far clip): WYSIWYG — the preview applies the SAME values the cook ships ──
        y0=y; cx.checkbox(ui::hashId("cfgfog"), x, y, "Distance fog (preview + cook)", cfgFog);
        cx.tip(x,y0,w,th.rowH,"Emit a ScenePlatformComponent distance fog AND show it live in the\npreview. Tune by eye; the preview closely approximates the device\n(confirm on the headset)."); y+=th.rowH+2*uiScale;
        if (cfgFog) {
            char fb[48];
            y0=y; cx.label(x,y,150*uiScale,th.rowH,"  fog color r,g,b",th.textDim);
            snprintf(fb,sizeof fb,"%.3f,%.3f,%.3f",cfgFogColor[0],cfgFogColor[1],cfgFogColor[2]); std::string fcs=fb;
            cx.textField(ui::hashId("fogcol"), x+152*uiScale, y, w-152*uiScale, th.rowH, fcs);
            { float a,b,c; if (sscanf(fcs.c_str(),"%f,%f,%f",&a,&b,&c)==3){cfgFogColor[0]=a;cfgFogColor[1]=b;cfgFogColor[2]=c;} } y+=th.rowH+2*uiScale;
            y0=y; cx.label(x,y,150*uiScale,th.rowH,"  fog start (m)",th.textDim);
            snprintf(fb,sizeof fb,"%.1f",cfgFogStart); std::string fss=fb;
            cx.textField(ui::hashId("fogstart"), x+152*uiScale, y, w-152*uiScale, th.rowH, fss); cfgFogStart=(float)atof(fss.c_str()); if(cfgFogStart<0)cfgFogStart=0; y+=th.rowH+2*uiScale;
            y0=y; cx.label(x,y,150*uiScale,th.rowH,"  fog density",th.textDim);
            snprintf(fb,sizeof fb,"%.4f",cfgFogDensity); std::string fds=fb;
            cx.textField(ui::hashId("fogdens"), x+152*uiScale, y, w-152*uiScale, th.rowH, fds); cfgFogDensity=(float)atof(fds.c_str()); if(cfgFogDensity<0)cfgFogDensity=0; y+=th.rowH+6*uiScale;
        }
        { y0=y; cx.label(x,y,150*uiScale,th.rowH,"Far clip (m)",th.textDim);
          char fcb[32]; snprintf(fcb,sizeof fcb,"%.0f",cfgFar); std::string fcss=fcb;
          cx.textField(ui::hashId("cfgfar"), x+152*uiScale, y, w-152*uiScale, th.rowH, fcss); cfgFar=(float)atof(fcss.c_str()); if(cfgFar<1.f)cfgFar=150000.f;
          cx.tip(x,y0,w,th.rowH,"ScenePlatformComponent farClippingPlane. The device default is 5000m;\nthe cook extends it to this. Use the viewport 'Far-clip' overlay to\nsee the 5000m default boundary."); y+=th.rowH+6*uiScale; }
        // (fog + far-clip WYSIWYG binding moved to buildFrame — applies EVERY frame, not just while this tab is open)
        // ── Install to headset (USB or Wi-Fi adb); the installer auto-detects root and picks spoofed vs unspoofed ──
        y0=y; cx.checkbox(ui::hashId("install"), x, y, "Install to headset after cook (auto)", installAfterCook);
        cx.tip(x,y0,w,th.rowH,"After cooking, install over adb. The installer detects root:\n  ROOT  -> install the UNSPOOFED APK + auto-select it.\n  NO root-> back up the real haven2025, install the SPOOF,\n           and relaunch the shell. The spoof REPLACES Haven 2025\n           in place (unrooted Quests can't switch envs).\nNeeds adb bundled beside the exe or on PATH."); y+=th.rowH+2*uiScale;
        { y0=y; const char* srN[3] = { "Shell restart after install: AUTO (only if no root)", "Shell restart after install: ALWAYS", "Shell restart after install: NEVER" };
          if (cx.button(ui::hashId("shellrestart"), x, y, w, th.rowH, srN[shellRestart%3])) shellRestart = (shellRestart+1)%3;
          cx.tip(x,y0,w,th.rowH,"Whether to kill/relaunch vrshell after installing, so it picks up\nthe new env. ROOTED headsets hot-swap via the environment_selected\nIPC (no restart needed); UNROOTED ones need the restart (nothing\nelse reloads the replaced Haven 2025). Click to cycle."); y+=th.rowH+2*uiScale; }
        y0=y; cx.label(x,y,64*uiScale,th.rowH,"Wi-Fi IP",th.textDim);
        cx.textField(ui::hashId("wifiip"), x+66*uiScale, y, w-66*uiScale-70*uiScale, th.rowH, wifiIp);
        if (cx.button(ui::hashId("wificon"), x+w-68*uiScale, y, 66*uiScale, th.rowH, "Connect")) wifiConnect();
        cx.tip(x,y0,w,th.rowH,"Wireless adb: type the headset IP (e.g. 192.168.1.35) and\nConnect. Enable Wi-Fi adb on the headset first.\nLeave blank to use a USB cable."); y+=th.rowH+2*uiScale;
        // ── device PICKER (several Quests attached): click to CYCLE; Refresh re-scans `adb devices` ──
        if (!adbDevScanned) refreshAdbDevices();
        y0=y; cx.label(x,y,64*uiScale,th.rowH,"Device",th.textDim);
        float rbw=64*uiScale;
        if (cx.button(ui::hashId("devpick"), x+66*uiScale, y, w-66*uiScale-rbw-4*uiScale, th.rowH, adbDeviceLabel().c_str())) cycleAdbDevice();
        if (cx.button(ui::hashId("devref"), x+w-rbw, y, rbw, th.rowH, "Refresh")) { refreshAdbDevices(); setStatus("adb: "+std::to_string(adbDeviceList.size())+" device(s) attached."); }
        cx.tip(x,y0,w,th.rowH,"Pick which attached Quest to target when several are connected.\nClick the name to CYCLE devices; Refresh re-scans `adb devices`.\n\"(default / only device)\" = unset (uses the single attached one)."); y+=th.rowH;
        cx.label(x,y,w,th.rowH*0.85f,"USB: leave blank. Wi-Fi: type IP, Connect.",th.textDim); y+=th.rowH*0.95f+6*uiScale;
        bool busy = cooking.load();
        if (busy) { cx.progressBar(x, y, w, th.rowH+2*uiScale, cookProg.load(), stageStr().c_str()); }
        else { y0=y; if (cx.button(ui::hashId("cookgo"), x, y, w, th.rowH+4*uiScale, installAfterCook?"COOK + SIGN + INSTALL":"COOK  +  SIGN", true)) startCook();
               cx.tip(x,y0,w,th.rowH+4*uiScale,"Cook the edited scene to APK(s), sign them, and (if Install\nis on) push to the headset. Outputs land next to the loaded\nenv:  <env>_Rooted-System.apk  +  <env>_NoRoot-Spoof.apk"); }
        y += th.rowH+8*uiScale;
        // ── INSTALL ONLY — skip the cook if the scene is UNCHANGED since the last cook (installs the existing
        //    APK). Shows whether the cooked APK is up to date / stale / missing so you know if a re-cook is needed.
        if (!busy) {
            bool hSys=false,hSpoof=false; int st=cookState(hSys,hSpoof);
            const char* stTxt = st==0? "cooked APK is UP TO DATE (unchanged)" : st==1? "scene CHANGED since last cook (stale) - re-cook" : "no cooked APK yet - cook first";
            uint32_t stCol = st==0? ui::rgba(120,220,140) : st==1? ui::rgba(240,200,110) : th.textDim;
            cx.label(x,y,w,th.rowH*0.9f, stTxt, stCol); y+=th.rowH*0.9f+2*uiScale;
            y0=y; bool canInstall = st==0;
            if (cx.button(ui::hashId("installonly"), x, y, w, th.rowH+2*uiScale, "INSTALL ONLY  (no re-cook if unchanged)", canInstall)) installOnly();
            cx.tip(x,y0,w,th.rowH+2*uiScale,"Install the already-cooked APK WITHOUT re-cooking - but only if\nnothing changed since it was cooked (a .cooksig hash of the scene +\ncook settings + source env is compared). If the scene changed, it\ntells you to re-cook instead of pushing a stale env.");
            y += th.rowH+10*uiScale;
        }
        // Undo a spoof: put the REAL Haven 2025 back from the auto-backup (off the UI thread).
        if (!busy && !restoring.load()) {
            y0=y; if (cx.button(ui::hashId("restorehaven"), x, y, w, th.rowH, "Restore original Haven 2025")) {
                if (restoreThread.joinable()) restoreThread.join();
                restoring.store(true);
                restoreThread = std::thread([this]{ restoreHaven(); restoring.store(false); });
            }
            cx.tip(x,y0,w,th.rowH,"Reinstall the ORIGINAL Haven 2025 from the auto-backup\n(folder \"Haven2025_Backup\" beside the exe) + relaunch the shell.\nUse this to undo a spoof install."); y += th.rowH+3*uiScale;
            // Manual uninstall: rip out whatever Haven 2025 is on the headset (your spoof, or a different Meta version).
            y0=y; if (cx.button(ui::hashId("uninsthaven"), x, y, w, th.rowH, "Uninstall Haven 2025 (remove the spoof)")) {
                if (restoreThread.joinable()) restoreThread.join();
                restoring.store(true);
                restoreThread = std::thread([this]{ uninstallHaven(); restoring.store(false); });
            }
            cx.tip(x,y0,w,th.rowH,"Uninstall whatever Haven 2025 is installed on the headset (your spoof,\nor a different Meta version) - no root needed, if it's a store app.\nDetects the installed version first. Then \"Restore original Haven 2025\"\nputs Meta's back, or just re-cook + install."); y += th.rowH+3*uiScale;
            // Standalone vista-neutralize (no re-cook): install an invisible vista over every installed vista package.
            y0=y; if (cx.button(ui::hashId("killvistabtn"), x, y, w, th.rowH, "Neutralize vistas now (invisible vista over each)")) neutralizeVistasButton();
            cx.tip(x,y0,w,th.rowH,"Right now (no re-cook): replace every installed com.meta.shell.env.vista.*\npackage with an INVISIBLE 0-mesh environment, so a force-paired vista\nrenders nothing. Use this if a vista shows behind your ported home.\nUpdatable vistas are replaced without root; system-app vistas are\nreported (need root). Restore a vista by reinstalling it from the store."); y += th.rowH+8*uiScale;
        } else if (restoring.load()) {
            // live progress bar (vista-neutralize drives setStage per vista; restore/uninstall just show "working")
            if (vistaBusy.load()) cx.progressBar(x, y, w, th.rowH+2*uiScale, cookProg.load(), stageStr().c_str());
            else cx.label(x,y,w,th.rowH,"Working on Haven 2025...",th.textDim);
            y += th.rowH+8*uiScale;
        }
        // PREVIEW THE COOK IN-PLACE: swap this SAME window to the freshest cooked APK, rendered the
        // HSL/V203 way (the closest desktop match of the device) - and swap BACK to the source, no restart ever.
        if (!busy) {
            std::string newest; std::filesystem::file_time_type newestT{};
            std::error_code ec;
            for (auto& de : std::filesystem::directory_iterator(cookOutPath(), ec)) {
                if (de.path().extension() != ".apk") continue;
                auto t = std::filesystem::last_write_time(de, ec);
                if (newest.empty() || t > newestT) { newest = de.path().string(); newestT = t; }
            }
            bool onCooked = !sourceEnvPath.empty() && projectPath != sourceEnvPath;
            if (!newest.empty() && !onCooked) {
                std::string bn = newest; size_t sl = bn.find_last_of("/\\"); if (sl != std::string::npos) bn = bn.substr(sl+1);
                y0=y; if (cx.button(ui::hashId("cookprev"), x, y, w, th.rowH+2*uiScale, ("Preview cooked (HSL): "+bn).c_str(), true))
                    swapTo = newest;   // main swaps the scene IN THIS WINDOW (no new process)
                cx.tip(x,y0,w,th.rowH+2*uiScale,"Reload THIS window with the newest cooked APK, rendered the\nHSL/V203 way (the closest desktop match of the device). 'Back to source'\nthen returns to this env - no restart, same window.");
                y += th.rowH+8*uiScale;
            }
            if (onCooked) {
                std::string sb = sourceEnvPath; size_t sl = sb.find_last_of("/\\"); if (sl != std::string::npos) sb = sb.substr(sl+1);
                y0=y; if (cx.button(ui::hashId("cookback"), x, y, w, th.rowH+2*uiScale, ("Back to source: "+sb).c_str(), true))
                    swapTo = sourceEnvPath;
                cx.tip(x,y0,w,th.rowH+2*uiScale,"Swap this window back to the ORIGINAL source env (OPA/V79/glTF\nmode) - same window, no restart.");
                y += th.rowH+8*uiScale;
            }
        }
        std::string st; { std::lock_guard<std::mutex> l(statusMx); st = cookStatus; }
        if (!st.empty()) {
            // word-ish wrap into the panel width
            float ly=y; size_t i=0; while (i<st.size()) { size_t take=std::min<size_t>(st.size()-i, (size_t)(w/ (7*uiScale)) ); cx.label(x, ly, w, th.rowH, st.substr(i,take).c_str(), th.textDim); i+=take; ly+=th.rowH*0.9f; }
        }
    }
    std::string stageStr() { std::lock_guard<std::mutex> l(statusMx); char b[80]; snprintf(b,sizeof b,"%s  %d%%", cookStage.c_str(), (int)(cookProg.load()*100)); return b; }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  CUSTOM GIZMO  (move/rotate/scale; projected handles drawn via ui_draw, ray-screen drag)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void quatRotVec(const float q[4], const float v[3], float o[3]) {   // o = q * v
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z), r02=2*(x*z+w*y);
        float r10=2*(x*y+w*z), r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
        float r20=2*(x*z-w*y), r21=2*(y*z+w*x), r22=1-2*(x*x+y*y);
        o[0]=r00*v[0]+r01*v[1]+r02*v[2]; o[1]=r10*v[0]+r11*v[1]+r12*v[2]; o[2]=r20*v[0]+r21*v[1]+r22*v[2];
    }
    bool worldToScreen(const float wpt[3], float& sx, float& sy) {
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        float c0=vp[0]*wpt[0]+vp[4]*wpt[1]+vp[8]*wpt[2]+vp[12];
        float c1=vp[1]*wpt[0]+vp[5]*wpt[1]+vp[9]*wpt[2]+vp[13];
        float c3=vp[3]*wpt[0]+vp[7]*wpt[1]+vp[11]*wpt[2]+vp[15];
        if (c3 <= 1e-5f) return false;
        float ndcx=c0/c3, ndcy=c1/c3;
        sx = rcViewport.offset.x + (ndcx*0.5f+0.5f)*rcViewport.extent.width;
        sy = rcViewport.offset.y + (ndcy*0.5f+0.5f)*rcViewport.extent.height;
        return true;
    }
    // Rubber-band: collect visible meshes whose projected centroid is inside the rect.
    //  Shift+box = ADD them.  Ctrl+box = SMART TOGGLE: if the whole boxed set is already selected, DESELECT it
    //  (re-box a range to unpick it); otherwise add the missing ones.
    void boxSelectRect(float ax, float ay, float bx, float by, bool ctrlToggle) {
        if (!r) return;
        float x0=std::min(ax,bx), x1=std::max(ax,bx), y0=std::min(ay,by), y1=std::max(ay,by);
        std::vector<int> hits;
        for (int i=0;i<(int)r->gpuMeshes.size();++i) {
            // deleted meshes MUST be unselectable here: box-selecting one and pressing Del made
            // toggleDeleteSelected see "all selected are deleted" and RESTORE them (the ghost comeback)
            if (r->isHidden(i) || r->isDeleted((size_t)i)) continue;
            VkGpuMesh& gm=r->gpuMeshes[i]; const auto& P=gm.pickPos; if (P.size()<3) continue;
            double c[3]={0,0,0}; size_t nv=P.size()/3, step=nv>512?nv/512:1, cnt=0;
            for (size_t v=0; v<nv; v+=step){ float w[3]; xformPoint(gm.model,&P[v*3],w); c[0]+=w[0];c[1]+=w[1];c[2]+=w[2]; cnt++; }
            if(!cnt) continue;
            float wc[3]={(float)(c[0]/cnt),(float)(c[1]/cnt),(float)(c[2]/cnt)}, s[2];
            if (!worldToScreen(wc,s[0],s[1])) continue;
            if (s[0]>=x0&&s[0]<=x1&&s[1]>=y0&&s[1]<=y1) hits.push_back(i);
        }
        int delta=0;
        bool allSel = !hits.empty(); for (int h:hits) if (!inSel(h)) { allSel=false; break; }
        if (ctrlToggle && allSel) { for (int h:hits) { toggleSel(h); delta--; } }   // re-boxed an already-selected range -> unpick it
        else { for (int h:hits) if (!inSel(h)) { sel.push_back(h); delta++; } }       // add the ones not yet selected
        selected = sel.empty()?-1:sel.back(); r->selectedMesh=selected; selItem=-1;
        setStatus("Box "+std::string(delta<0?"-":"+")+std::to_string(delta<0?-delta:delta)+" ("+std::to_string(sel.size())+" selected). Right-click -> Make skybox backdrop.");
    }
    // Draw a world-space line into the overlay, NEAR-CLIPPED in clip space. Without this, an edge with ONE endpoint
    // behind the camera was dropped entirely (worldToScreen returned false) -> when a camera tilt/rotation put part of
    // a surrounding mesh behind the eye, whole wireframe edges vanished. Now we clip the edge to the near plane and
    // still draw its visible part. vp = proj*view (computed once per mesh).
    void wireLine(const float vp[16], const float* wa, const float* wb, uint32_t col, float th) {
        float ax=vp[0]*wa[0]+vp[4]*wa[1]+vp[8]*wa[2]+vp[12], ay=vp[1]*wa[0]+vp[5]*wa[1]+vp[9]*wa[2]+vp[13], aw=vp[3]*wa[0]+vp[7]*wa[1]+vp[11]*wa[2]+vp[15];
        float bx=vp[0]*wb[0]+vp[4]*wb[1]+vp[8]*wb[2]+vp[12], by=vp[1]*wb[0]+vp[5]*wb[1]+vp[9]*wb[2]+vp[13], bw=vp[3]*wb[0]+vp[7]*wb[1]+vp[11]*wb[2]+vp[15];
        const float eps=1e-4f;
        if (aw<=eps && bw<=eps) return;                                         // both behind near -> nothing visible
        if (aw<=eps) { float t=(eps-aw)/(bw-aw); ax+=(bx-ax)*t; ay+=(by-ay)*t; aw=eps; }   // clip endpoint A to the near plane
        else if (bw<=eps) { float t=(eps-bw)/(aw-bw); bx+=(ax-bx)*t; by+=(ay-by)*t; bw=eps; }  // clip endpoint B
        float sax=rcViewport.offset.x+((ax/aw)*0.5f+0.5f)*rcViewport.extent.width, say=rcViewport.offset.y+((ay/aw)*0.5f+0.5f)*rcViewport.extent.height;
        float sbx=rcViewport.offset.x+((bx/bw)*0.5f+0.5f)*rcViewport.extent.width, sby=rcViewport.offset.y+((by/bw)*0.5f+0.5f)*rcViewport.extent.height;
        dl.line(sax,say,sbx,sby,col,th);
    }
    // Highlight the selected mesh's actual TRIANGLES (projected edges). Capped/sampled so huge meshes stay cheap.
    void drawWireframe(VkGpuMesh& gm){
        const auto& P=gm.pickPos; const auto& I=gm.pickIdx;
        if (P.empty() || I.size()<3) { drawAabbBox(gm); return; }            // no CPU geometry -> fall back to a box
        size_t ntri=I.size()/3, maxTri=2500, stride = ntri>maxTri ? ntri/maxTri : 1;
        uint32_t c=ui::rgba(255,170,60,70);
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        for (size_t t=0; t<ntri; t+=stride){
            uint32_t a=I[t*3],b=I[t*3+1],d=I[t*3+2];
            if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)d*3+2>=P.size()) continue;
            float wa[3],wb[3],wd[3];
            xformPoint(gm.model,&P[a*3],wa); xformPoint(gm.model,&P[b*3],wb); xformPoint(gm.model,&P[d*3],wd);
            wireLine(vp,wa,wb,c,0.7f); wireLine(vp,wb,wd,c,0.7f); wireLine(vp,wd,wa,c,0.7f);
        }
    }
    void drawAabbBox(VkGpuMesh& gm){
        float mn[3],mx[3]; worldAabb(gm,mn,mx);
        float w[8][3]; for (int c=0;c<8;c++){ w[c][0]=(c&1)?mx[0]:mn[0]; w[c][1]=(c&2)?mx[1]:mn[1]; w[c][2]=(c&4)?mx[2]:mn[2]; }
        static const int E[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        uint32_t c=ui::rgba(255,150,40,160);
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        for (auto& e:E) wireLine(vp, w[e[0]], w[e[1]], c, 1.f);   // near-clipped (don't vanish when a corner is behind the camera)
    }
    // True if a world point is HIDDEN behind scene geometry from the camera (CPU ray test — the 2D overlay has no GPU
    // depth buffer to test against). Used to dim collider/wall edges that sit behind a mesh so depth reads correctly.
    bool occludedPoint(const float wp[3]){
        if(!r||r->gpuMeshes.empty()) return false;
        float O[3]={r->cam.pos[0],r->cam.pos[1],r->cam.pos[2]};
        float D[3]={wp[0]-O[0],wp[1]-O[1],wp[2]-O[2]}; float dist=std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]); if(dist<1e-3f) return false;
        D[0]/=dist;D[1]/=dist;D[2]/=dist; float lim=dist-0.06f;   // bias so the box's own contact surface doesn't self-occlude
        for(int i=0;i<(int)r->gpuMeshes.size();++i){ if(r->isHidden(i))continue; auto&gm=r->gpuMeshes[i]; if(isBackdrop(gm.name))continue;
            float mn[3],mx[3]; worldAabb(gm,mn,mx); float ta; if(!rayAabb(O,D,mn,mx,ta))continue; if(ta>lim)continue;
            const auto&P=gm.pickPos; const auto&I=gm.pickIdx; if(P.empty()||I.size()<3)continue;
            for(size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size())continue;
                float w0[3],w1[3],w2[3]; xformPoint(gm.model,&P[a*3],w0);xformPoint(gm.model,&P[b*3],w1);xformPoint(gm.model,&P[c*3],w2);
                float t; if(rayTri(O,D,w0,w1,w2,t)&&t<lim) return true; } }
        return false;
    }
    // oriented wireframe box (pos + R*half corners) — colliders / chairs / wall-placement zones. Edges are NEAR-CLIPPED
    // (wireLine) so the box never vanishes when you get close and a corner crosses behind the eye. occludeAware (the
    // SELECTED item only — the CPU ray test is too costly for every box) dims edges hidden behind geometry as a depth cue.
    void drawBox(const float pos[3], const float half[3], const float q[4], uint32_t col, float thick, bool occludeAware=false){
        float w[8][3]; bool occ[8]={};
        for (int c=0;c<8;c++){ float lc[3]={ (c&1)?half[0]:-half[0], (c&2)?half[1]:-half[1], (c&4)?half[2]:-half[2] }, wl[3]; quatRotVec(q,lc,wl);
            w[c][0]=wl[0]+pos[0]; w[c][1]=wl[1]+pos[1]; w[c][2]=wl[2]+pos[2]; if(occludeAware) occ[c]=occludedPoint(w[c]); }
        static const int E[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        uint32_t dim=ui::withA(col,70);                                        // hidden behind geometry -> faint
        for (auto& e:E){ uint32_t c=(occludeAware&&occ[e[0]]&&occ[e[1]])?dim:col; wireLine(vp, w[e[0]], w[e[1]], c, thick); }
    }
    // forward (-Z) facing arrow — shows an item's orientation so ROTATING any component is visible (not just spawns)
    void drawFacingArrow(const float pos[3], const float q[4], uint32_t col, float thick, float lenW){
        float fwd[3]; { float f[3]={0,0,-1}; quatRotVec(q,f,fwd); }
        float s0[2]; if(!worldToScreen(pos,s0[0],s0[1])) return;
        float tp[3]={pos[0]+fwd[0]*lenW, pos[1]+fwd[1]*lenW, pos[2]+fwd[2]*lenW}, ts[2];
        if(!worldToScreen(tp,ts[0],ts[1])) return;
        dl.line(s0[0],s0[1],ts[0],ts[1],col,thick+0.4f);
        float dx=ts[0]-s0[0],dy=ts[1]-s0[1],l=std::sqrt(dx*dx+dy*dy); if(l<1e-3f) return; dx/=l;dy/=l; float px=-dy,py=dx,al=8*uiScale;
        dl.line(ts[0],ts[1], ts[0]-dx*al+px*al*0.5f, ts[1]-dy*al+py*al*0.5f, col,thick);
        dl.line(ts[0],ts[1], ts[0]-dx*al-px*al*0.5f, ts[1]-dy*al-py*al*0.5f, col,thick);
    }
    // Draw all scene-item markers (spawn cones / chair+exit / collider+wall boxes / hotspot rings / navmesh).
    bool exitHVis=false; float exitHS[2]={0,0}; bool exitDrag=false;   // chair exit-position smart handle
    bool editExit=false;   // when a chair is selected: retarget the MAIN gizmo to its exit point (full X/Y/Z gizmo, not a square)
    void drawItems(){
        if (showFarClip) {   // FAR-CLIP boundary on camera (HSL view): the device's SceneComponent DEFAULT far is 5000m — geometry
            // beyond this red sphere renders ONLY if the cook's ScenePlatformComponent far-extension applies on device (else it
            // clips here). Backdrops past it -> mark SkyboxPlatformComponent (far-exempt). 3 great circles @ origin (~spawn).
            VkRect2D vf=rcViewport; dl.pushClip((float)vf.offset.x,(float)vf.offset.y,(float)vf.extent.width,(float)vf.extent.height);
            const float R=5000.f; uint32_t fcc=ui::rgba(255,95,60,190);
            for (int pl=0;pl<3;pl++){ float pv[2]={0,0}; bool pk=false;
                for (int a=0;a<=64;a++){ float an=a/64.f*6.2831853f, cc=cosf(an)*R, sn=sinf(an)*R, wp[3];
                    if(pl==0){wp[0]=cc;wp[1]=sn;wp[2]=0;} else if(pl==1){wp[0]=cc;wp[1]=0;wp[2]=sn;} else {wp[0]=0;wp[1]=cc;wp[2]=sn;}
                    float sp[2]; bool ok=worldToScreen(wp,sp[0],sp[1]); if(pk&&ok) dl.line(pv[0],pv[1],sp[0],sp[1],fcc,1.4f); pv[0]=sp[0];pv[1]=sp[1];pk=ok; } }
            dl.popClip();
        }
        if (!showItems || items.empty()) return;
        exitHVis=false;
        VkRect2D v=rcViewport; dl.pushClip((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,(float)v.extent.height);
        for (int i=0;i<(int)items.size();++i){
            auto& it=items[i]; if (!itemShown(it)) continue;
            bool seld=(i==selItem); float th=seld?2.6f:1.5f; uint32_t col=typeColor(it.type, seld);
            float q[4]; eulerToQuat(it.rot,q);
            float s[2]; bool on=worldToScreen(it.pos,s[0],s[1]);
            switch (it.type){
              case sitem::SPAWN: { if (on){
                    float fwd[3]; { float f[3]={0,0,-1}; quatRotVec(q,f,fwd); }   // facing = -Z forward (Quest convention)
                    float rr=0.35f, prev[2]={0,0}; bool pok=false;                // ground ring (the spawn footprint)
                    for (int a2=0;a2<=10;a2++){ float ang=a2/10.f*6.2831853f, rp[3]={it.pos[0]+cosf(ang)*rr, it.pos[1], it.pos[2]+sinf(ang)*rr}, rs[2]; bool rok=worldToScreen(rp,rs[0],rs[1]); if(pok&&rok) dl.line(prev[0],prev[1],rs[0],rs[1],col,th); prev[0]=rs[0];prev[1]=rs[1];pok=rok; }
                    float tp[3]={it.pos[0]+fwd[0]*0.9f, it.pos[1], it.pos[2]+fwd[2]*0.9f}, ts2[2];   // facing arrow
                    if (worldToScreen(tp,ts2[0],ts2[1])){ dl.line(s[0],s[1],ts2[0],ts2[1],col,th+0.6f);
                        float dx=ts2[0]-s[0],dy=ts2[1]-s[1],l=std::sqrt(dx*dx+dy*dy); if(l>1e-3f){dx/=l;dy/=l; float px=-dy,py=dx,al=9*uiScale; dl.line(ts2[0],ts2[1], ts2[0]-dx*al+px*al*0.5f, ts2[1]-dy*al+py*al*0.5f,col,th); dl.line(ts2[0],ts2[1], ts2[0]-dx*al-px*al*0.5f, ts2[1]-dy*al-py*al*0.5f,col,th); } }
                    cx.textAligned(s[0]+10*uiScale,s[1]-8*uiScale,150*uiScale,16*uiScale, it.allowStart&&it.isLocal?"Spawn (local)":it.name.c_str(), col, 0);
                } break; }
              case sitem::BOXCOL: { float h[3]={it.half[0]*it.scale[0],it.half[1]*it.scale[1],it.half[2]*it.scale[2]}; drawBox(it.pos,h,q,col,th,seld); break; }
              case sitem::CHAIR: {   // CHAIR icon = seat quad + backrest (oriented to the sit facing) — a recognizable chair, not a tiny dot
                float sz=0.42f*((it.scale[0]+it.scale[2])*0.5f); if(sz<0.20f)sz=0.20f;
                auto lp=[&](float lx,float ly,float lz,float o[3]){ float vv[3]={lx,ly,lz},rr[3]; quatRotVec(q,vv,rr); o[0]=it.pos[0]+rr[0]; o[1]=it.pos[1]+rr[1]; o[2]=it.pos[2]+rr[2]; };
                auto ln=[&](const float* a,const float* b){ float as[2],bs[2]; if(worldToScreen(a,as[0],as[1])&&worldToScreen(b,bs[0],bs[1])) dl.line(as[0],as[1],bs[0],bs[1],col,th); };
                float c0[3],c1[3],c2[3],c3[3]; lp(-sz,0,-sz,c0); lp(sz,0,-sz,c1); lp(sz,0,sz,c2); lp(-sz,0,sz,c3);
                ln(c0,c1); ln(c1,c2); ln(c2,c3); ln(c3,c0);                 // seat (sit surface)
                float k0[3],k1[3],k2[3],k3[3]; lp(-sz,0,sz,k0); lp(sz,0,sz,k1); lp(sz,sz*1.7f,sz,k2); lp(-sz,sz*1.7f,sz,k3);
                ln(k0,k3); ln(k1,k2); ln(k2,k3);                            // backrest (rear uprights + top bar)
                drawFacingArrow(it.pos,q,col,th,0.6f);
                if (on) cx.textAligned(s[0]+10*uiScale,s[1]-8*uiScale,150*uiScale,16*uiScale, it.name.empty()?"Chair":it.name.c_str(), col, 0);
                float ep[3]={it.pos[0]+it.exitPos[0], it.pos[1]+it.exitPos[1], it.pos[2]+it.exitPos[2]}, es[2];   // draggable exit handle (ground)
                if (worldToScreen(ep,es[0],es[1])){ float hs=(seld?5:3)*uiScale; dl.rect(es[0]-hs,es[1]-hs,hs*2,hs*2,col); dl.border(es[0]-hs,es[1]-hs,hs*2,hs*2,ui::rgba(20,20,20),1);
                    if (on) dl.line(s[0],s[1],es[0],es[1],ui::withA(col,130),1.f);
                    if (seld){ exitHVis=true; exitHS[0]=es[0]; exitHS[1]=es[1]; cx.textAligned(es[0]+8*uiScale,es[1]-8*uiScale,80*uiScale,16*uiScale,"exit",col,0); } } break; }
              case sitem::WALLPLACE: { float h[3]={it.propW*0.5f,it.propH*0.5f,0.02f}; drawBox(it.pos,h,q,col,th,seld);
                    drawFacingArrow(it.pos,q,col,th,0.6f);   // props hang on THIS side ("loads on the wrong side" fix: see it + Flip button)
                    if (seld) cx.textAligned(s[0]+10*uiScale,s[1]-8*uiScale,150*uiScale,16*uiScale,"front",col,0); break; }
              case sitem::HOTSPOT: { if (on){ float rr=16*uiScale; for (int a=0;a<16;a++){ float a0=a/16.f*6.2831853f,a1=(a+1)/16.f*6.2831853f; dl.line(s[0]+cosf(a0)*rr,s[1]+sinf(a0)*rr, s[0]+cosf(a1)*rr,s[1]+sinf(a1)*rr, col,th); } drawFacingArrow(it.pos,q,col,th,0.6f); } break; }
              case sitem::NAVMESH: { drawNavWire(it, ui::withA(col, seld?210:120)); float mp[3]; itemMarkerPos(it,mp); float ms[2]; if (worldToScreen(mp,ms[0],ms[1])) { dl.rect(ms[0]-3*uiScale,ms[1]-3*uiScale,6*uiScale,6*uiScale,col); cx.textAligned(ms[0]+8*uiScale,ms[1]-8*uiScale,180*uiScale,16*uiScale,it.name.c_str(),col,0); } break; }
              case sitem::BOUNDARY: {   // kill-floor plane: a grid at it.pos + a normal arrow along the UnitAxis direction
                    float S=30.f; uint32_t bc=ui::withA(col, seld?210:130);
                    for (int g=-5; g<=5; ++g){ float t=g/5.f*S;
                        float a1[3]={it.pos[0]-S,it.pos[1],it.pos[2]+t}, a2[3]={it.pos[0]+S,it.pos[1],it.pos[2]+t}, b1[3]={it.pos[0]+t,it.pos[1],it.pos[2]-S}, b2[3]={it.pos[0]+t,it.pos[1],it.pos[2]+S};
                        float p1[2],p2[2]; if(worldToScreen(a1,p1[0],p1[1])&&worldToScreen(a2,p2[0],p2[1])) dl.line(p1[0],p1[1],p2[0],p2[1],bc,1.f);
                        if(worldToScreen(b1,p1[0],p1[1])&&worldToScreen(b2,p2[0],p2[1])) dl.line(p1[0],p1[1],p2[0],p2[1],bc,1.f); }
                    static const float ax6[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                    const float* av=ax6[(it.axis>=0&&it.axis<6)?it.axis:2];
                    float np[3]={it.pos[0]+av[0]*2.f, it.pos[1]+av[1]*2.f, it.pos[2]+av[2]*2.f}, na[2];
                    if (on && worldToScreen(np,na[0],na[1])) dl.line(s[0],s[1],na[0],na[1],col,th+0.6f);
                    if (on) cx.textAligned(s[0]+8*uiScale,s[1]-8*uiScale,160*uiScale,16*uiScale,it.name.c_str(),col,0); break; }
            }
        }
        dl.popClip();
    }
    // ray-cast the cursor to the y=planeY ground plane (for the chair exit-handle drag)
    bool screenToGround(float mx, float my, float planeY, float out[3]){
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp); float inv[16]; if(!invertMat4(vp,inv)) return false;
        float W=(float)rcViewport.extent.width, H=(float)rcViewport.extent.height;
        float ndcx=2*(mx-rcViewport.offset.x)/W-1, ndcy=2*(my-rcViewport.offset.y)/H-1;
        float O[3],F[3]; unproject(inv,ndcx,ndcy,1,O); unproject(inv,ndcx,ndcy,0,F);
        float dy=F[1]-O[1]; if(std::fabs(dy)<1e-6f) return false; float t=(planeY-O[1])/dy; if(t<0) return false;
        out[0]=O[0]+(F[0]-O[0])*t; out[1]=planeY; out[2]=O[2]+(F[2]-O[2])*t; return true;
    }
    int pickItem(double mx, double my){
        if (!showItems) return -1; int best=-1; float bd=20*uiScale*20*uiScale;
        for (int i=0;i<(int)items.size();++i){ if (!itemShown(items[i])) continue; float p[3]; itemMarkerPos(items[i],p); float s[2]; if (!worldToScreen(p,s[0],s[1])) continue; float d=(s[0]-mx)*(s[0]-mx)+(s[1]-my)*(s[1]-my); if (d<bd){bd=d;best=i;} }
        return best;
    }
    int gizmoHitTest(float mx, float my) {
        if (!gzVisible) return -1; int best=-1;
        if (gizmoOp==1){   // rotate: nearest projected ring segment within tolerance
            float bestD=11*uiScale;
            for (int k=0;k<3;k++){ if (lockAxis[k]) continue;   // a locked axis can't be grabbed
                for (int s=0;s<32;s++){ float d=distToSeg(mx,my, gzRing[k][s][0],gzRing[k][s][1], gzRing[k][s+1][0],gzRing[k][s+1][1]); if (d<bestD){bestD=d;best=k;} } }
            return best;
        }
        // SCALE: the center square = UNIFORM scale (all 3 axes at once). Test it FIRST with a tight radius —
        // every axis handle STARTS at the origin, so without priority the axes would always win at the center.
        if (gizmoOp==2 && std::hypot(mx-gzOrigin[0], my-gzOrigin[1]) < 8*uiScale) return 3;
        float bestD=14*uiScale;
        for (int k=0;k<3;k++){ if (lockAxis[k]) continue; float d=distToSeg(mx,my, gzOrigin[0],gzOrigin[1], gzTip[k][0],gzTip[k][1]); if (d<bestD){bestD=d;best=k;} }
        return best;
    }
    static float distToSeg(float px,float py,float ax,float ay,float bx,float by){
        float dx=bx-ax,dy=by-ay,l2=dx*dx+dy*dy; if(l2<1e-6f) return std::hypot(px-ax,py-ay);
        float t=std::clamp(((px-ax)*dx+(py-ay)*dy)/l2,0.f,1.f);
        return std::hypot(px-(ax+t*dx), py-(ay+t*dy));
    }
    // ── QUEST PLAYER GIZMO: a live marker at the headset player's world pos with three drag handles —
    //    center square = move on the GROUND plane (XZ), up-arrow = height (Y), yaw-arrow = facing. Dragging
    //    drives the device over the bridge (`warp`/`rot`). Self-contained (own hit-test) so it never fights
    //    the mesh/item gizmo. Needs the AIO bridge live (adb forward tcp:27042) + Track Player on.
    int playerGizmoHit(float mx, float my) {
        if (!gzPlayerVis) return -1;
        float r2=13*uiScale*13*uiScale;
        auto hitH=[&](const float* h){ float dx=mx-h[0], dy=my-h[1]; return dx*dx+dy*dy<=r2; };
        if (hitH(gzPlayerYawTip)) return 2;   // yaw arrow first (sits at the rim)
        if (hitH(gzPlayerUp))     return 1;   // height
        if (hitH(gzPlayerC))      return 0;   // ground move (center)
        return -1;
    }
    void drawPlayerGizmo() {
        gzPlayerVis=false;
        if (!trackPlayer) return;
        startPlayerChannel();   // idempotent: the worker polls the pose + streams drag commands over ONE socket
        float feet[3]={questPlayerPos[0],questPlayerPos[1],questPlayerPos[2]};
        float head=questPlayerHead[1]>feet[1]?questPlayerHead[1]:feet[1]+1.6f;   // stature line
        float cs[2]; if(!worldToScreen(feet,cs[0],cs[1])) { return; }
        gzPlayerC[0]=cs[0]; gzPlayerC[1]=cs[1];
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        uint32_t colBody = playerNoGrav ? ui::rgba(120,210,255) : ui::rgba(255,190,90);   // cyan when no-gravity, amber otherwise
        // stature line feet->head
        float hp[3]={feet[0],head,feet[2]}, hs[2];
        if (worldToScreen(hp,hs[0],hs[1])) dl.line(cs[0],cs[1],hs[0],hs[1], ui::withA(colBody,180), 2.f);
        // ground ring (radius ~0.4m in world) for depth
        for (int s=0;s<24;s++){ float a0=s/24.f*6.2831853f,a1=(s+1)/24.f*6.2831853f;
            float w0[3]={feet[0]+0.4f*std::cos(a0),feet[1],feet[2]+0.4f*std::sin(a0)};
            float w1[3]={feet[0]+0.4f*std::cos(a1),feet[1],feet[2]+0.4f*std::sin(a1)};
            float s0[2],s1[2]; if(worldToScreen(w0,s0[0],s0[1])&&worldToScreen(w1,s1[0],s1[1])) dl.line(s0[0],s0[1],s1[0],s1[1], ui::withA(colBody,150),1.5f); }
        // FACING arrow — shows what the player is ACTUALLY LOOKING AT (device head-forward, `face=` from the
        // bridge) when idle; the yaw you're commanding while dragging the yaw handle. yaw 0 = -Z (matches cam).
        float showYaw = (playerDrag && playerDragMode==2) ? questPlayerYaw : (questPlayerFaceOk ? questPlayerFace : questPlayerYaw);
        float aLen=1.3f;   // longer = clearer
        float fwd[3]={ std::sin(showYaw)*aLen, 0.f, -std::cos(showYaw)*aLen };
        float ft[3]={feet[0]+fwd[0], head*0.5f+feet[1]*0.5f, feet[2]+fwd[2]}, fts[2];   // mid-height so it's not lost in the ground ring
        bool yhot=(playerGizmoHit((float)cx.in.mx,(float)cx.in.my)==2)||(playerDrag&&playerDragMode==2);
        uint32_t faceCol = yhot?ui::rgba(255,235,80):ui::rgba(96,230,120);
        if (worldToScreen(ft,fts[0],fts[1])) { gzPlayerYawTip[0]=fts[0]; gzPlayerYawTip[1]=fts[1];
            float midh[3]={feet[0],ft[1],feet[2]}, ms2[2];
            if (worldToScreen(midh,ms2[0],ms2[1])) dl.line(ms2[0],ms2[1],fts[0],fts[1], faceCol, yhot?4.f:3.f);
            else dl.line(cs[0],cs[1],fts[0],fts[1], faceCol, yhot?4.f:3.f);
            float hs2=8*uiScale; dl.triangle(fts[0]-hs2,fts[1]+hs2, fts[0]+hs2,fts[1]+hs2, fts[0],fts[1]-hs2*1.4f, faceCol);
            cx.textAligned(fts[0]+hs2+2, fts[1]-8*uiScale, 60*uiScale,16*uiScale, "look", faceCol, 0); }
        // height handle (up) at head height
        float up[3]={feet[0],head+0.3f,feet[2]}, ups[2];
        bool uhot=(playerGizmoHit((float)cx.in.mx,(float)cx.in.my)==1)||(playerDrag&&playerDragMode==1);
        if (worldToScreen(up,ups[0],ups[1])) { gzPlayerUp[0]=ups[0]; gzPlayerUp[1]=ups[1];
            dl.line(hs[0],hs[1],ups[0],ups[1], uhot?ui::rgba(255,235,80):ui::rgba(80,130,245), uhot?3.f:2.f);
            float hs2=6*uiScale; dl.rect(ups[0]-hs2,ups[1]-hs2,hs2*2,hs2*2, uhot?ui::rgba(255,235,80):ui::rgba(80,130,245)); }
        // center (ground-move) square
        bool chot=(playerGizmoHit((float)cx.in.mx,(float)cx.in.my)==0)||(playerDrag&&playerDragMode==0);
        float cps=chot?7*uiScale:5*uiScale;
        dl.rect(cs[0]-cps,cs[1]-cps,cps*2,cps*2, chot?ui::rgba(255,235,80):colBody);
        dl.border(cs[0]-cps,cs[1]-cps,cps*2,cps*2, ui::rgba(20,20,20),1.5f);
        cx.textAligned(cs[0]+cps+3, cs[1]-8*uiScale, 120*uiScale,16*uiScale, playerNoGrav?"PLAYER (no-gravity)":"PLAYER", colBody, 0);
        gzPlayerVis=true;
        dl.popClip();
    }
    // per-frame drag apply (called from the frame loop when playerDrag). Queues the latest command every frame;
    // the persistent channel worker coalesces (only the newest matters) + streams it = smooth, no per-frame connect.
    void updatePlayerDrag() {
        if (!playerDrag || !trackPlayer || !cx.in.down[0]) return;
        if (playerDragMode==0) { float g[3]; if (screenToGround((float)cx.in.mx,(float)cx.in.my,questPlayerPos[1],g)){ questPlayerPos[0]=g[0]; questPlayerPos[2]=g[2]; } }
        else if (playerDragMode==1) { questPlayerPos[1] = pdY0 + (pdMy0-(float)cx.in.my)*0.02f; }          // drag up = higher
        else if (playerDragMode==2) { questPlayerYaw = pdYaw0 + ((float)cx.in.mx-pdMx0)*0.01f; }            // drag right = turn
        if (playerDragMode==2) sendPlayerYaw(questPlayerYaw);
        else sendPlayerWarp(questPlayerPos[0],questPlayerPos[1],questPlayerPos[2]);
    }
    void drawGizmo() {
        gzVisible = false;
        bool itemMode = selItem>=0 && selItem<(int)items.size();
        // A HIDDEN item must hide its GIZMO too, not just the marker overlay — else the gizmo stays on top of the mesh
        // and keeps intercepting clicks (gizmoHitTest), so you can't select/edit meshes again. For NAVMESH items (mesh
        // colliders / navmeshes) "hidden" = the TOP header toggle ("Colliders"/"Navmesh") is off — that's the toggle
        // the user clicks to hide a collider and edit more. Hidden -> gizmo drops -> clicks fall through to mesh pick.
        if (itemMode && !itemShown(items[selItem])) itemMode = false;
        if (!itemMode && (selected<0 || selected>=(int)r->gpuMeshes.size())) return;
        float origin[3], gizQuat[4]={0,0,0,1};
        if (itemMode) { auto& it=items[selItem];
            if (editExit && it.type==sitem::CHAIR) { origin[0]=it.pos[0]+it.exitPos[0]; origin[1]=it.pos[1]+it.exitPos[1]; origin[2]=it.pos[2]+it.exitPos[2]; gizQuat[0]=gizQuat[1]=gizQuat[2]=0; gizQuat[3]=1; }   // gizmo on the EXIT point
            else { itemMarkerPos(it, origin); eulerToQuat(it.rot, gizQuat); }   // navmesh gizmo sits on its geometry, not origin
        }
        else if (sel.size() > 1) {
            // MULTI-selection: pivot at the SELECTION CENTROID (average of all selected meshes), not the
            // active mesh — the natural group pivot, and far less likely to sit off-screen than one member.
            VkGpuMesh& agm=r->gpuMeshes[selected]; memcpy(gizQuat,agm.editR,16);
            origin[0]=origin[1]=origin[2]=0.f; int n=0;
            for (int s : sel) if (s>=0 && s<(int)r->gpuMeshes.size()) { auto& g=r->gpuMeshes[s];
                origin[0]+=g.centroid[0]+g.editT[0]; origin[1]+=g.centroid[1]+g.editT[1]; origin[2]+=g.centroid[2]+g.editT[2]; ++n; }
            if (n) { origin[0]/=n; origin[1]/=n; origin[2]/=n; }
        }
        else { VkGpuMesh& gm=r->gpuMeshes[selected]; origin[0]=gm.centroid[0]+gm.editT[0]; origin[1]=gm.centroid[1]+gm.editT[1]; origin[2]=gm.centroid[2]+gm.editT[2]; memcpy(gizQuat,gm.editR,16); }
        if (sliceGizmoOn && !itemMode) { memcpy(origin, slicePos, 12); memcpy(gizQuat, sliceQuat, 16); }   // SLICE mode: the gizmo drives the cutting plane
        float ds[3]={ origin[0]-r->cam.pos[0], origin[1]-r->cam.pos[1], origin[2]-r->cam.pos[2] };
        float dist = std::sqrt(ds[0]*ds[0]+ds[1]*ds[1]+ds[2]*ds[2]); if (dist<0.1f) dist=0.1f;
        // FIXED on-screen size (~78px) regardless of distance: len_world = px * dist / (focalY * vpH/2)
        float fy = std::fabs(r->cam.proj[5]); if (fy<1e-3f) fy=1.f;
        float len = (78.f*uiScale) * dist / (fy * rcViewport.extent.height * 0.5f);
        float ax[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        if (gizmoLocal) for (int k=0;k<3;k++){ float a[3]={ax[k][0],ax[k][1],ax[k][2]},o[3]; quatRotVec(gizQuat,a,o); memcpy(ax[k],o,12); }
        memcpy(gzAxisW, ax, sizeof ax);
        float os[2];
        if (!worldToScreen(origin, os[0], os[1])) {
            // Pivot is BEHIND the camera / outside the frustum: the gizmo used to silently vanish
            // ("gizmo goes invisible"). Say so + how to get it back, instead of drawing nothing.
            cx.textAligned((float)rcViewport.offset.x, (float)rcViewport.offset.y+rcViewport.extent.height-60*uiScale,
                           (float)rcViewport.extent.width, 18*uiScale,
                           "Selection is off-screen  -  double-click its row (or press F while hovering) to focus it", ui::rgba(255,210,120), 1);
            return;
        }
        gzOrigin[0]=os[0]; gzOrigin[1]=os[1];
        const uint32_t col[3]={ ui::rgba(232,72,72), ui::rgba(96,210,96), ui::rgba(80,130,245) };
        const char* axn[3]={"X","Y","Z"};
        dl.pushClip((float)rcViewport.offset.x,(float)rcViewport.offset.y,(float)rcViewport.extent.width,(float)rcViewport.extent.height);
        if (!itemMode) for (int s : sel) if (s>=0 && s<(int)r->gpuMeshes.size()) drawWireframe(r->gpuMeshes[s]);   // mesh wireframe
        // per-axis sign vs the view direction (rotate-drag handedness so the ring tracks the cursor)
        { float vd[3]={origin[0]-r->cam.pos[0],origin[1]-r->cam.pos[1],origin[2]-r->cam.pos[2]}; float l=std::sqrt(vd[0]*vd[0]+vd[1]*vd[1]+vd[2]*vd[2]); if(l>1e-6f){vd[0]/=l;vd[1]/=l;vd[2]/=l;}
          for (int k=0;k<3;k++) gzAxisFace[k]=ax[k][0]*vd[0]+ax[k][1]*vd[1]+ax[k][2]*vd[2]; }
        if (gizmoOp==1) {   // ── ROTATE: three colored rings in the axis planes ──
            for (int k=0;k<3;k++){ int u=(k+1)%3, v=(k+2)%3;
                for (int s=0;s<=32;s++){ float a=s/32.f*6.2831853f, ca=std::cos(a), sa=std::sin(a);
                    float wp[3]={ origin[0]+len*(ca*ax[u][0]+sa*ax[v][0]), origin[1]+len*(ca*ax[u][1]+sa*ax[v][1]), origin[2]+len*(ca*ax[u][2]+sa*ax[v][2]) };
                    float sp[2]; bool ok=worldToScreen(wp,sp[0],sp[1]); gzRing[k][s][0]=ok?sp[0]:os[0]; gzRing[k][s][1]=ok?sp[1]:os[1]; } }
            int hk=gizmoHitTest((float)cx.in.mx,(float)cx.in.my);
            for (int k=0;k<3;k++){ bool hot=(hk==k)||(gizmoDrag&&gizmoAxis==k); uint32_t c=lockAxis[k]?ui::rgba(110,110,110,150):(hot?ui::rgba(255,235,80):col[k]);
                for (int s=0;s<32;s++) dl.line(gzRing[k][s][0],gzRing[k][s][1],gzRing[k][s+1][0],gzRing[k][s+1][1], c, hot?3.f:2.f);
                gzTip[k][0]=gzRing[k][0][0]; gzTip[k][1]=gzRing[k][0][1];
                cx.textAligned(gzRing[k][0][0]+4*uiScale, gzRing[k][0][1]-8*uiScale, 14*uiScale,16*uiScale, axn[k], c, 0); }
        } else {            // ── MOVE / SCALE: three axis handles ──
            for (int k=0;k<3;k++){
                float tip[3]={origin[0]+ax[k][0]*len, origin[1]+ax[k][1]*len, origin[2]+ax[k][2]*len};
                float ts[2]; if (!worldToScreen(tip, ts[0], ts[1])) { gzTip[k][0]=os[0]; gzTip[k][1]=os[1]; continue; }
                gzTip[k][0]=ts[0]; gzTip[k][1]=ts[1];
                bool hot = (gizmoHitTest((float)cx.in.mx,(float)cx.in.my)==k) || (gizmoDrag&&gizmoAxis==k);
                uint32_t c = lockAxis[k] ? ui::rgba(110,110,110,150) : (hot ? ui::rgba(255,235,80) : col[k]);
                dl.line(os[0],os[1],ts[0],ts[1], c, hot?4.f:3.f);
                float hs=(gizmoOp==2?6.f:7.f)*uiScale;
                if (gizmoOp==2) { dl.rect(ts[0]-hs,ts[1]-hs,hs*2,hs*2,c); dl.border(ts[0]-hs,ts[1]-hs,hs*2,hs*2, ui::rgba(20,20,20), 1.5f); }  // scale = colored box handle
                else dl.triangle(ts[0]-hs,ts[1]+hs, ts[0]+hs,ts[1]+hs, ts[0],ts[1]-hs*1.4f, c);            // move = arrowhead
                cx.textAligned(ts[0]+hs+1, ts[1]-8*uiScale, 14*uiScale, 16*uiScale, axn[k], c, 0);
            }
        }
        // center pivot square — in SCALE mode it's a HANDLE (uniform scale of all 3 axes); light it up when hot
        bool uniHot = gizmoOp==2 && (gizmoHitTest((float)cx.in.mx,(float)cx.in.my)==3 || (gizmoDrag&&gizmoAxis==3));
        float cps = uniHot ? 6*uiScale : 4*uiScale;
        dl.rect(os[0]-cps, os[1]-cps, cps*2, cps*2, uniHot?ui::rgba(255,235,80):ui::rgba(240,240,240));
        gzVisible = true;
        dl.popClip();
        // apply an in-progress drag (uses this frame's accumulated mouse delta) to the WHOLE selection
        if (gizmoDrag && gizmoAxis>=0 && cx.in.down[0] && (cx.in.dmx!=0||cx.in.dmy!=0)) applyGizmoDrag(len);
    }
    void applyGizmoDrag(float len) {
        int k=gizmoAxis;
        if (gizmoOp==2 && k==3 && !sliceGizmoOn) {   // ── UNIFORM SCALE: the center square drags all 3 axes together ──
            float f = 1.f + (cx.in.dmx - cx.in.dmy)*0.005f;   // move right / up = grow
            if (selItem>=0 && selItem<(int)items.size()) {
                auto& it=items[selItem];
                if (!(editExit && it.type==sitem::CHAIR)) for (int a2=0;a2<3;a2++) it.scale[a2]=std::max(0.01f, it.scale[a2]*f);
                return;
            }
            for (int m : sel){ if(m<0||m>=(int)r->gpuMeshes.size())continue; VkGpuMesh& gm=r->gpuMeshes[m];
                for (int a2=0;a2<3;a2++) gm.editS[a2]=std::max(0.001f, gm.editS[a2]*f); recomputeModel(gm); }
            return;
        }
        float* A=gzAxisW[k];
        if (lockAxis[k]) return;   // axis locked mid-drag (Shift+X/Y/Z during a grab) -> freeze it
        if (sliceGizmoOn && selItem<0) {   // the gizmo drives the SLICE PLANE, not the meshes
            if (gizmoOp==1) { float ang = cx.in.dmx * 0.0075f * (gzAxisFace[k]>0?-1.f:1.f);
                float h2=std::sin(ang*0.5f), dq[4]={A[0]*h2,A[1]*h2,A[2]*h2,std::cos(ang*0.5f)};
                float nq[4]; quatMul(dq, sliceQuat, nq); memcpy(sliceQuat,nq,16); normalizeQuat(sliceQuat); return; }
            float sdx=gzTip[k][0]-gzOrigin[0], sdy=gzTip[k][1]-gzOrigin[1];
            float sl=std::sqrt(sdx*sdx+sdy*sdy); if (sl<1e-3f) return; sdx/=sl; sdy/=sl;
            float d=(cx.in.dmx*sdx + cx.in.dmy*sdy)*(len/sl);
            slicePos[0]+=A[0]*d; slicePos[1]+=A[1]*d; slicePos[2]+=A[2]*d; return;
        }
        bool exitMode = editExit && selItem>=0 && selItem<(int)items.size() && items[selItem].type==sitem::CHAIR;
        if (gizmoOp==1) {   // ── ROTATE: tangential mouse drag about world axis A (item euler OR mesh quat) ──
            if (exitMode) return;   // the exit point has no rotation
            // HORIZONTAL-scrub rotate: side-to-side mouse = rotate. (The old tangential "drag around the ring" model
            // rotated on up/down or side/side depending on where the cursor happened to sit relative to the gizmo
            // center — unintuitive; user wanted plain left/right = rotate.)
            float ang = cx.in.dmx * 0.0075f * (gzAxisFace[k]>0?-1.f:1.f);   // radians; dmx = horizontal mouse delta
            float h=std::sin(ang*0.5f), dq[4]={A[0]*h,A[1]*h,A[2]*h,std::cos(ang*0.5f)};
            if (selItem>=0 && selItem<(int)items.size()) {                 // item: rotate its euler via a world-axis delta quat
                auto& it=items[selItem];
                if (chairTiltLock && it.type==sitem::CHAIR) {              // TILT-LOCK: chairs only YAW (stay upright) — no pitch/roll tip-over
                    it.rot[1] += ang*57.29578f; it.rot[0]=0.f; it.rot[2]=0.f;
                    while(it.rot[1]>180.f)it.rot[1]-=360.f; while(it.rot[1]<-180.f)it.rot[1]+=360.f; return;
                }
                float q0[4]; eulerToQuat(it.rot,q0); float nq[4]; quatMul(dq,q0,nq); normalizeQuat(nq); quatToEuler(nq,it.rot); return;
            }
            for (int m : sel) { if (m<0||m>=(int)r->gpuMeshes.size()) continue; VkGpuMesh& gm=r->gpuMeshes[m];
                float nr[4]; quatMul(dq, gm.editR, nr); memcpy(gm.editR,nr,16); normalizeQuat(gm.editR); recomputeModel(gm); }
            return;
        }
        float sdx=gzTip[k][0]-gzOrigin[0], sdy=gzTip[k][1]-gzOrigin[1];
        float sl=std::sqrt(sdx*sdx+sdy*sdy); if (sl<1e-3f) return; sdx/=sl; sdy/=sl;
        float drag = cx.in.dmx*sdx + cx.in.dmy*sdy;       // pixels along the on-screen axis
        float worldPerPx = len/sl;
        if (selItem>=0 && selItem<(int)items.size()) {    // dragging a scene item (move/scale its transform)
            auto& it=items[selItem];
            if (exitMode) { if (gizmoOp==0){ float d=drag*worldPerPx; it.exitPos[0]+=A[0]*d; it.exitPos[1]+=A[1]*d; it.exitPos[2]+=A[2]*d; } return; }   // move the EXIT point
            if (gizmoOp==0)      { float d=drag*worldPerPx; it.pos[0]+=A[0]*d; it.pos[1]+=A[1]*d; it.pos[2]+=A[2]*d; }
            else                 { float f=1.f+drag*0.005f; it.scale[k]=std::max(0.01f, it.scale[k]*f); }  // gizmoOp==2
            return;
        }
        for (int m : sel) { if (m<0||m>=(int)r->gpuMeshes.size()) continue; VkGpuMesh& gm=r->gpuMeshes[m];
            if (gizmoOp==0) { float d=drag*worldPerPx; gm.editT[0]+=A[0]*d; gm.editT[1]+=A[1]*d; gm.editT[2]+=A[2]*d; }
            else            { float f=1.f+drag*0.005f; gm.editS[k]=std::max(0.001f, gm.editS[k]*f); }       // gizmoOp==2
            recomputeModel(gm);
        }
    }
    static void quatMul(const float a[4], const float b[4], float o[4]) {
        o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
        o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
        o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
        o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  COOK  (build the export snapshot on the main thread, run the heavy cook + auto-sign on a worker)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    std::vector<hslcook::ExportMesh> buildExportMeshes() {
        using namespace hslcook; std::vector<ExportMesh> ems;
        if (!r || !sceneMeshes) return ems;
        size_t n = std::min(sceneMeshes->size(), r->gpuMeshes.size());
        for (size_t i=0;i<n;i++){
            // isHidden is the EDITOR-ONLY visibility eye (a working aid) — hidden meshes MUST still ship in the cook
            // (user-confirmed). Deleted/removed items are already absent from sceneMeshes, so there's nothing to skip
            // here. (Was: `if (r->isHidden((int)i)) continue;` which wrongly dropped hidden geometry from the home.)
            if (r->isDeleted(i)) continue;   // editor DELETE: drop this mesh from the cooked home (non-destructive, persisted)
            // SPLIT-PART TEXTURE SHARING: a piece from splitMeshParts left its texture buffers empty + points
            // texShareSrc at the source mesh. Rebuild a texture-complete copy for the cook (transient, one at a
            // time) so the cooked pieces keep their material — without re-duplicating the textures in memory.
            const MeshData& mdRaw=(*sceneMeshes)[i];
            MeshData mdShared; const MeshData* mdp=&mdRaw;
            if (mdRaw.texShareSrc>=0 && (size_t)mdRaw.texShareSrc<sceneMeshes->size()){
                const MeshData& s2=(*sceneMeshes)[(size_t)mdRaw.texShareSrc]; mdShared=mdRaw;
                mdShared.texRGBA=s2.texRGBA; mdShared.texW=s2.texW; mdShared.texH=s2.texH; mdShared.hasTexture=s2.hasTexture;
                mdShared.astcRaw=s2.astcRaw; mdShared.astcBw=s2.astcBw; mdShared.astcBh=s2.astcBh;
                mdShared.srcAstc=s2.srcAstc; mdShared.srcAstcBw=s2.srcAstcBw; mdShared.srcAstcBh=s2.srcAstcBh; mdShared.srcAstcMips=s2.srcAstcMips;
                mdShared.normalRGBA=s2.normalRGBA; mdShared.normalW=s2.normalW; mdShared.normalH=s2.normalH; mdShared.hasNormal=s2.hasNormal;
                mdShared.ormRGBA=s2.ormRGBA; mdShared.ormW=s2.ormW; mdShared.ormH=s2.ormH; mdShared.hasOrm=s2.hasOrm; mdShared.ormTexName=s2.ormTexName;
                mdShared.emissiveRGBA=s2.emissiveRGBA; mdShared.emissiveW=s2.emissiveW; mdShared.emissiveH=s2.emissiveH; mdShared.hasEmissive=s2.hasEmissive;
                mdShared.lmRGBA=s2.lmRGBA; mdShared.lmW=s2.lmW; mdShared.lmH=s2.lmH; mdShared.hasLightmap=s2.hasLightmap;
                mdShared.vatRaw=s2.vatRaw; mdShared.vatW=s2.vatW; mdShared.vatH=s2.vatH; mdShared.hasVat=s2.hasVat;
                mdp=&mdShared;
            }
            const MeshData& md=*mdp; const VkGpuMesh& gm=r->gpuMeshes[i];
            size_t nv=md.positions.size()/3; if (nv<3||md.indices.size()<3) continue;
            ExportMesh em; em.name=md.name; em.srcMeshIdx=(int)i; em.positions.resize(nv*3);
            for (size_t v=0;v<nv;v++){ float p[3]={md.positions[v*3],md.positions[v*3+1],md.positions[v*3+2]},o[3]; xformPoint(gm.model,p,o); em.positions[v*3]=o[0]; em.positions[v*3+1]=o[1]; em.positions[v*3+2]=o[2]; }
            em.uvs=md.uvs; em.indices=md.indices; em.additive = md.additive;
            // FAITHFUL SpecIbl diffuse-irradiance bake (the renderer's uploadMesh bake at vk_renderer.h:677, ported to the
            // cook export — NO texture cheat): a textured *_specibl* mesh (the lake water) is env-lit by the diffuse IBL
            // cubemap. The cook otherwise ships white vertexColor0 -> device base·white = the DARK water basecolor = black
            // lake. Bake per-vertex color = diffuseCube(worldN)·ambientIBLTint so the device base·vertexColor0 = the lit
            // water (= what the preview shows). em.positions are already WORLD-space, so accumulated face normals ARE world
            // normals (same as the renderer's local `nrm`). View-dependent specular is intentionally omitted (can't bake).
            // Two cases, exactly matching the renderer's uploadMesh bake (vk_renderer.h:647 / :679):
            //   (1) the env HAS an IBL diffuse cubemap -> diffuseCube(worldN)·ambientIBLTint;
            //   (2) NO IBL (lakeside) -> the renderer's HEMISPHERIC AMBIENT fallback (0.55..1.0 by world-up, gently warm)
            //       — the "honest ambient" that lights an opaque, non-lightmapped, non-blend mesh (the SpecIbl lake water).
            // Without this the cook ships white vertexColor0 -> device base·white = the DARK water basecolor = the black lake.
            bool doIbl  = md.iblLit && r->iblDiffuse.ok();
            bool doHemi = !doIbl && !md.hasLightmap && !md.useBlend && md.colors.empty() && md.iblLit;   // gate to specibl (iblLit) so only the unlit water/specular meshes get the ambient, not every opaque shell
            if (doIbl || doHemi) {
                std::vector<float> nrm(nv*3, 0.f);
                for (size_t t=0;t+2<em.indices.size();t+=3){ uint32_t a=em.indices[t],b=em.indices[t+1],c=em.indices[t+2];
                    if (a>=nv||b>=nv||c>=nv) continue;
                    const float* pa=&em.positions[a*3]; const float* pb=&em.positions[b*3]; const float* pc=&em.positions[c*3];
                    float e1[3]={pb[0]-pa[0],pb[1]-pa[1],pb[2]-pa[2]}, e2[3]={pc[0]-pa[0],pc[1]-pa[1],pc[2]-pa[2]};
                    float fn[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
                    for (uint32_t vi : {a,b,c}){ nrm[vi*3]+=fn[0]; nrm[vi*3+1]+=fn[1]; nrm[vi*3+2]+=fn[2]; } }
                em.iblVertCol.resize(nv*4);
                auto cl=[](float x){ x=x<0?0:(x>1?1:x); return (uint8_t)(x*255.f+0.5f); };
                for (size_t v=0;v<nv;v++){ float* nP=&nrm[v*3]; float l=std::sqrt(nP[0]*nP[0]+nP[1]*nP[1]+nP[2]*nP[2]); if(l<1e-6f)l=1.f;
                    float nx=nP[0]/l, ny=nP[1]/l, nz=nP[2]/l;
                    if (doIbl) { float irr[3]; ibl::sample(r->iblDiffuse, nx, ny, nz, irr);
                        em.iblVertCol[v*4]=cl(irr[0]*r->ambientIBLTint[0]); em.iblVertCol[v*4+1]=cl(irr[1]*r->ambientIBLTint[1]); em.iblVertCol[v*4+2]=cl(irr[2]*r->ambientIBLTint[2]); }
                    else { float t=ny*0.5f+0.5f, amb=0.55f+0.45f*t;   // hemispheric: down 0.55 .. up 1.0, warm
                        em.iblVertCol[v*4]=cl(amb); em.iblVertCol[v*4+1]=cl(amb*0.98f); em.iblVertCol[v*4+2]=cl(amb*0.93f); }
                    em.iblVertCol[v*4+3]=255; }
            }
            // The cook routes by the AUTHORED MaterialProperties flags (md.*), NOT the renderer's PREVIEW classification
            // (gm.*). gm.useBlend/gm.alphaTest are mutated by preview-only heuristics (the opaque-fraction "solid scenery
            // -> alpha-test cutout", the King-Kai taMin>=255 opaque-blend reclassify, computesAlpha) that are RIGHT for the
            // OPA preview but WRONG to bake: they flipped the Transparent mountain/lakeshore CARDS to gm.alphaTest=true /
            // gm.useBlend=false, so the cook saw blend=false -> shipped them on the plain unlit (OPAQUE) shader -> their
            // transparent silhouette rendered BLACK in the cooked env (user: "transparent background rendered black").
            // md.useBlend = the .mat Transparent||Additive flag; md.alphaTest = the .mat AlphaTest flag -> faithful:
            //   Transparent (mountain/cliff/lakeshore cards, foliage, fog) -> BLEND (unlitblend) = composites over the SkyDome.
            //   AlphaTest:true ("_masked" GROUND) -> cutout discard shader. Additive (foam/glow) -> additive.
            em.alphaTest = md.alphaTest && !md.additive && !std::getenv("HSR_NOCUTOUT");
            em.alphaCutoff = md.alphaCutoff;   // AUTHORED 'alphatestthreshold' (zen tree 0.25) -> cook discards at the source's own threshold
            em.blend = (md.useBlend || md.additive) && !em.alphaTest;   // genuine alpha-blend from the authored flag (cutout is the opaque-pass discard shader)
            em.doubleSided = md.doubleSided;   // WAS DROPPED -> cooked single-sided -> flat/open doubleSided meshes (monitor screens, thin panels) back-face-culled on device = see-through HOLES. Carry it so the cook double-sides them (reversed tris). Renderer honors doubleSided (gm.cullBack=!doubleSided) so the live preview already looked right.
            em.wantCollider = isAnimCollider((int)i);   // user marked this animated mesh -> same-entity kinematic collider
            em.skybox = isSkyboxMesh((int)i);            // user marked this as far backdrop -> SkyboxPlatformComponent (far-clip-exempt)
            for (int k=0;k<4;k++) em.matTint[k]=md.tint[k];
            // EXPOSE-ALL: forward every decoded stream the cook can use (was dropped -> cooked envs lost lightmaps/
            // normals/uv1/per-instance-VAT/per-frame-tint). project_hsl_cooker_expose_all_audit.
            em.uvs2 = md.uvs2; em.bakeLightmapVtx = md.bakeLightmapVtx;
            for (int k=0;k<3;k++){ em.lightmapPower[k]=md.lightmapPower[k]; em.albedoFactor[k]=md.albedoFactor[k]; }
            // WYSIWYG tint: the live render multiplies EVERY draw by the user's per-mesh tint edit
            // (gm.editTint) and the global Lighting sliders (r->lightMul) via the color push-constant —
            // neither shipped, so tint/lighting edits vanished from the cooked env. Fold them into
            // curTint, the ONE factor the cook bakes into COLOR_0 (matTint feeds separate material
            // params — folding into both would double-apply).
            for (int k=0;k<4;k++) em.curTint[k]=md.curTint[k]*gm.editTint[k]*r->lightMul[k];
            if (md.hasLightmap && !md.lmRGBA.empty()){ em.lmRGBA=md.lmRGBA; em.lmW=md.lmW; em.lmH=md.lmH; em.hasLightmap=true; }
            if (md.hasNormal && !md.normalRGBA.empty()){ em.normalRGBA=md.normalRGBA; em.normalW=md.normalW; em.normalH=md.normalH; em.hasNormal=true; }
            em.vatInstTrackIndex=md.vatTrackIndex; em.vatInstRateFactor=md.vatRateFactor; em.vatInstTimeOffset=md.vatTimeOffset; em.atlasCellIndex=md.atlasCellIndex;
            int fcols=0,frows=0;
            if (detectAndCollapseFlipbook(em.positions, em.uvs, em.indices, fcols, frows)) {
                em.flipbook=true; em.flipCols=fcols; em.flipRows=frows; em.blend=true;
                em.flipFrames=fcols*frows;                                  // one cell per frame, the whole sheet
                em.flipFps = (animDuration>0.f) ? (float)em.flipFrames/animDuration : 5.f;   // cycle the sheet over the source loop
                // OBSERVED cell sequence (FLIP-VERIFY 2026-07-08): the row-major @global-fps grid assumption was
                // WRONG for most sheets (TV 23/100 frames, cube_maze torches 0/64 with per-torch phase stagger,
                // zelda fires at 0.45fps instead of their own loop). Replay the sequence the SOURCE actually
                // plays — per-frame 2x3 UV mats over the mesh's OWN clip period (exact frame-snap replay path).
                if (flipbookSequencer) { std::vector<float> fmats; int fN=0; float floop=0.f;
                    if (flipbookSequencer((int)i, fcols, frows, fmats, fN, floop) && fN >= 2) {
                        em.flipUVMats = std::move(fmats); em.flipN = fN; em.flipLoop = floop;
                        fprintf(stderr,"[COOK] m%zu '%s' FLIPBOOK %dx%d OBSERVED sequence: %d frames over %.2fs (exact replay)\n", i, md.name.c_str(), fcols, frows, fN, floop);
                    } else fprintf(stderr,"[COOK] m%zu '%s' FLIPBOOK %dx%d (%d cells @ %.2ffps, grid order — sequencer found no varying cell)\n", i, md.name.c_str(), fcols, frows, em.flipFrames, em.flipFps);
                } else fprintf(stderr,"[COOK] m%zu '%s' FLIPBOOK %dx%d (%d cells @ %.2ffps) -> auto-gen shader\n", i, md.name.c_str(), fcols, frows, em.flipFrames, em.flipFps);
            }
            else { if (vatBaker){ int bnv=0; auto off=vatBaker((int)i,64,bnv); if(!off.empty()&&bnv==(int)nv){ em.vatOffsets=std::move(off); em.vatFrames=64; } }
                   if (hzAnimExtractor) hzAnimExtractor((int)i,64,em); }
            if (md.hasTexture && md.texRGBA.size()>=(size_t)md.texW*md.texH*4){ em.rgba=md.texRGBA; em.w=md.texW; em.h=md.texH;
                em.srcAstc=md.srcAstc; em.srcAstcBw=md.srcAstcBw; em.srcAstcBh=md.srcAstcBh; em.srcAstcMips=md.srcAstcMips; }
            // ATLAS SUB-RECT CROP (the "balloons should be NITID and visible from far" fix — fidelity, not a
            // cap): a card that samples ONE CELL of a shared multi-design atlas (each balloon = 1 of 5 in the
            // sheet) inherits the WHOLE sheet's mip chain — the deep mips average every design = a muddy
            // "green dot without details" at distance. Crop the texture to the mesh's own UV sub-rect (+8px
            // pad) and remap the UVs: every mip level down to 1x1 now averages ONLY this card's content = a
            // crisp correctly-colored image at any distance. Static-UV meshes only (UV-animated cards sample
            // outside their base span); tiling (UVs beyond [0,1]) exempt.
            if (!em.rgba.empty() && em.w >= 256 && em.uvs.size() >= 4
                && !em.flipbook && !em.uvScroll && em.flipUVMats.empty() && !em.skybox) {
                float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
                for (size_t k=0;k+1<em.uvs.size();k+=2){ float u=em.uvs[k],v=em.uvs[k+1];
                    if(u<umin)umin=u; if(u>umax)umax=u; if(v<vmin)vmin=v; if(v>vmax)vmax=v; }
                bool inUnit = umin > -0.001f && umax < 1.001f && vmin > -0.001f && vmax < 1.001f;
                float area = (umax-umin)*(vmax-vmin);
                if (inUnit && area > 1e-6f && area < 0.4f) {
                    int W=(int)em.w, H=(int)em.h, PAD=8;
                    int x0=(int)std::floor(umin*W)-PAD, y0=(int)std::floor(vmin*H)-PAD;
                    int x1=(int)std::ceil (umax*W)+PAD, y1=(int)std::ceil (vmax*H)+PAD;
                    x0=std::max(0,x0); y0=std::max(0,y0); x1=std::min(W,x1); y1=std::min(H,y1);
                    int cw=x1-x0, ch=y1-y0;
                    if (cw>=16 && ch>=16 && (size_t)cw*ch < (size_t)W*H) {
                        std::vector<u8> crop((size_t)cw*ch*4);
                        for (int y=0;y<ch;y++) memcpy(&crop[(size_t)y*cw*4], &em.rgba[((size_t)(y0+y)*W + x0)*4], (size_t)cw*4);
                        em.rgba.swap(crop); em.w=cw; em.h=ch;
                        em.srcAstc.clear(); em.srcAstcBw=em.srcAstcBh=em.srcAstcMips=0;   // cropped -> re-encode (source footprint + quality kept by the cook)
                        for (size_t k=0;k+1<em.uvs.size();k+=2){
                            em.uvs[k]   = (em.uvs[k]  *W - (float)x0) / (float)cw;
                            em.uvs[k+1] = (em.uvs[k+1]*H - (float)y0) / (float)ch;
                        }
                        fprintf(stderr, "[COOK] m%zu '%s' ATLAS-CROP %dx%d -> %dx%d (uv sub-rect %.0f%%; nitid mips)\n",
                                i, md.name.c_str(), W, H, cw, ch, area*100.f);
                    }
                }
            }
            // TWO-PASS foliage: FUNDAMENTALLY BROKEN and disabled by default (opt-in HSR_FOLIAGE_2PASS). The opaque
            // depth-write cutout SIBLING is drawn behind the transparent blend, but the blend is see-through so the
            // BLOCKY cutout edges show THROUGH it -> "leaves back to being blocky". Shrinking the cutout doesn't help
            // (it's still visible through the transparent leaf). The only real smooth+depth path is alpha-to-coverage.
            bool foliage2pass = em.alphaTest && em.hzJointCount > 0 && !em.blend && !em.additive && std::getenv("HSR_FOLIAGE_2PASS");
            hslcook::ExportMesh prepass; if (foliage2pass) prepass = em;   // copy BEFORE the move (keeps all skinning)
            // ── ANIM-AUDIT: per-mesh SOURCE anim channels vs what the cook ACTUALLY ships. A mesh the source
            //    animates but whose ExportMesh carries no anim representation is a SILENT DROP — the exact class of
            //    bug the batch env audit greps for (`<< UNPORTED`). animSourceProbe is per-format and independent
            //    of the extractor gates above, so a gate rejecting a mesh still shows as source-animated here.
            if (animSourceProbe) {
                std::string srcCh = animSourceProbe((int)i);
                if (!srcCh.empty()) {
                    const char* repr = em.hzFrames>1 ? "HZANIM" : !em.vatOffsets.empty() ? "VAT" :
                                       em.rotReplay ? "ROTREPLAY" : em.rotAnim ? (em.rotOsc?"ROT-SWAY":"ROT-SPIN") :
                                       em.transAnim ? "TRANSLATE" : em.scaleAnim ? "SCALE" :
                                       em.uvScroll ? "UV-SCROLL" : !em.flipUVMats.empty() ? "UV-MATRIX" :
                                       em.flipbook ? "FLIPBOOK" : em.poseAnim ? "POSEANIM" : em.pulse ? "PULSE" :
                                       em.tintAnim ? "TINTREPLAY" : em.fadeAnim ? "FADE" : nullptr;
                    std::string layered;   // secondary layers riding the main repr (partial drops stay visible)
                    if (em.tintAnim && repr && strcmp(repr,"TINTREPLAY")!=0) layered += "+TINT";
                    if (em.fadeAnim && repr && strcmp(repr,"FADE")!=0)       layered += "+FADE";
                    bool constOnly = srcCh.find("(const)") != std::string::npos && srcCh.find(',') == std::string::npos;
                    fprintf(stderr, "[ANIM-AUDIT] m%03zu '%s' src={%s} cooked=%s%s%s\n", i, md.name.c_str(),
                            srcCh.c_str(), repr?repr:"STATIC", layered.c_str(),
                            (!repr && !constOnly) ? "   << UNPORTED" : "");
                }
            }
            // ── TEX-AUDIT (HSR_ANIMAUDIT runs): AUTHORED transparency vs what SHIPS. Compares the raw glTF
            //    alphaMode/baseColorFactor.a (md.srcAlphaMode/srcFactorA, recorded before any heuristic) and the
            //    SOURCE texture's alpha content against the ExportMesh class (blend/cutout/additive) + the SHIPPED
            //    texture's alpha histogram + the shipped tint alpha. Every divergence class is flagged:
            //      BLEND-DROPPED / MASK-DROPPED  = authored transparency ships opaque ("not ported at all")
            //      ALPHA-LOST                    = transparent class shipped, but the texture's alpha got flattened
            //      FACTOR-ALPHA-LOST             = material-level translucency (factor.a<1) not carried to the tint
            //      SPURIOUS                      = authored OPAQUE ships transparent (sorting/visibility risk)
            if (std::getenv("HSR_ANIMAUDIT") && md.srcAlphaMode >= 0) {
                auto alphaStats = [](const std::vector<u8>& px, float& mn, float& pctVar, float& pctMid){
                    mn = 1.f; size_t below = 0, mid = 0, n = px.size()/4;
                    for (size_t p2 = 3; p2 < px.size(); p2 += 4) { u8 a = px[p2];
                        if (a/255.f < mn) mn = a/255.f;
                        if (a < 250) below++;
                        if (a > 10 && a < 245) mid++; }
                    pctVar = n ? 100.f*below/n : 0.f; pctMid = n ? 100.f*mid/n : 0.f; };
                float sMn, sVar, sMid; alphaStats(md.texRGBA, sMn, sVar, sMid);      // SOURCE texture alpha
                float dMn, dVar, dMid; alphaStats(em.rgba, dMn, dVar, dMid);          // SHIPPED texture alpha
                float shipTintA = em.matTint[3] * em.curTint[3];
                bool srcTransparent = md.srcAlphaMode == 2 || md.srcFactorA < 0.99f;
                bool srcMask = md.srcAlphaMode == 1;
                bool shipT = em.blend || em.additive, shipM = em.alphaTest;
                std::string flags;
                if (srcTransparent && !shipT && !shipM) {
                    // authored BLEND with an all-opaque texture AND opaque factor = visually opaque — an
                    // INTENTIONAL opaque reclass (King Kai), not a drop.
                    if (!(md.srcAlphaMode == 2 && sVar < 0.5f && md.srcFactorA >= 0.99f)) flags += " <<BLEND-DROPPED";
                }
                if (srcMask && !shipM && !shipT) flags += " <<MASK-DROPPED";
                if (!srcTransparent && !srcMask && (shipT || shipM) && md.srcAlphaMode == 0) flags += " <<SPURIOUS";
                if ((shipT || shipM) && sVar >= 2.f && dVar < 0.5f) flags += " <<ALPHA-LOST";
                if (md.srcFactorA < 0.99f && std::fabs(shipTintA - md.srcFactorA) > 0.05f && dVar < 2.f) flags += " <<FACTOR-ALPHA-LOST";
                if (!flags.empty() || std::getenv("HSR_TEXAUDIT_ALL"))
                    fprintf(stderr, "[TEX-AUDIT] m%03zu '%s' src{mode=%s factorA=%.2f texA:min=%.2f var%%=%.1f mid%%=%.1f} ship{%s%s%s cutoff=%.2f tintA=%.2f texA:min=%.2f var%%=%.1f}%s\n",
                            i, md.name.c_str(), md.srcAlphaMode==2?"BLEND":md.srcAlphaMode==1?"MASK":"OPAQUE",
                            md.srcFactorA, sMn, sVar, sMid,
                            em.blend?"BLEND":"", em.alphaTest?"MASK":"", (!em.blend&&!em.alphaTest)?(em.additive?"ADDITIVE":"OPAQUE"):(em.additive?"+ADD":""),
                            em.alphaCutoff, shipTintA, dMn, dVar, flags.c_str());
            }
            ems.push_back(std::move(em));
            if (foliage2pass) {
                prepass.depthPrepass = true; prepass.name += "_depthwr";
                // shrink toward centroid (~1.5%) so the cutout is just behind the blend surface (blend wins the depth test)
                auto shrink = [](std::vector<float>& p){ if (p.size()<9) return; double c[3]={0,0,0}; size_t n=p.size()/3;
                    for (size_t v=0; v<n; v++){ c[0]+=p[v*3]; c[1]+=p[v*3+1]; c[2]+=p[v*3+2]; } c[0]/=n; c[1]/=n; c[2]/=n;
                    const float s = 0.985f;   // 1.5% inward
                    for (size_t v=0; v<n; v++){ p[v*3]=(float)(c[0]+(p[v*3]-c[0])*s); p[v*3+1]=(float)(c[1]+(p[v*3+1]-c[1])*s); p[v*3+2]=(float)(c[2]+(p[v*3+2]-c[2])*s); } };
                shrink(prepass.positions); shrink(prepass.hzRestPos);
                ems.push_back(std::move(prepass));
            }
        }
        if(skyPreviewMesh<0) appendSkyboxMesh(ems);   // GENERAL skybox -> far sky sphere in the cook (if a live preview sphere exists it's already in sceneMeshes, so don't double-emit)
        return ems;
    }
    static std::string cookShellPath() { const char* v=std::getenv("HSR_COOK_SHELL"); return v?v:"(embedded shell)"; }   // donor is baked in; path is a label only
    std::string cookOutPath() {   // ALL cooked APKs land in a single `cooked/` folder next to `cooker/` (cwd = project root,
        if (const char* v=std::getenv("HSR_COOK_OUT")) return v;             // where the cook already reads/writes cooker/*).
        std::string base = "edited_export";
        if (!projectPath.empty()) { size_t sl=projectPath.find_last_of("/\\");
            base = (sl==std::string::npos)? projectPath : projectPath.substr(sl+1);
            size_t dot=base.rfind('.'); if(dot!=std::string::npos) base=base.substr(0,dot); }
        std::error_code ec; std::filesystem::create_directories("cooked", ec);   // make the output dir (no-op if it exists)
        return "cooked/" + base + "_cooked.apk";
    }
    // ── COOK-UP-TO-DATE detection ("Install only if nothing changed") ────────────────────────────────────
    // The cooked APKs share a stem: cooked/<env>_Rooted-System.apk / _NoRoot-Spoof.apk. Alongside them we drop
    // <env>.cooksig = a content hash of everything that affects the cooked output (the serialized session incl.
    // every cook toggle + material/transform/item edit, the created-mesh .geom sidecar, and the base env's
    // identity). "Install only" recomputes this and, if it matches the recorded sig AND an APK exists, installs
    // the existing APK WITHOUT re-cooking. Camera orbit is excluded (the spawn is cosmetic, not a content change).
    std::string cookStem() {
        std::string out=cookOutPath(); size_t d=out.rfind(".apk"); if(d!=std::string::npos) out=out.substr(0,d);
        if (out.size()>=7 && out.substr(out.size()-7)=="_cooked") out=out.substr(0,out.size()-7);
        return out;
    }
    std::string cookSigPath() { return cookStem()+".cooksig"; }
    static uint64_t fnv1a(const void* p, size_t n, uint64_t h=1469598103934665603ull){
        const uint8_t* b=(const uint8_t*)p; for(size_t i=0;i<n;i++){ h^=b[i]; h*=1099511628211ull; } return h; }
    uint64_t cookInputHash() {
        // serialized session MINUS the CAM line (a camera orbit isn't a content change)
        std::string ses=serializeSession(), noCam;
        { size_t i=0; while(i<ses.size()){ size_t e=ses.find('\n',i); std::string ln=ses.substr(i,e==std::string::npos?std::string::npos:e-i+1);
            if (ln.rfind("CAM ",0)!=0) noCam+=ln; i=(e==std::string::npos)?ses.size():e+1; } }
        uint64_t h=fnv1a(noCam.data(), noCam.size());
        // created-mesh geometry (the .geom sidecar written next to the session on save/cook)
        std::string gp = saveTargetFile()+".geom";
        if (FILE* f=fopen(gp.c_str(),"rb")){ fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
            if(n>0){ std::vector<uint8_t> buf((size_t)n); size_t rd=fread(buf.data(),1,(size_t)n,f); h=fnv1a(buf.data(), rd, h); } fclose(f); }
        // base env identity (path + size) so a different / updated source env invalidates
        h=fnv1a(sourceEnvPath.data(), sourceEnvPath.size(), h);
        std::error_code ec; uint64_t sz = std::filesystem::exists(sourceEnvPath,ec) ? (uint64_t)std::filesystem::file_size(sourceEnvPath,ec) : 0;
        h=fnv1a(&sz,sizeof sz,h);
        return h;
    }
    std::string cookSignatureHex() { char b[20]; snprintf(b,sizeof b,"%016llx",(unsigned long long)cookInputHash()); return b; }
    void writeCookSig() { std::string sig=cookSignatureHex(); if (FILE* f=fopen(cookSigPath().c_str(),"w")){ fputs(sig.c_str(),f); fclose(f); } }
    std::string readCookSig() { std::string rec; if (FILE* f=fopen(cookSigPath().c_str(),"r")){ char b[64]={0}; if(fgets(b,sizeof b,f)) rec=b; fclose(f);
        while(!rec.empty()&&(rec.back()=='\n'||rec.back()=='\r'||rec.back()==' ')) rec.pop_back(); } return rec; }
    // 0 = up to date (unchanged since last cook), 1 = changed/stale, 2 = never cooked (no APK). Fills apkStem sizes.
    int cookState(bool& haveSys, bool& haveSpoof) {
        std::string stem=cookStem(); haveSys=fileEx(stem+"_Rooted-System.apk"); haveSpoof=fileEx(stem+"_NoRoot-Spoof.apk");
        if (!haveSys && !haveSpoof) return 2;
        std::string rec=readCookSig(); if (rec.empty()) return 1;   // APK exists but no sig (older cook) -> treat as stale
        return (rec==cookSignatureHex()) ? 0 : 1;
    }
    // Install the ALREADY-COOKED APK for this env WITHOUT re-cooking, but only if the scene is unchanged.
    void installOnly() {
        if (cooking.load()) { setStatus("busy — a cook is already running"); return; }
        saveProject();   // persist current edits so the signature reflects exactly what's on screen
        bool haveSys=false, haveSpoof=false; int st=cookState(haveSys,haveSpoof);
        if (st==2) { setStatus("No cooked APK for this env yet — run COOK first."); return; }
        if (st==1) { setStatus("Scene CHANGED since the last cook (or the cook predates change-tracking) — the cooked APK is STALE. Run COOK to rebuild; Install-only skipped so you don't push an outdated env."); return; }
        if (!deviceConnected()) { setStatus("Scene is up to date, but NO device is connected — connect the Quest (USB / adb connect) and press Install only again."); return; }
        std::string stem=cookStem(), sysApk=stem+"_Rooted-System.apk", spoofApk=stem+"_NoRoot-Spoof.apk";
        cooking.store(true); cookProg.store(0.f);
        if (cookThread.joinable()) cookThread.join();
        cookThread = std::thread([this,sysApk,spoofApk,haveSys,haveSpoof]{
            auto progress=[this](float f,const char* s){ setStage(f,s); };
            std::string msg="Install-only (scene unchanged — no re-cook): ";
            bool rooted=deviceIsRooted();
            if (rooted && haveSys) { bool ok=installToDevice(sysApk, cookPkg, progress, /*uninstallFirst=*/false, rooted);
                msg += ok? "installed "+sysApk+" ("+cookPkg+") + selected (shell restart: "+shellRestartName()+")" : "install FAILED (adb/device?)"; }
            else if (haveSpoof) {
                std::string bkp; HavenBkp hb=backupOriginalHaven(bkp);
                if (hb==HB_FAILED) msg += "ABORTED: could NOT back up the original Haven 2025 (left untouched).";
                else { bool ok=installToDevice(spoofApk, HAVEN_PKG(), progress, /*uninstallFirst=*/true, rooted);
                    msg += ok? "installed the SPOOF ("+spoofApk+") (shell restart: "+std::string(shellRestartName())+")" : "spoof install FAILED (try the Rooted-System APK)."; }
            } else msg += rooted? "no Rooted-System APK found (cook with a rooted device, or use the spoof)." : "no spoof APK found (enable the spoof toggle + cook once).";
            setStatus(msg); setStage(1.f,"Done"); cooking.store(false);
        });
    }

    void setStage(float f, const char* s){ cookProg.store(f); std::lock_guard<std::mutex> l(statusMx); cookStage=s; }
    void setStatus(const std::string& s){ std::lock_guard<std::mutex> l(statusMx); cookStatus=s; fprintf(stderr,"[COOK] %s\n", s.c_str()); }

    // shared cook core (used by the worker AND the headless CLI). terminalBar = print a \r progress bar to stderr.
    void runCook(std::vector<hslcook::ExportMesh> ems, std::array<float,3> camSpawn, std::string pkg, bool sign, bool spoof, bool terminalBar, std::vector<sitem::Item> sceneItems) {
        using namespace hslcook;
        if (ems.empty()) { setStatus("ERROR: no exportable meshes"); cooking.store(false); return; }
        // BAKE each navmesh/mesh-collider's GIZMO transform (T·R·S) into its navVerts — the cook reads navVerts raw, so
        // without this a navmesh you MOVED in the editor cooks at its ORIGINAL spot ("navmesh gizmo doesn't work").
        for (auto& it : sceneItems) if (it.type==sitem::NAVMESH && it.navVerts.size()>=9) {
            float M[16]; itemTRS(it,M);
            bool ident = M[12]==0&&M[13]==0&&M[14]==0 && M[0]==1&&M[5]==1&&M[10]==1 && M[1]==0&&M[2]==0&&M[4]==0&&M[6]==0&&M[8]==0&&M[9]==0;
            if (!ident){ for (size_t v=0;v+2<it.navVerts.size();v+=3){ float p[3]={it.navVerts[v],it.navVerts[v+1],it.navVerts[v+2]},o[3]; xformPoint(M,p,o); it.navVerts[v]=o[0]; it.navVerts[v+1]=o[1]; it.navVerts[v+2]=o[2]; }
                it.pos[0]=it.pos[1]=it.pos[2]=0; it.rot[0]=it.rot[1]=it.rot[2]=0; it.scale[0]=it.scale[1]=it.scale[2]=1; }   // baked in -> identity
        }
        // Navmesh options are UI TOGGLES (navmesh panel), not env flags — push them to the cook here so hsl_cooker.h reads them.
        setenv_("HSR_NAVSMOOTH", navSmooth ? "1" : "");
        setenv_("HSR_NAVDEBUG",  navDebugClone ? "1" : "");
        std::string nuxd=cookShellPath(), out=cookOutPath();
        std::string outDir; { size_t sl=out.find_last_of("/\\"); outDir = (sl==std::string::npos)? std::string(".") : out.substr(0,sl); }
        // CLEAR, self-describing final names (the tester found "_cooked_signed / _cooked_haven2025" confusing):
        //   <env>_Rooted-System.apk = the env's OWN package; needs adb root/su to auto-select (rooted/dev headsets).
        //   <env>_NoRoot-Spoof.apk  = masquerades as haven2025; install on any headset, then pick "Haven 2025" in the home menu.
        std::string stem = out; { size_t d=stem.rfind(".apk"); if(d!=std::string::npos) stem=stem.substr(0,d); }
        if (stem.size()>=7 && stem.substr(stem.size()-7)=="_cooked") stem=stem.substr(0,stem.size()-7);
        std::string systemOut = stem + "_Rooted-System.apk";
        std::string spoofOut  = stem + "_NoRoot-Spoof.apk";
        auto progress = [this,terminalBar](float f, const char* s){ setStage(f,s); if (terminalBar) printBar(f,s); };
        bool ok=false; std::vector<uint8_t> sceneZip; float spawn[3]={camSpawn[0],camSpawn[1],camSpawn[2]};
        // package spoof for the unsigned/own-package APK uses the env's COOK_PKG; we override via the field
        setenv_("HSR_COOK_PKG", pkg.c_str());
        setenv_("HSR_HZANIM", animSkinned ? "1" : "");   // emit skeletal HZANIM clips so skinned meshes ANIMATE on device (clouds/koi/droids)
        setenv_("HSR_NOCULL", noCull ? "1" : "");         // scene-spanning bounds -> V205 never culls our meshes (V79-style draw-everything); fixes cooked-home clipping
        setenv_("HSR_NOAUTOFLOOR", cookAutoFloor ? "" : "1");   // Cook-tab toggle: OFF = no generated floor/walls at all
        setenv_("HSR_NAVTRIMESH", solidCollision ? "1" : "");  // real double-sided trimesh collider (haven2025 SEBD: 16-align manifest + 128-align RTree + count-shift); off -> ColliderBox grid
        setenv_("HSR_NAVSLOPE", "");                           // no forced slope: the bake's solidCollision path now keeps ALL faces natively incl. CEILINGS (the old "0" dropped the roof undersides -> jump-through)
        setenv_("HSR_VOXSOLID", voxelSolid ? "1" : "");        // thick all-sides voxel ColliderBoxes: OPT-IN, DEFAULT OFF (reverted — it walled off room interiors, "can't walk inside rooms"). Default collision = thin per-triangle floor+wall boxes (walkable). HSR_VOXSOLID re-enables for the thin-wall clip case.
        // HSL render config -> cook's ScenePlatformComponent (the SAME values the live preview applies = WYSIWYG)
        if (cfgFog) { char fc[64]; snprintf(fc,sizeof fc,"%.4f,%.4f,%.4f",cfgFogColor[0],cfgFogColor[1],cfgFogColor[2]); setenv_("HSR_FOGCOLOR",fc);
            char fs[24]; snprintf(fs,sizeof fs,"%.3f",cfgFogStart); setenv_("HSR_FOGSTART",fs);
            char fd[24]; snprintf(fd,sizeof fd,"%.6f",cfgFogDensity); setenv_("HSR_FOGDENSITY",fd); }
        else setenv_("HSR_FOGDENSITY","0");               // fog disabled -> ship no visible distance fog
        { char fcl[24]; snprintf(fcl,sizeof fcl,"%.0f",cfgFar); setenv_("HSR_FARCLIP",fcl); }
        { char sd[32]; snprintf(sd,sizeof sd,"%.0f",skyboxDist); setenv_("HSR_SKYBOX_DIST", skybox ? sd : ""); }  // far backdrop -> skybox pass (escapes PortalStereoCamera far=5000)
        std::vector<uint8_t> vspv, fspv;
        auto apk = exportSceneAPK(ems, nuxd, vspv, fspv, true, &ok, spawn, &sceneZip, (cookAudio ? bgOgg : std::vector<uint8_t>{}), progress, sceneItems, serializeSession());
        if (!ok || apk.empty()) { setStatus("ERROR: cook failed (shell: "+nuxd+")"); cooking.store(false); return; }
        if (!writeFile(out, apk)) { setStatus("ERROR: cannot write "+out); cooking.store(false); return; }
        // ── ONE-CLICK COOK→PREVIEW (HSR_COOK_PREVIEW=1): spawn a fresh renderer on the just-cooked V205 APK, so the
        //    cooked result (skinned + car HZANIM animation, max far-clip, materials) is validated FIRST-HAND IN THE
        //    RENDERER's V205 path — no device, no manual reload. The new window IS the HSL-mode preview. ──
        if (std::getenv("HSR_COOK_PREVIEW")) {
            std::string exe = AppConfig::exePath();   // portable (Win32/macOS/Linux)
            if (!exe.empty() && AppConfig::launchDetached(exe, out)) setStatus("Cooked + launched HSL preview: " + out);
        }
        std::string finalSystem, finalSpoof, msg = "Cooked "+std::to_string(ems.size())+" meshes ("+std::to_string(apk.size()/1024)+"KB)";
        // ── own-package APK (sign -> <env>_Rooted-System.apk; drop the unsigned intermediate on success) ──
        if (sign) {
            if (signApk(out, systemOut, progress)) { finalSystem=systemOut; std::remove(out.c_str()); msg += "  | system(rooted) APK: "+systemOut; }
            else { finalSystem=out; msg += "  | sign FAILED (UNSIGNED "+out+"; run `--fetch-tools`)"; }
        } else finalSystem=out;
        // ── Spoof APK (-> <env>_NoRoot-Spoof.apk): uses NUXD's manifest (a plain `combined` NON-footprint
        //    Environment -> NO vista) but under the HAVEN2025 package name (the one an unrooted user can replace;
        //    nuxd is a non-removable system pkg). So install/backup stays on haven2025, but the shell loads it as a
        //    standalone Environment, not a footprint. nuxdIdentity=true selects the nuxd manifest. ──
        if (spoof && !sceneZip.empty()) {
            bool ok2=false; auto apk2=spliceAPK(nuxd, sceneZip, "com.meta.environment.prod.nuxd", "com.meta.shell.env.footprint.haven2025", &ok2, {}, /*nuxdIdentity=*/true);
            if (ok2 && !apk2.empty()){
                if (sign) {   // the spoof must ALSO be signed or it can't install (INSTALL_PARSE_FAILED_NO_CERTIFICATES)
                    std::string tmp2 = spoofOut + ".unsigned"; writeFile(tmp2, apk2);
                    if (signApk(tmp2, spoofOut, progress)) { finalSpoof=spoofOut; msg += "  | no-root spoof APK: "+spoofOut; }
                    else { writeFile(spoofOut, apk2); finalSpoof=spoofOut; msg += "  | spoof UNSIGNED: "+spoofOut; }
                    std::remove(tmp2.c_str());
                } else { writeFile(spoofOut, apk2); finalSpoof=spoofOut; msg += "  | spoof: "+spoofOut; }
            }
        }
        // ── auto-install: ROOT -> own package (+auto-select); else back up haven2025 and install the spoof ──
        if (installAfterCook && !deviceConnected()) {
            progress(1.0f, "no device - APKs written");
            msg += "  || NO DEVICE connected (adb) -> skipped auto-install; the APK files are written. Connect the Quest (USB or `adb connect`) and re-cook, or install the APK manually.";
        } else if (installAfterCook) {
            progress(0.9f, "detect root");
            bool rooted = deviceIsRooted();
            if (rooted && !finalSystem.empty()) {
                bool inst = installToDevice(finalSystem, pkg, progress, /*uninstallFirst=*/false, rooted);
                msg += inst ? "  || ROOT -> installed UNSPOOFED ("+pkg+") + auto-selected (shell restart: "+std::string(shellRestartName())+")" : "  || install FAILED (adb/device?)";
            } else if (!finalSpoof.empty()) {
                // SAFE ORDER: back up the ORIGINAL Haven 2025 first; THEN (cert mismatch) uninstall it; THEN install the
                // spoof. Only uninstall if the backup succeeded (or there was nothing installed to back up).
                std::string bkp; HavenBkp hb = backupOriginalHaven(bkp);
                if (hb == HB_FAILED) {
                    msg += "  || ABORTED: could NOT back up the original Haven 2025, so it was left UNTOUCHED (your original is safe). Check the device/storage and retry, or use the Rooted-System APK.";
                } else {
                    setStatus("Replacing Haven 2025: it's signed with Meta's certificate (not ours), so the ORIGINAL is uninstalled first (backup kept in "+havenBackupDir()+"). Restore anytime: --restore-haven.");
                    bool inst = installToDevice(finalSpoof, HAVEN_PKG(), progress, /*uninstallFirst=*/true, rooted);
                    msg += inst ? ("  || no root -> "+std::string(hb==HB_OK?("backed up Haven 2025 ("+havenBackupDir()+"), "):"")+"uninstalled the original + installed the SPOOF (shell restart: "+std::string(shellRestartName())+"). It loads where Haven 2025 does (unrooted can't switch envs); set Haven 2025 as your home if it isn't already. Restore: --restore-haven.")
                                : "  || spoof install FAILED (adb/device? Haven 2025 may be a non-removable system app - try the Rooted-System APK). Restore the original with --restore-haven if needed.";
                }
            } else {
                msg += rooted ? "  || ROOT but no system APK" : "  || no root and no spoof APK (enable the spoof toggle)";
            }
            // ── NEUTRALIZE VISTAS: after the home install, replace every installed vista package with an
            //    invisible (0-entity) environment so a force-paired vista renders nothing. Belt-and-suspenders
            //    for the "vista still shows behind my home" case the nuxd-manifest spoof doesn't fully cover.
            if (killVistas) {
                auto bs2=[](std::string p){
#ifdef _WIN32
                    for(char&c:p) if(c=='/')c='\\';
#endif
                    return p; };
                std::string ADB2=bs2(adbPath()), sel2 = adbSerial.empty()? "" : (" -s "+adbSerial);
                progress(0.9f, "neutralizing vistas");
                msg += "  || vistas: " + neutralizeVistasOnDevice(ADB2, sel2, rooted, progress);
            }
        }
        if (terminalBar) fprintf(stderr, "\n");
        // record the cook signature next to the APKs so "Install only" can detect an unchanged scene and skip
        // the re-cook next time. pendingCookSig was computed on the UI thread at cook start (the exact state
        // being cooked) — writing it here (worker) is a plain file write, no shared-state read.
        if ((!finalSystem.empty() || !finalSpoof.empty()) && !pendingCookSig.empty()) {
            if (FILE* f=fopen(cookSigPath().c_str(),"w")){ fputs(pendingCookSig.c_str(),f); fclose(f); }
        }
        setStatus(msg); setStage(1.f, "Done"); cooking.store(false);
    }
    std::string pendingCookSig;   // cook-input signature computed on the UI thread at cook start; runCook writes it on success
    static bool fileEx(const std::string& p){ FILE* f=fopen(p.c_str(),"rb"); if(f){ fclose(f); return true; } return false; }
    // adb resolution order: $HSR_ADB -> bundled beside the exe (adb.exe + AdbWinApi.dll + AdbWinUsbApi.dll, or a
    // platform-tools/ folder next to the renderer) -> the usual SDK path -> "adb" on PATH. Bundling those 3 files
    // beside the exe means users never need to install Android platform-tools.
    // A UI-set adb path (via "Browse for adb…"), persisted to adb_path.txt beside the exe.
    static std::string& adbUserSlot(){ static std::string s; return s; }
    static bool&        adbUserLoaded(){ static bool b=false; return b; }
    static std::string adbUserPath() {
        if (!adbUserLoaded()) { adbUserLoaded()=true;
            FILE* f=fopen(AppConfig::exeRel("adb_path.txt").c_str(),"rb");
            if(f){ char b[1024]; size_t n=fread(b,1,sizeof b-1,f); fclose(f); b[n]=0; adbUserSlot()=b;
                   while(!adbUserSlot().empty()&&(unsigned char)adbUserSlot().back()<=' ') adbUserSlot().pop_back(); } }
        return adbUserSlot();
    }
    static void setAdbUserPath(const std::string& p){
        adbUserSlot()=p; adbUserLoaded()=true;
        FILE* f=fopen(AppConfig::exeRel("adb_path.txt").c_str(),"wb"); if(f){ fwrite(p.data(),1,p.size(),f); fclose(f);} }

    // Resolve an adb binary: HSR_ADB -> UI-set path -> beside-the-exe (incl. auto-downloaded platform-tools/)
    // -> common SDK / SideQuest / Homebrew installs -> "adb" on PATH.
#ifdef _WIN32
    // Extract the adb bundled INSIDE the exe (the ADBWINZIP resource: adb.exe + AdbWin*.dll) to platform-tools/
    // beside the app. Instant + offline; used when no external adb is found. Returns the adb path, "" on failure.
    static std::string extractEmbeddedAdb(){
        namespace fs = std::filesystem;
        std::string exeDir = AppConfig::exeRel(""); if(!exeDir.empty()&&(exeDir.back()=='/'||exeDir.back()=='\\')) exeDir.pop_back();
        std::string adb = exeDir + "/platform-tools/adb.exe";
        if (fileEx(adb)) return adb;                                 // already extracted on a previous run
        HRSRC r = FindResourceA(nullptr, "ADBWINZIP", (LPCSTR)RT_RCDATA); if(!r) return "";
        HGLOBAL h = LoadResource(nullptr, r); DWORD sz = SizeofResource(nullptr, r); const void* p = h? LockResource(h): nullptr;
        if(!p || !sz) return "";
        mz_zip_archive z; memset(&z,0,sizeof z);
        if(!mz_zip_reader_init_mem(&z, p, sz, 0)) return "";
        mz_uint n = mz_zip_reader_get_num_files(&z); bool ok=true;
        for(mz_uint i=0;i<n;i++){ mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)){ ok=false; break; }
            if(mz_zip_reader_is_file_a_directory(&z,i)) continue;
            std::string outp = exeDir + "/" + st.m_filename; std::error_code ec; fs::create_directories(fs::path(outp).parent_path(), ec);
            if(!mz_zip_reader_extract_to_file(&z,i,outp.c_str(),0)){ ok=false; break; } }
        mz_zip_reader_end(&z);
        return (ok && fileEx(adb)) ? adb : "";
    }
#endif
    static std::string adbPath() {
        if (const char* a=std::getenv("HSR_ADB")) return a;
        std::string u=adbUserPath(); if (!u.empty() && fileEx(u)) return u;
        std::string e1=AppConfig::exeRel("adb.exe"), e2=AppConfig::exeRel("platform-tools/adb.exe"), e3=AppConfig::exeRel("adb");
        if (fileEx(e1)) return e1;
        if (fileEx(e2)) return e2;          // beside the exe, incl. the auto-downloaded platform-tools/
        if (fileEx(e3)) return e3;          // POSIX (Linux/macOS) bundled next to the binary
#ifdef _WIN32
        if (const char* la=std::getenv("LOCALAPPDATA")) { std::string L=la;
            for (const char* p : { "\\Android\\Sdk\\platform-tools\\adb.exe",
                                   "\\Programs\\SideQuest\\resources\\app.asar.unpacked\\build\\platform-tools\\adb.exe" })
                { std::string c=L+p; if (fileEx(c)) return c; } }
        if (const char* up=std::getenv("USERPROFILE")) { std::string U=up;
            for (const char* p : { "\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe",
                                   "\\SideQuest\\platform-tools\\adb.exe", "\\scrcpy\\adb.exe" })
                { std::string c=U+p; if (fileEx(c)) return c; } }
        if (fileEx("C:/Android/platform-tools/adb.exe")) return "C:/Android/platform-tools/adb.exe";
        if (fileEx("C:/platform-tools/adb.exe")) return "C:/platform-tools/adb.exe";
#else
        if (const char* h=std::getenv("HOME")) { std::string H=h;
            for (const char* p : { "/Android/Sdk/platform-tools/adb", "/Library/Android/sdk/platform-tools/adb",
                                   "/.config/SideQuest/platform-tools/adb",                        // SideQuest on Linux (Electron userData)
                                   "/Library/Application Support/SideQuest/platform-tools/adb" })  // SideQuest on macOS
                { std::string c=H+p; if (fileEx(c)) return c; } }
        for (const char* p : { "/usr/local/bin/adb", "/opt/homebrew/bin/adb", "/usr/bin/adb" }) if (fileEx(p)) return p;
#endif
        return "adb";   // last resort: PATH
    }
    // Does the resolved adb actually run? Cached (-1 unknown / 0 no / 1 yes); re-check after Download/Browse.
    int adbOk = -1;
    bool adbWorks(bool recheck=false){
        if (recheck) adbOk=-1;
        if (adbOk<0){ auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
            std::string v = adbCapture(bs(adbPath()), "", "version");
            adbOk = (v.find("Android Debug Bridge")!=std::string::npos) ? 1 : 0; }
        return adbOk==1;
    }
    int runAdb(const std::string& adb, const std::string& sel, const std::string& tail) {
        char cmd[1600];
#ifdef _WIN32
        snprintf(cmd, sizeof cmd, "\"\"%s\"%s %s\"", adb.c_str(), sel.c_str(), tail.c_str());   // cmd.exe strips ONE outer quote pair
#else
        snprintf(cmd, sizeof cmd, "\"%s\"%s %s", adb.c_str(), sel.c_str(), tail.c_str());
#endif
        return system(cmd);
    }
    // Run an adb command and CAPTURE its stdout+stderr (needed for getprop / id / pm path probing).
    static std::string adbCapture(const std::string& adb, const std::string& sel, const std::string& tail) {
        char cmd[1600];
#ifdef _WIN32
        snprintf(cmd, sizeof cmd, "\"\"%s\"%s %s 2>&1\"", adb.c_str(), sel.c_str(), tail.c_str());
        FILE* p = _popen(cmd, "r");
#else
        snprintf(cmd, sizeof cmd, "\"%s\"%s %s 2>&1", adb.c_str(), sel.c_str(), tail.c_str());
        FILE* p = popen(cmd, "r");
#endif
        if (!p) return "";
        std::string out; char b[512]; size_t n;
        while ((n = fread(b, 1, sizeof b, p)) > 0) out.append(b, n);
#ifdef _WIN32
        _pclose(p);
#else
        pclose(p);
#endif
        return out;
    }
    // Device PICKER: scan `adb devices -l` into adbDeviceList = {serial, "Model (serial)"} (ONLINE devices only).
    void refreshAdbDevices(){
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string out = adbCapture(bs(adbPath()), "", "devices -l");
        adbDeviceList.clear(); adbDevScanned = true;
        size_t s=0;
        while (s<out.size()){ size_t nl=out.find('\n',s); std::string ln=out.substr(s, nl==std::string::npos?std::string::npos:nl-s); s=(nl==std::string::npos)?out.size():nl+1;
            while(!ln.empty()&&(ln.back()=='\r'||ln.back()==' '||ln.back()=='\t')) ln.pop_back();
            if(ln.empty()||ln.rfind("List of",0)==0||ln.rfind("* daemon",0)==0||ln.rfind("adb",0)==0) continue;
            size_t sp=ln.find_first_of(" \t"); if(sp==std::string::npos) continue;
            std::string serial=ln.substr(0,sp), rest=ln.substr(sp);
            if(rest.find(" device")==std::string::npos && rest.find("\tdevice")==std::string::npos) continue;   // skip offline/unauthorized
            std::string model; size_t mp=rest.find("model:"); if(mp!=std::string::npos){ mp+=6; size_t me=rest.find_first_of(" \t",mp); model=rest.substr(mp, me==std::string::npos?std::string::npos:me-mp); for(char&c:model) if(c=='_')c=' '; }
            adbDeviceList.push_back({serial, model.empty()?serial:(model+"  ("+serial+")")});
        }
    }
    // Cycle adbSerial through the attached devices (+ the "default/only" empty state). Rescans if the list is empty.
    void cycleAdbDevice(){
        if (adbDeviceList.empty()) refreshAdbDevices();
        if (adbDeviceList.empty()) { adbSerial.clear(); return; }
        int idx=-1; for(size_t i=0;i<adbDeviceList.size();++i) if(adbDeviceList[i].first==adbSerial){ idx=(int)i; break; }
        ++idx; adbSerial = (idx>=(int)adbDeviceList.size()) ? std::string() : adbDeviceList[idx].first;
    }
    std::string adbDeviceLabel() const {
        if (adbSerial.empty()) return "(default / only device)";
        for (auto& d:adbDeviceList) if (d.first==adbSerial) return d.second;
        return adbSerial;
    }
    // True if the device's adb shell can act as root (su works, or adbd itself is root, or it's a userdebug build).
    // ROOT lets us install the proper own-package env and auto-select it via `oculuspreferences --setc`; without it
    // we fall back to the haven2025 spoof (which the user picks manually in the home menu).
    // TRUE iff an adb device is actually connected/online — checked via `adb get-state` (returns instantly: "device" when
    // online, "error: no devices/emulators found" otherwise). Gate the whole install flow on this so a cook with NO device
    // connected SKIPS the device steps cleanly instead of HANGING on `adb wait-for-device` (which blocks forever). Generic:
    // applies to every env cook (the APK files are still written; only the optional auto-install is skipped).
    bool deviceConnected() {
        if (std::getenv("HSR_NOINSTALL")) return false;   // headless test cooks: write the APKs, never touch the user's Quest
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        if (!wifiIp.empty()) { std::string ip=wifiIp; if(ip.find(':')==std::string::npos) ip+=":5555"; runAdb(ADB,"","connect "+ip); }
        std::string st = adbCapture(ADB, sel, "get-state");
        return st.find("error")==std::string::npos && st.find("no devices")==std::string::npos
            && st.find("offline")==std::string::npos && st.find("device")!=std::string::npos;
    }
    bool deviceIsRooted() {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        runAdb(ADB, sel, "root");                                   // best-effort: restart adbd as root (no-op on retail builds)
        if (!wifiIp.empty()) { std::string ip=wifiIp; if(ip.find(':')==std::string::npos) ip+=":5555"; runAdb(ADB,"","connect "+ip); }
        else runAdb(ADB, sel, "wait-for-device");                   // adbd may have restarted
        if (adbCapture(ADB, sel, "shell id").find("uid=0") != std::string::npos)        return true;   // adbd is root
        if (adbCapture(ADB, sel, "shell su -c id").find("uid=0") != std::string::npos)  return true;   // su available
        return adbCapture(ADB, sel, "shell getprop ro.debuggable").find('1') != std::string::npos;     // userdebug/eng
    }
    // ── Haven 2025 backup/restore ───────────────────────────────────────────────────────────────────────────
    static const char* HAVEN_PKG() { return "com.meta.shell.env.footprint.haven2025"; }
    // ONE canonical pristine backup, in a clearly-named FOLDER beside the EXE (not per-output-folder). Why a single
    // beside-the-exe spot: the FIRST spoof install must capture the REAL haven2025; a per-folder backup would, on a
    // later env, save the previous env's already-installed spoof as "the original" and lose the real one.
    static std::string havenBackupDir() { return AppConfig::s_exeDir.empty() ? std::string("Haven2025_Backup") : AppConfig::exeRel("Haven2025_Backup"); }
    static std::string havenBackupApk() { return havenBackupDir() + "/haven2025_ORIGINAL_backup.apk"; }
    enum HavenBkp { HB_ABSENT, HB_OK, HB_FAILED };   // not installed / backed-up (or already had one) / pull failed
    // The backup is the FULL split set (base.apk + split_config.*.apk) in the folder; haven2025 is a split APK on
    // modern Quest, so pulling only base.apk restores a BROKEN home. Returned base-first; legacy single-file last.
    static std::vector<std::string> havenBackupApks() {
        namespace fs=std::filesystem; std::error_code ec; std::string dir=havenBackupDir(); std::vector<std::string> v;
        if (fs::is_directory(dir, ec)) for (auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension()==".apk" && e.path().filename()!="haven2025_ORIGINAL_backup.apk") v.push_back(e.path().string());
        std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b){
            return (int)(a.find("base.apk")!=std::string::npos) > (int)(b.find("base.apk")!=std::string::npos); });   // base first
        if (v.empty() && fileEx(havenBackupApk())) v.push_back(havenBackupApk());   // legacy single-file backup
        return v;
    }
    // Install the backup set, CAPTURING adb output. `adb install` can exit 0 yet print "Failure", so the old
    // exit-code check falsely reported "Restored". On a signature/downgrade Failure (our spoof is installed),
    // uninstall it and clean-install. Returns true only if adb actually printed Success.
    static bool installHavenBackup(const std::string& ADB, const std::string& sel, const std::vector<std::string>& apks, std::string& log) {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        auto build=[&](const char* verb){ std::string t=verb; for(auto&a:apks) t+=" \""+bs(a)+"\""; return t; };
        log = adbCapture(ADB, sel, build(apks.size()>1 ? "install-multiple -r -d -g" : "install -r -d -g"));
        if (log.find("Success")!=std::string::npos) return true;
        adbCapture(ADB, sel, std::string("uninstall ")+HAVEN_PKG());               // spoof has a different signature -> remove it
        log += "\n" + adbCapture(ADB, sel, build(apks.size()>1 ? "install-multiple -g" : "install -g"));
        return log.find("Success")!=std::string::npos;
    }
    // Back up the REAL haven2025 off the device BEFORE anything is uninstalled. Keeps the first/pristine backup;
    // never overwrites it. Pulls the FULL split set. Writes a HOW_TO_RESTORE.txt and reports the folder.
    HavenBkp backupOriginalHaven(std::string& outBkp) {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        std::string dir = havenBackupDir(); outBkp = havenBackupApk();
        if (fileEx(outBkp) || !havenBackupApks().empty()) { fprintf(stderr, "[COOK] Haven 2025 backup already exists (pristine, kept): %s\n", dir.c_str()); return HB_OK; }
        std::string out = adbCapture(ADB, sel, std::string("shell pm path ")+HAVEN_PKG());
        if (out.find("package:") == std::string::npos) { fprintf(stderr, "[COOK] Haven 2025 not installed on device - nothing to back up\n"); outBkp.clear(); return HB_ABSENT; }
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        std::vector<std::string> pulled; size_t pos=0;                              // pull EVERY split (base + split_config.*)
        while ((pos=out.find("package:", pos)) != std::string::npos) {
            pos+=8; size_t e=out.find_first_of("\r\n", pos);
            std::string dev=out.substr(pos, e==std::string::npos?std::string::npos:e-pos);
            while(!dev.empty()&&(dev.back()=='\r'||dev.back()=='\n'||dev.back()==' '||dev.back()=='\t')) dev.pop_back();
            if(!dev.empty()){ std::string base=dev.substr(dev.find_last_of('/')+1), loc=dir+"/"+base;
                runAdb(ADB,sel,"pull \""+dev+"\" \""+bs(loc)+"\""); if(fileEx(loc)) pulled.push_back(loc); }
            if(e==std::string::npos) break; pos=e;
        }
        if (pulled.empty()) { fprintf(stderr, "[COOK] WARN: Haven 2025 backup pull FAILED\n"); outBkp.clear(); return HB_FAILED; }
        std::filesystem::copy_file(pulled[0], outBkp, std::filesystem::copy_options::overwrite_existing, ec);   // legacy single-file marker = base.apk
        if (FILE* rf = fopen((dir+"/HOW_TO_RESTORE.txt").c_str(), "wb")) {
            fputs("This folder holds a backup of the ORIGINAL Meta \"Haven 2025\" home (base.apk + any split_config.*.apk),\n"
                  "taken automatically before the converter replaced it with a spoofed (custom) home.\n\n"
                  "RESTORE the original Haven 2025:\n"
                  "  \"Quest Home Editor.exe\" --restore-haven    (or the editor's \"Restore original Haven 2025\" button)\n\n"
                  "Do NOT delete this folder - it is the only copy of your original Haven 2025.\n"
                  "If you ever lose it, re-download Haven 2025 from Meta (headset Settings) or factory-reset.\n", rf);
            fclose(rf);
        }
        fprintf(stderr, "[COOK] Backed up REAL Haven 2025 (%zu apk) -> %s  (restore: \"Quest Home Editor\" --restore-haven)\n", pulled.size(), dir.c_str());
        return HB_OK;
    }
    // RESTORE the original Haven 2025 from the backup (button + --restore-haven CLI share installHavenBackup).
    // MANUAL uninstall: rip out whatever Haven 2025 is installed (spoof or a different Meta version). No root needed
    // if it's a store/updatable app; a non-removable SYSTEM install can only be disabled (needs root to fully remove).
    void uninstallHaven() {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial), pkg = HAVEN_PKG();
        if (!adbWorks(true)) { setStatus("adb not found - open the Logcat tab (it auto-downloads adb), or Browse for it."); return; }
        std::string prev = pkgVersion(ADB, sel, pkg);
        if (prev.empty()) { setStatus("Haven 2025 ("+pkg+") is not installed - nothing to uninstall."); return; }
        setStatus("Uninstalling Haven 2025 ("+pkg+", version "+prev+")...");
        runAdb(ADB, sel, "uninstall "+pkg);
        if (pkgInstalled(ADB, sel, pkg)) adbCapture(ADB, sel, "shell pm uninstall --user 0 "+pkg);
        if (pkgInstalled(ADB, sel, pkg)) adbCapture(ADB, sel, "shell cmd package uninstall --user 0 "+pkg);
        bool gone = !pkgInstalled(ADB, sel, pkg);
        relaunchShell(ADB, sel);
        setStatus(gone ? "Uninstalled Haven 2025 (was version "+prev+"). Re-cook + install, or Restore original Haven 2025."
                       : "Couldn't fully remove Haven 2025 - it's a non-removable SYSTEM app on this headset (needs root). It's been disabled for the user where possible.");
    }
    // ── INVISIBLE VISTA: neutralize the shell's force-paired companion vista ─────────────────────────
    // List every INSTALLED com.meta.shell.env.vista.* package. The boot resolver (IsDefaultVistaInstalled,
    // a NAME check) pairs one of these with our haven2025 spoof independent of the env's footprint flag, so
    // on some devices a vista still shows behind/instead of the ported home. No root needed to read.
    std::vector<std::string> listInstalledVistas(const std::string& ADB, const std::string& sel) {
        std::vector<std::string> out;
        std::string s = adbCapture(ADB, sel, "shell pm list packages com.meta.shell.env.vista");
        size_t p=0; while (p<s.size()){ size_t nl=s.find('\n',p); std::string ln=s.substr(p, nl==std::string::npos?std::string::npos:nl-p); p=(nl==std::string::npos)?s.size():nl+1;
            while(!ln.empty()&&(ln.back()=='\r'||ln.back()==' '||ln.back()=='\t')) ln.pop_back();
            size_t kp=ln.find("package:"); if(kp==std::string::npos) continue;
            std::string pkg=ln.substr(kp+8);
            if (pkg.rfind("com.meta.shell.env.vista.",0)==0 && pkg.find(' ')==std::string::npos) out.push_back(pkg);
        }
        return out;
    }
    // Replace EVERY installed vista package with an INVISIBLE (0-entity) environment so any vista the shell
    // force-pairs renders nothing. Only /data (updatable) vistas can be replaced unrooted; a non-removable
    // system vista is DETECTED and reported, never forced. Returns a one-line summary (+ per-pkg detail).
    std::string neutralizeVistasOnDevice(const std::string& ADB, const std::string& sel, bool rooted,
                                         const std::function<void(float,const char*)>& progress) {
        if (progress) progress(0.0f, "detecting installed vistas...");
        std::vector<std::string> vistas = listInstalledVistas(ADB, sel);
        if (vistas.empty()) return "no com.meta.shell.env.vista.* packages installed (nothing to neutralize)";
        std::string tmpU = AppConfig::exeRel("_invisible_vista.unsigned.apk");
        std::string tmpS = AppConfig::exeRel("_invisible_vista.apk");
        std::string tmpP = AppConfig::exeRel("_vista_pulled.apk");
        int done=0, sysBlocked=0, failed=0; std::string detail;
        const size_t NV = vistas.size();
        for (size_t vi=0; vi<NV; ++vi) {
            const std::string& vp = vistas[vi];
            // progress advances per vista across [0..1]; the label names the vista + its index.
            if (progress) progress((float)vi/(float)NV, ("vista "+std::to_string(vi+1)+"/"+std::to_string(NV)+": "+vp).c_str());
            std::string path = adbCapture(ADB, sel, "shell pm path "+vp);   // package:/data/app/.../base.apk
            bool systemApp = path.find("/system/")!=std::string::npos || path.find("/product/")!=std::string::npos
                          || path.find("/vendor/")!=std::string::npos || path.find("/system_ext/")!=std::string::npos;
            // PRIMARY: pull the REAL installed vista APK and empty ONLY its scene ("whatever the vista has,
            // but an empty scene") — keeps its exact manifest/package/resources. FALLBACK: synthesize from
            // the nuxd donor if the pull or the base-apk parse fails.
            std::vector<uint8_t> apk; bool ok2=false;
            std::string basePath; { size_t p=path.find("package:"); while(p!=std::string::npos){ size_t e=path.find_first_of("\r\n",p); std::string ln=path.substr(p+8, e==std::string::npos?std::string::npos:e-(p+8));
                while(!ln.empty()&&(ln.back()=='\r'||ln.back()==' '))ln.pop_back();
                if (ln.size()>4 && ln.substr(ln.size()-9)=="/base.apk"){ basePath=ln; break; } if(basePath.empty()&&ln.size()>4&&ln.substr(ln.size()-4)==".apk") basePath=ln;
                p = e==std::string::npos?std::string::npos:path.find("package:",e); } }
            if (!basePath.empty()) {
                std::remove(tmpP.c_str());
                runAdb(ADB, sel, "pull \""+basePath+"\" \""+tmpP+"\"");
                std::vector<uint8_t> real; { FILE* f=fopen(tmpP.c_str(),"rb"); if(f){ fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ real.resize((size_t)n); if(fread(real.data(),1,(size_t)n,f)!=(size_t)n) real.clear(); } fclose(f); } }
                if (!real.empty()) apk = hslcook::emptyOutVistaApk(real, &ok2);
            }
            std::remove(tmpP.c_str());
            if (!ok2 || apk.empty()) apk = hslcook::buildInvisibleVista(vp, &ok2);   // fallback: synthesized empty env
            if (!ok2 || apk.empty()) { ++failed; detail += "\n  - "+vp+": build failed"; continue; }
            if (!writeFile(tmpU, apk)) { ++failed; detail += "\n  - "+vp+": temp write failed"; continue; }
            bool signed_ = hslcook::signApk(tmpU, tmpS, progress);
            std::remove(tmpU.c_str());
            if (!signed_) { ++failed; detail += "\n  - "+vp+": sign failed"; continue; }
            bool inst = installToDevice(tmpS, vp, progress, /*uninstallFirst=*/true, rooted);
            if (inst)                      { ++done;       detail += "\n  - "+vp+": replaced with an invisible (empty) vista"; }
            else if (systemApp && !rooted) { ++sysBlocked; detail += "\n  - "+vp+": SYSTEM app (in "+ (path.find("/product/")!=std::string::npos?"/product":"/system") +", needs root to replace) - left as-is"; }
            else                           { ++failed;    detail += "\n  - "+vp+": install failed"; }
        }
        if (progress) progress(1.0f, "vistas neutralized");
        std::remove(tmpS.c_str());
        std::string sum = std::to_string(done)+"/"+std::to_string(vistas.size())+" neutralized";
        if (sysBlocked) sum += ", "+std::to_string(sysBlocked)+" are system apps (need root)";
        if (failed)     sum += ", "+std::to_string(failed)+" failed";
        return sum + detail;
    }
    // Standalone button entry (off the UI thread): neutralize vistas without a full cook.
    std::atomic<bool> vistaBusy{false};   // true while a vista-neutralize runs (shows the progress bar vs a plain label)
    void neutralizeVistasButton() {
        if (restoring.exchange(true)) return;
        if (restoreThread.joinable()) restoreThread.join();
        vistaBusy.store(true); cookProg.store(0.f);
        restoreThread = std::thread([this]{
            auto bs=[](std::string p){
#ifdef _WIN32
                for(char&c:p) if(c=='/')c='\\';
#endif
                return p; };
            std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
            if (!adbWorks(true)) { setStatus("adb not found - open the Logcat tab (it auto-downloads adb) or Browse for it."); vistaBusy.store(false); restoring.store(false); return; }
            bool rooted = deviceIsRooted();
            auto prog=[this](float f,const char* s){ setStage(f, s); };
            std::string r = neutralizeVistasOnDevice(ADB, sel, rooted, prog);
            relaunchShell(ADB, sel);
            setStage(1.f, "Done");
            setStatus("Neutralize vistas: "+r);
            vistaBusy.store(false); restoring.store(false);
        });
    }
    void restoreHaven() {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        std::vector<std::string> apks = havenBackupApks();
        if (apks.empty()) { setStatus("No Haven 2025 backup found in "+havenBackupDir()+" - nothing to restore (a backup is made automatically the first time you install a spoof)."); return; }
        std::string log; bool ok = installHavenBackup(ADB, sel, apks, log);
        // restore has NO environment_selected IPC to piggyback on — the restart IS how the restored
        // original shows up — so honor only an explicit "Never" here.
        if (ok && shellRestart != 2) relaunchShell(ADB, sel);
        std::string first = log.substr(0, log.find_first_of("\r\n"));
        setStatus(ok ? ("Restored the ORIGINAL Haven 2025 ("+std::to_string(apks.size())+" apk"+std::string(apks.size()>1?"s":"")+") + relaunched shell.")
                     : ("Restore FAILED: "+(first.empty()?std::string("is the headset connected? (adb devices)"):first)+"  Backup kept in "+havenBackupDir()+"."));
    }
    // Static CLI restore (no Editor instance / no serial) for `"Quest Home Editor" --restore-haven`.
    static int cliRestoreHaven() {
        std::string ADB=adbPath(); for(char&c:ADB) if(c=='/')c='\\';
        std::vector<std::string> apks = havenBackupApks();
        if (apks.empty()) { fprintf(stderr, "[RESTORE] no backup in %s - nothing to restore.\n", havenBackupDir().c_str()); return 1; }
        std::string log; bool ok = installHavenBackup(ADB, "", apks, log);
        if (ok) {   // relaunch the shell so the restored home loads
            std::string pids = adbCapture(ADB, "", "shell pidof com.oculus.vrshell"); std::string pid;
            for (char c : pids){ if(c=='\r'||c=='\n') break; if(c!=' '&&c!='\t') pid.push_back(c); }
            if (!pid.empty()){ char k[256]; snprintf(k,sizeof k,"\"\"%s\" shell kill %s\"", ADB.c_str(), pid.c_str()); system(k); }
        }
        if (ok) fprintf(stderr, "[RESTORE] restored ORIGINAL Haven 2025 (%zu apk) from %s + relaunched shell\n", apks.size(), havenBackupDir().c_str());
        else    fprintf(stderr, "[RESTORE] FAILED. adb log:\n%s\nBackup kept in %s\n", log.c_str(), havenBackupDir().c_str());
        return ok ? 0 : 1;
    }
    // Connect wireless adb to wifiIp (e.g. "192.168.1.35[:5555]"); call before installing over Wi-Fi.
    bool wifiConnect() {
        if (wifiIp.empty()) return false;
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), ip=wifiIp; if (ip.find(':')==std::string::npos) ip+=":5555";
        bool ok = runAdb(ADB, "", "connect "+ip)==0;
        if (ok && adbSerial.empty()) adbSerial=ip;   // target it for the install
        setStatus(ok?("Wi-Fi adb connected: "+ip):("Wi-Fi connect FAILED: "+ip));
        return ok;
    }
    // Install an APK. uninstallFirst = overlay/spoof case: the existing package (Meta's Haven 2025) is signed with a
    // DIFFERENT certificate than our debug-signed spoof, so Android refuses an in-place update — it MUST be uninstalled
    // first. (The caller backs it up before allowing this.) Then best-effort select + relaunch.
    // True if `pkg` is still installed for user 0 (used to tell a real uninstall from a no-op on a system app).
    bool pkgInstalled(const std::string& ADB, const std::string& sel, const std::string& pkg) {
        return adbCapture(ADB, sel, "shell pm path "+pkg).find("package:") != std::string::npos;
    }
    // versionCode (+versionName) of an installed package, "" if not installed. Parses the FULL `dumpsys package`
    // in C++ (no on-device `| grep` — that pipe would run in Windows cmd, not the device shell).
    std::string pkgVersion(const std::string& ADB, const std::string& sel, const std::string& pkg) {
        std::string d = adbCapture(ADB, sel, "shell dumpsys package "+pkg);
        if (d.find("versionCode=")==std::string::npos && d.find("Unable to find")!=std::string::npos) return "";
        auto grab=[&](const char* key)->std::string{ size_t p=d.find(key); if(p==std::string::npos) return ""; p+=strlen(key);
            std::string v; while(p<d.size() && d[p]!=' '&&d[p]!='\r'&&d[p]!='\n'&&d[p]!='\t') v+=d[p++]; return v; };
        std::string vc=grab("versionCode="), vn=grab("versionName=");
        if (vc.empty() && vn.empty()) return "";
        return vc.empty()? vn : (vc + (vn.empty()?"":(" / "+vn)));
    }
    bool installToDevice(const std::string& apkPath, const std::string& pkg, const std::function<void(float,const char*)>& progress, bool uninstallFirst=false, bool rooted=false) {
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), AP=bs(apkPath), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        if (progress) progress(0.95f, "adb install");
        if (uninstallFirst) {
            // The spoof is signed with OUR cert, not Meta's -> an in-place update over ANY existing (differently-signed)
            // Haven 2025 is impossible. DETECT whatever version is installed and remove it up front for a clean install.
            std::string prev = pkgVersion(ADB, sel, pkg);
            if (!prev.empty()) {
                if (progress) progress(0.95f, "removing existing Haven 2025");
                setStatus("Detected an installed "+pkg+" (version "+prev+") - uninstalling it so the spoof installs cleanly.");
                runAdb(ADB, sel, "uninstall "+pkg);                                                        // store/updatable app
                if (pkgInstalled(ADB, sel, pkg)) adbCapture(ADB, sel, "shell pm uninstall --user 0 "+pkg); // per-user remnant / disable-update
                if (pkgInstalled(ADB, sel, pkg)) adbCapture(ADB, sel, "shell cmd package uninstall --user 0 "+pkg); // newer API fallback
                // if STILL installed here it's a non-removable SYSTEM app -> the install below fails and we report it.
            }
        }
        std::string ilog = adbCapture(ADB, sel, "install -r -d -g \""+AP+"\"");   // -d downgrade, -g grant perms
        bool ok = ilog.find("Success") != std::string::npos;
        if (!ok && uninstallFirst) {   // report the ACTUAL adb reason so the user knows why THIS unit failed
            std::string first = ilog.substr(0, ilog.find_first_of("\r\n"));
            setStatus("Install FAILED on this unit: " + (first.empty()?std::string("(no adb output — device/storage?)"):first));
            return false;
        }
        if (!ok && !uninstallFirst){ runAdb(ADB, sel, "uninstall "+pkg); ok = adbCapture(ADB, sel, "install -g \""+AP+"\"").find("Success")!=std::string::npos; }   // own-package sig/version clash fallback
        if (!ok) return false;
        if (progress) progress(0.98f, "select env");
        // Set the home to this package. DEVICE-PROVEN reality: the shell reads environment_selected from the ROOT-ONLY
        // oculuspreferences prefs DB — `settings put secure environment_selected` writes a SEPARATE copy the shell
        // IGNORES (verified: secure=X, DB=Y -> shell loads Y), and the PreferencesService binder no-ops for shell.
        // So a real env-SELECT needs root. UNROOTED users don't select here at all: the SPOOF replaces the content of
        // the already-selected haven2025 (the shell loads that package = our content), so no pref write is needed —
        // the port shows wherever Haven 2025 does. (Rooted own-package installs DO select via su below.)
        runAdb(ADB, sel, "shell su -c \"oculuspreferences --setc environment_selected apk://"+pkg+"/assets/scene.zip\"");   // rooted only
        // Shell restart is an OPTION (Auto=only unrooted / Always / Never). UNROOTED needs it (nothing else reloads the
        // replaced haven2025); the reload is `am force-stop` (kill is denied to uid 2000) — see relaunchShell.
        if (shellRestart==1 || (shellRestart==0 && !rooted)) {
            if (progress) progress(0.99f, loadDiag ? "relaunch + diagnose load" : "relaunch shell");
            if (loadDiag) { runAdb(ADB, sel, "shell setprop debug.logLevel Verbose"); runAdb(ADB, sel, "logcat -c"); }   // raise verbosity + clear (no root)
            relaunchShell(ADB, sel);
            if (loadDiag) setStatus(captureEnvDiagnostics(ADB, sel, pkg, apkPath+".loaddiag.txt"));   // NO-ROOT: read WHY it loaded/rejected
        }
        return true;
    }
    const char* shellRestartName() const { return shellRestart==1 ? "Always" : shellRestart==2 ? "Never" : "Auto"; }
    // Make the running shell pick up the freshly-installed env by restarting com.oculus.vrshell (Android relaunches
    // the home automatically). Rooted: kill the exact pid. UNROOTED: `adb shell kill` CANNOT kill vrshell (it runs as
    // a different uid -> "Operation not permitted"), so the old code silently did nothing on retail headsets ("the
    // restarter doesn't work"). The reliable no-root path is `am force-stop com.oculus.vrshell` — the ActivityManager
    // grants the shell user force-stop for debugging, and stopping the home makes Android restart it, reloading the
    // freshly-installed env. (Targeted at the exact package; NOT a broad pkill, which reboots the headset.)
    void relaunchShell(const std::string& ADB, const std::string& sel) {
        std::string pids = adbCapture(ADB, sel, "shell pidof com.oculus.vrshell");
        std::string pid; for (char c : pids) { if (c=='\r'||c=='\n') break; pid.push_back(c); }   // 1st line = space-sep pids of the exact pkg
        while (!pid.empty() && (pid.back()==' '||pid.back()=='\t')) pid.pop_back();
        // DEVICE-PROVEN (uid 2000 shell): `adb shell kill <vrshell pid>` FAILS with "Operation not permitted"
        // (vrshell runs as a different uid), so a plain kill silently does nothing on a retail/unrooted Quest.
        // `am force-stop com.oculus.vrshell` is the reliable NO-ROOT reload — the ActivityManager allows the shell
        // user to force-stop, and Android relaunches the home, which re-reads environment_selected. Use it always;
        // a rooted su-kill of the exact pid is a cleaner bonus when available.
        if (!pid.empty()) runAdb(ADB, sel, "shell su -c \"kill "+pid+"\"");   // rooted (best-effort, cleaner)
        runAdb(ADB, sel, "shell am force-stop com.oculus.vrshell");           // universal no-root reload
    }
    // ── NO-ROOT env-load diagnostics ("why did my home get rejected -> nuxd?") ──────────────────────────────────
    // libshell logs its ENTIRE EnvironmentSystem/EnvironmentManager load path to logcat (via __android_log_write,
    // tag "Clay"/"Shell"), and the reject reasons are emitted UNCONDITIONALLY at error level. `adb logcat` needs
    // NO ROOT (the shell uid holds READ_LOGS), so on a retail/unrooted Quest we can read exactly why the env failed.
    // We ALSO raise `debug.logLevel Verbose` (a shell-settable debug prop libshell reads at process init) for extra
    // context. The EXACT strings below are lifted from libshell.so v206 (IDA): the fallback marker @0x2091c6, the
    // manager reason @0xa73c0, and the specific causes (Asset ptr error @0xae208, Failed-to-find-in-zip @0x208f1b,
    // LoadEntry-not-found @0x20922c, MeshDefinition/MaterialDefinition::fix, manifest FourCc/version, etc.).
    // Writes the full filtered env-load log to <apk>.loaddiag.txt and returns a one-line verdict for the status bar.
    std::string captureEnvDiagnostics(const std::string& ADB, const std::string& sel, const std::string& pkg, const std::string& diagPath) {
        // The lines that matter (substring match against each logcat line).
        static const std::vector<std::string> KEEP = {
            "EnvironmentSystem:", "EnvironmentManager:", "SetSystemEnvironment resolved", "AssetHandler",
            "falling back to device default", "using fallback environment", "postInitAsset",
            "MeshDefinition::fix", "MaterialDefinition::fix", "Failed to deserialize asset manifest",
            "Skipping entitlement check", "checkShellEnvironmentRequirements", "as Environment", "as Footprint",
            "as Vista", "Scene loaded", "Hsr environment loaded", "Failed to load asset", "Asset ptr error",
            "unable to open package", "LoadEntry not found", "in zip from", "package type not supported",
            "package path not valid", "ClayPackage",
        };
        // Poll the log until a terminal marker lands (success or fallback), or ~12s.
        std::string dump;
        for (int i = 0; i < 16; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            dump = adbCapture(ADB, sel, "logcat -d -v time -t 6000");   // -d = dump+exit; no root needed (-v time: chronological + unambiguous)
            if (dump.find("Scene loaded") != std::string::npos ||
                dump.find("falling back to device default") != std::string::npos ||
                dump.find("using fallback environment") != std::string::npos) break;
        }
        // Filter to the env-load lines.
        std::string filtered; size_t start = 0;
        while (start < dump.size()) {
            size_t nl = dump.find('\n', start); std::string line = dump.substr(start, nl==std::string::npos?std::string::npos:nl-start);
            start = (nl==std::string::npos) ? dump.size() : nl+1;
            for (const auto& k : KEEP) if (line.find(k) != std::string::npos) { filtered += line; if(!filtered.empty()&&filtered.back()!='\n') filtered += '\n'; break; }
        }
        // Persist the full filtered log for the user to share.
        if (!diagPath.empty()) { std::ofstream f(diagPath, std::ios::binary); if (f) f << "# env-load diagnostics (adb logcat, no root) for " << pkg << "\n" << filtered; }
        // Classify a one-line verdict. Pull the first concrete REASON line if it fell back.
        auto has = [&](const char* s){ return dump.find(s) != std::string::npos; };
        auto firstReason = [&]() -> std::string {
            static const char* CAUSES[] = { "Asset ptr error", "unable to open package", "in zip from",
                "LoadEntry not found", "package type not supported", "package path not valid",
                "Failed to load hsr", "Failed to deserialize asset manifest", "MeshDefinition::fix",
                "MaterialDefinition::fix", "postInitAsset", "reason:" };
            size_t s2 = 0;
            while (s2 < filtered.size()) { size_t nl = filtered.find('\n', s2); std::string ln = filtered.substr(s2, nl==std::string::npos?std::string::npos:nl-s2); s2 = (nl==std::string::npos)?filtered.size():nl+1;
                for (auto c : CAUSES) if (ln.find(c) != std::string::npos) {
                    size_t p = ln.find("EnvironmentSystem"); if (p==std::string::npos) p = ln.find("EnvironmentManager");
                    return p==std::string::npos ? ln : ln.substr(p); } }
            return "";
        };
        if (has("falling back to device default") || has("using fallback environment")) {
            std::string r = firstReason();
            return "REJECTED -> nuxd fallback. " + (r.empty() ? std::string("see ")+diagPath : r);
        }
        if (has("Scene loaded") || has("Hsr environment loaded")) return "Home loaded OK on device (Scene loaded).";
        if (filtered.empty()) return "No env-load logs captured (device asleep, or shell didn't reload). Wake the Quest + retry.";
        return std::string("Load inconclusive - see ") + diagPath;
    }
    // ── LIVE no-root logcat stream for the Logcat page ──────────────────────────────────────────────────────────
    static uint8_t logSevOf(const std::string& ln){
        static const char* HL[] = { "EnvironmentSystem", "EnvironmentManager", "falling back to device default",
            "using fallback environment", "postInitAsset", "MeshDefinition::fix", "MaterialDefinition::fix",
            "Asset ptr error", "unable to open package", "LoadEntry not found", "Scene loaded",
            "Hsr environment loaded", "package type not supported", "in zip from", "deserialize asset manifest" };
        for (auto h : HL) if (ln.find(h)!=std::string::npos) return 4;   // env-load highlight
        // Priority letter = the char right before the first '/' — works for BOTH `-v brief`
        // ("E/Tag(pid): msg") and `-v time` ("MM-DD HH:MM:SS.mmm E/Tag(pid): msg"; the date uses
        // '-' and the time ':', so the first '/' is always the priority separator).
        char p = 'I';
        { size_t sl = ln.find('/'); if (sl != std::string::npos && sl >= 1) p = ln[sl-1]; }
        return p=='E'||p=='F'?3 : p=='W'?2 : p=='I'?1 : 0;
    }
    // Timestamp prefix of a `-v time` logcat line ("MM-DD HH:MM:SS.mmm", 18 chars); "" if absent.
    // Timestamps make the stream-merge deterministic: when the exact anchor line isn't found in the
    // next dump (buffer wrapped, --pid scope changed), we append ONLY lines newer than the last one
    // instead of re-appending the whole dump (which duplicated + interleaved lines in the export).
    static std::string logTsOf(const std::string& ln){
        if (ln.size() >= 18 && isdigit((unsigned char)ln[0]) && isdigit((unsigned char)ln[1]) && ln[2]=='-'
            && ln[5]==' ' && ln[8]==':' && ln[11]==':' && ln[14]=='.') return ln.substr(0, 18);
        return std::string();
    }
    static bool isEnvLine(const std::string& ln){
        static const char* K[] = {"EnvironmentSystem","EnvironmentManager","SetSystemEnvironment","AssetHandler",
            "falling back","using fallback","postInitAsset","MeshDefinition","MaterialDefinition","asset manifest",
            "entitlement","checkShellEnvironmentRequirements","as Environment","as Footprint","as Vista","Scene loaded",
            "Hsr environment","Failed to load","Asset ptr error","unable to open package","LoadEntry not found",
            "in zip from","package type not supported","ClayPackage","SystemComposer"};
        for (auto k:K) if (ln.find(k)!=std::string::npos) return true; return false;
    }
    void startLogStream(){
        if (logRun.exchange(true)) return;
        logThread = std::thread([this]{
            auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
            std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
            // AUTOMATIC FULL VERBOSE: raise libshell's log threshold once (no root), so the log has maximum detail.
            runAdb(ADB, sel, "shell setprop debug.logLevel Verbose"); logVerboseSet.store(true);
            while (logRun.load()){
                std::string pid; { std::string pids=adbCapture(ADB, sel, "shell pidof com.oculus.vrshell"); for(char c:pids){ if(c=='\r'||c=='\n'||c==' ')break; pid.push_back(c);} }
                // Mode 2 grabs the whole buffer; mode 0/1 scope to the vrshell process (--pid) so the
                // ring buffer stays vrshell-focused. While vrshell is mid-restart (no pid yet) SKIP the
                // poll instead of dumping everything: mixing an unscoped dump into a scoped stream is
                // exactly what interleaved foreign lines into the exported log. The new process's early
                // lines aren't lost - the next scoped -t 8000 dump still contains them.
                if (logMode.load()!=2 && pid.empty()){ for (int i=0;i<8 && logRun.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
                // `-v time` (not brief): the timestamp makes every line unique enough to merge the
                // rolling dumps deterministically - and the export becomes provably chronological.
                std::string cmd = (logMode.load()==2) ? "logcat -d -v time -t 8000"
                                                      : ("logcat -d -v time --pid="+pid+" -t 8000");
                std::string dump = adbCapture(ADB, sel, cmd);
                std::vector<std::string> lines; { size_t s=0; while(s<dump.size()){ size_t nl=dump.find('\n',s); std::string l=dump.substr(s, nl==std::string::npos?std::string::npos:nl-s); while(!l.empty()&&l.back()=='\r')l.pop_back(); if(!l.empty())lines.push_back(l); s=(nl==std::string::npos)?dump.size():nl+1; } }
                // Merge: find the exact last-appended line (anchor) and take what follows. If the anchor
                // is gone (buffer wrapped / scope changed), fall back to the TIMESTAMP: append only lines
                // strictly newer than the last appended one. (The old code re-appended the ENTIRE dump on
                // an anchor miss -> thousands of duplicated, out-of-order lines "mixed in each".)
                size_t startIdx=0; bool anchored=false;
                if(!logLastRaw.empty()){ for(size_t i=lines.size(); i-->0;){ if(lines[i]==logLastRaw){ startIdx=i+1; anchored=true; break; } } }
                { std::lock_guard<std::mutex> lk(logMx);
                  for (size_t i=startIdx;i<lines.size();++i){ const std::string& ln=lines[i];
                      if (!anchored && !logLastTs.empty()){ std::string ts=logTsOf(ln); if (!ts.empty() && ts<=logLastTs) continue; }
                      logBuf.push_back({ln, logSevOf(ln), isEnvLine(ln)}); }
                  while (logBuf.size()>12000) logBuf.pop_front(); }
                if (!lines.empty()){ logLastRaw = lines.back();
                    for (size_t i=lines.size(); i-->0;){ std::string ts=logTsOf(lines[i]); if(!ts.empty()){ logLastTs=ts; break; } } }
                for (int i=0;i<8 && logRun.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    void stopLogStream(){ logRun.store(false); if(logThread.joinable()) logThread.join(); }
    // Reload the home (no root) so the FRESH env load streams into the log; raise verbosity + clear first.
    void reloadShellForLog(){
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        runAdb(ADB, sel, "shell setprop debug.logLevel Verbose");
        { std::lock_guard<std::mutex> lk(logMx); logBuf.clear(); logLastRaw.clear(); logLastTs.clear(); }
        runAdb(ADB, sel, "logcat -c");
        runAdb(ADB, sel, "shell am force-stop com.oculus.vrshell");
        logStick=true; if(!logRun.load()) startLogStream();
        setStatus("Reloaded home + cleared log - watch the Logcat page for the env-load result.");
    }
    void setVerboseLogging(){
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        runAdb(ADB, sel, "shell setprop debug.logLevel Verbose");
        setStatus("debug.logLevel=Verbose set (no root). Reload the home for it to take effect.");
    }
    // Export a self-contained, SHAREABLE diagnostic report: device context + the (no-root) env-load log +
    // an auto verdict. This is what an unrooted user sends back to figure out WHY their Quest fell back to nuxd.
    void exportDiag(){
        auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        // default filename: quest_env_diag_<timestamp>.txt
        char ts[32]; { std::time_t t=std::time(nullptr); std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm,&t);
#else
            localtime_r(&t,&tm);
#endif
            std::strftime(ts,sizeof ts,"%Y%m%d_%H%M%S",&tm); }
        std::string defBase = std::string("quest_env_diag_") + ts + ".txt";
        std::string path;
#ifdef _WIN32
        std::wstring defName=L"quest_env_diag_"; { std::string s=ts; defName.append(s.begin(),s.end()); defName+=L".txt"; }
        path = pickSaveWin32(L"Export env-load diagnostic report", defName.c_str());
        if (path.empty()) return;   // dialog cancelled
#else
        // Linux/macOS: no native save dialog here -> write next to the exe's working dir (revealed after).
        path = defBase;
#endif
        auto gp=[&](const char* p){ std::string v=adbCapture(ADB, sel, std::string("shell getprop ")+p); while(!v.empty()&&(v.back()=='\n'||v.back()=='\r'||v.back()==' '))v.pop_back(); return v; };
        // snapshot the log
        std::vector<LogLine> snap; { std::lock_guard<std::mutex> lk(logMx); snap.assign(logBuf.begin(), logBuf.end()); }
        // device context (all NO-ROOT)
        bool devOk = adbCapture(ADB, sel, "get-state").find("device")!=std::string::npos;
        std::string model = gp("ro.product.model"), device = gp("ro.product.device");
        std::string osrel = gp("ro.build.version.release"), disp = gp("ro.build.display.id");
        bool rootAdb = adbCapture(ADB, sel, "shell id").find("uid=0")!=std::string::npos
                    || adbCapture(ADB, sel, "shell su -c id").find("uid=0")!=std::string::npos;
        std::string logLvl = gp("debug.logLevel");
        std::string haven  = adbCapture(ADB, sel, "shell pm path com.meta.shell.env.footprint.haven2025");
        std::string envpkgs= adbCapture(ADB, sel, "shell pm list packages com.environment");
        // resolved env + verdict from the captured log
        std::string resolved, verdict="(no env-load lines captured - use \"Reload home\" first)";
        for (auto& l : snap){ if (l.t.find("SetSystemEnvironment resolved")!=std::string::npos || l.t.find("Scene loaded:")!=std::string::npos){ size_t p=l.t.find("apk://"); if(p!=std::string::npos) resolved=l.t.substr(p); } }
        { bool fail=false, ok=false; std::string reason;
          for (auto& l : snap){ const std::string& s=l.t;
            if (s.find("falling back to device default")!=std::string::npos || s.find("using fallback environment")!=std::string::npos) fail=true;
            if (s.find("Scene loaded")!=std::string::npos || s.find("Hsr environment loaded")!=std::string::npos) ok=true;
            if (reason.empty()) for (const char* c : {"Asset ptr error","unable to open package","in zip from","LoadEntry not found","package type not supported","package path not valid","Failed to load hsr","deserialize asset manifest","MeshDefinition::fix","MaterialDefinition::fix","postInitAsset"}) if (s.find(c)!=std::string::npos){ size_t p=s.find("EnvironmentSystem"); reason=(p==std::string::npos?s:s.substr(p)); break; } }
          if (fail) verdict = "REJECTED -> nuxd fallback." + (reason.empty()?std::string(""):("  Reason: "+reason));
          else if (ok) verdict = "Home loaded OK (Scene loaded).";
        }
        std::ofstream f(path, std::ios::binary);
        if (!f){ setStatus("Export failed (can't write "+path+")"); return; }
        f << "=== Quest Home Editor - env-load diagnostic report ===\n";
        f << "generated   : " << ts << "\n\n";
        f << "--- device (all read WITHOUT root) ---\n";
        f << "adb online  : " << (devOk?"yes":"NO - connect the Quest via adb") << "\n";
        f << "serial      : " << (adbSerial.empty()?std::string("(default)"):adbSerial) << "\n";
        f << "model       : " << model << "  (" << device << ")\n";
        f << "os          : Android " << osrel << "  build " << disp << "\n";
        f << "adb root    : " << (rootAdb?"yes":"no (unrooted - the spoof path)") << "\n";
        f << "debug.logLevel: " << (logLvl.empty()?std::string("(unset)"):logLvl) << "\n\n";
        f << "--- environment ---\n";
        f << "resolved env: " << (resolved.empty()?std::string("(unknown - reload the home)"):resolved) << "\n";
        f << "haven2025   : " << (haven.empty()?std::string("(not installed?)"):haven);
        if (!haven.empty() && haven.back()!='\n') f << "\n";
        f << "env packages:\n" << envpkgs << "\n";
        f << "--- VERDICT ---\n" << verdict << "\n\n";
        f << "--- env-load log (adb logcat -v time, NO root, chronological; " << snap.size() << " lines) ---\n";
        for (auto& l : snap) f << l.t << "\n";
        f.close();
        std::error_code ec; std::string abs = std::filesystem::absolute(path, ec).string();
#ifdef _WIN32
        system(("explorer /select,\""+abs+"\"").c_str());
#elif defined(__APPLE__)
        system(("open -R \""+abs+"\" >/dev/null 2>&1 &").c_str());                                  // reveal in Finder
#else
        system(("xdg-open \""+std::filesystem::path(abs).parent_path().string()+"\" >/dev/null 2>&1 &").c_str());   // open the folder
#endif
        setStatus("Exported diagnostic report ("+std::to_string(snap.size())+" log lines) -> "+abs);
    }
    void drawLogcatPanel(float x, float y, float w, float h){
        auto& dl=*cx.dl; auto& th=cx.th; ui::Font* lf = cx.mono? cx.mono : cx.font;
        if (adbDlDone.exchange(false)) { adbWorks(true); adbDevScanned=false; }   // recheck after a background adb fetch
        ensureAdbAsync();                                                          // auto-grab adb (once, background) if none is usable
        if (!autoLogStarted){ autoLogStarted=true; startLogStream(); }   // AUTO-START (+ auto full verbose) on first open
        float rh=24*uiScale, pad=8*uiScale, gap=4*uiScale, bx=x+pad, by=y+pad, rowRight=x+w-pad;
        // controls WRAP to a new row when they'd overflow the panel width (narrow / resized panels)
        auto btn=[&](const char* id,const char* s,float bw,bool acc,const char* tipS)->bool{
            if (bx>x+pad && bx+bw>rowRight){ bx=x+pad; by+=rh+gap; }
            float bxx=bx, byy=by;
            bool r=cx.button(ui::hashId(id), bxx, byy, bw, rh, s, acc);
            if (tipS) cx.tip(bxx, byy, bw, rh, tipS);
            bx+=bw+gap; return r; };
        if (btn("logss", logRun.load()?"Stop":"Start", 66*uiScale, logRun.load(),
                "Start / Stop the live logcat stream (it auto-starts at full verbosity\nwhen you open this tab). No root needed.")) { if(logRun.load()) stopLogStream(); else startLogStream(); }
        if (btn("logrel","Reload home",112*uiScale,true,
                "Clears the log and restarts com.oculus.vrshell (am force-stop, no root),\nso you capture the home's env-load from a clean slate. Do this, then\nread the cyan EnvironmentSystem lines / Export.")) reloadShellForLog();
        if (btn("logclr","Clear",56*uiScale,false,"Clear the captured lines (doesn't stop the stream).")) { std::lock_guard<std::mutex> lk(logMx); logBuf.clear(); logLastRaw.clear(); logLastTs.clear(); }
        if (btn("logsave","Export",70*uiScale,false,
                "Write a shareable diagnostic report (device info + resolved env +\nverdict + the env-load log) to a .txt and reveal it. Attach it on\nDiscord / GitHub when reporting a home that fell back to Haven/nuxd.")) exportDiag();
        if (btn("loghmenu","Home menu",96*uiScale,false,
                "Open the HIDDEN Meta Home settings panel in the headset (no root):\nam start -n com.oculus.panelapp.settings/.SettingsActivity --es uri /home\nPut the headset on to see it.")) {
            auto bs=[](std::string p){   // cmd.exe needs backslashes; POSIX shells need the '/' path UNTOUCHED
#ifdef _WIN32
            for(char&c:p) if(c=='/')c='\\';
#endif
            return p; };
            std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
            runAdb(ADB, sel, "shell am start -n com.oculus.panelapp.settings/.SettingsActivity --es uri /home");
            setStatus("Hidden Meta Home menu opened on the headset (panelapp.settings /home).");
        }
        { int mo=logMode.load(); const char* m = mo==0? "Filter: Env-load" : mo==1? "Filter: vrshell" : "Filter: EVERYTHING";
          if (bx>x+pad && bx+138*uiScale>rowRight){ bx=x+pad; by+=rh+gap; }
          float fbx=bx, fby=by;
          if (cx.tab(ui::hashId("logmode"), fbx, fby, 138*uiScale, rh, m, true)) { logMode.store((mo+1)%3); std::lock_guard<std::mutex> lk(logMx); logBuf.clear(); logLastRaw.clear(); logLastTs.clear(); }
          cx.tip(fbx, fby, 138*uiScale, rh, "Cycle what's shown: Env-load (only the EnvironmentSystem load lines) ->\nvrshell (com.oculus.vrshell only) -> EVERYTHING (the full logcat).");
          bx+=138*uiScale+gap; }
        // device picker (multiple Quests): click to cycle the target device (clears the log)
        { std::string dv = adbSerial.empty()? std::string("Dev: default") : ("Dev: "+adbSerial);
          float dbw = std::min(200.f*uiScale, 84.f*uiScale + (float)dv.size()*6.5f*uiScale);
          if (bx>x+pad && bx+dbw>rowRight){ bx=x+pad; by+=rh+gap; }
          float dbx=bx, dby=by;
          if (cx.button(ui::hashId("logdev"), dbx, dby, dbw, rh, dv.c_str())) { cycleAdbDevice(); std::lock_guard<std::mutex> lk(logMx); logBuf.clear(); logLastRaw.clear(); logLastTs.clear(); }
          cx.tip(dbx, dby, dbw, rh, "Which attached Quest to read. Click to CYCLE devices when several are\nconnected (from `adb devices`). \"default\" = the single attached one.");
          bx+=dbw+gap; }
        // adb status / auto-grab banner — shown only while fetching, or when no usable adb was found.
        if (adbDling.load() || !adbWorks()) {
            bx = x+pad; by += rh+gap;
            std::string st; { std::lock_guard<std::mutex> lk(adbDlMx); st = adbDlStatus; }
            bool dling = adbDling.load();
            std::string msg = dling ? ("adb: " + (st.empty()? std::string("downloading...") : st))
                                    : ("adb not found - " + (st.empty()? std::string("auto-downloading Google platform-tools, or point at your own:") : st));
            float mw = w-2*pad; dl.rect(bx, by, mw, rh, dling? ui::rgba(52,92,150,235) : ui::rgba(150,74,40,235));
            cx.textAligned(bx+6*uiScale, by, mw-12*uiScale, rh, msg.c_str(), th.textSel, 0);
            by += rh+gap; bx = x+pad;
            if (!dling) {
                if (btn("adbdl","Re-try download",130*uiScale,true,"Download Google's official platform-tools (adb) beside the app -\nthe correct build for THIS OS, ~15 MB, one time. Needs a connection.")) { adbAutoTried=false; ensureAdbAsync(); }
                if (btn("adbbrowse","Browse for adb...",134*uiScale,false,"Point at an adb you already have (SideQuest's, the Android SDK's, scrcpy's).\nSaved to adb_path.txt for next time.")) {
                    std::string p = pickFileWin32(L"Locate adb (adb.exe / adb)", L"adb (adb.exe;adb)", L"adb.exe;adb");
                    if (!p.empty()) { setAdbUserPath(p); adbWorks(true); adbDevScanned=false; setStatus("adb set -> "+p); }
                }
            }
        }
        float top = by+rh+6*uiScale; dl.rect(x,top-3*uiScale,w,1,th.splitLine);
        // log area
        float ax=x+pad, ay=top, aw=w-2*pad, ah=y+h-ay-pad; if(ah<20*uiScale) ah=20*uiScale;
        dl.rect(ax,ay,aw,ah, th.field); dl.border(ax,ay,aw,ah, th.border);
        float lh = lf->lineHeight>1.f? lf->lineHeight+1*uiScale : 15*uiScale;
        std::vector<LogLine> snap; { std::lock_guard<std::mutex> lk(logMx); snap.assign(logBuf.begin(), logBuf.end()); }
        // display filter: mode 0 = env-load lines only; 1/2 = all captured lines
        int mode=logMode.load(); std::vector<const LogLine*> view; view.reserve(snap.size());
        for (auto& l : snap) if (mode!=0 || l.env) view.push_back(&l);
        // WRAP each line to the panel width. Use a REALISTIC average glyph width (measuring 'm' — the widest —
        // wrapped ~20% early, leaving blank space to the right); sample a natural mix of log chars instead.
        static const char SAMP[] = "abcdefghijklmnopqrstuvwxyz0123456789 /:.()[]";   // 44 chars
        float charW = lf->textWidth(SAMP)/44.f; if(charW<3.f) charW=6.f;
        int maxChars = std::max(24, (int)((aw-10*uiScale)/charW));
        auto rowsOf=[&](const std::string& s){ int n=(int)((s.size()+(size_t)maxChars-1)/(size_t)maxChars); return n<1?1:n; };
        size_t totalRows=0; for (auto p:view) totalRows += rowsOf(p->t);
        float total = totalRows*lh, maxScroll = std::max(0.f, total-ah+4*uiScale);
        if (cx.hover(ax,ay,aw,ah) && cx.in.wheel!=0){ logScroll -= cx.in.wheel*lh*3.f; logStick=false; }
        if (logStick) logScroll = maxScroll;
        logScroll = std::clamp(logScroll, 0.f, maxScroll);
        if (logScroll >= maxScroll-1.f) logStick=true;
        dl.pushClip(ax,ay,aw,ah);
        float ly = ay+2*uiScale - logScroll;
        for (auto p : view){
            int rc=rowsOf(p->t); float lineH=rc*lh;
            if (ly+lineH >= ay && ly < ay+ah){
                uint32_t col = p->env? ui::rgba(120,220,255) : p->sev==3? ui::rgba(255,96,96)
                             : p->sev==2? ui::rgba(240,200,90) : p->sev==1? th.text : th.textDim;
                for (int r=0;r<rc;r++){ float ry=ly+r*lh; if(ry>=ay+ah)break; if(ry+lh<ay)continue;
                    std::string seg=p->t.substr((size_t)r*maxChars, (size_t)maxChars);
                    cx.textAligned(ax+5*uiScale, ry, aw-10*uiScale, lh, seg.c_str(), col, 0, lf); }
            }
            ly += lineH; if (ly >= ay+ah) break;
        }
        dl.popClip();
        if (view.empty())
            cx.textAligned(ax+8*uiScale, ay+8*uiScale, aw-16*uiScale, lh,
                logRun.load()? "streaming (full verbose, no root)... press \"Reload home\" to capture a fresh env-load"
                             : "Connect the Quest via adb (NO root). Streaming auto-starts; press \"Reload home\" to see WHY the env loads or falls back to nuxd.",
                th.textDim, 0, lf);
    }
    // ── Blender round-trip ──────────────────────────────────────────────────────────────────────────────────────
    // Modern full-Explorer FOLDER picker (IFileOpenDialog + FOS_PICKFOLDERS). Returns "" on cancel / non-Windows.
    static std::string pickFolderWin32(const wchar_t* title) {
#ifdef _WIN32
        std::string result; bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
            DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (title) dlg->SetTitle(title);
            if (SUCCEEDED(dlg->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR w = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                        if (n > 1) { result.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &result[0], n, nullptr, nullptr); }
                        CoTaskMemFree(w);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (co) CoUninitialize();
        return result;
#else
        (void)title; return "";
#endif
    }
    std::string envStem() const {
        std::string s = projectPath.empty() ? std::string("env") : projectPath;
        size_t sl = s.find_last_of("/\\"); if (sl != std::string::npos) s = s.substr(sl + 1);
        size_t dot = s.find_last_of('.'); if (dot != std::string::npos) s = s.substr(0, dot);
        return s.empty() ? std::string("env") : s;
    }
    // Export the loaded env (meshes + materials + textures + per-mesh transforms) to a Blender-ready glTF 2.0 project
    // under the user-picked folder. Opens directly in Blender; the sidecar lets it be re-imported & re-cooked.
    void exportBlender() {
        if (!sceneMeshes || sceneMeshes->empty()) { setStatus("Blender export: no meshes loaded"); return; }
        std::string dir = pickFolderWin32(L"Choose a folder for the Blender project");
        if (dir.empty()) { setStatus("Blender export cancelled"); return; }
        std::string env = envStem();
        std::string outDir = dir + "/" + env + "_blender";
        setStatus("Exporting Blender project (geometry + skeletons + animations)...");
        auto ems = buildExportMeshes();   // the FULL cook source: geometry + skins + skeletal clips + node anims + materials
        bool ok = !ems.empty() && gltfexport::exportEnvFull(ems, outDir, env, "");
        setStatus(ok ? ("Blender project exported -> " + outDir + "\\" + env + ".gltf  (open in Blender)") : "Blender export FAILED");
    }
    // Full-Explorer FILE picker (glTF/glb). Returns "" on cancel / non-Windows.
    // filtName/filtSpec = the PRIMARY file-type filter for this dialog (was hardcoded to Blender glTF —
    // "Set texture" hilariously asked for a .glb). Every call site passes what it actually wants.
    static std::string pickFileWin32(const wchar_t* title, const wchar_t* filtName = L"All files", const wchar_t* filtSpec = L"*.*") {
#ifdef _WIN32
        std::string result; bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
            COMDLG_FILTERSPEC filt[] = { { filtName, filtSpec }, { L"All files", L"*.*" } };
            dlg->SetFileTypes(2, filt);
            DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
            if (title) dlg->SetTitle(title);
            if (SUCCEEDED(dlg->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR w = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                        if (n > 1) { result.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &result[0], n, nullptr, nullptr); }
                        CoTaskMemFree(w);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (co) CoUninitialize();
        return result;
#else
        // Native file-open dialog: macOS (osascript) / Linux (zenity, then kdialog). Returns "" only on cancel.
        (void)filtName;
        std::string t; if (title) for (const wchar_t* p=title; *p; ++p) if (*p>=32 && *p<127) t += (char)*p;   // titles are ASCII
        std::string spec; if (filtSpec) for (const wchar_t* p=filtSpec; *p; ++p) if (*p<127) spec += (char)*p;
        auto esc=[](const std::string& s){ std::string o; for(char c:s){ if(c=='"'||c=='\\'||c=='`'||c=='$') o+='\\'; o+=c; } return o; };
        auto run=[](const std::string& cmd)->std::string{ FILE* p=popen(cmd.c_str(),"r"); if(!p) return ""; std::string o; char b[1024]; size_t n; while((n=fread(b,1,sizeof b,p))>0) o.append(b,n); pclose(p);
                                                          while(!o.empty()&&(o.back()=='\n'||o.back()=='\r')) o.pop_back(); return o; };
  #ifdef __APPLE__
        return run("osascript -e 'POSIX path of (choose file with prompt \""+esc(t)+"\")' 2>/dev/null");
  #else
        if (system("command -v zenity >/dev/null 2>&1")==0) {
            std::string f; for (size_t i=0,st=0; i<=spec.size(); ++i) if (i==spec.size()||spec[i]==';') { std::string g=spec.substr(st,i-st); st=i+1; if(!g.empty()&&g!="*.*") f+=" --file-filter=\""+g+"\""; }   // *.png;*.jpg -> filters
            return run("zenity --file-selection --title=\""+esc(t)+"\""+f+" 2>/dev/null");
        }
        if (system("command -v kdialog >/dev/null 2>&1")==0)
            return run("kdialog --getopenfilename . 2>/dev/null");
        return "";   // no portable picker installed (zenity/kdialog) — the caller's type-a-path field still works
  #endif
#endif
    }
    // Save-As dialog (returns the chosen path, or "" on cancel). Mirrors pickFileWin32 with IFileSaveDialog.
    static std::string pickSaveWin32(const wchar_t* title, const wchar_t* defName, const wchar_t* filtName = L"Text (*.txt)", const wchar_t* filtSpec = L"*.txt") {
#ifdef _WIN32
        std::string result; bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        IFileSaveDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
            COMDLG_FILTERSPEC filt[] = { { filtName, filtSpec }, { L"All files", L"*.*" } };
            dlg->SetFileTypes(2, filt); dlg->SetDefaultExtension(L"txt");
            if (defName) dlg->SetFileName(defName);
            DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
            if (title) dlg->SetTitle(title);
            if (SUCCEEDED(dlg->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR w = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                        if (n > 1) { result.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &result[0], n, nullptr, nullptr); }
                        CoTaskMemFree(w);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (co) CoUninitialize();
        return result;
#else
        // Native save dialog: macOS (osascript) / Linux (zenity --save). Returns "" only on cancel.
        (void)filtName; (void)filtSpec;
        std::string t; if (title) for (const wchar_t* p=title; *p; ++p) if (*p>=32 && *p<127) t += (char)*p;
        std::string dn; if (defName) for (const wchar_t* p=defName; *p; ++p) if (*p>=32 && *p<127) dn += (char)*p;
        auto esc=[](const std::string& s){ std::string o; for(char c:s){ if(c=='"'||c=='\\'||c=='`'||c=='$') o+='\\'; o+=c; } return o; };
        auto run=[](const std::string& cmd)->std::string{ FILE* p=popen(cmd.c_str(),"r"); if(!p) return ""; std::string o; char b[1024]; size_t n; while((n=fread(b,1,sizeof b,p))>0) o.append(b,n); pclose(p);
                                                          while(!o.empty()&&(o.back()=='\n'||o.back()=='\r')) o.pop_back(); return o; };
  #ifdef __APPLE__
        return run("osascript -e 'POSIX path of (choose file name with prompt \""+esc(t)+"\" default name \""+esc(dn)+"\")' 2>/dev/null");
  #else
        if (system("command -v zenity >/dev/null 2>&1")==0)
            return run("zenity --file-selection --save --confirm-overwrite --title=\""+esc(t)+"\" --filename=\""+esc(dn)+"\" 2>/dev/null");
        if (system("command -v kdialog >/dev/null 2>&1")==0)
            return run("kdialog --getsavefilename \"./"+esc(dn)+"\" 2>/dev/null");
        return "";
  #endif
#endif
    }
    // Import a Blender-edited glTF/glb back in. The env is rebuilt from the loader path (gltf_import.h), so we open it
    // in a fresh editor instance with that file as the scene; the user closes this window when ready.
    void importBlender() {
        std::string file = pickFileWin32(L"Choose a Blender glTF / glb to import back", L"Blender glTF (*.gltf;*.glb)", L"*.gltf;*.glb");
        if (file.empty()) { setStatus("Blender import cancelled"); return; }
#ifdef _WIN32
        wchar_t exe[MAX_PATH]; if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) { setStatus("Blender import: can't locate the editor exe"); return; }
        int n = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
        std::wstring fw(n > 1 ? n - 1 : 0, L'\0'); if (n > 1) MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &fw[0], n);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + fw + L"\"";
        std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exe, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            setStatus("Imported project opened in a NEW editor window - close this one when ready");
        } else setStatus("Blender import: failed to launch the editor");
#else
        setStatus("Blender import is Windows-only for now");
#endif
    }
    void startCook() {
        if (cooking.load()) return;
        // PRE-COOK GUARD: no collision at all -> the port has no floor, the player falls forever. Warn (once) and let
        // the user place a Navmesh/Box collider or turn on Auto-floor. "Cook anyway" (modal) bypasses via startCookNow().
        if (!envHasCollision()) { cookWarnOpen = true; return; }
        startCookNow();
    }
    void startCookNow() {
        if (cooking.load()) return;
        saveProject();   // AUTO-SAVE the session on cook — the user's edits (transforms/renames/hides/spawn points/colliders/skybox marks + cook config) are persisted to <env>.hsledit BEFORE cooking, so a cook never silently loses unsaved edits (they're already baked into the APK via the live state; this just keeps the on-disk session in sync).
        // BEFORE buildExportMeshes: the anim-extractor lambda gates the fog/river RIGID-HZANIM+UV path on
        // HSR_HZANIM — runCook's setenv came AFTER extraction, so the gate was ALWAYS false on the first cook
        // of a session (river waterCards fell to the seam-smearing getTime TRANSLATE = the torn river).
        setenv_("HSR_HZANIM", animSkinned ? "1" : "");
        auto ems = buildExportMeshes();
        if (ems.empty()) { setStatus("ERROR: no exportable meshes"); return; }
        if (cookThread.joinable()) cookThread.join();
        pendingCookSig = cookSignatureHex();   // snapshot the up-to-date signature (UI thread) — runCook writes it on success
        cooking.store(true); cookProg.store(0.f);
        std::array<float,3> spawn{ r->cam.pos[0], r->cam.pos[1], r->cam.pos[2] };
        std::vector<sitem::Item> its=items; bakeNavmeshes(its);
        cookThread = std::thread([this, ems=std::move(ems), spawn, pkg=cookPkg, sign=autoSign, spoof=spoofHaven, its=std::move(its)]() mutable {
            runCook(std::move(ems), spawn, pkg, sign, spoof, false, std::move(its));
        });
    }
    // headless / CLI entry (replaces HSR_EXPORT path): synchronous, with a terminal progress bar.
    void exportAPKSync() {
        if (std::getenv("HSR_NOHZ")) animSkinned=false;   // diag: cook with skinned anim OFF (isolate the HZANIM crash)
        if (std::getenv("HSR_NOAUDIO")) cookAudio=false;  // headless/CLI: cook a silent home (no background audio loop)
        if (std::getenv("HSR_NOINSTALL")) installAfterCook=false;   // batch/CLI: cook the APK files only, don't touch the device
        if (std::getenv("HSR_NOAUTOFLOOR")) cookAutoFloor=false;   // headless/CLI: honor the no-generated-collision flag (runCook re-derives the env var from this)
        setenv_("HSR_HZANIM", animSkinned ? "1" : "");   // BEFORE buildExportMeshes (same as startCook): the extractor's RIGID+UV gates read it
        auto ems = buildExportMeshes();
        std::array<float,3> spawn{ r->cam.pos[0], r->cam.pos[1], r->cam.pos[2] };
        std::vector<sitem::Item> its=items; bakeNavmeshes(its);
        pendingCookSig = cookSignatureHex();   // record for "Install only" change-detection
        cooking.store(true); runCook(std::move(ems), spawn, cookPkg, autoSign, spoofHaven, true, std::move(its));
    }
    // The meshes a navmesh draws from: its explicit selection, else the whole walkable scene (non-backdrop, visible).
    std::vector<int> navSourceMeshes(const sitem::Item& si){
        if (!si.srcMeshes.empty()) return si.srcMeshes;
        std::vector<int> all;
        for (int i=0;i<(int)r->gpuMeshes.size();++i){
            if (r->isHidden(i) || isBackdrop(r->gpuMeshes[i].name)) continue;
            // SKIP backdrop-SCALE geometry by name-blind size: a vista/skybox dome or outer-structure mesh (the
            // spacestation's M_vista ±13000, stars, tubes) would make FLAT span the whole sky ("way too big") and
            // SMART "grab all meshes". Only meshes that fit the playable area (AABB extent < 2km) are walkable ground.
            float a[3],b[3]; worldAabb(r->gpuMeshes[i],a,b);
            if (b[0]-a[0] > 2000.f || b[1]-a[1] > 2000.f || b[2]-a[2] > 2000.f) continue;
            all.push_back(i);
        }
        return all;
    }
    // Build a navmesh item's WORLD-space triangles per its mode. The cook PhysX-cooks these into a Meta ColliderMesh;
    // the editor also draws them as the live preview (so you SEE the navmesh before cooking).
    //   navMode 0 = FLAT      : one quad at the lowest source Y, spanning the source bounds.
    //   navMode 1 = SMART     : only near-horizontal (walkable) faces of the source meshes.
    //   navMode 2 = SELECTION : every triangle of the selected meshes.
    void bakeNavGeometry(sitem::Item& si){
        si.navVerts.clear(); si.navIdx.clear();
        std::vector<int> ms = navSourceMeshes(si);
        // per-mesh COLLISION EXCLUSION ("walk-through") + editor-deleted meshes never contribute collision
        ms.erase(std::remove_if(ms.begin(), ms.end(), [&](int m){
            return noColMeshes.count(m)!=0 || (r && r->isDeleted((size_t)m)); }), ms.end());
        bool forceFlat = si.navMode==0 || std::getenv("HSR_NAVFLAT");   // diag: force a 2-tri flat quad (isolate cook vs geometry)
        if (forceFlat){                                       // FLAT — a single ground plane
            float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
            for (int m:ms){ if(m<0||m>=(int)r->gpuMeshes.size())continue; float a[3],b[3]; worldAabb(r->gpuMeshes[m],a,b); for(int k=0;k<3;k++){ mn[k]=std::min(mn[k],a[k]); mx[k]=std::max(mx[k],b[k]); } }
            if (mn[0]>mx[0]) return;
            float y=mn[1];                                    // floor = lowest source point
            float quad[12]={ mn[0],y,mn[2],  mx[0],y,mn[2],  mx[0],y,mx[2],  mn[0],y,mx[2] };
            for(float f:quad) si.navVerts.push_back(f);
            uint32_t qi[6]={0,2,1, 0,3,2}; for(uint32_t i:qi) si.navIdx.push_back(i);   // normal UP (+Y)
            return;
        }
        // SMART / SELECTION -> a COARSE HEIGHTFIELD GRID mesh. A proper navmesh is a SIMPLIFIED low-poly walkable
        // surface (haven2025's = ~272 tris), NOT the raw render geometry: a 10k-tri road SEBD TIMES OUT the device
        // loader (18s) -> abort -> fallback to nuxd. We rasterize the walkable (up-facing) faces into ~5m cells and
        // emit one quad per occupied cell at its surface height -> a few hundred tris that load instantly + are walkable.
        // A MESH COLLIDER (added via "Add Mesh Collider" -> "Collider (...)") is an EXACT solid obstacle, not a
        // walkable navmesh: keep EVERY triangle at ANY orientation. Without this the slope filter dropped
        // down-/side-facing faces — so a PATCH sheet (whose winding can point away from +Y) contributed NO
        // collision ("patch not counted"). Exact colliders respect the mesh's own geometry/plane verbatim.
        bool exactCollider = isMeshColliderItem(si);
        struct TB { float ax,ay,az, bx,by,bz, cx,cy,cz; };   // a walkable triangle (full verts, for height interpolation)
        std::vector<TB> tb; float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
        for (int m:ms){ if(m<0||m>=(int)r->gpuMeshes.size())continue; auto& gm=r->gpuMeshes[m];
            // ANIMATED/skinned meshes can carry EMPTY (or stale) pick arrays — falling through silently left the
            // item with NO geometry, and an empty mesh-collider item cooks as the ColliderBox fallback ("tried to
            // put a meshcollider, all I got was a square box" on the bubbles instancers). Bake from the LIVE scene
            // mesh (current pose) instead — exact triangles, correct world placement.
            const std::vector<float>*   Pp=&gm.pickPos;  const std::vector<uint32_t>* Ip=&gm.pickIdx;
            if ((Pp->size()<9 || Ip->size()<3) && sceneMeshes && m<(int)sceneMeshes->size()){
                Pp=&(*sceneMeshes)[(size_t)m].positions; Ip=&(*sceneMeshes)[(size_t)m].indices; }
            const auto& P=*Pp; const auto& I=*Ip; if (P.size()<9 || I.size()<3) continue;
            for (size_t k=0;k+2<I.size(); k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size()) continue;
                float wa[3],wb[3],wc[3]; xformPoint(gm.model,&P[a*3],wa); xformPoint(gm.model,&P[b*3],wb); xformPoint(gm.model,&P[c*3],wc);
                float e1[3]={wb[0]-wa[0],wb[1]-wa[1],wb[2]-wa[2]}, e2[3]={wc[0]-wa[0],wc[1]-wa[1],wc[2]-wa[2]};
                float nx=e1[1]*e2[2]-e1[2]*e2[1], ny=e1[2]*e2[0]-e1[0]*e2[2], nz=e1[0]*e2[1]-e1[1]*e2[0];
                // SLOPE FILTER. SOLID COLLISION (trimesh, default): slopeMin=0 keeps the FLOOR + vertical WALLS/columns
                // (drops only ceilings) -> the cooked DOUBLE-SIDED trimesh blocks you horizontally too (DEVICE-VERIFIED:
                // walk floors, hit walls, enter rooms). Box fallback (toggle OFF): floor-only (0.05/0.15) since box walls
                // made roof/air junk. navMode 2 = 0.05, navMode 1 = 0.15. HSR_NAVSLOPE overrides for diag.
                float slopeMin = (si.navMode==2) ? 0.05f : 0.15f;
                if (solidCollision) slopeMin = -2.0f;  // ALL faces incl. CEILINGS/undersides ("protect ALL sides": the roof underside
                                                       // must block from below too — the trimesh + voxel-solid boxes handle any orientation)
                if (exactCollider)  slopeMin = -2.0f;  // exact solid obstacle: keep ALL faces (unit ny/nl >= -1 > -2 always) incl. patches
                if(const char* e=std::getenv("HSR_NAVSLOPE")){ float s=(float)atof(e); if(s>=0.0f) slopeMin=s; }
                float nl=std::sqrt(nx*nx+ny*ny+nz*nz); if(nl<1e-9f || ny/nl < slopeMin) continue;   // signed -> floor + walls, drop ceilings
                tb.push_back(TB{wa[0],wa[1],wa[2], wb[0],wb[1],wb[2], wc[0],wc[1],wc[2]});
                for (const float* w : {wa,wb,wc}){ mn[0]=std::min(mn[0],w[0]); mx[0]=std::max(mx[0],w[0]); mn[2]=std::min(mn[2],w[2]); mx[2]=std::max(mx[2],w[2]); } }
        }
        if (tb.empty()) return;
        if (std::getenv("HSR_NAVDBG")) {   // DIAGNOSTIC: what's actually in the navmesh? up-facing(floor)/down(ceiling)/vertical(wall) + their Y spans
            int up=0,dn=0,vt=0; float uyl=1e30f,uyh=-1e30f,dyl=1e30f,dyh=-1e30f;
            for (const TB& t : tb){ float e1[3]={t.bx-t.ax,t.by-t.ay,t.bz-t.az},e2[3]={t.cx-t.ax,t.cy-t.ay,t.cz-t.az};
                float ny=e1[2]*e2[0]-e1[0]*e2[2]; float nl=std::sqrt((e1[1]*e2[2]-e1[2]*e2[1])*(e1[1]*e2[2]-e1[2]*e2[1])+ny*ny+(e1[0]*e2[1]-e1[1]*e2[0])*(e1[0]*e2[1]-e1[1]*e2[0])); if(nl<1e-9f)continue;
                float s=ny/nl, cy=(t.ay+t.by+t.cy)/3.f;
                if(s>0.3f){up++; uyl=std::min(uyl,cy); uyh=std::max(uyh,cy);} else if(s<-0.3f){dn++; dyl=std::min(dyl,cy); dyh=std::max(dyh,cy);} else vt++; }
            fprintf(stderr,"[NAVDBG] navmesh tris: UP(floor)=%d Y[%.2f..%.2f]  DOWN(ceiling)=%d Y[%.2f..%.2f]  VERTICAL(wall)=%d  (total %zu)\n", up,uyl,uyh, dn,dyl,dyh, vt, tb.size());
        }
        // REUSE the actual walkable triangles (NO rebuilt grid) -> the cook makes one TILTED collision box per triangle,
        // so the collision follows the road's exact shape/height/tilt. (Very dense meshes: the cook falls back to a height
        // grid; the editor caps the stored count for preview sanity.)
        int keep = 1; while ((int)(tb.size()/keep) > 500000) keep++;   // store the FULL walkable surface (the cook + preview read this); only decimate absurdly dense meshes
        for (size_t i=0;i<tb.size(); i+=keep){ const TB& t=tb[i];
            uint32_t b=(uint32_t)(si.navVerts.size()/3);
            float v[9]={t.ax,t.ay,t.az, t.bx,t.by,t.bz, t.cx,t.cy,t.cz}; for(float f:v) si.navVerts.push_back(f);
            si.navIdx.push_back(b); si.navIdx.push_back(b+1); si.navIdx.push_back(b+2);
            // MESH COLLIDER items = DOUBLE-SIDED: PhysX trimesh collision is single-sided per winding, so a
            // one-sided bake blocked from one side only ("only a side collidable, the roof is not") — append
            // the reversed twin so both faces + the roof's underside block. (Walkable navmeshes stay single-
            // sided: their reversed floor would block from below and doubles the SEBD for no gain.)
            if (exactCollider){ si.navIdx.push_back(b); si.navIdx.push_back(b+2); si.navIdx.push_back(b+1); }
        }
    }
    // For each NAVMESH item, (re)bake its triangles so the cook has fresh world geometry.
    void bakeNavmeshes(std::vector<sitem::Item>& its) {
        for (auto& si : its) if (si.type == sitem::NAVMESH) bakeNavGeometry(si);
    }
    // Add a navmesh of the chosen mode, bake its preview geometry, select it, ensure its markers are visible.
    void addNavmesh(int mode){
        sitem::Item it; it.type=sitem::NAVMESH; it.navMode=mode;
        if (mode==2){ it.srcMeshes=sel; it.name="Navmesh (sel "+std::to_string(sel.size())+")"; }
        else if (mode==1) it.name="Navmesh (smart)";
        else              it.name="Navmesh (flat)";
        bakeNavGeometry(it);
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; showType[sitem::NAVMESH]=true; tab=TAB_OBJECT; propScroll=0.f;
    }
    // "Make this object a mesh collider": a ColliderMesh built from ONE mesh's exact triangles (a solid obstacle you
    // can't walk through — same haven component as a navmesh, just sourced from a single object). Right-click -> Add.
    void addMeshCollider(int m, bool forceStatic=false){
        if (m<0 || m>=(int)r->gpuMeshes.size()) return;
        auto& gm=r->gpuMeshes[m];
        if (!forceStatic && gm.dynamicVerts) {   // ANIMATED mesh -> a same-entity KINEMATIC collider that follows the animation (toggle)
            auto it=std::find(animColliders.begin(),animColliders.end(),m);
            if (it==animColliders.end()){ animColliders.push_back(m); setStatus("Animated collider ON (follows anim): "+gm.name); }
            else { animColliders.erase(it); setStatus("Animated collider OFF: "+gm.name); }
            return;
        }
        // STATIC ColliderMesh entity from the mesh's exact triangles (forceStatic bakes the CURRENT pose of an animated mesh).
        sitem::Item it; it.type=sitem::NAVMESH; it.navMode=2; it.srcMeshes={m};
        it.name="Collider ("+gm.name+")";
        bakeNavGeometry(it);
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; showType[sitem::NAVMESH]=true; showMeshCol=true; tab=TAB_OBJECT; propScroll=0.f;
        setStatus("Static mesh collider from '"+gm.name+"'");
    }
    bool isAnimCollider(int m) const { return std::find(animColliders.begin(),animColliders.end(),m)!=animColliders.end(); }
    // A NAVMESH editor item is a MESH COLLIDER (added via "Add Mesh Collider") vs a walkable navmesh, by its name.
    bool isMeshColliderItem(const sitem::Item& it) const { return it.type==sitem::NAVMESH && it.name.rfind("Collider",0)==0; }
    // How many mesh-collider components are bound to any of these meshes (button/menu labels).
    int boundMeshColliders(const std::vector<int>& meshes) const {
        int n2=0;
        for (const auto& it : items){
            if (!isMeshColliderItem(it)) continue;
            bool hit=false;
            for (int m : it.srcMeshes){ for (int s2 : meshes) if (s2==m){ hit=true; break; } if (hit) break; }
            if (hit) ++n2;
        }
        return n2;
    }
    // "Remove component": delete the mesh-collider item(s) bound to these meshes — the reverse of
    // addMeshCollider (once added there was NO way to take the collider off an element again; its
    // marker is hidden by default so the item was near-impossible to select). Undoable (Ctrl+Z).
    int removeMeshColliders(const std::vector<int>& meshes){
        std::vector<int> kill;
        for (int i2=0;i2<(int)items.size();++i2){
            const auto& it=items[(size_t)i2];
            if (!isMeshColliderItem(it)) continue;
            bool bound=false;
            for (int m : it.srcMeshes){ for (int s2 : meshes) if (s2==m){ bound=true; break; } if (bound) break; }
            if (bound) kill.push_back(i2);
        }
        if (kill.empty()) return 0;
        pushItemUndo(items);
        for (auto i2=kill.rbegin(); i2!=kill.rend(); ++i2) items.erase(items.begin()+*i2);
        if (selItem>=(int)items.size()) selItem=-1;
        selItems.clear();
        return (int)kill.size();
    }
    // ── COMPONENTS FOLLOW GEOMETRY EDITS: mesh colliders / selection-navmeshes reference meshes BY
    //    INDEX (srcMeshes); every geometry op (cut/knife/slice/split/fuse/bend/seal...) soft-deletes
    //    the original and appends the edited result, so those components kept pointing at the DELETED
    //    original and the generated collision ignored the edit ("collider still blocks the doorway I
    //    just cut"). The result indices are APPENDED to srcMeshes and the old index is KEPT: deleted
    //    meshes are filtered at bake time, so Ctrl+Z (which revives the original and deletes the
    //    results) stays correct with no reverse remap. Collision-exclusion follows the same way, and
    //    every touched component re-bakes immediately so preview + cook see the edited geometry.
    void remapMeshComponents(int oldMi, const std::vector<int>& results){
        if (results.empty()) return;
        if (noColMeshes.count(oldMi)) for (int n2 : results) noColMeshes.insert(n2);
        int touched=0;
        for (auto& it : items){
            if (it.type!=sitem::NAVMESH || it.srcMeshes.empty()) continue;
            bool has=false; for (int m : it.srcMeshes) if (m==oldMi){ has=true; break; }
            if (!has) continue;
            for (int n2 : results){ bool dup=false; for (int m : it.srcMeshes) if (m==n2){ dup=true; break; }
                if (!dup) it.srcMeshes.push_back(n2); }
            bakeNavGeometry(it); ++touched;
        }
        if (touched) fprintf(stderr,"[EDIT] %d collider/navmesh component(s) followed the geometry edit (mesh %d -> %d result(s))\n",
                             touched, oldMi, (int)results.size());
    }
    // Whether a scene item's marker + GIZMO are shown. Mesh colliders get their OWN dedicated viewport toggle
    // (showMeshCol) on top of the Meta-Components eye — that's the quick "hide the collider so I can edit meshes"
    // control. When hidden, drawGizmo/pickItem skip it so the gizmo stops intercepting clicks.
    bool itemShown(const sitem::Item& it) const {
        if (it.hidden) return false;                                   // per-item eye (Scene Items list / navmesh list)
        if (it.type==sitem::NAVMESH) return showMeshCol && showType[sitem::NAVMESH];   // "Collision" header hides ALL navmesh+collider overlays
        return showType[it.type];
    }

    // ── PLAYER SIMULATOR: glue the fly-cam to the walkable surface so you can WALK the env in-editor (test the
    //    navmesh / floor / spawn / colliders without cooking). Steer with the normal WASD+mouse fly controls;
    //    the sim clamps you to the ground, makes you fall off edges, and respawns you below the kill-floor. ──
    float respawnY = 0.f; bool hasRespawn = false;   // editable kill-floor: fall below -> respawn at spawn
    void buildSimGeometry(){
        simV.clear(); simI.clear();
        for (auto& it:items) if (it.type==sitem::NAVMESH && it.navVerts.size()>=9){ uint32_t b=(uint32_t)(simV.size()/3); for(float f:it.navVerts) simV.push_back(f); for(uint32_t k:it.navIdx) simI.push_back(b+k); }
        if (simI.empty()) for (int m=0;m<(int)r->gpuMeshes.size();++m){ if(r->isHidden(m)||isBackdrop(r->gpuMeshes[m].name))continue; auto&gm=r->gpuMeshes[m]; const auto&P=gm.pickPos; const auto&I=gm.pickIdx; if(P.size()<9||I.size()<3)continue; uint32_t b=(uint32_t)(simV.size()/3);
            for(size_t v=0;v+2<P.size();v+=3){ float p[3]={P[v],P[v+1],P[v+2]},o[3]; xformPoint(gm.model,p,o); simV.push_back(o[0]);simV.push_back(o[1]);simV.push_back(o[2]); }
            for(size_t k=0;k+2<I.size();k+=3){ simI.push_back(b+I[k]);simI.push_back(b+I[k+1]);simI.push_back(b+I[k+2]); } }
    }
    // highest walkable triangle at (x,z) at or just above feetY (vertical ray-vs-triangle, barycentric height)
    bool groundAt(float x,float z,float feetY,float& outY){
        float best=-1e30f;
        for (size_t t=0;t+2<simI.size();t+=3){ const float*a=&simV[simI[t]*3],*b=&simV[simI[t+1]*3],*c=&simV[simI[t+2]*3];
            float d=(b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2]); if(std::fabs(d)<1e-9f)continue;
            float u=((b[2]-c[2])*(x-c[0])+(c[0]-b[0])*(z-c[2]))/d;
            float v=((c[2]-a[2])*(x-c[0])+(a[0]-c[0])*(z-c[2]))/d; float w=1.f-u-v;
            if(u<-0.02f||v<-0.02f||w<-0.02f)continue;
            float y=u*a[1]+v*b[1]+w*c[1];
            if(y<=feetY+0.6f && y>best) best=y; }
        if(best>-1e29f){ outY=best; return true; } return false;
    }
    void spawnPlayer(){
        for(auto&it:items) if(it.type==sitem::SPAWN && it.allowStart){ r->cam.pos[0]=it.pos[0]; r->cam.pos[2]=it.pos[2]; r->cam.pos[1]=it.pos[1]+1.6f; float q[4]; eulerToQuat(it.rot,q); float f[3]={0,0,-1},o[3]; quatRotVec(q,f,o); r->cam.yaw=std::atan2(o[0],-o[2]); r->cam.pitch=0; break; }
        float gy; if(groundAt(r->cam.pos[0],r->cam.pos[2], r->cam.pos[1], gy)) r->cam.pos[1]=gy+1.6f;
        pVelY=0;
    }
    void startSim(){ buildSimGeometry(); playSim=true; r->hideAllGeom=false; deselectAll(); selItem=-1; spawnPlayer(); setStatus("WALK MODE: WASD+mouse to walk; P to exit"); }
    void stopSim(){ playSim=false; }
    void simulatePlayer(float dt){
        if(!playSim) return; if(dt<=0.f||dt>0.1f) dt=0.016f;
        // HORIZONTAL blocking on placed box colliders (BOXCOL) + invisible walls (WALLPLACE) — walk mode used to phase
        // straight through them (only the floor was solid). Push the player capsule out of any box it's inside.
        const float pr=0.3f, feet=r->cam.pos[1]-1.6f, head=r->cam.pos[1];
        for (auto& it : items){
            float hx,hy,hz;
            if (it.type==sitem::BOXCOL)        { hx=it.half[0]*it.scale[0]; hy=it.half[1]*it.scale[1]; hz=it.half[2]*it.scale[2]; }
            else if (it.type==sitem::WALLPLACE){ hx=it.propW*0.5f; hy=it.propH*0.5f; hz=0.02f; }
            else continue;
            if (head < it.pos[1]-hy || feet > it.pos[1]+hy) continue;                    // no vertical overlap with this box
            float q[4]; eulerToQuat(it.rot,q); float qc[4]={-q[0],-q[1],-q[2],q[3]};      // box rotation + its inverse
            float rel[3]={r->cam.pos[0]-it.pos[0],0.f,r->cam.pos[2]-it.pos[2]}, loc[3]; quatRotVec(qc,rel,loc);   // player in box-local XZ
            if (std::fabs(loc[0]) < hx+pr && std::fabs(loc[2]) < hz+pr){                  // inside (inflated by player radius) -> eject along least-penetration axis
                float penX=(hx+pr)-std::fabs(loc[0]), penZ=(hz+pr)-std::fabs(loc[2]);
                if (penX<penZ) loc[0]=(loc[0]<0?-(hx+pr):(hx+pr)); else loc[2]=(loc[2]<0?-(hz+pr):(hz+pr));
                float back[3]; quatRotVec(q,loc,back); r->cam.pos[0]=it.pos[0]+back[0]; r->cam.pos[2]=it.pos[2]+back[2];
            }
        }
        float feetY=r->cam.pos[1]-1.6f, gy;
        if(groundAt(r->cam.pos[0],r->cam.pos[2],feetY,gy)){
            float target=gy+1.6f;
            if(r->cam.pos[1]<target) r->cam.pos[1]=target;                              // can't sink below the floor (step up)
            else r->cam.pos[1]+=(target-r->cam.pos[1])*std::min(1.f,dt*10.f);           // ease down off ledges
            pVelY=0;
        } else { pVelY-=12.f*dt; r->cam.pos[1]+=pVelY*dt; }                              // no floor below -> fall
        if((hasRespawn && r->cam.pos[1]<respawnY) || r->cam.pos[1]<-2000.f) spawnPlayer();   // fell into the void -> respawn
    }
    // X-RAY: draw the selected mesh(es) as an always-on-top wireframe so you can SEE the real mesh over the collider
    // boxes and align precisely (the boxes draw on top, so depth alone is misleading from many camera angles).
    void drawSelectedMeshWire(){
        uint32_t col = ui::rgba(120,255,180,220);   // bright green
        for (int m : sel){ if (m<0||m>=(int)r->gpuMeshes.size()) continue; auto& gm=r->gpuMeshes[m];
            const auto& P=gm.pickPos; const auto& I=gm.pickIdx; if (P.size()<9||I.size()<3) continue;
            size_t ntri=I.size()/3, maxTri=40000, stride=ntri>maxTri?ntri/maxTri:1;
            for (size_t t=0;t<ntri;t+=stride){ uint32_t a=I[t*3],b=I[t*3+1],d=I[t*3+2];
                if((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)d*3+2>=P.size()) continue;
                float wa[3],wb[3],wd[3]; xformPoint(gm.model,&P[a*3],wa); xformPoint(gm.model,&P[b*3],wb); xformPoint(gm.model,&P[d*3],wd);
                float sa[2],sb[2],sd[2]; bool oa=worldToScreen(wa,sa[0],sa[1]),ob=worldToScreen(wb,sb[0],sb[1]),od=worldToScreen(wd,sd[0],sd[1]);
                if(oa&&ob)dl.line(sa[0],sa[1],sb[0],sb[1],col,0.7f);
                if(ob&&od)dl.line(sb[0],sb[1],sd[0],sd[1],col,0.7f);
                if(od&&oa)dl.line(sd[0],sd[1],sa[0],sa[1],col,0.7f);
            }
        }
    }
    // Draw a navmesh's baked triangles as a wireframe overlay (the "way to see it"). Capped so huge meshes stay cheap.
    void drawNavWire(const sitem::Item& it, uint32_t col){
        const auto& V=it.navVerts; const auto& I=it.navIdx;
        if (V.size()<9 || I.size()<3){ for (int m:it.srcMeshes) if (m>=0&&m<(int)r->gpuMeshes.size()) drawAabbBox(r->gpuMeshes[m]); return; }
        float M[16]; itemTRS(it,M);   // apply the item's T·R·S (the gizmo edits this) so moving the gizmo moves the navmesh
        // Draw the FULL collision surface (was capped at 12000 tris -> a misleading "colander" of sparse triangles even
        // though the cooked collider is solid). 120k keeps it solid-looking at editor FPS — but that budget is now
        // SHARED across every visible NAVMESH item ("very laggy when there's lots of em": N colliders x 120k CPU-
        // projected tris per frame melted the drawlist). The SELECTED item gets the full budget (inspect closely);
        // unselected ones split the rest, min 3k each so shapes stay readable.
        size_t nNav=0; for (auto& x : items) if (x.type==sitem::NAVMESH && x.navVerts.size()>=9) ++nNav;
        bool isSel = (selItem>=0 && selItem<(int)items.size() && &items[(size_t)selItem]==&it);
        size_t maxTri = isSel ? 120000 : std::max<size_t>(3000, 120000 / (nNav?nNav:1));
        size_t ntri=I.size()/3, stride = ntri>maxTri ? ntri/maxTri : 1;
        uint32_t fillCol=ui::withA(col,60), edgeCol=ui::withA(col,180);   // FILLED translucent green surface + edges (haven2025 look)
        for (size_t t=0;t<ntri;t+=stride){
            uint32_t a=I[t*3],b=I[t*3+1],d=I[t*3+2];
            if ((size_t)a*3+2>=V.size()||(size_t)b*3+2>=V.size()||(size_t)d*3+2>=V.size()) continue;
            float wa[3],wb[3],wd[3]; xformPoint(M,&V[a*3],wa); xformPoint(M,&V[b*3],wb); xformPoint(M,&V[d*3],wd);
            uint32_t fc=fillCol, ec=edgeCol;
            if (navColorBySlope || solidCollision) {   // SOLID COLLISION (or debug): color each triangle by SLOPE so you SEE the real collider —
                // walkable floor = GREEN, vertical WALLS/columns = RED (the solid surfaces you'll be blocked by). Matches haven2025's nav/wall gizmo convention.
                float e1[3]={wb[0]-wa[0],wb[1]-wa[1],wb[2]-wa[2]}, e2[3]={wd[0]-wa[0],wd[1]-wa[1],wd[2]-wa[2]};
                float nx=e1[1]*e2[2]-e1[2]*e2[1], ny=e1[2]*e2[0]-e1[0]*e2[2], nz=e1[0]*e2[1]-e1[1]*e2[0];
                float nl=std::sqrt(nx*nx+ny*ny+nz*nz); float up = nl>1e-6f ? std::fabs(ny)/nl : 1.f;   // 1=flat .. 0=vertical
                float rr = up>0.85f ? (1.f-up)/0.15f : 1.f;             // red rises as it steepens below 0.85
                float gg = up>0.5f  ? 1.f : up/0.5f;                    // green falls off below 0.5
                int R=(int)(255*std::min(1.f,std::max(0.f,rr))), G=(int)(255*std::min(1.f,std::max(0.f,gg)));
                fc=ui::rgba(R,G,40,72); ec=ui::rgba(R,G,40,205);
            }
            float sa[2],sb[2],sd[2];
            bool oa=worldToScreen(wa,sa[0],sa[1]), ob=worldToScreen(wb,sb[0],sb[1]), od=worldToScreen(wd,sd[0],sd[1]);
            if(oa&&ob&&od) dl.triangle(sa[0],sa[1],sb[0],sb[1],sd[0],sd[1], fc);   // walkable surface fill (slope-colored)
            if(oa&&ob) dl.line(sa[0],sa[1],sb[0],sb[1],ec,0.7f);
            if(ob&&od) dl.line(sb[0],sb[1],sd[0],sd[1],ec,0.7f);
            if(od&&oa) dl.line(sd[0],sd[1],sa[0],sa[1],ec,0.7f);
        }
    }
    bool navColorBySlope = true;   // navmesh debug: colorize triangles by slope (flat=green .. steep=red) to see smoothness
    bool navSmooth = false;        // cook navmesh collision with averaged (smoothed) normals -> no per-edge creases (UI toggle, drives the cook)
    bool navDebugClone = false;    // cook a visible slope-colored clone of the navmesh so it renders ON DEVICE (UI toggle, drives the cook)
    static void printBar(float f, const char* s){
        int W=28, n=(int)(f*W); char bar[64]; for(int i=0;i<W;i++) bar[i]=i<n?'#':' '; bar[W]=0;
        fprintf(stderr, "\r[%s] %3d%%  %-22s", bar, (int)(f*100), s); fflush(stderr);
    }
    static void setenv_(const char* k, const char* v){
#ifdef _WIN32
        std::string s=std::string(k)+"="+v; _putenv(s.c_str());   // "K=" removes the var on Windows
#else
        // POSIX setenv(k,"",1) stores an EMPTY string which getenv() returns NON-NULL — every
        // `if (getenv("HSR_X"))` gate would read as ON. Empty value must UNSET, matching Windows.
        if (!v || !*v) unsetenv(k); else setenv(k, v, 1);
#endif
    }
    static bool writeFile(const std::string& p, const std::vector<uint8_t>& b){ FILE* f=fopen(p.c_str(),"wb"); if(!f) return false; fwrite(b.data(),1,b.size(),f); fclose(f); return true; }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  PICKING + MATH  (ported verbatim from the ImGui editor; pickIndex now uses the viewport pane)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    static bool inRect(const VkRect2D& r, float x, float y){ return x>=r.offset.x && y>=r.offset.y && x<r.offset.x+(int)r.extent.width && y<r.offset.y+(int)r.extent.height; }
    int pickIndex(double mx, double my) {
        if (gizmoDrag || !r || r->gpuMeshes.empty() || !inRect(rcViewport,(float)mx,(float)my)) return -1;
        float W=(float)rcViewport.extent.width, H=(float)rcViewport.extent.height; if (W<=0||H<=0) return -1;
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        float inv[16]; if (!invertMat4(vp, inv)) return -1;
        float ndcx = 2.0f*((float)mx-rcViewport.offset.x)/W - 1.0f;
        float ndcy = 2.0f*((float)my-rcViewport.offset.y)/H - 1.0f;
        float O[3], F[3];
        unproject(inv, ndcx, ndcy, 1.0f, O);
        unproject(inv, ndcx, ndcy, 0.0f, F);
        float D[3]={F[0]-O[0],F[1]-O[1],F[2]-O[2]};
        float dl_=std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]); if (dl_<1e-6f) return -1; D[0]/=dl_;D[1]/=dl_;D[2]/=dl_;
        int best=-1; float bestT=std::numeric_limits<float>::max();
        // EVERY mesh is clickable — including the sky dome/backdrop (it's a mesh: select it to
        // retexture/tint/hide it). Nearest triangle hit wins, so the dome only picks when nothing
        // closer is under the cursor. (The old isBackdrop click-through made domes unselectable.)
        for (int i=0;i<(int)r->gpuMeshes.size();++i) {
            if (r->isHidden(i) || r->isDeleted(i)) continue;   // deleted meshes are GONE: invisible AND unclickable
            auto& gm=r->gpuMeshes[i];
            float mn[3],mx2[3]; worldAabb(gm,mn,mx2); float taabb;
            if (!rayAabb(O,D,mn,mx2,taabb)) continue; if (taabb-0.02f>bestT) continue;
            const std::vector<float>& P=gm.pickPos; const std::vector<uint32_t>& I=gm.pickIdx;
            if (P.empty()||I.size()<3){ if (taabb<bestT){bestT=taabb;best=i;} continue; }
            for (size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size()) continue;
                float w0[3],w1[3],w2[3]; xformPoint(gm.model,&P[a*3],w0); xformPoint(gm.model,&P[b*3],w1); xformPoint(gm.model,&P[c*3],w2);
                float t; if (rayTri(O,D,w0,w1,w2,t)&&t<bestT){bestT=t;best=i;} }
        }
        return best;
    }
    // Raycast straight ahead from the camera; return the nearest scene-surface hit point. Used to drop a NEW item
    // where you're LOOKING (on a real surface) instead of floating a fixed 2.5 m in front ("objects in the wrong spot").
    bool cameraForwardHit(float out[3]){
        if (!r || r->gpuMeshes.empty()) return false;
        float O[3]={r->cam.pos[0],r->cam.pos[1],r->cam.pos[2]};
        float cp=std::cos(r->cam.pitch); float D[3]={std::sin(r->cam.yaw)*cp, std::sin(r->cam.pitch), -std::cos(r->cam.yaw)*cp};
        float bestT=1e30f; bool hit=false;
        for (int i=0;i<(int)r->gpuMeshes.size();++i){ if(r->isHidden(i)||r->isDeleted(i))continue; auto&gm=r->gpuMeshes[i]; if(isBackdrop(gm.name))continue;
            float mn[3],mx[3]; worldAabb(gm,mn,mx); float ta; if(!rayAabb(O,D,mn,mx,ta))continue; if(ta-0.02f>bestT)continue;
            const auto&P=gm.pickPos; const auto&I=gm.pickIdx; if(P.empty()||I.size()<3)continue;
            for(size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size())continue;
                float w0[3],w1[3],w2[3]; xformPoint(gm.model,&P[a*3],w0); xformPoint(gm.model,&P[b*3],w1); xformPoint(gm.model,&P[c*3],w2);
                float t; if(rayTri(O,D,w0,w1,w2,t)&&t<bestT){bestT=t;hit=true;} } }
        if(hit){ out[0]=O[0]+D[0]*bestT; out[1]=O[1]+D[1]*bestT; out[2]=O[2]+D[2]*bestT; }
        return hit;
    }
    // Raycast a SCREEN point against ONE mesh; return the hit point + that triangle's surface normal (oriented toward
    // the camera). Used to place a Wall on the exact clicked face, tilted to the surface.
    bool screenRayHitMesh(double mx, double my, int meshIdx, float outP[3], float outN[3]){
        if(!r||meshIdx<0||meshIdx>=(int)r->gpuMeshes.size())return false;
        float W=(float)rcViewport.extent.width,H=(float)rcViewport.extent.height; if(W<=0||H<=0)return false;
        float vp[16]; mat4mul(r->cam.proj,r->cam.view,vp); float inv[16]; if(!invertMat4(vp,inv))return false;
        float ndcx=2.f*((float)mx-rcViewport.offset.x)/W-1.f, ndcy=2.f*((float)my-rcViewport.offset.y)/H-1.f;
        float O[3],Fp[3]; unproject(inv,ndcx,ndcy,1.f,O); unproject(inv,ndcx,ndcy,0.f,Fp);
        float D[3]={Fp[0]-O[0],Fp[1]-O[1],Fp[2]-O[2]}; float dl_=std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]); if(dl_<1e-6f)return false; D[0]/=dl_;D[1]/=dl_;D[2]/=dl_;
        auto&gm=r->gpuMeshes[meshIdx]; const auto&P=gm.pickPos; const auto&I=gm.pickIdx; if(P.size()<9||I.size()<3)return false;
        float bestT=1e30f; bool hit=false; float bn[3]={0,1,0};
        for(size_t k=0;k+2<I.size();k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
            if((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size())continue;
            float w0[3],w1[3],w2[3]; xformPoint(gm.model,&P[a*3],w0); xformPoint(gm.model,&P[b*3],w1); xformPoint(gm.model,&P[c*3],w2);
            float t; if(rayTri(O,D,w0,w1,w2,t)&&t<bestT){ bestT=t; hit=true;
                float e1[3]={w1[0]-w0[0],w1[1]-w0[1],w1[2]-w0[2]},e2[3]={w2[0]-w0[0],w2[1]-w0[1],w2[2]-w0[2]};
                bn[0]=e1[1]*e2[2]-e1[2]*e2[1]; bn[1]=e1[2]*e2[0]-e1[0]*e2[2]; bn[2]=e1[0]*e2[1]-e1[1]*e2[0];
                float nl=std::sqrt(bn[0]*bn[0]+bn[1]*bn[1]+bn[2]*bn[2]); if(nl>1e-9f){bn[0]/=nl;bn[1]/=nl;bn[2]/=nl;} } }
        if(hit){ outP[0]=O[0]+D[0]*bestT; outP[1]=O[1]+D[1]*bestT; outP[2]=O[2]+D[2]*bestT;
            if(bn[0]*D[0]+bn[1]*D[1]+bn[2]*D[2]>0.f){bn[0]=-bn[0];bn[1]=-bn[1];bn[2]=-bn[2];}   // face TOWARD the camera
            outN[0]=bn[0];outN[1]=bn[1];outN[2]=bn[2]; }
        return hit;
    }
    // Euler (deg) for an orientation whose item-forward (-Z) points along F (e.g. a surface normal). Same matrix->quat
    // ->euler as cameraEuler, but for an arbitrary forward so a Wall can TILT to the surface, not just yaw.
    void forwardToEuler(const float Fin[3], float e[3]){
        float F[3]={Fin[0],Fin[1],Fin[2]}; float fl=std::sqrt(F[0]*F[0]+F[1]*F[1]+F[2]*F[2]); if(fl<1e-6f){e[0]=e[1]=e[2]=0;return;} for(int k=0;k<3;k++)F[k]/=fl;
        float z[3]={-F[0],-F[1],-F[2]}; float up[3]={0,1,0};
        float x[3]={up[1]*z[2]-up[2]*z[1], up[2]*z[0]-up[0]*z[2], up[0]*z[1]-up[1]*z[0]}; float xl=std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);
        if(xl<1e-4f){ up[0]=0;up[1]=0;up[2]=1; x[0]=up[1]*z[2]-up[2]*z[1]; x[1]=up[2]*z[0]-up[0]*z[2]; x[2]=up[0]*z[1]-up[1]*z[0]; xl=std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); }
        for(int k=0;k<3;k++)x[k]/=xl;
        float yv[3]={z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0]};
        float m00=x[0],m10=x[1],m20=x[2], m01=yv[0],m11=yv[1],m21=yv[2], m02=z[0],m12=z[1],m22=z[2];
        float tr=m00+m11+m22, q[4];
        if(tr>0){ float s=std::sqrt(tr+1.f)*2.f; q[3]=0.25f*s; q[0]=(m21-m12)/s; q[1]=(m02-m20)/s; q[2]=(m10-m01)/s; }
        else if(m00>m11&&m00>m22){ float s=std::sqrt(1.f+m00-m11-m22)*2.f; q[3]=(m21-m12)/s; q[0]=0.25f*s; q[1]=(m01+m10)/s; q[2]=(m02+m20)/s; }
        else if(m11>m22){ float s=std::sqrt(1.f+m11-m00-m22)*2.f; q[3]=(m02-m20)/s; q[0]=(m01+m10)/s; q[1]=0.25f*s; q[2]=(m12+m21)/s; }
        else { float s=std::sqrt(1.f+m22-m00-m11)*2.f; q[3]=(m10-m01)/s; q[0]=(m02+m20)/s; q[1]=(m12+m21)/s; q[2]=0.25f*s; }
        normalizeQuat(q); quatToEuler(q,e);
    }
    void pick(double mx, double my, bool add){
        int it = pickItem(mx,my);
        if (it>=0) { selItem=it; deselectAll(); return; }                  // a scene-item marker takes priority
        selItem=-1;
        int b=pickIndex(mx,my);
        if (b>=0) { if (add) toggleSel(b); else selectOne(b); scrollToSel=true; }
        else if (!add) deselectAll();                                      // click empty space (or the sky) = deselect
    }
    static void xformPoint(const float m[16], const float p[3], float o[3]){ o[0]=m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12]; o[1]=m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13]; o[2]=m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]; }
    static bool rayTri(const float O[3],const float D[3],const float v0[3],const float v1[3],const float v2[3],float& t){
        float e1[3]={v1[0]-v0[0],v1[1]-v0[1],v1[2]-v0[2]},e2[3]={v2[0]-v0[0],v2[1]-v0[1],v2[2]-v0[2]};
        float p[3]={D[1]*e2[2]-D[2]*e2[1],D[2]*e2[0]-D[0]*e2[2],D[0]*e2[1]-D[1]*e2[0]};
        float det=e1[0]*p[0]+e1[1]*p[1]+e1[2]*p[2]; if (std::fabs(det)<1e-12f) return false; float inv=1.f/det;
        float tv[3]={O[0]-v0[0],O[1]-v0[1],O[2]-v0[2]}; float u=(tv[0]*p[0]+tv[1]*p[1]+tv[2]*p[2])*inv; if (u<0||u>1) return false;
        float q[3]={tv[1]*e1[2]-tv[2]*e1[1],tv[2]*e1[0]-tv[0]*e1[2],tv[0]*e1[1]-tv[1]*e1[0]}; float v=(D[0]*q[0]+D[1]*q[1]+D[2]*q[2])*inv; if (v<0||u+v>1) return false;
        float tt=(e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2])*inv; if (tt<=1e-4f) return false; t=tt; return true;
    }
    static void unproject(const float inv[16],float ndcx,float ndcy,float ndcz,float out[3]){
        float x=inv[0]*ndcx+inv[4]*ndcy+inv[8]*ndcz+inv[12],y=inv[1]*ndcx+inv[5]*ndcy+inv[9]*ndcz+inv[13],z=inv[2]*ndcx+inv[6]*ndcy+inv[10]*ndcz+inv[14],w=inv[3]*ndcx+inv[7]*ndcy+inv[11]*ndcz+inv[15];
        if (std::fabs(w)<1e-12f) w=1; out[0]=x/w; out[1]=y/w; out[2]=z/w;
    }
    static void worldAabb(const VkGpuMesh& gm, float mn[3], float mx[3]){
        bool ident=gm.model[0]==1&&gm.model[5]==1&&gm.model[10]==1&&gm.model[15]==1&&gm.model[12]==0&&gm.model[13]==0&&gm.model[14]==0&&gm.model[1]==0&&gm.model[2]==0&&gm.model[4]==0&&gm.model[6]==0&&gm.model[8]==0&&gm.model[9]==0;
        if (ident){ for(int k=0;k<3;++k){mn[k]=gm.bbMin[k];mx[k]=gm.bbMax[k];} return; }
        mn[0]=mn[1]=mn[2]=std::numeric_limits<float>::max(); mx[0]=mx[1]=mx[2]=-std::numeric_limits<float>::max();
        for (int c=0;c<8;++c){ float px=(c&1)?gm.bbMax[0]:gm.bbMin[0],py=(c&2)?gm.bbMax[1]:gm.bbMin[1],pz=(c&4)?gm.bbMax[2]:gm.bbMin[2];
            float wx=gm.model[0]*px+gm.model[4]*py+gm.model[8]*pz+gm.model[12],wy=gm.model[1]*px+gm.model[5]*py+gm.model[9]*pz+gm.model[13],wz=gm.model[2]*px+gm.model[6]*py+gm.model[10]*pz+gm.model[14];
            mn[0]=std::min(mn[0],wx);mn[1]=std::min(mn[1],wy);mn[2]=std::min(mn[2],wz); mx[0]=std::max(mx[0],wx);mx[1]=std::max(mx[1],wy);mx[2]=std::max(mx[2],wz); }
    }
    static bool rayAabb(const float O[3],const float D[3],const float mn[3],const float mx[3],float& tHit){
        float tmin=-std::numeric_limits<float>::max(),tmax=std::numeric_limits<float>::max();
        for (int a=0;a<3;++a){ if (std::fabs(D[a])<1e-9f){ if (O[a]<mn[a]||O[a]>mx[a]) return false; continue; }
            float inv=1.f/D[a],t1=(mn[a]-O[a])*inv,t2=(mx[a]-O[a])*inv; if (t1>t2) std::swap(t1,t2); tmin=std::max(tmin,t1); tmax=std::min(tmax,t2); if (tmin>tmax) return false; }
        if (tmax<0) return false; tHit=(tmin>0)?tmin:tmax; return true;
    }
    static bool invertMat4(const float m[16], float inv[16]){
        float a[16];
        a[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
        a[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
        a[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
        a[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
        a[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
        a[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
        a[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
        a[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
        a[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
        a[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
        a[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
        a[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
        a[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
        a[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
        a[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
        a[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
        float det=m[0]*a[0]+m[1]*a[4]+m[2]*a[8]+m[3]*a[12]; if (std::fabs(det)<1e-20f) return false; det=1.f/det;
        for (int i=0;i<16;++i) inv[i]=a[i]*det; return true;
    }
    static void buildTRS(const float t[3],const float q[4],const float s[3],float o[16]){
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float r00=1-2*(y*y+z*z),r01=2*(x*y-w*z),r02=2*(x*z+w*y),r10=2*(x*y+w*z),r11=1-2*(x*x+z*z),r12=2*(y*z-w*x),r20=2*(x*z-w*y),r21=2*(y*z+w*x),r22=1-2*(x*x+y*y);
        o[0]=r00*s[0];o[4]=r01*s[1];o[8]=r02*s[2];o[12]=t[0];o[1]=r10*s[0];o[5]=r11*s[1];o[9]=r12*s[2];o[13]=t[1];o[2]=r20*s[0];o[6]=r21*s[1];o[10]=r22*s[2];o[14]=t[2];o[3]=0;o[7]=0;o[11]=0;o[15]=1;
    }
    static void mat4mul(const float a[16],const float b[16],float o[16]){ for (int c=0;c<4;++c) for (int row=0;row<4;++row){ float s=0; for (int k=0;k<4;++k) s+=a[k*4+row]*b[c*4+k]; o[c*4+row]=s; } }
    static void normalizeQuat(float q[4]){ float l=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]); if (l<1e-8f){q[0]=q[1]=q[2]=0;q[3]=1;return;} for (int i=0;i<4;++i) q[i]/=l; }
    static void quatToEuler(const float q[4],float e[3]){
        float x=q[0],y=q[1],z=q[2],w=q[3]; float sinr=2*(w*x+y*z),cosr=1-2*(x*x+y*y); e[0]=std::atan2(sinr,cosr);
        float sinp=2*(w*y-z*x); e[1]=(std::fabs(sinp)>=1.f)?std::copysign(1.5707963f,sinp):std::asin(sinp);
        float siny=2*(w*z+x*y),cosy=1-2*(y*y+z*z); e[2]=std::atan2(siny,cosy); for (int i=0;i<3;++i) e[i]*=57.2957795f;
    }
    static void eulerToQuat(const float e[3],float q[4]){
        float X=e[0]*0.00872664626f,Y=e[1]*0.00872664626f,Z=e[2]*0.00872664626f; float cx=std::cos(X),sx=std::sin(X),cy=std::cos(Y),sy=std::sin(Y),cz=std::cos(Z),sz=std::sin(Z);
        q[3]=cx*cy*cz+sx*sy*sz; q[0]=sx*cy*cz-cx*sy*sz; q[1]=cx*sy*cz+sx*cy*sz; q[2]=cx*cy*sz-sx*sy*cz;
    }
    void recomputeModel(VkGpuMesh& gm){
        const float* c=gm.centroid; float qi[4]={0,0,0,1},s1[3]={1,1,1};
        float rs[16]; { float z[3]={0,0,0}; buildTRS(z,gm.editR,gm.editS,rs); }
        float Tpre[16]; { float tp[3]={gm.editT[0]+c[0],gm.editT[1]+c[1],gm.editT[2]+c[2]}; buildTRS(tp,qi,s1,Tpre); }
        float Tneg[16]; { float tn[3]={-c[0],-c[1],-c[2]}; buildTRS(tn,qi,s1,Tneg); }
        float tmp[16],delta[16]; mat4mul(Tpre,rs,tmp); mat4mul(tmp,Tneg,delta); mat4mul(delta,gm.baseModel,gm.model);
    }
    // ── undo ──
    static Xform captureX(const VkGpuMesh& gm){ Xform x; memcpy(x.t,gm.editT,12); memcpy(x.r,gm.editR,16); memcpy(x.s,gm.editS,12); return x; }
    void applyX(VkGpuMesh& gm, const Xform& x){ memcpy(gm.editT,x.t,12); memcpy(gm.editR,x.r,16); memcpy(gm.editS,x.s,12); recomputeModel(gm); }
    static bool xeq(const Xform& a, const Xform& b){ for (int i=0;i<3;++i) if (a.t[i]!=b.t[i]||a.s[i]!=b.s[i]) return false; for (int i=0;i<4;++i) if (a.r[i]!=b.r[i]) return false; return true; }
    void pushUndo(const std::vector<int>& m, const std::vector<Xform>& b, const std::vector<Xform>& a){
        bool any=false; for (size_t i=0;i<m.size()&&i<b.size()&&i<a.size();++i) if (!xeq(b[i],a[i])) any=true;
        if (!any) return; undoStack.push_back({m,b,a}); redoStack.clear(); if (undoStack.size()>256) undoStack.erase(undoStack.begin());
    }
    void pushUndo(int mesh, const Xform& b, const Xform& a){ pushUndo(std::vector<int>{mesh}, std::vector<Xform>{b}, std::vector<Xform>{a}); }
    void endEdit(const VkGpuMesh& gm){ if (!editing) return; pushUndo(editMesh, editBefore, captureX(gm)); editing=false; }
    void restoreOp(const UndoOp& op, bool redo){ for (size_t i=0;i<op.m.size();++i) if (op.m[i]>=0&&op.m[i]<(int)r->gpuMeshes.size()) applyX(r->gpuMeshes[op.m[i]], redo?op.a[i]:op.b[i]); sel=op.m; selected=op.m.empty()?-1:op.m.back(); r->selectedMesh=selected; }
    // a delete-op touching a CREATED mesh (>= baseMeshCount) invalidates the .geom sidecar (see toggleDeleteSelected)
    void markGeomIfCreated(const std::vector<int>& ms){ if (baseMeshCount<0) return; for (int m:ms) if (m>=baseMeshCount){ geomDirty=true; return; } }
    void doUndo(){ if (undoStack.empty()) return; UndoOp op=undoStack.back(); undoStack.pop_back();
        if (op.isItems){ std::swap(op.itemsState, items); selItem=-1; }
        else if (op.isDelete){ for (size_t i=0;i<op.delM.size();++i) r->setDeleted((size_t)op.delM[i], i<op.delA.size() ? !op.delA[i] : false);
                               markGeomIfCreated(op.delM);
                               bakeNavmeshes(items); }   // undo = INVERT each state (restores originals, removes created clones); colliders re-bake off the revived geometry
        else restoreOp(op,false); redoStack.push_back(std::move(op)); }
    void doRedo(){ if (redoStack.empty()) return; UndoOp op=redoStack.back(); redoStack.pop_back();
        if (op.isItems){ std::swap(op.itemsState, items); selItem=-1; }
        else if (op.isDelete){ for (size_t i=0;i<op.delM.size();++i) r->setDeleted((size_t)op.delM[i], i<op.delA.size() ? op.delA[i]!=0 : true);
                               markGeomIfCreated(op.delM);
                               bakeNavmeshes(items); }   // redo flips back to the edited geometry - colliders follow again
        else restoreOp(op,true); undoStack.push_back(std::move(op)); }
    // ── focus ──
    void focusOn(float cx_,float cy,float cz){ Camera& c=r->cam; float ex=cx_,ey=cy+1.2f,ez=cz+3.5f; c.pos[0]=ex;c.pos[1]=ey;c.pos[2]=ez; float dx=cx_-ex,dy=cy-ey,dz=cz-ez,L=std::sqrt(dx*dx+dy*dy+dz*dz); if (L<1e-4f) L=1.f; c.yaw=std::atan2(dx,-dz); c.pitch=std::asin(dy/L); }
    void focusMesh(VkGpuMesh& gm){   // frame the whole object by its world AABB size (so big meshes aren't clipped)
        float mn[3],mx[3]; worldAabb(gm,mn,mx);
        float cx_=(mn[0]+mx[0])*0.5f, cy=(mn[1]+mx[1])*0.5f, cz=(mn[2]+mx[2])*0.5f;
        float rad=0.5f*std::sqrt((mx[0]-mn[0])*(mx[0]-mn[0])+(mx[1]-mn[1])*(mx[1]-mn[1])+(mx[2]-mn[2])*(mx[2]-mn[2]));
        float d=std::max(1.5f, rad*1.9f);
        Camera& c=r->cam; c.pos[0]=cx_; c.pos[1]=cy+d*0.22f; c.pos[2]=cz+d;
        float dy=cy-c.pos[1], dz=cz-c.pos[2], L=std::sqrt(dy*dy+dz*dz); if (L<1e-4f) L=1.f;
        c.yaw=0.f; c.pitch=std::asin(dy/L);
    }
};
