#!/usr/bin/env bash
# One-command Linux build for the Quest Home Editor. Run from anywhere:
#   bash QuestHomeEditor/build_linux.sh
# Produces ./build/Quest Home Editor and ./build/hsl_cook. PhysX is OFF (Windows-only vendored libs); the cook uses the
# device-compatible ColliderBox navmesh, so the Linux build is fully functional. Vulkan is loaded at
# runtime via volk (no Vulkan SDK needed) — you just need the loader + X11/Wayland dev headers.
set -e
cd "$(dirname "$0")/.."   # repo root

# ── deps (Debian/Ubuntu). On Fedora/Arch install the equivalents: vulkan-loader, libX11, wayland, etc. ──
if command -v apt-get >/dev/null 2>&1 && [ -n "$HSR_INSTALL_DEPS" ]; then
  sudo apt-get update
  sudo apt-get install -y cmake ninja-build libvulkan-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev pkg-config
fi

GEN=""; command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S QuestHomeEditor -B build $GEN -DCMAKE_BUILD_TYPE=Release   # PhysX OFF by default (cross-platform)
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Built binaries in $(pwd)/build/:"
echo "  - UI / Editor:  \"$(pwd)/build/Quest Home Editor\""
echo "  - CLI Cooker:   \"$(pwd)/build/hsl_cook\""
echo
echo "Run the editor on any env:  \"./build/Quest Home Editor\" path/to/old_home.apk"
echo "Run the CLI cooker:       \"./build/hsl_cook\" --help"
