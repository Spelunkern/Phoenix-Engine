#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;
layout(location = 5) in uvec4 inBoneIndices;
layout(location = 6) in vec4 inBoneWeights;
layout(location = 7) in vec4 inInstanceRight;
layout(location = 8) in vec4 inInstanceUp;
layout(location = 9) in vec4 inInstanceForward;
layout(location = 10) in vec4 inInstancePosition;

out vec3 shadowColor;
out vec2 shadowUv;
flat out uint shadowTextureLayer;

CAMERA_BLOCK

layout(std430, binding = 3) readonly buffer BonePalette { vec4 rows[]; } bonePalette;

vec3 skinPoint(uint base, uvec3 bones, vec3 weights, vec3 point)
{
    vec3 result = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        if (weights[i] <= 0.0)
            continue;
        uint row = (base + bones[i]) * 4u;
        result += weights[i] * (point.x * bonePalette.rows[row + 0u].xyz
            + point.y * bonePalette.rows[row + 1u].xyz
            + point.z * bonePalette.rows[row + 2u].xyz
            + bonePalette.rows[row + 3u].xyz);
    }
    return result;
}

void main()
{
    uint paletteBase = uint(inInstanceRight.w + 0.5);
    float totalWeight = inBoneWeights.x + inBoneWeights.y + inBoneWeights.z;
    vec3 localPosition = totalWeight > 0.0001
        ? skinPoint(paletteBase, inBoneIndices.xyz, inBoneWeights.xyz, inPosition) / totalWeight
        : inPosition;
    localPosition.x = -localPosition.x;
    vec3 worldPosition = inInstancePosition.xyz
        + inInstanceRight.xyz * localPosition.x
        + inInstanceUp.xyz * localPosition.y
        + inInstanceForward.xyz * localPosition.z;
    gl_Position = camera.shadowMatrix * vec4(worldPosition, 1.0);
    shadowColor = inColor;
    shadowUv = inUv;
    shadowTextureLayer = inTextureLayer;
}
