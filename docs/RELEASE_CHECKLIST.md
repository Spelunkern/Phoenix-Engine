# Open Source Release Checklist

Before publishing the repository:

- Confirm `Data/`, extracted assets, local logs, and build output are absent.
- Confirm `README.md` matches the current feature set and build path.
- Confirm `LICENSE` is the intended project license.
- Confirm third-party dependency licenses remain present under `external/`.
- Confirm `BUILD_LINUX.md`, `shell.nix`, and `CMakePresets.json` still match the supported build paths.
- Run `.\scripts\compile_shaders.ps1`.
- Run `.\scripts\build.ps1`.
- Run `cmake --preset windows-vs2022-x64` and `cmake --build --preset windows-release` when validating the CMake path on Windows.
- Smoke-test `bin/x64/Release/PhoenixEngine.exe` with a local `Data/` folder.
- Search for absolute local paths and old project names before publishing.
