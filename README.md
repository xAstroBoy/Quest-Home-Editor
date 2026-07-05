# V79 Meta Quest Home Editor &amp; Porter

A tool to **load, edit, and port old V79-era Meta Quest "home" environments** to the
latest Quest home format (V203 / HSL) — with a custom Vulkan renderer and a
Blender-style in-app editor.

---

## Status — work in progress, but a **LOT** now works

Under active reverse-engineering and shared **as-is** as a research project — not a polished
product; bugs, crashes, and visual errors still happen. That said, a lot has been cracked:

**Working now:**

- **V79 → HSL cook → installable, signed APK** — one-press **cook + sign + auto-install**
  (rooted own-package, or a non-root **Haven 2025 spoof**); the full pipeline is solid (see the
  [**Converter Guide (Wiki)**](https://github.com/xAstroBoy/v79-quest-home-porter/wiki)).
- **Geometry, textures, baked lightmaps, and most materials** — per-material shaders with the
  cooked constants bound **by name** (`matParams` = `MurmurHash3` of the member), so tints / masks /
  lightmaps resolve (this fixed the big "everything grey / pink / wrong-colour" blocker).
- **Animation** — V79 node anims (spin / sway / UV-scroll / flipbook), VAT vistas, and skeletal
  **HZANIM skinned meshes** (clouds / dragons / creatures). Large skinned meshes are auto-split into
  device-safe pieces so they **load and animate** without crashing or OOM.
- **Collision & navmesh** — walkable ColliderBox grid + double-sided trimesh, spawn / chair / mesh-collider
  scene items, and far-clip / skybox handling so distant backdrops don't get clipped.

**Still rough:**

- Some PBR material variants (specIBL / parts of the isotropic family) render flat or mis-shaded.
- A few vistas and effects aren't fully faithful; **Quest 2** can still miss some content.
- The device's async asset loader can need **one retry** on the first switch into a heavy skinned env.

Don't expect a finished product — but **most old homes now port, render, and animate.**

---

## What it does (so far)

- Loads V79 `.gltf.ovrscene` + `.opa` environments and V203 / HSL `.apk` homes.
- Renders them in a custom Vulkan renderer, decoding meshes, textures, materials,
  animations, and lightmaps as faithfully as possible from Meta's `libshell`.
- Blender-style in-app **editor** (custom immediate-mode UI): outliner tree with
  per-item / per-mesh **visibility eyes**, click-to-select, move / rotate / scale gizmos,
  undo/redo, mesh + material inspection, scene items (spawn / chair / navmesh / mesh
  collider / wall / hotspot), and always-visible viewport toggles (Move/Rotate/Scale,
  Walk-sim, PC audio, and overlay toggles for navmesh / collision / spawn / far-clip).
  Sessions persist to `saved/<env>.hsledit`.
- **Ports animated content** — V79 node animations (spin/sway/UV-scroll/flipbook) and
  skeletal **HZANIM** skinned meshes (clouds/dragons/creatures); large skinned meshes are
  auto-split into device-safe pieces so they load and animate without crashing.
- **V203 shader decode** — `v203_shader_tool.py` extracts the pre-compiled SPIR-V
  shaders directly out of an environment `.apk` (`scene.zip` → `*.surface/shader_t2w9`
  "SHAD" containers) and reflects each variant's descriptor + vertex interface into a
  manifest. `frida_shaders.js` / `frida_run.py` rip the live runtime GLSL from a rooted
  headset by neutralizing `glProgramBinary` to force a recompile.

## Prebuilt binaries

Every push builds **Windows / Linux / macOS** binaries on CI
([Actions](../../actions)); tagged releases attach them on the
[Releases](../../releases) page — Windows `.zip`, Linux `.AppImage` + `.tar.gz`,
macOS `.dmg` (Apple Silicon + Intel, MoltenVK bundled).

> The shipped binaries are built **without AVX2** so they run on any 64-bit CPU
> (SSE4.1, ~2008+). If your CPU is 2013+ and you cook a lot, build locally with
> `-DHSR_AVX2=ON` for ~1.3× faster texture encoding.

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

### Convert an old home to an installable APK

See the **[Converter Guide (Wiki)](https://github.com/xAstroBoy/v79-quest-home-porter/wiki)** for the full walkthrough. In short: drop `adb.exe` +
`AdbWinApi.dll` + `AdbWinUsbApi.dll` beside the exe, load the old home, open the **Cook** tab, and
press **`COOK + SIGN + INSTALL`**. A cook writes up to two APKs into the **`cooked/`** folder:

- **`<env>_NoRoot-Spoof.apk`** — masquerades as Meta's **Haven 2025** home; the **only** option that
  installs on a **non-rooted** Quest. Install it, then pick "Haven 2025" in the home menu.
- **`<env>_Rooted-System.apk`** — the env's own package; auto-selectable only on **rooted / dev** headsets.

The auto-installer detects root over `adb` and picks the right one for you (rooted → unspoofed +
auto-select; non-rooted → **back up the real Haven 2025**, install the spoof, relaunch the shell).
The pristine backup is kept beside the exe in the **`Haven2025_Backup/`** folder (never overwritten); restore it anytime with `hsr_renderer.exe --restore-haven`.

### Sign a cooked APK

If a shared/cooked home won't install — `INSTALL_PARSE_FAILED_NO_CERTIFICATES` ("Failed to collect
certificate") — it's **unsigned**. Sign it (no re-cook needed):

```
hsr_renderer.exe --sign home.apk [more.apk ...]
```

→ writes `home_signed.apk`, then `adb install home_signed.apk`. **Zero setup:** the Android build-tools are
auto-detected from an installed SDK, and if the machine has **no SDK (and no Java)** they're **auto-downloaded
beside the exe** on first use — Google's official build-tools + a Temurin JRE (needs `curl` + a network
connection; Windows 10+/Linux/macOS all ship `curl`). Pre-fetch them anytime with `hsr_renderer.exe
--fetch-tools`. Freshly cooked APKs are already signed automatically — `--sign` is only for older/shared
unsigned ones.

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
