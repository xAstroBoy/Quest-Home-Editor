#pragma once
// In-window editor UI (Dear ImGui). Blender-style scene outliner: each object is an expandable
// tree node whose children expose the full Transform (Position vector, Rotation as quaternion AND
// euler, Scale, plus the Local/base and World matrices), Mesh info and Material info — all editable
// live. Edits are a delta applied on top of the authored placement (pivoted at the world centroid),
// so OPA placements and glTF baked meshes both move/rotate/scale correctly. Decoupled from the
// renderer via its overlayBegin/overlayDraw hooks; lives entirely in this header + main.
#include "vk_renderer.h"
#include "audio.h"
#include "camera.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "ImGuizmo.h"
#include "hsl_cooker.h"   // self-contained V203/HSL APK cooker — the "Export APK" path
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <algorithm>
#include <limits>

struct Editor {
    VkRenderer*  r = nullptr;
    AudioPlayer* audio = nullptr;
    GLFWwindow*  win = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool ready = false;

    // animation control (main's loop reads these through pointers)
    bool*  animOverride = nullptr;   // when true the loop uses *animScrub as the time
    float* animScrub    = nullptr;
    float  animDuration = 0.0f;

    // UI state
    char  search[96] = "";
    int   selected   = -1;
    std::vector<MeshData>* sceneMeshes = nullptr;   // CPU geometry/textures (parallel to r->gpuMeshes) for Export APK
    std::function<std::vector<float>(int meshIdx, int frames, int& nvOut)> vatBaker;  // bound to GltfLoader::bakeVAT (V79) — VAT animation
    std::function<void(int meshIdx, int frames, hslcook::ExportMesh& em)> hzAnimExtractor;  // bound to GltfLoader::extractHzAnim (V79) — HZANIM skeletal
    std::string exportStatus;                       // last Export APK result line (shown in the toolbar)
    std::vector<uint8_t> bgOgg;                      // the env's raw _BACKGROUND_LOOP.ogg -> cooked as a FMOD SoundAsset
    float audioVol   = 1.0f;
    bool  audioMute  = false;
    bool  showLocal  = false;        // outliner matrix display: false=world (model), true=local/base
    bool  expandAll  = false;        // HSR_EDITOR_EXPAND: auto-open tree nodes (verification captures)
    bool  scrollToSel = false;       // after a 3D click-pick, scroll the outliner to the selected item
    ImGuizmo::OPERATION gizmoOp   = ImGuizmo::TRANSLATE;   // 1=move 2=rotate 3=scale
    ImGuizmo::MODE      gizmoMode = ImGuizmo::LOCAL;       // default to the model's LOCAL axes (X toggles to World)

    // ── Undo/redo of transform edits (Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y) ──
    struct Xform { float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; };
    struct UndoOp { int mesh; Xform before, after; };
    std::vector<UndoOp> undoStack, redoStack;
    bool   editing = false; int editMesh = -1; Xform editBefore;   // tree-widget edit in progress
    bool   gizmoWasUsing = false; Xform gizmoBefore; int gizmoMesh = -1;
    bool   openViewportMenu = false;   // a 3D right-click requested the object context menu this frame

