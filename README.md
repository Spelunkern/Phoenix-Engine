<p align="center">
  <img src="res/phoenix_256.png" alt="Phoenix Engine" width="128">
</p>

# Phoenix Engine

Phoenix Engine is an open source MMO engine focused on high performance, modular features, and ease of use. It uses Vulkan as its graphics API and runs on Windows and Linux.

The project is still in its infancy. Today it provides a gameplay/preview mode, not a full client and not a server component yet. The repository contains engine/source code only; game data and commercial assets are not included.

## Project Goals

Phoenix Engine has three long-term goals:

- A modern, highly optimized, and visually appealing MMO client.
- A functional server component.
- A portable and flexible codebase that can be used to create MMO games.

The immediate goal is to render classic client content correctly, matching the native game where it matters and improving it where modern rendering can do better. That includes maps, characters, effects, mantles, weapons, shields, monsters, NPCs, vehicles, and related world content.

Phoenix Engine also aims to modernize older visual and technical systems over time. Sky and water rendering are already custom systems instead of direct native-format reproductions. The project should remain modular, understandable, and practical to develop across a long timeline. Eventually, legacy data formats should be converted into modern formats without losing their original properties.

The current runtime is designed to be easy to test through a playable character mode and a free-view camera mode.

## Roadmap

Phase one is focused on engine foundations:

- Render supported client content accurately and efficiently.
- Improve outdated visuals with modern systems where appropriate.
- Keep the architecture modular and friendly to contributors.
- Build a path for converting legacy formats into modern equivalents.

Phase two begins once the initial rendering and gameplay-preview goals are covered:

- Build the server module and the real game layer.
- Transform the gameplay module into a proper client tied to server logic and server-side parameters.
- Recreate a defined suite of content up to a fixed episode, creating a stable base for future work.

Phoenix Engine does not assume deep technical knowledge from final users. Everyone is welcome to test, report issues, document behavior, and contribute where they can. This is intentionally a long-term project.

## Current Features

