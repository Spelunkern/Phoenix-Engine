# .EFT / .EF2 / .EF3 Effect Format

Shaiya's particle-effect files: emitters (fire, smoke, glows, area-of-effect
decals, skill casts, etc.) plus their supporting `.3DE` meshes and `.dds`
textures. This document covers the binary layout, every field's meaning, how
Phoenix Engine renders them, and how to inspect/edit/convert files in
practice.

Sources of truth used while porting this: [cupsocino/effect-renderer](https://github.com/cupsocino/effect-renderer)
(a Three.js renderer, visually validated against the retail client — referred
to below as "the reference") and [matigramirez/Parsec](https://github.com/matigramirez/Parsec)
(a C# reader that agrees on the binary layout but leaves several fields
unnamed).

Engine source: [src/world/eft_loader.h](../src/world/eft_loader.h),
[eft_loader.cpp](../src/world/eft_loader.cpp),
[eft_mesh_loader.h](../src/world/eft_mesh_loader.h),
[eft_mesh_loader.cpp](../src/world/eft_mesh_loader.cpp),
[eft_binary_reader.h](../src/world/eft_binary_reader.h) (parsing);
[src/runtime/effect_particle_system.h](../src/runtime/effect_particle_system.h),
[effect_particle_system.cpp](../src/runtime/effect_particle_system.cpp)
(simulation/rendering); [shaders/gl/effect_particle.vert](../shaders/gl/effect_particle.vert),
[effect_particle.frag](../shaders/gl/effect_particle.frag) (GPU side).

## 1. Where effect files live and how they're referenced

- `data/effects/*.eft` / `.ef2` / `.ef3` — the effect libraries themselves.
- `data/effects/3de/*.3de` — meshes referenced by `EftEffect::meshIndex`.
- `data/effects/dds/*.dds` — textures referenced by `EftEffect::textureIds`.
- A map (`.wld`) references one effect file by name
  (`WldAnalysis::effectFileName`) and places instances of it
  (`WldAnalysis::effectInstances`), each with a world position/orientation and
  an `effectId` that — despite the name — indexes into **`EftLibrary::sequences`**,
  not `EftLibrary::effects` directly (see §3).
- The whole `data/effects` tree must be **lowercase**; the engine's asset
  resolution is case-sensitive-safe elsewhere but this directory is walked
  directly by filename.

## 2. Binary layout

All integers are little-endian `int32`/`uint16`, floats are `float32`, and
strings are length-prefixed (`uint32` length, then that many bytes, latin/
Korean codepage — not transcoded, so effect/sequence names often show as
mojibake in the UI; only the numeric index is meaningful for lookups). See
`phoenix::world::detail::EftReader` for the shared bounds-checked reader.

### File header

```
char[3]   signature        "EFT", "EF2", or "EF3" (selects the variant below)
uint32    meshCount
string[]  meshNames        (meshCount entries — filenames under data/effects/3de/)
uint32    textureCount
string[]  textureNames     (textureCount entries — filenames under data/effects/dds/)
uint32    effectCount
Effect[]  effects          (effectCount entries — see below)
uint32    sequenceCount
Sequence[] sequences       (sequenceCount entries — see below)
```

`EF2` and `EF3` are the same layout as `EFT` except each `Effect` record gets
two extra `int32`s right after `rotationAxis` (see below) — `EF3` additionally
uses the second one as `distanceScaleMode`; `EF2` parses both but the engine
doesn't use them.

### Effect record

One particle-emitter component. Field order (all sequential, no padding):

| Field | Type | Meaning |
|---|---|---|
| `name` | string | Component name (Korean codepage, often garbled in the UI) |
| `velocityRandomEnabled[3]` | bool×3 | Per-axis: re-randomize velocity every tick from `velocityMin/Max` instead of using the one sampled at spawn |
| `loop` | bool | Emitter restarts forever vs. runs once through `emitterDuration` |
| `destinationBlend` | int32 | D3D blend-factor enum (see §4.5 — **currently unused by rendering**, kept for reference/debug display only) |
| `velocityMode` | int32 | 0 = linear, 1/2/3 = swirl around the XZ/XY/YZ plane (see §4.3) |
| `sourceBlend` | int32 | Same caveat as `destinationBlend` |
| `textureLoop` | bool | When the component has multiple `textureIds`: cycle (`true`) vs. clamp to the last frame (`false`) |
| `meshIndex` | int32 | Index into `meshNames`/loaded `EftMesh`s, or -1 for a plain billboard quad |
| `motionPathEnabled` | bool | Treat the mesh as a **spawn-position path** instead of renderable geometry — see §4.2 |
| `delayPerFrame` | float | Seconds per texture-animation frame (also the fallback "one-shot" cooldown) |
| `emitRateMax`, `lifeMax`, `emitRateMin`, `lifeMin` | float | Note the on-disk order: rate-max, life-max, rate-min, life-min |
| `emitterDuration` | float | How long the emitter keeps spawning new particles (loop or not) |
| `swirlSpeed` | float | Radians/sec for `velocityMode` 1/2/3 |
| `unknown18` | float | Parsed, not consumed anywhere yet |
| `emitPositionSpread[3]` | vec3 | Random offset range added to each particle's spawn position |
| `acceleration[3]` | vec3 | Constant world-space acceleration, applied when `gravityEnabled` |
| `emitOrigin[3]` | vec3 | Base spawn offset (local to the emitter) |
| `velocityMin[3]`, `velocityMax[3]` | vec3 | Per-axis random initial velocity range |
| `baseAxis` | int32 | Billboard orientation mode — see §4.1 |
| `gravityEnabled` | bool | Apply `acceleration` every tick |
| `attractEnabled` | bool | Pull particles toward `attractPoint` |
| `attractPoint[3]` | vec3 | World-space (well, emitter-local) attractor position |
| `attractStrength` | float | Attraction accel magnitude |
| `angularVelocityRandom` | bool | Randomize per-particle spin speed between `[min(0,angularVelocity), max(0,angularVelocity)]` instead of using it directly |
| `rotationEnabled` | bool | Apply continuous spin around `rotationAxis` |
| `angularVelocity` | float | Radians/sec |
| `rotationAxis` | int32 | 0 = none, 1/2/3 = the basis's right/up/forward axis (see §4.1) |
| *(EF3 only)* `unused` | int32 | Parsed and discarded |
| *(EF3 only)* `distanceScaleMode` | int32 | Parsed, **not implemented** in the renderer yet |
| `colorFrames` | `count32` + array | RGBA-over-lifetime keyframes: `{r, g, b, a, time}` each |
| `velocityScaleFrames` | `count32` + array | Velocity-multiplier-over-lifetime keyframes: `{value, time}` |
| `scaleFrames` | `count32` + array | Size-over-lifetime keyframes: `{min, max, time}` — min/max are re-rolled randomly per particle at each segment, not just once |
| `mirrorTexture` | bool | UV mode — see §4.6 |
| `initialRotationAxis` | int32 | Axis for the one-time spawn rotation (same 0/1/2/3 mapping as `rotationAxis`) |
| `initialRotationMinDegrees`, `initialRotationMaxDegrees` | int32 | Spawn rotation range in **degrees**; when `max < min` the reference (and this port) treats it as two ranges `[0,max]` ∪ `[min,360]` instead of clamping |
| `textureIds` | `count32` + array | Indices into `textureNames`, cycled per §4.4 |

`count32` fields are `uint32` counts with a sanity cap in the reader (see
`EftReader::count`) to reject corrupt/truncated files instead of allocating
absurd amounts.

### Sequence record

A named group of effect components played together — this is what a map
instance or a skill cast actually references.

```
string    name
uint32    recordCount
Record[]  records          {int32 effectId, float time} × recordCount
```

`effectId` indexes `EftLibrary::effects`. `time` is a start-delay in seconds
(component `i` starts `time` seconds after the sequence itself starts — see
§3).

## 3. Sequences vs. effects — the index that trips everyone up

A map's per-instance `effectId` (from the `.wld`) indexes
**`EftLibrary::sequences`**, not `effects`. A sequence is a bundle: e.g. a
"campfire" sequence might combine 4 raw effect components — flame billboard,
smoke, embers, and a ground glow decal — each with its own start delay. When
placing/spawning an effect, expand every record of the chosen sequence into
one emitter per record (`emitter.elapsed` starts at `-record.time` so it
waits out the delay before its first particle). Indexing `effects` directly
instead of `sequences` was an early, silent bug — most components still
"worked" in isolation, they just weren't grouped/staggered correctly.

The Effects debug panel exposes **both**: normally it drives `sequences`
(matching how the map/game actually invokes effects), but it also has a
"spawn one raw component" tool (`EffectParticleSystem::spawn_debug_component`,
indexing `effects` directly) for isolating exactly which sub-component of a
sequence is misbehaving — this is how the mip/sampler bug in §5 was found.

## 4. Rendering semantics

Each active sequence record becomes one **emitter**: a small particle pool
(`effect_particle_count()` sizes it from the average emit rate × lifetime,
capped to 300) simulated on the CPU every frame
(`EffectParticleSystem::step_emitter`) and turned into instanced billboard
quads or real mesh geometry.

### 4.1 `baseAxis` — orientation basis

Computed per-component every frame in `compute_orientation_basis()`:

| Value | Meaning |
|---|---|
| 0 | Camera-facing billboard (default) — basis = camera's right/up/forward |
| 1 | **Fixed world-lock**: right=(1,0,0), up=(0,0,1), forward=(0,-1,0) — i.e. a quad lying flat in the world XZ plane (ground decals, floor glows) |
| 2 | Horizontal camera-lock: yaws to face the camera but stays upright (up is always world +Y) |
| 3 | Placement-basis-locked: uses the emitter's own placement right/up/forward (map orientation, or the debug panel's character-facing yaw) |

`rotationAxis`/`initialRotationAxis` (0/1/2/3 → none/right/up/forward) resolve
through this *same* basis, before scaling — see `axis_vector()`.

### 4.2 Mesh vs. billboard, and `motionPathEnabled`

A component only renders its actual `.3DE` geometry when it has a valid
`meshIndex` **and** `motionPathEnabled` is false (`EffectParticleSystem::mesh_for`).
When `motionPathEnabled` is true, the mesh is *never* rendered — instead its
**last vertex** is sampled once at spawn time (at tick `spawnSeconds * 30`,
see `sample_mesh_frame_index`/`sample_mesh_vertex`) and added as a one-time
position offset. This lets a vertex-animated mesh act as a moving "path" that
scatters particles, while the particles themselves still render as plain
billboard quads. This matches the reference's `usesMesh = mesh && !motionPathEnabled && meshIndex >= 0`
— confirmed against `cupsocino/effect-renderer` while chasing the bug in §5,
so don't "fix" this into always using the mesh; it's intentional.

Real mesh rendering: vertex-animated meshes (`mesh.frames` non-empty) are
"prebaked" — sampled and transformed to world space on the CPU once per
particle per frame (since each particle may be at a different point in its
own animation) rather than drawn via instancing.

`rotationSign`/`meshFlip`: `.3DE` meshes are authored front-facing along
-Z/-X relative to the placement basis (opposite of the +Z/+X convention
regular SMOD entity meshes use), so real mesh geometry gets `right`/`forward`
flipped (`meshFlip = -1`) and spin direction flipped (`rotationSign = 1` for
mesh, `-1` for billboards) to face the way it was placed instead of exactly
backwards. Don't remove this — it was verified against a waterfall mesh
effect that rendered backwards without it.

### 4.3 `velocityMode` — swirl

0 = plain linear integration (`position += velocity * dt`). 1/2/3 rotate the
velocity vector around the XZ/XY/YZ plane by `swirlSpeed * dt` radians every
tick and add a small extra random kick (`random_velocity` re-sampled with a
time-varying seed) — this is what makes embers/sparks drift in lazy curves
instead of straight lines.

### 4.4 Texture-frame animation

When a component has more than one `textureIds` entry, `select_texture_layer_for()`
picks which one to show based on elapsed time: `frame = floor(elapsed / max(0.033, |delayPerFrame|))`,
then `textureLoop ? frame % count : min(frame, count-1)`. This swaps the
**whole texture** (a distinct file per animation frame — e.g. `FireA00.dds`
… `FireA03.dds`), not a UV sub-rect into a sprite sheet.

### 4.5 Blend factors — a known gap

`sourceBlend`/`destinationBlend` are D3D blend-factor enum values (see
`d3d_blend_to_gl` in `opengl_renderer.cpp` for the mapping table) and the
engine does apply them per-batch (`glBlendFunc`). **However**, cross-checking
against the reference renderer while investigating the bug in §5 turned up
that it does *not* use these fields at all — every component renders with a
flat `THREE.AdditiveBlending`, regardless of what's stored in the file. This
divergence hasn't caused a visible problem in testing so far (most components
use blend combos that look additive-ish anyway), but if a future effect looks
wrong in a way that tracks its specific blend values, this is the first place
to look — the per-effect blend factors may simply not be meaningful data the
real client uses for these components.

