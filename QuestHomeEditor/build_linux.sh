#!/usr/bin/env bash
# One-command Linux build for the Quest Home Editor. Run from anywhere:
#   bash hsr_renderer_cpp/build_linux.sh
# Produces ./build/hsr_renderer. PhysX is OFF (Windows-only vendored libs); the cook uses the
# device-compatible ColliderBox navmesh, so the Linux build is fully functional. Vulkan is loaded at
# runtime via volk (no Vulkan SDK needed) — you just need the loader + X11/Wayland dev headers.
set -e
cd "$(dirname "$0")/.."   # repo root

# ── deps (Debian/Ubuntu). On Fedora/Arch install the equivalents: vulkan-loader, libX11, wayland, etc. ──
if command -v apt-get >/dev/null 2>&1 && [ -z "$HSR_NO_APT" ]; then
  sudo apt-get update
  sudo apt-get install -y cmake ninja-build libvulkan-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev pkg-config
fi

GEN=""; command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S hsr_renderer_cpp -B build $GEN -DCMAKE_BUILD_TYPE=Release   # PhysX OFF by default (cross-platform)
cmake --build build --target hsr_renderer -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Built: $(pwd)/build/hsr_renderer"
echo "Run it on any env:  ./build/hsr_renderer path/to/old_home.apk"
echo "(the UI falls back to system fonts; install fonts-noto-cjk for the Chinese/Japanese/Korean UI)"
