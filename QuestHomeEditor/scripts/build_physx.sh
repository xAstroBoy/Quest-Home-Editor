#!/bin/bash
# ── build_physx.sh — build the PhysX 4.1.2 CHECKED static libs on Linux/macOS ────────────────────
#
# WHY THIS EXISTS
#   Collision cooking (cookNavmeshSEBD) is PhysX. Without it the cooker emits NO mesh colliders at
#   all and the smart floor degrades to a coarse ColliderBox grid — see src/cook/physx_navmesh.cpp.
#   Windows has vendored static libs committed under third_party/physx/lib/. Those are MSVC .lib,
#   so Linux/macOS have to build the same SDK from source. This script does that.
#
# WHY "CHECKED" AND NOT "RELEASE"
#   The committed Windows libs are a PX_CHECKED build (the release set is kept beside them in
#   lib/_release_backup/), and CMakeLists.txt compiles our own TU with PX_CHECKED=1. Byte-diffing
#   haven2025's working SEBD proved the device PhysX is a checked build whose serialized struct
#   layout differs from a release image. Mixing configs = wrong field offsets = collision that
#   loads but does nothing. So every platform builds config `checked`.
#
# SOURCES (both are PhysX 4.1.2 — the version must match the vendored headers, asserted below)
#   Linux : NVIDIAGameWorks/PhysX @ 4.1  — upstream; its linux CMakeLists already detects aarch64.
#   macOS : colincornaby/PhysX-Gameworks-Apple @ ApplePlatformPatches — upstream 4.1's mac
#           CMakeLists hardcodes `SET(CMAKE_OSX_ARCHITECTURES "x86_64")` and `-arch x86_64 -msse2`,
#           so it CANNOT produce an Apple Silicon build. This fork adds the PX_OUTPUT_ARCH=arm
#           branch (`-arch arm64` / CMAKE_OSX_ARCHITECTURES "arm64"). Same 4.1.2 version.
#
# OUTPUT
#   third_party/physx/lib/<platform>-<arch>/libPhysX*_static_64.a   (headers stay shared: the
#   already-vendored third_party/physx/include + pxshared/include, identical 4.1.2 for all targets)
#
# USAGE
#   bash scripts/build_physx.sh              # build for this host
#   bash scripts/build_physx.sh --print-tag  # just echo the platform tag (used for CI cache keys)
#   PHYSX_JOBS=4 bash scripts/build_physx.sh # override parallelism
set -euo pipefail

SDIR="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$SDIR/third_party/physx"

# ── platform / arch tag ─────────────────────────────────────────────────────────────────────────
UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"
case "$UNAME_S" in
  Linux)  PLATFORM=linux ;;
  Darwin) PLATFORM=macos ;;
  *) echo "build_physx.sh: unsupported OS '$UNAME_S' (Windows uses the committed third_party/physx/lib/*.lib)" >&2; exit 1 ;;
esac
case "$UNAME_M" in
  x86_64|amd64)  ARCH=x86_64 ; PX_OUTPUT_ARCH=x86 ;;
  arm64|aarch64) ARCH=arm64  ; PX_OUTPUT_ARCH=arm ;;
  *) echo "build_physx.sh: unsupported CPU '$UNAME_M'" >&2; exit 1 ;;
esac
TAG="$PLATFORM-$ARCH"
LIBOUT="$VENDOR/lib/$TAG"

if [ "${1:-}" = "--print-tag" ]; then echo "$TAG"; exit 0; fi

# TARGET_BUILD_PLATFORM is PhysX's own name for the platform ("mac", not "macos").
if [ "$PLATFORM" = macos ]; then
  PX_PLATFORM=mac
  PHYSX_REPO="${PHYSX_REPO:-https://github.com/colincornaby/PhysX-Gameworks-Apple.git}"
  PHYSX_REF="${PHYSX_REF:-ApplePlatformPatches}"
else
  PX_PLATFORM=linux
  PHYSX_REPO="${PHYSX_REPO:-https://github.com/NVIDIAGameWorks/PhysX.git}"
  PHYSX_REF="${PHYSX_REF:-4.1}"
