#!/bin/bash
# hsr_renderer / editor — one-command Linux build.
# Installs the system dev packages GLFW/Vulkan need (Debian/Ubuntu apt, Fedora dnf, Arch pacman),
# then configures with CMake (FetchContent auto-downloads GLFW + astcenc; Vulkan headers are
# vendored in third_party/) and builds Release.
# PhysX solid-collision cooking is Windows-only vendored libs -> OFF here; the cooker falls back
# to the device-compatible ColliderBox grid, so the build is fully functional.
set -e
SDIR="$(cd "$(dirname "$0")/.." && pwd)"
BDIR="$SDIR/build"

echo "================================================"
echo " hsr_renderer — Linux build"
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

echo "Configuring..."
cmake -S "$SDIR" -B "$BDIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHSR_HAVE_PHYSX=OFF

echo "Building..."
cmake --build "$BDIR"

echo ""
echo "================================================"
echo " Build SUCCESS"
echo " Editor:  $BDIR/hsr_renderer"
echo " Cooker:  $BDIR/hsl_cook"
echo " Run:     ./hsr_renderer <env.apk | .gltf.ovrscene>"
echo " NOTE: needs a Vulkan-capable GPU driver (vulkaninfo to check)."
echo "================================================"
