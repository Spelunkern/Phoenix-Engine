$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dxc = Join-Path $root 'external\dxc\bin\x64\dxc.exe'
$shader = Join-Path $root 'shaders\sky.hlsl'
$outDir = Join-Path $root 'shaders\compiled'

if (-not (Test-Path $dxc)) {
    throw "DXC was not found at $dxc."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $dxc -spirv -T vs_6_0 -E VSMain -Fo (Join-Path $outDir 'sky.vert.spv') $shader
& $dxc -spirv -T ps_6_0 -E PSMain -Fo (Join-Path $outDir 'sky.frag.spv') $shader