fi

echo "================================================"
echo " PhysX 4.1.2 (checked, static) — $TAG"
echo " source: $PHYSX_REPO @ $PHYSX_REF"
echo "================================================"

command -v cmake >/dev/null || { echo "ERROR: cmake not found"; exit 1; }
command -v git   >/dev/null || { echo "ERROR: git not found";   exit 1; }

SRC="$VENDOR/_src/$TAG"
BUILD="$VENDOR/_build/$TAG"

# ── fetch source (shallow; re-usable across runs) ───────────────────────────────────────────────
if [ ! -d "$SRC/.git" ]; then
  rm -rf "$SRC"
  mkdir -p "$(dirname "$SRC")"
  git clone --depth 1 --branch "$PHYSX_REF" "$PHYSX_REPO" "$SRC"
else
  echo "Reusing existing source checkout at $SRC"
fi

# ── HARD GUARD: the built libs must be the SAME PhysX version as the vendored headers we compile
#    against, or every struct offset in the serialized SEBD silently shifts. ────────────────────
ver_of() { # $1 = PxPhysicsVersion.h
  awk '/#define PX_PHYSICS_VERSION_MAJOR/  {maj=$3}
       /#define PX_PHYSICS_VERSION_MINOR/  {min=$3}
       /#define PX_PHYSICS_VERSION_BUGFIX/ {fix=$3}
       END {print maj "." min "." fix}' "$1"
}
SRC_VER="$(ver_of "$SRC/physx/include/PxPhysicsVersion.h")"
VENDOR_VER="$(ver_of "$VENDOR/include/PxPhysicsVersion.h")"
if [ "$SRC_VER" != "$VENDOR_VER" ]; then
  echo "ERROR: PhysX version mismatch — source is $SRC_VER but third_party/physx/include is $VENDOR_VER." >&2
  echo "       Linking those together would corrupt the cooked SEBD layout. Aborting." >&2
  exit 1
fi
echo "PhysX version check OK: source $SRC_VER == vendored headers $VENDOR_VER"

# ── PhysX 4.1 compiles its own sources with -Werror. Toolchains a decade newer than the SDK emit
#    new warnings (GCC 11+/Clang 14+), which then hard-fail a build that is otherwise fine. Drop
#    -Werror from PhysX's OWN warning flags only; our code's warnings are untouched. ─────────────
for f in "$SRC/physx/source/compiler/cmake/linux/CMakeLists.txt" \
         "$SRC/physx/source/compiler/cmake/mac/CMakeLists.txt"; do
  [ -f "$f" ] || continue
  # portable in-place sed (BSD sed on macOS needs the empty -i arg)
  sed -i.bak 's/-Werror//g' "$f" && rm -f "$f.bak"
done
echo "Stripped -Werror from PhysX's vendor warning flags"

# ── PhysX's public CMakeLists reads several paths from the environment (its generate_projects
#    script exports them); some are SET(... CACHE INTERNAL) so a bare -D would be overwritten.
#    Export them AND pass -D so either path works. ────────────────────────────────────────────────
export PM_CMakeModules_PATH="$SRC/externals/cmakemodules"
export PM_PxShared_PATH="$SRC/pxshared"
export PM_PATHS="$SRC/externals/cmakemodules;$SRC/pxshared"

# Prefer clang on Linux: it is the compiler NVIDIA's own linux presets use, so it is the tested
# path for this SDK. GCC also works. Override with PHYSX_CC / PHYSX_CXX.
if [ "$PLATFORM" = linux ] && [ -z "${PHYSX_CXX:-}" ] && command -v clang++ >/dev/null; then
  PHYSX_CC=clang ; PHYSX_CXX=clang++
fi

GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)

