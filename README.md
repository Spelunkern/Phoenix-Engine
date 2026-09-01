# Phoenix Client

Phoenix Client is an open-source C++ foundation for MMO game clients. It
provides a complete rendering and gameplay-preview runtime for supported data,
using OpenGL 4.5 on Windows and Linux.

**Version 1.0 is the stable, final release.** The project is complete within its
defined scope and is not planned to receive further feature updates.

## Scope

Phoenix Client is designed to be a technical base from which a team can study,
prototype, or build an MMO client. It includes the difficult low-level pieces:
legacy data loading, rendering, animation, world presentation, input, audio,
streaming, debugging tools, and a playable preview mode.

It is deliberately **not**:

- A finished game.
- A game server, networking stack, account service, database, launcher, patcher,
  or content distribution system.
- A turnkey MMO product that can become a production game overnight.
- A redistribution of commercial game data or assets.

Turning this foundation into a real MMO still requires original content,
server-authoritative gameplay, networking, persistence, authentication,
security, production UI, tools, testing, deployment infrastructure, and the
work needed to integrate all of those systems.

The repository contains source code only. Runtime data and commercial assets
are intentionally excluded. The included playable and free-camera modes exist
to exercise and validate client systems, not to represent a complete game.

## Project Status

Phoenix Client v1.0 is stable and feature-frozen. The source remains available
under the BSD 3-Clause License for study, modification, and use as a base for
independent work, but this repository should be treated as a finished reference
implementation rather than an actively maintained roadmap.

## Included Features

- OpenGL 4.5 renderer with terrain, objects, water, fog, and procedural sky.
- Runtime animation with cached CPU skinning for the playable character and
  GPU palette skinning/pose sharing for NPC and monster crowds.
- WLD/DG map loading with free-camera viewer mode and playable character mode.
- Field lightmaps with baked shadows and colour tones, alpha-mask terrain splatting ("tonality" maps), and full dungeon lightmap support (per-vertex page sampling).
- VANI vertex-animated decor (distance-based animation LOD) and MANI GPU-driven rotating objects.
- World NPCs and monsters placed from each map's `.svmap`: NPCs stand at their authored spots (multi-point groups patrol between waypoints), and monster spawn areas populate with mobs that wander their area and follow the terrain. Entities stream in only within camera range, with off-thread visual loading, distance-based despawn, LRU texture-slot eviction, parallel/LOD pose skinning, and native hover labels. Placement and movement are local preview behavior; server authority is outside this project's scope.
- Character appearance loading with race, armor, face, hair, weapon, shield, and mantle selection.
- Per-race/class weapon and shield attach-bone mapping, with a default starting loadout (one-hand sword + light shield + mantle).
- Mounts/vehicles: ride seated on a data-driven seat bone, mount animations, and faster-than-foot movement.
- Ladder climbing: "Object"-section assets (ladders/ivy) latch the character into the climb animation up to the top instead of colliding.
- Universal content-based transparency: cutout is decided by each texture's actual alpha channel, never by filename heuristics.
- Canonical BC3 texture pipeline with full mip chains. Pre-normalised DDS data
  uploads directly; PNG, BMP, TGA, BC1/BC2, and uncompressed fallback inputs
  are decoded and normalised when loaded.
- Bot stress-test system: GPU-instanced characters with randomized equipment and shared pose skinning.
- Terrain-based footstep sounds (ground-only), map music/sound zones with distance fade, and full audio stop on map change (OGG Vorbis via miniaudio).
- Water surface rendering (natural, static), underwater tinting, swimming, floating, and camera-driven movement.
- Emote animations (one-shot, 10 slots) and occasional idle gestures between breathing cycles.
- Double-tap dodges with full world collision, jump, sit, and swim states.
- Native Phoenix UI controls for map selection, fog/render and actor distance,
  character/loadout selection, mount seat bone, sky/weather, VSync,
  anti-aliasing, FPS cap, and world shadows, plus a CPU/RAM/VRAM performance
  HUD. Persistent graphics values and panel placement live in `phoenix.ini`.
- Procedural sky styles: default, storm, snowstorm, sunset, and night with stars/moon/meteors.
- Instant window close at any moment, including mid-load (the whole loading pipeline stays responsive).

