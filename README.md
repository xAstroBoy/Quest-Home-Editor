# V79 Meta Quest Home Editor &amp; Porter

A tool to **load, edit, and port old V79-era Meta Quest "home" environments** to the
latest Quest home format (V203 / HSL) — with a custom Vulkan renderer and a
Blender-style in-app editor.

---

## ⚠️ DISCLAIMER — WORK IN PROGRESS, **NOT DONE**

This tool is **incomplete and under active development**. Expect broken rendering,
missing features, and rough edges. It is shared **as-is**, as a research /
reverse-engineering work-in-progress — **not** a finished product.

Known unfinished areas:

- **V203 / HSL material rendering is not finished.** PBR materials (lightmap,
  emissive, rgbmasked, the isotropic family) are only partially wired — many surfaces
  render flat, mis-shaded, or with wrong colors. The per-material constants
  (`matParams`) are not yet mapped by name, and some texture/UV mappings are off.
- **The V79 → HSL cooking / repack pipeline is not complete.** You cannot yet produce
  a final, installable ported home.
- **Vistas and many environments are not yet faithful.**

Do not expect a polished result. Bugs, crashes, and visual errors are expected.

---

## What it does (so far)

- Loads V79 `.gltf.ovrscene` + `.opa` environments and V203 / HSL `.apk` homes.
- Renders them in a custom Vulkan renderer, decoding meshes, textures, materials,
  animations, and lightmaps as faithfully as possible from Meta's `libshell`.
- Blender-style in-app **editor** (ImGui + ImGuizmo): outliner tree, click-to-select,
  move / rotate / scale gizmos, undo/redo, mesh + material inspection, editor overlays
  (navmesh / collision / spawn points).
- **V203 shader decode** — `v203_shader_tool.py` extracts the pre-compiled SPIR-V
  shaders directly out of an environment `.apk` (`scene.zip` → `*.surface/shader_t2w9`
  "SHAD" containers) and reflects each variant's descriptor + vertex interface into a
  manifest. `frida_shaders.js` / `frida_run.py` rip the live runtime GLSL from a rooted
  headset by neutralizing `glProgramBinary` to force a recompile.

## Building

The vendored deps are bundled (Vulkan headers, volk, miniz, miniaudio, stb, ACL/RTM);
GLFW + astc-encoder are fetched by CMake. **PhysX is off by default** (the cooker uses the
device-compatible ColliderBox grid, not the incompatible cooked trimesh), so the build is
dependency-light and **cross-platform**. Vulkan is loaded at runtime via volk — no SDK needed.

### Windows
MSVC (C++17) + Ninja. From `hsr_renderer_cpp/`:
```
python build.py          # or: build.bat
```
→ `hsr_renderer_cpp/build/hsr_renderer.exe`.

### Linux
```
sudo apt-get install -y cmake ninja-build libvulkan-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
cmake -S hsr_renderer_cpp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hsr_renderer
```

### macOS
Install MoltenVK first (`brew install molten-vk vulkan-loader vulkan-headers ninja`), then the
same two CMake commands as Linux.

> The UI loads `third_party/fonts/InterVariable.ttf` at runtime — keep it next to the binary on
> Linux/macOS (the Windows Segoe/Arial fallbacks don't exist there).

**Prebuilt binaries:** see [Releases](../../releases). `.github/workflows/release.yml` builds all
three platforms on a `v*` tag — it's committed but **dormant** (GitHub Actions isn't enabled on
this repo); enable Actions or fork to use it.

## Usage

```
hsr_renderer.exe <env.apk | env.gltf.ovrscene>
```

Drag-and-drop an environment onto the window, or pass it on the command line.

## Layout

| Path | What |
| --- | --- |
| `hsr_renderer_cpp/src/` | The renderer, editor, and loaders (C++ headers) |
| `hsr_renderer_cpp/*.py` | Shader generation / analysis helpers |
| `v203_shader_tool.py` | V203 / HSL SPIR-V extractor + interface reflector |
| `frida_shaders.js`, `frida_run.py` | Live runtime-shader rip tooling |

## License

No license is granted yet — this is a personal research project shared for
reference. Reverse-engineering is for interoperability / preservation of old
Meta Quest home environments.
