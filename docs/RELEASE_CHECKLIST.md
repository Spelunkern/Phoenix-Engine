# Phoenix Client v1.0 Release Checklist

Before publishing the repository:

- Confirm `Data/`, extracted assets, local logs, and build output are absent.
- Confirm `README.md` matches the current feature set and build path.
- Confirm `LICENSE` is the intended project license.
- Confirm third-party dependency licenses remain present under `external/`.
- Confirm `BUILD_LINUX.md` and `CMakePresets.json` still match the supported build paths.
- Confirm `phoenix.ini` is not bundled with developer-specific settings.
- Run `.\scripts\build.ps1`.
- Run `cmake --preset windows-vs2022-x64` and `cmake --build --preset windows-release` when validating the CMake path on Windows.
- Run a clean Linux Release build: `cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release` then `cmake --build build/linux-release -j"$(nproc)"`.
- Run the warning-focused audit build documented in `BUILD_LINUX.md`; project-owned sources should compile without warnings.
- Run `cppcheck` or an equivalent static analyzer over `src/` and review third-party-header findings separately.
- Smoke-test an AddressSanitizer + UndefinedBehaviorSanitizer build through initial map and character loading.
- Smoke-test `bin/x64/Release/PhoenixClient.exe` with a local `data/` folder.
- Smoke-test the Linux executable from outside the source directory to verify executable-relative shader/data discovery.
- Search for absolute local paths and old project names before publishing.