## Repository Layout

```text
src/
  app/       Application setup helpers.
  assets/    Data indexing and path resolution.
  audio/     Audio playback via miniaudio (OGG Vorbis).
  character/ Playable character controller, bots, NPC and monster managers, and character mesh assembly.
  runtime/   Client runtime state, map loading, terrain/object scene building.
  platform/  SDL2 window/input wrapper.
  renderer/  OpenGL 4.5 renderer (split by subsystem), texture loading, GPU resources.
  ui/        Lightweight native editor panel, performance HUD, loading screen.
  world/     File format loaders.
shaders/gl/  GLSL source shaders loaded as text by the runtime at startup.
res/         Windows icon and resource files.
external/    Vendored third-party dependencies.
scripts/     Helper scripts for building.
docs/        Public documentation and release notes.
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Windows build guide](BUILD_WINDOWS.md)
- [Linux build guide](BUILD_LINUX.md)
- [Runtime data and assets](docs/ASSETS.md)
- [System requirements](docs/system-requirements.md)
- [EFT/EF2/EF3 technical format reference](docs/EFT_FORMAT.md)
- [Third-party dependencies](docs/THIRD_PARTY.md)
- [v1.0 release checklist](docs/RELEASE_CHECKLIST.md)
- [Changelog](CHANGELOG.md)

## Supported Platforms

| Platform | Status | Build System |
|----------|--------|-------------|
| Windows 10/11 | Primary | Visual Studio 2022 / MSBuild |
| Linux (X11/Wayland) | Supported | CMake + GCC/Clang |

Both platforms share the same codebase. The platform layer uses SDL2, the renderer uses OpenGL 4.5 core, and the audio system uses miniaudio with stb_vorbis.

## Requirements

### Windows

- Visual Studio 2022 Build Tools with MSVC v143.
- Windows SDK.
- CMake 3.20+ installed on `PATH`.
- An OpenGL 4.5 capable GPU and current graphics driver.

SDL2 is vendored in the repository.

### Linux

- GCC 13+ or Clang 17+ (C++23 required).
- CMake 3.20+ installed on `PATH`.
- SDL2 development libraries.
- An OpenGL 4.5 capable GPU and driver.

Install dependencies on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libsdl2-dev
```

On Fedora:

```bash
sudo dnf install gcc-c++ cmake pkgconf SDL2-devel
```

On Arch:

```bash
sudo pacman -S base-devel cmake pkgconf sdl2
```

## Build

### Windows

From the repository root in PowerShell:

```powershell
cmake -S . -B build/windows-vs2022-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/windows-vs2022-x64 --config Release
```

Output: `bin\x64\Release\PhoenixClient.exe`

