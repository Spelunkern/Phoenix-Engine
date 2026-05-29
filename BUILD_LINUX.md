# Building Phoenix Engine on Linux

Two commands:

```bash
./scripts/build.sh     # checks prerequisites, configures and builds (Release)
./scripts/run.sh       # launches the engine from the repo root
```

`build.sh` inspects your system first and, if something is missing, prints the
exact install command for your distro (apt / dnf / pacman / zypper). If `cmake`
is not on your `PATH`, it uses the **portable CMake bundled in
`external/cmake/`** (extracted locally into `.cmake-portable/`, works offline,
nothing installed system-wide). Only if that tarball is absent does it fall back
to downloading one.

Shaders are committed precompiled (`shaders/compiled/*.spv`), so **no shader
toolchain (dxc/glslang) is needed on Linux**.

## Prerequisites

| Need                | Why                          | Debian/Ubuntu        | Fedora            | Arch                  |
|---------------------|------------------------------|----------------------|-------------------|-----------------------|
| C++23 compiler      | build the engine             | `build-essential`    | `gcc-c++`         | `base-devel`          |
| CMake ≥ 3.20        | build system (auto-fetched)  | `cmake`              | `cmake`           | `cmake`               |
| pkg-config          | locate SDL2                  | `pkg-config`         | `pkgconf`         | `pkgconf`             |
| SDL2 (dev)          | window / input               | `libsdl2-dev`        | `SDL2-devel`      | `sdl2`                |
| Vulkan loader       | run-time graphics            | `libvulkan1`         | `vulkan-loader`   | `vulkan-icd-loader`   |
| Vulkan driver       | your GPU's ICD               | `mesa-vulkan-drivers`| `mesa-vulkan-drivers` | `vulkan-radeon` / `nvidia-utils` |

One-liner for Debian/Ubuntu:

```bash
sudo apt install -y build-essential cmake pkg-config libsdl2-dev libvulkan1 mesa-vulkan-drivers
```

GCC 13+ or Clang 16+ is recommended for full C++23 support.

## Game data

The engine looks for a `Data/` directory next to the executable, in the repo
root, or one/two levels up — so running via `./scripts/run.sh` (which `cd`s to
the repo root) always finds `Data/` when it sits at the repo root. You can also
point at it explicitly:

```bash
PHOENIX_ENGINE_DATA=/path/to/Data ./scripts/run.sh
```

## Troubleshooting

- **"Could not initialize Vulkan."** — install a Vulkan driver for your GPU and
  verify with `vulkaninfo` (from `vulkan-tools`).
- **Shaders fail to load** — make sure `shaders/compiled/` exists in the repo;
  it is shipped with the source and resolved relative to both the binary and the
  current directory.
- **Wrong/missing models on Linux** — the engine resolves asset paths
  case-insensitively, but the `Data/` tree must be intact.
