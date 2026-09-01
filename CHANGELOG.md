# Changelog

All notable changes to Phoenix Engine are documented here. Dates are ISO-8601.

## [Unreleased]

### Added
- **Native Phoenix UI.** The debug panel, performance HUD, controls reference,
  and graphics controls now render through the engine's own lightweight
  screen-space UI. Graphics settings include persistent VSync, anti-aliasing,
  FPS cap, world shadows, and render/fog distances through `phoenix.ini`.
- **Native world labels and canonical entity names.** NPC and monster names no
  longer depend on a debug UI backend; monster names appear on hover and use
  the matching names imported from the canonical game data when available.

### Changed
- **Godot presentation parity.** World handedness, playable-character
  orientation and controls, camera behavior/collision, environment lighting,
  shadows, sky, water, cloak simulation, default equipment, and map-effect
  composition/timing now follow the established Phoenix Godot client as
  closely as the C++ renderer permits.
- **Aggressive world LOD is now the default.** Terrain detail is preserved,
  while static world objects and actors use distance-aware batching and LOD.
  Fog distance consistently controls world-asset visibility, and asset/effect
  selectors no longer have fixed index ceilings.
- **World texture sampling matches the canonical client more closely.** Static
  assets use stable trilinear filtering and regenerated mip chains derived
  from their base texture level to reduce movement shimmer.
- **Texture fallback support is explicit.** Canonical BC3 DDS data still takes
  the fast path; PNG, BMP, TGA, BC1/BC2, and uncompressed inputs are accepted
  and normalised when needed.

### Fixed
- Camera penetration into terrain, reversed lateral movement/dodge input, and
  inconsistent character/cloak lighting.
- Map-effect sequence composition and timing differences from the canonical
  client.
- A missing or unreadable binary asset can no longer turn a failed stream
  length (`tellg() == -1`) into a huge allocation and terminate the runtime.
- Negative bot spawn counts are rejected instead of converting to an enormous
  unsigned reservation.
- Loader bounds, image-size arithmetic, signed effect-name sanitisation, and
  partial OpenGL buffer update checks are now overflow-safe.

### Removed
- Dear ImGui, its SDL/OpenGL backends, icon resources, settings integration,
  and build dependency after the native UI replacement became functional.
- Obsolete sky presets and the unused aurora shader path.
- Unreachable legacy terrain/object rendering code and the permanently
  disabled compute-culling scaffolding.

## [v0.10] - 2026-07-19

### Changed
- **Perf HUD icons moved from hardcoded arrays to PNG files.** The Windows/
  Linux/Nvidia/AMD/Intel icons in `src/ui/perf_hud_icons.h` (raw RGBA byte
  arrays baked into the source) became loose `res/icons/*.png` resources
  loaded through the new PNG loader. The generated header was removed.

### Added
- **PNG loading via stb_image.** Vendored `external/stb_image.h` (same
  nothings/stb family already used for `stb_vorbis.c`), built PNG-only
  (`STBI_ONLY_PNG`) with file I/O going through the existing
  `read_file_binary`/case-insensitive path resolution instead of stb's own
  `fopen` path, for consistency with every other asset loader. New
  `src/renderer/png_loader.{h,cpp}` (`PngImage load_png(path)`) decodes to
  8-bit RGBA for UI texture uploads.

### Performance
- **CPU skinning: skin matrices hoisted out of the per-vertex loop.**
  `CharacterSystem::skin_and_transform()` recomputed the combined skin matrix
  (`mat4_from_shaiya_transposed` + `mat4_multiply`) for every influence of
  every vertex, even though it only depends on the palette entry — roughly
  vertices×3 redundant 4x4 matrix ops per frame instead of one per distinct
  bone (~two orders of magnitude more math than needed on a fully equipped
  character). Both the rider and mount loops now memoise the matrix per
  palette entry (bit-identical results — same operations, computed once). The
  cache is `thread_local` because bot pose skinning runs this method
  concurrently on the loading worker pool.

