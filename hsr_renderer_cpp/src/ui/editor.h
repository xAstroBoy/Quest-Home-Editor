#pragma once
// ── In-window EDITOR — a custom, from-scratch Blender-style C++ UI (NO Dear ImGui) ──────────────────────────
// Drawn through the renderer's own Vulkan via the src/ui/ toolkit (ui_font/ui_draw/ui_core): a tiled Blender
// layout (header + 3D Viewport pane + Outliner + Properties tabs + Timeline), dark Blender-faithful theme,
// click-select + a custom move/rotate/scale gizmo with undo, and the porting tools embedded as Properties
// panels — including a threaded Cook/Export with a live progress bar and one-click APK auto-signing.
// Decoupled from the renderer via its overlayBegin/overlayDraw hooks + uiViewportRect (the 3D scissor pane).
#include "render/vk_renderer.h"
#include "core/audio.h"
#include "core/camera.h"
#include "core/scene_items.h"
#include "cook/hsl_cooker.h"
#include "ui/ui_core.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <thread>
#include <atomic>
#include <mutex>

struct Editor {
    // ── bindings ──
    VkRenderer*  r = nullptr;
    AudioPlayer* audio = nullptr;
    GLFWwindow*  win = nullptr;
    bool ready = false;
    std::string projectPath;           // the loaded env path; the editor session saves/loads to <projectPath>.hsledit
    bool*  animOverride = nullptr;     // main's loop reads these through pointers (timeline scrub)
    float* animScrub    = nullptr;
    float  animDuration = 0.0f;
    bool   animPlaying  = true;        // the editor OWNS the playback clock -> the timeline playhead advances LIVE
    std::vector<MeshData>* sceneMeshes = nullptr;                                  // CPU geometry for Cook
    std::function<std::vector<float>(int,int,int&)> vatBaker;                      // V79 VAT bake hook
    std::function<void(int,int,hslcook::ExportMesh&)> hzAnimExtractor;             // V79 HZANIM skeletal hook
    std::vector<uint8_t> bgOgg;                                                    // env background loop -> FMOD asset

    // ── UI toolkit ──
    ui::Font font, mono; ui::UIDraw uiDraw; ui::Context cx; ui::DrawList dl;
    int fbW = 0, fbH = 0; float uiScale = 1.f; double lastT = 0.0;

    // ── selection / outliner ──
    int  selected = -1;            // the ACTIVE object (gizmo origin, properties); -1 = none
    std::vector<int> sel;          // the full selection set (multi-select); `selected` is its active member
    bool showLocal = false;
    bool inSel(int i) const { for (int s : sel) if (s==i) return true; return false; }
    void selectOne(int i){ sel.clear(); if (i>=0) sel.push_back(i); selected=i; r->selectedMesh=i; }
    void toggleSel(int i){ if (i<0) return; for (size_t k=0;k<sel.size();++k) if (sel[k]==i){ sel.erase(sel.begin()+k); selected = sel.empty()?-1:sel.back(); r->selectedMesh=selected; return; } sel.push_back(i); selected=i; r->selectedMesh=i; }
    void deselectAll(){ sel.clear(); selected=-1; r->selectedMesh=-1; }
    static bool isBackdrop(const std::string& n){ auto h=n; for (auto& c:h) c=(char)tolower(c); return h.find("sky")!=std::string::npos||h.find("backdrop")!=std::string::npos||h.find("skybox")!=std::string::npos; }
    char search[96] = "";
    float outlinerScroll = 0.f, propScroll = 0.f;
    bool  scrollToSel = false;
    bool  didAutoSel = false;      // auto-select a centered object once (frame 1)
    // right-click context menu
    bool  ctxOpen = false; float ctxX = 0, ctxY = 0; int ctxMesh = -1;
    float ctxRX = 0, ctxRY = 0, ctxRW = 0, ctxRH = 0;   // its on-screen rect (so clicks route to it)
    bool  insideCtx(float x, float y) const { return x>=ctxRX && y>=ctxRY && x<ctxRX+ctxRW && y<ctxRY+ctxRH; }

    // ── scene items (spawn/chair/collider/navmesh/wall/hotspot — addable/removable/positionable/cookable) ──
    std::vector<sitem::Item> items;
    std::vector<int> animColliders;   // mesh indices marked as ANIMATED colliders (same-entity kinematic collider in the cook)
    // ── player simulator (walk the env in-editor to test navmesh/spawn/floor; the cam is glued to the walkable surface) ──
    bool playSim = false; float pVelY = 0.f;
    std::vector<float> simV; std::vector<uint32_t> simI;   // cached walkable triangles for the sim
    int  selItem = -1;             // index into items (-1 = none; mesh selection active instead)
    bool showItems = true;         // item markers visible (the things you add are always shown)
    bool showType[sitem::TYPE_COUNT] = { true,true,true,true,true,true,true };   // per-Meta-component visibility toggles
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
    void addItem(int type) {
        if (type==sitem::NAVMESH) { addNavmesh(sel.empty()?1:2); return; }   // navmesh has its own mode-aware path
        sitem::Item it; it.type=type; it.name=std::string(sitem::typeName(type))+" "+std::to_string(items.size()+1);
        float cp=std::cos(r->cam.pitch); float fwd[3]={std::sin(r->cam.yaw)*cp, std::sin(r->cam.pitch), -std::cos(r->cam.yaw)*cp};
        it.pos[0]=r->cam.pos[0]+fwd[0]*2.5f; it.pos[1]=r->cam.pos[1]+fwd[1]*2.5f; it.pos[2]=r->cam.pos[2]+fwd[2]*2.5f;
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; tab=TAB_OBJECT;
    }
    void deleteSelItem() { if (selItem>=0 && selItem<(int)items.size()) { items.erase(items.begin()+selItem); selItem=-1; } }
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
    // Sidecar text file next to the env (<env>.hsledit): camera + per-mesh transform/visibility/RENAME + all scene items.
    std::string projectFile() const { return projectPath.empty() ? std::string("editor_project.hsledit") : projectPath + ".hsledit"; }
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
    void saveProject(){
        FILE* f=fopen(projectFile().c_str(),"wb"); if(!f){ setStatus("SAVE FAILED: "+projectFile()); return; }
        fprintf(f,"HSLEDIT 2\n");
        fprintf(f,"CAM %.4f %.4f %.4f %.5f %.5f\n", r->cam.pos[0],r->cam.pos[1],r->cam.pos[2], r->cam.yaw, r->cam.pitch);
        for(int i=0;i<(int)r->gpuMeshes.size();++i){ auto& gm=r->gpuMeshes[i];
            fprintf(f,"MESH %d %s %.5f %.5f %.5f %.6f %.6f %.6f %.6f %.5f %.5f %.5f %d\n", i, qstr(gm.name).c_str(),
                gm.editT[0],gm.editT[1],gm.editT[2], gm.editR[0],gm.editR[1],gm.editR[2],gm.editR[3],
                gm.editS[0],gm.editS[1],gm.editS[2], r->isHidden(i)?1:0); }
        for(auto& it:items){
            fprintf(f,"ITEM %d %s %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d %d %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %d %d",
                it.type, qstr(it.name).c_str(), it.pos[0],it.pos[1],it.pos[2], it.rot[0],it.rot[1],it.rot[2],
                it.scale[0],it.scale[1],it.scale[2], it.allowStart?1:0, it.isLocal?1:0,
                it.exitPos[0],it.exitPos[1],it.exitPos[2], it.half[0],it.half[1],it.half[2], it.propW,it.propH,
                it.navMode, (int)it.srcMeshes.size());
            for(int m:it.srcMeshes) fprintf(f," %d", m);
            fprintf(f,"\n");
        }
        if(!animColliders.empty()){ fprintf(f,"COLLIDERS %d", (int)animColliders.size()); for(int m:animColliders) fprintf(f," %d", m); fprintf(f,"\n"); }
        fclose(f);
        setStatus("Saved -> "+projectFile());
    }
    void loadProject(){
        FILE* f=fopen(projectFile().c_str(),"rb"); if(!f) return;
        std::string all; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ all.resize(n); fread(&all[0],1,n,f);} fclose(f);
        items.clear(); selItem=-1; deselectAll(); animColliders.clear();
        size_t p=0; int meshN=0, itemN=0;
        while(p<all.size()){
            size_t e=all.find('\n',p); std::string line=all.substr(p, e==std::string::npos?std::string::npos:e-p); p=(e==std::string::npos)?all.size():e+1;
            auto t=tokenize(line); if(t.empty()) continue;
            if(t[0]=="CAM" && t.size()>=6){ r->cam.pos[0]=(float)atof(t[1].c_str()); r->cam.pos[1]=(float)atof(t[2].c_str()); r->cam.pos[2]=(float)atof(t[3].c_str()); r->cam.yaw=(float)atof(t[4].c_str()); r->cam.pitch=(float)atof(t[5].c_str()); }
            else if(t[0]=="MESH" && t.size()>=14){ int idx=atoi(t[1].c_str()); if(idx>=0&&idx<(int)r->gpuMeshes.size()){ auto& gm=r->gpuMeshes[idx];
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
                if(it.type==sitem::NAVMESH) bakeNavGeometry(it);
                items.push_back(std::move(it)); itemN++; }
            else if(t[0]=="COLLIDERS"){ int nc=atoi(t[1].c_str()); for(int k=0;k<nc && 2+k<(int)t.size();++k) animColliders.push_back(atoi(t[2+k].c_str())); }
        }
        didAutoSel = true;   // a session was restored -> don't let frame-1 auto-focus clobber the restored camera
        setStatus("Loaded "+std::to_string(meshN)+" mesh edits + "+std::to_string(itemN)+" items from "+projectFile());
    }
    // V79 stores NO navmesh file — the LocomotionSystem generates it from the walkable ground geometry at runtime.
    // So auto-add a NAVMESH item sourced from the env's ground/floor/terrain meshes (the faithful re-creation; the
    // cook PhysX-cooks them into a V203 ColliderMesh). Returns the mesh count (0 = none found -> user picks manually).
    int autoNavmeshFromGround() {
        sitem::Item nv; nv.type=sitem::NAVMESH; nv.name="Navmesh (V79 ground)";
        static const char* kw[] = { "ground","floor","terrain","mainground","lakeshore","walk","path","road","plane","sidewalk","tile" };
        for (int i=0;i<(int)r->gpuMeshes.size();++i){
            if (r->isHidden(i) || isBackdrop(r->gpuMeshes[i].name)) continue;
            std::string n=r->gpuMeshes[i].name; for (auto& c:n) c=(char)tolower(c);
            for (const char* k : kw) if (n.find(k)!=std::string::npos) { nv.srcMeshes.push_back(i); break; }
        }
        if (nv.srcMeshes.empty()) return 0;
        items.push_back(nv);
        fprintf(stderr, "[EDITOR] auto-navmesh from %d V79 walkable ground meshes\n", (int)nv.srcMeshes.size());
        return (int)nv.srcMeshes.size();
    }
    int  localSpawnCount() const { int n=0; for (auto& it:items) if (it.type==sitem::SPAWN && it.allowStart && it.isLocal) ++n; return n; }

