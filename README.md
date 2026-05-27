<p align="center">
  <img src="res/phoenix_256.png" alt="Phoenix Engine" width="128">
</p>

# Phoenix Engine

Phoenix Engine is an open source MMO engine focused on high performance, modular features, and ease of use. It uses Vulkan as its graphics API and aims to become portable over time, starting with Windows and Linux-oriented builds.

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
- WLD/DG map loading with free-camera viewer mode and playable character mode.
- Character appearance loading with race, armor, face, and hair selection.
- NPC and monster loading from server/map data with nameplates, scale, idle/walk animation, and distance culling.
- Map ambience support for music and sound zones with distance-based fade behavior.
- Water surface rendering, underwater tinting, swimming, floating, and camera-driven movement.
- ImGui runtime controls for map selection, fog, render distance, actor distance, overlays, character selection, and sky/weather styles.
- Procedural sky styles: default, storm, snowstorm, sunset, and night with stars/moon/meteors.

## Repository Layout

```text
src/
  assets/      Data indexing and path resolution.
  audio/       XAudio2 ambience playback.
  character/   Playable character controller and character mesh assembly.
  runtime/     Engine runtime state, map loading, terrain/object scene building.
  platform/    Win32 window/input wrapper.
  renderer/    Vulkan renderer, texture loading, GPU resources.
  world/       File format loaders and actor scene construction.
shaders/       HLSL source and compiled SPIR-V used by the runtime.
res/           Windows icon/resource files.
external/      Vendored third-party dependencies.
scripts/       Helper scripts for local build and shader compilation.
docs/          Public documentation and release notes.
```

## Requirements

- Windows 10/11.
- Visual Studio 2022 Build Tools with MSVC v143.
- Windows SDK.
- A Vulkan-capable GPU and current graphics driver.
- PowerShell 5+.

The repository vendors Vulkan Headers, volk, Dear ImGui, and DXC binaries used for shader compilation. A full Vulkan SDK install is not required for this project layout.

## Build

From the repository root:

```powershell
.\scripts\build.ps1
```

Or directly:

```powershell
& 'C:\BuildTools\MSBuild\Current\Bin\MSBuild.exe' .\PhoenixEngine.sln /p:Configuration=Release /p:Platform=x64 /m
```

The executable is generated at:

```text
bin\x64\Release\PhoenixEngine.exe
```

## Shader Compilation

Compiled shader binaries are stored in `shaders/compiled/` because the runtime loads SPIR-V at startup.

To recompile shaders:

```powershell
.\scripts\compile_shaders.ps1
```

## Runtime Data

Phoenix Engine resolves runtime data from the first valid location in this order:

1. `PHOENIX_ENGINE_DATA` environment variable.
2. `Data/` next to the executable.
3. `Data/` in the current working directory.
4. `Data/` in parent directories above the executable, useful for source-tree development.
5. `%LOCALAPPDATA%/Phoenix Engine/Data`.
6. `%PROGRAMDATA%/Phoenix Engine/Data`.

For quick local development, the recommended layout is:

```text
Phoenix Engine\Data\
```

The code references formats such as `.wld`, `.dg`, `.smod`, `.3dc`, `.ani`, `.dds`, `.mon`, `.sdata`, and `.svmap`. These files are user-supplied and are intentionally excluded from the repository.

See [docs/ASSETS.md](docs/ASSETS.md) for more details.

## Usage

1. Keep the `Data/` folder in one of the supported runtime data locations.
2. Launch Phoenix Engine.
3. Use playable mode or free-view mode to explore and test maps.

## Controls

- `W/A/S/D`: move.
- Right mouse drag: camera look.
- Mouse wheel: zoom in playable mode or move camera in viewer mode.
- `Shift`: faster movement.
- `P`: toggle playable mode.
- `F`: toggle fog.
- ImGui panel: map loading, distances, overlays, audio toggles, character selection, and weather/sky style.

## Open Source Notes

- Do not commit extracted game data, logs, local build output, or generated ImGui state.
- Third-party dependency licenses are documented in [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).
- The engine code is licensed under the BSD 3-Clause License. See [LICENSE](LICENSE).
