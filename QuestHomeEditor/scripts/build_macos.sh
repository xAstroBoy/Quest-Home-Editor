#!/bin/bash
# hsr_renderer / editor — one-command macOS build (EXPERIMENTAL — Vulkan via MoltenVK).
# Mirrors the CI release workflow: brew deps, CMake+Ninja, PhysX off (Windows-only vendored libs;
# the cooker falls back to the ColliderBox grid — fully functional).
set -e
SDIR="$(cd "$(dirname "$0")/.." && pwd)"
BDIR="$SDIR/build"

echo "================================================"
echo " hsr_renderer — macOS build (MoltenVK)"
echo "================================================"

if ! command -v brew >/dev/null; then
  echo "ERROR: Homebrew required — https://brew.sh" ; exit 1
fi
command -v clang >/dev/null || xcode-select --install
brew install cmake ninja molten-vk vulkan-loader vulkan-headers

echo "Configuring..."
cmake -S "$SDIR" -B "$BDIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_HAVE_PHYSX=OFF

echo "Building..."
cmake --build "$BDIR"

echo ""
echo "================================================"
echo " Build SUCCESS"
echo " Editor:  $BDIR/hsr_renderer"
echo " Run:     ./hsr_renderer <env.apk | .gltf.ovrscene>"
echo " NOTE: runs on MoltenVK (Vulkan->Metal). Untested territory —"
echo " report issues at github.com/xAstroBoy/v79-quest-home-porter"
echo "================================================"
