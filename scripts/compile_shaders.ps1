$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dxc = Join-Path $root 'external\dxc\bin\x64\dxc.exe'
$shaderDir = Join-Path $root 'shaders'
$outDir = Join-Path $root 'shaders\compiled'

if (-not (Test-Path $dxc)) {
    throw "DXC was not found at $dxc."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Graphics shaders: VSMain + PSMain in a single .hlsl, emitted as <name>.vert.spv / <name>.frag.spv.
$graphics = @('sky', 'terrain', 'static_object', 'particle')
foreach ($name in $graphics) {
    $src = Join-Path $shaderDir "$name.hlsl"
    if (-not (Test-Path $src)) { throw "Shader source missing: $src" }
    & $dxc -spirv -T vs_6_0 -E VSMain -Fo (Join-Path $outDir "$name.vert.spv") $src
    & $dxc -spirv -T ps_6_0 -E PSMain -Fo (Join-Path $outDir "$name.frag.spv") $src
}

# NOTE: compute shaders (cull_objects.comp) are GLSL (#version 450)
# and are compiled separately by glslang; their .spv are committed. dxc cannot compile
# GLSL, so they are intentionally not handled here.
