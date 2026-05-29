#!/usr/bin/env bash
# Phoenix Engine - one-command Linux build.
#
# Checks prerequisites, prints copy-paste install hints for your distro when
# something is missing, bootstraps a portable CMake if none is on PATH, then
# configures and builds in Release. Shaders ship precompiled in
# shaders/compiled/, so no shader toolchain is needed on Linux.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# Print the right install command for the detected package manager.
# Args: <apt-pkg> <dnf-pkg> <pacman-pkg>
install_hint() {
    if   command -v apt-get >/dev/null 2>&1; then echo "sudo apt install -y $1"
    elif command -v dnf     >/dev/null 2>&1; then echo "sudo dnf install -y $2"
    elif command -v pacman  >/dev/null 2>&1; then echo "sudo pacman -S --needed $3"
    elif command -v zypper  >/dev/null 2>&1; then echo "sudo zypper install $2"
    else echo "(install package providing: $1)"; fi
}

# ---- C++ compiler (C++23) ----
if   command -v g++     >/dev/null 2>&1; then CXX_BIN=g++
elif command -v clang++ >/dev/null 2>&1; then CXX_BIN=clang++
else die "No C++ compiler found. Install one: $(install_hint build-essential gcc-c++ base-devel)"; fi
say "Compiler: $CXX_BIN"

# ---- pkg-config ----
command -v pkg-config >/dev/null 2>&1 \
    || die "pkg-config missing: $(install_hint pkg-config pkgconf pkgconf)"

# ---- SDL2 development files ----
pkg-config --exists sdl2 \
    || die "SDL2 dev files missing: $(install_hint libsdl2-dev SDL2-devel sdl2)"

# ---- Vulkan loader (needed at run time) ----
if ! { ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so'; }; then
    warn "Vulkan loader (libvulkan.so) not found. Install before running:"
    warn "    $(install_hint libvulkan1 vulkan-loader vulkan-icd-loader)"
fi

# ---- CMake ----
# Preference order:
#   1. cmake already on PATH (use the system one),
#   2. the portable CMake bundled in external/cmake/ (extracted locally, no net),
#   3. download a portable copy as a last resort.
# The vendored tarball makes the build work offline across distros without
# requiring the user to install CMake system-wide.
CMAKE_VER="3.29.6"
CMAKE_DIST="cmake-${CMAKE_VER}-linux-x86_64"
CACHE="$ROOT/.cmake-portable"
VENDORED_TGZ="$ROOT/external/cmake/${CMAKE_DIST}.tar.gz"

CMAKE_BIN="$(command -v cmake || true)"
if [ -z "$CMAKE_BIN" ]; then
    CMAKE_BIN="$CACHE/${CMAKE_DIST}/bin/cmake"
    if [ ! -x "$CMAKE_BIN" ]; then
        mkdir -p "$CACHE"
        if [ -f "$VENDORED_TGZ" ]; then
            say "Using bundled CMake from external/cmake/ ..."
            tar -xzf "$VENDORED_TGZ" -C "$CACHE"
        else
            warn "No system or bundled cmake; downloading a portable copy ..."
            url="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/${CMAKE_DIST}.tar.gz"
            if   command -v curl >/dev/null 2>&1; then curl -fL "$url" -o "$CACHE/cmake.tgz"
            elif command -v wget >/dev/null 2>&1; then wget -O "$CACHE/cmake.tgz" "$url"
            else die "Need curl or wget to fetch cmake, or install it: $(install_hint cmake cmake cmake)"; fi
            tar -xzf "$CACHE/cmake.tgz" -C "$CACHE"
            rm -f "$CACHE/cmake.tgz"
        fi
    fi
fi
say "CMake: $CMAKE_BIN"

# ---- Configure & build ----
say "Configuring (Release)..."
"$CMAKE_BIN" -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

say "Building with $(nproc) jobs..."
"$CMAKE_BIN" --build "$BUILD_DIR" -j"$(nproc)"

echo ""
say "Build complete: $BUILD_DIR/PhoenixEngine"
say "Run it with:    ./scripts/run.sh"