- Vulkan renderer with terrain, objects, animated actors, water, fog, and procedural sky.
- Runtime skinning for character and actor animations with frame caching for high FPS (GPU compute path + CPU fallback).
- WLD/DG map loading with free-camera viewer mode and playable character mode.
- Character appearance loading with race, armor, face, hair, weapon, shield, and cloak/mantle selection.
- Per-race/class weapon and shield attach-bone mapping, with a default starting loadout (one-hand sword + light shield + cloak).
- Mounts/vehicles: ride seated on the mount's bone, mount animations, and faster-than-foot movement.
- Procedural weapon "aura" effects: fully shader-generated layered particles (no asset files) with birth→death colour gradients and element presets (fire, ice, holy, poison, shadow, arcane).
- NPC and monster loading from server/map data with nameplates, scale, idle/walk animation, and distance culling.
- CSV-based data formats replacing legacy binary formats for monster definitions, NPC data, server metadata, and spawn maps (see [Data Formats](#data-formats)).
- Map ambience support for music and sound zones with distance-based fade behavior (OGG Vorbis via miniaudio).
- Fast, async loading with a responsive loading screen (colour-shifting progress bar) during initialization and map changes.
- Water surface rendering, underwater tinting, swimming, floating, and camera-driven movement.
- ImGui runtime controls for map selection, fog, render distance, actor distance, overlays, character/loadout selection, weapon aura, and sky/weather styles, plus a CPU/RAM/VRAM performance HUD.
- Procedural sky styles: default, storm, snowstorm, sunset, and night with stars/moon/meteors.

## Repository Layout

```text
src/
  assets/      Data indexing and path resolution.
  audio/       Audio playback via miniaudio (OGG Vorbis).
  character/   Playable character controller and character mesh assembly.
  runtime/     Engine runtime state, map loading, terrain/object scene building.
  platform/    SDL2 window/input wrapper.
  renderer/    Vulkan renderer (split by subsystem), texture loading, GPU resources.
  ui/          ImGui editor panel and performance HUD.
  world/       File format loaders and actor scene construction.
shaders/       HLSL source and compiled SPIR-V used by the runtime.
res/           Windows icon/resource files.
external/      Vendored third-party dependencies.
scripts/       Helper scripts for building and shader compilation.
docs/          Public documentation and release notes.
```

## Supported Platforms

| Platform | Status | Build System |
|----------|--------|-------------|
| Windows 10/11 | Primary | Visual Studio 2022 / MSBuild |
| Linux (X11/Wayland) | Supported | CMake + GCC/Clang |

Both platforms share the same codebase. The platform layer uses SDL2, the renderer uses Vulkan through volk, and the audio system uses miniaudio with stb_vorbis — all cross-platform.

## Requirements

### Windows

- Visual Studio 2022 Build Tools with MSVC v143.
- Windows SDK.
- A Vulkan-capable GPU and current graphics driver.

SDL2 is vendored in the repository. No additional downloads needed.

### Linux

- GCC 13+ or Clang 17+ (C++23 required).
- CMake 3.20+ — a portable copy is bundled in `external/cmake/`, so a system install is optional (`scripts/build.sh` uses it automatically when `cmake` is absent).
- SDL2 development libraries.
- Vulkan-capable GPU and driver with ICD loader (Vulkan headers are vendored).

Install dependencies on Debian/Ubuntu:

```bash
sudo apt install build-essential pkg-config libsdl2-dev libvulkan1 mesa-vulkan-drivers
```

On Fedora:

```bash
sudo dnf install gcc-c++ pkgconf SDL2-devel vulkan-loader mesa-vulkan-drivers
```

On Arch:

```bash
sudo pacman -S base-devel pkgconf sdl2 vulkan-icd-loader
```

See [BUILD_LINUX.md](BUILD_LINUX.md) for the full Linux guide (`scripts/build.sh` inspects prerequisites and prints exact install commands for your distro).

## Build

### Windows

From the repository root in PowerShell:

```powershell
.\scripts\build.ps1
```

Or directly:

```powershell
& 'C:\BuildTools\MSBuild\Current\Bin\MSBuild.exe' .\PhoenixEngine.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output: `bin\x64\Release\PhoenixEngine.exe`

### Linux

```bash
./scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Output: `build/PhoenixEngine`. Run it from the repo root with `./scripts/run.sh` so `shaders/` and `Data/` resolve correctly.

The repository vendors Vulkan Headers, volk, Dear ImGui, and DXC binaries used for shader compilation. A full Vulkan SDK install is not required for this project layout.

## Shader Compilation

Compiled shader binaries are stored in `shaders/compiled/` because the runtime loads SPIR-V at startup.

To recompile shaders (Windows):

```powershell
.\scripts\compile_shaders.ps1
```

On Linux, use any HLSL-to-SPIR-V compiler (e.g. `dxc` from the Vulkan SDK):

```bash
dxc -spirv -T vs_6_0 -E VSMain -Fo shaders/compiled/sky.vert.spv shaders/sky.hlsl
dxc -spirv -T ps_6_0 -E PSMain -Fo shaders/compiled/sky.frag.spv shaders/sky.hlsl
```

Pre-compiled SPIR-V is checked into the repository, so shader recompilation is only needed when modifying shader source.

## Runtime Data

Phoenix Engine resolves runtime data from the first valid location in this order:

1. `PHOENIX_ENGINE_DATA` environment variable.
2. `Data/` next to the executable.
3. `Data/` in the current working directory.
4. `Data/` in parent directories above the executable, useful for source-tree development.

Platform-specific fallback locations:

| Platform | Paths |
|----------|-------|
| Windows | `%LOCALAPPDATA%/Phoenix Engine/Data`, `%PROGRAMDATA%/Phoenix Engine/Data` |
| Linux | `~/.local/share/Phoenix Engine/Data` |

For quick local development, the recommended layout is:

```text
Phoenix Engine/Data/
```

The code references formats such as `.wld`, `.dg`, `.smod`, `.3dc`, `.ani`, `.dds`, `.mon`, `.sdata`, and `.svmap`. These files are user-supplied and are intentionally excluded from the repository.

See [docs/ASSETS.md](docs/ASSETS.md) for more details.

## Usage

1. Keep the `Data/` folder in one of the supported runtime data locations.
2. Launch Phoenix Engine.
3. Use playable mode or free-view mode to explore and test maps.

## Controls

- `W/A/S/D`: move.
- `Space`: jump (playable mode) / raise camera (viewer mode).
- Right mouse drag: camera look.
- Mouse wheel: zoom in playable mode or move camera in viewer mode.
- `Shift`: faster movement.
- `P`: toggle playable mode.
- `F`: toggle fog.
- ImGui panel: map loading, distances, overlays, audio toggles, character/loadout selection, mount, weapon aura, and weather/sky style.

## Data Formats

Phoenix Engine replaces several legacy binary formats with human-readable CSV files. This makes the data easier to inspect, edit, and version-control without specialized tooling.

| Legacy Format | CSV Replacement | Content |
|---------------|----------------|---------|
| `.mon` (binary) | `mob.csv`, `npc.csv` | Monster and NPC model definitions: mesh/texture parts, animation slots, sounds, scale, height. |
| `.sdata` (binary) | `monster.csv`, `NpcQuest.csv` | Server-side metadata: monster model IDs, sizes, display names; NPC quest references. |
| `.svmap` (binary) | `svmap/{mapId}/` folder | Spawn and placement data split into `metadata.csv`, `monster_areas.csv`, `monster_spawns.csv`, `npcs.csv`, `npc_positions.csv`. |

Audio references in WLD files (originally `.wav`) are resolved to `.ogg` (Vorbis) files on disk. Texture references (`.tga`, `.bmp`) are resolved to `.dds` when available.

The engine still loads the original WLD, DG, 3DC, SMOD, VANI, MANI, and ANI binary formats for world geometry, models, and animations. Only the data tables above have been migrated to CSV.

## Open Source Notes

- See [CHANGELOG.md](CHANGELOG.md) for notable changes.
- Do not commit extracted game data, logs, local build output, or generated ImGui state.
- Third-party dependency licenses are documented in [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).
- The engine code is licensed under the BSD 3-Clause License. See [LICENSE](LICENSE).
