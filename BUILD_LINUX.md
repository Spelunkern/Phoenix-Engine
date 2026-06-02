# Building Phoenix Engine on Linux

Fast path:

```bash
./scripts/build.sh
./scripts/run.sh
```

`build.sh` checks prerequisites, configures a Release build, and compiles the
engine. Precompiled SPIR-V shaders are committed under `shaders/compiled/`, so
Linux builds do not require DXC, glslang, or the Vulkan SDK.

## Prerequisites

| Need | Why | Debian/Ubuntu | Fedora | Arch | openSUSE | Gentoo | Nix |
|------|-----|---------------|--------|------|----------|--------|-----|
| C++23 compiler | Build the engine | `build-essential` | `gcc-c++` | `base-devel` | `gcc-c++` | `sys-devel/gcc` | `gcc` |
| CMake >= 3.20 | Build system | `cmake` | `cmake` | `cmake` | `cmake` | `dev-build/cmake` | `cmake` |
| pkg-config | Locate SDL2 | `pkg-config` | `pkgconf` | `pkgconf` | `pkgconf` | `dev-build/pkgconf` | `pkg-config` |
| SDL2 dev files | Window/input | `libsdl2-dev` | `SDL2-devel` | `sdl2` | `libSDL2-devel` | `media-libs/libsdl2` | `SDL2` |
| Vulkan loader | Runtime graphics | `libvulkan1` | `vulkan-loader` | `vulkan-icd-loader` | `vulkan-loader` | `media-libs/vulkan-loader` | `vulkan-loader` |
| Vulkan driver | GPU ICD | `mesa-vulkan-drivers` or vendor driver | `mesa-vulkan-drivers` or vendor driver | `vulkan-radeon` / `nvidia-utils` / vendor driver | Mesa or vendor driver | Mesa or vendor driver | Host driver stack |

GCC 13+ or Clang 17+ is recommended.

## Install Commands

Debian/Ubuntu:

```bash
sudo apt install -y build-essential cmake pkg-config libsdl2-dev libvulkan1 mesa-vulkan-drivers
```

Fedora:

```bash
sudo dnf install -y gcc-c++ cmake pkgconf SDL2-devel vulkan-loader mesa-vulkan-drivers
```

Arch:

```bash
sudo pacman -S --needed base-devel cmake pkgconf sdl2 vulkan-icd-loader
```

openSUSE:

```bash
sudo zypper install gcc-c++ cmake pkgconf libSDL2-devel vulkan-loader
```

Gentoo:

```bash
sudo emerge --ask sys-devel/gcc dev-build/cmake dev-build/pkgconf media-libs/libsdl2 media-libs/vulkan-loader
```

Nix:

```bash
nix-shell
./scripts/build.sh
```

The repository includes `shell.nix` for a development shell with compiler,
CMake, pkg-config, SDL2, and Vulkan loader/tools.

## Manual CMake Build

Using presets:

```bash
cmake --preset linux-release
cmake --build --preset linux-release -j"$(nproc)"
```

Without presets:

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release -j"$(nproc)"
```

Run from the repository root:

```bash
./scripts/run.sh
```

## Game Data

The engine looks for `Data/` in standard runtime locations and in the repository
root. For local development, this layout is the simplest:

```text
Phoenix Engine/
  Data/
  build/
  scripts/
  src/
```

You can also point to data explicitly:

```bash
PHOENIX_ENGINE_DATA=/path/to/Data ./scripts/run.sh
```

## Troubleshooting

- `Could not initialize Vulkan`: install a Vulkan driver for your GPU and check
  with `vulkaninfo` from `vulkan-tools`.
- Shaders fail to load: confirm `shaders/compiled/` exists and run from the repo
  root with `./scripts/run.sh`.
- Wrong or missing models on Linux: keep the `Data/` tree intact. The engine has
  case-insensitive path resolution, but it cannot recover from missing files.