    void init(VkRenderer* renderer, GLFWwindow* window, AudioPlayer* a,
              bool* animOver, float* animSc, float animDur) {
        r = renderer; win = window; audio = a;
        animOverride = animOver; animScrub = animSc; animDuration = animDur;
        expandAll = std::getenv("HSR_EDITOR_EXPAND") != nullptr;
        if (const char* s = std::getenv("HSR_EDITOR_SELECT")) {   // pre-select+focus a mesh (gizmo capture)
            selected = atoi(s);
            if (selected >= 0 && selected < (int)renderer->gpuMeshes.size()) {
                renderer->selectedMesh = selected;
                focusMesh(renderer->gpuMeshes[selected]);
                if (const char* sc = std::getenv("HSR_EDITOR_TESTSCALE")) {   // verify edit->render chain
                    float s = (float)atof(sc);
                    auto& gm = renderer->gpuMeshes[selected];
                    gm.editS[0]=gm.editS[1]=gm.editS[2]=s;
                    recomputeModel(gm);
                }
            }
        }

        VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 } };
        VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 128; pi.poolSizeCount = 1; pi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(r->device, &pi, nullptr, &pool) != VK_SUCCESS) return;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        // DPI-aware UI: scale widgets + font by the monitor's content scale (Windows display scaling)
        // so the editor isn't tiny on HiDPI screens. Override with HSR_UISCALE=<f>.
        { float xs=1.f, ys=1.f; glfwGetWindowContentScale(window, &xs, &ys);
          float ui = (xs > 0.1f) ? xs : 1.0f;
          if (const char* e = std::getenv("HSR_UISCALE")) { float v=(float)atof(e); if (v>0.1f) ui=v; }
          ImGui::GetStyle().ScaleAllSizes(ui);
          ImGui::GetIO().FontGlobalScale = ui; }
        ImGui_ImplGlfw_InitForVulkan(win, true);
        ImGui_ImplVulkan_LoadFunctions([](const char* n, void* ud) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr((VkInstance)ud, n);
        }, r->instance);
        ImGui_ImplVulkan_InitInfo ii{};
        ii.Instance = r->instance; ii.PhysicalDevice = r->physicalDevice; ii.Device = r->device;
        ii.QueueFamily = r->graphicsQueueFamily; ii.Queue = r->graphicsQueue;
        ii.DescriptorPool = pool; ii.RenderPass = r->renderPass;
        ii.MinImageCount = (uint32_t)r->swapchainImages.size();
        ii.ImageCount    = (uint32_t)r->swapchainImages.size();
        ii.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&ii);

        r->overlayBegin = [this]() { this->newFrame(); };
        r->overlayDraw  = [this](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); };
        ready = true;
    }

    void newFrame() {
        if (!ready) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        buildUI();
        drawGizmo();      // xyz arrows over the selected object (uses the background draw list)
        ImGui::Render();
    }

    // ── 4x4 column-major general inverse (for ray un-projection) ──
    static bool invertMat4(const float m[16], float inv[16]) {
        float a[16];
        a[0]= m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
        a[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
        a[8]= m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
        a[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
        a[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
        a[5]= m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
        a[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
        a[13]= m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
        a[2]= m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
        a[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
        a[10]= m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
        a[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
        a[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
        a[7]= m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
        a[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
        a[15]= m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
        float det = m[0]*a[0]+m[1]*a[4]+m[2]*a[8]+m[3]*a[12];
        if (std::fabs(det) < 1e-20f) return false;
        det = 1.0f/det;
        for (int i=0;i<16;++i) inv[i]=a[i]*det;
        return true;
    }

    // Cast the screen ray (mx,my in window px) and return the nearest mesh whose world AABB it hits
    // (-1 = none). Skips when the cursor is over the gizmo (so dragging the arrows doesn't reselect).
    int pickIndex(double mx, double my) {
        if (ImGuizmo::IsOver() || ImGuizmo::IsUsing() || !r || r->gpuMeshes.empty()) return -1;
        int W=0, H=0; glfwGetWindowSize(win, &W, &H);   // cursor pos is in window coords (not fb px)
        if (W<=0 || H<=0) return -1;
        float vp[16]; mat4mul(r->cam.proj, r->cam.view, vp);
        float inv[16]; if (!invertMat4(vp, inv)) return -1;
        float ndcx = 2.0f*(float)mx/(float)W - 1.0f;
        float ndcy = 2.0f*(float)my/(float)H - 1.0f;   // proj already y-flips; fb y-down matches
        float O[3], F[3];
        unproject(inv, ndcx, ndcy, 1.0f, O);   // reversed-Z: near plane z_ndc=1
        unproject(inv, ndcx, ndcy, 0.0f, F);   // far plane z_ndc=0
        float D[3]={F[0]-O[0], F[1]-O[1], F[2]-O[2]};
        float dl=std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]); if (dl<1e-6f) return -1; D[0]/=dl;D[1]/=dl;D[2]/=dl;

        int best=-1; float bestT=std::numeric_limits<float>::max();
        for (int i=0;i<(int)r->gpuMeshes.size();++i) {
            if (r->isHidden(i)) continue;        // can't pick a hidden mesh
            auto& gm=r->gpuMeshes[i];
            float mn[3], mx2[3];
            worldAabb(gm, mn, mx2);
            float taabb;
            if (!rayAabb(O, D, mn, mx2, taabb)) continue;        // broad-phase: miss the box -> skip
            if (taabb - 0.02f > bestT) continue;                 // box starts past the best hit -> skip
            // Narrow-phase: ray vs the actual TRIANGLES (transformed by gm.model). This is what fixes
            // "click the owl, select the ground" — the ground's huge AABB no longer wins over a closer
            // surface. Falls back to the AABB hit only if a mesh has no CPU geometry.
            const std::vector<float>& P = gm.pickPos;
            const std::vector<uint32_t>& I = gm.pickIdx;
            if (P.empty() || I.size() < 3) { if (taabb < bestT) { bestT=taabb; best=i; } continue; }
            for (size_t k=0; k+2 < I.size(); k+=3) {
                uint32_t a=I[k], b=I[k+1], c=I[k+2];
                if ((size_t)a*3+2>=P.size() || (size_t)b*3+2>=P.size() || (size_t)c*3+2>=P.size()) continue;
                float w0[3], w1[3], w2[3];
                xformPoint(gm.model, &P[a*3], w0);
                xformPoint(gm.model, &P[b*3], w1);
                xformPoint(gm.model, &P[c*3], w2);
                float t;
                if (rayTri(O, D, w0, w1, w2, t) && t < bestT) { bestT=t; best=i; }
            }
        }
        return best;
    }
    static void xformPoint(const float m[16], const float p[3], float o[3]) {  // column-major 4x4 * point
        o[0]=m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12];
        o[1]=m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13];
        o[2]=m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14];
    }
    // Möller–Trumbore ray/triangle (double-sided). Returns the hit distance along D (must be > 0).
    static bool rayTri(const float O[3], const float D[3], const float v0[3], const float v1[3], const float v2[3], float& t) {
        float e1[3]={v1[0]-v0[0],v1[1]-v0[1],v1[2]-v0[2]};
        float e2[3]={v2[0]-v0[0],v2[1]-v0[1],v2[2]-v0[2]};
        float p[3]={D[1]*e2[2]-D[2]*e2[1], D[2]*e2[0]-D[0]*e2[2], D[0]*e2[1]-D[1]*e2[0]};
        float det=e1[0]*p[0]+e1[1]*p[1]+e1[2]*p[2];
        if (std::fabs(det)<1e-12f) return false;
        float inv=1.0f/det;
        float tv[3]={O[0]-v0[0],O[1]-v0[1],O[2]-v0[2]};
        float u=(tv[0]*p[0]+tv[1]*p[1]+tv[2]*p[2])*inv;
        if (u<0.0f||u>1.0f) return false;
        float q[3]={tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0]};
        float v=(D[0]*q[0]+D[1]*q[1]+D[2]*q[2])*inv;
        if (v<0.0f||u+v>1.0f) return false;
        float tt=(e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2])*inv;
        if (tt<=1e-4f) return false;       // behind / at the camera
        t=tt; return true;
    }
    // Left-click in the 3D view -> select the mesh under the cursor.
    void pick(double mx, double my) {
        int best = pickIndex(mx, my);
        if (best>=0) {
            selected=best; r->selectedMesh=best;
            scrollToSel = true;       // make the outliner reveal the clicked item
            search[0] = 0;            // clear any filter so the picked item is visible in the tree
            fprintf(stderr, "[PICK] selected [%d] '%s'\n", best, r->gpuMeshes[best].name.c_str());
        }
    }
    // Right-click in the 3D view -> select the mesh under the cursor AND open its context menu.
    void pickForMenu(double mx, double my) {
        int best = pickIndex(mx, my);
        if (best>=0) {
            selected=best; r->selectedMesh=best; scrollToSel=true; search[0]=0;
            openViewportMenu = true;   // buildUI() opens the popup at the cursor this frame
        }
    }
    static void unproject(const float inv[16], float ndcx, float ndcy, float ndcz, float out[3]) {
        float x=inv[0]*ndcx+inv[4]*ndcy+inv[8]*ndcz+inv[12];
        float y=inv[1]*ndcx+inv[5]*ndcy+inv[9]*ndcz+inv[13];
        float z=inv[2]*ndcx+inv[6]*ndcy+inv[10]*ndcz+inv[14];
        float w=inv[3]*ndcx+inv[7]*ndcy+inv[11]*ndcz+inv[15];
        if (std::fabs(w)<1e-12f) w=1; out[0]=x/w; out[1]=y/w; out[2]=z/w;
    }
    // AABB of a mesh in world space (the baked bbox, transformed by its edit model if non-identity).
    static void worldAabb(const VkGpuMesh& gm, float mn[3], float mx[3]) {
        bool ident = gm.model[0]==1&&gm.model[5]==1&&gm.model[10]==1&&gm.model[15]==1&&
                     gm.model[12]==0&&gm.model[13]==0&&gm.model[14]==0&&
                     gm.model[1]==0&&gm.model[2]==0&&gm.model[4]==0&&gm.model[6]==0&&gm.model[8]==0&&gm.model[9]==0;
        if (ident) { for(int k=0;k<3;++k){mn[k]=gm.bbMin[k];mx[k]=gm.bbMax[k];} return; }
        mn[0]=mn[1]=mn[2]= std::numeric_limits<float>::max();
        mx[0]=mx[1]=mx[2]=-std::numeric_limits<float>::max();
        for (int c=0;c<8;++c) {
            float px=(c&1)?gm.bbMax[0]:gm.bbMin[0];
            float py=(c&2)?gm.bbMax[1]:gm.bbMin[1];
            float pz=(c&4)?gm.bbMax[2]:gm.bbMin[2];
            float wx=gm.model[0]*px+gm.model[4]*py+gm.model[8]*pz+gm.model[12];
            float wy=gm.model[1]*px+gm.model[5]*py+gm.model[9]*pz+gm.model[13];
            float wz=gm.model[2]*px+gm.model[6]*py+gm.model[10]*pz+gm.model[14];
            mn[0]=std::min(mn[0],wx); mn[1]=std::min(mn[1],wy); mn[2]=std::min(mn[2],wz);
            mx[0]=std::max(mx[0],wx); mx[1]=std::max(mx[1],wy); mx[2]=std::max(mx[2],wz);
        }
    }
    static bool rayAabb(const float O[3], const float D[3], const float mn[3], const float mx[3], float& tHit) {
        float tmin=-std::numeric_limits<float>::max(), tmax=std::numeric_limits<float>::max();
        for (int a=0;a<3;++a) {
            if (std::fabs(D[a])<1e-9f) { if (O[a]<mn[a]||O[a]>mx[a]) return false; continue; }
            float inv=1.0f/D[a];
            float t1=(mn[a]-O[a])*inv, t2=(mx[a]-O[a])*inv;
            if (t1>t2) std::swap(t1,t2);
            tmin=std::max(tmin,t1); tmax=std::min(tmax,t2);
            if (tmin>tmax) return false;
        }
        if (tmax<0) return false;
        tHit = (tmin>0)?tmin:tmax;
        return true;
    }

    // ImGuizmo move/rotate/scale handles over the selected object.
    void drawGizmo() {
        if (selected<0 || selected>=(int)r->gpuMeshes.size()) return;
        auto& gm = r->gpuMeshes[selected];
        ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
        float view[16]; memcpy(view, r->cam.view, 64);
        float proj[16]; memcpy(proj, r->cam.proj, 64);
        proj[5] = -proj[5];   // un-flip Y: ImGuizmo expects GL y-up NDC; our proj is Vulkan y-down
        float T[3]={ gm.centroid[0]+gm.editT[0], gm.centroid[1]+gm.editT[1], gm.centroid[2]+gm.editT[2] };
        float e[3]; quatToEuler(gm.editR, e);
        float S[3]={ gm.editS[0], gm.editS[1], gm.editS[2] };
        float model[16]; ImGuizmo::RecomposeMatrixFromComponents(T, e, S, model);
        if (ImGuizmo::Manipulate(view, proj, gizmoOp, gizmoMode, model)) {
            float nT[3], nE[3], nS[3];
            ImGuizmo::DecomposeMatrixToComponents(model, nT, nE, nS);
            gm.editT[0]=nT[0]-gm.centroid[0]; gm.editT[1]=nT[1]-gm.centroid[1]; gm.editT[2]=nT[2]-gm.centroid[2];
            eulerToQuat(nE, gm.editR);
            gm.editS[0]=nS[0]; gm.editS[1]=nS[1]; gm.editS[2]=nS[2];
            recomputeModel(gm);
        }
        // Record ONE undo op per drag: capture pre-drag state while idle, push on release.
        bool using_ = ImGuizmo::IsUsing();
        if (gizmoWasUsing && !using_) pushUndo(gizmoMesh, gizmoBefore, captureX(gm));
        if (!using_) { gizmoBefore = captureX(gm); gizmoMesh = selected; }
        gizmoWasUsing = using_;
    }

    // ── small math helpers (column-major 4x4, matching VkRenderer::buildModelMatrix) ──
    static void buildTRS(const float t[3], const float q[4], const float s[3], float o[16]) {
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z), r02=2*(x*z+w*y);
        float r10=2*(x*y+w*z), r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
        float r20=2*(x*z-w*y), r21=2*(y*z+w*x), r22=1-2*(x*x+y*y);
        o[0]=r00*s[0]; o[4]=r01*s[1]; o[8] =r02*s[2]; o[12]=t[0];
        o[1]=r10*s[0]; o[5]=r11*s[1]; o[9] =r12*s[2]; o[13]=t[1];
        o[2]=r20*s[0]; o[6]=r21*s[1]; o[10]=r22*s[2]; o[14]=t[2];
        o[3]=0;        o[7]=0;        o[11]=0;        o[15]=1;
    }
    static void mat4mul(const float a[16], const float b[16], float o[16]) {
        for (int c=0;c<4;++c) for (int row=0;row<4;++row) {
            float s=0; for (int k=0;k<4;++k) s += a[k*4+row]*b[c*4+k]; o[c*4+row]=s; }
    }
    static void normalizeQuat(float q[4]) {
        float l=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
        if (l<1e-8f){ q[0]=q[1]=q[2]=0; q[3]=1; return; }
        for (int i=0;i<4;++i) q[i]/=l;
    }
    static void quatToEuler(const float q[4], float e[3]) {     // out: XYZ degrees
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float sinr=2*(w*x+y*z), cosr=1-2*(x*x+y*y);      e[0]=std::atan2(sinr,cosr);
        float sinp=2*(w*y-z*x);
        e[1]= (std::fabs(sinp)>=1.f) ? std::copysign(1.5707963f,sinp) : std::asin(sinp);
        float siny=2*(w*z+x*y), cosy=1-2*(y*y+z*z);      e[2]=std::atan2(siny,cosy);
        for (int i=0;i<3;++i) e[i]*=57.2957795f;
    }
    static void eulerToQuat(const float e[3], float q[4]) {     // in: XYZ degrees
        float X=e[0]*0.00872664626f, Y=e[1]*0.00872664626f, Z=e[2]*0.00872664626f; // deg*pi/180/2
        float cx=std::cos(X),sx=std::sin(X),cy=std::cos(Y),sy=std::sin(Y),cz=std::cos(Z),sz=std::sin(Z);
        q[3]=cx*cy*cz+sx*sy*sz; q[0]=sx*cy*cz-cx*sy*sz;
        q[1]=cx*sy*cz+sx*cy*sz; q[2]=cx*cy*sz-sx*sy*cz;
    }

    // Rebuild gm.model from the editor delta (T*R*S pivoted at the world centroid) applied on top of
    // the authored base matrix:  model = T(editT) * T(c) * R*S * T(-c) * baseModel.
    void recomputeModel(VkGpuMesh& gm) {
        const float* c = gm.centroid;
        float qi[4]={0,0,0,1}, s1[3]={1,1,1};
        float rs[16];   { float z[3]={0,0,0}; buildTRS(z, gm.editR, gm.editS, rs); }
        float Tpre[16]; { float tp[3]={gm.editT[0]+c[0], gm.editT[1]+c[1], gm.editT[2]+c[2]}; buildTRS(tp, qi, s1, Tpre); }
        float Tneg[16]; { float tn[3]={-c[0],-c[1],-c[2]}; buildTRS(tn, qi, s1, Tneg); }
        float tmp[16], delta[16];
        mat4mul(Tpre, rs, tmp);
        mat4mul(tmp, Tneg, delta);
        mat4mul(delta, gm.baseModel, gm.model);
    }

    // ── Undo/redo machinery ──
    static Xform captureX(const VkGpuMesh& gm) {
        Xform x; memcpy(x.t, gm.editT, 12); memcpy(x.r, gm.editR, 16); memcpy(x.s, gm.editS, 12); return x;
    }
    void applyX(VkGpuMesh& gm, const Xform& x) {
        memcpy(gm.editT, x.t, 12); memcpy(gm.editR, x.r, 16); memcpy(gm.editS, x.s, 12); recomputeModel(gm);
    }
    static bool xeq(const Xform& a, const Xform& b) {
        for (int i=0;i<3;++i) if (a.t[i]!=b.t[i] || a.s[i]!=b.s[i]) return false;
        for (int i=0;i<4;++i) if (a.r[i]!=b.r[i]) return false;
        return true;
    }
    void pushUndo(int mesh, const Xform& b, const Xform& a) {
        if (xeq(b,a)) return;                  // no-op edit -> don't record
        undoStack.push_back({mesh, b, a});
        redoStack.clear();
        if (undoStack.size() > 256) undoStack.erase(undoStack.begin());
    }
    void beginEdit(int mesh, const VkGpuMesh& gm) { if (editing) return; editing=true; editMesh=mesh; editBefore=captureX(gm); }
    void endEdit(const VkGpuMesh& gm) { if (!editing) return; pushUndo(editMesh, editBefore, captureX(gm)); editing=false; }
    void doUndo() {
        if (undoStack.empty()) return;
        UndoOp op = undoStack.back(); undoStack.pop_back();
        if (op.mesh>=0 && op.mesh<(int)r->gpuMeshes.size()) applyX(r->gpuMeshes[op.mesh], op.before);
        redoStack.push_back(op);
        selected = op.mesh; r->selectedMesh = op.mesh;
    }
    void doRedo() {
        if (redoStack.empty()) return;
        UndoOp op = redoStack.back(); redoStack.pop_back();
        if (op.mesh>=0 && op.mesh<(int)r->gpuMeshes.size()) applyX(r->gpuMeshes[op.mesh], op.after);
        undoStack.push_back(op);
        selected = op.mesh; r->selectedMesh = op.mesh;
    }

    // Point the camera at a world position (Focus / double-click).
    void focusOn(float cx, float cy, float cz) {
        Camera& c = r->cam;
        float ex = cx, ey = cy + 1.2f, ez = cz + 3.5f;
        c.pos[0] = ex; c.pos[1] = ey; c.pos[2] = ez;
        float dx = cx - ex, dy = cy - ey, dz = cz - ez;
        float L = std::sqrt(dx*dx + dy*dy + dz*dz); if (L < 1e-4f) L = 1.f;
        c.yaw   = std::atan2(dx, -dz);
        c.pitch = std::asin(dy / L);
    }
    void focusMesh(VkGpuMesh& gm) {
        focusOn(gm.centroid[0]+gm.editT[0], gm.centroid[1]+gm.editT[1], gm.centroid[2]+gm.editT[2]);
    }

    // One outliner row: expandable object node with Transform / Mesh / Material children.
    void objectNode(int i, VkGpuMesh& gm) {
        ImGui::PushID(i);
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected == i) f |= ImGuiTreeNodeFlags_Selected;
        char label[256]; snprintf(label, sizeof(label), "%s  [%d]", gm.name.c_str(), i);
        if (expandAll) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (selected == i && scrollToSel) ImGui::SetNextItemOpen(true);   // reveal a click-picked item
        bool open = ImGui::TreeNodeEx(label, f);
        if (selected == i && scrollToSel) { ImGui::SetScrollHereY(0.4f); scrollToSel = false; }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { selected = i; r->selectedMesh = i; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) focusMesh(gm);
        // Right-click context menu (right-click selects + opens). Lets you act on / copy an item fast.
        if (ImGui::BeginPopupContextItem()) {
            selected = i; r->selectedMesh = i;
            ImGui::TextUnformatted(gm.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Focus / teleport"))   focusMesh(gm);
            if (ImGui::MenuItem(r->soloMesh==i ? "Unsolo" : "Solo only")) r->soloMesh = (r->soloMesh==i)?-1:i;
            if (ImGui::MenuItem(r->isHidden(i) ? "Unhide" : "Hide"))      r->setHidden(i, !r->isHidden(i));
            if (r->hiddenCount() > 0 && ImGui::MenuItem("Unhide all")) r->unhideAll();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy name"))     ImGui::SetClipboardText(gm.name.c_str());
            if (ImGui::MenuItem("Reset transform")) {
                gm.editT[0]=gm.editT[1]=gm.editT[2]=0; gm.editR[0]=gm.editR[1]=gm.editR[2]=0; gm.editR[3]=1;
                gm.editS[0]=gm.editS[1]=gm.editS[2]=1; recomputeModel(gm);
            }
            ImGui::EndPopup();
        }

        if (open) {
            // ── Transform ─────────────────────────────────────────
            if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool ch = false;
                // Each drag widget records ONE undo op (capture on activate, push on deactivate-after-edit).
                ImGui::DragFloat3("Position", gm.editT, 0.01f, 0, 0, "%.3f");
                if (ImGui::IsItemActivated()) beginEdit(i, gm);
                if (ImGui::IsItemEdited()) ch = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) endEdit(gm);

                float e[3]; quatToEuler(gm.editR, e);
                ImGui::DragFloat3("Rotation (euler\xc2\xb0)", e, 0.5f);
                if (ImGui::IsItemActivated()) beginEdit(i, gm);
                if (ImGui::IsItemEdited()) { eulerToQuat(e, gm.editR); ch = true; }
                if (ImGui::IsItemDeactivatedAfterEdit()) endEdit(gm);

                ImGui::DragFloat4("Rotation (quat xyzw)", gm.editR, 0.01f);
                if (ImGui::IsItemActivated()) beginEdit(i, gm);
                if (ImGui::IsItemEdited()) { normalizeQuat(gm.editR); ch = true; }
                if (ImGui::IsItemDeactivatedAfterEdit()) endEdit(gm);

                ImGui::DragFloat3("Scale", gm.editS, 0.01f, 0.001f, 1000.f, "%.3f");
                if (ImGui::IsItemActivated()) beginEdit(i, gm);
                if (ImGui::IsItemEdited()) ch = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) endEdit(gm);

                if (ImGui::SmallButton("reset transform")) {
                    Xform before = captureX(gm);
                    gm.editT[0]=gm.editT[1]=gm.editT[2]=0;
                    gm.editR[0]=gm.editR[1]=gm.editR[2]=0; gm.editR[3]=1;
                    gm.editS[0]=gm.editS[1]=gm.editS[2]=1;
                    recomputeModel(gm);
                    pushUndo(i, before, captureX(gm));
                }
                if (ch) recomputeModel(gm);

                ImGui::Separator();
                ImGui::TextDisabled("authored local (faithful):");
                ImGui::TextDisabled("  T %.3f %.3f %.3f", gm.local.pos[0], gm.local.pos[1], gm.local.pos[2]);
                ImGui::TextDisabled("  R %.3f %.3f %.3f %.3f", gm.local.rot[0], gm.local.rot[1], gm.local.rot[2], gm.local.rot[3]);
                ImGui::TextDisabled("  S %.3f %.3f %.3f", gm.local.scale[0], gm.local.scale[1], gm.local.scale[2]);

                ImGui::Checkbox("show local/base matrix", &showLocal);
                const float* M = showLocal ? gm.baseModel : gm.model;
                ImGui::TextDisabled("%s matrix:", showLocal ? "local/base" : "world");
                for (int row = 0; row < 4; ++row)
                    ImGui::TextDisabled("  %7.3f %7.3f %7.3f %7.3f", M[row], M[4+row], M[8+row], M[12+row]);
                ImGui::TreePop();
            }
            // ── Mesh ──────────────────────────────────────────────
            if (ImGui::TreeNode("Mesh")) {
                ImGui::TextDisabled("indices: %u", gm.nIdx);
                ImGui::TextDisabled("centroid: %.2f %.2f %.2f", gm.centroid[0], gm.centroid[1], gm.centroid[2]);
                ImGui::TextDisabled("%s", gm.info.c_str());
                ImGui::TextDisabled("skinned: %s   dynamic: %s", gm.isSkinned?"yes":"no", gm.dynamicVerts?"yes":"no");
                ImGui::TreePop();
            }
            // ── Material ──────────────────────────────────────────
            if (ImGui::TreeNode("Material")) {
                ImGui::TextDisabled("blend: %s", gm.useBlend ? "alpha" : (gm.additive ? "additive" : "opaque"));
                ImGui::TextDisabled("alphaTest (cutout): %s", gm.alphaTest ? "yes" : "no");
                ImGui::TextDisabled("cull: %s", gm.cullBack ? "back (single-sided)" : "none (double-sided)");
                ImGui::TreePop();
            }
            if (ImGui::SmallButton("Focus")) focusMesh(gm);
            ImGui::SameLine();
            if (ImGui::SmallButton(r->soloMesh == i ? "Unsolo" : "Solo"))
                r->soloMesh = (r->soloMesh == i) ? -1 : i;
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy name")) ImGui::SetClipboardText(gm.name.c_str());
            // Full name in a selectable/copyable read-only field (so you can point out broken items).
            static char nameBuf[256];
            snprintf(nameBuf, sizeof(nameBuf), "%s", gm.name.c_str());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##fullname", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ── Export the CURRENT (edited) scene to a bootable V203/HSL APK via the self-contained cooker.
    //    Each mesh's positions are baked through its final model matrix (gm.model) into WORLD space, so the
    //    cooked entities use identity transforms and the export matches exactly what's on screen. UNSIGNED
    //    (sign with apksigner, or the built-in v2 signer once enabled). Paths overridable via env. ──────────
    void exportAPK() {
        using namespace hslcook;
        auto envOr = [](const char* k, const char* d){ const char* v = std::getenv(k); return std::string(v ? v : d); };
        std::string nuxd  = envOr("HSR_COOK_SHELL",   "Envs To check/v203 Ufficial Envs/Nuxd.apk");
        std::string shdir = envOr("HSR_COOK_SHADERS", "cooker/shaders");
        std::string out   = envOr("HSR_COOK_OUT",     "cooker/out/edited_export_unsigned.apk");
        auto rd = [](const std::string& p){ std::vector<uint8_t> b; FILE* f=fopen(p.c_str(),"rb"); if(!f) return b;
            fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n>0){ b.resize(n); size_t r=fread(b.data(),1,n,f); b.resize(r);} fclose(f); return b; };
        // myunlit.*.spv are UNUSED by exportSceneAPK (it bundles the real V203 unlit/unlitblend/skinned shaders). Don't
        // fail the GUI "repack APK" when they're absent (they only exist relative to a specific CWD).
        auto vspv = rd(shdir + "/myunlit.vert.spv"), fspv = rd(shdir + "/myunlit.frag.spv");
        if (!r || !sceneMeshes) { exportStatus = "ERROR: renderer/scene not bound"; return; }
        std::vector<ExportMesh> ems;
        size_t n = std::min(sceneMeshes->size(), r->gpuMeshes.size());
        for (size_t i = 0; i < n; ++i) {
            if (r->isHidden((int)i)) continue;
            const MeshData& md = (*sceneMeshes)[i];
            const VkGpuMesh& gm = r->gpuMeshes[i];
            size_t nv = md.positions.size() / 3;
            if (nv < 3 || md.indices.size() < 3) continue;
            ExportMesh em; em.name = md.name; em.positions.resize(nv * 3);
            for (size_t v = 0; v < nv; ++v) {
                float p[3] = { md.positions[v*3], md.positions[v*3+1], md.positions[v*3+2] }, o[3];
                xformPoint(gm.model, p, o);            // bake the edited world transform into the geometry
                em.positions[v*3] = o[0]; em.positions[v*3+1] = o[1]; em.positions[v*3+2] = o[2];
            }
            em.uvs = md.uvs; em.indices = md.indices;
            em.blend = gm.useBlend || gm.additive;     // transparent -> unlitblend.surface (fixes black-where-see-through)
            for (int k=0;k<4;k++) em.matTint[k] = md.tint[k];   // carry the mesh's own base-color tint to the cooker
            // V79 spritesheet FLIPBOOK (sonic_schoolhouse Sprite_3x3): 9 overlapping cell-quads skinned to bones the
            // armature scale-toggles. Collapse to ONE quad (UV 0..1) + flag flipbook; the getTime() shader cycles the
            // cells. Do this BEFORE VAT/HZANIM so the sprite ports as a flat animated material, NOT a skinned mesh.
            int fcols = 0, frows = 0;
            if (hslcook::detectAndCollapseFlipbook(em.positions, em.uvs, em.indices, fcols, frows)) {
                em.flipbook = true; em.flipCols = fcols; em.flipRows = frows; em.blend = true;
            } else {
                if (vatBaker) {                            // animated node -> bake a VAT (non-skeletal); 64-frame loop
                    int bnv = 0; auto off = vatBaker((int)i, 64, bnv);
                    if (!off.empty() && bnv == (int)nv) { em.vatOffsets = std::move(off); em.vatFrames = 64; }
                }
                if (hzAnimExtractor) hzAnimExtractor((int)i, 64, em);   // HZANIM: skinned+animated -> fill em's skeletal fields
            }
            if (md.hasTexture && md.texRGBA.size() >= (size_t)md.texW * md.texH * 4) { em.rgba = md.texRGBA; em.w = md.texW; em.h = md.texH; }
            ems.push_back(std::move(em));
        }
        if (ems.empty()) { exportStatus = "ERROR: no exportable meshes"; return; }
        bool ok = false;
        float camSpawn[3] = { r->cam.pos[0], r->cam.pos[1], r->cam.pos[2] };   // spawn the player where the V79 view is
        std::vector<uint8_t> sceneZip;
        auto apk = exportSceneAPK(ems, nuxd, vspv, fspv, true, &ok, camSpawn, &sceneZip, bgOgg);   // unspoofed (own package); bgOgg = the V79 background loop
        if (!ok || apk.empty()) { exportStatus = "ERROR: cook failed (shell: " + nuxd + ")"; return; }
        auto writeF = [](const std::string& p, const std::vector<uint8_t>& b){ FILE* f=fopen(p.c_str(),"wb"); if(!f) return false; fwrite(b.data(),1,b.size(),f); fclose(f); return true; };
        if (!writeF(out, apk)) { exportStatus = "ERROR: cannot write " + out; return; }
        // ALSO emit a spoof that masquerades as the one overridable official env (haven2025) — for UNROOTED users who
        // can't set environment_selected: they install this and pick haven2025 in the UI. The unspoofed `out` is for
        // rooted users (set environment_selected to its own package). Both share the SAME cooked scene.
        std::string spoofPkg = std::getenv("HSR_COOK_SPOOFPKG") ? std::getenv("HSR_COOK_SPOOFPKG") : "com.meta.shell.env.footprint.haven2025";
        std::string out2 = out; size_t dot = out2.rfind(".apk"); out2 = (dot==std::string::npos? out2 : out2.substr(0,dot)) + "_haven2025.apk";
        bool ok2 = false; size_t sp2 = 0;
        if (!sceneZip.empty()) {
            auto apk2 = spliceAPK(nuxd, sceneZip, "com.meta.environment.prod.nuxd", spoofPkg, &ok2);
            if (ok2 && !apk2.empty()) { writeF(out2, apk2); sp2 = apk2.size(); }
        }
        char b[300]; snprintf(b, sizeof b, "Cooked %zu meshes -> %s (%zuKB)%s", ems.size(), out.c_str(), apk.size()/1024,
                              sp2 ? (" + spoof " + out2 + " (" + std::to_string(sp2/1024) + "KB)").c_str() : " [spoof FAILED]");
        exportStatus = b; fprintf(stderr, "[EXPORT] %s\n", exportStatus.c_str());
    }

    void buildUI() {
        Camera& cam = r->cam;
        ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("HSR Editor")) { ImGui::End(); return; }

        // 3D-viewport right-click context menu (opened at the cursor by pickForMenu()).
        if (openViewportMenu) { ImGui::OpenPopup("ViewportCtx"); openViewportMenu = false; }
        if (ImGui::BeginPopup("ViewportCtx")) {
            if (selected >= 0 && selected < (int)r->gpuMeshes.size()) {
                auto& gm = r->gpuMeshes[selected];
                ImGui::TextUnformatted(gm.name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Focus / teleport")) focusMesh(gm);
                if (ImGui::MenuItem(r->isHidden(selected) ? "Unhide" : "Hide"))
                    r->setHidden(selected, !r->isHidden(selected));
                if (r->hiddenCount() > 0 && ImGui::MenuItem("Unhide all")) r->unhideAll();
                if (ImGui::MenuItem(r->soloMesh==selected ? "Unsolo" : "Solo only"))
                    r->soloMesh = (r->soloMesh==selected) ? -1 : selected;
                ImGui::Separator();
                if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(gm.name.c_str());
                if (ImGui::MenuItem("Reset transform")) {
                    gm.editT[0]=gm.editT[1]=gm.editT[2]=0; gm.editR[0]=gm.editR[1]=gm.editR[2]=0;
                    gm.editR[3]=1; gm.editS[0]=gm.editS[1]=gm.editS[2]=1; recomputeModel(gm);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::Text("Scene: %zu objects    FPS %.0f", r->gpuMeshes.size(), ImGui::GetIO().Framerate);
        ImGui::Text("cam (%.2f, %.2f, %.2f)  yaw %.0f  pitch %.0f",
                    cam.pos[0], cam.pos[1], cam.pos[2], cam.yaw*57.3f, cam.pitch*57.3f);
        // Fly speed: editable here + mousewheel (up=faster). Log-ish drag so you can reach far props.
        ImGui::SetNextItemWidth(150);
        ImGui::DragFloat("Fly speed (m/s)", &cam.speed, cam.speed*0.05f + 0.1f, cam.minSpeed, cam.maxSpeed, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); if (ImGui::SmallButton("Reset##spd")) cam.speed = 3.0f;
        ImGui::SameLine(); ImGui::TextDisabled("(mousewheel)");
        // Selected-item banner (click any model in the 3D view to select it; name is copyable).
        if (selected >= 0 && selected < (int)r->gpuMeshes.size()) {
            auto& sg = r->gpuMeshes[selected];
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.85f,0.2f,1.0f));
            ImGui::Text("Selected [%d]: %s", selected, sg.name.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(sg.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Go to")) focusMesh(sg);
        } else {
            ImGui::TextDisabled("Click a model in the 3D view to select it.");
        }

        // ── Keybinds: 1/2/3 = move/rotate/scale, X = world/local, Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y = undo/redo ──
        if (!ImGui::GetIO().WantTextInput) {
            ImGuiIO& kio = ImGui::GetIO();
            if (ImGui::IsKeyPressed(ImGuiKey_1)) gizmoOp = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) gizmoOp = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) gizmoOp = ImGuizmo::SCALE;
            if (ImGui::IsKeyPressed(ImGuiKey_X)) gizmoMode = (gizmoMode==ImGuizmo::WORLD)?ImGuizmo::LOCAL:ImGuizmo::WORLD;
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (kio.KeyShift) doRedo(); else doUndo(); }
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doRedo();
        }
        auto opBtn = [&](const char* l, ImGuizmo::OPERATION op){
            bool on = (gizmoOp==op);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f,0.45f,0.80f,1.f));
            if (ImGui::SmallButton(l)) gizmoOp = op;
            if (on) ImGui::PopStyleColor();
        };
        ImGui::TextUnformatted("Gizmo:"); ImGui::SameLine();
        opBtn("Move",   ImGuizmo::TRANSLATE); ImGui::SameLine();
        opBtn("Rotate", ImGuizmo::ROTATE);    ImGui::SameLine();
        opBtn("Scale",  ImGuizmo::SCALE);     ImGui::SameLine();
        if (ImGui::SmallButton(gizmoMode==ImGuizmo::WORLD?"World":"Local"))
            gizmoMode = (gizmoMode==ImGuizmo::WORLD)?ImGuizmo::LOCAL:ImGuizmo::WORLD;
        ImGui::SameLine();
        ImGui::BeginDisabled(undoStack.empty()); if (ImGui::SmallButton("Undo")) doUndo(); ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(redoStack.empty()); if (ImGui::SmallButton("Redo")) doRedo(); ImGui::EndDisabled();
        if (r->hiddenCount() > 0) {
            ImGui::SameLine();
            char ub[40]; snprintf(ub, sizeof(ub), "Unhide all (%d)", r->hiddenCount());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f,0.30f,0.15f,1.f));
            if (ImGui::SmallButton(ub)) r->unhideAll();
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(); ImGui::TextDisabled("(click model; right-click=menu; 1/2/3,X; Ctrl+Z)");

        // ── Editor overlays (collision/navigation geometry, normally not drawn) ──
        ImGui::TextUnformatted("Overlays:"); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f,1.0f,0.45f,1.f));
        ImGui::Checkbox("Navmesh", &r->showNavmesh); ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.4f,0.35f,1.f));
        ImGui::Checkbox("Collision/Walls", &r->showCollision); ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f,0.9f,1.0f,1.f));
        ImGui::Checkbox("Spawns", &r->showSpawn); ImGui::PopStyleColor();

        // ── Cook / Export: bake the current edited scene into a bootable V203/HSL APK ──
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.55f,0.30f,1.f));
        if (ImGui::SmallButton("Export APK")) exportAPK();
        ImGui::PopStyleColor();
        if (!exportStatus.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(exportStatus.c_str()); }

        // ── Animation ──
        if (ImGui::CollapsingHeader("Animation") && animOverride && animScrub) {
            ImGui::Checkbox("Pause / scrub", animOverride);
            ImGui::BeginDisabled(!*animOverride);
            ImGui::SliderFloat("time (s)", animScrub, 0.0f, animDuration > 0 ? animDuration : 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::TextDisabled("duration %.2fs", animDuration);
        }
        // ── Audio ──
        if (ImGui::CollapsingHeader("Audio") && audio && audio->ok) {
            if (ImGui::Checkbox("Mute", &audioMute)) ma_device_set_master_volume(&audio->device, audioMute ? 0.f : audioVol);
            ImGui::BeginDisabled(audioMute);
            if (ImGui::SliderFloat("Volume", &audioVol, 0.0f, 1.0f)) ma_device_set_master_volume(&audio->device, audioVol);
            ImGui::EndDisabled();
            ImGui::TextDisabled("%u Hz  %u ch", audio->sampleRate, audio->channels);
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint("##search", "search name...", search, sizeof(search));
        ImGui::SameLine(); if (ImGui::Button("clear")) search[0] = 0;
        ImGui::Text("Outliner");

        std::string q = search; for (auto& ch : q) ch = (char)tolower(ch);
        float footer = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("tree", ImVec2(0, -footer), true);
        int shown = 0;
        for (int i = 0; i < (int)r->gpuMeshes.size(); ++i) {
            auto& gm = r->gpuMeshes[i];
            if (!q.empty()) { std::string low = gm.name; for (auto& ch : low) ch = (char)tolower(ch);
                              if (low.find(q) == std::string::npos) continue; }
            objectNode(i, gm); ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("(no objects match \"%s\")", search);
        ImGui::EndChild();

        if (ImGui::Button("Save transforms")) saveOverlay();
        ImGui::SameLine(); ImGui::TextDisabled("-> editor_overlay.txt");

        ImGui::End();
    }

    void saveOverlay() {
        FILE* f = fopen("editor_overlay.txt", "w");
        if (!f) return;
        fprintf(f, "# HSR editor overlay — per-object transform edits (delta on the authored placement)\n");
        fprintf(f, "# idx\tname\tposX posY posZ\teulerX eulerY eulerZ(deg)\tscaleX scaleY scaleZ\n");
        int n = 0;
        for (int i = 0; i < (int)r->gpuMeshes.size(); ++i) {
            auto& gm = r->gpuMeshes[i];
            bool moved  = gm.editT[0] || gm.editT[1] || gm.editT[2];
            bool scaled = gm.editS[0] != 1.f || gm.editS[1] != 1.f || gm.editS[2] != 1.f;
            bool rotd   = gm.editR[0] || gm.editR[1] || gm.editR[2] || gm.editR[3] != 1.f;
            if (!moved && !scaled && !rotd) continue;
            float e[3]; quatToEuler(gm.editR, e);
            fprintf(f, "%d\t%s\t%.4f %.4f %.4f\t%.3f %.3f %.3f\t%.4f %.4f %.4f\n",
                    i, gm.name.c_str(), gm.editT[0], gm.editT[1], gm.editT[2],
                    e[0], e[1], e[2], gm.editS[0], gm.editS[1], gm.editS[2]);
            ++n;
        }
        fclose(f);
        fprintf(stderr, "[EDITOR] saved %d edited transform(s) -> editor_overlay.txt\n", n);
    }

    void shutdown() {
        if (!ready) return;
        vkDeviceWaitIdle(r->device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (pool) vkDestroyDescriptorPool(r->device, pool, nullptr);
        ready = false;
    }
};
