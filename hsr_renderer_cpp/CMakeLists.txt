cmake_minimum_required(VERSION 3.16)
project(hsr_renderer LANGUAGES C CXX)

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

# ── astcenc (ARM ASTC decoder — AVX2, decompress only) ──────────
include(FetchContent)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG 4.8.0
    GIT_SHALLOW TRUE
)
set(ASTCENC_ISA_AVX2    ON  CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR ON  CACHE BOOL "" FORCE)
set(ASTCENC_CLI         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(astcenc)

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

# ── Dear ImGui (editor UI) — fetched, sources compiled into the exe ──
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# ── ImGuizmo (move/rotate/scale gizmo) — vendored src (master, imgui-1.90 compatible) ──
set(IMGUIZMO_DIR "${CMAKE_SOURCE_DIR}/third_party/imguizmo")

set(IMGUI_SRC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    "${IMGUIZMO_DIR}/ImGuizmo.cpp"
)

# ── Executable ──────────────────────────────────────────────────
add_executable(hsr_renderer
    src/main.cpp
    src/miniaudio_impl.c
    src/stb_vorbis_impl.c
    src/stb_image_impl.c
    ${IMGUI_SRC}
)

# VK_NO_PROTOTYPES for ALL TUs (incl. imgui_impl_vulkan) so it uses volk-loaded fn pointers
# via ImGui_ImplVulkan_LoadFunctions().
target_compile_definitions(hsr_renderer PRIVATE VK_NO_PROTOTYPES GLFW_INCLUDE_VULKAN IMGUI_DEFINE_MATH_OPERATORS)

target_include_directories(hsr_renderer PRIVATE
    "${VULKAN_HEADERS_DIR}"
    "${VOLK_DIR}"
    "${MINIZ_DIR}"
    "${MINIAUDIO_DIR}"
    "${STB_DIR}"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${IMGUIZMO_DIR}"
    "${CMAKE_SOURCE_DIR}"
)

target_include_directories(hsr_renderer PRIVATE
    "${astcenc_SOURCE_DIR}/Source"
)

target_link_libraries(hsr_renderer PRIVATE
    volk
    miniz
    ${GLFW_TARGET}
    astcdec-avx2-static
)

# ── Platform-specific ───────────────────────────────────────────
if(WIN32)
    target_compile_definitions(hsr_renderer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # volk loads Vulkan at runtime — no vulkan-1.lib needed
    # miniaudio WASAPI backend needs COM (ole32) + winmm
    target_link_libraries(hsr_renderer PRIVATE ole32 winmm)
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
