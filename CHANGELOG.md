# Changelog

All notable changes to Phoenix Engine are documented here. Dates are ISO-8601.

## [Unreleased]

## [v0.4] - 2026-06-02

### Added
- Phoenix Engine project polish for open-source publishing: CMake presets,
  Linux build guide updates, Gentoo/Nix notes, release checklist updates, and a
  Nix development shell.
- Bot stress-test equipment randomization now uses valid indices discovered from
  character, item, cloak, and vehicle CSV data instead of fixed hardcoded ranges.
- Depth prepass shader assets and renderer-side support for the current v0.4
  rendering path.

### Changed
- Removed vendored portable CMake from the repository. CMake is now a normal
  system dependency on Windows and Linux.
- Updated Windows and Linux build scripts to use the standard CMake preset flow.
- Refined character, bot, renderer, shader, water/sky/effects, and performance
  work accumulated for the v0.4 engine preview.

### Notes
- Runtime `Data/` remains excluded from the repository and should be distributed
  separately.

### Added
- **Effects system** (`src/effects/effect_system.*`): our own procedural,
  texture-free particle effects engine built on the existing billboard pipeline.
  - Reusable effect definitions with up to 3 layers; per-layer emitter shapes
    (point, sphere, ring, disc, cone, line), additive/alpha blend, birth→death
    colour gradient, lifetime, size, speed, gravity, drag.
  - Anchoring via a position+basis transform: world-static (portals, map props),
    entity/bone-attached, or one-shot at a point (attack impacts).
  - `EffectManager`: spawn/move/stop/clear; looping vs one-shot (auto-despawn).
  - Large categorized preset library (~60 effects across Fire, Water, Ice, Wind,
    Earth, Rock, Lightning, Holy, Shadow, Nature, Arcane, Poison, Normal),
    oriented to character spells and map props, built from a compact data table.
  - New `Shockwave` emitter shape (flat radial burst in the XZ plane).
  - ImGui "Effects" window with category filter + effect picker to spawn/preview
    (at character or ahead) and clear; `G` spawns an impact burst at the weapon.
- Unified particle rendering: the weapon aura and all effects now feed a single
  per-frame `ParticleBatch` (alpha then additive) uploaded in one call.

### Notes
- Next iterations for the effects system: textured flipbook layers, mesh-based
  layers (portal rings/shields), a data-driven definition format + editor, and
  bloom/soft-particles for extra polish.

## [v0.3.1] - 2026-05-29

### Added
- Master volume slider in the ImGui panel (applies to all music/sound voices).

### Fixed
- Dungeon mob/NPC placement: coordinates were centred like open-world maps
  (`mapSize/2`), but dungeon geometry is uncentred — actors appeared scattered.
  Use no centring (`halfMap = 0`) for dungeons, matching the geometry.
- Dungeon actor height: dungeons have no terrain heightmap and are multi-level;
  actors snapped to the collision floor nearest the player's Y and collapsed to
  the bottom floor. Use the authored svmap spawn Y instead.
- Dungeon mobs sinking when they move: moving mobs snapped their Y to
  `terrain_height` (~0 in dungeons) every frame. Keep the authored floor Y while
  roaming in dungeons; open-world mobs still follow the terrain.

## [v0.3] - 2026-05-29

This round focused on gameplay/visual features, a large modularity refactor,
startup-time performance, cross-platform (Linux) portability, and bug fixes.
No game data or commercial assets are included — engine/source only.

### Added
- **Procedural weapon aura effects.** Fully shader-generated particle system
  anchored to the equipped weapon's attach bone — no asset files or effect
  folders. Up to 3 stacked layers, each with a birth→death colour gradient and
  controls for intensity, spawn rate, flow speed, lifetime, size, blade length,
  swirl radius/speed, and blade axis. Element presets: fire, ice, holy, poison,
  shadow, arcane. (`src/character/weapon_effect.*`, `shaders/particle.hlsl`,
  Vulkan textureless billboard pipeline.)
- **Per-character weapon/shield attach-bone map.** Default attach bones per
  race/gender/class, with dedicated bones for ranged weapons (bow/crossbow on
  elf rangers; bow/javelin on deatheater hunters). Still overridable live in the
  UI. (`src/character/weapon_bone_map.*`)
- **Default character loadout.** New characters start with a one-hand sword,
  light shield, and cloak design 1.
- **Mounts/vehicles improvements.** Default seat bone 25; mounted movement is
  faster than running on foot (base 9.5 / sprint 14.0 vs 4.6 / 7.4).
- **Performance HUD on Linux.** CPU (per core), RAM, and process memory read
  from `/proc` (previously Windows-only).
- **Bundled portable CMake for Linux** (`external/cmake/`) plus `scripts/run.sh`
  and a friendly `scripts/build.sh` that checks prerequisites and prints
  distro-specific install commands. See `BUILD_LINUX.md`.

### Changed
- **Faster startup.** Warm load reduced from ~13.7 s to ~5.8 s (cold improves
  more), with identical output:
  - Memoise world asset texture-layer resolution by name (was re-resolving +
    `stat`ing per mesh, thousands of times): `load_world_assets` ~5.6 s → ~0.6 s.
  - Build the data index with `lexically_relative` (no filesystem access) and
    reserved maps: indexing ~21k files ~3.9 s → ~0.6 s.
  - Parse world asset models in parallel.
- **Modular refactor (no behaviour change).** Split the largest files:
  - `main.cpp` → `ui/perf_hud.*` (HUD) and `ui/editor_panel.*` (editor panel +
    weather/fog). `main.cpp` ~3.6k → ~2.7k lines.
  - `renderer/vulkan_renderer.cpp` → `vulkan_renderer_internal.h` (private
    `Impl` + shared helpers) and `vulkan_renderer_particles.cpp`.
  - `character/character_system.cpp` → `character/weapon_bone_map.*`.
- **Loading bar** now shifts colour with progress/state
  (orange → yellow → light green → dark green).

### Fixed
- **Minimize/restore freeze.** The swapchain went out-of-date while minimized
  but the window kept the same size, so it was never recreated and the app
  froze until a manual resize. `render_frame` now recreates the swapchain when
  `vkAcquireNextImageKHR`/`vkQueuePresentKHR` report `OUT_OF_DATE`/`SUBOPTIMAL`.
- **Jump/fall animation looping.** A long fall replayed the whole jump clip
  repeatedly. The jump now plays the take-off once and holds a mid-air pose
  (80% of the clip) until landing — for both the on-foot character and mounts.
- **Mounted characters no longer carry weapons/shields.**
- **Actors (mobs/NPCs) missing.** A `.svmap` path is synthetic (used only to
  derive the CSV folder); routing it through a file-existence resolver yielded
  an empty path and skipped all actor loading. Fixed; `svmap/<id>` folder is now
  resolved case-insensitively.
- **Linux portability.** Case-insensitive asset/path resolution, CRLF trimming
  in CSV parsers, forward-slash shader paths + executable-relative resolution,
  `ImGui_ImplVulkan_LoadFunctions` for the `VK_NO_PROTOTYPES`/volk setup, and
  `#include <cstring>` where MSVC allowed it implicitly.

### Removed
- The experimental `.seff` weapon-effect loader and its asset dependency,
  replaced by the procedural weapon aura above.
