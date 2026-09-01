# Building Phoenix Engine on Linux

## Prerequisites

| Need | Why | Debian/Ubuntu | Fedora | Arch |
|------|-----|---------------|--------|------|
| C++23 compiler | Build the engine | `build-essential` | `gcc-c++` | `base-devel` |
| CMake >= 3.20 | Build system | `cmake` | `cmake` | `cmake` |
| pkg-config | Locate SDL2 | `pkg-config` | `pkgconf` | `pkgconf` |
| SDL2 dev files | Window/input | `libsdl2-dev` | `SDL2-devel` | `sdl2` |
| OpenGL driver | GPU rendering (4.5 core) | mesa or vendor driver | mesa or vendor driver | mesa or vendor driver |

GCC 13+ or Clang 17+ is recommended.

## Install Commands

Debian/Ubuntu:

```bash
sudo apt install -y build-essential cmake pkg-config libsdl2-dev
```

Fedora:

```bash
sudo dnf install -y gcc-c++ cmake pkgconf SDL2-devel
```

Arch:

```bash
sudo pacman -S --needed base-devel cmake pkgconf sdl2
```

## Build

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release -j"$(nproc)"
```

The build is self-contained apart from the system prerequisites above and does
not download dependencies during configuration. Linux uses the platform
allocator and calls `malloc_trim()` when explicitly releasing large loading
caches.

For a warning-focused local audit build:

```bash
cmake -S . -B build/audit -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2"
cmake --build build/audit -j"$(nproc)"
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

- `Could not create OpenGL context`: install/update the OpenGL driver for your
  GPU (mesa or vendor) and confirm 4.5 core support with `glxinfo | grep "OpenGL version"`.
- Wrong or missing models on Linux: keep the `Data/` tree intact. The engine has
  case-insensitive path resolution, but it cannot recover from missing files.
- Missing shaders after launching the executable directly: keep `shaders/gl/`
  beside the packaged executable. The runtime also searches from its executable
  directory, so the current working directory does not need to be the source
  tree.
