cmake_minimum_required(VERSION 3.16)
project(hsr_renderer LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)   # compile app.rc -> the .exe file icon
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Bundled Vulkan headers (no SDK needed) ──────────────────────
set(VULKAN_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include")
if(NOT EXISTS "${VULKAN_HEADERS_DIR}/vulkan/vulkan.h")
    message(FATAL_ERROR "Vulkan headers not found at ${VULKAN_HEADERS_DIR}. Run setup first.")
endif()

# ── volk (Vulkan loader — single-header) ────────────────────────
set(VOLK_DIR "${CMAKE_SOURCE_DIR}/third_party/volk")
add_library(volk STATIC "${VOLK_DIR}/volk.c" "${VOLK_DIR}/volk.h")
target_include_directories(volk PUBLIC "${VOLK_DIR}")
target_include_directories(volk PUBLIC "${VULKAN_HEADERS_DIR}")
target_compile_definitions(volk PRIVATE VK_NO_PROTOTYPES)

# ── miniz (zip reading — single-header) ─────────────────────────
set(MINIZ_DIR "${CMAKE_SOURCE_DIR}/third_party/miniz")
add_library(miniz
    "${MINIZ_DIR}/miniz.c"
    "${MINIZ_DIR}/miniz_tdef.c"
    "${MINIZ_DIR}/miniz_tinfl.c"
    "${MINIZ_DIR}/miniz_zip.c"
)
target_include_directories(miniz PUBLIC "${MINIZ_DIR}")

# ── astcenc (ARM ASTC — AVX2) — FULL encoder+decoder so the cooker can ENCODE textures (RGBA8->ASTC)
#    as well as the renderer DECODE them. (DECOMPRESSOR OFF = full build; target = astcenc-avx2-static.) ──
include(FetchContent)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG 4.8.0
    GIT_SHALLOW TRUE
)
set(ASTCENC_ISA_AVX2    ON  CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(astcenc)
set(ASTCENC_LIB astcenc-avx2-static)

# ── GLFW — FetchContent or find ─────────────────────────────────
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
    set(GLFW_TARGET glfw)
else()
    set(GLFW_TARGET glfw)
endif()

# ── miniaudio (ambient playback) + stb_vorbis (Ogg Vorbis decode) — separate impl TUs ──
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party/miniaudio")
set(STB_DIR       "${CMAKE_SOURCE_DIR}/third_party/stb")

# ── Editor UI = a from-scratch custom C++ toolkit (src/ui/), NOT Dear ImGui. Drawn through the renderer's
#    own Vulkan; text via stb_truetype (impl in its own TU, like stb_image/stb_vorbis). No external UI libs. ──

# ── PhysX 4.x (OPTIONAL, vendored Windows static libs). OFF by default: the cooked SEBD trimesh is binary-
#    incompatible with the device PhysX (crashes its BVH33 narrow-phase), so the real walkable collision is the
#    ColliderBox grid (no cooked data). With it off, physx_navmesh.cpp is a stub and the build is PhysX-free, so it
#    compiles on Linux/macOS — not just Windows. Turn ON with -DHSR_HAVE_PHYSX=ON if you have the vendored libs. ──
option(HSR_HAVE_PHYSX "Cook PhysX SEBD trimesh navmesh (needs the vendored Windows PhysX static libs)" OFF)
set(PHYSX_DIR "${CMAKE_SOURCE_DIR}/third_party/physx")
set(PHYSX_LIBS
    "${PHYSX_DIR}/lib/PhysXExtensions_static.lib"
    "${PHYSX_DIR}/lib/PhysXCooking_static.lib"
    "${PHYSX_DIR}/lib/PhysX_static.lib"
    "${PHYSX_DIR}/lib/PhysXCommon_static.lib"
    "${PHYSX_DIR}/lib/PhysXPvdSDK_static.lib"
    "${PHYSX_DIR}/lib/PhysXFoundation_static.lib")

# ── Executable ──────────────────────────────────────────────────
add_executable(hsr_renderer
    src/main.cpp
    src/impl/miniaudio_impl.c
    src/impl/stb_vorbis_impl.c
    src/impl/stb_image_impl.c
    src/impl/stb_truetype_impl.c
    src/cook/hzanim_acl.cpp
    src/cook/physx_navmesh.cpp
    src/cook/embedded_assets.cpp
    $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/app.rc>   # Win32 .exe file icon
)

target_compile_definitions(hsr_renderer PRIVATE VK_NO_PROTOTYPES GLFW_INCLUDE_VULKAN)

target_include_directories(hsr_renderer PRIVATE
    "${VULKAN_HEADERS_DIR}"
    "${VOLK_DIR}"
    "${MINIZ_DIR}"
    "${MINIAUDIO_DIR}"
    "${STB_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)

target_include_directories(hsr_renderer PRIVATE
    "${astcenc_SOURCE_DIR}/Source"
)

# ── ACL + RTM (Animation Compression Library — header-only; decodes the V203 HzAnim clips) ──
target_include_directories(hsr_renderer PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/acl/includes"
    "${CMAKE_SOURCE_DIR}/third_party/rtm/includes"
)

target_link_libraries(hsr_renderer PRIVATE
    volk
    miniz
    ${GLFW_TARGET}
    ${ASTCENC_LIB}
)

# ── PhysX (optional) — only when -DHSR_HAVE_PHYSX=ON + the vendored libs are present ──
if(HSR_HAVE_PHYSX)
    target_compile_definitions(hsr_renderer PRIVATE HSR_HAVE_PHYSX PX_PHYSX_STATIC_LIB)
    target_include_directories(hsr_renderer PRIVATE "${PHYSX_DIR}/include" "${PHYSX_DIR}/pxshared/include")
    target_link_libraries(hsr_renderer PRIVATE ${PHYSX_LIBS})
endif()

# ── Platform-specific ───────────────────────────────────────────
if(MSVC)
    target_compile_options(hsr_renderer PRIVATE /Zc:__cplusplus)  # ACL checks __cplusplus for the C++ level
    target_compile_options(hsr_renderer PRIVATE /Zi)              # debug symbols (PDB) for the crash-handler stack traces
    target_link_options(hsr_renderer PRIVATE /DEBUG)              # emit the .pdb so segfault stacks resolve to file:line
endif()
if(WIN32)
    target_compile_definitions(hsr_renderer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # volk loads Vulkan at runtime — no vulkan-1.lib needed
    # miniaudio WASAPI backend needs COM (ole32) + winmm ; dbghelp = symbolized crash stack traces ;
    # ws2_32 = the HSR_LIVE HTTP control server (Winsock)
    target_link_libraries(hsr_renderer PRIVATE ole32 winmm dbghelp ws2_32)
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    target_link_libraries(hsr_renderer PRIVATE ${COCOA_LIBRARY} ${IOKIT_LIBRARY})
else()
    find_package(Vulkan REQUIRED)
    target_link_libraries(hsr_renderer PRIVATE Vulkan::Vulkan)
endif()

# ── Shader resource ─────────────────────────────────────────────
# The shaders are loaded at runtime from the APK (RENDSHAD → SPIRV).
# No compile-time shader embedding needed.

# ── hsl_cook — standalone self-contained APK cooker (no Vulkan/GLFW; builds fast) ──
# Same hsl_cooker.h the editor's "Export APK" uses; this CLI driver validates the format pipeline.
# hzanim_acl.cpp defines hzAclEncode() — exportSceneAPK() calls it for skinned-anim (HZAN) clips, so it MUST be
# linked here too (the hsr_renderer target already compiles it). Without it: LNK2019 unresolved hzAclEncode.
add_executable(hsl_cook src/cook/_ckcook.cpp src/cook/hzanim_acl.cpp src/cook/physx_navmesh.cpp src/cook/embedded_assets.cpp)
target_include_directories(hsl_cook PRIVATE
    "${MINIZ_DIR}"
    "${astcenc_SOURCE_DIR}/Source"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/acl/includes"   # ACL (Animation Compression Library) — header-only encoder
    "${CMAKE_SOURCE_DIR}/third_party/rtm/includes"   # RTM (Realtime Math) — ACL's math dependency
)
target_link_libraries(hsl_cook PRIVATE miniz ${ASTCENC_LIB})
if(HSR_HAVE_PHYSX)
    target_compile_definitions(hsl_cook PRIVATE HSR_HAVE_PHYSX PX_PHYSX_STATIC_LIB)
    target_include_directories(hsl_cook PRIVATE "${PHYSX_DIR}/include" "${PHYSX_DIR}/pxshared/include")
    target_link_libraries(hsl_cook PRIVATE ${PHYSX_LIBS})
endif()
if(WIN32)
    target_compile_definitions(hsl_cook PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(hsl_cook PRIVATE /Zc:__cplusplus)  # ACL checks __cplusplus for the C++ level
    target_link_libraries(hsl_cook PRIVATE bcrypt crypt32)   # self-contained APK v2 signing (next increment)
endif()