    // ── undo/redo ──
    struct Xform { float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; };
    struct UndoOp { std::vector<int> m; std::vector<Xform> b, a; };   // multi-mesh (one drag of a multi-selection = one op)
    std::vector<UndoOp> undoStack, redoStack;
    bool editing = false; int editMesh = -1; Xform editBefore;

    // ── gizmo ──
    int  gizmoOp = 0;            // 0=move 1=rotate 2=scale
    bool gizmoLocal = true;      // local vs world axes
    int  gizmoAxis = -1;         // axis being dragged (0..2), -1 = none
    bool gizmoDrag = false; std::vector<int> gizmoSel; std::vector<Xform> gizmoBeforeV;
    bool gzVisible = false; float gzOrigin[2]={0,0}, gzTip[3][2]={{0,0},{0,0},{0,0}};
    float gzAxisW[3][3];         // cached world-space axis directions
    float gzAxisFace[3]={1,1,1}; // sign of each axis vs the view dir (rotate-drag handedness)
    float gzRing[3][33][2];      // cached projected rotation-ring points (per axis) for hit-test

    // ── layout (draggable ratios) ──
    float rightRatio = 0.235f, outlinerRatio = 0.45f, timelineH = 80.f;
    VkRect2D rcHeader{}, rcViewport{}, rcOutliner{}, rcProps{}, rcTimeline{};
    int dragSplit = 0;           // 1=right border 2=outliner/props border 3=timeline border

    // ── properties tabs ──
    enum { TAB_OBJECT, TAB_SCENE, TAB_MATERIAL, TAB_ANIM, TAB_PHYSICS, TAB_COOK };
    int tab = TAB_OBJECT;

    // ── cook (threaded) ──
    std::thread cookThread; std::atomic<bool> cooking{false}; std::atomic<float> cookProg{0.f};
    std::mutex statusMx; std::string cookStage = "idle", cookStatus;
    std::string cookPkg = "com.environment.outerwilds";
    bool autoSign = true, spoofHaven = true;
    bool installAfterCook = true;      // DEFAULT ON: cook -> sign -> install to the headset. The installer auto-detects
                                       // adb root: ROOT -> install the UNSPOOFED own-package APK (+ auto-select it);
                                       // NO root -> back up the real haven2025, then install the haven2025 SPOOF.
    std::string adbSerial, wifiIp;     // device serial ("" = default); wifiIp -> "adb connect" for wireless adb
    std::thread restoreThread; std::atomic<bool> restoring{false};   // "Restore original Haven 2025" button (runs off the UI thread)
    bool animSkinned = false;  // HZANIM skinned clips (clouds/koi/droids). DEFAULT OFF: the clip cook still emits a malformed
                               // string -> device std::length_error -> crash. Opt-in/experimental until the HZANIM clip is fixed.
    bool noCull = true;        // DEFAULT ON: emit scene-spanning bounds so V205's frustum/occlusion/CLOD/size culler never
                               // drops a mesh = V79-style "draw everything" (old homes had NO env culler). Fixes cooked-home
                               // clipping/disappearing; trades the Quest's culling perf for full visibility. -> HSR_NOCULL

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  INIT / SHUTDOWN
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void init(VkRenderer* renderer, GLFWwindow* window, AudioPlayer* a, bool* animOver, float* animSc, float animDur) {
        r = renderer; win = window; audio = a;
        animOverride = animOver; animScrub = animSc; animDuration = animDur;
        float xs = 1.f, ys = 1.f; glfwGetWindowContentScale(window, &xs, &ys);
        uiScale = (xs > 0.5f) ? xs : 1.f;
        loadUIFont(font, 15.f * uiScale, false);
        mono = font;   // the UI pipeline binds ONE atlas (the main font); "mono" MUST share it or its glyph UVs index garbage
        uiDraw.init(r, &font);
        cx.font = &font; cx.mono = &font;
        // scale the theme metrics for HiDPI
        cx.th.rowH *= uiScale; cx.th.headerH *= uiScale; cx.th.pad *= uiScale; cx.th.indent *= uiScale;
        timelineH *= uiScale;
        r->overlayBegin = [this]() { this->buildFrame(); };
        r->overlayDraw  = [this](VkCommandBuffer cmd) { uiDraw.record(cmd, dl); };
        // (auto-select of a centered, in-front object happens on frame 1 in buildFrame, when the camera matrices are valid)
        ready = true;
    }
    void shutdown() {
        if (cookThread.joinable()) cookThread.join();
        if (restoreThread.joinable()) restoreThread.join();
        if (ready) { vkDeviceWaitIdle(r->device); uiDraw.destroy(); ready = false; }
    }
    ~Editor() { if (cookThread.joinable()) cookThread.join(); if (restoreThread.joinable()) restoreThread.join(); }

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
        if (b == 0 && (ctxOpen || addMenuOpen)) {               // an open popup owns the mouse
            if (action == GLFW_PRESS) { cx.in.down[0]=true; cx.in.pressed[0]=true; cx.in.pressX[0]=cx.in.mx; cx.in.pressY[0]=cx.in.my; if (!insideCtx(cx.in.mx,cx.in.my)) { ctxOpen=false; addMenuOpen=false; } }
            else { cx.in.down[0]=false; cx.in.released[0]=true; }
            return;                                             // (item action is performed in drawContextMenu / drawAddMenu)
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
            if (b == 0 && gzVisible && in3D && !exitDrag) {     // gizmo handle hit-test (cached screen positions)
                int hit = gizmoHitTest(cx.in.mx, cx.in.my);
                if (hit >= 0) {
                    if (selItem>=0) { gizmoDrag=true; gizmoAxis=hit; gizmoSel.clear(); }   // scene-item drag (its own transform)
                    else if (!sel.empty()) { gizmoDrag=true; gizmoAxis=hit; gizmoSel=sel; gizmoBeforeV.clear(); for (int m:sel) gizmoBeforeV.push_back(captureX(r->gpuMeshes[m])); }
                }
            }
        } else if (action == GLFW_RELEASE) {
            cx.in.down[b] = false; cx.in.released[b] = true;
            bool wasExit = exitDrag; if (b==0) exitDrag=false;
            if (b == 0 && gizmoDrag) { std::vector<Xform> after; for (int m:gizmoSel) after.push_back(captureX(r->gpuMeshes[m])); pushUndo(gizmoSel, gizmoBeforeV, after); gizmoDrag=false; gizmoAxis=-1; }
            else if (b == 0 && in3D && !wasExit) {   // a click (not a look-drag) in the 3D area = pick (Shift = add)
                float dx=cx.in.mx-(float)cx.in.pressX[0], dy=cx.in.my-(float)cx.in.pressY[0];
                if (dx*dx+dy*dy < 25.f*uiScale*uiScale) pick(cx.in.mx, cx.in.my, cx.in.shift||cx.in.ctrl);   // Ctrl or Shift = add to multi-selection
            }
            else if (b == 1 && in3D) {   // right-click (no drag) = object context menu
                float dx=cx.in.mx-(float)cx.in.pressX[1], dy=cx.in.my-(float)cx.in.pressY[1];
                if (dx*dx+dy*dy < 25.f*uiScale*uiScale) { pick(cx.in.mx, cx.in.my, false); if (selected>=0){ ctxOpen=true; ctxX=cx.in.mx; ctxY=cx.in.my; ctxMesh=selected; } }
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
        if (key=='P' && !(mods&GLFW_MOD_CONTROL)) { if(playSim) stopSim(); else startSim(); }   // toggle WALK mode (player sim)
        if (key==GLFW_KEY_DELETE && selItem>=0) deleteSelItem();   // remove the selected scene item
    }
    int winW() const { int w=0,h=0; glfwGetWindowSize(win,&w,&h); return w; }
    int winH() const { int w=0,h=0; glfwGetWindowSize(win,&w,&h); return h; }
    // main gates camera/WASD on these
    bool wantsMouse() const { return ctxOpen || cx.active!=0 || cx.kbFocus!=0 || gizmoDrag || exitDrag || dragSplit!=0 || !inRect(rcViewport, cx.in.mx, cx.in.my); }
    bool wantsKeyboard() const { return cx.kbFocus != 0; }

    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    //  FRAME  (overlayBegin: layout -> set viewport pane -> build the DrawList; overlayDraw records it)
    // ════════════════════════════════════════════════════════════════════════════════════════════════════
    void buildFrame() {
        if (!ready) return;
        fbW = (int)r->swapchainExtent.width; fbH = (int)r->swapchainExtent.height;
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
        r->uiViewportRect = rcViewport;                         // the 3D scene scissors to the Viewport pane
        dl.begin(fbW, fbH, &font, uiDraw.whiteU, uiDraw.whiteV);
        cx.dl = &dl; cx.hot = 0;                                // hot recomputed each frame
        // NOTE: do NOT fill the whole window — the 3D scene shows through rcViewport; each panel paints its
        // own opaque background and the layout tiles the rest, so the viewport pane stays the live 3D view.
        drawViewportOverlay();
        drawItems();                                            // spawn/chair/collider/wall/hotspot/navmesh markers
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
        drawGizmo();
        drawHeader();
        drawOutliner();
        drawProperties();
        drawTimeline();
        drawSplitters();
        drawContextMenu();                                      // floating; drawn last = on top
        drawAddMenu();
        cx.drawTooltip();                                       // deferred hover tooltips — drawn ABOVE everything
        cx.in.newFrame();                                       // consume per-frame input edges/deltas
    }

    // ── tiled Blender layout: header strip on top, timeline strip on bottom, a right column (outliner over
    //    properties), and the 3D viewport filling the remaining left/center. Borders are draggable. ──
    void layout() {
        float W=(float)fbW, H=(float)fbH, hH=cx.th.headerH, tH=timelineH;
        float rightW = std::clamp(W*rightRatio, 220.f*uiScale, W*0.5f);
        float midY = hH, midH = H - hH - tH;
        rcHeader   = {{0,0},{(uint32_t)W,(uint32_t)hH}};
        rcViewport = {{0,(int)midY},{(uint32_t)(W-rightW),(uint32_t)midH}};
        float oH = std::clamp(midH*outlinerRatio, 80.f*uiScale, midH-80.f*uiScale);
        rcOutliner = {{(int)(W-rightW),(int)midY},{(uint32_t)rightW,(uint32_t)oH}};
        rcProps    = {{(int)(W-rightW),(int)(midY+oH)},{(uint32_t)rightW,(uint32_t)(midH-oH)}};
        rcTimeline = {{0,(int)(H-tH)},{(uint32_t)W,(uint32_t)tH}};
    }
    void drawSplitters() {
        // vertical: viewport|right column
        float bx = (float)rcOutliner.offset.x, by=(float)rcHeader.extent.height, bh=(float)(rcViewport.extent.height);
        handleSplit(1, bx-3, by, 6, bh, true);
        // horizontal: outliner|properties
        float hy = (float)rcProps.offset.y;
        handleSplit(2, (float)rcOutliner.offset.x, hy-3, (float)rcOutliner.extent.width, 6, false);
        // horizontal: middle|timeline
        handleSplit(3, 0, (float)rcTimeline.offset.y-3, (float)fbW, 6, false);
        // thin separator lines
        dl.rect(bx-1, by, 1, bh, cx.th.splitLine);
        dl.rect((float)rcProps.offset.x, hy-1, (float)rcProps.extent.width, 1, cx.th.splitLine);
        dl.rect(0, (float)rcTimeline.offset.y-1, (float)fbW, 1, cx.th.splitLine);
    }
    void handleSplit(int id, float x, float y, float w, float h, bool vertical) {
        bool hv = cx.hover(x,y,w,h);
        if (hv && cx.in.pressed[0]) dragSplit = id;
        if (dragSplit == id) {
            if (cx.in.down[0]) {
                if (id==1) rightRatio = std::clamp((fbW - cx.in.mx)/(float)fbW, 0.12f, 0.5f);
                else if (id==2) { float midH=(float)rcViewport.extent.height; outlinerRatio = std::clamp((cx.in.my - rcHeader.extent.height)/midH, 0.12f, 0.88f); }
                else if (id==3) timelineH = std::clamp((float)fbH - cx.in.my, 36.f, (float)fbH*0.5f);
            } else dragSplit = 0;
        }
    }

    void drawTimeline() {
        auto& th=cx.th; VkRect2D a=rcTimeline;
        float x=(float)a.offset.x, y=(float)a.offset.y, w=(float)a.extent.width, h=(float)a.extent.height;
        dl.rect(x,y,w,h, th.headerBg);
        float hh=20*uiScale;
        cx.textAligned(x+8*uiScale, y, 120*uiScale, hh, "Timeline", th.textDim, 0);
        if (!animOverride || !animScrub) { cx.textAligned(x+8*uiScale, y+hh, 320*uiScale, h-hh, "(no animation in this scene)", th.textDim, 0); return; }
        float bx=x+8*uiScale, by=y+hh+6*uiScale, bw=58*uiScale, bh=std::max(16.f, h-hh-12*uiScale);
        if (cx.button(ui::hashId("tlplay"), bx, by, bw, bh, animPlaying?"Pause":"Play")) animPlaying = !animPlaying;
        float tx=bx+bw+10*uiScale, tw=std::max(20.f, w-(tx-x)-120*uiScale), ty=by+bh*0.5f-3*uiScale;
        float dur=animDuration>0?animDuration:1.f, frac=std::clamp(*animScrub/dur, 0.f, 1.f);
        dl.rect(tx,ty,tw,6*uiScale, th.field); dl.border(tx,ty,tw,6*uiScale, th.border);
        dl.rect(tx,ty,tw*frac,6*uiScale, th.accent);
        dl.rect(tx+tw*frac-3*uiScale, by, 6*uiScale, bh, th.textSel);             // playhead (advances live while playing)
        if (cx.hover(tx,by,tw,bh) && cx.in.down[0]) { *animScrub = std::clamp((cx.in.mx-tx)/tw,0.f,1.f)*dur; *animOverride=true; }   // scrub
        char b[48]; snprintf(b,sizeof b,"%.2f / %.2fs", *animScrub, dur);
        cx.textAligned(x+w-112*uiScale, y, 104*uiScale, hh, b, th.textDim, 2);
    }

    // Floating object context menu (right-click in the viewport). Items act on ctxMesh.
    void drawContextMenu() {
        if (!ctxOpen) return;
        if (ctxMesh<0 || ctxMesh>=(int)r->gpuMeshes.size()) { ctxOpen=false; return; }
        auto& th=cx.th; VkGpuMesh& gm=r->gpuMeshes[ctxMesh];
        bool soloed=(r->soloMesh==ctxMesh), hidden=r->isHidden(ctxMesh);
        std::string colLbl = gm.dynamicVerts ? (isAnimCollider(ctxMesh) ? "Remove Animated Collider" : "Animated Collider (follows anim)")
                                              : "Add Mesh Collider";
        std::vector<std::pair<std::string,int>> items = {
            {gm.name, -1}, {"Focus / teleport",0}, {soloed?"Unsolo":"Solo only",1},
            {hidden?"Unhide":"Hide",2}, {"Reset transform",3}, {"Copy name",4},
            {colLbl,5} };   // static -> ColliderMesh entity; animated -> same-entity kinematic collider (follows anim)
        float rh=th.rowH+2*uiScale, w=180*uiScale, h=items.size()*rh+6*uiScale;
        float x=ctxX, y=ctxY; if (x+w>fbW) x=fbW-w; if (y+h>fbH) y=fbH-h; if (x<0)x=0; if (y<0)y=0;
        ctxRX=x; ctxRY=y; ctxRW=w; ctxRH=h;
        dl.rect(x,y,w,h, th.panelBg); dl.border(x,y,w,h, th.border);
        dl.pushClip(x,y,w,h);
        float ry=y+3*uiScale;
        for (auto& it : items) {
            if (it.second<0) { cx.textAligned(x+8*uiScale,ry,w-10*uiScale,rh, it.first.c_str(), th.textDim, 0); dl.rect(x+4*uiScale,ry+rh-1,w-8*uiScale,1,th.border); ry+=rh; continue; }
            bool hv=cx.hover(x,ry,w,rh);
            if (hv) dl.rect(x+1,ry,w-2,rh, th.accent);
            cx.textAligned(x+12*uiScale,ry,w-14*uiScale,rh, it.first.c_str(), hv?th.textSel:th.text, 0);
            if (hv && cx.in.pressed[0]) {
                int a=it.second;
                if (a==0) focusMesh(gm);
                else if (a==1) r->soloMesh = soloed?-1:ctxMesh;
                else if (a==2) r->setHidden(ctxMesh, !hidden);
                else if (a==3) { Xform b=captureX(gm); gm.editT[0]=gm.editT[1]=gm.editT[2]=0; gm.editR[0]=gm.editR[1]=gm.editR[2]=0; gm.editR[3]=1; gm.editS[0]=gm.editS[1]=gm.editS[2]=1; recomputeModel(gm); pushUndo(ctxMesh,b,captureX(gm)); }
                else if (a==4) glfwSetClipboardString(win, gm.name.c_str());
                else if (a==5) addMeshCollider(ctxMesh);
                ctxOpen=false;
            }
            ry+=rh;
        }
        dl.popClip();
    }

    void record(VkCommandBuffer cmd) { uiDraw.record(cmd, dl); }

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
        float mx = 220*uiScale;
        for (auto m : menus) { float w = dl.textW(m)+18*uiScale; cx.button(ui::hashId(m), mx, 3*uiScale, w, h-6*uiScale, m); mx += w+2*uiScale; }
        // right side: Save / Load (persist the session) + Cook quick-button + progress
        float bw = 96*uiScale, bh = h-8*uiScale, bx = W - bw - pad;
        if (cooking) { cx.progressBar(bx-150*uiScale, 4*uiScale, 150*uiScale+bw, bh, cookProg.load(), stageStr().c_str()); }
        else {
            if (cx.button(ui::hashId("hdrcook"), bx, 4*uiScale, bw, bh, "Cook APK", true)) { tab=TAB_COOK; startCook(); }
            float sw=56*uiScale;
            if (cx.button(ui::hashId("hdrload"), bx-sw-4*uiScale, 4*uiScale, sw, bh, "Load")) loadProject();
            if (cx.button(ui::hashId("hdrsave"), bx-2*sw-8*uiScale, 4*uiScale, sw, bh, "Save")) saveProject();
        }
    }

