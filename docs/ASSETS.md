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
  Sound/
  Terrain/
  World/
```

Commonly consumed file types include:

- World/map data: `.wld`, `.dg`, `.svmap`
- Static and animated models: `.smod`, `.3dc`, `.vani`, `.mani`
- Actor metadata: `.mon`, `.sdata`
- Animation and textures: `.ani`, `.dds`, `.bmp`
- Audio: `.ogg` files referenced by map sound/music zones

The `.gitignore` intentionally excludes `Data/` at every repository depth. Keep this rule unless the project later gains a fully original asset pack that is safe to redistribute.

## Legal Boundary

Phoenix Engine is code-first. Contributors should not upload, mirror, or commit assets unless they own the rights or the asset is explicitly licensed for redistribution.