### 4.6 `mirrorTexture` — mirrored UV tiling

Several textures (e.g. `lamp001.dds`) store only **one quadrant** of a
symmetric radial glow, with the brightest point at a texture corner rather
than the center. `mirrorTexture` components are meant to sample UV range
`[-1, 2]` (a 3×3 tiled span) with `GL_MIRRORED_REPEAT` wrapping instead of the
usual `[0, 1]`: the corner brightness peak ends up at the *center* of the
assembled quad, and the mirrored tiles meet seamlessly at the boundaries,
reconstructing a full soft circular glow from a quarter of the pixels. Ported
in `effect_particle_system.cpp` (`mirrorUvs` in the quad-geometry builder) and
`opengl_renderer.cpp`'s `debugEffectSampler` (`GL_MIRRORED_REPEAT`). Safe to
apply unconditionally to non-mirror components too, since their UVs never
leave `[0, 1]` where repeat/mirrored-repeat/clamp all sample identically.

This field went unimplemented for a while without causing visible problems,
because the resulting visual defect (a single stretched copy of the texture
instead of a 3×3 mirrored tile) is imperceptible on small billboards and only
became obvious on a large-scale ground decal.

## 5. A real bug, as a worked example (mip-incomplete sampler)

Debug-panel-invoked effects showed flat, hard-edged colored squares on **one
specific component** of a multi-part sequence, never on map-placed effects.
Isolating the exact component with `spawn_debug_component` (§3) pinned it to
a large-scale (world-space) billboard. Root cause: the dedicated debug
texture array (`upload_debug_effect_textures`, unit 3) is uploaded with a
single mip level, but `render_frame` bound `terrainSampler` to that unit —
and `terrainSampler`'s `MIN_FILTER` is `GL_LINEAR_MIPMAP_LINEAR`, which
expects a full mip chain. Per GL rules, a texture whose sampler requests
mipmapped filtering but doesn't have the levels to back it is **mipmap
incomplete**, and sampling it returns opaque black — but *only* for
fragments whose computed LOD needs mip level > 0, which only happens for
large/distant billboards (small ones stay in magnification, LOD < 0, never
triggering it). Fix: a dedicated `debugEffectSampler` (`GL_LINEAR` min/mag,
no mipmap dependency) bound to unit 3 instead of reusing `terrainSampler`.

