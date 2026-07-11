# hsr_renderer source layout

| Dir / file | What lives here |
|---|---|
| `main.cpp` | Entry point + flow: one window from t=0 (drop zone -> GDI loading splash -> Vulkan), worker-thread scene load, progressive GPU upload, render loop, HSR_LIVE HTTP control server, crash handler. |
| `core/` | Cross-cutting basics: `types.h` (MeshData & friends), `camera.h`, `config.h`, `audio.h` (playback), `audio_convert.h` (ogg-vorbis / ogg-OPUS / wav / mp3 / flac -> PCM + WAV re-wrap), `load_progress.h` (loader -> splash progress), `scene_items.h`, `tinyjson.h`. |
| `loaders/` | Env parsing to `MeshData[]`: `scene_loader.h` (V203/HSL RENDMESH+MATL+HSTF), `gltf_loader.h` (V79 .gltf.ovrscene), `opa_loader.h` (V79 official .opa homes), plus the format parsers (`rendmesh/rendtxtr/matlmatl/hstf/rendshad/rendskel/asmh`). |
| `render/` | `vk_renderer.h` (Vulkan renderer: swapchain, per-material programs, texture dedup cache, draw passes, live light/tint push constants), `shadergen`/`universal_shader`/`v79_shader` (SPIR-V), `ibl.h`, `gl_loader`. |
| `ui/` | The from-scratch editor: `ui_font/ui_draw/ui_core` (widget toolkit), `editor.h` (outliner, gizmos, Material/Scene/Cook tabs, sessions, keybinds panel, cook/export UI). |
| `cook/` | V79 -> V203 cooker: `hsl_cooker.h` (APK baker), `hzanim_acl` (skeletal clips), `physx_navmesh`, `cook_verify.h`, `node_rot_fit.h`. |
| `io/` | Blender round-trip: `gltf_export.h` / `gltf_import.h`. |
| `impl/` | Single-TU impls for stb/miniaudio. |
| `shadergen/` | getTime() shader generation (rot/scale/flipbook/uv-scroll/VAT and friends). |

Conventions: header-only modules, one subsystem per header; UI strings are ASCII only
(the stb_truetype UI font has no multi-byte glyphs); loaders report progress through
`g_loadProgress` so the splash/loading bar stays truthful.
