#pragma once
#include "core/types.h"
#include "core/camera.h"
#include "render/ibl.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <functional>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>

// Vulkan renderer replicating libshell's pipeline.
// Descriptor layout matches actual NUXD APK unlit.surface/shader SPIRV:
//   set=0 bind=0  globalUniforms  (UBO, 736 bytes)
//   set=0 bind=1  linearWrapSampler  (SAMPLER)
//   set=2 bind=0  matParams  (UBO, 16 bytes)
//   set=2 bind=1  baseColorTex  (SAMPLED_IMAGE)
//
// GlobalUniforms layout (confirmed from SPIRV OpMemberDecorate — ALL ColMajor):
//   +  0  clipFromWorld0        mat4 ColMajor  (P*V*M for eye 0)
//   + 64  clipFromWorld1        mat4 ColMajor  (same for mono)
//   +128  skyBoxClipFromWorld0  mat4 ColMajor
//   +192  skyBoxClipFromWorld1  mat4 ColMajor
//   +256  viewFromWorld0        mat4 ColMajor
//   +320  viewFromWorld1        mat4 ColMajor
//   +384  worldCameraPos0       vec4
//   +400  worldCameraPos1       vec4
//   +416  lightingParams / lightprobes ...
//   +592  whitePoint  f32
//   +596  time  f32
//   +600  debugVisMode  i32
//   +604  reverseZ  i32
//   +656  viewportOffset  vec2
//   +664  viewportSize  vec2
//   +672  fogColor  vec4
//   total: 736 bytes
//   Write column-major matrices directly — NO transpose needed.
//
// matParamsTag layout:
//   +0  uvScaleOffset  vec4  (default {1,1,0,0})

struct VkGpuMesh {
    std::vector<EnvComponent> components;   // all hstf components of the owning entity (generic editor inspect/edit)
    VkBuffer vbo = VK_NULL_HANDLE;
    VkDeviceMemory vboMem = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE;
    VkDeviceMemory iboMem = VK_NULL_HANDLE;
    VkImage texImage = VK_NULL_HANDLE;
    VkDeviceMemory texMem = VK_NULL_HANDLE;
    VkImageView texView = VK_NULL_HANDLE;
    VkBuffer globalUbo = VK_NULL_HANDLE;       // set0 bind0: GlobalUniforms 736B
    VkDeviceMemory globalUboMem = VK_NULL_HANDLE;
    // Scene fog (nuxd SceneSettings: distanceFog=[0,19], heightFog=[0,1500], fogColor=[0,0,0,0]).
    float fogR=0.f, fogG=0.f, fogB=0.f, fogA=0.f, fogStart=0.f, fogDensity=19.f, heightFogStart=0.f, heightFogEnd=1500.f;
    VkBuffer matUbo = VK_NULL_HANDLE;          // set2 bind0: matParamsTag 16B
    VkDeviceMemory matUboMem = VK_NULL_HANDLE;
    // Extra per-material UBOs for the PBR set2 (lightprobesParams SH, lightBakerParams).
    // One entry per UBO binding in set2 beyond matParams; freed in cleanup.
    std::vector<VkBuffer> set2Ubos;
    std::vector<VkDeviceMemory> set2UboMems;
    VkImage normalImage = VK_NULL_HANDLE;      // ONxRNy_Tx (normal+rough+AO)
    VkDeviceMemory normalMem = VK_NULL_HANDLE;
    VkImageView normalView = VK_NULL_HANDLE;
    VkImage ormImage = VK_NULL_HANDLE;         // ORM / roughness-metallic-AO (if shader has slot)
    VkDeviceMemory ormMem = VK_NULL_HANDLE;
    VkImageView ormView = VK_NULL_HANDLE;
    VkImage emissiveImage = VK_NULL_HANDLE;     // emissiveTex (V203 isotropicemissiveusd etc.)
    VkDeviceMemory emissiveMem = VK_NULL_HANDLE;
    VkImageView emissiveView = VK_NULL_HANDLE;
    VkImage lmImage = VK_NULL_HANDLE;          // baked lightmap (if material provides one)
    VkDeviceMemory lmMem = VK_NULL_HANDLE;
    VkImageView lmView = VK_NULL_HANDLE;
    VkImage vatImage = VK_NULL_HANDLE;         // VAT offset texture (R16G16B16A16_SFLOAT: verts x frames)
    VkDeviceMemory vatMem = VK_NULL_HANDLE;
    VkImageView vatView = VK_NULL_HANDLE;      // VAT offset texture (decoded .exr vatdata); null => zero-offset rest pose
    VkDescriptorSet descSet0 = VK_NULL_HANDLE; // set 0: globalUniforms + linearWrapSampler
    VkDescriptorSet descSet2 = VK_NULL_HANDLE; // set 2: matParams + baseColorTex (unlit)
    // Skinned pipeline extras (set when isSkinned=true)
    VkBuffer skinUbo = VK_NULL_HANDLE;          // bone matrices SSBO (256 * 64 = 16384B)
    VkDeviceMemory skinUboMem = VK_NULL_HANDLE;
    void* skinMapped = nullptr;                  // persistently mapped for per-frame animation
    VkDescriptorSet descSet2Skin = VK_NULL_HANDLE; // set 2: matUbo + skinUbo + baseColorTex
    bool isSkinned = false;
    // CPU vertex animation (the skinned GPU shader produces no output on desktop, so
    // we animate the bind-pose vertex buffer directly). We keep the VBO mapped and
    // remember each vertex's base position + its bone index to offset it per-frame.
    void* vboMapped = nullptr;
    u32 vboStride = 32;
    bool dynamicVerts = false;   // glTF skeletal anim: positions streamed each frame
    u32 dynVertCount = 0;
    u32 posOffset = 0;           // byte offset of the position attribute within a vertex
    u32 uvOffset = 0;            // byte offset of the UV attribute (for streamed UV/flipbook anim)
    float curTint[4] = {1.0f,1.0f,1.0f,1.0f}; // per-frame MaterialTint (UniformColor); 1,1,1,1=no effect
    float lmPow[3]   = {1.0f,1.0f,1.0f};      // lightmappower (ModmapFactor) -> applied IN-SHADER via tint (HDR, faithful)
    int  overlayKind = 0;                     // editor overlay: 0=normal, 1=navmesh, 2=collision/wall, 3=phys
    float centroid[3] = {0,0,0}; // world-space centroid — back-to-front sort of blend (transparent) meshes
    float bbMin[3] = {0,0,0};    // world-space AABB (baked positions) — used for editor ray-pick selection
    float bbMax[3] = {0,0,0};
    std::vector<float> pickPos;   // CPU x,y,z per vertex (model-space snapshot) for precise ray-TRIANGLE pick
    std::vector<u32>   pickIdx;   // triangle indices for the ray-triangle pick (transform by gm.model)
    std::vector<float> basePos;   // x,y,z per vertex (authored bind pose)
    std::vector<u8> vBone;        // first bone index per vertex
    bool animated = false;
    u32 nIdx = 0;
    float model[16];
    // ── Editor transform state ──────────────────────────────────────────────────────────────
    // baseModel = the authored world matrix as built at upload (buildModelMatrix(local)). The editor
    // applies its edits as a delta ON TOP of this, pivoted at the world centroid, so model = delta*base.
    float baseModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Transform local;             // authored local TRS (pos / quat xyzw / scale) — shown in the outliner
    int progIdx = -1;            // HSR_PERMAT: index into VkRenderer::programs (this mesh's own shader); -1=global
    float editT[3] = {0,0,0};    // editor delta translation (world space)
    float editR[4] = {0,0,0,1};  // editor delta rotation quaternion (xyzw), pivot = centroid
    float editS[3] = {1,1,1};    // editor delta scale, pivot = centroid
    bool useBlend = false;
    bool alphaTest = false;      // cutout: opaque pass, depth-write ON, shader discards (libshell AlphaTest)
    bool cullBack = false;       // single-sided material (glTF doubleSided=false) -> back-face cull
    bool additive = false;       // Additive material (god-rays/glow) -> ADD blend instead of alpha
    bool isSkybox = false;       // SkyboxPlatformComponent dome/cube: camera-locked + far-scaled each frame (a 1-unit cube at origin otherwise = invisible)
    bool culled = false;         // per-frame: V205 frustum+distance cull result (HSR_CLIP). false unless culling ON.
    std::string name;
    std::string info;  // "texWxH blend=N"
};

class VkRenderer {
public:
    Camera cam;
    bool verbose = true;

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[VK] ");
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

    // ── Vulkan objects ──
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    u32 graphicsQueueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchainExtent = {1280, 720};

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline alphaTestPipeline = VK_NULL_HANDLE;  // opaque DS + discard frag (cutouts)
    // set0: globalUniforms+sampler, set1: empty placeholder, set2: matParams+texture
    VkDescriptorSetLayout descSetLayouts[3] = {};
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkSampler sharedSampler = VK_NULL_HANDLE;  // linearWrapSampler at set0 bind1

    // Shader-driven set2 layout. Env shaders differ: nuxd has matParams(UBO@0) +
    // baseColorTex(SAMPLED_IMAGE@1); haven2025 unlittiledlightmap has matParams(UBO@0)
    // + sampler@1 + BaseColor_Tx(SAMPLED_IMAGE@2) + LightMap_Tx(SAMPLED_IMAGE@3).
    // We introspect the chosen fragment shader's set2 bindings and bind: UBO->matParams,
    // SAMPLER->sharedSampler, the FIRST texture->the mesh base-color, any further
    // texture (lightmap etc.)->a 1x1 white fallback so multiplies don't zero out.
    // type: 0=UBO/SSBO,1=SAMPLER,2=SAMPLED_IMAGE,3=other. name = the SPIR-V OpName of
    // the variable (e.g. "matParams","lightprobesParams","lightmap","BaseColorMetallic_Tx",
    // "ONxRNy_Tx","globalIblDiffuseCubeMap") so resources can be bound BY ROLE, like
    // libshell does, instead of guessing by binding index.
    struct DescBind { u32 binding; int type; std::string name; u32 structSize = 0; bool isStorage = false; };
    std::vector<DescBind> set0Binds, set1Binds, set2Binds;  // introspected, sorted by binding
    int set2BaseColorBinding = 1;               // binding index of the base-color texture

    // Per-shader VERTEX INPUT layout, introspected from the chosen vertex shader's
    // Input variables (location + OpName). The VBO + pipeline vertex-input are built to
    // MATCH whatever the shader declares — nuxd unlit wants pos/uv/color/bone, Haven
    // isotropictiled wants pos/normal/uv/lightmapUv. A fixed global layout corrupts
    // whichever env it doesn't match (this is how libshell drives every shader).
    struct VIn { u32 location; std::string name; VkFormat fmt; u32 offset; u32 size; int role; };
    // role: 0=pos 1=normal 2=uv 3=lightmapUv/uv1 4=color 5=boneIdx 6=boneWgt 7=tangent 8=other
    std::vector<VIn> vertInputs;                 // sorted by location
    u32 vertStride = 0;
    // matParams UBO struct member (name, byte offset, value byte-size from the SPIR-V type) so the
    // matParams buffer is filled BY NAME (uvScaleOffset/Tint/GlobalTile/F0/...) instead of hardcoded
    // per-shader offsets. vsize is the TIGHT value size (scalar 4, vec3 12, vec4 16): the cooked block
    // packs values tightly, so the repack needs the REAL size — a gap heuristic mis-sizes a scalar that
    // precedes a 16-aligned vec3 (e.g. leaves' lightSSSIntensity before Tint -> washed-white leaves).
    struct MatMember { std::string name; u32 off = 0; u32 vsize = 4; };
    std::vector<MatMember> matParamsMembers;

    // ── Per-material shader programs (HSR_PERMAT) ───────────────────────────────────────────────
    // The renderer normally draws every mesh with ONE global shader. For faithful v203 we build a
    // ShaderProgram per DISTINCT shader a material references (emissive/masked/vege/...) and route each
    // mesh to its own. Each program bundles its own introspected vertex layout + set1/set2 descriptor
    // layouts + pipelines (set0 globalUniforms layout is shared). Built lazily in uploadMesh, matched
    // by the material's ".surface" name. set1 lighting is the shared synthesized one (sharedSet1).
    struct ShaderProgram {
        std::string surface;                      // "NAME.surface" identity (matches md.shaderPath)
        std::vector<u32> vert, frag;
        std::vector<VIn> vertInputs; u32 vertStride = 0;
        std::vector<DescBind> set1Binds, set2Binds;
        std::vector<MatMember> matParamsMembers;
        int set2BaseColorBinding = 1; bool hasSet1 = false;
        VkDescriptorSetLayout layout1 = VK_NULL_HANDLE, layout2 = VK_NULL_HANDLE;
        VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE, pipeBlend = VK_NULL_HANDLE, pipeCull = VK_NULL_HANDLE,
                   pipeBlendCull = VK_NULL_HANDLE, pipeAdditive = VK_NULL_HANDLE, pipeAlphaTest = VK_NULL_HANDLE,
                   pipeWire = VK_NULL_HANDLE;   // wireframe variant on THIS program's layout (F + selection highlight)
        VkDescriptorSet set1Desc = VK_NULL_HANDLE;
    };
    bool perMat = false;                          // HSR_PERMAT: route each mesh to its own shader program
    std::vector<ShaderProgram> programs;          // built lazily, one per distinct shader surface
    // All shader (vert,frag,surface) candidates loaded from the APK, to build programs on demand.
    struct LoadedShader { std::string surface; std::vector<u32> vert, frag; };
    std::vector<LoadedShader> loadedShaders;

    // 1x1 white fallback texture for unprovided sampled images (lightmap/cubemap/etc.)
    VkImage whiteImage = VK_NULL_HANDLE;
    VkDeviceMemory whiteMem = VK_NULL_HANDLE;
    VkImageView whiteView = VK_NULL_HANDLE;
    // Flat normal map fallback (0.5,0.5,1.0) -> tangent-space up, for meshes/materials
    // without a normal texture.
    VkImage flatNormalImage = VK_NULL_HANDLE;
    VkDeviceMemory flatNormalMem = VK_NULL_HANDLE;
    VkImageView flatNormalView = VK_NULL_HANDLE;
    // 1x1 BLACK (0,0,0,0): zero-offset fallback for the VAT offset texture (vatAnimTex). The VAT vertex
    // shader does worldPosition = inPos + offset, offset = sample(vatAnimTex); a black sample => offset 0
    // => the mesh renders at its rest pose. (White made offset=(1,1,1) -> huge displacement -> stretched.)
    VkImage blackImage = VK_NULL_HANDLE;
    VkDeviceMemory blackMem = VK_NULL_HANDLE;
    VkImageView blackView = VK_NULL_HANDLE;

    // ── Synthesized lighting for the real PBR shaders (set 1) ───────────────
    // Haven2025 is a "footprint" env: its IBL cubemaps / lightprobe volume / lights are
    // produced by the shell at runtime and are NOT in the APK. We synthesize a neutral
    // studio environment so the real isotropictiled.surface shaders produce a sensibly
    // lit image instead of reading undefined descriptors.
    bool hasSet1 = false;
    VkDescriptorSet sharedSet1 = VK_NULL_HANDLE;
    VkBuffer lightUbo = VK_NULL_HANDLE;       VkDeviceMemory lightUboMem = VK_NULL_HANDLE;   // lightUniforms
    VkBuffer lightItemsSsbo = VK_NULL_HANDLE; VkDeviceMemory lightItemsMem = VK_NULL_HANDLE; // sbLightItems
    VkImage iblDiffImage = VK_NULL_HANDLE;  VkDeviceMemory iblDiffMem = VK_NULL_HANDLE;  VkImageView iblDiffView = VK_NULL_HANDLE;
    VkImage iblReflImage = VK_NULL_HANDLE;  VkDeviceMemory iblReflMem = VK_NULL_HANDLE;  VkImageView iblReflView = VK_NULL_HANDLE;
    VkImage shadowImage  = VK_NULL_HANDLE;  VkDeviceMemory shadowMem  = VK_NULL_HANDLE;  VkImageView shadowView  = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    // Neutral ambient radiance used for synthesized IBL + lightprobe SH (warm white).
    // The shell normally provides this from the room; we approximate a bright interior.
    float ambientRGB[3] = {0.95f, 0.90f, 0.83f};   // synth default (used when the env has NO real lightprobes)
    bool  hasEnvAmbient = false;                    // set by main when the env's REAL ambient was parsed from its
                                                    // LightprobesPlatformComponent .lprb (field2 L00/3.5449). When true,
                                                    // ambientRGB IS that faithful value -> SKIP the synthetic boost.

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFramebuffer> framebuffers;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    VkFence inFlightFence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;

    std::vector<VkGpuMesh> gpuMeshes;
    // Editor overlay visibility (navmesh / collision-walls). Toggled from the editor panel.
    bool showNavmesh = false;     // navmesh overlay — OFF by default (it's a flat gizmo over the floor); toggle in editor
    bool showCollision = false;   // ~85 wall/collision meshes — off by default, toggle on to wall-map
    bool showSpawn = false;       // player spawn-point cone markers — OFF by default; toggle in editor
    // Scene fog — DECODED from the env's SceneSettings (basescene.scene) by the loader, NOT hardcoded.
    // density 0 = no fog (libshell's fallback when an env has no distanceFog). Set via setSceneFog().
    float sceneFogColor[4] = {0,0,0,0};
    float sceneFogStart = 0.f, sceneFogDensity = 0.f, sceneHeightFog[2] = {0.f, 0.f};
    void setSceneFog(const float col[4], float dStart, float dEnd, float hStart, float hEnd) {
        for (int i=0;i<4;i++) sceneFogColor[i]=col[i];
        sceneFogStart = dStart; sceneFogDensity = dEnd;   // distanceFog [start,end] -> UBO fogStart/fogDensity
        sceneHeightFog[0]=hStart; sceneHeightFog[1]=hEnd;
    }
    // Device-faithful FINITE clip planes from the env's space.hstf (ScenePlatformComponent farClippingPlane/
    // nearClippingPlane — horror/oceanarium far=2000, calming far=20000, near=0.1). The DEVICE projects with
    // these, so geometry beyond farClippingPlane is GPU-clipped (vanishes). HSR_CLIP toggles them ON (use the
    // env's finite far → far geometry clips, like the device) vs OFF (fit far to scene → draw everything),
    // so we can check whether a cooked home's geometry survives the device's far-clip.
    float sceneNearClip = 0.f, sceneFarClip = 0.f;
    void setSceneClip(float n, float f) { sceneNearClip = n; sceneFarClip = f; }
    std::vector<std::pair<float,size_t>> blendOrder;  // reused scratch: (distance², meshIdx) for back-to-front blend sort

    std::vector<u32> vertSpirv;
    std::vector<u32> fragSpirv;
    std::string globalShaderPath;   // path of the chosen global shader; a material's cooked matParams
                                    // constant block is only applied if its shader matches this one.
    // Extract the "NAME.surface" identity from a shader/material path (ignores dir prefix + file).
    static std::string surfaceName(const std::string& p) {
        size_t s = p.find(".surface");
        if (s == std::string::npos) return p;
        size_t b = p.rfind('/', s);
        size_t st = (b == std::string::npos) ? 0 : b + 1;
        return p.substr(st, s - st);
    }
    std::vector<u32> alphaTestFragSpirv;  // optional: discard-on-low-alpha frag (cutouts); empty => no alpha-test pipeline
    std::vector<u32> skinnedVertSpirv;
    std::vector<u32> skinnedFragSpirv;

    // Skinned pipeline objects
    VkDescriptorSetLayout descSetLayout2Skin = VK_NULL_HANDLE;
    VkPipelineLayout skinnedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline skinnedPipeline = VK_NULL_HANDLE;

    float cachedVP[16];  // view-projection matrix cached from updateUniforms
    std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
    bool timeStarted = false;

    bool debugMode = false;
    bool wireframe = false;                    // toggle with F key
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipeline blendPipeline = VK_NULL_HANDLE;  // SRC_ALPHA blend for dome/motes
    VkPipeline graphicsPipelineCull = VK_NULL_HANDLE;  // opaque, back-face cull (single-sided glTF materials)
    VkPipeline blendPipelineCull = VK_NULL_HANDLE;     // blend, back-face cull (cel-outline inverted hull)
    VkPipeline additivePipeline = VK_NULL_HANDLE;      // ADD blend (god-rays/glow: dst += src, no depth write)
    VkCullModeFlags singleSidedCull = VK_CULL_MODE_BACK_BIT;  // env HSR_CULLMODE=front overrides (testing)
    bool framebufferResized = false;           // set by GLFW resize callback
    bool astcLdrSupported = false;             // GPU native ASTC_LDR decode
    bool shaderFloat16Supported = false;       // GPU supports SPIR-V Float16 (Haven PBR shaders)
    int selectedMesh = -1;                     // Tab to cycle, -1=none
    int soloMesh = -1;                          // if >=0, draw ONLY this mesh index (debug, env HSR_SOLO)
    int hideMesh = -1;                          // if >=0, SKIP this mesh index (debug, env HSR_HIDEMESH)
    bool hideAllGeom = false;                   // editor "solo navmesh": skip ALL meshes so only the navmesh overlay shows
    float clearRGB[3] = {0.05f, 0.05f, 0.08f};  // render-pass clear colour; set bright (e.g. magenta) to expose see-through HOLES
    // Diffuse IBL irradiance cubemap (V79 SpecIbl). When loaded, *_specibl* meshes get their per-vertex
    // color = diffuseCube(worldN)·ambientIBLTint baked in (the dominant lift off the white/dark fallback).
    ibl::Cubemap iblDiffuse;
    // SpecIbl specular (roughness-prefiltered) cubemap, mip0 decoded for CPU per-vertex sampling — the
    // sharp environment reflection of no-albedo metallic/gem shells (divingHelmet, rubyGem).
    ibl::Cubemap iblSpecular;
    float ambientIBLTint[3] = {1.0f, 1.0f, 1.0f};
    // SPECULAR reflection cubemap on the GPU (RGBA16F). The V79 SpecIbl frag samples it at
    // reflect(viewDir, worldNormal) for the reflective metal/glass look. Built from the raw
    // *_specular.dds.opa bytes via setSpecularCubemap().
    VkImage iblSpecImage = VK_NULL_HANDLE; VkDeviceMemory iblSpecMem = VK_NULL_HANDLE;
    VkImageView iblSpecView = VK_NULL_HANDLE;
    std::vector<bool> hiddenMeshes;             // editor multi-hide: true => skip this mesh in draw
    bool isHidden(size_t mi) const { return mi < hiddenMeshes.size() && hiddenMeshes[mi]; }
    void setHidden(size_t mi, bool h) { if (hiddenMeshes.size() < gpuMeshes.size()) hiddenMeshes.resize(gpuMeshes.size(), false); if (mi < hiddenMeshes.size()) hiddenMeshes[mi] = h; }
    void unhideAll() { hiddenMeshes.assign(gpuMeshes.size(), false); }
    int hiddenCount() const { int n=0; for (bool b : hiddenMeshes) if (b) ++n; return n; }
    // Editor overlay hooks (set by editor.h): overlayBegin() builds the UI each frame; overlayDraw(cmd)
    // records the ImGui draw data inside the active render pass (just before it ends).
    std::function<void()> overlayBegin;
    std::function<void(VkCommandBuffer)> overlayDraw;
    // Editor viewport pane: the 3D scene is scissored+viewported into this sub-rect (the Blender 3D-view area),
    // leaving the rest of the window for the custom UI panels (painted in overlayDraw). {0,0} extent = full window.
    VkRect2D uiViewportRect{{0,0},{0,0}};
    std::string hideMat;                         // if set, SKIP meshes whose name contains this (env HSR_HIDEMAT)
    std::string soloMat;                         // if set, draw ONLY meshes whose name contains this (env HSR_SOLOMAT)
    bool useSkinnedPipeline = false;            // skinned shader outputs nothing on desktop; animate VBO on CPU via regular pipeline

    // ── Init ────────────────────────────────────────────────────
    bool init(GLFWwindow* window, const std::vector<u32>& vertSpv,
              const std::vector<u32>& fragSpv,
              const std::vector<u32>& skinnedVertSpv = {},
              const std::vector<u32>& skinnedFragSpv = {}) {
        vertSpirv = vertSpv;
        fragSpirv = fragSpv;
        skinnedVertSpirv = skinnedVertSpv;
        skinnedFragSpirv = skinnedFragSpv;

        log("Initializing Vulkan renderer...");
        log("  Vertex SPIRV: %zu bytes (%zu words)", vertSpv.size()*4, vertSpv.size());
        log("  Fragment SPIRV: %zu bytes (%zu words)", fragSpv.size()*4, fragSpv.size());
        if (!skinnedVertSpv.empty())
            log("  Skinned Vertex SPIRV: %zu words", skinnedVertSpv.size());
        if (!skinnedFragSpv.empty())
            log("  Skinned Fragment SPIRV: %zu words", skinnedFragSpv.size());

        // Introspect SPIRV for entry point and matrix layout
        log("--- SPIRV introspection ---");
        for (int pass = 0; pass < 2; ++pass) {
            auto& spv = (pass == 0) ? vertSpv : fragSpv;
            const char* tag = (pass == 0) ? "VERT" : "FRAG";
            if (spv.size() < 5) { log("  %s: too small", tag); continue; }
            // Header: magic, version, gen, bound, reserved
            log("  %s: bound=%u", tag, spv[3]);
            for (size_t i = 5; i < spv.size(); ) {
                u32 w = spv[i];
                u32 op = w & 0xFFFF;
                u32 wc = w >> 16;
                if (wc == 0) break;
                if (op == 15 && i + 3 < spv.size()) { // OpEntryPoint
                    u32 execModel = spv[i+1];
                    const char* em = "?";
                    if (execModel == 0) em = "VERTEX";
                    else if (execModel == 4) em = "FRAGMENT";
                    const char* name = (const char*)(spv.data() + i + 3);
                    log("  %s OpEntryPoint: exec=%u(%s) name='%s'", tag, execModel, em, name);
                } else if (op == 71 && i + 3 < spv.size()) { // OpDecorate
                    u32 target = spv[i+1];
                    u32 decor = spv[i+2];
                    if (decor == 4) log("  %s OpDecorate id=%u RowMajor", tag, target);
                    else if (decor == 5) log("  %s OpDecorate id=%u ColMajor", tag, target);
                    else if (decor == 7 && i + 4 < spv.size()) log("  %s OpDecorate id=%u MatrixStride=%u", tag, target, spv[i+3]);
                }
                i += wc;
            }
        }
        log("--- end SPIRV introspection ---");

        if (volkInitialize() != VK_SUCCESS) {
            log("FATAL: volkInitialize failed");
            return false;
        }

        if (!createInstance()) return false;
        volkLoadInstance(instance);
        setupDebugMessenger();

        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            log("FATAL: Failed to create window surface");
            return false;
        }

        if (!pickPhysicalDevice()) return false;
        if (!createLogicalDevice()) return false;
        volkLoadDevice(device);

        createSwapchain();
        createImageViews();
        createRenderPass();
        createDescriptorSetLayout();
        createGraphicsPipeline();
        createDepthResources();
        createFramebuffers();
        createCommandPool();
        createSyncObjects();