The takeaway for future format/render bugs: if something only breaks at a
specific *scale* or *distance* (not a specific effect's parameters), suspect
LOD/mip selection before suspecting the parsed data.

## 6. Tooling

### 6.1 In-engine: the Effects debug panel

ImGui → Effects tab. Browses the **entire** `data/effects` catalog (hundreds
of files), loading each library on demand only when selected
(`PhoenixRuntime::effect_library_files()` / `load_effect_library_file()`) —
nothing is preloaded, since effects will eventually be reused for character
skills, not just maps. Features:

- Spawn any sequence at the character's position (with a Y-offset control)
  in normal or one-shot mode.
- Track and clear ground-placed debug spawns.
- "Spawn component": bypasses sequence expansion to spawn one raw
  `library.effects[i]` directly — the isolation tool from §3/§5.

Textures for debug-spawned effects go into their own GPU texture array
(independent of the map's shared terrain array, which is fixed-size once the
map loads) — see `upload_debug_effect_textures`. This is why debug and map
rendering share almost all simulation code but have separate texture-array
plumbing and, as of §5, separate samplers.

### 6.2 Inspecting a file outside the engine

There's no committed CLI dumper (it's cheap enough to write one ad hoc when
needed): a ~100-line `.cpp` that calls `phoenix::world::load_eft(path)` and
prints every field, linked against `eft_loader.cpp` + `eft_mesh_loader.cpp` +
`assets/data_index.cpp` (for `read_file_binary`), builds standalone with MSVC:

