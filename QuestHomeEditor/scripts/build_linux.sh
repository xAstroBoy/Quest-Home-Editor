#!/bin/bash
# Quest Home Editor / editor — one-command Linux build.
# Installs the system dev packages GLFW/Vulkan need (Debian/Ubuntu apt, Fedora dnf, Arch pacman),
# then configures with CMake (FetchContent auto-downloads GLFW + astcenc; Vulkan headers are
# vendored in third_party/) and builds Release.
# PhysX solid-collision cooking is REQUIRED for correct collision (without it no mesh colliders are
# cooked at all and the floor degrades to a coarse box grid), so build_physx.sh builds PhysX 4.1.2
# from source first. That step is slow (~10 min) but only runs once — it is skipped when the libs
# are already there.
set -e
SDIR="$(cd "$(dirname "$0")/.." && pwd)"
BDIR="$SDIR/build"

echo "================================================"
echo " Quest Home Editor — Linux build"
echo "================================================"

# ── dev packages (same set the CI release workflow uses) ──
if command -v apt-get >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y cmake ninja-build build-essential libvulkan-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev pkg-config
elif command -v dnf >/dev/null; then
  sudo dnf install -y cmake ninja-build gcc-c++ vulkan-loader-devel \
    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
    wayland-devel libxkbcommon-devel pkgconf-pkg-config
elif command -v pacman >/dev/null; then
  sudo pacman -S --needed --noconfirm cmake ninja base-devel vulkan-icd-loader vulkan-headers \
    libx11 libxrandr libxinerama libxcursor libxi wayland libxkbcommon pkgconf
else
  echo "Unknown distro — install manually: cmake, ninja, a C++17 compiler, Vulkan loader dev, X11/Wayland dev headers"
fi

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
echo " Cooker:  $BDIR/hsl_cook"
echo " Run:     ./Quest Home Editor <env.apk | .gltf.ovrscene>"
echo " NOTE: needs a Vulkan-capable GPU driver (vulkaninfo to check)."
echo "================================================"