        // 1x1 white fallback texture for shader bindings we don't supply (lightmaps,
        // cubemaps): multiplying base color by white is a no-op. Needs commandPool.
        {
            u8 whitePixel[4] = {255,255,255,255};
            createTextureImage(whitePixel, 1, 1, whiteImage, whiteMem);
            whiteView = createImageView(whiteImage, VK_FORMAT_R8G8B8A8_SRGB);
            // Flat tangent-space normal (0.5,0.5,1.0) for meshes lacking a normal map.
            u8 flatN[4] = {128,128,255,255};
            createTextureImage(flatN, 1, 1, flatNormalImage, flatNormalMem);
            flatNormalView = createImageView(flatNormalImage, VK_FORMAT_R8G8B8A8_UNORM);
            // 1x1 black for the VAT zero-offset fallback (UNORM so sampling returns 0.0, not sRGB-decoded).
            u8 blackPixel[4] = {0,0,0,0};
            createTextureImage(blackPixel, 1, 1, blackImage, blackMem);
            blackView = createImageView(blackImage, VK_FORMAT_R8G8B8A8_UNORM);
        }

        // Synthesized lighting (set1) for the real PBR shaders. No-op for unlit shaders.
        createLightingResources();

        log("Vulkan init complete.");
        log("  Physical device: (queue family %u)", graphicsQueueFamily);
        log("  Swapchain: %ux%u, %zu images", swapchainExtent.width, swapchainExtent.height,
            swapchainImages.size());
        return true;
    }

    // ── Upload mesh ─────────────────────────────────────────────
    void uploadMesh(const MeshData& md) {
        VkGpuMesh gm;
        gm.nIdx = md.nIdx;
        // Use the faithful 4×4 parent→child world matrix when the loader composed one (preserves hierarchy
        // shear from non-uniform-scale × rotation that a TRS recompose drops). Else rebuild T·R·S locally.
        if (md.hasWorldMatrix) memcpy(gm.model, md.worldMatrix, sizeof(gm.model));
        else                   buildModelMatrix(md.transform, gm.model);
        gm.local = md.transform;                           // authored TRS (editor outliner reads this)
        memcpy(gm.baseModel, gm.model, sizeof(gm.baseModel)); // edits are a delta applied on top of base

        // HSR_PERMAT: route this mesh to its material's OWN shader program (built lazily). -1 => global.
        // Editor overlays (navmesh/collision/spawn) MUST draw with the GLOBAL flat-colour shader so their
        // curTint (green/red/cyan) shows. A per-material program would draw the proxy's own material instead
        // (the collision proxy = pink) -> the "wallDecor_COL bleeding pink" + "navmesh not visible" bugs.
        // ALSO: a genuinely-TILED architectural material (md.tiled) with a REAL albedo texture (the floor's
        // megascans parquet, walls/ceiling) must SHOW that albedo. Its rgbmasked per-material shader treats
        // the albedo as a MASK and, without the baked lightmap (still unsupported), washes it to garbage
        // purple/yellow -> route it to the GLOBAL albedo shader. The rgbmasked props with a white.png MASK
        // base (rug/sofaCushions, texW==4) genuinely need the masked path, so they keep their program.
        // Routing: an UNLIT per-material forward shader (set1=0, e.g. unlittiledlightmaprgmasked — the
        // couch/carpet/cushions) now renders faithfully (samples BaseColor_Tx as the real albedo + binds
        // matParams by name), so use it. But a LIT isotropic PBR shader (e.g. unpackedisotropictiledrgbmasked
        // — the floor/walls) multiplies by scene lighting we can't fully feed, so per-material renders it
        // pink/wrong; for those tiled-real-albedo surfaces fall back to the GLOBAL shader, which shows the
        // base albedo (parquet). Distinguish by the surface name: "unlit*" = self-lit -> per-material.
        std::string surfLower = surfaceName(md.shaderPath); for (char& c : surfLower) c = (char)tolower((unsigned char)c);
        bool unlitSurface = surfLower.find("unlit") != std::string::npos;
        bool tiledRealAlbedo = md.tiled && md.texW > 64 && md.texH > 64 && !unlitSurface;
        // MASKED materials (rgbmasked/rgmasked) render GRAY/WRONG on the global shader: their base texture
        // is a MASK and the real colors live in the cooked LayerRed/LayerBlue/Tint matParams that the global
        // path SKIPS (the shader-match gate at ~908). They bind correctly BY NAME on their OWN program
        // (proven: the candle's amber wax, the fgPockets garden/pot/pebbles — 15-24/N members from the
        // cooked block). So route masked materials to the per-material path BY DEFAULT — this is the
        // candle "not right texture", BGgroundC, and sofaCushions color fix. EXCLUDE tiledRealAlbedo (the
        // floor/wall megascans parquet, whose real albedo the masked shader would wash) + overlays.
        // HSR_NOMASKMAT reverts to the old global-shader behaviour for A/B.
        bool maskedSurface = (surfLower.find("rgbmasked") != std::string::npos || surfLower.find("rgmasked") != std::string::npos)
                             && !std::getenv("HSR_NOMASKMAT");
        bool routePerMat = perMat || (maskedSurface && !tiledRealAlbedo);
        gm.progIdx = (routePerMat && !md.overlayKind && !tiledRealAlbedo) ? programForSurface(surfaceName(md.shaderPath), &md.texSlots) : -1;
        if (std::getenv("HSR_MATDBG")) log("  ROUTE '%s' progIdx=%d tiled=%d texW=%u unlit=%d", md.name.c_str(), gm.progIdx, (int)md.tiled, md.texW, (int)unlitSurface);
        // Skinned meshes (prism_wave_a_01) MUST use the "skinned" program variant: it has the boneIdx/boneWgt
        // vertex inputs (vstride 32) + the sbSkinningMatrices buffer. programForSurface can resolve the mesh
        // to the NON-skinned variant (vstride 24, no bones) -> the skinned shader reads garbage bones and
        // collapses every vertex to the origin (prism invisible). Force the skinned program for bone meshes.
        // BUT a VAT mesh (md.hasVat) is NOT skinned — its stride-28/44 vertex stream has the VAT vertexCol,
        // not boneIdx/boneWgt. parseRendMesh's stride>=28 bone heuristic mis-flags it as skinned; forcing it
        // to the skinned program here overrode its vatlitbubble/vatunlit program -> the bubbles stayed broken.
        // Prefer the mesh's OWN cooked skinned shader (unlitskinned/unlitskinnedwindmill/isotropicskinned*).
        // The old code overrode EVERY bone mesh with one generic programForSkinned() -> the bats/owl got the
        // wrong skinned shader (e.g. an isotropicskinned* PBR one sampling an unbound normal/mask -> MAGENTA/
        // purple + misaligned verts). Only fall back to the generic skinned program if the mesh's own shader
        // isn't a skinned variant (false-positive hasBones, or unresolved).
        if (routePerMat && md.hasBones && !md.hasVat) {
            bool ownIsSkinned = gm.progIdx >= 0 && programs[gm.progIdx].surface.find("skinned") != std::string::npos;
            if (!ownIsSkinned) { int sp = programForSkinned(); if (sp >= 0) gm.progIdx = sp; }
        }

        // Build the VBO to MATCH the (per-mesh or global) shader's introspected vertex inputs: each
        // attribute is written at its role-derived offset (pos/normal/uv/lightmapUv/color/bone…).
        u32 nVerts = md.nVerts;
        const std::vector<VIn>& vins = (gm.progIdx>=0) ? programs[gm.progIdx].vertInputs : vertInputs;
        const u32 stride = (gm.progIdx>=0) ? programs[gm.progIdx].vertStride : (vertStride ? vertStride : 32);
        // per-program (or global) set2 layout + matParams members + base-color binding for this mesh
        const std::vector<MatMember>& mpmembers = (gm.progIdx>=0) ? programs[gm.progIdx].matParamsMembers : matParamsMembers;
        const std::vector<DescBind>& s2binds = (gm.progIdx>=0) ? programs[gm.progIdx].set2Binds : set2Binds;
        const int s2base = (gm.progIdx>=0) ? programs[gm.progIdx].set2BaseColorBinding : set2BaseColorBinding;
        VkDescriptorSetLayout s2layout = (gm.progIdx>=0) ? programs[gm.progIdx].layout2 : descSetLayouts[2];

        // Smooth per-vertex normals from faces (RENDMESH packed normal not decoded yet).
        std::vector<float> nrm(nVerts * 3, 0.0f);
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            u32 a = md.indices[t], b = md.indices[t+1], c = md.indices[t+2];
            if (a >= nVerts || b >= nVerts || c >= nVerts) continue;
            float ax=md.positions[a*3],ay=md.positions[a*3+1],az=md.positions[a*3+2];
            float bx=md.positions[b*3],by=md.positions[b*3+1],bz=md.positions[b*3+2];
            float cx=md.positions[c*3],cy=md.positions[c*3+1],cz=md.positions[c*3+2];
            float e1x=bx-ax,e1y=by-ay,e1z=bz-az, e2x=cx-ax,e2y=cy-ay,e2z=cz-az;
            float fnx=e1y*e2z-e1z*e2y, fny=e1z*e2x-e1x*e2z, fnz=e1x*e2y-e1y*e2x;
            for (u32 vi : {a,b,c}) { nrm[vi*3]+=fnx; nrm[vi*3+1]+=fny; nrm[vi*3+2]+=fnz; }
        }
        for (u32 i = 0; i < nVerts; ++i) {
            float* n = &nrm[i*3];
            float l = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if (l > 1e-8f) { n[0]/=l; n[1]/=l; n[2]/=l; } else { n[0]=0; n[1]=0; n[2]=1; }
        }
        // Zero-weight fix for skinned bone data (so verts aren't collapsed to origin).
        std::vector<u8> bIdx = md.boneIndices, bWgt = md.boneWeights;
        if (md.hasBones) for (u32 i=0;i<nVerts;++i) { u32 b=i*4;
            if (b+3<bWgt.size() && !(bWgt[b]|bWgt[b+1]|bWgt[b+2]|bWgt[b+3])) { bWgt[b]=255; bIdx[b]=bIdx[b+1]=bIdx[b+2]=bIdx[b+3]=0; } }
        if (md.hasBones && md.name.find("prism")!=std::string::npos && bWgt.size()>=20)
            log("  PRISMBONE v0 idx=%u,%u,%u,%u wgt=%u,%u,%u,%u sum=%u | v1 idx=%u,%u,%u,%u wgt=%u,%u,%u,%u sum=%u",
                bIdx[0],bIdx[1],bIdx[2],bIdx[3], bWgt[0],bWgt[1],bWgt[2],bWgt[3], bWgt[0]+bWgt[1]+bWgt[2]+bWgt[3],
                bIdx[4],bIdx[5],bIdx[6],bIdx[7], bWgt[4],bWgt[5],bWgt[6],bWgt[7], bWgt[4]+bWgt[5]+bWgt[6]+bWgt[7]);

        // SpecIbl meshes: bake the diffuse-IBL irradiance into the per-vertex color (role 4) so the
        // V79 shader (which multiplies texture·tint·vColor) shows them ENV-LIT instead of the white/dark
        // fallback. diffuseCube(worldN)·ambientIBLTint. Non-specibl meshes keep white (no-op).
        bool meshSpecibl = iblDiffuse.ok() && md.iblLit;
        // Tiled-real-albedo (floor/walls routed to the global shader): bake the cooked GlobalTile into the
        // UV so the texture TILES — the global shader doesn't apply GlobalTile, so it showed one ENLARGED
        // tile (the floor's GlobalTile is 8x8). Read GlobalTileX/Y from the cooked block by name.
        float gtX = 1.0f, gtY = 1.0f;
        if (tiledRealAlbedo && !md.matParamsBlob.empty()) {
            // gm.progIdx is -1 (we route to the global shader), so look up the material's OWN rgbmasked
            // program for the matParams layout — only IT declares GlobalTileX/Y.
            int rp = programForSurface(surfaceName(md.shaderPath));
            if (rp >= 0) {
                std::vector<MatMember> mem(programs[rp].matParamsMembers.begin(), programs[rp].matParamsMembers.end());
                std::sort(mem.begin(), mem.end(), [](const MatMember& a, const MatMember& b){ return a.off < b.off; });
                size_t so = 0;
                for (auto& m : mem) {
                    if (so + m.vsize > md.matParamsBlob.size()) break;
                    std::string mn = m.name; for (char& c : mn) c = (char)tolower((unsigned char)c);
                    if      (mn == "globaltilex") memcpy(&gtX, md.matParamsBlob.data()+so, 4);
                    else if (mn == "globaltiley") memcpy(&gtY, md.matParamsBlob.data()+so, 4);
                    so += m.vsize;
                }
                if (gtX <= 0.001f) gtX = 1.0f;
                if (gtY <= 0.001f) gtY = 1.0f;
            }
        }
        std::vector<u8> vbo((size_t)nVerts * stride, 0);
        for (u32 i = 0; i < nVerts; ++i) {
            u8* vp = vbo.data() + (size_t)i * stride;
            for (auto& vin : vins) {
                u8* dst = vp + vin.offset;
                switch (vin.role) {
                    case 0: { float p[3]={md.positions[i*3],md.positions[i*3+1],md.positions[i*3+2]}; memcpy(dst,p,12);} break;
                    case 1: { float nn[3]={nrm[i*3],nrm[i*3+1],nrm[i*3+2]}; memcpy(dst,nn,12);} break;
                    case 2: { float uv[2]={(i*2<md.uvs.size())?md.uvs[i*2]*gtX:0.f,(i*2+1<md.uvs.size())?md.uvs[i*2+1]*gtY:0.f}; memcpy(dst,uv,8);} break;
                    case 3: { // role 3 = uv1 (lightmap unwrap); fall back to uv0 if absent
                        const std::vector<float>& u1 = !md.uvs2.empty() ? md.uvs2 : md.uvs;
                        float uv[2]={(i*2<u1.size())?u1[i*2]:0.f,(i*2+1<u1.size())?u1[i*2+1]:0.f}; memcpy(dst,uv,8);} break;
                    case 4: {
                        // rgb = the real per-vertex COLOR (device vertexColor0) when the mesh has one, else white;
                        // a = iblLit flag. libshell multiplies base·vertexColor0 — the butterfly's wing colour and
                        // the animvege leaf bend-mask live here. Writing white discarded them (grey butterfly stayed
                        // white; leaves didn't bend). HSR_NOVCOL forces the old white behaviour.
                        u8 col[4]={255,255,255, (u8)(md.iblLit?255:0)};
                        bool haveVC = !std::getenv("HSR_NOVCOL") && (size_t)i*4+3 < md.colors.size();
                        if (haveVC) { col[0]=md.colors[i*4]; col[1]=md.colors[i*4+1]; col[2]=md.colors[i*4+2]; col[3]=md.colors[i*4+3]; }
                        auto cl=[](float x){ x = x<0?0:(x>1?1:x); return (u8)(x*255.0f+0.5f); };
                        if (md.bakeLightmapVtx && !md.lmRGBA.empty() && (size_t)i*2+1 < md.uvs2.size()) {
                            // TEXTURED ShellEnv shell: bake lightmap(uv1)·lightmappower into the vertex
                            // colour (the frag multiplies diffuse·vColor). lightmappower is the coloured
                            // neon/glow boost (concrete pink, floor warm). Bilinear sample, wrap UVs.
                            int W=md.lmW, H=md.lmH;
                            float lu=md.uvs2[i*2], lv=md.uvs2[i*2+1];
                            lu-=std::floor(lu); lv-=std::floor(lv);
                            float fx=lu*W-0.5f, fy=lv*H-0.5f;
                            int x0=(int)std::floor(fx), y0=(int)std::floor(fy);
                            float wx=fx-x0, wy=fy-y0;
                            auto px=[&](int x,int y,int c)->float{ x=x<0?0:(x>=W?W-1:x); y=y<0?0:(y>=H?H-1:y);
                                return md.lmRGBA[((size_t)y*W+x)*4+c]/255.0f; };
                            for (int c=0;c<3;++c){
                                float a=px(x0,y0,c)*(1-wx)+px(x0+1,y0,c)*wx;
                                float b=px(x0,y0+1,c)*(1-wx)+px(x0+1,y0+1,c)*wx;
                                col[c]=cl((a*(1-wy)+b*wy) * md.lightmapPower[c]);
                            }
                            col[3]=255;
                        } else if (meshSpecibl) {
                            float N0=nrm[i*3], N1=nrm[i*3+1], N2=nrm[i*3+2];
                            float irr[3]; ibl::sample(iblDiffuse, N0, N1, N2, irr);
                            if (md.iblFullSpec) {
                                // FULL split-sum IBL for no-albedo metallic/gem shells (V79 SpecIbl),
                                // per-vertex (CPU-baked, no samplerCube — faithful to V79: there is no
                                // per-pixel cube sampler in the binary). Params read verbatim from the mat:
                                //   diffuse  = (1-m)·albedo·irr·diffScale
                                //   env      = mix(specCube(N), irr, roughness)   (smooth->sharp, rough->dim irradiance)
                                //   specular = F0·env·specScale,   F0 = mix(0.04, albedo, metallic)
                                //   colour   = (diffuse + specular)·ambientIBLTint
                                // rough->irradiance collapse keeps rough metals (and any rough surface) from
                                // mirroring a bright sky to white — the goldmine-blowout failure mode.
                                float spec0[3];
                                if (iblSpecular.ok()) ibl::sample(iblSpecular, N0, N1, N2, spec0);
                                else { spec0[0]=irr[0]; spec0[1]=irr[1]; spec0[2]=irr[2]; }
                                float r = md.roughness; r = r<0?0:(r>1?1:r);
                                float m = md.metallic;  m = m<0?0:(m>1?1:m);
                                float kd = 1.0f - m;
                                for (int c=0;c<3;++c) {
                                    float a   = md.albedoFactor[c];
                                    float env = spec0[c]*(1.0f-r) + irr[c]*r;
                                    float F0  = 0.04f*(1.0f-m) + a*m;
                                    float diff = kd * a * irr[c] * md.speciblDiffScale;
                                    float spc  = F0 * env       * md.speciblSpecScale;
                                    col[c] = cl((diff + spc) * ambientIBLTint[c]);
                                }
                            } else {
                                // Diffuse-only irradiance bake (textured *_specibl ground, no-albedo
                                // non-specibl couch/merged shells): albedo lives in the 1x1 tex / real tex.
                                col[0]=cl(irr[0]*ambientIBLTint[0]); col[1]=cl(irr[1]*ambientIBLTint[1]); col[2]=cl(irr[2]*ambientIBLTint[2]);
                            }
                        } else if (!md.hasLightmap && !haveVC && !std::getenv("HSR_NOHEMI")) {
                            // Meshes with NO baked lightmap (the merge-grouped ones whose lightmap mapping is
                            // absent from the standalone APK: cieling/rugB/slats/woodPlanks/... + footprint
                            // floors) get a soft HEMISPHERIC ambient baked into vColor (shader does base·vColor)
                            // so they read as LIT, not flat — warm from above, dimmer below (approximates the
                            // lightprobe SH the device bakes). NOT a faked lightmap match — an honest ambient.
                            float ny = nrm[i*3+1];                       // -1 down .. +1 up
                            float t  = ny*0.5f+0.5f;
                            float amb = 0.55f + 0.45f*t;                 // ceiling/down 0.55 .. floor/up 1.0
                            col[0]=cl(amb); col[1]=cl(amb*0.98f); col[2]=cl(amb*0.93f);   // gently warm
                        }
                        memcpy(dst,col,4);
                    } break;
                    case 5: { u8 bi[4]={0,0,0,0}; if(i*4+3<bIdx.size()){bi[0]=bIdx[i*4];bi[1]=bIdx[i*4+1];bi[2]=bIdx[i*4+2];bi[3]=bIdx[i*4+3];} memcpy(dst,bi,4);} break;
                    case 6: { u8 bw[4]={0,0,0,0}; if(i*4+3<bWgt.size()){bw[0]=bWgt[i*4];bw[1]=bWgt[i*4+1];bw[2]=bWgt[i*4+2];bw[3]=bWgt[i*4+3];} memcpy(dst,bw,4);} break;
                    case 7: { float tg[4]={0,0,1,1}; memcpy(dst,tg,16);} break;
                    case 9: { float uv[2]={(i*2<md.uvs3.size())?md.uvs3[i*2]:0.f,(i*2+1<md.uvs3.size())?md.uvs3[i*2+1]:0.f}; memcpy(dst,uv,8);} break;  // animvege uv2
                    case 10:{ float uv[2]={(i*2<md.uvs4.size())?md.uvs4[i*2]:0.f,(i*2+1<md.uvs4.size())?md.uvs4[i*2+1]:0.f}; memcpy(dst,uv,8);} break;  // animvege uv3
                    default: break;  // unknown -> zeros
                }
            }
        }

        VkDeviceSize vbSize = vbo.size();
        createBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     gm.vbo, gm.vboMem);
        void* vdata;
        vkMapMemory(device, gm.vboMem, 0, vbSize, 0, &vdata);
        memcpy(vdata, vbo.data(), (size_t)vbSize);
        // pos offset within a vertex (for CPU animation writes) — role 0.
        u32 posOff = 0; for (auto& vin : vins) if (vin.role==0) posOff = vin.offset;
        // CPU vertex animation (nuxd motes/prism): keep VBO mapped + remember bind pose.
        if (md.hasBones && !md.boneIndices.empty() && md.dynamicVerts && !md.hasVat) {
            // v203 HzAnim: the CPU skinner (loader.animate -> sampleRendClip) streams real skinned
            // world positions into the position attribute each frame. Use the dynamicVerts path (NOT
            // the gm.animated procedural-drift placeholder) so the decoded clip actually drives verts.
            gm.vboMapped = vdata; gm.vboStride = stride; gm.dynamicVerts = true; gm.dynVertCount = nVerts;
            gm.posOffset = posOff;
            u32 uvOffB = 0; bool hasUv0B = false;
            for (auto& vin : vins) if (vin.role==2) { uvOffB = vin.offset; hasUv0B = true; break; }
            if (!hasUv0B) for (auto& vin : vins) if (vin.role==3) { uvOffB = vin.offset; break; }
            gm.uvOffset = uvOffB;
            gm.basePos.resize(nVerts * 3); gm.vBone.resize(nVerts);
            for (u32 i = 0; i < nVerts; ++i) {
                gm.basePos[i*3+0] = md.positions[i*3+0];
                gm.basePos[i*3+1] = md.positions[i*3+1];
                gm.basePos[i*3+2] = md.positions[i*3+2];
                gm.vBone[i] = (i*4 < bIdx.size()) ? bIdx[i*4] : 0;
            }
        } else if (md.hasBones && !md.boneIndices.empty() && !md.hasVat) {
            // NOT for VAT meshes: gm.animated drives a procedural CPU drift that fights the VAT vertex
            // shader's per-frame offset -> the bubbles shot off into spikes. VAT is GPU-driven; leave verts.
            gm.vboMapped = vdata; gm.vboStride = stride; gm.animated = true;
            gm.basePos.resize(nVerts * 3); gm.vBone.resize(nVerts);
            for (u32 i = 0; i < nVerts; ++i) {
                gm.basePos[i*3+0] = md.positions[i*3+0];
                gm.basePos[i*3+1] = md.positions[i*3+1];
                gm.basePos[i*3+2] = md.positions[i*3+2];
                gm.vBone[i] = (i*4 < bIdx.size()) ? bIdx[i*4] : 0;
            }
        } else if (md.dynamicVerts) {
            // glTF skeletal animation: keep mapped so the external animator streams
            // skinned positions into the position attribute each frame.
            gm.vboMapped = vdata; gm.vboStride = stride; gm.dynamicVerts = true; gm.dynVertCount = nVerts;
            gm.posOffset = posOff;
            // uv0 (role 2) is the DIFFUSE UV the mat.sanim animates — NOT uv1 (role 3, lightmap).
            // The old "role==2||role==3" took the LAST match (uv1), so the streamed UV scroll landed on
            // the lightmap UV while the diffuse stayed static => "mat anims not moving".
            u32 uvOff = 0; bool hasUv0 = false;
            for (auto& vin : vins) if (vin.role==2) { uvOff = vin.offset; hasUv0 = true; break; }
            if (!hasUv0) for (auto& vin : vins) if (vin.role==3) { uvOff = vin.offset; break; }
            gm.uvOffset = uvOff;
        } else {
            vkUnmapMemory(device, gm.vboMem);
        }

        // Index buffer
        VkDeviceSize ibSize = md.indices.size() * sizeof(u32);
        createBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     gm.ibo, gm.iboMem);
        void* idata;
        vkMapMemory(device, gm.iboMem, 0, ibSize, 0, &idata);
        memcpy(idata, md.indices.data(), (size_t)ibSize);
        vkUnmapMemory(device, gm.iboMem);

        // HSR_TEXDBG: log decoded base-color mean RGB per mesh (hunt washed-out/unbound textures).
        if (std::getenv("HSR_TEXDBG") && !md.texRGBA.empty()) {
            double sr=0,sg=0,sb=0,sa=0; size_t np=md.texRGBA.size()/4;
            for (size_t i=0;i<np;++i){ sr+=md.texRGBA[i*4];sg+=md.texRGBA[i*4+1];sb+=md.texRGBA[i*4+2];sa+=md.texRGBA[i*4+3]; }
            // Also report the IBL/ASTC state + the actual baked vColor of vertex 0 (the value the GPU
            // really shows for iblLit meshes — texRGBA is just the 1x1 multiplier there).
            u8 v0[4]={255,255,255,255};
            if (iblDiffuse.ok() && md.iblLit && !nrm.empty()) {
                auto cl=[](float x){ x=x<0?0:(x>1?1:x); return (u8)(x*255.0f+0.5f); };
                float irr[3]; ibl::sample(iblDiffuse, nrm[0],nrm[1],nrm[2], irr);
                if (md.iblFullSpec) {
                    float sp[3]; if(iblSpecular.ok()) ibl::sample(iblSpecular,nrm[0],nrm[1],nrm[2],sp); else {sp[0]=irr[0];sp[1]=irr[1];sp[2]=irr[2];}
                    float r=md.roughness<0?0:(md.roughness>1?1:md.roughness), m=md.metallic<0?0:(md.metallic>1?1:md.metallic);
                    for(int c=0;c<3;++c){float a=md.albedoFactor[c];float env=sp[c]*(1-r)+irr[c]*r;float F0=0.04f*(1-m)+a*m;
                        v0[c]=cl(((1-m)*a*irr[c]*md.speciblDiffScale + F0*env*md.speciblSpecScale)*ambientIBLTint[c]);}
                } else { v0[0]=cl(irr[0]*ambientIBLTint[0]);v0[1]=cl(irr[1]*ambientIBLTint[1]);v0[2]=cl(irr[2]*ambientIBLTint[2]); }
            }
            if (np) log("  TEXDBG [%zu] '%s' texMean=(%.0f,%.0f,%.0f) | iblLit=%d full=%d | bakeLM=%d lmTex=%zu lmPow=(%.1f,%.1f,%.1f) vColor0=(%d,%d,%d)",
                        gpuMeshes.size(), md.name.c_str(), sr/np, sg/np, sb/np,
                        (int)md.iblLit, (int)md.iblFullSpec,
                        (int)md.bakeLightmapVtx, md.lmRGBA.size()/4, md.lightmapPower[0],md.lightmapPower[1],md.lightmapPower[2],
                        v0[0],v0[1],v0[2]);
        }
        // HSR_DUMPTEX=<substr>: write the decoded base texture of the first matching mesh to a PNG
        // (to inspect what a region actually decodes to, e.g. a "white pillow" patch on the couch atlas).
        if (const char* ds = std::getenv("HSR_DUMPTEX")) {
            if (md.name.find(ds) != std::string::npos && !md.texRGBA.empty() && md.texW>1) {
                char path[256]; snprintf(path,sizeof(path),"_dumptex_base_%zu.png", gpuMeshes.size());
                writePNG(path, md.texW, md.texH, md.texRGBA.data());
                log("  DUMPTEX base -> %s (%ux%u) for '%s'", path, md.texW, md.texH, md.name.c_str());
                if (md.hasEmissive && !md.emissiveRGBA.empty()) {
                    snprintf(path,sizeof(path),"_dumptex_emis_%zu.png", gpuMeshes.size());
                    writePNG(path, md.emissiveW, md.emissiveH, md.emissiveRGBA.data());
                    log("  DUMPTEX emis -> %s (%ux%u)", path, md.emissiveW, md.emissiveH);
                }
                if (md.hasOrm && !md.ormRGBA.empty()) {
                    snprintf(path,sizeof(path),"_dumptex_orm_%zu.png", gpuMeshes.size());
                    writePNG(path, md.ormW, md.ormH, md.ormRGBA.data());
                    log("  DUMPTEX orm -> %s (%ux%u)", path, md.ormW, md.ormH);
                }
            }
            // matParams: shader members (name@offset) + the cooked constant block values
            if (md.name.find(ds) != std::string::npos) {
                log("  DUMPMAT '%s' shader matParams members(%zu):", md.name.c_str(), mpmembers.size());
                for (auto& mm : mpmembers) log("     @%u %s (%uB)", mm.off, mm.name.c_str(), mm.vsize);
                if (!md.matParamsBlob.empty()) {
                    const float* f=(const float*)md.matParamsBlob.data(); size_t nf=md.matParamsBlob.size()/4;
                    char b[512]; int p=0; for(size_t k=0;k<nf&&k<16;k++) p+=snprintf(b+p,(size_t)(sizeof(b)-p),"%.4f ",f[k]);
                    log("  DUMPMAT cooked blob(%zuB): %s", md.matParamsBlob.size(), b);
                } else log("  DUMPMAT cooked blob: EMPTY");
                if (!md.uvs.empty()) {
                    float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                    for (size_t k=0;k+1<md.uvs.size();k+=2){ float u=md.uvs[k],v=md.uvs[k+1];
                        if(u<u0)u0=u; if(u>u1)u1=u; if(v<v0)v0=v; if(v>v1)v1=v; }
                    char sb[300]; int p=0; size_t nn=md.uvs.size()/2, st=nn/10?nn/10:1;
                    for (size_t k=0;k<nn && p<260;k+=st) p+=snprintf(sb+p,(size_t)(sizeof(sb)-p),"(%.2f,%.2f) ",md.uvs[k*2],md.uvs[k*2+1]);
                    log("  DUMPUV uv0 range u[%.3f,%.3f] v[%.3f,%.3f] spread: %s", u0,u1,v0,v1, sb);
                }
                if (!md.uvs2.empty()) {
                    float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                    for (size_t k=0;k+1<md.uvs2.size();k+=2){ float u=md.uvs2[k],v=md.uvs2[k+1];
                        if(u<u0)u0=u; if(u>u1)u1=u; if(v<v0)v0=v; if(v>v1)v1=v; }
                    log("  DUMPUV uv1 range u[%.3f,%.3f] v[%.3f,%.3f] (VAT column)", u0,u1,v0,v1);
                } else log("  DUMPUV uv1 EMPTY (VAT column falls back to uv0!)");
                auto col=[&](int c){ return std::sqrt(gm.model[c]*gm.model[c]+gm.model[c+1]*gm.model[c+1]+gm.model[c+2]*gm.model[c+2]); };
                float pmn[3]={1e9f,1e9f,1e9f},pmx[3]={-1e9f,-1e9f,-1e9f};
                for(size_t k=0;k+2<md.positions.size();k+=3) for(int c=0;c<3;c++){ float p=md.positions[k+c]; if(p<pmn[c])pmn[c]=p; if(p>pmx[c])pmx[c]=p; }
                log("  MODEL scale=(%.3f,%.3f,%.3f) inPos AABB x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]",
                    col(0),col(4),col(8), pmn[0],pmx[0],pmn[1],pmx[1],pmn[2],pmx[2]);
            }
        }
        // Texture image + view — use GPU-native ASTC if supported and data available
        VkFormat texFmt = VK_FORMAT_R8G8B8A8_SRGB;
        lastTexMip = 1;
        // Prefer the DECODED RGBA path (mipmapped) for big textures that minify hard (prism_wave's
        // 2900x866 flame strip aliases into harsh wisps as a raw ASTC upload with no mips). Small ASTC
        // atlases keep the compressed fast path.
        bool wantMip = !md.texRGBA.empty() && (md.texW >= 1024 || md.texH >= 1024);
        if (astcLdrSupported && !md.astcRaw.empty() && !wantMip) {
            texFmt = astcVkFormat(md.astcBw, md.astcBh);
            createTextureImageRaw(md.astcRaw.data(), (u32)md.astcRaw.size(),
                                  md.texW, md.texH, texFmt, gm.texImage, gm.texMem);
        } else {
            createTextureImage(md.texRGBA.data(), md.texW, md.texH, gm.texImage, gm.texMem);
        }
        gm.texView = createImageView(gm.texImage, texFmt, lastTexMip);

        // Per-material role textures (linear for normal/ORM, sRGB for lightmap). Bound to
        // the shader's named texture slots below. Fallbacks: flat-normal / white.
        if (md.hasNormal && !md.normalRGBA.empty()) {
            createTextureImage(md.normalRGBA.data(), md.normalW, md.normalH, gm.normalImage, gm.normalMem, VK_FORMAT_R8G8B8A8_UNORM);
            gm.normalView = createImageView(gm.normalImage, VK_FORMAT_R8G8B8A8_UNORM);
        }
        if (md.hasOrm && !md.ormRGBA.empty()) {
            std::vector<u8> ormData = md.ormRGBA;
            if (const char* sw = std::getenv("HSR_MASKSWIZ")) {   // diagnostic channel remap: new RGBA = old[s0],old[s1],...
                auto ci=[&](char c)->int{ c=(char)tolower((unsigned char)c); return c=='r'?0:c=='g'?1:c=='b'?2:c=='a'?3:0; };
                std::string s = sw; while (s.size()<4) s+="rgba"[s.size()];
                for (size_t i=0;i+3<ormData.size();i+=4) { u8 o[4]={md.ormRGBA[i],md.ormRGBA[i+1],md.ormRGBA[i+2],md.ormRGBA[i+3]};
                    ormData[i]=o[ci(s[0])]; ormData[i+1]=o[ci(s[1])]; ormData[i+2]=o[ci(s[2])]; ormData[i+3]=o[ci(s[3])]; }
            }
            createTextureImage(ormData.data(), md.ormW, md.ormH, gm.ormImage, gm.ormMem, VK_FORMAT_R8G8B8A8_UNORM);
            gm.ormView = createImageView(gm.ormImage, VK_FORMAT_R8G8B8A8_UNORM);
        }
        if (md.hasEmissive && !md.emissiveRGBA.empty()) {
            createTextureImage(md.emissiveRGBA.data(), md.emissiveW, md.emissiveH, gm.emissiveImage, gm.emissiveMem, VK_FORMAT_R8G8B8A8_SRGB);
            gm.emissiveView = createImageView(gm.emissiveImage, VK_FORMAT_R8G8B8A8_SRGB);
        }
        if (md.hasLightmap && !md.lmRGBA.empty()) {
            createTextureImage(md.lmRGBA.data(), md.lmW, md.lmH, gm.lmImage, gm.lmMem, VK_FORMAT_R8G8B8A8_SRGB);
            gm.lmView = createImageView(gm.lmImage, VK_FORMAT_R8G8B8A8_SRGB);
        }
        if (md.hasVat && !md.vatRaw.empty()) {
            // Upload the half-float offset texture verbatim as R16G16B16A16_SFLOAT (verts x frames). The
            // vatunlit vertex shader texelFetches it: worldPosition = inPos + offset[vertexCol][frameRow].
            createTextureImageRaw(md.vatRaw.data(), (u32)md.vatRaw.size(), md.vatW, md.vatH,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, gm.vatImage, gm.vatMem);
            gm.vatView = createImageView(gm.vatImage, VK_FORMAT_R16G16B16A16_SFLOAT);
            if (std::getenv("HSR_VATDBG")) log("  VAT '%s' offsetTex %ux%u f16 bound", md.name.c_str(), md.vatW, md.vatH);
        }
        // lightmappower applied IN-SHADER (tint*lmPow), faithful HDR order (not pre-baked+clamped) — for
        // BOTH the lightmap-sampler path (concrete shells) and the lightmap-as-base fallback (helmet/gem/loft_lamp).
        gm.lmPow[0]=md.lightmapPower[0]; gm.lmPow[1]=md.lightmapPower[1]; gm.lmPow[2]=md.lightmapPower[2];

        // globalUniforms UBO (736 bytes) — set0 bind0
        createBuffer(736, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     gm.globalUbo, gm.globalUboMem);

        // matParams UBO — filled BY INTROSPECTED MEMBER NAME (not hardcoded offsets, which
        // corrupt whichever shader they don't match — e.g. isotropictiled's F0@0 set nuxd's
        // uvScaleOffset.x to 0.04 → white floor). Each member gets a sane neutral default;
        // eventually these come from the material's constantParameters. uvScaleOffset =
        // (scale.xy=1, offset.zw=0); Tint/color = white; tiles/mults = 1; F0 = 0.04.
        static constexpr u32 MAT_UBO_SIZE = 256;
        createBuffer(MAT_UBO_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     gm.matUbo, gm.matUboMem);
        {
            void* mp;
            vkMapMemory(device, gm.matUboMem, 0, MAT_UBO_SIZE, 0, &mp);
            float* fp = (float*)mp;
            for (u32 k = 0; k < MAT_UBO_SIZE/4; ++k) fp[k] = 1.0f;  // neutral default
            for (auto& mm : mpmembers) {
                const std::string& rawname = mm.name; u32 off = mm.off;
                std::string nm = rawname; for (auto& c : nm) c = (char)tolower((unsigned char)c);
                u32 fi = off / 4;
                auto S = [&](u32 idx, float v){ if (fi+idx < MAT_UBO_SIZE/4) fp[fi+idx] = v; };
                if (nm.find("uvscaleoffset") != std::string::npos)      { S(0,1); S(1,1); S(2,0); S(3,0); }
                else if (nm.find("offset") != std::string::npos)        { S(0,0); S(1,0); S(2,0); S(3,0); }
                else if (nm.find("scale") != std::string::npos || nm.find("tile") != std::string::npos) { S(0,1); S(1,1); }
                // PBR defaults (device shader-declared; renderer's blanket 1.0 was the WHITE: a dark base went
                // fully-metallic (all specular IBL = white) + additive subsurface (white). Standard defaults:
                else if (nm.find("subsurface") != std::string::npos)   { S(0,0); S(1,0); S(2,0); S(3,0); }  // no SSS (additive -> white at 1)
                else if (nm.find("metallic") != std::string::npos || nm.find("metalness") != std::string::npos) S(0,0.0f);  // non-metal (1 = all-spec IBL = white)
                else if (nm.find("flicker") != std::string::npos)      S(0,0.0f);  // no flicker by default
                else if (nm.find("tint") != std::string::npos || nm.find("color") != std::string::npos || nm.find("colour") != std::string::npos) { S(0,1); S(1,1); S(2,1); S(3,1); }
                else if (nm.find("f0") != std::string::npos || nm.find("reflectivity") != std::string::npos || nm.find("incident") != std::string::npos) S(0,0.04f);
                else if (nm.find("roughnessmin") != std::string::npos || nm.find("reflectoverride") != std::string::npos) S(0,0.0f);
                // else: leave 1.0
            }
            // FAITHFUL matParams: if this material uses the SAME shader as our global pipeline, its
            // cooked constant block is already in the exact matParams UBO layout -> copy the REAL
            // values (Tint/LayerRed/LayerBlue/Metallic/Roughness). This is what un-grays rgbmasked
            // terrain (base texture is a MASK; real colors live here). Gated on shader match so a
            // mismatched layout never corrupts matParams.
            // NOTE: the cooked block is the per-material constant VALUES (vec4 each, in shader-param
            // order) — NOT the padded matParams UBO layout. A raw memcpy is misaligned; the faithful
            // build pairs each shader matParams member with its value BY NAME (libshell
            // MaterialPlatformComponent::getConstantValue, entry={vec4 value@0, std::string name@24}).
            // Left behind the HSR_MATBLOB flag until the by-name remap is implemented.
            // Apply the REAL cooked constant block when its layout matches the shader actually drawing
            // this mesh: under HSR_PERMAT the mesh uses ITS OWN shader (progIdx>=0) so it always matches;
            // otherwise only when the material's shader == the global shader.
            bool blockMatches = (gm.progIdx>=0) ||
                (!globalShaderPath.empty() && surfaceName(md.shaderPath) == surfaceName(globalShaderPath));
            // SKIP the cooked block for the PROP PBR shaders (isotropictiled / isotropictiledemissive):
            // its GlobalTile is a TILING factor that compresses the UV, but props use a unique [0,1] UV
            // UNWRAP (not a tiled texture), so GlobalTile warps them — SculptureA's emissive atlas (pink
            // pot), the bowls, the vases, every prop. The unwrap needs the RAW uv0. KEEP the block for the
            // room's genuinely-TILED materials — rgbmasked (needs cooked Tint/Layer; floor goes white
            // without it) and lightmap — where GlobalTile is the correct repeat count.
            // ... but NOT if the material itself is the TILED variant (md.tiled, e.g. rugA's
            // pbrlightmap_tiled): those genuinely tile, so they KEEP the block (Tint + GlobalTile).
            bool propUnwrap = (gm.progIdx>=0) && !md.tiled &&
                programs[gm.progIdx].surface.find("isotropictiled") != std::string::npos &&
                programs[gm.progIdx].surface.find("rgbmasked") == std::string::npos;
            if (!std::getenv("HSR_NOMATBLOB") && !md.matParamsBlob.empty() && blockMatches) {
                // The cooked block is the per-material constant VALUES, TIGHTLY PACKED in matParams-member
                // (declaration = offset) order — scalar 4B, vec3 12B — NOT the std140-PADDED UBO layout. A
                // raw memcpy misaligns everything after the first vec3, so rgbmasked/tiled materials (the
                // rug, the room floor/walls) rendered magenta/teal instead of their real LayerRed/LayerBlue/
                // Tint colors. REPACK value-by-value into each member's reflected UBO offset; the value size
                // is inferred from the gap to the next member (>=12 => vec3, else scalar), last = remainder.
                // For UNWRAP props (propUnwrap: a unique [0,1] unwrap, not a tiled texture) KEEP every cooked
                // constant — Tint, EmissiveTint (the lamp's golden glow), Metallic, Roughness — but FORCE
                // GlobalTile=1 so the tiling factor can't warp the unwrap (SculptureA's atlas, bowls, lamps).
                // FAITHFUL by-NAME binding (libshell): each cooked `constantParameter` carries
                // {nameHash, blobOffset, byteSize}; the shader UBO member it targets is the one whose
                // MurmurHash3_x86_32("matParams."+memberName,0) == nameHash. The cooked value blob is
                // tight-packed in .surface param-declaration order, which DIFFERS from the SPIR-V std140
                // member order (the compiler slots a scalar into a vec3's padding gap, e.g. AOfloor@44
                // before Tint@48), so positional binding swapped Tint↔AOfloor → sofaCushions rendered flat
                // yellow (read Tint=(1,1,0.1)). Map each introspected member to its entry BY HASH.
                if (!md.constParams.empty() && !mpmembers.empty() && !std::getenv("HSR_NOREPACK") &&
                    !std::getenv("HSR_NOHASHBIND")) {
                    int bound = 0;
                    for (auto& m : mpmembers) {
                        u32 want = matParamNameHash(m.name);
                        for (auto& cp : md.constParams) {
                            if (cp.nameHash != want) continue;
                            if ((size_t)cp.blobOffset + cp.byteSize > md.matParamsBlob.size()) break;
                            if (m.off + cp.byteSize > (u32)MAT_UBO_SIZE) break;
                            std::string mn = m.name; for (char& c : mn) c = (char)tolower((unsigned char)c);
                            if (propUnwrap && mn.find("tile") != std::string::npos) {
                                float one = 1.0f; memcpy((u8*)fp + m.off, &one, 4);   // neutralize GlobalTile for unwraps
                            } else {
                                memcpy((u8*)fp + m.off, md.matParamsBlob.data() + cp.blobOffset, cp.byteSize);
                                // Vulkan clip-space Y is flipped vs the device, which REVERSES a per-material
                                // shader's VERTICAL UV-pan (the device SPIR-V scrolls in the device's V space).
                                // A water overlay panning DOWN on-device (overlayPanSpeed<0) therefore scrolls
                                // UP here ("waterfall_overlay SINCE WHEN THEY GO UP"). Negate the VERTICAL pan
                                // speed to restore on-device flow. Horizontal pan (…X / …U) is left alone.
                                if (cp.byteSize==4 && mn.find("panspeed")!=std::string::npos
                                    && !mn.empty() && mn.back()!='x' && mn.back()!='u'
                                    && !std::getenv("HSR_NOVPANFLIP")) {
                                    float pv; memcpy(&pv,(u8*)fp+m.off,4); pv=-pv; memcpy((u8*)fp+m.off,&pv,4);
                                }
                            }
                            ++bound; break;
                        }
                    }
                    if (std::getenv("HSR_MATDBG")) {
                        log("  MATHASHBIND '%s' bound %d/%zu members from %zu constParams",
                            md.name.c_str(), bound, mpmembers.size(), md.constParams.size());
                        char vb[400]; int vp=0;
                        for (auto& m : mpmembers) { float val; memcpy(&val,(u8*)fp+m.off,4);
                            vp+=snprintf(vb+vp,(size_t)(sizeof(vb)-vp),"%s=%.3g ",m.name.c_str(),val); if(vp>360)break; }
                        log("    MATVALS %s", vb);
                    }
                } else if (!mpmembers.empty() && !std::getenv("HSR_NOREPACK")) {
                    // Fallback (no parsed constantParameters — older schema): positional repack by offset
                    // order. Tight-packed value-by-value into each member's reflected UBO offset.
                    std::vector<MatMember> mem(mpmembers.begin(), mpmembers.end());
                    std::sort(mem.begin(), mem.end(),
                              [](const MatMember& a, const MatMember& b){ return a.off < b.off; });
                    const u8* src = md.matParamsBlob.data(); size_t srcN = md.matParamsBlob.size(), so = 0;
                    for (size_t i = 0; i < mem.size(); ++i) {
                        size_t vsz = mem[i].vsize ? mem[i].vsize : 4;    // REAL value size from the SPIR-V type
                        if (so + vsz > srcN) break;
                        if (mem[i].off + vsz <= (u32)MAT_UBO_SIZE) {
                            std::string mn = mem[i].name; for (char& c : mn) c = (char)tolower((unsigned char)c);
                            if (propUnwrap && mn.find("tile") != std::string::npos) {
                                float one = 1.0f; memcpy((u8*)fp + mem[i].off, &one, 4);   // neutralize GlobalTile for unwraps
                            } else {
                                memcpy((u8*)fp + mem[i].off, src + so, vsz);
                            }
                        }
                        so += vsz;
                    }
                } else if (!propUnwrap) {
                    size_t n = std::min((size_t)MAT_UBO_SIZE, md.matParamsBlob.size());
                    memcpy(fp, md.matParamsBlob.data(), n);
                }
                // Per-instance MaterialPropertyOverrides ("matParams.*") applied OVER whatever the cooked block /
                // fallback wrote — the device entity override ALWAYS wins. This MUST run for every path, including
                // shaders with NO cooked constant block: the waterfall FLIPBOOK takes its animationSpeed/
                // flipbookParams/transparency/baseColorMult ENTIRELY from the entity override, so gating this behind
                // a cooked block left the flipbook frozen on a garbage frame (faint, non-flowing water). Also covers
                // plant wind, fog opacity/colour, prop roughness regardless of whether the mesh has a cooked block.
                if (!md.matOverrides.empty()) {
                    for (const auto& ov : md.matOverrides) {
                        if (ov.name.rfind("matParams.", 0) != 0) continue;
                        const std::string member = ov.name.substr(10);
                        for (auto& m : mpmembers) {
                            if (m.name != member) continue;
                            u32 nn = m.vsize > 16 ? 16u : m.vsize;
                            if (m.off + nn <= (u32)MAT_UBO_SIZE) memcpy((u8*)fp + m.off, ov.v, nn);
                            break;
                        }
                    }
                }
            }
            vkUnmapMemory(device, gm.matUboMem);
        }

        // Descriptor set 0: bind0=UBO(globalUniforms), bind1=SAMPLER(linearWrapSampler)
        VkDescriptorSetAllocateInfo ai0 = {};
        ai0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai0.descriptorPool = descPool;
        ai0.descriptorSetCount = 1;
        ai0.pSetLayouts = &descSetLayouts[0];
        vkAllocateDescriptorSets(device, &ai0, &gm.descSet0);

        VkDescriptorBufferInfo globalBufInfo = {gm.globalUbo, 0, 736};
        VkDescriptorImageInfo samplerInfo = {};
        samplerInfo.sampler = sharedSampler;

        VkWriteDescriptorSet set0Writes[2] = {};
        set0Writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        set0Writes[0].dstSet = gm.descSet0;
        set0Writes[0].dstBinding = 0;
        set0Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        set0Writes[0].descriptorCount = 1;
        set0Writes[0].pBufferInfo = &globalBufInfo;
        set0Writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        set0Writes[1].dstSet = gm.descSet0;
        set0Writes[1].dstBinding = 1;
        set0Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        set0Writes[1].descriptorCount = 1;
        set0Writes[1].pImageInfo = &samplerInfo;
        vkUpdateDescriptorSets(device, 2, set0Writes, 0, nullptr);

        // Descriptor set 2: bind0=UBO(matParams), bind1=SAMPLED_IMAGE(baseColorTex)
        VkDescriptorSetAllocateInfo ai2 = {};
        ai2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai2.descriptorPool = descPool;
        ai2.descriptorSetCount = 1;
        ai2.pSetLayouts = &s2layout;
        vkAllocateDescriptorSets(device, &ai2, &gm.descSet2);

        // Populate every introspected set2 binding BY NAME (libshell-style), so each
        // resource lands in its correct slot regardless of binding index:
        //   UBO "matParams"        -> the per-material params above
        //   UBO "lightprobesParams"-> flat ambient spherical-harmonics (L00 = ambient)
        //   UBO "lightBakerParams" -> lightmap uvScale=1 / uvOffset=0
        //   SAMPLER (aniso)        -> shared sampler
        //   IMAGE basecolor/albedo -> mesh base-color texture
        //   IMAGE normal/onxrny    -> per-material normal map (or flat-normal fallback)
        //   IMAGE lightmap / other -> 1x1 white (no-op multiply)
        // Reserve so &back() pointers stay valid across the loop.
        std::vector<VkDescriptorBufferInfo> bufInfos; bufInfos.reserve(s2binds.size());
        std::vector<VkDescriptorImageInfo>  imgInfos; imgInfos.reserve(s2binds.size());
        std::vector<VkWriteDescriptorSet>   writes;
        auto lower = [](std::string s){ for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
        for (auto& d : s2binds) {
            std::string n = lower(d.name);
            VkWriteDescriptorSet w = {};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = gm.descSet2;
            w.dstBinding = d.binding;
            w.descriptorCount = 1;
            if (d.type == 0) {
                VkBuffer buf; u32 sz = MAT_UBO_SIZE;
                if (n.find("matparam") != std::string::npos || (n.empty() && d.binding == 0)) {
                    buf = gm.matUbo;
                } else if (n.find("skinning") != std::string::npos || n.find("sbskin") != std::string::npos || n.find("bonemat") != std::string::npos) {
                    // sbSkinningMatrices: runtime array of mat4 bone matrices. At REST bones = IDENTITY
                    // (= bind pose). The generic UBO path below ZEROS it -> every skinned vertex collapses
                    // to the origin -> the mesh (prism_wave_a_01) vanishes (the "no output" we chased).
                    // Fill 256 IDENTITY mat4 so the bind-pose rest wave renders. (V203 libshell HzAnimSystem
                    // sub_ECA534 uploads quat->mat per frame to this exact buffer; identity = static rest.)
                    const u32 SKSZ = 256 * 64;
                    VkBuffer b; VkDeviceMemory m;
                    createBuffer(SKSZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b, m);
                    void* mp2; vkMapMemory(device, m, 0, SKSZ, 0, &mp2);
                    float ty = std::getenv("HSR_SKINTEST") ? 8.0f : 0.0f;   // DIAG: +8 Y translation to prove the buffer is read
                    const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,ty,0,1};
                    float* fI = (float*)mp2; for (u32 k=0;k<256;k++) memcpy(fI+k*16, I, 64);
                    vkUnmapMemory(device, m);
                    gm.set2Ubos.push_back(b); gm.set2UboMems.push_back(m);
                    buf = b; sz = SKSZ;
                } else {
                    // Per-binding UBO filled by role.
                    const u32 USZ = 256;
                    VkBuffer b; VkDeviceMemory m;
                    createBuffer(USZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b, m);
                    void* p; vkMapMemory(device, m, 0, USZ, 0, &p);
                    memset(p, 0, USZ); float* f = (float*)p;
                    if (n.find("lightprobe") != std::string::npos) {
                        // SH L00 (vec4 @0) carries the constant ambient. For a constant
                        // environment of radiance L the DC coefficient is L*sqrt(4*pi)=
                        // L*3.5449, which reconstructs to diffuse irradiance ~= albedo*L.
                        f[0]=ambientRGB[0]*3.5449f; f[1]=ambientRGB[1]*3.5449f; f[2]=ambientRGB[2]*3.5449f; f[3]=1.0f;
                    } else if (n.find("lightbaker") != std::string::npos) {
                        f[0]=1.0f; f[1]=1.0f;   // uvScale
                        f[2]=0.0f; f[3]=0.0f;   // uvOffset
                    } else if (n.find("instance") != std::string::npos &&
                               (md.atlasCellIndex >= 0.0f || md.vatTrackIndex >= 0.0f ||
                                md.vatRateFactor >= 0.0f  || md.vatTimeOffset >= 0.0f)) {
                        // Per-instance "instance" UBO = { atlasCellIndex@0, animTrackIndex@4, animRateFactor@8,
                        // animTimeOffset@12 } — reversed from the vatunlit*/unlitatlasinstance shader (set2 bind1).
                        // atlasCellIndex = which baked atlas cell (coral colour variant). The VAT trio gives each
                        // creature its OWN swim track + speed + phase; without them every turtle snapped to track 0
                        // in sync (wrong pose → the turtle clipped the deck). Was zeroed.
                        memset(f, 0, USZ);
                        f[0] = md.atlasCellIndex >= 0.0f ? md.atlasCellIndex : 0.0f;
                        if (USZ >= 16) {
                            f[1] = md.vatTrackIndex >= 0.0f ? md.vatTrackIndex : 0.0f;
                            f[2] = md.vatRateFactor >= 0.0f ? md.vatRateFactor : 1.0f;  // default rate 1 = normal speed
                            f[3] = md.vatTimeOffset >= 0.0f ? md.vatTimeOffset : 0.0f;
                        }
                    } else {
                        // Unknown per-material UBO: zero = neutral (keeps base albedo; 1.0f washed to gray).
                        memset(f, 0, USZ);
                    }
                    vkUnmapMemory(device, m);
                    gm.set2Ubos.push_back(b); gm.set2UboMems.push_back(m);
                    buf = b; sz = USZ;
                }
                bufInfos.push_back({buf, 0, sz});
                w.descriptorType = d.isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &bufInfos.back();
            } else if (d.type == 1) {
                imgInfos.push_back({sharedSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                w.pImageInfo = &imgInfos.back();
            } else {
                // Route each shader texture slot to the matching role texture, by name.
                // Order matters: "BaseColorMetallic_Tx" contains "metal", so base/normal/
                // lightmap are matched BEFORE the ORM (roughness/metallic) catch-all.
                VkImageView v;
                bool isCube = n.find("cube") != std::string::npos || n.find("specularibl") != std::string::npos;
                bool isBase = n.find("basecolor") != std::string::npos || n.find("albedo") != std::string::npos ||
                              n.find("diffuse") != std::string::npos;
                // ONxRNy_Tx / RBAoDir_Tx is the merged R(rough)/BentNormal/AO/Direction map. For the
                // rgbmasked shaders its .rg channels ARE the layer mask (the carpet/fabric/wood pattern):
                //   baseColor = mix(mix(baseTex, LayerRed, RBAoDir.r·RedLayerMult·grunge), LayerBlue, RBAoDir.g·…)
                // so it MUST reach this sampler — a flat fallback gives a constant mask -> flat beige.
                bool isNorm = n.find("onxrny") != std::string::npos || n.find("normal") != std::string::npos ||
                              n.find("_nx") != std::string::npos || n.find("rbaodir") != std::string::npos ||
                              n.find("rbao") != std::string::npos;
                bool isLm   = n.find("lightmap") != std::string::npos || n.find("lightbaker") != std::string::npos;
                bool isOrm  = n.find("orm") != std::string::npos || n.find("rough") != std::string::npos ||
                              n.find("metal") != std::string::npos || n.find("_ao") != std::string::npos ||
                              n.find("occl") != std::string::npos || n.find("rbaodir") != std::string::npos ||
                              n.find("rbao") != std::string::npos;
                bool isEmissive = n.find("emissive") != std::string::npos;
                // The merged map is loaded into EITHER the normal OR the orm slot depending on its filename
                // (a "_rbaodir" classifies as orm); bind whichever is present so the rg-mask reaches the
                // shader. Real PBR materials with SEPARATE normal+orm keep their own (primary wins).
                bool isVat = n.find("vatanim") != std::string::npos || n.find("vattex") != std::string::npos ||
                             (n.find("vat") != std::string::npos && n.find("anim") != std::string::npos);
                if (isVat)           v = (gm.vatView ? gm.vatView : blackView);  // VAT offset tex (decoded .exr) or zero-offset rest pose
                else if (isCube)     v = (iblSpecView ? iblSpecView : whiteView);   // specular reflection cube
                else if (isEmissive) v = (gm.emissiveView ? gm.emissiveView : blackView);  // emissiveTex (glow): ADDITIVE -> no-tex fallback MUST be BLACK (white added base+white=WHITE; was washing the whole scene). base decodes dark; emissive=0 = no glow.
                else if (isNorm) v = (gm.normalView ? gm.normalView : (gm.ormView ? gm.ormView : flatNormalView));
                // missing lightmap -> white (neutral "fully lit"). HSR_LMBLACK = bind black instead (the lightprobe
                // SH + IBL cube then light it) for lightprobe-lit PBR meshes that ship no baked lightmap.
                else if (isLm)   v = (gm.lmView ? gm.lmView : (std::getenv("HSR_LMBLACK") ? blackView : whiteView));
                else if (isBase || (int)d.binding == s2base) v = gm.texView;
                else if (isOrm)  v = (gm.ormView ? gm.ormView : (gm.normalView ? gm.normalView : whiteView));
                // animatedfog samples ONE noise texture TWICE (noiseTexture1 @base + noiseTexture2): its alpha
                // = combinedNoise(noise1*noise2) * edgeMask * opacity. Binding noiseTexture2 to white made
                // combinedNoise = noise*1 (too high) -> opacity 2.5 clamps it OPAQUE = the "fog too bright".
                // Bind the 2nd noise sampler to the same noise texture so noise1*noise2 (<1) stays subtle.
                else if (n.find("noise") != std::string::npos) v = (gm.texView ? gm.texView : whiteView);
                else             v = whiteView;
                imgInfos.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                w.pImageInfo = &imgInfos.back();
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(device, (u32)writes.size(), writes.data(), 0, nullptr);

        // ── Skinned pipeline extras ──────────────────────────────────────────────
        // Only when the mesh has bones AND the skinned descriptor layout is ready. NOT for VAT meshes:
        // a VAT mesh is mis-flagged hasBones by the stride>=28 heuristic, and setting isSkinned makes the
        // DRAW bind the skinned pipeline (32-byte bone vertex layout) to read the bubble's 44-byte
        // vatlitbubble VBO -> misaligned attributes -> verts read garbage -> the bubble shoots off in spikes.
        if (md.hasBones && descSetLayout2Skin != VK_NULL_HANDLE && !md.hasVat) {
            gm.isSkinned = true;

            // Bone matrices UBO: 256 bone slots * 64 bytes (mat4) = 16384 bytes.
            // Initialise bone 0 to identity; all others remain zero (safe for unused slots).
            // sbSkinningMatrices is a STORAGE buffer (BufferBlock w/ runtime array of
            // mat4 'elements') in the shader — NOT a UBO. Binding a UBO here made the
            // shader read zero matrices -> collapsed geometry -> skinned meshes invisible.
            static constexpr VkDeviceSize SKIN_UBO_SIZE = 256 * 64;
            createBuffer(SKIN_UBO_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         gm.skinUbo, gm.skinUboMem);
            {
                // Persistently map so updateSkinning() can animate bone matrices each
                // frame. Init all 256 slots to identity (a vertex weighted to a zero
                // matrix collapses to the origin).
                vkMapMemory(device, gm.skinUboMem, 0, SKIN_UBO_SIZE, 0, &gm.skinMapped);
                const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                float* slots = reinterpret_cast<float*>(gm.skinMapped);
                for (u32 b = 0; b < 256; ++b)
                    memcpy(slots + b * 16, identity, sizeof(identity));
            }

            // Descriptor set 2 for skinned pipeline:
            //   bind=0: matParams UBO (16B)
            //   bind=1: sbSkinningMatrices UBO (16384B)
            //   bind=2: baseColorTex SAMPLED_IMAGE
            VkDescriptorSetAllocateInfo aiSkin = {};
            aiSkin.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            aiSkin.descriptorPool = descPool;
            aiSkin.descriptorSetCount = 1;
            aiSkin.pSetLayouts = &descSetLayout2Skin;
            vkAllocateDescriptorSets(device, &aiSkin, &gm.descSet2Skin);

            VkDescriptorBufferInfo skinBufInfo = {gm.skinUbo, 0, SKIN_UBO_SIZE};
            VkDescriptorBufferInfo matBufInfoSkin = {gm.matUbo, 0, 256};
            VkDescriptorImageInfo texImgInfoSkin = {};
            texImgInfoSkin.imageView = gm.texView;
            texImgInfoSkin.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet skinWrites[3] = {};
            skinWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skinWrites[0].dstSet = gm.descSet2Skin;
            skinWrites[0].dstBinding = 0;
            skinWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            skinWrites[0].descriptorCount = 1;
            skinWrites[0].pBufferInfo = &matBufInfoSkin;   // same matUbo
            skinWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skinWrites[1].dstSet = gm.descSet2Skin;
            skinWrites[1].dstBinding = 1;
            skinWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            skinWrites[1].descriptorCount = 1;
            skinWrites[1].pBufferInfo = &skinBufInfo;
            skinWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            skinWrites[2].dstSet = gm.descSet2Skin;
            skinWrites[2].dstBinding = 2;
            skinWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            skinWrites[2].descriptorCount = 1;
            skinWrites[2].pImageInfo = &texImgInfoSkin;
            vkUpdateDescriptorSets(device, 3, skinWrites, 0, nullptr);

            log("  Skinned mesh — skin UBO + descSet2Skin created");
        }

        // Depth/blend routing must follow the MATERIAL (faithful to libshell): opaque & alpha-test
        // cutouts write depth (occlude correctly); only Transparent/Additive go to the no-depth-write
        // blend pass. In the OPA/V79 unlit path skinning is done on the CPU (positions baked into the
        // dynamic VBO), so a skinned banner/animal with an opaque material draws opaquely and writes
        // depth — fixing "models behind overlay the ones in front". The HSR skinned-blend pipeline
        // ("unlitblend​skinned", iridescent prism) has no CPU skinning, so there we keep force-blend.
        bool cpuSkinnedPath = !alphaTestFragSpirv.empty();  // set only for the OPA/V79 unlit path
        gm.alphaTest = md.alphaTest;
        // nuxd skinned meshes (prism_wave, motes) use "unlitblendskinned" = a BLEND material with a
        // banded-alpha holographic texture (alpha 0 gaps / 255 bands). Plain alpha-blend renders it
        // ~invisible (its mean alpha is 0.27 and the fragment opacity likely needs the cooked blend
        // state we don't yet replicate), so default OPAQUE keeps the (now non-exploding, animated) wave
        // visible. HSR_SKINBLEND opts into alpha-blend for tuning the final holographic look.
        // unlitblendskinned = a PREMULTIPLIED-alpha holographic material (banded-alpha tex). Blend it
        // (the skinned blend pipeline uses srcColor=ONE) so it composites as a soft transparent iridescent
        // ripple over the env. (Opaque forced the transparent gaps solid -> the "striped" look.)
        // OPA material says the nuxd skinned prism/motes are Transparent+ADDITIVE+DoubleSided. Additive
        // adds the glow over the env (it can't "vanish" like alpha-blend did) and DoubleSided shows both
        // faces of the thin ring.
        // Route by the mesh's ACTUAL blend, NOT by isSkinned. Forcing skinned->blend dumped OPAQUE
        // skinned characters (King Kai / snakeway) into the no-depth-write blend pass -> his front
        // didn't occlude his back, so you saw THROUGH his body. The nuxd prism/motes are genuinely
        // Transparent (md.useBlend=true), so they still blend; King Kai (md.useBlend=false) now draws
        // in the depth-WRITING opaque pass = solid. (HSR_SKINBLEND forces the old behaviour back.)
        gm.useBlend = md.useBlend || (gm.isSkinned && std::getenv("HSR_SKINBLEND"));
        (void)cpuSkinnedPath;
        if (gm.isSkinned && std::getenv("HSR_SKINOPAQUE")) gm.useBlend = false;
        gm.cullBack = !md.doubleSided && !gm.isSkinned;   // OPA prism/motes material = DoubleSided -> no cull
        gm.additive = md.additive;       // god-rays/glow -> ADD blend pipeline
        gm.isSkybox = md.isSkybox;       // skybox dome: camera-locked + far-scaled each frame (see render())
        if (gm.isSkybox) gm.cullBack = false;   // camera is INSIDE the skybox cube -> draw inward faces (no back-cull)
        gm.overlayKind = md.overlayKind; // editor navmesh/collision overlay
        if (gm.overlayKind) {            // translucent, double-sided, flat-colour overlay
            gm.useBlend = true; gm.cullBack = false;
            if (gm.overlayKind == 1)      { gm.curTint[0]=0.15f; gm.curTint[1]=1.00f; gm.curTint[2]=0.40f; gm.curTint[3]=0.55f; } // navmesh green
            else if (gm.overlayKind == 2) { gm.curTint[0]=1.00f; gm.curTint[1]=0.28f; gm.curTint[2]=0.22f; gm.curTint[3]=0.45f; } // collision red
            else if (gm.overlayKind == 4) { gm.curTint[0]=0.20f; gm.curTint[1]=0.95f; gm.curTint[2]=1.00f; gm.curTint[3]=0.85f; } // spawn cyan
            else                          { gm.curTint[0]=1.00f; gm.curTint[1]=0.65f; gm.curTint[2]=0.10f; gm.curTint[3]=0.45f; } // phys orange
        }
        // Per-instance colour from MaterialPropertyOverrides "matParams.tint": the cooked butterfly/leaf texture
        // is GREYSCALE (luminance detail) and the device MULTIPLIES it by this entity tint. Feed it into curTint
        // -> push-constant[16..19] (honoured by the unlit/global shader: frag = texture * tint). Without it the
        // grey butterfly stayed white. Only when a real (non-white) tint exists and this isn't an editor overlay.
        else if (md.tint[0]!=1.0f || md.tint[1]!=1.0f || md.tint[2]!=1.0f) {
            gm.curTint[0]=md.tint[0]; gm.curTint[1]=md.tint[1]; gm.curTint[2]=md.tint[2];
            gm.curTint[3]=(md.tint[3]>0.0f)?md.tint[3]:1.0f;
        }
        // World-space centroid (avg of positions) for back-to-front blend sorting. OPA positions are
        // baked into world space (identity model), so this is already world-space; good enough for a
        // per-mesh sort key (animated meshes shift slightly but the bind centroid still sorts right).
        if (!md.positions.empty()) {
            double cx=0, cy=0, cz=0; size_t nv = md.positions.size()/3;
            float mn[3]={md.positions[0],md.positions[1],md.positions[2]};
            float mx[3]={md.positions[0],md.positions[1],md.positions[2]};
            for (size_t i=0;i<nv;++i){
                float px=md.positions[i*3], py=md.positions[i*3+1], pz=md.positions[i*3+2];
                cx+=px; cy+=py; cz+=pz;
                mn[0]=std::min(mn[0],px); mn[1]=std::min(mn[1],py); mn[2]=std::min(mn[2],pz);
                mx[0]=std::max(mx[0],px); mx[1]=std::max(mx[1],py); mx[2]=std::max(mx[2],pz);
            }
            // WORLD-space centroid = gm.model * localCentroid. Local-vert props (placed by md.transform,
            // e.g. Haven lamps/plants/globe) have a local center near the origin; without this the editor's
            // focus/gizmo/move pivot + blend sort registered them at (0,0,0) ("items outside the map").
            float lc[3]={(float)(cx/nv),(float)(cy/nv),(float)(cz/nv)};
            gm.centroid[0]=gm.model[0]*lc[0]+gm.model[4]*lc[1]+gm.model[8]*lc[2]+gm.model[12];
            gm.centroid[1]=gm.model[1]*lc[0]+gm.model[5]*lc[1]+gm.model[9]*lc[2]+gm.model[13];
            gm.centroid[2]=gm.model[2]*lc[0]+gm.model[6]*lc[1]+gm.model[10]*lc[2]+gm.model[14];
            gm.bbMin[0]=mn[0]; gm.bbMin[1]=mn[1]; gm.bbMin[2]=mn[2];
            gm.bbMax[0]=mx[0]; gm.bbMax[1]=mx[1]; gm.bbMax[2]=mx[2];
        }
        // CPU geometry snapshot for PRECISE ray-triangle picking (so clicking the owl doesn't select
        // the ground's huge AABB). Same space as md.positions; the editor transforms by gm.model.
        gm.pickPos = md.positions;
        gm.pickIdx = md.indices;
        gm.name = md.name;
        gm.components = md.components;   // carry the entity's components through to the editor inspector
        gm.info = std::to_string(md.texW) + "x" + std::to_string(md.texH)
                + (md.useBlend ? " blend" : " opaque")
                + (gm.isSkinned ? " skinned" : "");
        if (gm.name.find("prism") != std::string::npos && md.positions.size() >= 9) {
            float ymin=1e9f,ymax=-1e9f; for(size_t q=1;q<md.positions.size();q+=3){float y=md.positions[q]; if(y<ymin)ymin=y; if(y>ymax)ymax=y;}
            log("  PRISMPOS nPos=%zu Y[%.3f..%.3f] v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)",
                md.positions.size()/3, ymin, ymax, md.positions[0],md.positions[1],md.positions[2],
                md.positions[3],md.positions[4],md.positions[5], md.positions[6],md.positions[7],md.positions[8]);
        }
        { int taMin=255,taMax=0; double taSum=0; size_t taN=0;
          for (size_t q=3;q<md.texRGBA.size();q+=4){int a=md.texRGBA[q]; if(a<taMin)taMin=a; if(a>taMax)taMax=a; taSum+=a; ++taN;}
          // A mesh flagged blend (glTF alphaMode=BLEND / cooked mdBlend) whose texture is FULLY OPAQUE
          // (min alpha 255) has nothing to blend — the no-depth-write blend pass made it see-through
          // (King Kai / snakeway: his front didn't occlude his back -> scrambled black blob). Treat it
          // as opaque so it WRITES depth = solid. Skip additive glow + editor overlays (truly translucent).
          // EXEMPT shaders that compute their OWN alpha (mist/smoke/water/lakesurf/fade): their texture is
          // alpha-255 but the fragment derives transparency from noise×opacity, so the "opaque texture ->
          // opaque" heuristic wrongly forced them solid (mist_plane_CR rendered as a teal BLOCK).
          bool computesAlpha = md.shaderPath.find("mist")!=std::string::npos || md.shaderPath.find("smoke")!=std::string::npos ||
                               md.shaderPath.find("lakesurf")!=std::string::npos || md.shaderPath.find("water")!=std::string::npos ||
                               md.shaderPath.find("fade")!=std::string::npos ||
                               // animatedfog (atmospheric_fog/waterfallSplash): opaque base tex (alpha=255) but the
                               // fragment derives fog density/alpha from noise×time — keep its alpha blend (was a WHITE block).
                               md.shaderPath.find("animatedfog")!=std::string::npos;
          if (gm.useBlend && taN>0 && taMin>=255 && !gm.additive && !gm.overlayKind && !computesAlpha) gm.useBlend = false;
          log("  GPU mesh uploaded: [%zu] '%s' nVerts=%u nIdx=%u tex=%ux%u mdBlend=%d gmBlend=%d add=%d skinned=%d stride=%u uvOff=%u texA[%d/%.0f/%d] worldC=(%.2f,%.2f,%.2f)",
            gpuMeshes.size(), gm.name.c_str(), nVerts, gm.nIdx, md.texW, md.texH,
            (int)md.useBlend, (int)gm.useBlend, (int)gm.additive, (int)gm.isSkinned, gm.vboStride, gm.uvOffset,
            taMin, taN?taSum/taN:0.0, taMax, gm.centroid[0],gm.centroid[1],gm.centroid[2]); }
        gpuMeshes.push_back(gm);
    }

    // ── Render frame ────────────────────────────────────────────
    void render() {
        vkWaitForFences(device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &inFlightFence);

        u32 imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                  imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            log("ERROR: vkAcquireNextImageKHR failed: %d", result);
            return;
        }

        vkResetCommandBuffer(commandBuffers[imageIndex], 0);
        updateUniforms();
        updateSkinning();
        // SKYBOX camera-lock: the SkyboxPlatformComponent dome is a UNIT cube at the origin (±0.5) — a
        // 1-unit speck the camera never sees ("NO SKYBOX"). Centre it on the camera and scale it to ~0.4*far
        // each frame so it becomes a surrounding sky well beyond the vista but inside the frustum; all scene
        // geometry (closer) draws in front of it. Camera is INSIDE (cullBack already off at upload).
        {
            float S = cam.farZ * 0.4f; if (S < 100.0f) S = 5000.0f;
            for (auto& gm : gpuMeshes) {
                if (!gm.isSkybox) continue;
                float* m = gm.model;
                m[0]=S; m[1]=0; m[2]=0; m[3]=0;  m[4]=0; m[5]=S; m[6]=0; m[7]=0;
                m[8]=0; m[9]=0; m[10]=S; m[11]=0; m[12]=cam.pos[0]; m[13]=cam.pos[1]; m[14]=cam.pos[2]; m[15]=1;
            }
        }
        static int noUI = -1; if (noUI < 0) noUI = std::getenv("HSR_NOUI") ? 1 : 0;
        if (overlayBegin && !noUI) overlayBegin();   // ImGui NewFrame + build editor UI (CPU); HSR_NOUI hides it

        VkCommandBuffer cmd = commandBuffers[imageIndex];
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkRenderPassBeginInfo rpInfo = {};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = renderPass;
        rpInfo.framebuffer = framebuffers[imageIndex];
        rpInfo.renderArea.extent = swapchainExtent;
        VkClearValue clearColors[2] = {};
        clearColors[0].color = {{clearRGB[0], clearRGB[1], clearRGB[2], 1.0f}};
        clearColors[1].depthStencil = {0.0f, 0};  // reversed-Z: clear to 0 (far)
        rpInfo.clearValueCount = 2;
        rpInfo.pClearValues = clearColors;

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Set dynamic viewport/scissor so resize works without pipeline rebuild. When the editor sets a viewport
        // pane (uiViewportRect), the 3D scene is confined to it (the rest of the window is the custom UI panels).
        bool paned = uiViewportRect.extent.width > 0 && uiViewportRect.extent.height > 0;
        VkRect2D pane = paned ? uiViewportRect : VkRect2D{{0,0}, swapchainExtent};
        VkViewport vp = {(float)pane.offset.x, (float)pane.offset.y, (float)pane.extent.width, (float)pane.extent.height, 0.f, 1.f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &pane);

        auto drawMesh = [&](const VkGpuMesh& gm) {
            if (gm.overlayKind == 1 && !showNavmesh)   return;   // editor toggle: hide navmesh overlay
            if ((gm.overlayKind == 2 || gm.overlayKind == 3) && !showCollision) return;  // hide collision/wall overlay
            if (gm.overlayKind == 4 && !showSpawn)     return;   // editor toggle: hide spawn markers
            // Per-material (HSR_PERMAT): bind this mesh's OWN program's pipeline layout + lighting set.
            VkPipelineLayout pl = (gm.progIdx >= 0) ? programs[gm.progIdx].pipeLayout : pipelineLayout;
            bool useS1 = (gm.progIdx >= 0) ? programs[gm.progIdx].hasSet1 : hasSet1;
            VkDescriptorSet s1 = (gm.progIdx >= 0) ? programs[gm.progIdx].set1Desc : sharedSet1;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbo, &offset);
            vkCmdBindIndexBuffer(cmd, gm.ibo, 0, VK_INDEX_TYPE_UINT32);
            float pcData[32] = {};
            memcpy(pcData, gm.model, 64);
            // UniformColor = MaterialTint * lightmappower (ModmapFactor). Folding lightmappower into the
            // tint applies it IN-SHADER as diffuse*lightmap*lightmappower (HDR order, clamped once at output)
            // — faithful to the captured MeshShellEnv, vs the old pre-baked+LDR-clamped lightmap (clipped).
            pcData[16] = gm.curTint[0]*gm.lmPow[0]; pcData[17] = gm.curTint[1]*gm.lmPow[1];
            pcData[18] = gm.curTint[2]*gm.lmPow[2]; pcData[19] = gm.curTint[3];
            vkCmdPushConstants(cmd, pl,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pcData);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pl, 0, 1, &gm.descSet0, 0, nullptr);
            // set1 = shared synthesized lighting (PBR shaders only; empty for unlit)
            if (useS1 && s1)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pl, 1, 1, &s1, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pl, 2, 1, &gm.descSet2, 0, nullptr);
            vkCmdDrawIndexed(cmd, gm.nIdx, 1, 0, 0, 0);
        };

        // Pass 1: opaque + alpha-test cutouts — both write depth so they occlude correctly.
        // Cutouts bind the discard pipeline; everything else the plain opaque (or wireframe) pipeline.
        VkPipeline opaquePipe = (wireframe && wireframePipeline) ? wireframePipeline : graphicsPipeline;
        if (opaquePipe != VK_NULL_HANDLE) {
            VkPipeline boundPipe = VK_NULL_HANDLE;
            for (size_t mi = 0; mi < gpuMeshes.size(); ++mi) {
                auto& gm = gpuMeshes[mi];
                if (hideAllGeom) continue;
                if (soloMesh >= 0 && (int)mi != soloMesh) continue;
                if (hideMesh >= 0 && (int)mi == hideMesh) continue;
                if (isHidden(mi)) continue;
                if (!hideMat.empty() && gm.name.find(hideMat) != std::string::npos) continue;
                if (!soloMat.empty() && gm.name.find(soloMat) == std::string::npos) continue;
                if (gm.culled) continue;     // V205 frustum+far cull (HSR_CLIP) — false unless culling toggled ON
                if (gm.useBlend) continue;   // blend handled in pass 2
                VkPipeline want;
                if (gm.progIdx >= 0) {  // per-material program pipeline (its OWN layout matches descSet2)
                    auto& p = programs[gm.progIdx];
                    want = wireframe ? (p.pipeWire ? p.pipeWire : p.pipe)
                         : (gm.alphaTest && p.pipeAlphaTest) ? p.pipeAlphaTest
                         : (gm.cullBack && p.pipeCull) ? p.pipeCull : p.pipe;
                } else {
                    want = (!wireframe && gm.alphaTest && alphaTestPipeline) ? alphaTestPipeline : opaquePipe;
                    if (!wireframe && gm.cullBack && !gm.alphaTest && graphicsPipelineCull) want = graphicsPipelineCull;
                }
                if (!want) continue;
                if (want != boundPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want); boundPipe = want; }
                drawMesh(gm);
            }
        }

        // Pass 2: alpha-blended (dome panels, motes, prism_wave) — drawn after opaque
        // Skinned blended meshes use the skinned pipeline + skinnedPipelineLayout.
        if (blendPipeline != VK_NULL_HANDLE) {
            // Transparent meshes don't write depth, so overlapping ones must be composited
            // far->near (a near smoke card drawn before a far one would wrongly show through).
            // Sort blend meshes by centroid distance to the camera, descending. (libshell orders
            // transparents by render-order then back-to-front within a group; with no authored
            // renderOrderOffset in the cooked asset, distance is the faithful intra-group key.)
            blendOrder.clear();
            for (size_t mi = 0; mi < gpuMeshes.size(); ++mi) {
                auto& gm = gpuMeshes[mi];
                if (hideAllGeom) continue;
                if (soloMesh >= 0 && (int)mi != soloMesh) continue;
                if (hideMesh >= 0 && (int)mi == hideMesh) continue;
                if (isHidden(mi)) continue;
                if (!hideMat.empty() && gm.name.find(hideMat) != std::string::npos) continue;
                if (!soloMat.empty() && gm.name.find(soloMat) == std::string::npos) continue;
                if (gm.culled) continue;     // V205 frustum+far cull (HSR_CLIP)
                if (!gm.useBlend) continue;
                float dx=gm.centroid[0]-cam.pos[0], dy=gm.centroid[1]-cam.pos[1], dz=gm.centroid[2]-cam.pos[2];
                blendOrder.emplace_back(dx*dx+dy*dy+dz*dz, mi);
            }
            std::sort(blendOrder.begin(), blendOrder.end(),
                      [](const std::pair<float,size_t>& a, const std::pair<float,size_t>& b){ return a.first > b.first; });
            for (auto& bo : blendOrder) {
                size_t mi = bo.second;
                auto& gm = gpuMeshes[mi];
                if (useSkinnedPipeline && gm.isSkinned && skinnedPipeline != VK_NULL_HANDLE && gm.descSet2Skin != VK_NULL_HANDLE) {
                    // Skinned pipeline path (animation). Currently the skinned shader
                    // outputs nothing on desktop; until that's resolved useSkinnedPipeline
                    // stays false and skinned meshes draw via the regular blend pipeline
                    // using their bind-pose VB positions (halo + motes visible).
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline);
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbo, &offset);
                    vkCmdBindIndexBuffer(cmd, gm.ibo, 0, VK_INDEX_TYPE_UINT32);
                    float pcData[32] = {};
                    memcpy(pcData, gm.model, 64);
                    pcData[16] = 1.0f; pcData[17] = 1.0f;
                    pcData[18] = 1.0f; pcData[19] = 1.0f;
                    vkCmdPushConstants(cmd, skinnedPipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pcData);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            skinnedPipelineLayout, 0, 1, &gm.descSet0, 0, nullptr);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            skinnedPipelineLayout, 2, 1, &gm.descSet2Skin, 0, nullptr);
                    vkCmdDrawIndexed(cmd, gm.nIdx, 1, 0, 0, 0);
                } else {
                    // Regular blended mesh. Additive (god-rays/glow) -> ADD pipeline; single-sided blend
                    // -> back-face-cull variant; else alpha blend. Per-material uses its own program.
                    VkPipeline bp;
                    // Per-material program's blend pipeline. Skinned programs (unlitblendskinned = prism_wave)
                    // now get IDENTITY sbSkinningMatrices bound in set2 (bind-pose rest wave), so they render
                    // via their OWN pipeline (the built-in can't run the program's procedural/skinned vert shader).
                    if (gm.progIdx >= 0) {
                        auto& p = programs[gm.progIdx];
                        bp = (gm.additive && p.pipeAdditive) ? p.pipeAdditive
                           : (gm.cullBack && p.pipeBlendCull) ? p.pipeBlendCull : p.pipeBlend;
                    } else {
                        bp = (gm.additive && additivePipeline) ? additivePipeline
                           : (gm.cullBack && blendPipelineCull) ? blendPipelineCull : blendPipeline;
                    }
                    if (!bp) continue;
                    if (gm.name.find("prism")!=std::string::npos){ static int d=0; if(!d){d=1; log("  PRISMDRAW reached BLEND draw bp=%p nIdx=%u cull=%d", (void*)bp, gm.nIdx, (int)gm.cullBack);} }
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bp);
                    drawMesh(gm);
                }
            }
        }

        // Pass 3: selected mesh highlight — wireframe overlay on top
        if (selectedMesh >= 0 && selectedMesh < (int)gpuMeshes.size()) {
            auto& gm = gpuMeshes[selectedMesh];
            // Draw the highlight with the SELECTED mesh's OWN wireframe pipeline + layout + descriptor sets.
            // Binding a per-material descSet2 to the global-layout wireframe pipeline is an invalid layout
            // mismatch that HANGS the GPU on HSL (the click-freeze) — so use the program's own pipeWire.
            VkPipeline       hlPipe = (gm.progIdx >= 0) ? programs[gm.progIdx].pipeWire : wireframePipeline;
            VkPipelineLayout pl     = (gm.progIdx >= 0) ? programs[gm.progIdx].pipeLayout : pipelineLayout;
            bool useS1 = (gm.progIdx >= 0) ? programs[gm.progIdx].hasSet1 : hasSet1;
            VkDescriptorSet s1 = (gm.progIdx >= 0) ? programs[gm.progIdx].set1Desc : sharedSet1;
            if (hlPipe != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hlPipe);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbo, &off);
                vkCmdBindIndexBuffer(cmd, gm.ibo, 0, VK_INDEX_TYPE_UINT32);
                float pcData[32] = {};
                memcpy(pcData, gm.model, 64);
                pcData[16] = 1.0f; pcData[17] = 0.9f;   // yellow-ish highlight (the global/unlit shader honors it)
                pcData[18] = 0.0f; pcData[19] = 1.0f;
                vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pcData);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &gm.descSet0, 0, nullptr);
                if (useS1 && s1) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &s1, 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &gm.descSet2, 0, nullptr);
                vkCmdDrawIndexed(cmd, gm.nIdx, 1, 0, 0, 0);
            }
        }

        if (overlayDraw && !noUI) overlayDraw(cmd);   // ImGui editor UI on top of the scene; HSR_NOUI hides it

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSems[] = {imageAvailable};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSems;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers[imageIndex];
        VkSemaphore signalSems[] = {renderFinished};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSems;

        vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFence);

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSems;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(graphicsQueue, &presentInfo);
        lastPresentedImage = imageIndex;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapchain();
        }
    }

    u32 lastPresentedImage = 0;

    // Capture the last presented swapchain image to a PNG (BGRA->RGBA, top-down).
    // In-engine readback — no external window capture, so it always grabs exactly
    // what was rendered regardless of where the OS placed the window.
    void shotMark(const char* m) {
        FILE* f = fopen("_shot_trace.txt", "a");
        if (f) { fprintf(f, "%s\n", m); fclose(f); }
    }
    bool screenshot(const char* path) {
        shotMark("enter");
        vkDeviceWaitIdle(device);
        shotMark("waitIdle done");
        u32 w = swapchainExtent.width, h = swapchainExtent.height;
        VkDeviceSize sz = (VkDeviceSize)w * h * 4;
        VkBuffer buf; VkDeviceMemory bufMem;
        createBuffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     buf, bufMem);
        VkImage src = swapchainImages[lastPresentedImage];
        VkCommandBuffer cmd = beginSingleTimeCommands();
        // src: PRESENT_SRC -> TRANSFER_SRC
        VkImageMemoryBarrier b1 = {};
        b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b1.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = src;
        b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b1);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent = {w,h,1};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        // src back to PRESENT
        VkImageMemoryBarrier b2 = b1;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b2);
        endSingleTimeCommands(cmd);
        shotMark("gpu copy done");

        void* data;
        vkMapMemory(device, bufMem, 0, sz, 0, &data);
        shotMark("mapped");
        std::vector<u8> rgba((size_t)sz);
        const u8* p = (const u8*)data;
        bool bgr = (swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                    swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM);
        for (size_t i = 0; i < (size_t)w*h; ++i) {
            u8 r = bgr ? p[i*4+2] : p[i*4+0];
            u8 g = p[i*4+1];
            u8 bl = bgr ? p[i*4+0] : p[i*4+2];
            rgba[i*4+0]=r; rgba[i*4+1]=g; rgba[i*4+2]=bl; rgba[i*4+3]=255;
        }
        vkUnmapMemory(device, bufMem);
        vkDestroyBuffer(device, buf, nullptr);
        vkFreeMemory(device, bufMem, nullptr);
        shotMark("writing png");
        bool ok = writePNG(path, w, h, rgba.data());
        shotMark("png done");
        log("  Screenshot %s -> %s (%ux%u)", ok?"saved":"FAILED", path, w, h);
        return ok;
    }

    // Minimal zlib-free PNG writer (stored/uncompressed deflate blocks).
    static bool writePNG(const char* path, u32 w, u32 h, const u8* rgba) {
        FILE* f = fopen(path, "wb");
        if (!f) return false;
        auto u32be=[&](u32 v){ u8 b[4]={(u8)(v>>24),(u8)(v>>16),(u8)(v>>8),(u8)v}; fwrite(b,1,4,f); };
        // CRC32
        auto crc32=[](const u8* d, size_t n)->u32{
            u32 c=0xFFFFFFFFu;
            for(size_t i=0;i<n;i++){ c^=d[i];
                for(int k=0;k<8;k++) c = (c>>1) ^ (0xEDB88320u & (-(int)(c&1))); }
            return c^0xFFFFFFFFu;
        };
        auto chunk=[&](const char* type, const std::vector<u8>& body){
            u32be((u32)body.size());
            std::vector<u8> tc(4+body.size());
            memcpy(tc.data(),type,4); if(!body.empty()) memcpy(tc.data()+4,body.data(),body.size());
            fwrite(tc.data(),1,tc.size(),f);
            u32be(crc32(tc.data(),tc.size()));
        };
        const u8 sig[8]={137,80,78,71,13,10,26,10}; fwrite(sig,1,8,f);
        std::vector<u8> ihdr={(u8)(w>>24),(u8)(w>>16),(u8)(w>>8),(u8)w,
                              (u8)(h>>24),(u8)(h>>16),(u8)(h>>8),(u8)h,8,6,0,0,0};
        chunk("IHDR",ihdr);
        // raw image with filter byte 0 per row
        std::vector<u8> raw; raw.reserve((size_t)h*(1+w*4));
        for(u32 y=0;y<h;y++){ raw.push_back(0);
            raw.insert(raw.end(), rgba+(size_t)y*w*4, rgba+(size_t)(y+1)*w*4); }
        // zlib stream: stored deflate blocks (no compression)
        std::vector<u8> z; z.push_back(0x78); z.push_back(0x01);
        size_t off=0, n=raw.size();
        while(off<n){
            size_t blk=std::min<size_t>(65535,n-off);
            u8 final=(off+blk>=n)?1:0;
            z.push_back(final);
            z.push_back((u8)(blk&0xFF)); z.push_back((u8)(blk>>8));
            z.push_back((u8)(~blk&0xFF)); z.push_back((u8)((~blk>>8)&0xFF));
            z.insert(z.end(), raw.begin()+off, raw.begin()+off+blk);
            off+=blk;
        }
        // adler32
        u32 a=1,b=0; for(size_t i=0;i<raw.size();i++){ a=(a+raw[i])%65521; b=(b+a)%65521; }
        u32 adler=(b<<16)|a;
        z.push_back((u8)(adler>>24)); z.push_back((u8)(adler>>16));
        z.push_back((u8)(adler>>8)); z.push_back((u8)adler);
        chunk("IDAT",z);
        chunk("IEND",{});
        fclose(f);
        return true;
    }

    void cleanup() {
        vkDeviceWaitIdle(device);
        for (auto& gm : gpuMeshes) {
            vkDestroyBuffer(device, gm.vbo, nullptr);
            vkFreeMemory(device, gm.vboMem, nullptr);
            vkDestroyBuffer(device, gm.ibo, nullptr);
            vkFreeMemory(device, gm.iboMem, nullptr);
            vkDestroyImage(device, gm.texImage, nullptr);
            vkFreeMemory(device, gm.texMem, nullptr);
            vkDestroyImageView(device, gm.texView, nullptr);
            vkDestroyBuffer(device, gm.globalUbo, nullptr);
            vkFreeMemory(device, gm.globalUboMem, nullptr);
            vkDestroyBuffer(device, gm.matUbo, nullptr);
            vkFreeMemory(device, gm.matUboMem, nullptr);
            if (gm.skinUbo)    vkDestroyBuffer(device, gm.skinUbo, nullptr);
            if (gm.skinUboMem) vkFreeMemory(device, gm.skinUboMem, nullptr);
        }
        vkDestroyDescriptorPool(device, descPool, nullptr);
        vkDestroyDescriptorSetLayout(device, descSetLayouts[0], nullptr);
        vkDestroyDescriptorSetLayout(device, descSetLayouts[1], nullptr);
        vkDestroyDescriptorSetLayout(device, descSetLayouts[2], nullptr);
        if (descSetLayout2Skin) vkDestroyDescriptorSetLayout(device, descSetLayout2Skin, nullptr);
        vkDestroySampler(device, sharedSampler, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        if (alphaTestPipeline) vkDestroyPipeline(device, alphaTestPipeline, nullptr);
        if (wireframePipeline) vkDestroyPipeline(device, wireframePipeline, nullptr);
        if (blendPipeline)     vkDestroyPipeline(device, blendPipeline,    nullptr);
        if (skinnedPipeline)   vkDestroyPipeline(device, skinnedPipeline,  nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (skinnedPipelineLayout) vkDestroyPipelineLayout(device, skinnedPipelineLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        for (auto& fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto& iv : swapchainViews) vkDestroyImageView(device, iv, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthMem, nullptr);
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroySemaphore(device, imageAvailable, nullptr);
        vkDestroySemaphore(device, renderFinished, nullptr);
        vkDestroyFence(device, inFlightFence, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        if (debugMessenger) {
            auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (fn) fn(instance, debugMessenger, nullptr);
        }
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

private:
    // ── Vulkan setup helpers ─────────────────────────────────────

    bool createInstance() {
        if (volkInitialize() != VK_SUCCESS) return false;

        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "HSR Renderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "HSR Replica";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        // Vulkan 1.1 required: the skinned shader uses MultiView, and the multiview
        // render-pass / device-feature structs are core in 1.1 (ignored on a 1.0
        // instance, which left the skinned pipeline producing no output).
        // 1.2 so we can enable shaderFloat16 / 16-bit storage via the aggregate
        // Vulkan11/12 feature structs (NVIDIA honors these reliably; the standalone
        // KHR structs on a 1.1 instance left float16 effectively off -> PBR vertex
        // pipelines were rejected with VK_ERROR_UNKNOWN).
        appInfo.apiVersion = VK_API_VERSION_1_2;

        u32 glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
        bool enableValidation = debugMode || std::getenv("HSR_VALIDATE") != nullptr;
        if (enableValidation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = (u32)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();
        if (enableValidation) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = validationLayers;
        } else {
            createInfo.enabledLayerCount = 0;
        }

        VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
        if (res != VK_SUCCESS) {
            log("FATAL: vkCreateInstance failed: %d", res);
            log("  Requested %u extensions:", (u32)extensions.size());
            for (auto& e : extensions) log("    %s", e);
            return false;
        }
        log("  Instance created OK with %zu extensions", extensions.size());
        return true;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT sev,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* user) {
        fprintf(stderr, "[VK-VALIDATION] %s\n", data->pMessage);
        return VK_FALSE;
    }

    void setupDebugMessenger() {
        if (!debugMode && !std::getenv("HSR_VALIDATE")) return;
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (!fn) return;
        VkDebugUtilsMessengerCreateInfoEXT info = {};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = debugCallback;
        fn(instance, &info, nullptr, &debugMessenger);
        log("  Validation layers enabled");
    }

    bool pickPhysicalDevice() {
        u32 count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) { log("FATAL: No Vulkan-capable GPU found"); return false; }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        for (auto& pd : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pd, &props);
            log("  Found GPU: %s (type=%d)", props.deviceName, props.deviceType);

            u32 qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qfProps(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfProps.data());

            for (u32 i = 0; i < qfCount; ++i) {
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &presentSupport);
                if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && presentSupport) {
                    physicalDevice = pd;
                    graphicsQueueFamily = i;
                    log("  Selected: %s, queue family %u", props.deviceName, i);
                    // Probe shaderFloat16 support (needed by Haven's real PBR shaders).
                    VkPhysicalDeviceShaderFloat16Int8Features f16 = {};
                    f16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
                    VkPhysicalDeviceFeatures2 feats2 = {};
                    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    feats2.pNext = &f16;
                    vkGetPhysicalDeviceFeatures2(pd, &feats2);
                    shaderFloat16Supported = (f16.shaderFloat16 == VK_TRUE);
                    log("  GPU shaderFloat16: %s", shaderFloat16Supported ? "supported" : "NOT supported");
                    return true;
                }
            }
        }
        log("FATAL: No GPU with graphics + present support");
        return false;
    }

    bool createLogicalDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qInfo = {};
        qInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qInfo.queueFamilyIndex = graphicsQueueFamily;
        qInfo.queueCount = 1;
        qInfo.pQueuePriorities = &priority;

        std::vector<const char*> deviceExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                               VK_KHR_MULTIVIEW_EXTENSION_NAME};

        // Enable the multiview feature — the skinned shader (unlitblendskinned)
        // requires the MultiView SPIR-V capability; without it the skinned pipeline
        // renders nothing (prism halo + motes invisible).
        VkPhysicalDeviceMultiviewFeatures mvFeat = {};
        mvFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
        mvFeat.multiview = VK_TRUE;

        // Haven2025's real PBR shaders (isotropictiled.surface family) use SPIR-V Float16
        // + Int8 and 16/8-bit storage. Enable via the Vulkan 1.2 aggregate feature structs
        // (Vulkan11Features = multiview + 16-bit storage, Vulkan12Features = shaderFloat16/
        // Int8 + 8-bit storage). Query support first, enable only what's reported. NVIDIA
        // honors these reliably (the standalone KHR structs on a 1.1 instance did not, so
        // float16 vertex pipelines were rejected with VK_ERROR_UNKNOWN).
        static VkPhysicalDeviceVulkan12Features v12q = {}; v12q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        static VkPhysicalDeviceVulkan11Features v11q = {}; v11q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; v12q.pNext = &v11q;
        VkPhysicalDeviceFeatures2 q2 = {}; q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; q2.pNext = &v12q;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &q2);

        static VkPhysicalDeviceVulkan11Features v11 = {}; v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        v11.multiview = VK_TRUE;
        v11.storageBuffer16BitAccess = v11q.storageBuffer16BitAccess;
        v11.uniformAndStorageBuffer16BitAccess = v11q.uniformAndStorageBuffer16BitAccess;
        v11.storagePushConstant16 = v11q.storagePushConstant16;
        v11.storageInputOutput16 = v11q.storageInputOutput16;
        static VkPhysicalDeviceVulkan12Features v12 = {}; v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        v12.shaderFloat16 = v12q.shaderFloat16;
        v12.shaderInt8 = v12q.shaderInt8;
        v12.storageBuffer8BitAccess = v12q.storageBuffer8BitAccess;
        v12.uniformAndStorageBuffer8BitAccess = v12q.uniformAndStorageBuffer8BitAccess;
        v12.storagePushConstant8 = v12q.storagePushConstant8;
        v11.pNext = &v12;
        // mvFeat replaced by v11.multiview; chain v11/v12 directly.
        log("  GPU float16=%d int8=%d sb16=%d push16=%d : Vulkan1.2 feature chain ENABLED",
            v12.shaderFloat16, v12.shaderInt8, v11.storageBuffer16BitAccess, v11.storagePushConstant16);

        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &v11;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &qInfo;
        createInfo.enabledExtensionCount = (u32)deviceExts.size();
        createInfo.ppEnabledExtensionNames = deviceExts.data();

        VkPhysicalDeviceFeatures supported = {};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supported);
        VkPhysicalDeviceFeatures features = {};
        features.samplerAnisotropy = VK_TRUE;
        features.fillModeNonSolid = VK_TRUE;  // required for wireframe
        if (supported.textureCompressionASTC_LDR) {
            features.textureCompressionASTC_LDR = VK_TRUE;
            astcLdrSupported = true;
            log("  GPU ASTC_LDR: SUPPORTED — will upload textures natively");
        } else {
            log("  GPU ASTC_LDR: not supported — using software decode");
        }
        createInfo.pEnabledFeatures = &features;

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
            log("FATAL: vkCreateDevice failed");
            return false;
        }
        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        log("  Device created OK");
        return true;
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

        u32 formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

        swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
        for (auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                swapchainFormat = f.format;
                break;
            }
        }

        swapchainExtent = caps.currentExtent;
        if (swapchainExtent.width == UINT32_MAX) {
            swapchainExtent = {1280, 720};
            if (swapchainExtent.width > caps.maxImageExtent.width) swapchainExtent.width = caps.maxImageExtent.width;
            if (swapchainExtent.height > caps.maxImageExtent.height) swapchainExtent.height = caps.maxImageExtent.height;
        }

        // Present mode: FIFO (vsync) judders on a fast GPU when the CPU frame time varies (the
        // single-frame-in-flight model can't overlap CPU skinning/streaming with GPU render, so a
        // CPU spike misses the vsync deadline -> stutter). Prefer MAILBOX (low-latency, tear-free,
        // always shows the newest frame) then IMMEDIATE, falling back to FIFO (always available).
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        { u32 pmCount = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &pmCount, nullptr);
          std::vector<VkPresentModeKHR> pms(pmCount);
          vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &pmCount, pms.data());
          bool mailbox=false, immediate=false;
          for (auto m : pms) { if (m==VK_PRESENT_MODE_MAILBOX_KHR) mailbox=true; if (m==VK_PRESENT_MODE_IMMEDIATE_KHR) immediate=true; }
          if (mailbox) presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
          else if (immediate) presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; }
        log("  Present mode: %s", presentMode==VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX (low-latency, no judder)"
            : presentMode==VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO (vsync)");

        u32 imageCount = caps.minImageCount + 1;
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR && imageCount < 3) imageCount = 3;   // MAILBOX wants 3
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = swapchainFormat;
        createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        createInfo.imageExtent = swapchainExtent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = caps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);

        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        swapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
    }

    void createImageViews() {
        swapchainViews.resize(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); ++i) {
            swapchainViews[i] = createImageView(swapchainImages[i], swapchainFormat);
        }
    }

    VkImageView createImageView(VkImage image, VkFormat format, u32 mipLevels = 1) {
        VkImageViewCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = mipLevels;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;
        VkImageView view;
        vkCreateImageView(device, &createInfo, nullptr, &view);
        return view;
    }

    // Create a solid-color cubemap (6 faces, 1 mip) used as a neutral synthesized IBL
    // environment. rgb in linear 0..1.