```
cl /std:c++20 /EHsc /I src your_dumper.cpp src/world/eft_loader.cpp ^
   src/world/eft_mesh_loader.cpp src/assets/data_index.cpp /Fe:dump_eft.exe
```

Same approach works for `.3DE` (`load_eft_mesh`) or for inspecting a `.dds`
texture's decoded RGBA/alpha (`phoenix::renderer::load_dds` +
`decode_texture_rgba` from `src/renderer/dds_loader.cpp`) — this is exactly
how the `lamp001.dds` quadrant-glow trick in §4.6 was discovered: dump the
alpha channel as text/PGM and look at where it peaks.

### 6.3 Editing / converting

There is no in-house `.EFT` editor. Options in practice:

- **Small parameter tweaks** (rates, lifetimes, colors, scale) can be done by
  hex-editing at the known field offsets once you've located the right effect
  index with the layout table in §2 — every field is a fixed-size primitive,
  so nothing shifts as long as you don't touch the count-prefixed arrays.
- **Structural edits** (adding/removing keyframes, textures, or components)
  need a small script that fully re-reads and re-writes the file per §2 —
  safest to model directly on `eft_loader.cpp`/`eft_mesh_loader.cpp`'s field
  order rather than reverse-engineering from scratch.
- **Visual validation**: cupsocino/effect-renderer (the Three.js reference)
  is the fastest way to check whether an edited file still parses and looks
  right, independent of this engine — it accepts a raw data-folder drop and
  renders any file/sequence/component in-browser.
- New textures/meshes referenced by an edited file must go in
  `data/effects/dds/` / `data/effects/3de/` respectively, lowercase, matching
  the exact filename stored in the `.EFT`'s `textureNames`/`meshNames` tables.
