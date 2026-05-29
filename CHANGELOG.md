# Changelog

All notable changes to Phoenix Engine are documented here. Dates are ISO-8601.

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
