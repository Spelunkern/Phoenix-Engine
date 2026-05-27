# Runtime Data and Assets

Phoenix Engine does not include commercial game data or extracted client/server assets.

Phoenix Engine resolves runtime data from the first valid location in this order:

1. `PHOENIX_ENGINE_DATA` environment variable.
2. `Data/` next to the executable.
3. `Data/` in the current working directory.
4. `Data/` in parent directories above the executable.

Platform-specific fallback locations:

| Platform | Paths |
|----------|-------|
| Windows | `%LOCALAPPDATA%/Phoenix Engine/Data`, `%PROGRAMDATA%/Phoenix Engine/Data` |
| Linux | `~/.local/share/Phoenix Engine/Data` |

For local development, place your own data directory at the project root:

```text
Data/
```

The current runtime expects a layout similar to:

```text
Data/
  Character/
  Entity/
  Effect/
  Monster/
    mob.csv
    monster.csv
  Npc/
    npc.csv
    NpcQuest.csv
  Sound/
  Terrain/
  World/
    svmap/{mapId}/
      metadata.csv
      monster_areas.csv
      monster_spawns.csv
      npcs.csv
      npc_positions.csv
```

### File formats

**Binary formats** (loaded as-is from the original client):

- World/map data: `.wld`, `.dg`
- Static and animated models: `.smod`, `.3dc`, `.vani`, `.mani`
- Animation: `.ani`
- Textures: `.dds`, `.bmp`, `.tga`

**CSV formats** (replacing legacy binary equivalents):

- `mob.csv` / `npc.csv` — monster and NPC model definitions (replaces binary `.mon`)
- `monster.csv` / `NpcQuest.csv` — server metadata (replaces binary `.sdata`)
- `svmap/{mapId}/*.csv` — spawn maps and NPC placement (replaces binary `.svmap`)

**Audio:**

- `.ogg` (Vorbis) — WLD files reference `.wav` names but the engine resolves them to `.ogg` on disk

The `.gitignore` intentionally excludes `Data/` at every repository depth. Keep this rule unless the project later gains a fully original asset pack that is safe to redistribute.

## Legal Boundary

Phoenix Engine is code-first. Contributors should not upload, mirror, or commit assets unless they own the rights or the asset is explicitly licensed for redistribution.