rm -rf "$BUILD"
cmake -S "$SRC/physx/compiler/public" -B "$BUILD" "${GEN[@]}" \
  -DCMAKE_BUILD_TYPE=checked \
  ${PHYSX_CC:+-DCMAKE_C_COMPILER="$PHYSX_CC"} \
  ${PHYSX_CXX:+-DCMAKE_CXX_COMPILER="$PHYSX_CXX"} \
  -DPHYSX_ROOT_DIR="$SRC/physx" \
  -DPXSHARED_PATH="$SRC/pxshared" \
  -DPXSHARED_INSTALL_PREFIX="$BUILD/install" \
  -DCMAKEMODULES_PATH="$SRC/externals/cmakemodules" \
  -DCMAKEMODULES_NAME=CMakeModules \
  -DCMAKEMODULES_VERSION=1.27 \
  -DTARGET_BUILD_PLATFORM="$PX_PLATFORM" \
  -DPX_OUTPUT_ARCH="$PX_OUTPUT_ARCH" \
  -DPX_GENERATE_STATIC_LIBRARIES=ON \
  -DPX_BUILDSNIPPETS=OFF \
  -DPX_BUILDPUBLICSAMPLES=OFF \
  -DPX_FLOAT_POINT_PRECISE_MATH=OFF \
  -DPX_OUTPUT_LIB_DIR="$BUILD/out" \
  -DPX_OUTPUT_BIN_DIR="$BUILD/out" \
  -DNV_FORCE_64BIT_SUFFIX=ON \
  -DNV_USE_GAMEWORKS_OUTPUT_DIRS=OFF

JOBS="${PHYSX_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"
cmake --build "$BUILD" --parallel "$JOBS"

# ── collect: PhysX scatters static libs under bin/<platform.compiler>/<config>/. Flatten them into
#    lib/<tag>/ so CMakeLists finds them the same way it finds the Windows .lib set. ─────────────
rm -rf "$LIBOUT"; mkdir -p "$LIBOUT"
found=0
while IFS= read -r lib; do
  cp -f "$lib" "$LIBOUT/"
  found=$((found + 1))
done < <(find "$BUILD" -name 'libPhysX*.a' -type f)

if [ "$found" -eq 0 ]; then
  echo "ERROR: build produced no libPhysX*.a — nothing to install into $LIBOUT" >&2
  exit 1
fi

# The six archives cookNavmeshSEBD() actually needs. Missing any = link failure later, so fail now.
# PhysX decorates archive names with a bitness/config suffix that varies by platform and by how it
# was configured — Linux/macOS emit libPhysX_x64.a while the Windows set is PhysX_static.lib. Match
# any suffix, but ANCHOR it: a loose lib${want}*.a would let libPhysXCommon_x64.a satisfy "PhysX".
for want in PhysXFoundation PhysXCommon PhysX PhysXCooking PhysXExtensions PhysXPvdSDK; do
  if ! ls "$LIBOUT" | grep -qE "^lib${want}(_[A-Za-z0-9]+)*\.a$"; then
    echo "ERROR: required archive lib${want}*.a missing from $LIBOUT" >&2
    ls -la "$LIBOUT" >&2
    exit 1
  fi
done

# The `_x64` in the filename is PhysX's 64-BIT tag, not an x86 tag — an Apple Silicon build is also
# named _x64. So confirm the real Mach-O arch instead of trusting the name: a silent x86_64 build on
# an arm64 host would only surface later as a confusing link error against the editor.
if [ "$PLATFORM" = macos ] && command -v lipo >/dev/null; then
  got="$(lipo -archs "$LIBOUT"/libPhysXCooking*.a 2>/dev/null || true)"
  case " $got " in
    *" $ARCH "*) echo "Mach-O arch check OK: $got" ;;
    *) echo "ERROR: built PhysX is '$got' but this host is $ARCH." >&2
       echo "       PX_OUTPUT_ARCH=$PX_OUTPUT_ARCH did not take effect — is the source the Apple fork?" >&2
       exit 1 ;;
  esac
fi

echo ""
echo "================================================"
echo " PhysX build SUCCESS — $found archives"
ls -la "$LIBOUT"
echo ""
echo " Now configure the editor with:  -DHSR_HAVE_PHYSX=ON"
echo "================================================"
