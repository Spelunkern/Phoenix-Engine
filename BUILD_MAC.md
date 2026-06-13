# Building Phoenix Engine on macOS (experimental)

Phoenix Engine targets macOS through **MoltenVK** (Vulkan over Metal). The
codebase is prepared for it — portability enumeration and the portability
subset device extension are detected and enabled automatically at runtime —
but macOS builds are **not yet validated on real hardware**. Treat this as
the bring-up guide.

## Requirements

- macOS 11+ (Apple Silicon or Intel). Apple Silicon needs macOS 11+ for BC
  texture support, which the canonical data format relies on.
- Xcode 15.3+ command line tools (AppleClang with C++23 / `std::format`).
- [Homebrew](https://brew.sh) packages:

```bash
brew install cmake pkg-config sdl2
```

- The [LunarG Vulkan SDK for macOS](https://vulkan.lunarg.com/sdk/home)
  (provides the Vulkan loader + the MoltenVK ICD). After installing, make sure
  its environment is active (the SDK installer offers a `setup-env.sh`):

```bash
source ~/VulkanSDK/<version>/setup-env.sh
```

## Build

```bash
cmake --preset macos-release
cmake --build build/macos-release -j$(sysctl -n hw.ncpu)
```

Output: `build/macos-release/PhoenixEngine`

Place the `data/` tree and the `shaders/` directory next to the executable (or
run from the repository root) as on the other platforms.

## What is already handled in code

- `VK_KHR_portability_enumeration` + the portability instance flag (required
  for the MoltenVK device to be listed by loaders >= 1.3.216).
- `VK_KHR_portability_subset` enabled on the device when advertised.
- MAILBOX-less present modes (falls back to FIFO).
- Optional features (`multiDrawIndirect`, pipeline statistics, anisotropy)
  degrade gracefully when MoltenVK does not expose them.
- BC3-only canonical textures — supported by Metal on all targeted Macs.

## Known gaps (acceptable for bring-up)

- The performance HUD reads `/proc` for CPU/RAM metrics; on macOS those
  values display as zero until a `host_statistics64`/`task_info` branch is
  added. Cosmetic only.
- No `.app` bundle / code signing yet — the raw binary runs from a terminal.
- Untested visually: the terrain splat shader (`SampleGrad` + dynamic loops)
  is the first thing to verify on real hardware.

## CI

`.github/workflows/macos-build.yml` contains a manually-triggered
(`workflow_dispatch`) build job on a GitHub macOS runner — useful to produce a
Mac binary without owning a Mac. Trigger it from the GitHub Actions tab when
needed.