    void drawViewportOverlay() {
        auto& th = cx.th; VkRect2D v = rcViewport;
        dl.pushClip((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,(float)v.extent.height);
        // little viewport header bar
        float bh = 20*uiScale;
        dl.rect((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,bh, ui::withA(th.headerBg,210));
        cx.textAligned(v.offset.x+8*uiScale, v.offset.y, 200*uiScale, bh, "Viewport", th.textDim, 0);
        // transform-mode pills (G/R/S) + axis space
        const char* ops[]={"Move","Rotate","Scale"}; float px=v.offset.x+90*uiScale;
        for (int i=0;i<3;i++){ float w=dl.textW(ops[i])+14*uiScale; if (cx.tab(ui::hashId(2000+i, 7), px, v.offset.y, w, bh, ops[i], gizmoOp==i)) gizmoOp=i; px+=w; }
        { const char* s = gizmoLocal?"Local":"World"; float w=dl.textW(s)+14*uiScale; if (cx.tab(ui::hashId("axspace"), px+6*uiScale, v.offset.y, w, bh, s, false)) gizmoLocal=!gizmoLocal; px+=w+6*uiScale; }
        { const char* s = playSim?"Stop (P)":"Walk (P)"; float w=dl.textW(s)+16*uiScale; if (cx.tab(ui::hashId("walksim"), px+10*uiScale, v.offset.y, w, bh, s, playSim)) { if(playSim) stopSim(); else startSim(); } px+=w+16*uiScale; }
        // camera fly speed (drag or type) — in the header strip so it never fires a viewport pick
        { cx.textAligned(px, v.offset.y, 34*uiScale, bh, "Spd", th.textDim, 0); cx.dragFloat(ui::hashId("camspd"), px+34*uiScale, v.offset.y+1*uiScale, 54*uiScale, bh-2*uiScale, r->cam.speed, 0.1f, "%.1f"); }
        if (playSim) cx.textAligned(v.offset.x+8*uiScale, v.offset.y+v.extent.height-40*uiScale, v.extent.width-16, 18*uiScale, "WALK MODE — WASD+mouse to walk the navmesh, P to exit", ui::rgba(120,230,140), 0);
        // overlay toggles (the navmesh/collider/spawn gizmos) — SEPARATE + all OFF by default, right of the header
        const char* ov[3]={"Navmesh","Colliders","Spawn"}; bool* ovp[3]={&r->showNavmesh,&r->showCollision,&r->showSpawn};
        float ox = v.offset.x + v.extent.width - 6*uiScale;
        for (int i=2;i>=0;--i){ float ow=dl.textW(ov[i])+16*uiScale; ox-=ow+3*uiScale; if (cx.tab(ui::hashId(2100+i,9), ox, (float)v.offset.y, ow, bh, ov[i], *ovp[i])) *ovp[i]=!*ovp[i]; }
        // stats (bottom-left)
        char st[128]; snprintf(st,sizeof st,"%zu objects   sel: %s", r->gpuMeshes.size(), selected>=0?r->gpuMeshes[selected].name.c_str():"-");
        cx.textAligned(v.offset.x+8*uiScale, v.offset.y+v.extent.height-20*uiScale, v.extent.width-16, 18*uiScale, st, th.textDim, 0);
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
        // search field
        std::string sb = search; float fw = std::max(36.f*uiScale, std::min(110.f*uiScale, w-(addX+addW+14*uiScale-x)));
        cx.textField(ui::hashId("osearch"), x+w-fw-6*uiScale, y+3*uiScale, fw, hh-6*uiScale, sb);
        strncpy(search, sb.c_str(), sizeof(search)-1); search[sizeof(search)-1]=0;
        // scrollable content: SCENE ITEMS (the things you add) then MESHES
        float listY = y+hh, listH = h-hh, rowH = th.rowH;
        dl.pushClip(x, listY, w, listH);
        if (cx.hover(x,listY,w,listH) && cx.in.wheel!=0) outlinerScroll -= cx.in.wheel*rowH*3;
        int nItems=(int)items.size(), nMesh=(int)r->gpuMeshes.size();
        float total = (nItems?(nItems+1)*rowH:0) + (nMesh+1)*rowH;
        outlinerScroll = std::clamp(outlinerScroll, 0.f, std::max(0.f, total-listH));
        float ry = listY - outlinerScroll;
        auto onScreen=[&](float yy){ return yy+rowH>listY && yy<listY+listH; };
        if (nItems) {
            if (onScreen(ry)) cx.textAligned(x+6*uiScale,ry,w,rowH,"SCENE ITEMS",th.textDim,0); ry+=rowH;
            for (int i=0;i<nItems;++i, ry+=rowH) { if (!onScreen(ry)) continue;
                auto& it=items[i]; bool seld=(i==selItem);
                if (seld) dl.rect(x,ry,w,rowH,th.rowSel); else if (cx.hover(x,ry,w,rowH)) dl.rect(x,ry,w,rowH,th.rowHover);
                char lbl[180]; snprintf(lbl,sizeof lbl,"%s   %s", it.name.c_str(), sitem::typeName(it.type));
                cx.textAligned(x+16*uiScale,ry,w-36*uiScale,rowH,lbl, seld?th.textSel:th.text, 0);
                float ex=x+w-15*uiScale; bool dh=cx.hover(ex-3*uiScale,ry,18*uiScale,rowH);
                cx.textAligned(ex,ry,14*uiScale,rowH,"x", dh?ui::rgba(255,120,120):th.textDim, 0);
                if (!addMenuOpen && cx.hover(x,ry,w,rowH) && cx.in.pressed[0]) { if (dh) { items.erase(items.begin()+i); selItem=-1; dl.popClip(); return; } selItem=i; deselectAll(); focusItem(i); }   // pick -> focus on it
            }
        }
        if (onScreen(ry)) cx.textAligned(x+6*uiScale,ry,w,rowH,"MESHES",th.textDim,0); ry+=rowH;
        for (int i=0;i<nMesh;i++, ry+=rowH) {
            if (!onScreen(ry)) continue;
            auto& gm = r->gpuMeshes[i];
            if (search[0] && gm.name.find(search)==std::string::npos) { ry-=rowH; continue; }
            bool vis = !r->isHidden(i);
            auto rr = cx.treeRow(ui::hashId(3000+i,11), x, ry, w, rowH, (gm.name+"  ["+std::to_string(i)+"]").c_str(), 0, false, false, inSel(i), vis);
            if (addMenuOpen) rr = ui::Context::RowResult{};   // the Add menu overlays the list -> ignore row clicks under it
            if (rr.clicked) { selItem=-1; if (cx.in.shift||cx.in.ctrl) toggleSel(i); else selectOne(i); }   // Ctrl/Shift = add to multi-selection
            if (rr.toggledVis) r->setHidden(i, vis);
            if (rr.clicked && cx.in.pressed[0] && (cx.t - lastClickT < 0.3f) && lastClickIdx==i) focusMesh(gm);
            if (rr.clicked) { lastClickT = cx.t; lastClickIdx = i; }
        }
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
        y+=4*uiScale;
        switch (it.type){
          case sitem::SPAWN: { cx.checkbox(ui::hashId("spstart"), x, y, "allowStart (this is a player start)", it.allowStart); y+=rh;
                               cx.checkbox(ui::hashId("splocal"), x, y, "local (else: remote players)", it.isLocal); y+=rh; break; }
          case sitem::CHAIR: { vecRowF("Exit pos", it.exitPos, 3, 0.01f, x, y, w); cx.label(x,y,w,rh,"(where the avatar stands up)",th.textDim); y+=rh;
                               if (cx.button(ui::hashId("exitgiz"), x, y, w, rh, editExit?"Editing EXIT with gizmo (click to stop)":"Move exit point with the GIZMO", editExit)) editExit=!editExit;
                               y+=rh+2*uiScale;
                               if (cx.button(ui::hashId("exitcam"), x, y, w, rh, "Set exit point to camera position")) {
                                   it.exitPos[0]=r->cam.pos[0]-it.pos[0]; it.exitPos[1]=(r->cam.pos[1]-1.6f)-it.pos[1]; it.exitPos[2]=r->cam.pos[2]-it.pos[2]; }
                               y+=rh+2*uiScale; break; }
          case sitem::BOXCOL: { vecRowF("Half size", it.half, 3, 0.01f, x, y, w); cx.label(x,y,w,rh,"(invisible wall / path blocker)",th.textDim); y+=rh; break; }
          case sitem::WALLPLACE: { cx.label(x,y,78*uiScale,rh,"Max W",th.textDim); cx.dragFloat(ui::hashId("wpw"),x+80*uiScale,y,w-80*uiScale,rh,it.propW,0.01f); y+=rh+2*uiScale;
                                   cx.label(x,y,78*uiScale,rh,"Max H",th.textDim); cx.dragFloat(ui::hashId("wph"),x+80*uiScale,y,w-80*uiScale,rh,it.propH,0.01f); y+=rh; break; }
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
        cx.label(x,y,w,rh,"Meta Components",th.text); y+=rh;
        cx.label(x,y,w,rh*0.9f,"eye = show markers     + Add = spawn one",th.textDim); y+=rh+4*uiScale;
        int types[]={sitem::SPAWN,sitem::CHAIR,sitem::BOXCOL,sitem::NAVMESH,sitem::WALLPLACE,sitem::HOTSPOT,sitem::BOUNDARY};
        for (int t : types){
            int cnt=0; for (auto& it:items) if (it.type==t) cnt++;
            dl.rect(x, y+3*uiScale, 10*uiScale, rh-6*uiScale, typeColor(t,true));               // colour key
            eyeToggle(x+26*uiScale, y+rh*0.5f, showType[t]);                                    // EYE visibility toggle
            char lbl[80]; snprintf(lbl,sizeof lbl,"%s  (%d)", sitem::typeName(t), cnt);
            cx.label(x+40*uiScale, y, w-98*uiScale, rh, lbl, th.text);
            if (cx.button(ui::hashId(7100u+t, 7u), x+w-54*uiScale, y+1*uiScale, 52*uiScale, rh-2*uiScale, "+ Add", true)) addItem(t);
            y+=rh+1*uiScale;
            cx.textAligned(x+40*uiScale, y, w-44*uiScale, rh*0.85f, sitem::metaName(t), th.textDim, 0, mono.ok?&mono:&font); y+=rh*0.85f+6*uiScale;
        }
    }

    void drawProperties() {
        auto& th = cx.th; VkRect2D a = rcProps;
        float x=(float)a.offset.x, y=(float)a.offset.y, w=(float)a.extent.width, h=(float)a.extent.height;
        dl.rect(x,y,w,h, th.panelBg);
        // tab strip
        float th_h = 22*uiScale; const char* tabs[]={"Object","Scene","Material","Anim","Physics","Cook"};
        float tx=x, tw=w/6.f;
        for (int i=0;i<6;i++){ if (cx.tab(ui::hashId(4000+i,13), tx, y, tw, th_h, tabs[i], tab==i)) tab=i; tx+=tw; }
        dl.rect(x,y+th_h,w,1,th.splitLine);
        float cy = y+th_h+6*uiScale, cx0 = x+8*uiScale, cw = w-16*uiScale;
        dl.pushClip(x, y+th_h, w, h-th_h);
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
        if (editing && cx.in.released[0]) endEdit(gm);
        if (cx.button(ui::hashId("resetx"), x, y, 120*uiScale, th.rowH, "Reset Transform")) {
            Xform b=captureX(gm); gm.editT[0]=gm.editT[1]=gm.editT[2]=0; gm.editR[0]=gm.editR[1]=gm.editR[2]=0; gm.editR[3]=1; gm.editS[0]=gm.editS[1]=gm.editS[2]=1; recomputeModel(gm); pushUndo(selected,b,captureX(gm));
        }
        if (cx.button(ui::hashId("focusx"), x+126*uiScale, y, 80*uiScale, th.rowH, "Focus")) focusMesh(gm);
        y += th.rowH+8*uiScale;
        char b[96]; snprintf(b,sizeof b,"indices: %u    centroid %.1f %.1f %.1f", gm.nIdx, gm.centroid[0],gm.centroid[1],gm.centroid[2]);
        cx.label(x,y,w,th.rowH,b,th.textDim); y+=th.rowH;
        snprintf(b,sizeof b,"skinned: %s   dynamic: %s", gm.isSkinned?"yes":"no", gm.dynamicVerts?"yes":"no");
        cx.label(x,y,w,th.rowH,b,th.textDim);
    }
    void drawMaterialTab(VkGpuMesh& gm, float x, float y, float w) {
        auto& th=cx.th; char b[96];
        snprintf(b,sizeof b,"blend: %s", gm.useBlend?"alpha":(gm.additive?"additive":"opaque")); cx.label(x,y,w,th.rowH,b,th.text); y+=th.rowH;
        snprintf(b,sizeof b,"alphaTest (cutout): %s", gm.alphaTest?"yes":"no"); cx.label(x,y,w,th.rowH,b,th.text); y+=th.rowH;
        snprintf(b,sizeof b,"cull: %s", gm.cullBack?"back (single-sided)":"none (double-sided)"); cx.label(x,y,w,th.rowH,b,th.text); y+=th.rowH;
        if (cx.button(ui::hashId("solox"), x, y+4*uiScale, 90*uiScale, th.rowH, r->soloMesh==selected?"Unsolo":"Solo only")) r->soloMesh = (r->soloMesh==selected)?-1:selected;
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
            if (cx.button(ui::hashId(7800u+(unsigned)i, 5u), x, y, w-58*uiScale, rh, b, seld)) { deselectAll(); selItem=i; tab=TAB_OBJECT; }
            if (cx.button(ui::hashId(7900u+(unsigned)i, 5u), x+w-56*uiScale, y, 54*uiScale, rh, "Delete")) { items.erase(items.begin()+i); if(selItem==i)selItem=-1; else if(selItem>i)selItem--; --i; }
            y+=rh+2*uiScale;
        }
    }

    // ── Cook / Export panel: package name, auto-sign + spoof toggles, Cook button, live progress, status ──
    void drawCookPanel(float x, float y, float w) {
        auto& th=cx.th;
        float y0;
        cx.label(x,y,w,th.rowH,"Cook to bootable Quest APK",th.text); y+=th.rowH+6*uiScale;
        y0=y; cx.label(x,y,90*uiScale,th.rowH,"Package",th.textDim);
        cx.textField(ui::hashId("cookpkg"), x+92*uiScale, y, w-92*uiScale, th.rowH, cookPkg);
        cx.tip(x,y0,w,th.rowH,"Android package id for the UNSPOOFED (rooted) APK,\ne.g. com.environment.outerwilds.\nThe haven2025 spoof always uses Meta's haven2025\npackage and ignores this field."); y+=th.rowH+4*uiScale;
        y0=y; cx.checkbox(ui::hashId("autosign"), x, y, "Auto-sign (zipalign + apksigner)", autoSign);
        cx.tip(x,y0,w,th.rowH,"Sign the APKs so the Quest will install them (unsigned ->\nINSTALL_PARSE_FAILED_NO_CERTIFICATES). Build-tools are\nauto-detected, or auto-downloaded beside the exe on first\nuse (pre-fetch with --fetch-tools). Keep this ON."); y+=th.rowH;
        y0=y; cx.checkbox(ui::hashId("spoof"), x, y, "Emit haven2025 spoof (no-root install)", spoofHaven);
        cx.tip(x,y0,w,th.rowH,"Also build <env>_NoRoot-Spoof.apk, which masquerades as\nMeta's haven2025 home. This is the ONLY way to install on a\nNON-rooted Quest: it replaces haven2025, then you pick\n\"Haven 2025\" in the home menu. Keep this ON."); y+=th.rowH;
        y0=y; cx.checkbox(ui::hashId("hzanim"), x, y, "Animate skinned meshes (HZANIM — EXPERIMENTAL)", animSkinned);
        cx.tip(x,y0,w,th.rowH,"Emit skeletal animation for skinned meshes (clouds/koi/\ndroids). EXPERIMENTAL: the clip cook can still crash the\nenvironment on the device. Leave OFF unless testing."); y+=th.rowH+6*uiScale;
        y0=y; cx.checkbox(ui::hashId("nocull"), x, y, "Draw everything — disable culling (fixes clipping, V79-style)", noCull);
        cx.tip(x,y0,w,th.rowH,"Emit scene-spanning bounds so the V205 shell NEVER culls/clips\nyour meshes (frustum + Hi-Z occlusion + distance + CLOD budget).\nThe old V79 shell had NO environment culler, so this matches how\nold homes looked. Geometry still sits at its real position; only\nculling is defeated. Trades the Quest's culling perf for full\nvisibility. Keep ON if cooked homes clip / disappear at distance."); y+=th.rowH+6*uiScale;
        // ── Install to headset (USB or Wi-Fi adb); the installer auto-detects root and picks spoofed vs unspoofed ──
        y0=y; cx.checkbox(ui::hashId("install"), x, y, "Install to headset after cook (auto)", installAfterCook);
        cx.tip(x,y0,w,th.rowH,"After cooking, install over adb. The installer detects root:\n  ROOT  -> install the UNSPOOFED APK + auto-select it.\n  NO root-> back up the real haven2025, install the SPOOF,\n           and relaunch the shell. The spoof REPLACES Haven 2025\n           in place (unrooted Quests can't switch envs).\nNeeds adb bundled beside the exe or on PATH."); y+=th.rowH+2*uiScale;
        y0=y; cx.label(x,y,64*uiScale,th.rowH,"Wi-Fi IP",th.textDim);
        cx.textField(ui::hashId("wifiip"), x+66*uiScale, y, w-66*uiScale-70*uiScale, th.rowH, wifiIp);
        if (cx.button(ui::hashId("wificon"), x+w-68*uiScale, y, 66*uiScale, th.rowH, "Connect")) wifiConnect();
        cx.tip(x,y0,w,th.rowH,"Wireless adb: type the headset IP (e.g. 192.168.1.35) and\nConnect. Enable Wi-Fi adb on the headset first.\nLeave blank to use a USB cable."); y+=th.rowH+2*uiScale;
        y0=y; cx.label(x,y,64*uiScale,th.rowH,"Device",th.textDim);
        cx.textField(ui::hashId("adbser"), x+66*uiScale, y, w-66*uiScale, th.rowH, adbSerial);
        cx.tip(x,y0,w,th.rowH,"adb device serial to target (see `adb devices`).\nBlank = the default/only device. Set this when several\ndevices are attached at once."); y+=th.rowH;
        cx.label(x,y,w,th.rowH*0.85f,"USB: leave blank. Wi-Fi: type IP, Connect.",th.textDim); y+=th.rowH*0.95f+6*uiScale;
        bool busy = cooking.load();
        if (busy) { cx.progressBar(x, y, w, th.rowH+2*uiScale, cookProg.load(), stageStr().c_str()); }
        else { y0=y; if (cx.button(ui::hashId("cookgo"), x, y, w, th.rowH+4*uiScale, installAfterCook?"COOK + SIGN + INSTALL":"COOK  +  SIGN", true)) startCook();
               cx.tip(x,y0,w,th.rowH+4*uiScale,"Cook the edited scene to APK(s), sign them, and (if Install\nis on) push to the headset. Outputs land next to the loaded\nenv:  <env>_Rooted-System.apk  +  <env>_NoRoot-Spoof.apk"); }
        y += th.rowH+12*uiScale;
        // Undo a spoof: put the REAL Haven 2025 back from the auto-backup (off the UI thread).
        if (!busy && !restoring.load()) {
            y0=y; if (cx.button(ui::hashId("restorehaven"), x, y, w, th.rowH, "Restore original Haven 2025")) {
                if (restoreThread.joinable()) restoreThread.join();
                restoring.store(true);
                restoreThread = std::thread([this]{ restoreHaven(); restoring.store(false); });
            }
            cx.tip(x,y0,w,th.rowH,"Reinstall the ORIGINAL Haven 2025 from the auto-backup\n(folder \"Haven2025_Backup\" beside the exe) + relaunch the shell.\nUse this to undo a spoof install."); y += th.rowH+8*uiScale;
        } else if (restoring.load()) { cx.label(x,y,w,th.rowH,"Restoring Haven 2025…",th.textDim); y += th.rowH+8*uiScale; }
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
    // Highlight the selected mesh's actual TRIANGLES (projected edges). Capped/sampled so huge meshes stay cheap.
    void drawWireframe(VkGpuMesh& gm){
        const auto& P=gm.pickPos; const auto& I=gm.pickIdx;
        if (P.empty() || I.size()<3) { drawAabbBox(gm); return; }            // no CPU geometry -> fall back to a box
        size_t ntri=I.size()/3, maxTri=2500, stride = ntri>maxTri ? ntri/maxTri : 1;
        uint32_t c=ui::rgba(255,170,60,70);
        for (size_t t=0; t<ntri; t+=stride){
            uint32_t a=I[t*3],b=I[t*3+1],d=I[t*3+2];
            if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)d*3+2>=P.size()) continue;
            float wa[3],wb[3],wd[3],sa[2],sb[2],sd[2];
            xformPoint(gm.model,&P[a*3],wa); xformPoint(gm.model,&P[b*3],wb); xformPoint(gm.model,&P[d*3],wd);
            bool oa=worldToScreen(wa,sa[0],sa[1]),ob=worldToScreen(wb,sb[0],sb[1]),od=worldToScreen(wd,sd[0],sd[1]);
            if (oa&&ob) dl.line(sa[0],sa[1],sb[0],sb[1],c,0.7f);
            if (ob&&od) dl.line(sb[0],sb[1],sd[0],sd[1],c,0.7f);
            if (od&&oa) dl.line(sd[0],sd[1],sa[0],sa[1],c,0.7f);
        }
    }
    void drawAabbBox(VkGpuMesh& gm){
        float mn[3],mx[3]; worldAabb(gm,mn,mx);
        float s[8][2]; bool ok[8];
        for (int c=0;c<8;c++){ float p[3]={ (c&1)?mx[0]:mn[0], (c&2)?mx[1]:mn[1], (c&4)?mx[2]:mn[2] }; ok[c]=worldToScreen(p,s[c][0],s[c][1]); }
        static const int E[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        uint32_t c=ui::rgba(255,150,40,160);
        for (auto& e:E) if (ok[e[0]]&&ok[e[1]]) dl.line(s[e[0]][0],s[e[0]][1],s[e[1]][0],s[e[1]][1],c,1.f);
    }
    // oriented wireframe box (pos + R*half corners) — colliders / chairs / wall-placement zones
    void drawBox(const float pos[3], const float half[3], const float q[4], uint32_t col, float thick){
        float s[8][2]; bool ok[8];
        for (int c=0;c<8;c++){ float lc[3]={ (c&1)?half[0]:-half[0], (c&2)?half[1]:-half[1], (c&4)?half[2]:-half[2] }, w[3]; quatRotVec(q,lc,w); w[0]+=pos[0];w[1]+=pos[1];w[2]+=pos[2]; ok[c]=worldToScreen(w,s[c][0],s[c][1]); }
        static const int E[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        for (auto& e:E) if (ok[e[0]]&&ok[e[1]]) dl.line(s[e[0]][0],s[e[0]][1],s[e[1]][0],s[e[1]][1],col,thick);
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
        if (!showItems || items.empty()) return;
        exitHVis=false;
        VkRect2D v=rcViewport; dl.pushClip((float)v.offset.x,(float)v.offset.y,(float)v.extent.width,(float)v.extent.height);
        for (int i=0;i<(int)items.size();++i){
            auto& it=items[i]; if (!showType[it.type]) continue;
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
              case sitem::BOXCOL: { float h[3]={it.half[0]*it.scale[0],it.half[1]*it.scale[1],it.half[2]*it.scale[2]}; drawBox(it.pos,h,q,col,th); break; }
              case sitem::CHAIR: { float h[3]={0.24f*it.scale[0],0.24f*it.scale[1],0.24f*it.scale[2]}; drawBox(it.pos,h,q,col,th); drawFacingArrow(it.pos,q,col,th,0.55f);
                float ep[3]={it.pos[0]+it.exitPos[0], it.pos[1]+it.exitPos[1], it.pos[2]+it.exitPos[2]}, es[2];   // draggable exit handle (ground)
                if (worldToScreen(ep,es[0],es[1])){ float hs=(seld?5:3)*uiScale; dl.rect(es[0]-hs,es[1]-hs,hs*2,hs*2,col); dl.border(es[0]-hs,es[1]-hs,hs*2,hs*2,ui::rgba(20,20,20),1);
                    if (on) dl.line(s[0],s[1],es[0],es[1],ui::withA(col,130),1.f);
                    if (seld){ exitHVis=true; exitHS[0]=es[0]; exitHS[1]=es[1]; cx.textAligned(es[0]+8*uiScale,es[1]-8*uiScale,80*uiScale,16*uiScale,"exit",col,0); } } break; }
              case sitem::WALLPLACE: { float h[3]={it.propW*0.5f,it.propH*0.5f,0.02f}; drawBox(it.pos,h,q,col,th); break; }
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
        for (int i=0;i<(int)items.size();++i){ if (!showType[items[i].type]) continue; float p[3]; itemMarkerPos(items[i],p); float s[2]; if (!worldToScreen(p,s[0],s[1])) continue; float d=(s[0]-mx)*(s[0]-mx)+(s[1]-my)*(s[1]-my); if (d<bd){bd=d;best=i;} }
        return best;
    }
    int gizmoHitTest(float mx, float my) {
        if (!gzVisible) return -1; int best=-1;
        if (gizmoOp==1){   // rotate: nearest projected ring segment within tolerance
            float bestD=11*uiScale;
            for (int k=0;k<3;k++) for (int s=0;s<32;s++){ float d=distToSeg(mx,my, gzRing[k][s][0],gzRing[k][s][1], gzRing[k][s+1][0],gzRing[k][s+1][1]); if (d<bestD){bestD=d;best=k;} }
            return best;
        }
        float bestD=14*uiScale;
        for (int k=0;k<3;k++){ float d=distToSeg(mx,my, gzOrigin[0],gzOrigin[1], gzTip[k][0],gzTip[k][1]); if (d<bestD){bestD=d;best=k;} }
        return best;
    }
    static float distToSeg(float px,float py,float ax,float ay,float bx,float by){
        float dx=bx-ax,dy=by-ay,l2=dx*dx+dy*dy; if(l2<1e-6f) return std::hypot(px-ax,py-ay);
        float t=std::clamp(((px-ax)*dx+(py-ay)*dy)/l2,0.f,1.f);
        return std::hypot(px-(ax+t*dx), py-(ay+t*dy));
    }
    void drawGizmo() {
        gzVisible = false;
        bool itemMode = selItem>=0 && selItem<(int)items.size();
        if (!itemMode && (selected<0 || selected>=(int)r->gpuMeshes.size())) return;
        float origin[3], gizQuat[4]={0,0,0,1};
        if (itemMode) { auto& it=items[selItem];
            if (editExit && it.type==sitem::CHAIR) { origin[0]=it.pos[0]+it.exitPos[0]; origin[1]=it.pos[1]+it.exitPos[1]; origin[2]=it.pos[2]+it.exitPos[2]; gizQuat[0]=gizQuat[1]=gizQuat[2]=0; gizQuat[3]=1; }   // gizmo on the EXIT point
            else { itemMarkerPos(it, origin); eulerToQuat(it.rot, gizQuat); }   // navmesh gizmo sits on its geometry, not origin
        }
        else { VkGpuMesh& gm=r->gpuMeshes[selected]; origin[0]=gm.centroid[0]+gm.editT[0]; origin[1]=gm.centroid[1]+gm.editT[1]; origin[2]=gm.centroid[2]+gm.editT[2]; memcpy(gizQuat,gm.editR,16); }
        float ds[3]={ origin[0]-r->cam.pos[0], origin[1]-r->cam.pos[1], origin[2]-r->cam.pos[2] };
        float dist = std::sqrt(ds[0]*ds[0]+ds[1]*ds[1]+ds[2]*ds[2]); if (dist<0.1f) dist=0.1f;
        // FIXED on-screen size (~78px) regardless of distance: len_world = px * dist / (focalY * vpH/2)
        float fy = std::fabs(r->cam.proj[5]); if (fy<1e-3f) fy=1.f;
        float len = (78.f*uiScale) * dist / (fy * rcViewport.extent.height * 0.5f);
        float ax[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        if (gizmoLocal) for (int k=0;k<3;k++){ float a[3]={ax[k][0],ax[k][1],ax[k][2]},o[3]; quatRotVec(gizQuat,a,o); memcpy(ax[k],o,12); }
        memcpy(gzAxisW, ax, sizeof ax);
        float os[2]; if (!worldToScreen(origin, os[0], os[1])) return;
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
            for (int k=0;k<3;k++){ bool hot=(hk==k)||(gizmoDrag&&gizmoAxis==k); uint32_t c=hot?ui::rgba(255,235,80):col[k];
                for (int s=0;s<32;s++) dl.line(gzRing[k][s][0],gzRing[k][s][1],gzRing[k][s+1][0],gzRing[k][s+1][1], c, hot?3.f:2.f);
                gzTip[k][0]=gzRing[k][0][0]; gzTip[k][1]=gzRing[k][0][1];
                cx.textAligned(gzRing[k][0][0]+4*uiScale, gzRing[k][0][1]-8*uiScale, 14*uiScale,16*uiScale, axn[k], c, 0); }
        } else {            // ── MOVE / SCALE: three axis handles ──
            for (int k=0;k<3;k++){
                float tip[3]={origin[0]+ax[k][0]*len, origin[1]+ax[k][1]*len, origin[2]+ax[k][2]*len};
                float ts[2]; if (!worldToScreen(tip, ts[0], ts[1])) { gzTip[k][0]=os[0]; gzTip[k][1]=os[1]; continue; }
                gzTip[k][0]=ts[0]; gzTip[k][1]=ts[1];
                bool hot = (gizmoHitTest((float)cx.in.mx,(float)cx.in.my)==k) || (gizmoDrag&&gizmoAxis==k);
                uint32_t c = hot ? ui::rgba(255,235,80) : col[k];
                dl.line(os[0],os[1],ts[0],ts[1], c, hot?4.f:3.f);
                float hs=(gizmoOp==2?6.f:7.f)*uiScale;
                if (gizmoOp==2) { dl.rect(ts[0]-hs,ts[1]-hs,hs*2,hs*2,c); dl.border(ts[0]-hs,ts[1]-hs,hs*2,hs*2, ui::rgba(20,20,20), 1.5f); }  // scale = colored box handle
                else dl.triangle(ts[0]-hs,ts[1]+hs, ts[0]+hs,ts[1]+hs, ts[0],ts[1]-hs*1.4f, c);            // move = arrowhead
                cx.textAligned(ts[0]+hs+1, ts[1]-8*uiScale, 14*uiScale, 16*uiScale, axn[k], c, 0);
            }
        }
        dl.rect(os[0]-4*uiScale, os[1]-4*uiScale, 8*uiScale, 8*uiScale, ui::rgba(240,240,240));        // center pivot square
        gzVisible = true;
        dl.popClip();
        // apply an in-progress drag (uses this frame's accumulated mouse delta) to the WHOLE selection
        if (gizmoDrag && gizmoAxis>=0 && cx.in.down[0] && (cx.in.dmx!=0||cx.in.dmy!=0)) applyGizmoDrag(len);
    }
    void applyGizmoDrag(float len) {
        int k=gizmoAxis; float* A=gzAxisW[k];
        bool exitMode = editExit && selItem>=0 && selItem<(int)items.size() && items[selItem].type==sitem::CHAIR;
        if (gizmoOp==1) {   // ── ROTATE: tangential mouse drag about world axis A (item euler OR mesh quat) ──
            if (exitMode) return;   // the exit point has no rotation
            float rox=cx.in.mx-gzOrigin[0], roy=cx.in.my-gzOrigin[1]; float rl=std::sqrt(rox*rox+roy*roy); if (rl<4.f) return;
            float tx=-roy/rl, ty=rox/rl;                                   // screen tangent at the cursor
            float drag = cx.in.dmx*tx + cx.in.dmy*ty;                      // px swept around the ring
            float ang  = drag * 0.0075f * (gzAxisFace[k]>0?-1.f:1.f);      // radians (sign tracks the cursor)
            float h=std::sin(ang*0.5f), dq[4]={A[0]*h,A[1]*h,A[2]*h,std::cos(ang*0.5f)};
            if (selItem>=0 && selItem<(int)items.size()) {                 // item: rotate its euler via a world-axis delta quat
                auto& it=items[selItem]; float q0[4]; eulerToQuat(it.rot,q0); float nq[4]; quatMul(dq,q0,nq); normalizeQuat(nq); quatToEuler(nq,it.rot); return;
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
            if (r->isHidden((int)i)) continue;
            const MeshData& md=(*sceneMeshes)[i]; const VkGpuMesh& gm=r->gpuMeshes[i];
            size_t nv=md.positions.size()/3; if (nv<3||md.indices.size()<3) continue;
            ExportMesh em; em.name=md.name; em.positions.resize(nv*3);
            for (size_t v=0;v<nv;v++){ float p[3]={md.positions[v*3],md.positions[v*3+1],md.positions[v*3+2]},o[3]; xformPoint(gm.model,p,o); em.positions[v*3]=o[0]; em.positions[v*3+1]=o[1]; em.positions[v*3+2]=o[2]; }
            em.uvs=md.uvs; em.indices=md.indices; em.blend = gm.useBlend||gm.additive;
            em.wantCollider = isAnimCollider((int)i);   // user marked this animated mesh -> same-entity kinematic collider
            for (int k=0;k<4;k++) em.matTint[k]=md.tint[k];
            int fcols=0,frows=0;
            if (detectAndCollapseFlipbook(em.positions, em.uvs, em.indices, fcols, frows)) { em.flipbook=true; em.flipCols=fcols; em.flipRows=frows; em.blend=true; }
            else { if (vatBaker){ int bnv=0; auto off=vatBaker((int)i,64,bnv); if(!off.empty()&&bnv==(int)nv){ em.vatOffsets=std::move(off); em.vatFrames=64; } }
                   if (hzAnimExtractor) hzAnimExtractor((int)i,64,em); }
            if (md.hasTexture && md.texRGBA.size()>=(size_t)md.texW*md.texH*4){ em.rgba=md.texRGBA; em.w=md.texW; em.h=md.texH; }
            ems.push_back(std::move(em));
        }
        return ems;
    }
    static std::string cookShellPath() { const char* v=std::getenv("HSR_COOK_SHELL"); return v?v:"(embedded shell)"; }   // donor is baked in; path is a label only
    std::string cookOutPath() {   // STANDALONE: write next to the loaded env (always writable), not a fixed cooker/out
        if (const char* v=std::getenv("HSR_COOK_OUT")) return v;
        if (!projectPath.empty()) { size_t sl=projectPath.find_last_of("/\\");
            std::string dir = (sl==std::string::npos)? std::string(".") : projectPath.substr(0,sl);
            std::string base = (sl==std::string::npos)? projectPath : projectPath.substr(sl+1);
            size_t dot=base.rfind('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
            return dir + "/" + base + "_cooked.apk"; }
        return "cooker/out/edited_export.apk";
    }

    void setStage(float f, const char* s){ cookProg.store(f); std::lock_guard<std::mutex> l(statusMx); cookStage=s; }
    void setStatus(const std::string& s){ std::lock_guard<std::mutex> l(statusMx); cookStatus=s; fprintf(stderr,"[COOK] %s\n", s.c_str()); }

    // shared cook core (used by the worker AND the headless CLI). terminalBar = print a \r progress bar to stderr.
    void runCook(std::vector<hslcook::ExportMesh> ems, std::array<float,3> camSpawn, std::string pkg, bool sign, bool spoof, bool terminalBar, std::vector<sitem::Item> sceneItems) {
        using namespace hslcook;
        if (ems.empty()) { setStatus("ERROR: no exportable meshes"); cooking.store(false); return; }
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
        std::vector<uint8_t> vspv, fspv;
        auto apk = exportSceneAPK(ems, nuxd, vspv, fspv, true, &ok, spawn, &sceneZip, bgOgg, progress, sceneItems);
        if (!ok || apk.empty()) { setStatus("ERROR: cook failed (shell: "+nuxd+")"); cooking.store(false); return; }
        if (!writeFile(out, apk)) { setStatus("ERROR: cannot write "+out); cooking.store(false); return; }
        std::string finalSystem, finalSpoof, msg = "Cooked "+std::to_string(ems.size())+" meshes ("+std::to_string(apk.size()/1024)+"KB)";
        // ── own-package APK (sign -> <env>_Rooted-System.apk; drop the unsigned intermediate on success) ──
        if (sign) {
            if (signApk(out, systemOut, progress)) { finalSystem=systemOut; std::remove(out.c_str()); msg += "  | system(rooted) APK: "+systemOut; }
            else { finalSystem=out; msg += "  | sign FAILED (UNSIGNED "+out+"; run `--fetch-tools`)"; }
        } else finalSystem=out;
        // ── haven2025 spoof APK (-> <env>_NoRoot-Spoof.apk) ──
        if (spoof && !sceneZip.empty()) {
            bool ok2=false; auto apk2=spliceAPK(nuxd, sceneZip, "com.meta.environment.prod.nuxd", "com.meta.shell.env.footprint.haven2025", &ok2);
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
        if (installAfterCook) {
            progress(0.9f, "detect root");
            bool rooted = deviceIsRooted();
            if (rooted && !finalSystem.empty()) {
                bool inst = installToDevice(finalSystem, pkg, progress);
                msg += inst ? "  || ROOT -> installed UNSPOOFED ("+pkg+") + auto-selected + relaunched shell" : "  || install FAILED (adb/device?)";
            } else if (!finalSpoof.empty()) {
                // SAFE ORDER: back up the ORIGINAL Haven 2025 first; THEN (cert mismatch) uninstall it; THEN install the
                // spoof. Only uninstall if the backup succeeded (or there was nothing installed to back up).
                std::string bkp; HavenBkp hb = backupOriginalHaven(bkp);
                if (hb == HB_FAILED) {
                    msg += "  || ABORTED: could NOT back up the original Haven 2025, so it was left UNTOUCHED (your original is safe). Check the device/storage and retry, or use the Rooted-System APK.";
                } else {
                    setStatus("Replacing Haven 2025: it's signed with Meta's certificate (not ours), so the ORIGINAL is uninstalled first (backup kept in "+havenBackupDir()+"). Restore anytime: --restore-haven.");
                    bool inst = installToDevice(finalSpoof, HAVEN_PKG(), progress, /*uninstallFirst=*/true);
                    msg += inst ? ("  || no root -> "+std::string(hb==HB_OK?("backed up Haven 2025 ("+havenBackupDir()+"), "):"")+"uninstalled the original + installed the SPOOF + relaunched shell. It loads where Haven 2025 does (unrooted can't switch envs); set Haven 2025 as your home if it isn't already. Restore: --restore-haven.")
                                : "  || spoof install FAILED (adb/device? Haven 2025 may be a non-removable system app — try the Rooted-System APK). Restore the original with --restore-haven if needed.";
                }
            } else {
                msg += rooted ? "  || ROOT but no system APK" : "  || no root and no spoof APK (enable the spoof toggle)";
            }
        }
        if (terminalBar) fprintf(stderr, "\n");
        setStatus(msg); setStage(1.f, "Done"); cooking.store(false);
    }
    static bool fileEx(const std::string& p){ FILE* f=fopen(p.c_str(),"rb"); if(f){ fclose(f); return true; } return false; }
    // adb resolution order: $HSR_ADB -> bundled beside the exe (adb.exe + AdbWinApi.dll + AdbWinUsbApi.dll, or a
    // platform-tools/ folder next to the renderer) -> the usual SDK path -> "adb" on PATH. Bundling those 3 files
    // beside the exe means users never need to install Android platform-tools.
    static std::string adbPath() {
        if (const char* a=std::getenv("HSR_ADB")) return a;
        std::string e1=AppConfig::exeRel("adb.exe"), e2=AppConfig::exeRel("platform-tools/adb.exe"), e3=AppConfig::exeRel("adb");
        if (fileEx(e1)) return e1;
        if (fileEx(e2)) return e2;
        if (fileEx(e3)) return e3;          // POSIX (Linux/macOS) bundled next to the binary
        if (fileEx("C:/Android/platform-tools/adb.exe")) return "C:/Android/platform-tools/adb.exe";
        return "adb";   // on PATH
    }
    int runAdb(const std::string& adb, const std::string& sel, const std::string& tail) {
        char cmd[1600]; snprintf(cmd, sizeof cmd, "\"\"%s\"%s %s\"", adb.c_str(), sel.c_str(), tail.c_str()); return system(cmd);
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
    // True if the device's adb shell can act as root (su works, or adbd itself is root, or it's a userdebug build).
    // ROOT lets us install the proper own-package env and auto-select it via `oculuspreferences --setc`; without it
    // we fall back to the haven2025 spoof (which the user picks manually in the home menu).
    bool deviceIsRooted() {
        auto bs=[](std::string p){ for(char&c:p) if(c=='/')c='\\'; return p; };
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
    // Back up the REAL haven2025 off the device BEFORE anything is uninstalled. Keeps the first/pristine backup;
    // never overwrites it. Writes a HOW_TO_RESTORE.txt next to it and reports the folder.
    HavenBkp backupOriginalHaven(std::string& outBkp) {
        auto bs=[](std::string p){ for(char&c:p) if(c=='/')c='\\'; return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        std::string dir = havenBackupDir(); outBkp = havenBackupApk();
        if (fileEx(outBkp)) { fprintf(stderr, "[COOK] Haven 2025 backup already exists (pristine, kept): %s\n", outBkp.c_str()); return HB_OK; }
        std::string out = adbCapture(ADB, sel, std::string("shell pm path ")+HAVEN_PKG());
        size_t p = out.find("package:");
        if (p == std::string::npos) { fprintf(stderr, "[COOK] Haven 2025 not installed on device — nothing to back up\n"); outBkp.clear(); return HB_ABSENT; }
        p += 8; size_t e = out.find_first_of("\r\n", p);
        std::string dev = out.substr(p, e==std::string::npos ? std::string::npos : e-p);
        while (!dev.empty() && (dev.back()=='\r'||dev.back()=='\n'||dev.back()==' '||dev.back()=='\t')) dev.pop_back();
        if (dev.empty()) { outBkp.clear(); return HB_FAILED; }
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        runAdb(ADB, sel, "pull \""+dev+"\" \""+bs(outBkp)+"\"");
        if (!fileEx(outBkp)) { fprintf(stderr, "[COOK] WARN: Haven 2025 backup pull FAILED (%s)\n", outBkp.c_str()); outBkp.clear(); return HB_FAILED; }
        if (FILE* rf = fopen((dir+"/HOW_TO_RESTORE.txt").c_str(), "wb")) {            // plain-language recovery note
            fputs("This folder holds a backup of the ORIGINAL Meta \"Haven 2025\" home APK, taken automatically\n"
                  "before the converter replaced it with a spoofed (custom) home.\n\n"
                  "RESTORE the original Haven 2025:\n"
                  "  hsr_renderer.exe --restore-haven\n"
                  "  (or manually:  adb install -r -d \"haven2025_ORIGINAL_backup.apk\"\n"
                  "                 adb shell kill $(adb shell pidof com.oculus.vrshell) )\n\n"
                  "Do NOT delete this folder — it is the only copy of your original Haven 2025.\n"
                  "If you ever lose it, re-download Haven 2025 from Meta (headset Settings) or factory-reset.\n", rf);
            fclose(rf);
        }
        fprintf(stderr, "[COOK] Backed up REAL Haven 2025 -> %s  (restore: hsr_renderer --restore-haven)\n", dir.c_str());
        return HB_OK;
    }
    // RESTORE the original Haven 2025 from the backup (button + --restore-haven CLI share this idea).
    void restoreHaven() {
        auto bs=[](std::string p){ for(char&c:p) if(c=='/')c='\\'; return p; };
        std::string ADB=bs(adbPath()), sel = adbSerial.empty()? "" : (" -s "+adbSerial), bkp=havenBackupApk();
        if (!fileEx(bkp)) { setStatus("No Haven 2025 backup found at "+bkp+" — nothing to restore. See the guide for re-download / factory-reset."); return; }
        int rc = runAdb(ADB, sel, "install -r -d \""+bs(bkp)+"\"");
        if (rc!=0){ runAdb(ADB, sel, std::string("uninstall ")+HAVEN_PKG()); rc = runAdb(ADB, sel, "install \""+bs(bkp)+"\""); }
        relaunchShell(ADB, sel);
        setStatus(rc==0 ? "Restored the ORIGINAL Haven 2025 from backup + relaunched shell." : "Restore FAILED (adb/device?). Backup is still at "+bkp);
    }
    // Static CLI restore (no Editor instance / no serial) for `hsr_renderer --restore-haven`.
    static int cliRestoreHaven() {
        std::string ADB=adbPath(); for(char&c:ADB) if(c=='/')c='\\';
        std::string bkp=havenBackupApk(), bbkp=bkp; for(char&c:bbkp) if(c=='/')c='\\';
        if (!fileEx(bkp)) { fprintf(stderr, "[RESTORE] no backup at %s — nothing to restore.\n", bkp.c_str()); return 1; }
        auto run=[&](const std::string& tail){ char c[1600]; snprintf(c,sizeof c,"\"\"%s\" %s\"", ADB.c_str(), tail.c_str()); return system(c); };
        int rc = run("install -r -d \""+bbkp+"\"");
        if (rc!=0){ run(std::string("uninstall ")+HAVEN_PKG()); rc = run("install \""+bbkp+"\""); }
        std::string pids = adbCapture(ADB, "", "shell pidof com.oculus.vrshell"); std::string pid;
        for (char c : pids){ if(c=='\r'||c=='\n') break; pid.push_back(c); }
        while(!pid.empty() && (pid.back()==' '||pid.back()=='\t')) pid.pop_back();
        if (!pid.empty()){ run("shell su -c \"kill "+pid+"\""); run("shell kill "+pid); }
        fprintf(stderr, rc==0 ? "[RESTORE] restored ORIGINAL Haven 2025 from %s + relaunched shell\n" : "[RESTORE] FAILED (adb/device?). Backup kept at %s\n", bkp.c_str());
        return rc==0 ? 0 : 1;
    }
    // Connect wireless adb to wifiIp (e.g. "192.168.1.35[:5555]"); call before installing over Wi-Fi.
    bool wifiConnect() {
        if (wifiIp.empty()) return false;
        auto bs=[](std::string p){ for(char&c:p) if(c=='/')c='\\'; return p; };
        std::string ADB=bs(adbPath()), ip=wifiIp; if (ip.find(':')==std::string::npos) ip+=":5555";
        bool ok = runAdb(ADB, "", "connect "+ip)==0;
        if (ok && adbSerial.empty()) adbSerial=ip;   // target it for the install
        setStatus(ok?("Wi-Fi adb connected: "+ip):("Wi-Fi connect FAILED: "+ip));
        return ok;
    }
    // Install an APK. uninstallFirst = overlay/spoof case: the existing package (Meta's Haven 2025) is signed with a
    // DIFFERENT certificate than our debug-signed spoof, so Android refuses an in-place update — it MUST be uninstalled
    // first. (The caller backs it up before allowing this.) Then best-effort select + relaunch.
    bool installToDevice(const std::string& apkPath, const std::string& pkg, const std::function<void(float,const char*)>& progress, bool uninstallFirst=false) {
        auto bs=[](std::string p){ for(char&c:p) if(c=='/')c='\\'; return p; };
        std::string ADB=bs(adbPath()), AP=bs(apkPath), sel = adbSerial.empty()? "" : (" -s "+adbSerial);
        if (progress) progress(0.95f, "adb install");
        if (uninstallFirst) runAdb(ADB, sel, "uninstall "+pkg);   // overlay: remove the differently-signed original UP FRONT (cert mismatch -> in-place update is impossible)
        int rc = runAdb(ADB, sel, "install -r -d \""+AP+"\"");    // -d = allow version downgrade
        if (rc!=0 && !uninstallFirst){ runAdb(ADB, sel, "uninstall "+pkg); rc = runAdb(ADB, sel, "install \""+AP+"\""); }   // own-package sig/version clash fallback
        if (rc!=0) return false;
        if (progress) progress(0.98f, "select env");
        // environment_selected = apk://pkg/assets/scene.zip (needs root/su; best-effort — else pick it in the headset).
        runAdb(ADB, sel, "shell su -c \"oculuspreferences --setc environment_selected apk://"+pkg+"/assets/scene.zip\"");
        if (progress) progress(0.99f, "relaunch shell");
        relaunchShell(ADB, sel);
        return true;
    }
    // Make the running shell pick up the freshly-installed env: kill its EXACT pid so it relaunches.
    // ⚠ `am force-stop` does NOT reload the home, and a broad `pkill vrshell` reboots the headset — so target the
    // exact com.oculus.vrshell pid only. Best-effort: su first (rooted), then a plain kill (works if adb shell has it).
    void relaunchShell(const std::string& ADB, const std::string& sel) {
        std::string pids = adbCapture(ADB, sel, "shell pidof com.oculus.vrshell");
        std::string pid; for (char c : pids) { if (c=='\r'||c=='\n') break; pid.push_back(c); }   // 1st line = space-sep pids of the exact pkg
        while (!pid.empty() && (pid.back()==' '||pid.back()=='\t')) pid.pop_back();
        if (pid.empty()) return;
        runAdb(ADB, sel, "shell su -c \"kill "+pid+"\"");   // rooted
        runAdb(ADB, sel, "shell kill "+pid);                // non-root best-effort
    }
    void startCook() {
        if (cooking.load()) return;
        auto ems = buildExportMeshes();
        if (ems.empty()) { setStatus("ERROR: no exportable meshes"); return; }
        if (cookThread.joinable()) cookThread.join();
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
        if (std::getenv("HSR_NOINSTALL")) installAfterCook=false;   // batch/CLI: cook the APK files only, don't touch the device
        auto ems = buildExportMeshes();
        std::array<float,3> spawn{ r->cam.pos[0], r->cam.pos[1], r->cam.pos[2] };
        std::vector<sitem::Item> its=items; bakeNavmeshes(its);
        cooking.store(true); runCook(std::move(ems), spawn, cookPkg, autoSign, spoofHaven, true, std::move(its));
    }
    // The meshes a navmesh draws from: its explicit selection, else the whole walkable scene (non-backdrop, visible).
    std::vector<int> navSourceMeshes(const sitem::Item& si){
        if (!si.srcMeshes.empty()) return si.srcMeshes;
        std::vector<int> all;
        for (int i=0;i<(int)r->gpuMeshes.size();++i) if (!r->isHidden(i) && !isBackdrop(r->gpuMeshes[i].name)) all.push_back(i);
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
        struct TB { float ax,ay,az, bx,by,bz, cx,cy,cz; };   // a walkable triangle (full verts, for height interpolation)
        std::vector<TB> tb; float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
        for (int m:ms){ if(m<0||m>=(int)r->gpuMeshes.size())continue; auto& gm=r->gpuMeshes[m];
            const auto& P=gm.pickPos; const auto& I=gm.pickIdx; if (P.size()<9 || I.size()<3) continue;
            for (size_t k=0;k+2<I.size(); k+=3){ uint32_t a=I[k],b=I[k+1],c=I[k+2];
                if ((size_t)a*3+2>=P.size()||(size_t)b*3+2>=P.size()||(size_t)c*3+2>=P.size()) continue;
                float wa[3],wb[3],wc[3]; xformPoint(gm.model,&P[a*3],wa); xformPoint(gm.model,&P[b*3],wb); xformPoint(gm.model,&P[c*3],wc);
                float e1[3]={wb[0]-wa[0],wb[1]-wa[1],wb[2]-wa[2]}, e2[3]={wc[0]-wa[0],wc[1]-wa[1],wc[2]-wa[2]};
                float nx=e1[1]*e2[2]-e1[2]*e2[1], ny=e1[2]*e2[0]-e1[0]*e2[2], nz=e1[0]*e2[1]-e1[1]*e2[0];
                // SLOPE FILTER — controls how many triangles become collision. Curved/hilly envs (Outer Wilds) have a
                // LOT of steep ground; the old hard 0.5 (drop >60deg) carved holes -> "colander". navMode 2 (explicit
                // selection / auto-ground): include EVERY triangle (slope 0) -> you picked these meshes as the ground,
                // so cover all of them incl. cliffs/curves. navMode 1 (smart, scans the whole scene): keep a filter to
                // skip near-vertical walls but loosen it to 0.15 (keeps up to ~81deg). HSR_NAVSLOPE overrides both.
                float slopeMin = (si.navMode==2) ? 0.0f : 0.15f;
                if(const char* e=std::getenv("HSR_NAVSLOPE")){ float s=(float)atof(e); if(s>=0.0f) slopeMin=s; }
                float nl=std::sqrt(nx*nx+ny*ny+nz*nz); if(nl<1e-9f || (slopeMin>0.0f && std::fabs(ny)/nl < slopeMin)) continue;
                tb.push_back(TB{wa[0],wa[1],wa[2], wb[0],wb[1],wb[2], wc[0],wc[1],wc[2]});
                for (const float* w : {wa,wb,wc}){ mn[0]=std::min(mn[0],w[0]); mx[0]=std::max(mx[0],w[0]); mn[2]=std::min(mn[2],w[2]); mx[2]=std::max(mx[2],w[2]); } }
        }
        if (tb.empty()) return;
        // REUSE the actual walkable triangles (NO rebuilt grid) -> the cook makes one TILTED collision box per triangle,
        // so the collision follows the road's exact shape/height/tilt. (Very dense meshes: the cook falls back to a height
        // grid; the editor caps the stored count for preview sanity.)
        int keep = 1; while ((int)(tb.size()/keep) > 80000) keep++;   // safety cap for the previewed/stored tri count
        for (size_t i=0;i<tb.size(); i+=keep){ const TB& t=tb[i];
            uint32_t b=(uint32_t)(si.navVerts.size()/3);
            float v[9]={t.ax,t.ay,t.az, t.bx,t.by,t.bz, t.cx,t.cy,t.cz}; for(float f:v) si.navVerts.push_back(f);
            si.navIdx.push_back(b); si.navIdx.push_back(b+1); si.navIdx.push_back(b+2);
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
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; showType[sitem::NAVMESH]=true; tab=TAB_OBJECT;
    }
    // "Make this object a mesh collider": a ColliderMesh built from ONE mesh's exact triangles (a solid obstacle you
    // can't walk through — same haven component as a navmesh, just sourced from a single object). Right-click -> Add.
    void addMeshCollider(int m){
        if (m<0 || m>=(int)r->gpuMeshes.size()) return;
        auto& gm=r->gpuMeshes[m];
        if (gm.dynamicVerts) {   // ANIMATED mesh -> a same-entity KINEMATIC collider that follows the animation (toggle)
            auto it=std::find(animColliders.begin(),animColliders.end(),m);
            if (it==animColliders.end()){ animColliders.push_back(m); setStatus("Animated collider ON (follows anim): "+gm.name); }
            else { animColliders.erase(it); setStatus("Animated collider OFF: "+gm.name); }
            return;
        }
        sitem::Item it; it.type=sitem::NAVMESH; it.navMode=2; it.srcMeshes={m};   // STATIC mesh -> separate ColliderMesh entity
        it.name="Collider ("+gm.name+")";
        bakeNavGeometry(it);
        deselectAll(); items.push_back(std::move(it)); selItem=(int)items.size()-1; showType[sitem::NAVMESH]=true; tab=TAB_OBJECT;
    }
    bool isAnimCollider(int m) const { return std::find(animColliders.begin(),animColliders.end(),m)!=animColliders.end(); }

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
        float feetY=r->cam.pos[1]-1.6f, gy;
        if(groundAt(r->cam.pos[0],r->cam.pos[2],feetY,gy)){
            float target=gy+1.6f;
            if(r->cam.pos[1]<target) r->cam.pos[1]=target;                              // can't sink below the floor (step up)
            else r->cam.pos[1]+=(target-r->cam.pos[1])*std::min(1.f,dt*10.f);           // ease down off ledges
            pVelY=0;
        } else { pVelY-=12.f*dt; r->cam.pos[1]+=pVelY*dt; }                              // no floor below -> fall
        if((hasRespawn && r->cam.pos[1]<respawnY) || r->cam.pos[1]<-2000.f) spawnPlayer();   // fell into the void -> respawn
    }
    // Draw a navmesh's baked triangles as a wireframe overlay (the "way to see it"). Capped so huge meshes stay cheap.
    void drawNavWire(const sitem::Item& it, uint32_t col){
        const auto& V=it.navVerts; const auto& I=it.navIdx;
        if (V.size()<9 || I.size()<3){ for (int m:it.srcMeshes) if (m>=0&&m<(int)r->gpuMeshes.size()) drawAabbBox(r->gpuMeshes[m]); return; }
        float M[16]; itemTRS(it,M);   // apply the item's T·R·S (the gizmo edits this) so moving the gizmo moves the navmesh
        size_t ntri=I.size()/3, maxTri=12000, stride = ntri>maxTri ? ntri/maxTri : 1;
        uint32_t fillCol=ui::withA(col,60), edgeCol=ui::withA(col,180);   // FILLED translucent green surface + edges (haven2025 look)
        for (size_t t=0;t<ntri;t+=stride){
            uint32_t a=I[t*3],b=I[t*3+1],d=I[t*3+2];
            if ((size_t)a*3+2>=V.size()||(size_t)b*3+2>=V.size()||(size_t)d*3+2>=V.size()) continue;
            float wa[3],wb[3],wd[3]; xformPoint(M,&V[a*3],wa); xformPoint(M,&V[b*3],wb); xformPoint(M,&V[d*3],wd);
            float sa[2],sb[2],sd[2];
            bool oa=worldToScreen(wa,sa[0],sa[1]), ob=worldToScreen(wb,sb[0],sb[1]), od=worldToScreen(wd,sd[0],sd[1]);
            if(oa&&ob&&od) dl.triangle(sa[0],sa[1],sb[0],sb[1],sd[0],sd[1], fillCol);   // walkable surface fill
            if(oa&&ob) dl.line(sa[0],sa[1],sb[0],sb[1],edgeCol,0.7f);
            if(ob&&od) dl.line(sb[0],sb[1],sd[0],sd[1],edgeCol,0.7f);
            if(od&&oa) dl.line(sd[0],sd[1],sa[0],sa[1],edgeCol,0.7f);
        }
    }
    static void printBar(float f, const char* s){
        int W=28, n=(int)(f*W); char bar[64]; for(int i=0;i<W;i++) bar[i]=i<n?'#':' '; bar[W]=0;
        fprintf(stderr, "\r[%s] %3d%%  %-22s", bar, (int)(f*100), s); fflush(stderr);
    }
    static void setenv_(const char* k, const char* v){ std::string s=std::string(k)+"="+v; _putenv(s.c_str()); }
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
        for (int i=0;i<(int)r->gpuMeshes.size();++i) {
            if (r->isHidden(i)) continue; auto& gm=r->gpuMeshes[i];
            if (isBackdrop(gm.name)) continue;                             // the sky/backdrop isn't selectable (click-through = deselect)
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
    void doUndo(){ if (undoStack.empty()) return; UndoOp op=undoStack.back(); undoStack.pop_back(); restoreOp(op,false); redoStack.push_back(op); }
    void doRedo(){ if (redoStack.empty()) return; UndoOp op=redoStack.back(); redoStack.pop_back(); restoreOp(op,true); undoStack.push_back(op); }
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
