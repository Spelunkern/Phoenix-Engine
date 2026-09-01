# Phoenix Client Architecture

Phoenix Client v1.0 is a native C++23 application organized as a small MMO
client foundation. SDL2 provides the window and input layer, OpenGL 4.5 handles
rendering, and miniaudio with stb_vorbis handles audio. No game engine or
external UI framework is required.

## Runtime Flow

1. `src/main.cpp` creates the SDL window and OpenGL context.
2. `src/app/bootstrap.*` locates shaders and initializes application services.
3. `PhoenixRuntime` resolves the runtime data directory and parses the selected
   WLD or DG map.
4. CPU-side loaders construct terrain, static-object, collision, actor, water,
   and effect data.
5. GPU resources are uploaded on the render thread.
6. The main loop processes input, advances simulation, builds visible batches,
   renders the frame, updates audio, and draws Phoenix UI.

The SDL event loop and every OpenGL call remain on the main thread. CPU and I/O
work that does not touch OpenGL can run through `LoadingScheduler`; completed
resources return to the main thread before GPU upload. NPC and monster visual
parsing follows the same boundary.

## Source Modules

| Directory | Responsibility |
|-----------|----------------|
| `src/app` | Bootstrap, loading workers, map-session state, and renderer uploads. |
| `src/assets` | Data-root discovery, case-tolerant path resolution, and binary reads. |
| `src/audio` | Music, ambient sounds, one-shots, and master-volume control. |
| `src/character` | Playable controller, animation, equipment, bots, NPCs, and monsters. |
| `src/platform` | SDL window, OpenGL context, and platform input. |
| `src/renderer` | OpenGL resources, passes, texture loading, visibility, and batching. |
| `src/runtime` | Map state, world construction, weather, celestial systems, and particles. |
| `src/ui` | Native immediate-mode Phoenix UI, loading screen, and performance HUD. |
| `src/world` | Bounds-checked readers for supported legacy world and effect formats. |
| `shaders/gl` | GLSL 4.50 shaders compiled at client startup. |

## Rendering Model

The renderer uses an OpenGL 4.5 core context. Major passes include shadow maps,
terrain, static and animated world objects, skinned actors, water, particles,
world labels, sky/weather, and screen-space UI.

- Field terrain uses height data, splat masks, material layers, and baked
  lightmaps.
- Dungeons use DG geometry and authored lightmap pages.
- Static world objects are spatially batched and distance culled.
- The playable character uses cached CPU skinning; crowd actors use shared
  poses and GPU palette skinning where applicable.
- Canonical BC3 DDS textures upload directly. Supported fallback formats are
  decoded and normalized by the client.
- Phoenix UI renders through its own lightweight command list and font atlas.

## Runtime Data Boundary

Commercial data is not part of this repository. The client resolves an
external `data/` tree through `PHOENIX_CLIENT_DATA`, executable-relative paths,
the current working directory, parent development directories, and documented
platform data locations.

All binary readers validate bounds before allocation or access. CPU parsing and
GPU upload are intentionally separate: loaders return ordinary data structures,
while renderer methods create or replace OpenGL resources.

See [ASSETS.md](ASSETS.md) for the supported directory layout and formats.

## Configuration

`phoenix.ini`, stored beside the executable when writable, contains the native
UI position and persistent graphics settings. Runtime data is never written by
the client. Developer-specific `phoenix.ini`, logs, build products, and game
data must not be committed or included in source releases.

## Intended Extension Boundary

This repository ends at the local client/runtime layer. A derived MMO requires
separate server authority, protocol and networking code, authentication,
persistence, gameplay rules, production UI, original content, security, and
deployment infrastructure. Those systems are not partial features hidden in
the v1 source; they are outside the project's final scope.