public:
    // Build the GPU SPECULAR cubemap (RGBA16F, 6 faces, mip0) from the raw *_specular.dds.opa bytes.
    // Call after the device exists and BEFORE uploadMesh (so descriptors can bind iblSpecView).
    // V203 IBL: build the CPU diffuse+specular irradiance cubes from an EQUIRECT panorama (the skybox
    // reflectionMap, ASTC-HDR decoded to RGBA8). The existing per-vertex SpecIbl bake (uploadMesh) then consumes
    // iblDiffuse/iblSpecular for meshes with md.iblLit. Caller gates with HSR_SKYIBL (off by default → no regression).
    void setIblEquirectRGBA8(const uint8_t* rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return;
        std::vector<float> rgb((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; ++i)
            for (int c = 0; c < 3; ++c) { float s = rgba[i*4+c] / 255.f; rgb[i*3+c] = s <= 0.04045f ? s/12.92f : std::pow((s+0.055f)/1.055f, 2.4f); }
        iblSpecular = ibl::equirectToCubemap(rgb.data(), w, h, 64);
        iblDiffuse  = ibl::irradianceFromCube(iblSpecular, 16);
        log("  [SKYIBL] equirect %dx%d -> IBL cubes (spec 64, diff 16) ok=%d", w, h, (int)iblDiffuse.ok());
    }
    void setSpecularCubemap(const std::vector<uint8_t>& raw) {
        int S = 0; std::vector<uint8_t> faces;   // 6 faces of S*S*RGBA16F, contiguous
        if (raw.empty() || !ibl::extractCubeRawRGBA16F(raw.data(), raw.size(), S, faces) || S <= 0) return;
        const size_t faceBytes = (size_t)S * S * 4 * 2;
        VkDeviceSize total = (VkDeviceSize)faceBytes * 6;
        VkBuffer staging; VkDeviceMemory stagingMem;
        createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
        void* ptr; vkMapMemory(device, stagingMem, 0, total, 0, &ptr);
        memcpy(ptr, faces.data(), total);
        vkUnmapMemory(device, stagingMem);

        VkImageCreateInfo ii = {};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.extent = {(u32)S, (u32)S, 1};
        ii.mipLevels = 1; ii.arrayLayers = 6;
        ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &ii, nullptr, &iblSpecImage) != VK_SUCCESS) { vkDestroyBuffer(device,staging,nullptr); vkFreeMemory(device,stagingMem,nullptr); return; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, iblSpecImage, &mr);
        VkMemoryAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size; ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ai, nullptr, &iblSpecMem);
        vkBindImageMemory(device, iblSpecImage, iblSpecMem, 0);

        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.image = iblSpecImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        std::vector<VkBufferImageCopy> copies(6);
        for (u32 f = 0; f < 6; ++f) {
            copies[f] = {};
            copies[f].bufferOffset = (VkDeviceSize)f * faceBytes;
            copies[f].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
            copies[f].imageExtent = {(u32)S, (u32)S, 1};
        }
        vkCmdCopyBufferToImage(cmd, staging, iblSpecImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies.data());
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        endSingleTimeCommands(cmd);
        vkDestroyBuffer(device, staging, nullptr); vkFreeMemory(device, stagingMem, nullptr);

        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = iblSpecImage; vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vkCreateImageView(device, &vi, nullptr, &iblSpecView);
        log("  IBL specular cubemap on GPU: %d^2 x6 RGBA16F", S);
    }

    // The V79 shader always samples the specular cube (set2 b2). Envs without a `*_specular.dds.opa`
    // still need a VALID cube view bound there or it's a descriptor type mismatch -> crash. Give them
    // a tiny BLACK cube (reflection adds nothing). Call for every V79/OPA env after setSpecularCubemap.
    void ensureSpecCube() {
        if (iblSpecView != VK_NULL_HANDLE) return;
        createSolidCubemap(0.0f, 0.0f, 0.0f, iblSpecImage, iblSpecMem, iblSpecView);
    }

    void createSolidCubemap(float r, float g, float b, VkImage& image, VkDeviceMemory& mem, VkImageView& view,
                            float gr=-1.0f, float gg=-1.0f, float gb=-1.0f) {
        // HEMISPHERE gradient: sky colour (r,g,b) toward +Y, ground colour (gr,gg,gb) toward -Y. The PBR
        // shaders sample this IBL diffuse cube by the surface NORMAL, so a vertical gradient gives natural
        // top-down shading (up-faces brighter, down-faces dimmer) and makes the NORMAL MAP visible — vs a
        // solid cube which lights everything flat (the "unlit/unnatural" walls). ground<0 => solid (legacy).
        if (gr < 0) { gr = r; gg = g; gb = b; }
        const u32 S = 16;                      // smooth gradient
        const u32 faceBytes = S * S * 4;
        auto q = [](float x){ x = x<0?0:(x>1?1:x); return (u8)(x*255.0f + 0.5f); };
        std::vector<u8> faces((size_t)faceBytes * 6);
        for (u32 f = 0; f < 6; ++f) {
            for (u32 ty = 0; ty < S; ++ty) for (u32 tx = 0; tx < S; ++tx) {
                float a = ((tx + 0.5f) / S) * 2.0f - 1.0f, bb = ((ty + 0.5f) / S) * 2.0f - 1.0f;
                float dx, dy, dz;
                switch (f) {
                    case 0: dx= 1; dy=-bb; dz=-a; break;  // +X
                    case 1: dx=-1; dy=-bb; dz= a; break;  // -X
                    case 2: dx= a; dy= 1;  dz= bb; break; // +Y (sky)
                    case 3: dx= a; dy=-1;  dz=-bb; break; // -Y (ground)
                    case 4: dx= a; dy=-bb; dz= 1; break;  // +Z
                    default:dx=-a; dy=-bb; dz=-1; break;  // -Z
                }
                float len = std::sqrt(dx*dx + dy*dy + dz*dz); float ny = dy / len;
                float t = ny * 0.5f + 0.5f;            // 0 = ground, 1 = sky
                u8* px = &faces[(size_t)f*faceBytes + ((size_t)ty*S + tx)*4];
                px[0]=q(gr + (r-gr)*t); px[1]=q(gg + (g-gg)*t); px[2]=q(gb + (b-gb)*t); px[3]=255;
            }
        }
        VkDeviceSize total = (VkDeviceSize)faceBytes * 6;
        VkBuffer staging; VkDeviceMemory stagingMem;
        createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
        void* ptr; vkMapMemory(device, stagingMem, 0, total, 0, &ptr);
        memcpy(ptr, faces.data(), total);
        vkUnmapMemory(device, stagingMem);

        VkImageCreateInfo ii = {};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.extent = {S, S, 1};
        ii.mipLevels = 1; ii.arrayLayers = 6;
        ii.format = VK_FORMAT_R8G8B8A8_SRGB;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateImage(device, &ii, nullptr, &image);
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, image, &mr);
        VkMemoryAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size; ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ai, nullptr, &mem);
        vkBindImageMemory(device, image, mem, 0);

        // Transition all 6 layers, copy, transition to shader read.
        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkImageMemoryBarrier bar = {};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.image = image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        std::vector<VkBufferImageCopy> copies(6);
        for (u32 f = 0; f < 6; ++f) {
            copies[f] = {};
            copies[f].bufferOffset = (VkDeviceSize)f*faceBytes;
            copies[f].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
            copies[f].imageExtent = {S, S, 1};
        }
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies.data());
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        endSingleTimeCommands(cmd);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);

        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format = VK_FORMAT_R8G8B8A8_SRGB;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vkCreateImageView(device, &vi, nullptr, &view);
    }

    // Build the shared set-1 lighting resources + descriptor set for the real PBR shaders.
    // Synthesizes a neutral studio environment (no shadows, neutral IBL, flat ambient SH).
    void createLightingResources() {
        if (!hasSet1) return;
        // Brightness: we synthesize the environment lighting (the env's real lightprobes/
        // lightmaps aren't loaded), so V79 geometry under the HSR PBR shader comes out dim.
        // Scale the neutral ambient up (tunable via HSR_AMBIENT; default brightens noticeably)
        // so interiors read properly. Applied once -> both the IBL cubemaps and lightprobe SH
        // (which reads ambientRGB) get the boost.
        // ONLY boost the SYNTHESIZED ambient. When the env's REAL lightprobe ambient was loaded (hasEnvAmbient),
        // ambientRGB already IS the device's value (.lprb field2 L00/3.5449, e.g. calming (0.40,0.27,0.23)) —
        // boosting it would re-introduce the 4x over-bright wash that made the ground white instead of green.
        if (!hasEnvAmbient) {
            float amb = 1.8f;
            if (const char* e = std::getenv("HSR_AMBIENT")) { float v = (float)atof(e); if (v > 0.0f) amb = v; }
            ambientRGB[0] *= amb; ambientRGB[1] *= amb; ambientRGB[2] *= amb;
        } else if (const char* e = std::getenv("HSR_AMBIENT")) {
            float v=(float)atof(e); if(v>0.0f){ ambientRGB[0]*=v; ambientRGB[1]*=v; ambientRGB[2]*=v; }  // manual tweak still honored
        }
        // lightUniforms UBO (generous fixed size; we set only the fields that matter to
        // avoid a black image: neutral IBL tints, no shadows, identity shadow MVP, 0 lights).
        const u32 LU = 1024;
        createBuffer(LU, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, lightUbo, lightUboMem);
        {
            std::vector<u8> buf(LU, 0);
            float* f = (float*)buf.data(); u32* u = (u32*)buf.data();
            // identity shadowmapMVP @0
            f[0]=1; f[5]=1; f[10]=1; f[15]=1;
            // Directional SUN/key = the DISTANT light. CONFIRMED from the lightUniforms SPIR-V reflection:
            // the "...directional..." PBR shaders read distantLightDir@80 + distantLightRadiantIntensity@64
            // (+ numDistantLights@240), NOT sbLightItems (those are LOCAL/punctual). This is why the candle/
            // haven PBR was BLACK — no distant light was set.
            u[24]=0; u[25]=0; u[26]=0;           // numLights/Static/Dynamic (LOCAL punctual) @96/100/104 = none
            {
                float keyI = 2.5f; if (const char* e=std::getenv("HSR_KEYLIGHT")) { float v=(float)atof(e); if (v>=0) keyI=v; }
                float kd[3] = {0.38f, 0.86f, 0.34f};                 // direction (tunable via HSR_KEYDIR)
                if (const char* e=std::getenv("HSR_KEYDIR")) sscanf(e,"%f,%f,%f",&kd[0],&kd[1],&kd[2]);
                { float l=std::sqrt(kd[0]*kd[0]+kd[1]*kd[1]+kd[2]*kd[2]); if (l>1e-6f){kd[0]/=l;kd[1]/=l;kd[2]/=l;} }
                f[16]=keyI; f[17]=keyI*0.97f; f[18]=keyI*0.90f;      // distantLightRadiantIntensity @64 (warm key)
                f[20]=kd[0]; f[21]=kd[1]; f[22]=kd[2];               // distantLightDir @80
                u[60]=1;                                              // numDistantLights @240
                f[64]=keyI; f[65]=keyI*0.97f; f[66]=keyI*0.90f;      // distantLightIntensities[0] @256 (array form)
                f[80]=kd[0]; f[81]=kd[1]; f[82]=kd[2];               // distantLightDirections[0]  @320
            }
            // iblReflectionTint @112, iblDiffuseTint @128
            f[28]=1; f[29]=1; f[30]=1; f[31]=1;
            f[32]=1; f[33]=1; f[34]=1; f[35]=1;
            f[36]=1;                             // externalAverageLuminance @144
            f[42]=1; f[43]=1;                    // iblReflectionMaxAdaptationFactor @168, LuminanceInverse @172
            void* p; vkMapMemory(device, lightUboMem, 0, LU, 0, &p); memcpy(p, buf.data(), LU); vkUnmapMemory(device, lightUboMem);
        }
        // sbLightItems SSBO — one directional KEY light. Item = 28 B scalar-packed, IDA-proven from
        // ForwardLightProcessor__1762134: color.rgb @0, direction.xyz @12 (NORMALIZED, points TOWARD the
        // light), shadowIndex(i32) @24 (=-1 none). The cooked PBR frag does Σ color·max(N·dir,0).
        const u32 LI = 4096;
        createBuffer(LI, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, lightItemsSsbo, lightItemsMem);
        {
            float keyI = 1.6f; if (const char* e=std::getenv("HSR_KEYLIGHT")) { float v=(float)atof(e); if (v>=0) keyI=v; }
            float kd[3] = {0.38f, 0.86f, 0.34f};                 // toward light: above + slightly front-right
            if (const char* e=std::getenv("HSR_KEYDIR")) sscanf(e,"%f,%f,%f",&kd[0],&kd[1],&kd[2]);
            { float l=std::sqrt(kd[0]*kd[0]+kd[1]*kd[1]+kd[2]*kd[2]); if (l>1e-6f){kd[0]/=l;kd[1]/=l;kd[2]/=l;} }
            std::vector<u8> li(LI, 0);
            float* lf=(float*)li.data(); int* lii=(int*)li.data();
            lf[0]=keyI;        lf[1]=keyI*0.97f;  lf[2]=keyI*0.90f;   // warm-white key color @0
            lf[3]=kd[0];       lf[4]=kd[1];        lf[5]=kd[2];        // direction @12
            lii[6]=-1;                                                // shadowIndex @24 = none
            void* p; vkMapMemory(device, lightItemsMem, 0, LI, 0, &p); memcpy(p, li.data(), LI); vkUnmapMemory(device, lightItemsMem);
        }

        // Neutral IBL cubemaps (diffuse irradiance + reflection). Mid neutral so the PBR
        // shader's ambient/specular terms light the scene rather than reading nothing.
        // Hemisphere IBL: bright warm sky overhead, dimmer warm ground bounce below. Surfaces shade by
        // their normal (incl. the normal map) -> natural top-down lighting instead of flat unlit walls.
        float sky = std::getenv("HSR_HEMI") ? (float)atof(std::getenv("HSR_HEMI")) : 1.0f;
        createSolidCubemap(ambientRGB[0]*sky, ambientRGB[1]*sky, ambientRGB[2]*sky, iblDiffImage, iblDiffMem, iblDiffView,
                           ambientRGB[0]*0.30f, ambientRGB[1]*0.27f, ambientRGB[2]*0.22f);
        createSolidCubemap(ambientRGB[0]*0.95f, ambientRGB[1]*0.92f, ambientRGB[2]*0.88f, iblReflImage, iblReflMem, iblReflView,
                           ambientRGB[0]*0.40f, ambientRGB[1]*0.37f, ambientRGB[2]*0.32f);

        // 1x1 shadow map + a COMPARISON sampler with compareOp=ALWAYS. The PBR frag samples the shadowmap
        // with OpImageSampleDref (sampler2DShadow) and MULTIPLIES the distant/punctual light by the result.
        // IDA+SPIR-V proven: the candle lit frag does 6 shadow-compares; with a non-comparison sampler the
        // Dref returned 0 (=shadowed) -> distant light × 0 = BLACK. We don't compute real shadows, so make
        // every compare pass (=fully lit) via VK_COMPARE_OP_ALWAYS -> the distant key light reaches PBR mats.
        {
            u8 white[4] = {255,255,255,255};
            createTextureImage(white, 1, 1, shadowImage, shadowMem);
            shadowView = createImageView(shadowImage, VK_FORMAT_R8G8B8A8_SRGB);
            VkSamplerCreateInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.compareEnable = VK_TRUE; si.compareOp = VK_COMPARE_OP_ALWAYS;   // always "lit"
            si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            if (vkCreateSampler(device, &si, nullptr, &shadowSampler) != VK_SUCCESS)
                shadowSampler = createTextureSampler();
        }

        // Allocate + write the shared set1 descriptor set, binding each resource BY NAME.
        VkDescriptorSetAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &descSetLayouts[1];
        if (vkAllocateDescriptorSets(device, &ai, &sharedSet1) != VK_SUCCESS) {
            log("WARN: set1 descriptor alloc failed");
            return;
        }
        VkDescriptorBufferInfo luInfo = {lightUbo, 0, LU};
        VkDescriptorBufferInfo liInfo = {lightItemsSsbo, 0, LI};
        VkDescriptorImageInfo diffInfo = {VK_NULL_HANDLE, iblDiffView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo reflInfo = {VK_NULL_HANDLE, iblReflView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo shadInfo = {VK_NULL_HANDLE, shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sampInfo = {shadowSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        std::vector<VkWriteDescriptorSet> w;
        for (auto& d : set1Binds) {
            std::string n = d.name; for (auto& c : n) c = (char)tolower((unsigned char)c);
            VkWriteDescriptorSet wr = {}; wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = sharedSet1; wr.dstBinding = d.binding; wr.descriptorCount = 1;
            if (d.type == 0) {
                wr.descriptorType = d.isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                wr.pBufferInfo = (n.find("item") != std::string::npos || d.isStorage) ? &liInfo : &luInfo;
            } else if (d.type == 1) {
                wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; wr.pImageInfo = &sampInfo;
            } else {
                wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                if (n.find("refl") != std::string::npos)        wr.pImageInfo = &reflInfo;
                else if (n.find("diff") != std::string::npos)   wr.pImageInfo = &diffInfo;
                else if (n.find("shadow") != std::string::npos) wr.pImageInfo = &shadInfo;
                else                                            wr.pImageInfo = &diffInfo;
            }
            w.push_back(wr);
        }
        vkUpdateDescriptorSets(device, (u32)w.size(), w.data(), 0, nullptr);
        log("  Synthesized set1 lighting bound (%zu bindings)", w.size());
    }

    void createRenderPass() {
        VkAttachmentDescription colorAtt = {};
        colorAtt.format = swapchainFormat;
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAtt = {};
        depthAtt.format = VK_FORMAT_D32_SFLOAT;   // FLOAT depth: reversed-Z only gains precision on a float buffer.
                                                  // D24_UNORM gave reversed-Z NO benefit -> z-fighting/blinking at the
                                                  // huge 0.1..40000 range. D32_SFLOAT = near-uniform precision, no z-fight.
        depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkAttachmentDescription atts[] = {colorAtt, depthAtt};
        VkRenderPassCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = 2;
        createInfo.pAttachments = atts;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;

        // The skinned shader (unlitblendskinned) declares the MultiView capability
        // (Quest renders stereo via VK_KHR_multiview). A single-view render pass makes
        // that pipeline produce NO output (prism halo + motes were invisible). Enable
        // multiview with a 1-bit view mask so gl_ViewIndex resolves to 0 and the
        // skinned pipeline rasterizes correctly. correlationMask matches.
        u32 viewMask = 0x1, corrMask = 0x1;
        VkRenderPassMultiviewCreateInfo mv = {};
        mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        mv.subpassCount = 1;
        mv.pViewMasks = &viewMask;
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks = &corrMask;
        createInfo.pNext = &mv;

        vkCreateRenderPass(device, &createInfo, nullptr, &renderPass);
    }

    // Introspect a SPIR-V fragment shader for its set==2 descriptor bindings, so we
    // can build set2's layout to match whatever shader the env uses (replicating what
    // libshell does: the pipeline layout is derived from the shader, not hardcoded).
    // Introspect the vertex shader's Input variables (location + name + component count)
    // and build the VBO/pipeline vertex layout to MATCH. Role is inferred from the SPIR-V
    // OpName (inPos/inNorm/inUv/inLightmapUv/inColor/inBoneIndices/inBoneWeights/inTangent),
    // and each role gets the data format libshell feeds it (f32 for pos/normal/uv, u8 for
    // color/bones). This is what lets one renderer drive nuxd unlit AND Haven isotropictiled.
    void introspectVertexInputs(const std::vector<u32>& spv) {
        vertInputs.clear(); vertStride = 0;
        if (spv.size() < 5) return;
        u32 n = (u32)spv.size();
        std::unordered_map<u32,std::string> names;
        std::unordered_map<u32,u32> dloc, varType, ptrPointee, ptrStorage, vecComps;
        u32 i = 5;
        while (i < n) {
            u32 w = spv[i], op = w & 0xFFFF, wc = w >> 16;
            if (wc == 0 || i + wc > n) break;
            if (op == 5 && wc >= 3) names[spv[i+1]] = std::string((const char*)(spv.data()+i+2));
            else if (op == 71 && wc >= 4 && spv[i+2] == 30) dloc[spv[i+1]] = spv[i+3];  // Location
            else if (op == 32 && wc >= 4) { ptrStorage[spv[i+1]] = spv[i+2]; ptrPointee[spv[i+1]] = spv[i+3]; }
            else if (op == 23 && wc >= 4) vecComps[spv[i+1]] = spv[i+3];  // OpTypeVector: id, compType, count
            else if (op == 22) vecComps[spv[i+1]] = 1;                    // OpTypeFloat scalar -> 1 comp
            else if (op == 59 && wc >= 4) varType[spv[i+2]] = spv[i+1];   // OpVariable
            i += wc;
        }
        for (auto& [vid, ptype] : varType) {
            if (ptrStorage.count(ptype) == 0 || ptrStorage[ptype] != 1) continue;  // 1 = Input
            if (dloc.count(vid) == 0) continue;
            u32 loc = dloc[vid];
            u32 pointee = ptrPointee.count(ptype) ? ptrPointee[ptype] : 0;
            u32 comps = vecComps.count(pointee) ? vecComps[pointee] : 4;
            std::string nm = names.count(vid) ? names[vid] : "";
            std::string ln = nm; for (auto& c : ln) c = (char)tolower((unsigned char)c);
            // Role + data format by name (libshell's vertex stream layout).
            int role = 8; VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT; u32 size = 16;
            auto F = [&](int r, VkFormat f, u32 s){ role=r; fmt=f; size=s; };
            if      (ln.find("pos") != std::string::npos)        F(0, VK_FORMAT_R32G32B32_SFLOAT, 12);
            else if (ln.find("norm") != std::string::npos)       F(1, VK_FORMAT_R32G32B32_SFLOAT, 12);
            else if (ln.find("tangent") != std::string::npos)    F(7, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
            else if (ln.find("lightmap") != std::string::npos || ln.find("uv1") != std::string::npos)
                                                                 F(3, VK_FORMAT_R32G32_SFLOAT, 8);
            // animvege packs per-vertex flutter phase/pivot in uv2/uv3 — give them their OWN roles (9/10) so they
            // don't collapse into uv0 (role 2). MUST precede the generic "uv" match below.
            else if (ln.find("uv2") != std::string::npos)        F(9,  VK_FORMAT_R32G32_SFLOAT, 8);
            else if (ln.find("uv3") != std::string::npos)        F(10, VK_FORMAT_R32G32_SFLOAT, 8);
            else if (ln.find("uv") != std::string::npos || ln.find("texcoord") != std::string::npos)
                                                                 F(2, VK_FORMAT_R32G32_SFLOAT, 8);
            else if (ln.find("boneind") != std::string::npos || ln.find("joint") != std::string::npos || ln.find("indices") != std::string::npos)
                                                                 // GROUND TRUTH from the unlitblendskinned VERTEX SPIR-V (blob3): jointIndexN =
                                                                 // int(inBoneIndices[N] * 255.0)  (constant %189 = 255.0). So the shader expects
                                                                 // inBoneIndices as UNORM (0..1) and rescales to 0..255 itself. Feeding USCALED (0..255)
                                                                 // made it compute (0..255)*255 -> up to 65025 -> sbSkinningMatrices OOB read -> garbage
                                                                 // skin matrix -> verts explode radially. UNORM is the faithful format.
                                                                 F(5, VK_FORMAT_R8G8B8A8_UNORM, 4);
            else if (ln.find("bonewe") != std::string::npos || ln.find("weight") != std::string::npos)
                                                                 F(6, VK_FORMAT_R8G8B8A8_UNORM, 4);
            else if (ln.find("color") != std::string::npos || ln.find("colour") != std::string::npos)
                                                                 F(4, VK_FORMAT_R8G8B8A8_UNORM, 4);
            else { // unknown: size from comps as f32
                role=8; size=comps*4;
                fmt = comps>=4?VK_FORMAT_R32G32B32A32_SFLOAT:comps==3?VK_FORMAT_R32G32B32_SFLOAT:comps==2?VK_FORMAT_R32G32_SFLOAT:VK_FORMAT_R32_SFLOAT;
            }
            vertInputs.push_back({loc, nm, fmt, 0, size, role});
        }
        std::sort(vertInputs.begin(), vertInputs.end(), [](const VIn&a,const VIn&b){return a.location<b.location;});
        u32 off = 0;
        for (auto& v : vertInputs) { v.offset = off; off += v.size; }
        vertStride = off ? off : 32;
        log("  vertex inputs (%zu, stride=%u):", vertInputs.size(), vertStride);
        for (auto& v : vertInputs) log("    loc%u %-16s off=%u role=%d", v.location, v.name.c_str(), v.offset, v.role);
    }

    // Introspect ALL descriptor sets (0,1,2) of the chosen program, capturing each
    // binding's type AND its SPIR-V variable name so resources are bound BY ROLE
    // (matParams / lightprobesParams / lightmap / BaseColorMetallic_Tx / ...), exactly
    // like libshell's MaterialAsset does. Fills set0Binds, set1Binds, set2Binds.
    void introspectSet2(const std::vector<u32>& vspv, const std::vector<u32>& fspv) {
        set0Binds.clear(); set1Binds.clear(); set2Binds.clear();
        set2BaseColorBinding = -1;
        matParamsMembers.clear();
        // (set,binding) -> DescBind, unioned across both stages.
        std::map<std::pair<u32,u32>, DescBind> all;
        for (const std::vector<u32>* spvp : {&vspv, &fspv}) {
            const std::vector<u32>& spv = *spvp;
            if (spv.size() < 5) continue;
            u32 n = (u32)spv.size();
            std::unordered_map<u32,u32> dset, dbind, varType, names;
            std::unordered_map<u32,std::pair<u32,u32>> ptr;     // ptrType -> (storage, pointee)
            std::unordered_map<u32,int> typeKind;               // 2=image,1=sampler
            std::unordered_map<u32,u32> structOfArray;          // arrayTypeId -> elementType
            std::set<u32> bufferBlockTypes;                     // struct ids decorated BufferBlock (SSBO)
            std::map<std::pair<u32,u32>,std::string> memName;   // (struct,member) -> name
            std::map<std::pair<u32,u32>,u32> memOff;            // (struct,member) -> byte offset
            std::unordered_map<u32,std::vector<u32>> structMems;// OpTypeStruct id -> member type ids (for value sizes)
            std::unordered_map<u32,u32> vecCount;               // OpTypeVector id -> component count (vec3=3 ...)
            u32 i = 5;
            while (i < n) {
                u32 word = spv[i], op = word & 0xFFFF, wc = word >> 16;
                if (wc == 0 || i + wc > n) break;
                if (op == 5 && wc >= 3) {                       // OpName: id, literal string
                    names[spv[i+1]] = i + 2;                    // remember string word offset
                } else if (op == 6 && wc >= 4) {               // OpMemberName: struct, member, str
                    memName[{spv[i+1],spv[i+2]}] = std::string((const char*)(spv.data()+i+3));
                } else if (op == 72 && wc >= 5 && spv[i+3] == 35) { // OpMemberDecorate Offset
                    memOff[{spv[i+1],spv[i+2]}] = spv[i+4];
                } else if (op == 71 && wc >= 4) {              // OpDecorate
                    u32 t = spv[i+1], dec = spv[i+2];
                    if (dec == 34) dset[t] = spv[i+3];          // DescriptorSet
                    else if (dec == 33) dbind[t] = spv[i+3];    // Binding
                } else if (op == 71 && wc >= 3 && spv[i+2] == 3) {
                    bufferBlockTypes.insert(spv[i+1]);          // OpDecorate <struct> BufferBlock -> SSBO
                } else if (op == 32 && wc >= 4) {              // OpTypePointer
                    ptr[spv[i+1]] = {spv[i+2], spv[i+3]};
                } else if (op == 30 && wc >= 2) {              // OpTypeStruct: result, member_type_ids...
                    std::vector<u32> mt; for (u32 k = 2; k < wc; ++k) mt.push_back(spv[i+k]);
                    structMems[spv[i+1]] = std::move(mt);
                } else if (op == 23 && wc >= 4) {              // OpTypeVector: result, component_type, count
                    vecCount[spv[i+1]] = spv[i+3];
                } else if ((op == 28 || op == 29) && wc >= 3) { // OpTypeArray / OpTypeRuntimeArray
                    structOfArray[spv[i+1]] = spv[i+2];         // element type
                } else if (op == 25) typeKind[spv[i+1]] = 2;    // OpTypeImage
                else if (op == 26) typeKind[spv[i+1]] = 1;      // OpTypeSampler
                else if (op == 27) typeKind[spv[i+1]] = 2;      // OpTypeSampledImage
                else if (op == 59 && wc >= 4) varType[spv[i+2]] = spv[i+1]; // OpVariable
                i += wc;
            }
            auto strOf = [&](u32 vid) -> std::string {
                auto it = names.find(vid); if (it == names.end()) return "";
                return std::string((const char*)(spv.data() + it->second));
            };
            // Capture matParams struct members (name+offset) once, from whichever stage
            // declares the set2 UBO named "matParams".
            if (matParamsMembers.empty()) {
                for (auto& [vid, ptrTypeId] : varType) {
                    if (strOf(vid) != "matParams") continue;
                    auto pit = ptr.find(ptrTypeId); if (pit == ptr.end()) continue;
                    u32 structId = pit->second.second;
                    for (u32 m = 0; m < 64; ++m) {
                        auto nIt = memName.find({structId,m}); if (nIt == memName.end()) break;
                        u32 off = memOff.count({structId,m}) ? memOff[{structId,m}] : 0;
                        u32 mvsz = 4;                            // value byte-size from the member's SPIR-V type
                        auto smIt = structMems.find(structId);
                        if (smIt != structMems.end() && m < smIt->second.size()) {
                            auto vcIt = vecCount.find(smIt->second[m]);
                            if (vcIt != vecCount.end()) mvsz = vcIt->second * 4;   // vecN -> N*4 bytes (scalar stays 4)
                        }
                        matParamsMembers.push_back({nIt->second, off, mvsz});
                    }
                    break;
                }
            }
            // One-time: dump lightUniforms (set1) member name@offset — to confirm numLights's TRUE offset
            // (the createLightingResources hardcode was a guess). HSR_LIGHTDBG=1.
            if (std::getenv("HSR_LIGHTDBG")) {
                for (auto& [vid, ptrTypeId] : varType) {
                    if (strOf(vid) != "lightUniforms") continue;
                    auto pit = ptr.find(ptrTypeId); if (pit == ptr.end()) continue;
                    u32 structId = pit->second.second;
                    for (u32 m = 0; m < 96; ++m) {
                        auto nIt = memName.find({structId,m}); if (nIt == memName.end()) break;
                        u32 off = memOff.count({structId,m}) ? memOff[{structId,m}] : 0;
                        log("  [LIGHTUBO] m%u %s @ %u", m, nIt->second.c_str(), off);
                    }
                    break;
                }
            }
            for (auto& [vid, ptrTypeId] : varType) {
                auto sIt = dset.find(vid); auto bIt = dbind.find(vid);
                if (sIt == dset.end() || bIt == dbind.end()) continue;
                u32 set = sIt->second, binding = bIt->second;
                if (set > 2) continue;
                int kind = 3; bool isStorage = false;
                auto pit = ptr.find(ptrTypeId);
                if (pit != ptr.end()) {
                    u32 storage = pit->second.first, pointee = pit->second.second;
                    if (storage == 2 || storage == 12) {
                        kind = 0;
                        // SPIR-V 1.0 SSBOs are storage class Uniform(2) + BufferBlock
                        // decoration on the struct; SPIR-V 1.3+ uses StorageBuffer(12).
                        isStorage = (storage == 12) || bufferBlockTypes.count(pointee) > 0;
                    } else {
                        auto tk = typeKind.find(pointee);
                        kind = (tk != typeKind.end()) ? tk->second : 3;
                    }
                }
                auto key = std::make_pair(set, binding);
                auto ex = all.find(key);
                std::string nm = strOf(vid);
                if (ex == all.end()) all[key] = {binding, kind, nm, 0, isStorage};
                else {
                    if (ex->second.type == 3 && kind != 3) { ex->second.type = kind; ex->second.isStorage = isStorage; }
                    if (ex->second.name.empty() && !nm.empty()) ex->second.name = nm;
                }
            }
        }
        for (auto& [key, db] : all) {
            if (key.first == 0) set0Binds.push_back(db);
            else if (key.first == 1) set1Binds.push_back(db);
            else set2Binds.push_back(db);
        }
        auto bySlot = [](const DescBind&a,const DescBind&b){return a.binding<b.binding;};
        std::sort(set0Binds.begin(), set0Binds.end(), bySlot);
        std::sort(set1Binds.begin(), set1Binds.end(), bySlot);
        std::sort(set2Binds.begin(), set2Binds.end(), bySlot);
        // Base-color texture = the set2 image whose name mentions basecolor/albedo, else
        // the first image binding.
        for (auto& d : set2Binds) {
            std::string n = d.name; for (auto& c : n) c = (char)tolower((unsigned char)c);
            if (d.type == 2 && (n.find("basecolor") != std::string::npos || n.find("albedo") != std::string::npos)) {
                set2BaseColorBinding = (int)d.binding; break;
            }
        }
        if (set2BaseColorBinding < 0)
            for (auto& d : set2Binds) if (d.type == 2) { set2BaseColorBinding = (int)d.binding; break; }
        if (set2BaseColorBinding < 0) set2BaseColorBinding = 1;
        auto kindStr = [](int t){ return t==0?"BUF":t==1?"SAMPLER":t==2?"IMAGE":"other"; };
        log("  introspected sets: set0=%zu set1=%zu set2=%zu, baseColor@set2.bind%d",
            set0Binds.size(), set1Binds.size(), set2Binds.size(), set2BaseColorBinding);
        for (auto& d : set1Binds) log("    set1 bind%u %-7s %s", d.binding, kindStr(d.type), d.name.c_str());
        for (auto& d : set2Binds) log("    set2 bind%u %-7s %s", d.binding, kindStr(d.type), d.name.c_str());
    }

    void createDescriptorSetLayout() {
        introspectVertexInputs(vertSpirv);
        introspectSet2(vertSpirv, fragSpirv);
        // Set 0: globalUniforms (UBO, vert+frag) + linearWrapSampler (SAMPLER, frag)
        {
            VkDescriptorSetLayoutBinding binds[2] = {};
            binds[0].binding = 0;
            binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binds[0].descriptorCount = 1;
            binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            binds[1].binding = 1;
            binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            binds[1].descriptorCount = 1;
            binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = 2;
            info.pBindings = binds;
            vkCreateDescriptorSetLayout(device, &info, nullptr, &descSetLayouts[0]);
        }

        // Helper: build a set layout from an introspected binding list. UBO storage
        // class -> UNIFORM_BUFFER (or STORAGE_BUFFER if SSBO), sampler -> SAMPLER,
        // image -> SAMPLED_IMAGE. Empty list -> empty (valid placeholder) layout.
        auto buildSetLayout = [&](const std::vector<DescBind>& bl, VkDescriptorSetLayout& out) {
            std::vector<VkDescriptorSetLayoutBinding> binds;
            for (auto& d : bl) {
                VkDescriptorSetLayoutBinding b = {};
                b.binding = d.binding;
                b.descriptorType = d.type==0 ? (d.isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                            : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                                 : d.type==1 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                b.descriptorCount = 1;
                b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                binds.push_back(b);
            }
            VkDescriptorSetLayoutCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = (u32)binds.size();
            info.pBindings = binds.empty() ? nullptr : binds.data();
            VkResult r = vkCreateDescriptorSetLayout(device, &info, nullptr, &out);
            if (r != VK_SUCCESS) log("  ERROR: descriptor set layout create failed: %d (%u binds)", r, (u32)binds.size());
        };

        // Set 1: the PBR shaders' lighting set (lightUniforms, sbLightItems, IBL diffuse +
        // reflection cubemaps, shadowmap sampler + texture). For unlit shaders set1Binds is
        // empty -> empty placeholder (so set 2 still binds at index 2).
        hasSet1 = !set1Binds.empty();
        buildSetLayout(set1Binds, descSetLayouts[1]);

        // Set 2: matParams + lightprobesParams + lightBakerParams + samplers + lightmap +
        // BaseColorMetallic_Tx + ONxRNy_Tx (introspected; bound by name per-material).
        {
            if (set2Binds.empty()) {  // fallback to classic nuxd layout
                set2Binds = {{0,0,"matParams"},{1,2,"baseColorTex"}};
                set2BaseColorBinding = 1;
            }
            buildSetLayout(set2Binds, descSetLayouts[2]);
        }

        // Set 2 (skinned variant): matParams UBO + sbSkinningMatrices STORAGE buffer + baseColorTex
        {
            VkDescriptorSetLayoutBinding binds[3] = {};
            binds[0].binding = 0;
            binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binds[0].descriptorCount = 1;
            binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            binds[1].binding = 1;
            binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  // sbSkinningMatrices (BufferBlock)
            binds[1].descriptorCount = 1;
            binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            binds[2].binding = 2;
            binds[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binds[2].descriptorCount = 1;
            binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = 3;
            info.pBindings = binds;
            vkCreateDescriptorSetLayout(device, &info, nullptr, &descSetLayout2Skin);
        }

        createDescriptorPool(2048);
        sharedSampler = createTextureSampler();
    }

    void createDescriptorPool(u32 maxSets) {
        if (descPool) vkDestroyDescriptorPool(device, descPool, nullptr);
        // PBR set2 per mesh: up to 4 UBOs (global + matParams + lightprobesParams +
        // lightBakerParams) + 2 SAMPLERs (aniso4x/2x) + 4 SAMPLED_IMAGEs (lightmap +
        // base + normal + spare). Plus the shared set1 (lightUniforms UBO, sbLightItems
        // SSBO, 2 IBL cubemaps + shadow image, shadow sampler). Be generous.
        VkDescriptorPoolSize poolSizes[5] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = maxSets * 5;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[1].descriptorCount = maxSets * 3;   // set0 + set2 aniso samplers
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSizes[2].descriptorCount = maxSets * 6;   // base + normal + lightmap + IBL + shadow
        poolSizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // spare
        poolSizes[3].descriptorCount = 16;
        poolSizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  // sbSkinningMatrices + sbLightItems
        poolSizes[4].descriptorCount = maxSets + 8;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 5;
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = maxSets * 3 + 16;  // 3 desc sets per skinned mesh (set0 + set2 unlit + set2 skin)
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    }

    // ── Per-material program machinery (HSR_PERMAT) ─────────────────────────────────────────────
    VkDescriptorSetLayout makeSetLayout(const std::vector<DescBind>& bl) {
        std::vector<VkDescriptorSetLayoutBinding> binds;
        for (auto& d : bl) {
            VkDescriptorSetLayoutBinding b = {};
            b.binding = d.binding;
            b.descriptorType = d.type==0 ? (d.isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                             : d.type==1 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            binds.push_back(b);
        }
        VkDescriptorSetLayoutCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = (u32)binds.size();
        info.pBindings = binds.empty() ? nullptr : binds.data();
        VkDescriptorSetLayout out = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device, &info, nullptr, &out);
        return out;
    }

    // Build this program's pipelines (opaque/blend/cull/blendCull/additive) from its own vert/frag +
    // vertex layout + descriptor layouts. Mirrors createGraphicsPipeline's state (reversed-Z, push const).
    void makeProgramPipelines(ShaderProgram& p) {
        VkShaderModule vm = createShaderModule(p.vert), fm = createShaderModule(p.frag);
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fm; stages[1].pName = "main";
        VkVertexInputBindingDescription bind = { 0, p.vertStride, VK_VERTEX_INPUT_RATE_VERTEX };
        std::vector<VkVertexInputAttributeDescription> attrs;
        for (auto& v : p.vertInputs) { VkVertexInputAttributeDescription a={}; a.location=v.location; a.binding=0; a.format=v.fmt; a.offset=v.offset; attrs.push_back(a); }
        VkPipelineVertexInputStateCreateInfo vi={}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
        vi.vertexAttributeDescriptionCount=(u32)attrs.size(); vi.pVertexAttributeDescriptions=attrs.empty()?nullptr:attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia={}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkDynamicState dyn[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dd={}; dd.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dd.dynamicStateCount=2; dd.pDynamicStates=dyn;
        VkPipelineViewportStateCreateInfo vps={}; vps.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount=1; vps.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs={}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.f;
        VkPipelineRasterizationStateCreateInfo rsCull=rs; rsCull.cullMode=singleSidedCull;
        VkPipelineMultisampleStateCreateInfo ms={}; ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds={}; ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineDepthStencilStateCreateInfo dsNoW=ds; dsNoW.depthWriteEnable=VK_FALSE;
        VkPipelineColorBlendAttachmentState cbO={}; cbO.blendEnable=VK_FALSE; cbO.colorWriteMask=0xF;
        VkPipelineColorBlendAttachmentState cbB=cbO; cbB.blendEnable=VK_TRUE; cbB.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; cbB.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cbB.colorBlendOp=VK_BLEND_OP_ADD; cbB.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; cbB.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cbB.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendAttachmentState cbA=cbB; cbA.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; cbA.dstColorBlendFactor=VK_BLEND_FACTOR_ONE; cbA.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; cbA.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        VkPipelineColorBlendStateCreateInfo cbIO={}; cbIO.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbIO.attachmentCount=1; cbIO.pAttachments=&cbO;
        VkPipelineColorBlendStateCreateInfo cbIB=cbIO; cbIB.pAttachments=&cbB;
        VkPipelineColorBlendStateCreateInfo cbIA=cbIO; cbIA.pAttachments=&cbA;
        VkDescriptorSetLayout sl[3]={ descSetLayouts[0], p.layout1?p.layout1:descSetLayouts[1], p.layout2 };
        VkPushConstantRange pc={ VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80 };
        VkPipelineLayoutCreateInfo pl={}; pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount=3; pl.pSetLayouts=sl; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&pc;
        vkCreatePipelineLayout(device,&pl,nullptr,&p.pipeLayout);
        VkGraphicsPipelineCreateInfo gp={}; gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia; gp.pViewportState=&vps; gp.pMultisampleState=&ms; gp.pDynamicState=&dd; gp.layout=p.pipeLayout; gp.renderPass=renderPass; gp.subpass=0;
        gp.pRasterizationState=&rs;     gp.pDepthStencilState=&ds;   gp.pColorBlendState=&cbIO; vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipe);
        gp.pRasterizationState=&rs;     gp.pDepthStencilState=&dsNoW;gp.pColorBlendState=&cbIB; vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipeBlend);
        gp.pRasterizationState=&rsCull; gp.pDepthStencilState=&ds;   gp.pColorBlendState=&cbIO; vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipeCull);
        gp.pRasterizationState=&rsCull; gp.pDepthStencilState=&dsNoW;gp.pColorBlendState=&cbIB; vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipeBlendCull);
        gp.pRasterizationState=&rs;     gp.pDepthStencilState=&dsNoW;gp.pColorBlendState=&cbIA; vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipeAdditive);
        // Wireframe variant on THIS program's layout — so the editor "vertex structure" (F) + selection
        // highlight can draw per-material meshes without binding their descSet2 to the global-layout
        // wireframe pipeline (that mismatch HANGS the GPU on HSL — the click-freeze).
        { VkPipelineRasterizationStateCreateInfo rsWire=rs; rsWire.polygonMode=VK_POLYGON_MODE_LINE; rsWire.cullMode=VK_CULL_MODE_NONE;
          gp.pRasterizationState=&rsWire; gp.pDepthStencilState=&ds; gp.pColorBlendState=&cbIO;
          vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&p.pipeWire); }
        p.pipeAlphaTest = p.pipe;   // no per-program discard frag; cutouts draw opaque in per-mat path
        vkDestroyShaderModule(device,vm,nullptr); vkDestroyShaderModule(device,fm,nullptr);
    }

    // Introspect a program's SPIR-V (via the existing global introspectors + save/restore of the
    // global scratch), build its descriptor layouts + pipelines. set1 reuses the synthesized lighting.
    void buildProgram(ShaderProgram& p) {
        auto sVI=vertInputs; auto sVS=vertStride; auto s1=set1Binds; auto s2=set2Binds; auto s0=set0Binds;
        auto sM=matParamsMembers; auto sB=set2BaseColorBinding; auto sH=hasSet1;
        introspectVertexInputs(p.vert);
        introspectSet2(p.vert, p.frag);
        p.vertInputs=vertInputs; p.vertStride=vertStride; p.set1Binds=set1Binds; p.set2Binds=set2Binds;
        p.matParamsMembers=matParamsMembers; p.set2BaseColorBinding=set2BaseColorBinding; p.hasSet1=hasSet1;
        vertInputs=sVI; vertStride=sVS; set1Binds=s1; set2Binds=s2; set0Binds=s0; matParamsMembers=sM; set2BaseColorBinding=sB; hasSet1=sH;
        if (p.vertStride==0) p.vertStride=32;
        if (p.set2Binds.empty()) { p.set2Binds.push_back({0,0,"matParams"}); p.set2Binds.push_back({1,2,"baseColorTex"}); p.set2BaseColorBinding=1; }
        p.layout1 = p.set1Binds.empty() ? descSetLayouts[1] : makeSetLayout(p.set1Binds);
        p.layout2 = makeSetLayout(p.set2Binds);
        makeProgramPipelines(p);
        p.set1Desc = sharedSet1;
        log("  [PERMAT] program '%s': set1=%zu set2=%zu vstride=%u base@%d", p.surface.c_str(),
            p.set1Binds.size(), p.set2Binds.size(), p.vertStride, p.set2BaseColorBinding);
    }

    // Lazily get/build the program for a material's ".surface". -1 => not available (use global path).
    // MurmurHash3 of every sampler-like name (…_Tx / …Map / …Sampler / …Cube) a fragment declares.
    // The material's textureParameters name its slots by the same hash, so this lets us pick the variant
    // whose samplers MATCH the material (instead of the largest übershader the builder defaulted to).
    static std::vector<u32> fragSamplerHashes(const std::vector<u32>& spv) {
        std::vector<u32> out; u32 n=(u32)spv.size(), i=5;
        while (i < n) { u32 w=spv[i], op=w&0xFFFF, wc=w>>16; if (wc==0 || i+wc>n) break;
            if (op==5 && wc>=3) {                                   // OpName: id, literal string
                std::string s; for (u32 k=i+2;k<i+wc;++k){ u32 ww=spv[k]; bool done=false;
                    for (int b=0;b<4;++b){ char c=(char)((ww>>(b*8))&0xFF); if(!c){done=true;break;} s+=c; } if(done)break; }
                if (s.find("_Tx")!=std::string::npos || s.find("Map")!=std::string::npos ||
                    s.find("Sampler")!=std::string::npos || s.find("Cube")!=std::string::npos)
                    out.push_back(murmur3_x86_32(s.data(), s.size(), 0));
            }
            i += wc;
        }
        return out;
    }
    // True if a fragment declares the set1 LIGHTING resources (IBL/shadow/light list). Such a "lit"
    // variant multiplies albedo by scene lighting we can't fully feed (synthesised ambient only) ->
    // dark/black. For these surfaces the intended variant is the UNLIT colour one (no set1).
    static bool fragHasLighting(const std::vector<u32>& spv) {
        u32 n=(u32)spv.size(), i=5;
        while (i<n) { u32 w=spv[i], op=w&0xFFFF, wc=w>>16; if (wc==0 || i+wc>n) break;
            if (op==5 && wc>=3) {
                std::string s; for (u32 k=i+2;k<i+wc;++k){ u32 ww=spv[k]; bool d=false;
                    for(int b=0;b<4;++b){char c=(char)((ww>>(b*8))&0xFF); if(!c){d=true;break;} s+=(char)tolower((unsigned char)c);} if(d)break; }
                if (s.find("globalibl")!=std::string::npos || s.find("shadowmap")!=std::string::npos ||
                    s.find("lightuniforms")!=std::string::npos || s.find("lightprobe")!=std::string::npos ||
                    s.find("sblightitems")!=std::string::npos) return true;
            }
            i += wc;
        }
        return false;
    }
    int programForSurface(const std::string& surface, const std::vector<u32>* texSlots=nullptr) {
        if (surface.empty()) return -1;
        for (int i=0;i<(int)programs.size();++i) if (programs[i].surface==surface) return i;
        // Among ALL variants of this surface, score by: (1) most samplers matching the material's
        // textureParameters slots, (2) prefer UNLIT (no set1 lighting we can't feed), (3) largest
        // (the full colour shader vs depth/shadow prepasses).
        int bestLs=-1; long bestScore=-1;
        for (int li=0; li<(int)loadedShaders.size(); ++li) {
            auto& ls=loadedShaders[li];
            if (ls.surface!=surface || ls.vert.empty() || ls.frag.empty()) continue;
            int sc=0;
            if (texSlots && !texSlots->empty()) {
                for (u32 h : fragSamplerHashes(ls.frag))
                    if (std::find(texSlots->begin(), texSlots->end(), h) != texSlots->end()) ++sc;
            }
            // Prefer the LIT (forward) variant now that we FEED real lighting (distant key light +
            // distantLightDir/RadiantIntensity in lightUniforms + IBL). Previously this preferred UNLIT
            // because lighting wasn't fed → candle/PBR rendered flat/unlit. (HSR_UNLITVARIANT reverts.)
            bool preferUnlit = std::getenv("HSR_UNLITVARIANT") != nullptr;
            long score = (long)sc * 100000000L
                       + ((fragHasLighting(ls.frag) != preferUnlit) ? 50000000L : 0L)
                       + (long)ls.frag.size();
            if (score > bestScore) { bestScore=score; bestLs=li; }
        }
        int bestFragSz = bestLs>=0 ? (int)loadedShaders[bestLs].frag.size() : 0; (void)bestFragSz;
        if (bestLs < 0) return -1;
        auto& ls=loadedShaders[bestLs];
        ShaderProgram p; p.surface=surface; p.vert=ls.vert; p.frag=ls.frag;
        buildProgram(p); programs.push_back(std::move(p));
        if (std::getenv("HSR_MATDBG"))
            log("  [PERMAT] surface '%s' picked variant %d (sampler match=%d, %zu candidates)",
                surface.c_str(), bestLs, bestScore, loadedShaders.size());
        return (int)programs.size()-1;
    }
    // The skinned program variant (surface name contains "skinned", e.g. nuxd's unlitblendskinned) —
    // bone meshes must use it (vstride 32 + sbSkinningMatrices) instead of the non-skinned default.
    int programForSkinned() {
        auto lc=[](std::string s){ for(auto&c:s)c=(char)tolower((unsigned char)c); return s; };
        for (int i=0;i<(int)programs.size();++i) if (lc(programs[i].surface).find("skinned")!=std::string::npos) return i;
        for (auto& ls : loadedShaders) if (lc(ls.surface).find("skinned")!=std::string::npos && !ls.vert.empty() && !ls.frag.empty())
            return programForSurface(ls.surface);
        return -1;
    }

    void createGraphicsPipeline() {
        if (const char* cm = std::getenv("HSR_CULLMODE")) {
            if (std::string(cm) == "front") singleSidedCull = VK_CULL_MODE_FRONT_BIT;
            else if (std::string(cm) == "none") singleSidedCull = VK_CULL_MODE_NONE;
            else singleSidedCull = VK_CULL_MODE_BACK_BIT;
        }
        VkShaderModule vertMod = createShaderModule(vertSpirv);
        VkShaderModule fragMod = createShaderModule(fragSpirv);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";

        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";

        // Vertex input: built from the chosen shader's introspected inputs (per-shader
        // layout — matches whatever the env's vertex shader declares).
        VkVertexInputBindingDescription bindingDesc = {};
        bindingDesc.binding = 0;
        bindingDesc.stride = vertStride;
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attrDescs;
        for (auto& v : vertInputs) {
            VkVertexInputAttributeDescription a = {};
            a.binding = 0; a.location = v.location; a.format = v.fmt; a.offset = v.offset;
            attrDescs.push_back(a);
        }

        VkPipelineVertexInputStateCreateInfo viInfo = {};
        viInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        viInfo.vertexBindingDescriptionCount = 1;
        viInfo.pVertexBindingDescriptions = &bindingDesc;
        viInfo.vertexAttributeDescriptionCount = (u32)attrDescs.size();
        viInfo.pVertexAttributeDescriptions = attrDescs.empty() ? nullptr : attrDescs.data();

        VkPipelineInputAssemblyStateCreateInfo iaInfo = {};
        iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Viewport and scissor are dynamic — updated each frame so resize works without pipeline rebuild
        VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynInfo = {};
        dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynInfo.dynamicStateCount = 2;
        dynInfo.pDynamicStates = dynStates;

        VkPipelineViewportStateCreateInfo vpInfo = {};
        vpInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vpInfo.viewportCount = 1;  // count required even with dynamic state
        vpInfo.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rsInfo = {};
        rsInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rsInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rsInfo.cullMode = VK_CULL_MODE_NONE;
        rsInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsInfo.lineWidth = 1.0f;

        VkPipelineRasterizationStateCreateInfo rsWireInfo = rsInfo;
        rsWireInfo.polygonMode = VK_POLYGON_MODE_LINE;

        VkPipelineMultisampleStateCreateInfo msInfo = {};
        msInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dsInfo = {};
        dsInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dsInfo.depthTestEnable = VK_TRUE;
        dsInfo.depthWriteEnable = VK_TRUE;
        dsInfo.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed-Z (clear 0, near->1)

        // Opaque pass: textures author alpha=0 (used as material mask, not transparency).
        // libshell renders the dome/floor opaquely — matching that here.
        // Motes use additive blending but are handled by a separate pipeline in libshell.
        VkPipelineColorBlendAttachmentState cbAtt = {};
        cbAtt.blendEnable = VK_FALSE;
        cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cbInfo = {};
        cbInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbInfo.attachmentCount = 1;
        cbInfo.pAttachments = &cbAtt;

        // Push constant range: worldFromModel (mat4=64B) + color (vec4=16B) = 80 bytes
        VkPushConstantRange pcRange = {};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = 80;

        // 3 set layouts: indices 0, 1(empty), 2 — required so set 2 binds at index 2
        VkPipelineLayoutCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 3;
        plInfo.pSetLayouts = descSetLayouts;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &pcRange;

        VkResult layoutRes = vkCreatePipelineLayout(device, &plInfo, nullptr, &pipelineLayout);
        if (layoutRes != VK_SUCCESS) log("ERROR: vkCreatePipelineLayout failed: %d", layoutRes);

        VkGraphicsPipelineCreateInfo pipeInfo = {};
        pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeInfo.stageCount = 2;
        pipeInfo.pStages = stages;
        pipeInfo.pVertexInputState = &viInfo;
        pipeInfo.pInputAssemblyState = &iaInfo;
        pipeInfo.pViewportState = &vpInfo;
        pipeInfo.pRasterizationState = &rsInfo;
        pipeInfo.pMultisampleState = &msInfo;
        pipeInfo.pDepthStencilState = &dsInfo;
        pipeInfo.pColorBlendState = &cbInfo;
        pipeInfo.pDynamicState = &dynInfo;
        pipeInfo.layout = pipelineLayout;
        pipeInfo.renderPass = renderPass;
        pipeInfo.subpass = 0;

        VkResult pipeRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &graphicsPipeline);
        if (pipeRes != VK_SUCCESS) {
            log("ERROR: vkCreateGraphicsPipelines FAILED: %d — graphicsPipeline is null, nothing will render!", pipeRes);
            // Isolate which stage the driver rejects: try a vertex-only pipeline
            // (rasterizerDiscard) and a fragment-only-ish probe.
            VkPipelineRasterizationStateCreateInfo rsDiscard = rsInfo;
            rsDiscard.rasterizerDiscardEnable = VK_TRUE;
            VkGraphicsPipelineCreateInfo vtest = pipeInfo;
            vtest.stageCount = 1;            // vertex stage only
            vtest.pStages = &stages[0];
            vtest.pRasterizationState = &rsDiscard;
            vtest.pColorBlendState = nullptr;
            vtest.pDepthStencilState = nullptr;
            VkPipeline vp = VK_NULL_HANDLE;
            VkResult vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &vtest, nullptr, &vp);
            log("  [diag] vertex-only pipeline result: %d  (%s)", vr, vr==VK_SUCCESS?"VERTEX OK -> fragment is the problem":"VERTEX rejected");
            if (vp) vkDestroyPipeline(device, vp, nullptr);
        } else
            log("  Graphics pipeline (fill) created OK");

        // Wireframe pipeline
        pipeInfo.pRasterizationState = &rsWireInfo;
        VkResult wireRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &wireframePipeline);
        if (wireRes != VK_SUCCESS)
            log("WARN: wireframe pipeline creation failed: %d (fillModeNonSolid may not be supported)", wireRes);
        else
            log("  Wireframe pipeline created OK");

        // Blend pipeline: SRC_ALPHA for dome panels and particles (no depth write)
        {
            VkPipelineDepthStencilStateCreateInfo dsBlend = dsInfo;
            dsBlend.depthWriteEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState cbBlendAtt = {};
            cbBlendAtt.blendEnable = VK_TRUE;
            cbBlendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cbBlendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbBlendAtt.colorBlendOp = VK_BLEND_OP_ADD;
            cbBlendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cbBlendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbBlendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
            cbBlendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo cbBlendInfo = cbInfo;
            cbBlendInfo.pAttachments = &cbBlendAtt;

            pipeInfo.pRasterizationState = &rsInfo;
            pipeInfo.pDepthStencilState = &dsBlend;
            pipeInfo.pColorBlendState = &cbBlendInfo;

            VkResult blendRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &blendPipeline);
            if (blendRes != VK_SUCCESS)
                log("WARN: blend pipeline creation failed: %d", blendRes);
            else
                log("  Blend pipeline created OK");

            // Back-face-cull variants (single-sided glTF materials, incl. cel-outline inverted hull).
            // Faithful to libshell: doubleSided=false => cull. frontFace stays CCW (glTF convention).
            VkPipelineRasterizationStateCreateInfo rsCull = rsInfo;
            rsCull.cullMode = singleSidedCull;     // BACK by default; HSR_CULLMODE=front for testing
            // opaque cull variant
            pipeInfo.pRasterizationState = &rsCull;
            pipeInfo.pDepthStencilState  = &dsInfo;
            pipeInfo.pColorBlendState    = &cbInfo;
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &graphicsPipelineCull) != VK_SUCCESS)
                log("WARN: opaque cull pipeline creation failed");
            else log("  Opaque (cull) pipeline created OK");
            // blend cull variant
            pipeInfo.pRasterizationState = &rsCull;
            pipeInfo.pDepthStencilState  = &dsBlend;
            pipeInfo.pColorBlendState    = &cbBlendInfo;
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &blendPipelineCull) != VK_SUCCESS)
                log("WARN: blend cull pipeline creation failed");
            else log("  Blend (cull) pipeline created OK");

            // ADDITIVE pipeline (libshell material Additive:true — god-rays/light-shafts/glow):
            // dst.rgb += src.rgb (black texels add nothing, the bright shaft adds light). No depth
            // write. Faithful to "Additive" blend; fixes the dark-rectangle godrays in storybook.
            VkPipelineColorBlendAttachmentState cbAddAtt = cbBlendAtt;
            // Additive light effects (godrays, caustics, glow) ADD their light scaled by COVERAGE:
            // result.rgb = src.rgb * src.a + dst.rgb. The caustics texture is solid white with the
            // pattern only in its ALPHA (mean ~6/255) — with srcFactor=ONE it added full white
            // EVERYWHERE (solid white blobs over the floor). SRC_ALPHA premultiplies by coverage so
            // it adds a faint caustic. Godrays (black bg / alpha-shaped) work the same either way.
            cbAddAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cbAddAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cbAddAtt.colorBlendOp        = VK_BLEND_OP_ADD;
            cbAddAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cbAddAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            VkPipelineColorBlendStateCreateInfo cbAddInfo = cbInfo;
            cbAddInfo.pAttachments = &cbAddAtt;
            pipeInfo.pRasterizationState = &rsInfo;
            pipeInfo.pDepthStencilState  = &dsBlend;     // no depth write
            pipeInfo.pColorBlendState    = &cbAddInfo;
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &additivePipeline) != VK_SUCCESS)
                log("WARN: additive pipeline creation failed");
            else log("  Additive pipeline created OK");

            // restore for any later pipeline that reuses pipeInfo
            pipeInfo.pRasterizationState = &rsInfo;
            pipeInfo.pDepthStencilState  = &dsInfo;
            pipeInfo.pColorBlendState    = &cbInfo;
        }

        // Alpha-test (cutout) pipeline: same as opaque (depth-test + depth-WRITE on, no color blend)
        // but with a fragment shader that discards texels below the alpha threshold. Lets hard-edged
        // cutouts (flags/foliage/animals — materials with AlphaTest:true) occlude correctly instead of
        // being routed to the no-depth-write blend pass. Faithful to how libshell handles AlphaTest.
        VkShaderModule fragAlphaMod = VK_NULL_HANDLE;
        if (!alphaTestFragSpirv.empty()) {
            fragAlphaMod = createShaderModule(alphaTestFragSpirv);
            VkPipelineShaderStageCreateInfo atStages[2] = { stages[0], stages[1] };
            atStages[1].module = fragAlphaMod;
            VkGraphicsPipelineCreateInfo atInfo = pipeInfo;
            atInfo.pStages = atStages;
            atInfo.pRasterizationState = &rsInfo;      // fill
            atInfo.pDepthStencilState = &dsInfo;       // depth test + WRITE on (occludes)
            atInfo.pColorBlendState = &cbInfo;         // no blend (cbAtt)
            VkResult atRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &atInfo, nullptr, &alphaTestPipeline);
            if (atRes != VK_SUCCESS)
                log("WARN: alpha-test pipeline creation failed: %d", atRes);
            else
                log("  Alpha-test (cutout) pipeline created OK");
        }

        vkDestroyShaderModule(device, vertMod, nullptr);
        vkDestroyShaderModule(device, fragMod, nullptr);
        if (fragAlphaMod) vkDestroyShaderModule(device, fragAlphaMod, nullptr);

        // ── Skinned pipeline ─────────────────────────────────────────────────────
        // Only create if both skinned shaders were provided.
        if (!skinnedVertSpirv.empty() && !skinnedFragSpirv.empty()
            && descSetLayout2Skin != VK_NULL_HANDLE) {

            VkShaderModule sVertMod = createShaderModule(skinnedVertSpirv);
            VkShaderModule sFragMod = createShaderModule(skinnedFragSpirv);

            if (sVertMod != VK_NULL_HANDLE && sFragMod != VK_NULL_HANDLE) {
                // Pipeline layout: set0 (descSetLayouts[0]) + set1 empty (descSetLayouts[1])
                //                  + set2 skinned (descSetLayout2Skin)
                VkDescriptorSetLayout skinLayouts[3] = {
                    descSetLayouts[0], descSetLayouts[1], descSetLayout2Skin
                };
                VkPushConstantRange skinPcRange = {};
                skinPcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                skinPcRange.offset = 0;
                skinPcRange.size = 80;

                VkPipelineLayoutCreateInfo skinPlInfo = {};
                skinPlInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                skinPlInfo.setLayoutCount = 3;
                skinPlInfo.pSetLayouts = skinLayouts;
                skinPlInfo.pushConstantRangeCount = 1;
                skinPlInfo.pPushConstantRanges = &skinPcRange;
                VkResult skinLayoutRes = vkCreatePipelineLayout(device, &skinPlInfo, nullptr, &skinnedPipelineLayout);
                if (skinLayoutRes != VK_SUCCESS) {
                    log("WARN: skinnedPipelineLayout creation failed: %d", skinLayoutRes);
                } else {
                    VkPipelineShaderStageCreateInfo skinStages[2] = {};
                    skinStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    skinStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                    skinStages[0].module = sVertMod;
                    skinStages[0].pName = "main";
                    skinStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    skinStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                    skinStages[1].module = sFragMod;
                    skinStages[1].pName = "main";

                    // Reuse the existing pipeline info structs (viInfo, iaInfo, vpInfo,
                    // dynInfo, msInfo, dsInfo, cbInfo) from the unlit pipeline above —
                    // they are still in scope.
                    // Skinned uses the same blend state as the regular blend pipeline.
                    VkPipelineDepthStencilStateCreateInfo dsSkinBlend = dsInfo;
                    dsSkinBlend.depthWriteEnable = VK_FALSE;

                    VkPipelineColorBlendAttachmentState cbSkinAtt = {};
                    cbSkinAtt.blendEnable = VK_TRUE;
                    // The V79 OPA cook of this exact env renders prism_wave with ALPHA blend (mdBlend=1,
                    // add=0) as a clean iridescent disc. Match it: standard SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
                    // (The v203 cook's 5020-vert SKINNED ring mesh is the real cause of the "borked" look;
                    // the proper fix is re-cooking the clean V79 prism into v203.)
                    cbSkinAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    cbSkinAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbSkinAtt.colorBlendOp = VK_BLEND_OP_ADD;
                    cbSkinAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cbSkinAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    cbSkinAtt.alphaBlendOp = VK_BLEND_OP_ADD;
                    cbSkinAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                    VkPipelineColorBlendStateCreateInfo cbSkinInfo = cbInfo;
                    cbSkinInfo.pAttachments = &cbSkinAtt;

                    VkGraphicsPipelineCreateInfo skinPipeInfo = {};
                    skinPipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                    skinPipeInfo.stageCount = 2;
                    skinPipeInfo.pStages = skinStages;
                    skinPipeInfo.pVertexInputState = &viInfo;
                    skinPipeInfo.pInputAssemblyState = &iaInfo;
                    skinPipeInfo.pViewportState = &vpInfo;
                    skinPipeInfo.pRasterizationState = &rsInfo;
                    skinPipeInfo.pMultisampleState = &msInfo;
                    skinPipeInfo.pDepthStencilState = &dsSkinBlend;
                    skinPipeInfo.pColorBlendState = &cbSkinInfo;
                    skinPipeInfo.pDynamicState = &dynInfo;
                    skinPipeInfo.layout = skinnedPipelineLayout;
                    skinPipeInfo.renderPass = renderPass;
                    skinPipeInfo.subpass = 0;

                    VkResult skinPipeRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                                      &skinPipeInfo, nullptr, &skinnedPipeline);
                    if (skinPipeRes != VK_SUCCESS)
                        log("WARN: skinnedPipeline creation failed: %d", skinPipeRes);
                    else
                        log("  Skinned pipeline (blend) created OK");
                }
            }

            if (sVertMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, sVertMod, nullptr);
            if (sFragMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, sFragMod, nullptr);
        }
    }

    VkShaderModule createShaderModule(const std::vector<u32>& code) {
        VkShaderModuleCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size() * sizeof(u32);
        createInfo.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult res = vkCreateShaderModule(device, &createInfo, nullptr, &mod);
        if (res != VK_SUCCESS)
            log("ERROR: vkCreateShaderModule FAILED: %d (code size=%zu words)", res, code.size());
        return mod;
    }

    void createDepthResources() {
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;   // float depth for reversed-Z (see renderpass note)
        createImage(swapchainExtent.width, swapchainExtent.height, depthFormat,
                    VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    depthImage, depthMem);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &viewInfo, nullptr, &depthView);
    }

    void createFramebuffers() {
        framebuffers.resize(swapchainViews.size());
        for (size_t i = 0; i < swapchainViews.size(); ++i) {
            VkImageView atts[] = {swapchainViews[i], depthView};
            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments = atts;
            fbInfo.width = swapchainExtent.width;
            fbInfo.height = swapchainExtent.height;
            fbInfo.layers = 1;
            vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = graphicsQueueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);

        commandBuffers.resize(swapchainImages.size());
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (u32)commandBuffers.size();
        vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
    }

    void createSyncObjects() {
        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailable);
        vkCreateSemaphore(device, &semInfo, nullptr, &renderFinished);

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fenceInfo, nullptr, &inFlightFence);
    }

    // ── Memory helpers ──────────────────────────────────────────

    u32 findMemoryType(u32 typeFilter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }
        return 0;
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& mem) {
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &buffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, buffer, &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, props);
        vkAllocateMemory(device, &allocInfo, nullptr, &mem);
        vkBindBufferMemory(device, buffer, mem, 0);
    }

    void createImage(u32 w, u32 h, VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags props,
                     VkImage& image, VkDeviceMemory& mem, u32 mipLevels = 1) {
        VkImageCreateInfo imgInfo = {};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.extent = {w, h, 1};
        imgInfo.mipLevels = mipLevels;
        imgInfo.arrayLayers = 1;
        imgInfo.format = format;
        imgInfo.tiling = tiling;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgInfo.usage = usage;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateImage(device, &imgInfo, nullptr, &image);

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, image, &memReqs);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, props);
        vkAllocateMemory(device, &allocInfo, nullptr, &mem);
        vkBindImageMemory(device, image, mem, 0);
    }

    VkFormat astcVkFormat(u32 bw, u32 bh) {
        // Every ASTC footprint has a VK *_SRGB_BLOCK enum — cover all of them so a raw
        // upload of a non-square block (e.g. 8x6, the dome floor atlas) is interpreted
        // with the correct footprint instead of being mis-strided as 8x8.
        if (bw==4  && bh==4)   return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
        if (bw==5  && bh==4)   return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
        if (bw==5  && bh==5)   return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
        if (bw==6  && bh==5)   return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
        if (bw==6  && bh==6)   return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
        if (bw==8  && bh==5)   return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
        if (bw==8  && bh==6)   return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
        if (bw==8  && bh==8)   return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
        if (bw==10 && bh==5)   return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
        if (bw==10 && bh==6)   return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
        if (bw==10 && bh==8)   return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
        if (bw==10 && bh==10)  return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
        if (bw==12 && bh==10)  return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
        if (bw==12 && bh==12)  return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    }

    void createTextureImageRaw(const u8* data, u32 dataSize,
                               u32 w, u32 h, VkFormat fmt,
                               VkImage& image, VkDeviceMemory& mem) {
        VkBuffer staging; VkDeviceMemory stagingMem;
        createBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, stagingMem);
        void* ptr;
        vkMapMemory(device, stagingMem, 0, dataSize, 0, &ptr);
        memcpy(ptr, data, dataSize);
        vkUnmapMemory(device, stagingMem);
        createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, mem);
        transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(staging, image, w, h);
        transitionImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);
    }

    void createTextureImage(const u8* rgba, u32 w, u32 h, VkImage& image, VkDeviceMemory& mem,
                            VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB) {
        VkDeviceSize imgSize = (VkDeviceSize)w * h * 4;

        VkBuffer stagingBuf;
        VkDeviceMemory stagingMem;
        createBuffer(imgSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuf, stagingMem);

        void* data;
        vkMapMemory(device, stagingMem, 0, imgSize, 0, &data);
        memcpy(data, rgba, (size_t)imgSize);
        vkUnmapMemory(device, stagingMem);

        // Mipmaps: a high-res texture (e.g. prism_wave's 2900x866 flame strip) minified onto a small/thin
        // surface aliases hard without mips -> harsh "exploded" wisps. Generate a full mip chain so the
        // high-frequency detail averages into a smooth gradient (libshell-faithful: it uses mipped samplers).
        u32 mipLevels = 1u + (u32)std::floor(std::log2((float)std::max(w, h)));
        lastTexMip = mipLevels;
        createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, mem, mipLevels);

        transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipLevels);
        copyBufferToImage(stagingBuf, image, w, h);   // writes mip 0
        generateMipmaps(image, (int32_t)w, (int32_t)h, mipLevels);  // blit-chain 0->1->.. + -> SHADER_READ

        vkDestroyBuffer(device, stagingBuf, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);
    }

    u32 lastTexMip = 1;   // mip count of the most recent createTextureImage (for the view)

    void generateMipmaps(VkImage image, int32_t w, int32_t h, u32 mipLevels) {
        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.image = image; b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.layerCount = 1; b.subresourceRange.levelCount = 1;
        int32_t mw = w, mh = h;
        for (u32 i = 1; i < mipLevels; ++i) {
            b.subresourceRange.baseMipLevel = i - 1;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,0, 0,0, 1,&b);
            VkImageBlit blit = {};
            blit.srcOffsets[1] = {mw, mh, 1};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i-1, 0, 1};
            blit.dstOffsets[1] = {mw>1?mw/2:1, mh>1?mh/2:1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,0, 0,0, 1,&b);
            if (mw>1) mw/=2; if (mh>1) mh/=2;
        }
        b.subresourceRange.baseMipLevel = mipLevels - 1;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,0, 0,0, 1,&b);
        endSingleTimeCommands(cmd);
    }

    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, u32 mipLevels = 1) {
        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage, dstStage;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            srcStage = dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        endSingleTimeCommands(cmd);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, u32 w, u32 h) {
        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        endSingleTimeCommands(cmd);
    }

    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &allocInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    void endSingleTimeCommands(VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
    }

    VkSampler createTextureSampler() {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = 16.0f;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;   // trilinear: sample the mip chain
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;                   // use all available mips (anti-alias)
        VkSampler sampler;
        vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
        return sampler;
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(device);
        for (auto& fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto& iv : swapchainViews) vkDestroyImageView(device, iv, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthMem, nullptr);
        vkDestroyImageView(device, depthView, nullptr);

        createSwapchain();
        createImageViews();
        createDepthResources();
        createFramebuffers();
    }

    // Per-frame skeletal animation. The HZANANIM3 take_001 curve data is a bit-packed
    // sparse/quantized format (decompressor in libshell sub_175F004, NEON popcount +
    // 1/255 dequant — not yet fully replicated). Until that's decoded byte-exact, drive
    // each joint with a smooth procedural motion keyed off elapsed time so the motes
    // drift/bob and the wave undulates — the ambient motion these env joints produce.
    // skinningMatrix is column-major; with identity invBind this is a world delta.
    void updateSkinning() {
        if (!timeStarted) { startTime = std::chrono::high_resolution_clock::now(); timeStarted = true; }
        float t = std::chrono::duration<float>(
            std::chrono::high_resolution_clock::now() - startTime).count();
        // Precompute a small per-bone drift offset (256 bones), each with its own phase.
        float off[256][3];
        for (u32 b = 0; b < 256; ++b) {
            float ph = (float)b * 0.7f;
            off[b][0] = 0.18f * sinf(t * 0.9f + ph);
            off[b][1] = 0.12f * sinf(t * 1.3f + ph * 1.7f);
            off[b][2] = 0.18f * cosf(t * 0.8f + ph * 0.5f);
        }
        for (auto& gm : gpuMeshes) {
            if (!gm.animated || !gm.vboMapped) continue;
            u8* base = reinterpret_cast<u8*>(gm.vboMapped);
            u32 nV = (u32)gm.vBone.size();
            for (u32 i = 0; i < nV; ++i) {
                u8 b = gm.vBone[i];
                float* p = reinterpret_cast<float*>(base + (size_t)i * gm.vboStride);
                p[0] = gm.basePos[i*3+0] + off[b][0];
                p[1] = gm.basePos[i*3+1] + off[b][1];
                p[2] = gm.basePos[i*3+2] + off[b][2];
            }
        }
    }

    bool farFitDone = false;
    // Fit the far clip plane to the ACTUAL scenery (libshell sizes the view frustum to contain the
    // scene + its vista, rather than a fixed magic number — some envs have ~13k-26k vista spheres,
    // others much bigger). We take the scene's bounding-sphere radius from the origin and set
    // far = 2*radius + margin (floored at 40k), so the skybox/mountains never get clipped. Reversed-Z
    // keeps near precision regardless of how big far is. Run once after the meshes are uploaded.
    void fitFarToScene() {
        if (farFitDone || gpuMeshes.empty()) return;
        farFitDone = true;
        // HSR_CLIP: use the DEVICE's finite far/near clip planes (from space.hstf) instead of fitting to scene.
        // Geometry beyond farClippingPlane is then GPU-clipped exactly as on-device — toggle off to draw all.
        if (std::getenv("HSR_CLIP") && sceneFarClip > 0.f) {
            cam.farZ = sceneFarClip;
            if (sceneNearClip > 0.f) cam.nearZ = sceneNearClip;
            if (const char* e = std::getenv("HSR_CLIPFAR")) { float v=(float)atof(e); if (v>0) cam.farZ=v; }
            log("  [CLIP] device far-clip ON: near=%.3f far=%.0f (geometry beyond far is CLIPPED; HSR_CLIP off = draw all)",
                cam.nearZ, cam.farZ);
            return;
        }
        float r2 = 0.0f;
        for (auto& gm : gpuMeshes) {
            for (int c = 0; c < 8; ++c) {
                float x=(c&1)?gm.bbMax[0]:gm.bbMin[0], y=(c&2)?gm.bbMax[1]:gm.bbMin[1], z=(c&4)?gm.bbMax[2]:gm.bbMin[2];
                float d = x*x + y*y + z*z; if (d > r2) r2 = d;
            }
        }
        float radius = std::sqrt(r2);
        float fit = radius * 2.0f + 200.0f;
        if (fit > cam.farZ) { cam.farZ = fit; log("  far plane fit to scenery: radius=%.0f -> farZ=%.0f", radius, cam.farZ); }
    }

    // ── V205 device culling replica (RenderableCullJob__9BA420 — IDA-read): per renderable, (1) frustum side
    // planes vs world AABB, (2) DISTANCE cull = squared dist from camera to CLOSEST AABB point > maxDist² (the
    // device's `a2[33]` test). TOGGLEABLE via HSR_CLIP so we can verify whether a cooked home's bounds bypass
    // the culler: ON → meshes outside the frustum/beyond far are dropped (= what the device would clip); OFF
    // (default) → nothing culled (draw everything). HSR_CLIPFAR overrides the cull distance (default = farZ).
    // HSR_CLIP_TINT keeps culled meshes visible but the count is logged, so you SEE what would vanish on-device.
    void computeCull(const float* vp) {
        bool on = std::getenv("HSR_CULL") != nullptr;   // SEPARATE from HSR_CLIP (the projection clip plane)
        float pl[4][4];                       // L,R,B,T side planes (near/far handled by the distance test)
        auto row = [&](int r, int k){ return vp[k*4 + r]; };
        for (int i = 0; i < 4; ++i) { float s = (i & 1) ? -1.f : 1.f; int rr = i >> 1;
            for (int k = 0; k < 4; ++k) pl[i][k] = row(3, k) + s * row(rr, k); }
        float maxD = cam.farZ; if (const char* e = std::getenv("HSR_CLIPFAR")) { float v=(float)atof(e); if (v>0) maxD=v; }
        float far2 = maxD * maxD;
        int culled = 0, total = 0;
        for (auto& gm : gpuMeshes) {
            gm.culled = false;
            if (!on || gm.nIdx == 0) continue;
            ++total;
            float wmin[3] = {1e30f,1e30f,1e30f}, wmax[3] = {-1e30f,-1e30f,-1e30f};
            for (int c = 0; c < 8; ++c) {
                float x=(c&1)?gm.bbMax[0]:gm.bbMin[0], y=(c&2)?gm.bbMax[1]:gm.bbMin[1], z=(c&4)?gm.bbMax[2]:gm.bbMin[2];
                float wx=gm.model[0]*x+gm.model[4]*y+gm.model[8]*z+gm.model[12];
                float wy=gm.model[1]*x+gm.model[5]*y+gm.model[9]*z+gm.model[13];
                float wz=gm.model[2]*x+gm.model[6]*y+gm.model[10]*z+gm.model[14];
                wmin[0]=std::min(wmin[0],wx); wmax[0]=std::max(wmax[0],wx);
                wmin[1]=std::min(wmin[1],wy); wmax[1]=std::max(wmax[1],wy);
                wmin[2]=std::min(wmin[2],wz); wmax[2]=std::max(wmax[2],wz);
            }
            float ctr[3]={(wmin[0]+wmax[0])*.5f,(wmin[1]+wmax[1])*.5f,(wmin[2]+wmax[2])*.5f};
            float ext[3]={(wmax[0]-wmin[0])*.5f,(wmax[1]-wmin[1])*.5f,(wmax[2]-wmin[2])*.5f};
            bool out = false;
            for (int i = 0; i < 4 && !out; ++i) {
                float d = pl[i][0]*ctr[0]+pl[i][1]*ctr[1]+pl[i][2]*ctr[2]+pl[i][3];
                float r = ext[0]*std::fabs(pl[i][0])+ext[1]*std::fabs(pl[i][1])+ext[2]*std::fabs(pl[i][2]);
                if (d < -r) out = true;
            }
            if (!out) {                       // distance cull = device closest-AABB-point > maxDist²
                float dx=std::max(0.f,std::max(wmin[0]-cam.pos[0],cam.pos[0]-wmax[0]));
                float dy=std::max(0.f,std::max(wmin[1]-cam.pos[1],cam.pos[1]-wmax[1]));
                float dz=std::max(0.f,std::max(wmin[2]-cam.pos[2],cam.pos[2]-wmax[2]));
                if (dx*dx+dy*dy+dz*dz > far2) out = true;
            }
            gm.culled = out; if (out) ++culled;
        }
        if (on) { static int last=-1; if (culled!=last){ last=culled;
            log("  [CULL] V205 frustum+far cull ON (maxD=%.0f): %d/%d meshes would be CULLED. (HSR_CULL off = draw all)", maxD, culled, total); } }
    }

    void updateUniforms() {
        fitFarToScene();
        bool paned = uiViewportRect.extent.width > 0 && uiViewportRect.extent.height > 0;
        float aw = paned ? (float)uiViewportRect.extent.width  : (float)swapchainExtent.width;
        float ah = paned ? (float)uiViewportRect.extent.height : (float)swapchainExtent.height;
        cam.updateProj(aw / ah);
        cam.updateView();

        // clipFromWorld = P * V  (clip-from-world, NO model — model goes in push constants)
        // Shader: gl_Position = clipFromWorld0 * worldFromModel * vec4(pos, 1.0)
        float vp[16];
        mat4mul(cam.proj, cam.view, vp);
        memcpy(cachedVP, vp, 64);  // cache for push constant computation in render()
        computeCull(vp);           // V205 frustum+far cull (HSR_CLIP) — marks gm.culled; draw loops skip them

        float W = (float)swapchainExtent.width;
        float H = (float)swapchainExtent.height;
        float cx = cam.pos[0], cy = cam.pos[1], cz = cam.pos[2];

        static float ubo[184];
        memset(ubo, 0, sizeof(ubo));

        // +0   clipFromWorld0  mat4 ColMajor  (P*V — no model, shader multiplies worldFromModel)
        memcpy(ubo + 0,  vp, 64);
        // +64  clipFromWorld1  mat4 ColMajor  (same for mono)
        memcpy(ubo + 16, vp, 64);
        // +128 skyBoxClipFromWorld0  mat4 ColMajor  (same)
        memcpy(ubo + 32, vp, 64);
        // +192 skyBoxClipFromWorld1  mat4 ColMajor  (same)
        memcpy(ubo + 48, vp, 64);
        // +256 viewFromWorld0  mat4 ColMajor  (V only — no model)
        memcpy(ubo + 64, cam.view, 64);
        // +320 viewFromWorld1  mat4 ColMajor
        memcpy(ubo + 80, cam.view, 64);
        // +384 worldCameraPos0  vec4
        ubo[96] = cx; ubo[97] = cy; ubo[98] = cz; ubo[99] = 1.0f;
        // +400 worldCameraPos1  vec4
        ubo[100] = cx; ubo[101] = cy; ubo[102] = cz; ubo[103] = 1.0f;
        // +592 whitePoint = 1.0
        ubo[148] = 1.0f;
        // +596 time (seconds elapsed) — drives procedural vertex animations (motes, prism_wave)
        if (!timeStarted) { startTime = std::chrono::high_resolution_clock::now(); timeStarted = true; }
        ubo[149] = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTime).count();
        // +664 viewportSize = {W, H}
        ubo[166] = W; ubo[167] = H;
        // FOG — the cooked frag's fogMixColor reads GlobalUniforms.fogColor(@672)/fogStart(@688)/
        // fogDensity(@692); the renderer left them 0 = NO fog. The env SceneSettings (V79 basescene.scene,
        // same nuxd env) sets distanceFog=[0,19], heightFog=[0,1500], fogColor=[0,0,0,0]. Wire them so the
        // scene gets its depth fog (HSR_NOFOG disables for debugging).
        // Fog comes ONLY from the env's decoded SceneSettings (sceneFog*). density 0 => env has no
        // distanceFog => write nothing (libshell fallback = no distance fog). NEVER hardcode per-env values.
        if (!std::getenv("HSR_NOFOG") && sceneFogDensity > 0.f) {
            ubo[168]=sceneFogColor[0]; ubo[169]=sceneFogColor[1]; ubo[170]=sceneFogColor[2]; ubo[171]=sceneFogColor[3];
            ubo[172]=sceneFogStart;                                   // +688 fogStart  = distanceFog[0]
            ubo[173]=sceneFogDensity; ubo[174]=sceneHeightFog[0]; ubo[175]=sceneHeightFog[1];  // +692 fogDensity + height
        }

        // Per-mesh globalUniforms. The real libshell SPIR-V transforms world-baked vertices by
        // clipFromWorld ONLY (the worldFromModel push constant is dead on desktop), so an editor move/
        // rotate/scale must be folded into THIS mesh's clipFromWorld: clipFromWorld = P*V*gm.model and
        // viewFromWorld = V*gm.model. gm.model is identity for un-edited meshes (=> unchanged P*V), and
        // the editor's centroid-pivoted delta for edited ones — so objects actually transform on screen.
        static int uudbg = -1; if (uudbg<0) uudbg = std::getenv("HSR_UUDBG") ? 1 : 0;
        for (auto& gm : gpuMeshes) {
            if (uudbg && gm.name.find("LightFixtureB")!=std::string::npos)
                log("  [UU] %s trans=(%.2f,%.2f,%.2f) m0=%.3f m5=%.3f m10=%.3f m1=%.2f m2=%.2f m4=%.2f m6=%.2f m8=%.2f m9=%.2f centroid=(%.2f,%.2f,%.2f)",
                    gm.name.c_str(), gm.model[12],gm.model[13],gm.model[14],
                    gm.model[0],gm.model[5],gm.model[10], gm.model[1],gm.model[2],gm.model[4],gm.model[6],gm.model[8],gm.model[9],
                    gm.centroid[0],gm.centroid[1],gm.centroid[2]);
            if (gm.name.find("prism")!=std::string::npos) {
                static int pl=0; if(!pl){ pl=1;
                    float c0=gm.centroid[0],c1=gm.centroid[1],c2=gm.centroid[2];
                    float clx=vp[0]*c0+vp[4]*c1+vp[8]*c2+vp[12], cly=vp[1]*c0+vp[5]*c1+vp[9]*c2+vp[13],
                          clz=vp[2]*c0+vp[6]*c1+vp[10]*c2+vp[14], clw=vp[3]*c0+vp[7]*c1+vp[11]*c2+vp[15];
                    log("  PRISMCLIP cam=(%.2f,%.2f,%.2f) centroid=(%.2f,%.2f,%.2f) clipW=%.2f NDC=(%.2f,%.2f,%.2f) cull=%d useBlend=%d",
                        cx,cy,cz, c0,c1,c2, clw, clw!=0?clx/clw:0, clw!=0?cly/clw:0, clw!=0?clz/clw:0, (int)gm.cullBack, (int)gm.useBlend);
                }
            }
            // clipFromWorld = P*V, viewFromWorld = V (NO per-mesh model). The per-object worldFromModel
            // (incl. editor move/rotate/scale edits) is carried by the PUSH CONSTANT, which the real
            // libshell isotropictiled/default/etc. vertex shaders consume. Folding gm.model into
            // clipFromWorld here too DOUBLE-transforms locally-authored meshes (haven lamps/plants/
            // fixtures: rendered at ~2x their position, flung outside the room) while world-baked meshes
            // (model=identity) were unaffected — which masked the bug. ubo+0/+64 already hold vp / view.
            memcpy(ubo + 0,  vp, 64);            // clipFromWorld0 = P*V
            memcpy(ubo + 16, vp, 64);            // clipFromWorld1
            memcpy(ubo + 64, cam.view, 64);      // viewFromWorld0 = V
            memcpy(ubo + 80, cam.view, 64);      // viewFromWorld1
            void* data;
            vkMapMemory(device, gm.globalUboMem, 0, 736, 0, &data);
            memcpy(data, ubo, 736);
            vkUnmapMemory(device, gm.globalUboMem);
        }
    }

    void buildModelMatrix(const Transform& t, float out[16]) {
        float x=t.rot[0], y=t.rot[1], z=t.rot[2], w=t.rot[3];
        float sx=t.scale[0], sy=t.scale[1], sz=t.scale[2];
        float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z), r02=2*(x*z+w*y);
        float r10=2*(x*y+w*z), r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
        float r20=2*(x*z-w*y), r21=2*(y*z+w*x), r22=1-2*(x*x+y*y);
        out[0]=r00*sx; out[4]=r01*sy; out[8]=r02*sz; out[12]=t.pos[0];
        out[1]=r10*sx; out[5]=r11*sy; out[9]=r12*sz; out[13]=t.pos[1];
        out[2]=r20*sx; out[6]=r21*sy; out[10]=r22*sz; out[14]=t.pos[2];
        out[3]=0;      out[7]=0;      out[11]=0;      out[15]=1;
    }

    void mat4mul(const float a[16], const float b[16], float out[16]) {
        for (int col=0; col<4; ++col)
            for (int row=0; row<4; ++row) {
                float sum=0;
                for (int k=0; k<4; ++k) sum += a[k*4+row] * b[col*4+k];
                out[col*4+row] = sum;
            }
    }

};
