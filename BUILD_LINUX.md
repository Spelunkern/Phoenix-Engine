# Building Phoenix Engine on Linux

## Prerequisites

| Need | Why | Debian/Ubuntu | Fedora | Arch |
|------|-----|---------------|--------|------|
| C++23 compiler | Build the engine | `build-essential` | `gcc-c++` | `base-devel` |
| CMake >= 3.20 | Build system | `cmake` | `cmake` | `cmake` |
| pkg-config | Locate SDL2 | `pkg-config` | `pkgconf` | `pkgconf` |
| SDL2 dev files | Window/input | `libsdl2-dev` | `SDL2-devel` | `sdl2` |
| Vulkan loader | Runtime graphics | `libvulkan1` | `vulkan-loader` | `vulkan-icd-loader` |
| Vulkan driver | GPU ICD | `mesa-vulkan-drivers` or vendor driver | `mesa-vulkan-drivers` or vendor driver | `vulkan-radeon` / `nvidia-utils` / vendor driver |

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

## Build

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release -j"$(nproc)"
```

## Run

Place `Data/` next to the executable or in the repository root, then:

```bash
./build/linux-release/PhoenixEngine
```

Or point to data explicitly:

```bash
PHOENIX_ENGINE_DATA=/path/to/Data ./build/linux-release/PhoenixEngine
```

## Troubleshooting

- `Could not initialize Vulkan`: install a Vulkan driver for your GPU and check
  with `vulkaninfo` from `vulkan-tools`.
- Shaders fail to load: confirm `shaders/compiled/` exists next to the executable
  or in the working directory.
- Wrong or missing models on Linux: keep the `Data/` tree intact. The engine has
  case-insensitive path resolution, but it cannot recover from missing files.
