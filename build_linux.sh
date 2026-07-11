#!/usr/bin/env bash
# One-command Linux build script for Quest Home Editor.
# Run from anywhere inside or outside the repository root:
#   ./build_linux.sh [options]
#
# Options:
#   --appimage       Build portable self-contained .AppImage bundle
#   --tarball        Build portable .tar.gz archive
#   --docker         Build inside a Docker container
#   --install-deps   Install required Debian/Ubuntu build dependencies
#   --avx2           Enable AVX2 SIMD optimizations (-DHSR_AVX2=ON)
#   --help, -h       Display this help message

set -e
cd "$(dirname "$0")"

AVX2_FLAG="OFF"
DO_APPIMAGE=0
DO_TARBALL=0
DO_DOCKER=0
INSTALL_DEPS=0

for arg in "$@"; do
  case "$arg" in
    --appimage) DO_APPIMAGE=1 ;;
    --tarball) DO_TARBALL=1 ;;
    --docker) DO_DOCKER=1 ;;
    --install-deps) INSTALL_DEPS=1 ;;
    --avx2) AVX2_FLAG="ON" ;;
    --help|-h)
      sed -ne '/^#/!q;s/^#* *//p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg (use --help for usage)"
      exit 1
      ;;
  esac
done

if [ "$DO_DOCKER" -eq 1 ]; then
  echo "Building Linux binaries inside Docker container..."
  docker build -f Dockerfile.linux -t quest-home-editor-linux .
  mkdir -p dist
  docker run --rm -v "$(pwd)/dist:/out" quest-home-editor-linux sh -c "cp -r /app/dist/* /out/"
  echo "Docker build complete! Binaries extracted to $(pwd)/dist/"
  exit 0
fi

if [ "$INSTALL_DEPS" -eq 1 ] || [ -n "$HSR_INSTALL_DEPS" ]; then
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y cmake ninja-build libvulkan-dev \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
      libwayland-dev libxkbcommon-dev pkg-config
  else
    echo "Warning: apt-get not found. Please install CMake, Ninja, Vulkan dev headers, X11/Wayland dev libraries manually."
  fi
fi

GEN=""
if command -v ninja >/dev/null 2>&1; then
  GEN="-G Ninja"
fi

echo "Configuring Quest Home Editor for Linux (AVX2=${AVX2_FLAG})..."
cmake -S QuestHomeEditor -B build $GEN -DCMAKE_BUILD_TYPE=Release -DHSR_AVX2="${AVX2_FLAG}"

echo "Building binaries..."
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "=== Build Successful ==="
echo "  Editor GUI:  $(pwd)/build/Quest Home Editor"
echo "  CLI Cooker:  $(pwd)/build/hsl_cook"
echo

if [ "$DO_APPIMAGE" -eq 1 ]; then
  make appimage
fi

if [ "$DO_TARBALL" -eq 1 ]; then
  make tarball
fi
