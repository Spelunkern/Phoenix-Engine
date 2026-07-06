# Runtime Data and Assets

Phoenix Engine does not include commercial game data or extracted client/server assets.

Phoenix Engine resolves runtime data from the first valid location in this order:

1. `PHOENIX_ENGINE_DATA` environment variable.
2. `data/` next to the executable.
3. `data/` in the current working directory.
4. `data/` in parent directories above the executable.

Platform-specific fallback locations:

| Platform | Paths |
|----------|-------|
| Windows | `%LOCALAPPDATA%/Phoenix Engine/data`, `%PROGRAMDATA%/Phoenix Engine/data` |
| Linux | `~/.local/share/Phoenix Engine/data` |

For local development, place your own data directory at the project root:

```text
data/
```

The data tree is all-lowercase (legacy capitalised layouts still resolve on
case-sensitive filesystems). The runtime expects this layout:

```text
data/
  world/              All maps as flat <id>.wld files, plus <id>.svmap (actor
                      placement: NPC positions and monster spawn areas).
    field/<id>/       Field lightmaps  <id>_<sec>_l.dds (baked shadow + tone)
                      and alpha splat masks <id>_<sec>_a0..a7.dds per section.
    dungeon/          <name>.dg models plus <name>/<name>_L<i>.dds lightmap pages.
  entity/             Placeable world assets grouped by WLD section:
                      building/ shape/ tree/ grass/ vani/ mani/ terrain/ texture/
                      object/ (climbable ladders & ivy).
  character/<race>/   3dc/ dds/ ani/ plus <prefix>_<part>.csv and <prefix>_action.csv.
  npc/                3dc/ dds/ ani/ plus npc.csv (per-model visuals) and
                      npcdata.csv (id / type / name catalog).
  monster/            3dc/ dds/ ani/ plus monster.csv (per-model visuals) and
                      monsterdata.csv (id / name / size catalog).
  weapons/            3do/ dds/ plus per-type CSVs (sword1h.csv, bow.csv, shieldlight.csv, ...).
  vehicle/            3dc/ dds/ ani/ plus vehicle_<class>_01.csv.
  mantles/            3DC/ DDS/ plus mantle_<race>.csv.
  sound/              OGG audio referenced by maps/terrain only.
```

### File formats

**Binary formats** (loaded as-is from the original client):

- World/map data: `.wld`, `.dg`
- Actor placement: `.svmap` (per-map NPC positions and monster spawn areas)
- Static and animated models: `.smod`, `.vani`, `.mani`, `.3dc`, `.3do`
- Animation: `.ani`
- Textures: `.dds` (see canonical format below), `.bmp`, `.tga`

**CSV tables** (trimmed to the columns the engine consumes):

- `weapons/<type>.csv` — `RecordIndex,MeshName,TextureName,AlphaBlendingMode`,
  deduplicated by mesh+texture pair.
- `vehicle/vehicle_<class>_01.csv` — animations, `Objects` (mesh:texture list),
  `Bone` (rider seat bone), `Bone2` (reserved), `AlternateAnimation` (0/1).
- `character/<race>/<prefix>_<part>.csv` — body part tables by record index.
- `character/<race>/<prefix>_action.csv` — animation clips by action id.
- `npc/npc.csv`, `monster/monster.csv` — per-model visual rows (mesh, texture,
  and the walk/run/attack/death/breath/damage/idle animation names).
- `npc/npcdata.csv` — NPC catalog (`npc_index,npc_id,npc_type,npc_type_name,`
  `npc_type_id,model,name`); the svmap `(NpcType, NpcId)` resolves to
  `(npc_type, npc_type_id)`.
- `monster/monsterdata.csv` — monster catalog (`monster_id,name,model_id,size`);
  the svmap `MobId` resolves to `monster_id`, and `size` is a percentage scale.

**Canonical texture format:**

Every `.dds` is expected as **BC3 (DXT5) with a full mip chain** — 256x256 for
content textures, native dimensions under `world/`. The renderer then uploads
GPU-native with no load-time conversion. New or imported textures should be run
through the standalone `dds_normalize` tool once (idempotent) — it's not part
of this repo; build it separately:

```text
dds_normalize <directory> 256 256   # resize + convert
dds_normalize <directory> 0 0       # convert only, keep dimensions
```

Transparency is decided by each texture's actual alpha content (cutout
auto-detection); character/weapon tables additionally declare their alpha mode
via `AlphaBlendingMode`.

**Audio:**

- `.ogg` (Vorbis) — WLD files reference `.wav` names but the engine resolves them to `.ogg` on disk

The `.gitignore` intentionally excludes `data/` at every repository depth. Keep this rule unless the project later gains a fully original asset pack that is safe to redistribute.

## Legal Boundary

Phoenix Engine is code-first. Contributors should not upload, mirror, or commit assets unless they own the rights or the asset is explicitly licensed for redistribution.
