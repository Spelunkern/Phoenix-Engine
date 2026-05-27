$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $root 'PhoenixEngine.sln'
$msbuild = 'C:\BuildTools\MSBuild\Current\Bin\MSBuild.exe'

if (-not (Test-Path $msbuild)) {
    throw "MSBuild was not found at $msbuild. Install Visual Studio 2022 Build Tools or update this script."
}

& $msbuild $solution /p:Configuration=Release /p:Platform=x64 /m

