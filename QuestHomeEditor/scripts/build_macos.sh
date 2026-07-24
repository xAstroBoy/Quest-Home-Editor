#!/bin/bash
# Quest Home Editor / editor — one-command macOS build (EXPERIMENTAL — Vulkan via MoltenVK).
# Mirrors the CI release workflow: brew deps, CMake+Ninja, and a from-source PhysX 4.1.2 (checked)
# build — required for correct collision cooking. On Apple Silicon that comes from the
# PhysX-Gameworks-Apple fork, because upstream 4.1's mac build is hardcoded to x86_64.
set -e
SDIR="$(cd "$(dirname "$0")/.." && pwd)"
BDIR="$SDIR/build"

echo "================================================"
echo " Quest Home Editor — macOS build (MoltenVK)"
echo "================================================"

if ! command -v brew >/dev/null; then
  echo "ERROR: Homebrew required — https://brew.sh" ; exit 1
fi
command -v clang >/dev/null || xcode-select --install
brew install cmake ninja molten-vk vulkan-loader vulkan-headers

# ── PhysX static libs (once per machine) ──
PHYSX_TAG="$(bash "$SDIR/scripts/build_physx.sh" --print-tag)"
if ls "$SDIR/third_party/physx/lib/$PHYSX_TAG"/libPhysXCooking*.a >/dev/null 2>&1; then
  echo "PhysX already built for $PHYSX_TAG — skipping"
else
  echo "Building PhysX 4.1.2 (checked) for $PHYSX_TAG — this takes a while, once..."
  bash "$SDIR/scripts/build_physx.sh"
fi

echo "Configuring..."
cmake -S "$SDIR" -B "$BDIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_HAVE_PHYSX=ON

echo "Building..."
cmake --build "$BDIR"

echo ""
echo "================================================"
echo " Build SUCCESS"
echo " Editor:  $BDIR/Quest Home Editor"
echo " Run:     ./Quest Home Editor <env.apk | .gltf.ovrscene>"
echo " NOTE: runs on MoltenVK (Vulkan->Metal). Untested territory —"
echo " report issues at github.com/xAstroBoy/Quest-Home-Editor"
echo "================================================"
