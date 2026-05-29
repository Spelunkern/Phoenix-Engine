# Third-Party Dependencies

This repository vendors a small set of third-party dependencies to keep the build self-contained.

## SDL2

- Path: `external/SDL2/`
- License: zlib
- License file: `external/SDL2/LICENSE.txt`

## Dear ImGui

- Path: `external/imgui/`
- License: MIT
- License file: `external/imgui/LICENSE.txt`

## volk

- Path: `external/volk/`
- License: MIT
- License file: `external/volk/LICENSE.md`

## Vulkan Headers

- Path: `external/Vulkan-Headers/`
- License: Apache-2.0
- License file: `external/Vulkan-Headers/LICENSE.md`

## miniaudio

- Path: `external/miniaudio.h`
- License: MIT-0 (public domain alternative)
- Source: https://miniaud.io

## stb_vorbis

- Path: `external/stb_vorbis.c`
- License: MIT / public domain
- Source: https://github.com/nothings/stb

## DirectX Shader Compiler

- Path: `external/dxc/`
- Licenses: upstream Microsoft/LLVM/MIT license files
- License files:
  - `external/dxc/LICENSE-MS.txt`
  - `external/dxc/LICENSE-LLVM.txt`
  - `external/dxc/LICENSE-MIT.txt`

## CMake (portable Linux build, optional)

- Path: `external/cmake/cmake-*-linux-x86_64.tar.gz`
- License: BSD-3-Clause
- Source: https://cmake.org
- Note: a portable Linux x86_64 CMake distribution, extracted on demand by
  `scripts/build.sh` when no system `cmake` is present. Not used on Windows.

When updating vendored dependencies, keep their license files intact.
