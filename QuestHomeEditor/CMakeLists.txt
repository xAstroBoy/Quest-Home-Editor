cmake_minimum_required(VERSION 3.16)
project(questhomeeditor LANGUAGES C CXX)
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
if(NOT WIN32)
    target_link_libraries(volk PRIVATE ${CMAKE_DL_LIBS})
endif()

# ── miniz (zip reading — single-header) ─────────────────────────
set(MINIZ_DIR "${CMAKE_SOURCE_DIR}/third_party/miniz")
add_library(miniz
    "${MINIZ_DIR}/miniz.c"
    "${MINIZ_DIR}/miniz_tdef.c"
    "${MINIZ_DIR}/miniz_tinfl.c"
    "${MINIZ_DIR}/miniz_zip.c"
)
target_include_directories(miniz PUBLIC "${MINIZ_DIR}")

# ── astcenc (ARM ASTC) — FULL encoder+decoder so the cooker can ENCODE textures (RGBA8->ASTC)
#    as well as the renderer DECODE them. (DECOMPRESSOR OFF = full build.)
#    ISA: the old hardcoded AVX2 build crashed with 0xc000001d (illegal instruction) on any CPU
#    without AVX2 (pre-2013 Intel, e.g. a 3rd-gen i5 — GitHub issue #1). Default is the broadly
#    compatible ISA per arch (SSE4.1 on x86-64 = every 64-bit CPU since ~2008; NEON on arm64);
#    -DHSR_AVX2=ON opts back into the ~1.3x faster AVX2 encoder for your own machine. ──
option(HSR_AVX2 "Build astcenc with AVX2 (faster cooks; needs a 2013+ CPU — crashes older ones)" OFF)
include(FetchContent)
# astcenc includes CTest and enables the global BUILD_TESTING cache option by default;
# Opus interprets that as a request to register its own upstream test executables. We
# build only selected product targets in CI, so those unbuilt vendor tests make CTest
# fail spuriously. Keep dependency test suites off and register our tests explicitly.
set(BUILD_TESTING OFF CACHE BOOL "Build third-party test suites" FORCE)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG 4.8.0
    GIT_SHALLOW TRUE
)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI          OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)   # macOS: pick the ISA explicitly per arch below
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(ASTCENC_LIB astcenc-neon-static)
elseif(HSR_AVX2)
    set(ASTCENC_ISA_AVX2 ON CACHE BOOL "" FORCE)
    set(ASTCENC_LIB astcenc-avx2-static)
else()
    set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
    set(ASTCENC_LIB astcenc-sse4.1-static)
endif()
FetchContent_MakeAvailable(astcenc)

# astcenc 4.8 enables -Werror and supplies both -ffp-model=precise and an explicit
# -ffp-contract setting. Clang 22 diagnoses that intentional combination as an
# overriding option, which otherwise prevents MinGW/Clang builds before our code is
# compiled. Keep vendor warnings strict while allowing this one toolchain diagnostic.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 22)
    target_compile_options(${ASTCENC_LIB} PRIVATE -Wno-error=overriding-option)
endif()

# ── libopus (xiph) — DECODER for Ogg-Opus env audio. Half the official V79 envs (storybook,
#    polarvillage, underwater, ...) ship their background loop as Ogg-OPUS, which stb_vorbis can't
#    decode (PC preview silent + "0 Hz" cook). The tiny Ogg page demux is hand-rolled in
#    audio_convert.h; opus itself does the packet decode. ──
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

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

