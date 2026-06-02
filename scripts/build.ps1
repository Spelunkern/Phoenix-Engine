$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# --- Locate CMake ---
# 1. PATH, 2. Visual Studio / Build Tools bundled CMake.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source

if (-not $cmake) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -products * -property installationPath 2>$null
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { $cmake = $candidate }
        }
    }
}

if (-not $cmake) {
    throw 'CMake not found. Install CMake from https://cmake.org/download/ and make sure cmake is on PATH.'
}

Write-Host "CMake: $cmake" -ForegroundColor Cyan

Push-Location $root
try {
    # --- Configure ---
    Write-Host '==> Configuring (Release)...' -ForegroundColor Cyan
    & $cmake --preset windows-vs2022-x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    # --- Build ---
    Write-Host '==> Building...' -ForegroundColor Cyan
    & $cmake --build --preset windows-release -- /m
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
} finally {
    Pop-Location
}

Write-Host ''
Write-Host "==> Build complete: bin\x64\Release\PhoenixEngine.exe" -ForegroundColor Cyan
