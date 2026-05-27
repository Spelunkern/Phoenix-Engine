# Contributing

Thanks for helping improve Phoenix Engine.

## Development Setup

1. Install Visual Studio 2022 Build Tools with MSVC v143 and the Windows SDK.
2. Clone the repository.
3. Provide your own local runtime `Data/` folder next to the built executable.
4. Build with:

```powershell
.\scripts\build.ps1
```

## Guidelines

- Keep changes scoped and easy to review.
- Do not commit extracted game data, proprietary assets, logs, or local build output.
- Keep shader source and compiled shader binaries in sync when modifying `shaders/*.hlsl`.
- Prefer existing engine patterns before adding new abstractions.
- Document new file format assumptions in code or docs when they affect parsing behavior.

## Code Style

- C++20-or-newer MSVC style.
- Use namespaces under `phoenix::`.
- Keep comments short and useful.
- Avoid broad rewrites unless the task is explicitly a cleanup/refactor.

