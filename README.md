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

Requires a C++17 toolchain, the Vulkan SDK, and Ninja. From `hsr_renderer_cpp/`:

```
python build.py
```

The renderer is written to `hsr_renderer_cpp/build/hsr_renderer.exe`.

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