### Linux

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release -j$(nproc)
```

Output: `build/linux-release/PhoenixClient`

The repository vendors SDL2 for Windows and the stb headers it uses. Linux
links against the system SDL2 package. No graphics SDK is required beyond an
OpenGL-capable driver.

## Shaders

Shaders live as plain GLSL source under `shaders/gl/*.vert` and `*.frag` and
are compiled by the renderer at startup. There is no offline shader compilation
step; edit a shader and restart the client to see the change.

## Runtime Data

Phoenix Client resolves runtime data from the first valid location in this order:

1. `PHOENIX_CLIENT_DATA` environment variable.
2. `data/` next to the executable.
3. `data/` in the current working directory.
4. `data/` in parent directories above the executable, useful for source-tree development.

Platform-specific fallback locations:

| Platform | Paths |
|----------|-------|
| Windows | `%LOCALAPPDATA%/Phoenix Client/data`, `%PROGRAMDATA%/Phoenix Client/data` |
| Linux | `~/.local/share/Phoenix Client/data` |

The data tree is all-lowercase (legacy capitalised layouts still resolve). Expected layout:

```text
data/
  world/          All maps as flat <id>.wld files, plus <id>.svmap actor placement.
    field/<id>/   Field lightmaps (<id>_<sec>_l.dds) and alpha splat masks (<id>_<sec>_a0..7.dds).
    dungeon/      Dungeon DG models plus per-dungeon lightmap pages (<name>/<name>_L<i>.dds).
  entity/         Placeable world assets by section (building, tree, grass, object = climbables, ...).
  character/      Per-race 3dc/dds/ani plus part and action CSV tables.
  npc/            NPC 3dc/dds/ani plus npc.csv (visuals) and npcdata.csv (id/type/name).
  monster/        Monster 3dc/dds/ani plus monster.csv (visuals) and monsterdata.csv (id/name/size).
  weapons/        Item meshes/textures plus per-type CSVs (sword1h.csv, bow.csv, ...).
  vehicle/        Mount meshes/animations plus vehicle_<class>_01.csv.
  mantles/        Cloak meshes, textures, and per-race CSVs.
  sound/          Map/terrain-referenced OGG audio only.
```

The code references formats such as `.wld`, `.svmap`, `.dg`, `.smod`, `.vani`, `.3dc`, `.3do`, `.ani`, and `.dds`. These files are user-supplied and are intentionally excluded from the repository.

See [docs/ASSETS.md](docs/ASSETS.md) for more details.

## Usage

1. Keep the `data/` folder in one of the supported runtime data locations.
2. Launch Phoenix Client.
3. Use playable mode or free-view mode to explore and test maps.

## Controls

- `W/A/S/D`: move (`A`/`D` strafe).
- `Space`: jump (playable mode) / raise camera (viewer mode).
- Right mouse drag: camera look.
- Mouse wheel: zoom in playable mode or move camera in viewer mode.
- `Shift`: faster movement.
- Double-tap `A` or `D`: lateral dodge.
- `P`: toggle playable mode.
- Phoenix UI panel: map loading, fog, distances, overlays, audio toggles, character/loadout selection, mount, and weather/sky style.

## Data Formats

Maps load directly from the native WLD/DG binary formats, with per-map actor
placement (NPC positions and monster spawn areas) read from `.svmap`. Item,
mount, NPC, and monster catalogs use trimmed CSV tables that contain only the
columns the client consumes:

| Table | Format |
|-------|--------|
| `weapons/<type>.csv` (e.g. `sword1h.csv`, `bow.csv`, `shieldlight.csv`) | `RecordIndex,MeshName,TextureName,AlphaBlendingMode` — deduplicated, one row per unique mesh+texture. |
| `vehicle/vehicle_<class>_01.csv` | `RecordIndex,Name,Walk/Run/Jump/Breath/IdleAnimation,Objects,Bone,Bone2,AlternateAnimation` — `Bone` is the rider seat bone; `AlternateAnimation=1` switches the rider to the variant ride clips. |
| `character/<race>/<prefix>_<part>.csv` | Body part tables (mesh, texture, alpha mode per record index). |
| `character/<race>/<prefix>_action.csv` | Animation clips by action id. |
| `npc/npc.csv`, `monster/monster.csv` | Per-model visual rows: mesh, texture, and the walk/run/attack/death/breath/damage/idle animation names. |
| `npc/npcdata.csv` | NPC catalog: `npc_index,npc_id,npc_type,npc_type_name,npc_type_id,model,name` — the svmap `(NpcType, NpcId)` resolves to `(npc_type, npc_type_id)`. |
| `monster/monsterdata.csv` | Monster catalog: `monster_id,name,model_id,size` — the svmap `MobId` resolves to `monster_id`; `size` is a percentage scale. |

For fastest startup, textures should use the canonical format: **BC3 (DXT5)
with a full mip chain** (256x256 for content textures, native dimensions for
lightmaps). The runtime also accepts PNG, BMP, TGA, BC1/BC2, and uncompressed
DDS inputs and converts them to the upload format when needed.
`dds_normalize` converts a DDS tree to the canonical format in one idempotent
pass; it is a standalone data tool (not part of this repository):

```powershell
dds_normalize.exe data\entity 256 256   # resize + convert
dds_normalize.exe data\world 0 0        # convert only, keep dimensions
```

Audio references (originally `.wav`) are resolved to `.ogg` (Vorbis) files on
disk. Texture references (`.tga`, `.bmp`) are resolved to `.dds` when available.

## License

BSD 3-Clause License

Copyright (c) 2025-2026, Phoenix Client contributors. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