- **`compute_client_finals()`: 2 of its 3 per-call heap allocations removed.**
  Called up to 3x per frame per character (base animation, blend-out
  animation, mount) it built 3 fresh `std::vector<Mat4>` every call
  (`rawMatrices`, `locals`, `finals`). `rawMatrices`/`locals` are pure
  scratch — never returned, never visible outside the function — so they're
  now `thread_local` reused buffers (same reasoning/pattern as the skin-matrix
  cache above: bot pose skinning calls this concurrently on the loading
  worker pool, one call at a time per thread). `finals` still allocates
  fresh on each call since two results (current + previous animation) are
  kept alive at once for blending — removing that one safely would need an
  out-parameter per call site, left for later.

- **A few small per-frame allocations/lookups removed** (low-risk, mechanical
  cleanups):
  - `CharacterSystem`'s cloak cloth-normal recompute allocated a fresh
    `std::vector<float>` every frame it ran; now a reused instance member
    (`clothRefNormals_`).
  - `BotManager`'s pose-refresh tick (~30x/s) allocated a fresh
    `std::vector<PoseSkinWork>` every tick; now a reused instance member
    (`skinWork`).
  - `BotManager::update()` looked up `visualPresets[pi]` three separate times
    per visible bot per frame; now cached once as a pointer
    (`presetForBot`) and reused.

- **Redundant GL state changes in `render_frame()` removed.** Every draw call
  used to unconditionally re-issue `glUseProgram`/re-upload the 60-float
  camera UBO even when the previous draw already used the identical program
  and constants blob (most notably monster→NPC, both on
  `skinnedCharacterProgram` with the same camera data). `OpenGLRenderer::render_frame`
  now tracks the last-bound program and last-uploaded constants pointer and
  skips the call when unchanged.
- **Per-frame audio scene re-evaluation throttled.** `build_audible_tracks_into()`
  (distance checks + a full sort of every ambient sound emitter against the
  listener) ran unconditionally every frame in `main.cpp`. Sound sources don't
  move and don't need frame-perfect updates, so it's now throttled to ~150ms;
  `AudioSystem::update()` still runs every frame so volume fades stay smooth.

- **CPU skinning: the full per-frame bind-pose copy removed.**
  `CharacterSystem::skin_and_transform()` started every call with
  `animated = data_.bindVertices` — a full vertex-buffer copy, every frame,
  per character, even though skinning only ever touches a subset (the
  equipped-part ranges). The local-space working buffer (`localSkinned_`) is
  now a persistent member re-seeded from `bindVertices` only when it's
  actually stale (on `load()`, tracked by `localSkinnedValid_`) instead of
  every frame. This required separating the function's local-space and
  world-space data, which used to alias the same buffer (the world-transform
  step rewrote position/normal/color/textureLayer in place): the
  world-transform loop now reads from `localSkinned_` and writes into the
  existing `worldVertices_` member instead, so untouched vertices can never
  be transformed to world space twice. Verified by full read-through of every
  downstream use of the buffer (weapon-attach math reads bone matrices
  directly and is unaffected; the cloak cloth simulation already correctly
  expected world-space data and now reads/writes `worldVertices_`).

- **NPC/monster instance grouping: O(models) rescan removed.**
  `NpcManager`/`MonsterManager` grouped visible instances by model for
  batched instanced draws by deduplicating models with a `std::find` scan,
  then re-scanning the *entire* visible list once per unique model to collect
  its instances — O(visible × unique models) instead of O(visible). Both now
  bucket by model in a single pass (`unordered_map<model, vector<instance>>`,
  built while iterating the visible list once), then append each bucket
  contiguously in first-seen order. Same output ordering and batch layout,
  fewer comparisons per frame in scenes with many NPCs/monsters of few
  distinct models.

### Changed
- **Renamed the renderer from Vulkan-branded to OpenGL-branded naming.**
  `VulkanRenderer` → `OpenGLRenderer`, `src/renderer/vulkan_renderer.{h,cpp}` →
  `opengl_renderer.{h,cpp}`, `vulkan_renderer_internal.h` →
  `opengl_renderer_internal.h`. Naming only — no logic changed. The engine has
  been an OpenGL 4.5 renderer since it was ported from the original Vulkan
  implementation; keeping the old "Vulkan" name in the class/files was
  confusing for a backend that hasn't used Vulkan in a long time.