# ── PhysX 4.1.2 (CHECKED, static) — the collision cook. ON EVERYWHERE. ─────────────────────────────────────
#    cookNavmeshSEBD() IS PhysX. With it compiled out, physx_navmesh.cpp is the #else stub returning {}, and the
#    cooker then: emits NO mesh collider at all (hsl_cooker.h — the auto/animated collider path has no fallback,
#    it just logs "PhysX cook FAILED"), and degrades the smart floor to a coarse ColliderBox grid. That is a
#    SILENT downgrade — the build succeeds and every test passes — so this is ON by default and a missing lib set
#    is a hard configure error, never a quiet fallback.
#      Windows      : the static libs are committed under third_party/physx/lib/ — nothing to do.
#      Linux/macOS  : run `bash scripts/build_physx.sh` once (builds PhysX 4.1.2 config `checked` from source
#                     into third_party/physx/lib/<os>-<arch>/). Headers are shared: the vendored
#                     third_party/physx/include + pxshared/include are the same 4.1.2 for every platform.
#    PX_CHECKED=1 is NOT optional: the device PhysX is a checked build (proven by byte-diffing haven2025's working
#    SEBD), and a release-config PhysX serializes a different struct layout = collision that loads but does nothing. ──
option(HSR_HAVE_PHYSX "Cook PhysX SEBD trimesh colliders (Windows: committed libs; Linux/macOS: scripts/build_physx.sh)" ON)
set(PHYSX_DIR "${CMAKE_SOURCE_DIR}/third_party/physx")

if(HSR_HAVE_PHYSX)
    if(WIN32)
        set(HSR_PHYSX_TAG "windows-x64")
        set(HSR_PHYSX_LIBDIR "${PHYSX_DIR}/lib")
        set(PHYSX_LIBS
            "${HSR_PHYSX_LIBDIR}/PhysXExtensions_static.lib"
            "${HSR_PHYSX_LIBDIR}/PhysXCooking_static.lib"
            "${HSR_PHYSX_LIBDIR}/PhysX_static.lib"
            "${HSR_PHYSX_LIBDIR}/PhysXCommon_static.lib"
            "${HSR_PHYSX_LIBDIR}/PhysXPvdSDK_static.lib"
            "${HSR_PHYSX_LIBDIR}/PhysXFoundation_static.lib")
        foreach(_pxlib IN LISTS PHYSX_LIBS)
            if(NOT EXISTS "${_pxlib}")
                message(FATAL_ERROR
                    "HSR_HAVE_PHYSX=ON but ${_pxlib} is missing.\n"
                    "These libs are committed to the repo — a partial clone or a stripped checkout will lack them.")
            endif()
        endforeach()
    else()
        # Tag MUST mirror the one scripts/build_physx.sh writes.
        if(APPLE)
            set(_hsr_px_os "macos")
        else()
            set(_hsr_px_os "linux")
        endif()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(_hsr_px_cpu "arm64")
        else()
            set(_hsr_px_cpu "x86_64")
        endif()
        set(HSR_PHYSX_TAG "${_hsr_px_os}-${_hsr_px_cpu}")
        set(HSR_PHYSX_LIBDIR "${PHYSX_DIR}/lib/${HSR_PHYSX_TAG}")
        set(PHYSX_LIBS "")
        # PhysX decorates archive names with a suffix that varies by platform/config (Linux and macOS
        # both emit libPhysX_x64.a — that _x64 is the 64-BIT tag, so an arm64 build carries it too).
        # Rather than guess every spelling, glob and match with an ANCHORED regex: a plain
        # find_library(NAMES PhysX ...) would happily return libPhysXCommon_x64.a for "PhysX".
        file(GLOB _pxarchives "${HSR_PHYSX_LIBDIR}/libPhysX*.a")
        # Link order below is the dependency order (only matters for ld; grouped for GNU ld anyway).
        foreach(_pxcomp PhysXExtensions PhysXCooking PhysX PhysXCommon PhysXPvdSDK PhysXFoundation)
            set(_pxfound "")
            foreach(_pxcand IN LISTS _pxarchives)
                get_filename_component(_pxname "${_pxcand}" NAME)
                if(_pxname MATCHES "^lib${_pxcomp}(_[A-Za-z0-9]+)*\\.a$")
                    set(_pxfound "${_pxcand}")
                    break()
                endif()
            endforeach()
            if(NOT _pxfound)
                message(FATAL_ERROR
                    "HSR_HAVE_PHYSX=ON but lib${_pxcomp}*.a was not found in ${HSR_PHYSX_LIBDIR}.\n"
                    "Build the PhysX static libs for this platform first:\n"
                    "    bash scripts/build_physx.sh\n"
                    "Or configure with -DHSR_HAVE_PHYSX=OFF to accept DEGRADED collision "
                    "(no mesh colliders cooked, coarse ColliderBox floor only).")
            endif()
            list(APPEND PHYSX_LIBS "${_pxfound}")
        endforeach()
    endif()

    # One interface target so the editor and hsl_cook link PhysX identically (they must: both run the same cook).
    add_library(hsr_physx INTERFACE)
    target_compile_definitions(hsr_physx INTERFACE HSR_HAVE_PHYSX PX_PHYSX_STATIC_LIB PX_CHECKED=1)
    target_include_directories(hsr_physx INTERFACE "${PHYSX_DIR}/include" "${PHYSX_DIR}/pxshared/include")
    if(WIN32 OR APPLE)
        # link.exe and ld64 both resolve static archives without needing an explicit group.
        target_link_libraries(hsr_physx INTERFACE ${PHYSX_LIBS})
    else()
        # GNU ld walks archives in one pass and the six PhysX archives reference each other both ways,
        # so any single ordering leaves undefined symbols. Group them.
        target_link_libraries(hsr_physx INTERFACE -Wl,--start-group ${PHYSX_LIBS} -Wl,--end-group ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "PhysX: ON (4.1.2 checked, ${HSR_PHYSX_TAG}) from ${HSR_PHYSX_LIBDIR}")
