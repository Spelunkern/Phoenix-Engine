#!/usr/bin/env bash
# Phoenix Client - one-command Linux build.
#
# Checks prerequisites, prints copy-paste install hints for your distro when
# something is missing, then configures and builds in Release. GLSL shaders
# compile at client startup, so no offline shader toolchain is needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build/linux-release"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# Print the right install command for the detected package manager.
# Args: <apt-pkg> <dnf-pkg> <pacman-pkg> <zypper-pkg> <gentoo-pkg> <nix-pkg>
install_hint() {
    if   command -v apt-get >/dev/null 2>&1; then echo "sudo apt install -y $1"
    elif command -v dnf     >/dev/null 2>&1; then echo "sudo dnf install -y $2"
    elif command -v pacman  >/dev/null 2>&1; then echo "sudo pacman -S --needed $3"
    elif command -v zypper  >/dev/null 2>&1; then echo "sudo zypper install $4"
    elif command -v emerge  >/dev/null 2>&1; then echo "sudo emerge --ask $5"
    elif command -v nix-shell >/dev/null 2>&1; then echo "nix-shell -p $6"
    else echo "(install package providing: $1)"; fi
}

# ---- C++ compiler (C++23) ----
if   command -v g++     >/dev/null 2>&1; then CXX_BIN=g++
elif command -v clang++ >/dev/null 2>&1; then CXX_BIN=clang++
else die "No C++ compiler found. Install one: $(install_hint build-essential gcc-c++ base-devel gcc-c++ sys-devel/gcc gcc)"; fi
say "Compiler: $CXX_BIN"

# ---- pkg-config ----
command -v pkg-config >/dev/null 2>&1 \
    || die "pkg-config missing: $(install_hint pkg-config pkgconf pkgconf pkgconf dev-build/pkgconf pkg-config)"

# ---- SDL2 development files ----
pkg-config --exists sdl2 \
    || die "SDL2 dev files missing: $(install_hint libsdl2-dev SDL2-devel sdl2 libSDL2-devel media-libs/libsdl2 SDL2)"

CMAKE_BIN="$(command -v cmake || true)"
[ -n "$CMAKE_BIN" ] || die "CMake missing: $(install_hint cmake cmake cmake cmake dev-build/cmake cmake)"
say "CMake: $CMAKE_BIN"

# ---- Configure & build ----
say "Configuring (Release)..."
(cd "$ROOT" && "$CMAKE_BIN" --preset linux-release)

say "Building with $(nproc) jobs..."
(cd "$ROOT" && "$CMAKE_BIN" --build --preset linux-release -j"$(nproc)")

echo ""
say "Build complete: $BUILD_DIR/PhoenixClient"
say "Run it with:    ./scripts/run.sh"
