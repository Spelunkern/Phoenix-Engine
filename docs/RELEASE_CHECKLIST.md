# Open Source Release Checklist

Before publishing the repository:

- Confirm `Data/`, extracted assets, local logs, and build output are absent.
- Confirm `README.md` matches the current feature set and build path.
- Confirm `LICENSE` is the intended project license.
- Confirm third-party dependency licenses remain present under `external/`.
- Run `.\scripts\compile_shaders.ps1`.
- Run `.\scripts\build.ps1`.
- Smoke-test `bin/x64/Release/PhoenixEngine.exe` with a local `Data/` folder.
- Search for absolute local paths and old project names before publishing.