### Fixed
- **Stale VANI geometry leaking between maps (the real cause of the green
  "phantom" shapes in dungeons).** `uploadCurrentWorld()` in `main.cpp` only
  called `set_animated_object_mesh()` when the new map's animated-object scene
  was non-empty. `set_animated_object_mesh()` itself destroys the old GPU
  buffers and clears `animatedObjectsReady` unconditionally before checking
  for empty input — but skipping the call entirely (as most dungeons do,
  since they have zero VANI/vertex-animated decor) left the **previous**
  map's animated buffers, instance transforms, and ready flag untouched, so
  its foliage/decor kept rendering at its old coordinates inside the new map.
  This explains why the shapes' layout depended on whichever map was loaded
  right before, and why loading a dungeon directly (no prior map) never
  showed them. Fixed by always calling `set_animated_object_mesh()`, letting
  it clear stale state even when the new scene has nothing to upload.

### Removed
- **Collision debug visualization.** Removed the "Collision" checkbox from
  the Display panel and its whole rendering path: `OpenGLRenderer::set_debug_mesh`/
  `set_debug_visible`, the debug VAO/vertex/index buffers, the debug draw
  call in `render_frame`, and the `uploadDebugGizmos()` wireframe-mesh builder
  in `main.cpp`. Real world collision (`WorldCollisionMesh`, used for character
  movement blocking, spawn placement, and line-of-sight checks) is untouched —
  only the green debug overlay that visualized it is gone.

### Fixed
- **Stray solid-colour shapes in some interiors/dungeons.** Several `.smod`
  building/shape/furniture models bundle a small non-visual helper sub-mesh
  with a blank texture name (e.g. `b1_guildhouse_01.smod`, `table_a_*.smod`)
  — a leftover from the original data, never meant to be drawn. The renderer
  was drawing it anyway via the "no texture" vertex-colour-only fallback,
  which showed up as small floating solid-colour shapes in incoherent
  positions. `PhoenixRuntime::load_world_assets()` (`src/runtime/phoenix_runtime.cpp`)
  now skips any `.smod`/`.dg` sub-mesh with an empty texture name entirely,
  matching how the original client treats them.

### Removed
- **`dds_normalize` moved out of this repo.** It's a one-shot data-prep tool,
  not engine code, so mixing it into the engine's source tree/build didn't
  make sense. Moved to its own standalone project (`tools/dds_normalize.cpp`,
  `src/renderer/dds_loader.*`, `src/assets/data_index.*` copies + its own
  `CMakeLists.txt`) outside this repo; removed the `dds_normalize` CMake
  target and the `tools/` directory from here entirely. Usage docs in
  README.md/`docs/ASSETS.md` now just reference the standalone tool by name.

### Changed
- **Settings consolidated into imgui.ini.** `display.ini` (character shadow
  toggle) and `perf_hud.ini` (FPS cap) are gone; both are now a custom
  `[PhoenixSettings][Config]` section inside the existing `imgui.ini`, wired
  up via a real `ImGuiSettingsHandler` (`src/ui/app_settings.*`). One file
  next to the executable instead of three. Values load synchronously right
  after `initialize_imgui()` (before the loading screen needs them) and are
  force-flushed on both hard-exit paths (`std::_Exit` on window close),
  since those bypass ImGui's own on-shutdown save.

### Removed
- **Client-side portal teleportation.** Removed `src/world/portal_runtime.*`
  (`check_portal_activation`) and all the map-load/teleport-queue wiring in
  `main.cpp`. Portal-triggered map transitions will be server-authoritative
  going forward, not something the client decides on its own.
- **macOS detection/icon in the performance HUD.** The engine only targets
  Windows and Linux, so the Mac icon asset and OS-name sniffing for it are
  gone from `src/ui/perf_hud.*`/`perf_hud_icons.h`; unrecognized OS names now
  just fall back to the Linux icon.
