# Building Phoenix Client on Windows

## Prerequisites

- Windows 10 or Windows 11, 64-bit.
- Visual Studio 2022 Build Tools or Visual Studio 2022 with the **Desktop
  development with C++** workload and MSVC v143.
- Windows SDK.
- CMake 3.20 or newer on `PATH`, or the CMake bundled with Visual Studio.
- A current OpenGL 4.5-capable graphics driver.

SDL2 is provided under `external/SDL2` for Windows builds. No Vulkan SDK or
separate OpenGL SDK is required.

## CMake Build

From PowerShell in the repository root:

```powershell
cmake --preset windows-vs2022-x64
cmake --build --preset windows-release
```

Or use the helper, which also locates Visual Studio's bundled CMake:

```powershell
.\scripts\build.ps1
```

The Release executable is written to:

```text
bin\x64\Release\PhoenixClient.exe
```

With `PHOENIX_COPY_RUNTIME_DLLS=ON` (the default), CMake copies `SDL2.dll` next
to the executable.

## Visual Studio Solution

`PhoenixClient.sln` and `PhoenixClient.vcxproj` are provided for direct Visual
Studio builds. Select `Release` and `x64`, then build the solution. The output
path matches the CMake Windows layout.

## Run

Keep these resources available beside the executable or in a discoverable
development path:

```text
PhoenixClient.exe
SDL2.dll
shaders/gl/
data/
```

Instead of placing `data/` beside the executable, set an explicit path for the
current PowerShell session:

```powershell
$env:PHOENIX_CLIENT_DATA = 'D:\path\to\data'
.\bin\x64\Release\PhoenixClient.exe
```

## Troubleshooting

- **OpenGL context creation fails:** install the current NVIDIA, AMD, or Intel
  graphics driver and confirm the GPU supports OpenGL 4.5.
- **`SDL2.dll` is missing:** rebuild with `PHOENIX_COPY_RUNTIME_DLLS=ON` or copy
  `external\SDL2\lib\x64\SDL2.dll` next to the executable.
- **Shaders are missing:** copy `shaders\gl` beside the packaged executable.
- **Models or maps are missing:** verify the complete `data/` tree or set
  `PHOENIX_CLIENT_DATA` explicitly. Commercial runtime data is not included in
  the repository.