else()
    message(WARNING
        "HSR_HAVE_PHYSX=OFF — cookNavmeshSEBD() is a STUB. No mesh colliders will be cooked and the smart floor "
        "falls back to a coarse ColliderBox grid. Do not ship release binaries built this way.")
endif()

# ── Executable ──────────────────────────────────────────────────
add_executable(questhomeeditor
    src/main.cpp
    src/impl/miniaudio_impl.c
    src/impl/stb_vorbis_impl.c
    src/impl/stb_image_impl.c
    src/impl/stb_image_write_impl.c
    src/impl/stb_truetype_impl.c
    src/cook/hzanim_acl.cpp
    src/cook/physx_navmesh.cpp
    src/cook/embedded_assets.cpp
    $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/app.rc>   # Win32 .exe file icon
)

target_compile_definitions(questhomeeditor PRIVATE VK_NO_PROTOTYPES GLFW_INCLUDE_VULKAN)

target_include_directories(questhomeeditor PRIVATE
    "${VULKAN_HEADERS_DIR}"
    "${VOLK_DIR}"
    "${MINIZ_DIR}"
    "${MINIAUDIO_DIR}"
    "${STB_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)

target_include_directories(questhomeeditor PRIVATE
    "${astcenc_SOURCE_DIR}/Source"
)

# ── ACL + RTM (Animation Compression Library — header-only; decodes the V203 HzAnim clips) ──
target_include_directories(questhomeeditor PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/acl/includes"
    "${CMAKE_SOURCE_DIR}/third_party/rtm/includes"
)

target_link_libraries(questhomeeditor PRIVATE
    volk
    miniz
    ${GLFW_TARGET}
    ${ASTCENC_LIB}
    opus
)

# ── PhysX — the collision cook (defines/includes/libs all come from the hsr_physx interface target) ──
if(HSR_HAVE_PHYSX)
    target_link_libraries(questhomeeditor PRIVATE hsr_physx)
endif()

# ── Platform-specific ───────────────────────────────────────────
if(NOT WIN32)
    # hsl_cook links Threads::Threads on every non-Windows platform (line ~239), so the
    # imported target must exist on macOS too — not just in the Linux else() branch below.
    find_package(Threads REQUIRED)
endif()
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(questhomeeditor PRIVATE /Zc:__cplusplus)  # ACL checks __cplusplus for the C++ level
    target_compile_options(questhomeeditor PRIVATE /Zi)              # debug symbols (PDB) for the crash-handler stack traces
    target_compile_options(questhomeeditor PRIVATE /bigobj)          # main.cpp TU exceeds the 64k COFF section limit (big inline cook/shadergen headers)
    target_link_options(questhomeeditor PRIVATE /DEBUG)              # emit the .pdb so segfault stacks resolve to file:line
endif()
if(WIN32)
    target_compile_definitions(questhomeeditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # volk loads Vulkan at runtime — no vulkan-1.lib needed
    # miniaudio WASAPI backend needs COM (ole32) + winmm ; dbghelp = symbolized crash stack traces ;
    # ws2_32 = the HSR_LIVE HTTP control server (Winsock)
    target_link_libraries(questhomeeditor PRIVATE ole32 winmm dbghelp ws2_32 gdi32 msimg32)   # gdi32/msimg32 = the in-window GDI loading splash (GradientFill)
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    target_link_libraries(questhomeeditor PRIVATE ${COCOA_LIBRARY} ${IOKIT_LIBRARY})
else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        target_link_libraries(questhomeeditor PRIVATE Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS} m)
    else()
        target_link_libraries(questhomeeditor PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
endif()

# The product is "Quest Home Editor" — build the exe under that name (the CMake TARGET stays `questhomeeditor`
# so `cmake --build --target questhomeeditor` and existing tooling keep working; only the output file is renamed).
set_target_properties(questhomeeditor PROPERTIES OUTPUT_NAME "Quest Home Editor")

# ── Shader resource ─────────────────────────────────────────────
# The shaders are loaded at runtime from the APK (RENDSHAD → SPIRV).
# No compile-time shader embedding needed.

# ── hsl_cook — standalone self-contained APK cooker (no Vulkan/GLFW; builds fast) ──
# Same hsl_cooker.h the editor's "Export APK" uses; this CLI driver validates the format pipeline.
# hzanim_acl.cpp defines hzAclEncode() — exportSceneAPK() calls it for skinned-anim (HZAN) clips, so it MUST be
# linked here too (the questhomeeditor target already compiles it). Without it: LNK2019 unresolved hzAclEncode.
add_executable(hsl_cook src/cook/_ckcook.cpp src/cook/hzanim_acl.cpp src/cook/physx_navmesh.cpp src/cook/embedded_assets.cpp src/impl/stb_image_impl.c)
target_include_directories(hsl_cook PRIVATE
    "${MINIZ_DIR}"
    "${STB_DIR}"                                      # stb_image.h — headless --cookglb glTF import
    "${astcenc_SOURCE_DIR}/Source"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/acl/includes"   # ACL (Animation Compression Library) — header-only encoder
    "${CMAKE_SOURCE_DIR}/third_party/rtm/includes"   # RTM (Realtime Math) — ACL's math dependency
)
target_link_libraries(hsl_cook PRIVATE miniz ${ASTCENC_LIB})
if(HSR_HAVE_PHYSX)
    target_link_libraries(hsl_cook PRIVATE hsr_physx)   # same PhysX as the editor — both run the same cook
endif()
if(WIN32)
    target_compile_definitions(hsl_cook PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(hsl_cook PRIVATE /Zc:__cplusplus)  # ACL checks __cplusplus for the C++ level
    endif()
    target_link_libraries(hsl_cook PRIVATE bcrypt crypt32)   # self-contained APK v2 signing (next increment)
else()
    target_link_libraries(hsl_cook PRIVATE Threads::Threads m)
endif()

option(HSR_BUILD_TESTS "Build Quest Home Editor regression tests" ON)
if(HSR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