- **Particle/effects system.** Removed `src/effects/` (`EffectManager`, the
  procedural effect catalog, and portal placement), the weapon-aura system
  (`src/character/weapon_effect.*`), the bot one-shot effects and weapon auras
  in `BotManager`, and the shared particle rendering path
  (`ParticleInstance`/`ParticleBatch`, `vulkan_renderer_particles.cpp`,
  `shaders/gl/particle.vert`/`.frag`). The ImGui "Effects" section, the weapon
  aura controls, and the bot "Bot Effects"/"Weapon Auras" checkboxes are gone
  from the editor panel.
- **Water variants.** Removed the `WaterMode` selector (Ocean, Tropical, River,
  Lake, Cold, Swamp) and its ImGui "Water" combo. Water rendering is now a
  single static "natural" tint baked into the renderer, matching the previous
  default.

## [v0.8] - 2026-06-14

### Added
- **World NPCs from `.svmap`.** Each map's `data/world/<id>.svmap` is parsed for
  NPC placements (`src/world/svmap_loader.*`) and rendered at their authored
  positions, keeping their authored Y (no terrain clamp). The svmap
  `(NpcType, NpcId)` resolves against `npc/npcdata.csv`'s `(npc_type,
  npc_type_id)`; visuals come from `npc/npc.csv`.
  - **Patrol routes.** An NPC entry with multiple authored positions becomes a
    single NPC that occasionally walks (walk clip) between those waypoints,
    instead of one static NPC per point.
- **World monsters from `.svmap`.** Monster spawn areas (a box + per-mob counts)
  populate with the authored number of mobs of each `MobId` (resolved against
  `monster/monsterdata.csv`), scaled by the catalog `size`.
  - **Wandering.** Mobs roam their spawn area: rest, then walk to a random point
    in the box, with an occasional short run, **following the ground** — both at
    spawn and while moving they sample the same height callback the character
    uses (terrain heightmap plus collision-mesh floor), so they stay on the
    surface. (NPCs, by contrast, keep their authored Y and never clamp.)
- **Floating name labels.** Small outlined Arial labels above on-screen actors:
  NPC name (yellow) over type (light blue; hidden for generic `Normal`/`Animal`/
  `DeadNpc`/`GamblingHouse` types), monster name only. Labels are occluded by
  world geometry (collision-mesh segment test), distance-capped, and hold a
  consistent screen-space offset regardless of camera distance.
- **Lazy streaming + async visual loading.** Actors only stream in within camera
  range; a model's mesh/texture/animation parse runs off the render thread on the
  CPU worker pool, so the first sighting of a new actor type no longer hitches.
- **Entity lifecycle and budget.** Actors beyond a despawn radius (with
  hysteresis past the stream range) are freed and re-stream when the camera
  returns; hard caps bound the active count. Under texture-slot pressure, the
  least-recently-used idle visuals are evicted (slots refcounted and returned to
  a free list), bounding GPU texture memory across a long session.
- **Optimised pose skinning.** Each visual precomputes its static skinning plan
  (the referenced bones + their transposed bind matrices), so per-frame skinning
  walks ~dozens of bones instead of every vertex. Distance-based animation LOD
  coarsens the pose-dedup rate for far actors (more share one skinning job).
  Skinning is serial by default and only fans out to the worker pool for very
  large crowds (waking the sleeping pool per frame costs more than it saves at
  normal counts).
- The panel "clear" command for NPCs/monsters now removes only manually-spawned
  actors, leaving the map's `.svmap` actors (which are always present).
- **Dual-wield off-hand defaults.** A per-character off-hand attach bone plus a
  local-space offset/rotation, recovered for dual swords (human/deatheater
  fighters) and claws (elf/vile rangers); elf rangers also get a dedicated
  primary claw bone. Still tunable live from the panel.
- New map/prop particle effects (cursed grave mist, forge heat haze, lantern
  light, and more) with richer per-layer parameters (per-layer turbulence and
  fade controls) on the procedural effect system.

### Changed
- The NPC and monster bone-palette and instance buffers are double-buffered
  across frames-in-flight. A single shared buffer was overwritten by the CPU
  while the previous frame's GPU draw still read it, garbling bone palettes
  (deformed meshes) under load.
- On every map load the NPC/monster visual and texture-slot caches are reset, so
  reused models re-upload their textures into the freshly recreated GPU texture
  array instead of rendering green.

### Removed
- Dead NPC/monster manager state left over from the GPU-skinning move: the
  legacy `TerrainVertex` bind copy, the cached per-vertex source array (now a
  transient parse local), unused per-entity buffer offsets, and the unread
  `active_label`/`texturePaths` fields.

## [v0.7] - 2026-06-11

### Added
- Full lightmap support: field maps (baked shadow/tone `_l.dds` per section)
  and dungeons (per-vertex lightmap page sampling from `<name>_L<i>.dds`),
  applied only when the map itself is a dungeon.
- Alpha-mask terrain splatting ("tonality" maps): field `_a0.._a7.dds` weights
  packed four-per-RGBA into two lightmap-array layers per section, blended in
  the terrain shader with a one-sample fast path.
- VANI vertex-animated decor with distance LOD (animates under 100m, frozen
  beyond, distance-culled by the universal render distance) and MANI rotating
  objects driven entirely on the GPU (Rodrigues rotation packed in instance
  data).
- Ladder climbing: WLD "Object"-section assets (ladders/ivy) no longer collide;
  proximity latches the character into the climb animation (action 18) up to
  the top, ending with a small forward step.
- Occasional idle gestures (idle1/idle2 one-shots between breathing cycles).
- Universal content-based transparency: cutout is decided by each texture's
  actual alpha channel (BC3 block inspection); filename heuristics removed.
  Mounts and cloaks now classify their transparency correctly.
- Canonical DDS pipeline: every texture pre-normalised to BC3 + full mip chain
  by the new `dds_normalize` tool (`tools/`), activating the renderer's
  GPU-native upload path — the load-time conversion stage dropped from ~1.1s
  to ~8ms.
- Per-mount data-driven seat bone (`Bone` column), secondary-rider placeholder
  (`Bone2`), and alternate ride animation flag (`AlternateAnimation`) in the
  vehicle CSVs.
- Device hard-requirement validation at startup (push constant budget) with
  graceful texture-array truncation when a GPU's layer limit is exceeded;
  discrete GPU is now preferred on hybrid systems.
- 24-bit RGB DDS support in the texture loader.

### Changed
- Data tree reorganised and lower-cased: all maps live flat in `data/world/`
  (`<id>.wld`), field lightmaps in `data/world/field/<id>/`, dungeon assets in
  `data/world/dungeon/`; `Assets/` renamed to `entity/` (with `ladder/` →
  `object/`). Legacy capitalised layouts still resolve on case-sensitive
  filesystems.
- Weapon CSVs renamed from numeric ids to type names (`sword1h.csv`, ...) and
  trimmed to the four columns the engine reads; rows with duplicate or missing
  assets removed, along with the orphaned asset files (sound, character,
  weapon, and vehicle data deep-cleaned).
- World asset building parallelised (texture-layer assignment stays
  deterministic); bot pose skinning reuses the persistent worker pool; bot and
  VANI vertex uploads send only dirty ranges; lightmap/mask preparation runs
  across all cores.
- Terrain splat sampling uses explicit-gradient `SampleGrad` with a single
  sample on uniform terrain; instance distance culling moved before rotation
  math and tests the instance origin (fixes screen-covering smeared triangles
  on far VANI objects).
- Footstep sounds only play while grounded (never while jumping, falling,
  swimming, or climbing); changing maps stops all playing audio immediately.
- Dodge displacement now respects world collision; dodging is blocked while
  mounted or climbing; emotes and idle gestures play their full duration.
- The window closes instantly at any moment, including mid-load: every loading
  phase pumps messages (chunked GPU uploads, parallel lightmap prep) and the
  exit path skips teardown entirely.
- Linux RAM metrics in the performance HUD fixed (robust /proc/meminfo parsing
  with MemAvailable fallback).

### Removed
- Dead subsystems: unused world loaders (`mon`, `eft`, `svmap`, `sdata`,
  `phoenix_world`), the never-drawn depth prepass pipelines and shaders, the
  file logging system, WLD preview PNG generation, the unpopulated BC3 RAM
  cache, and assorted write-only fields across the WLD analysis structures.

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
