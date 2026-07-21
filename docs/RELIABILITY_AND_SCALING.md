# Reliability and scaling

This note describes the reliability work in this branch and the constraints it is
designed around. It is intentionally independent of a version or release tag.

## What changes

### Large-scene texture memory

Large legacy environments often reference the same 2K or 4K texture from many
render meshes. Keeping a decoded copy for every mesh makes memory consumption
grow with the number of references rather than with the number of unique images.

The legacy OPA loader and export/cook stages now reuse immutable base-colour
texture payloads when the source and material semantics are compatible. Split
mesh chunks keep sharing the same decoded RGBA or source ASTC payload instead
of copying it into every chunk.
Textures which are edited, atlas-cropped, blended, cut out, or accompanied by
lightmap, normal, ORM, emissive, or VAT data remain independent so reuse cannot
change rendering semantics.

Production scenes also avoid eagerly decoding every fallback material when the
scene already has authored materials. This reduces peak memory without changing
the all-fallback legacy path.

There is **no artificial 1,000-mesh limit** in the editor or cooker. A scene's
practical limit depends on unique texture memory, geometry and entity counts,
cook-time working memory, archive size, and the Quest model/OS renderer. A cook
that succeeds on a workstation can still exceed an on-device resource limit, so
large environments must always be verified on the target headset.

### Safe, deterministic ZIP/APK handling

APK and nested scene archives are treated as untrusted input. Archive entry names
are canonicalized consistently before comparison, and processing rejects:

- absolute, UNC, drive-qualified, and parent-traversal paths;
- control characters and NTFS alternate-stream names;
- duplicate names after separator and dot-segment normalization;
- conflicts with cooker-owned manifest or scene entries;
- classic-ZIP count, filename, entry-size, offset, or total-size overflow.

Every archive initialization, entry append, finalization, and APK splice is
checked. A failed library call or narrowing conversion becomes a cook error
instead of a partially written or silently truncated package. “Deterministic”
here means that equivalent paths are normalized and validated the same way; it
does not promise byte-for-byte identical signed APKs across toolchains.

### Cook validation and diagnostics

Validation runs before the final package is accepted. The cooker reports the
offending archive path, duplicate asset key, reserved-entry conflict, malformed
geometry/index data, or size/format boundary instead of continuing with an
ambiguous best-effort result. Invalid triangles that reference missing vertices
are rejected before large static-mesh splitting begins.

Treat validation errors as source-data problems to fix, not warnings to suppress.
Keep the complete cooker log when reporting a failure; the first explicit error
normally identifies the asset that needs attention. Successful desktop cooking
is not a substitute for the editor's Logcat workflow and an on-device load test.

### Cross-platform build artifacts

The maintained CI configuration builds both deliverables on Windows, Linux, and
macOS:

- `questhomeeditor` — the desktop editor and preview renderer;
- `hsl_cook` — the standalone command-line cooker.

Registered CTest tests run before packaging. Platform archives contain the editor,
the matching cooker, and required runtime resources; standalone cooker artifacts
remain available for automation. Tag publishing is handled by one workflow so two
jobs cannot race to create different assets for the same tag.

## Local build and test

Dependencies are fetched by CMake. Use an out-of-tree build directory and keep the
same configuration for the editor, cooker, and tests.

### Windows

Run from an x64 Visual Studio developer prompt with CMake and Ninja available:

```powershell
cmake -S QuestHomeEditor -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The repository's `QuestHomeEditor/build.bat` and
`QuestHomeEditor/do_build.bat` wrappers locate Visual Studio through `vswhere`
and use the same maintained CMake project.

### Linux

Install the Vulkan and window-system development packages listed in the main
README, then run:

```bash
cmake -S QuestHomeEditor -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DHSR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The repository also provides `make`, `build_linux.sh`, and a Docker build for the
packaged Linux variants.

### macOS

Install Ninja and MoltenVK, then run:

```bash
cmake -S QuestHomeEditor -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DHSR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Useful options

- `-DHSR_BUILD_TESTS=OFF` disables this project's regression tests for a minimal
  local build. CI should leave them enabled.
- `-DHSR_AVX2=ON` enables a faster texture-encoding path on compatible CPUs; it
  is not required for correct output.
- `-DHSR_HAVE_PHYSX=OFF` explicitly selects the default non-PhysX build.

For a clean verification, remove only the chosen out-of-tree build directory,
configure again, build both product targets, then run CTest. Do not validate a PR
only against a stale CMake cache.

## Security and content boundary

This project converts files supplied by the user. It does not ship Meta home APKs,
Haven, `libshell`, signing identities, or other proprietary Meta assets. Tests and
CI must use synthetic fixtures or files that contributors are authorized to share.

Do not commit cooked APKs, extracted Meta assets, local signing keys, device dumps,
or absolute workstation paths. Review generated archives as build artifacts, not
source. The path and size checks above reduce packaging risk, but they do not make
an arbitrary third-party APK trustworthy; obtain input files from a source you
trust and keep normal OS and device installation prompts enabled.
