#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;
layout(location = 5) in uvec4 inBoneIndices;
layout(location = 6) in vec4 inBoneWeights;
layout(location = 7) in vec4 inInstanceRight;
layout(location = 8) in vec4 inInstanceUp;
layout(location = 9) in vec4 inInstanceForward;
layout(location = 10) in vec4 inInstancePosition;

out vec3 vColor;
out vec3 vNormal;
out vec2 vUv;
flat out uint vTextureLayer;
out float vFogFactor;
out float vLighting;
out vec3 vWorldPos;

CAMERA_BLOCK

layout(std430, binding = 3) readonly buffer BonePalette { vec4 rows[]; } bonePalette;

vec3 skinPoint(uint base, uvec3 bones, vec3 w, vec3 p)
{
    vec3 result = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        if (w[i] <= 0.0) continue;
        uint r = (base + bones[i]) * 4u;
        result += w[i] * (p.x * bonePalette.rows[r + 0u].xyz
                        + p.y * bonePalette.rows[r + 1u].xyz
                        + p.z * bonePalette.rows[r + 2u].xyz
                        + bonePalette.rows[r + 3u].xyz);
    }
    return result;
}

vec3 skinNormal(uint base, uvec3 bones, vec3 w, vec3 n)
{
    vec3 result = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        if (w[i] <= 0.0) continue;
        uint r = (base + bones[i]) * 4u;
        result += w[i] * (n.x * bonePalette.rows[r + 0u].xyz
                        + n.y * bonePalette.rows[r + 1u].xyz
                        + n.z * bonePalette.rows[r + 2u].xyz);
    }
    return result;
}

void main()
{
    float cullDist = inInstancePosition.w;
    if (cullDist > 0.0)
    {
        cullDist = min(cullDist, camera.pitchAspectFov.w);
        vec3 instDelta = inInstancePosition.xyz - camera.positionYaw.xyz;
        if (dot(instDelta, instDelta) > cullDist * cullDist)
        {
            gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
            vColor = vec3(0.0); vNormal = vec3(0.0); vUv = vec2(0.0);
            vTextureLayer = 0xFFFFFFFFu; vFogFactor = 0.0; vLighting = 0.0; vWorldPos = vec3(0.0);
            return;
        }
    }

    uint paletteBase = uint(inInstanceRight.w + 0.5);
    float totalWeight = inBoneWeights.x + inBoneWeights.y + inBoneWeights.z;
    vec3 localPos;
    vec3 localNrm;
    if (totalWeight > 0.0001)
    {
        float inv = 1.0 / totalWeight;
        localPos = skinPoint(paletteBase, inBoneIndices.xyz, inBoneWeights.xyz, inPosition) * inv;
        localNrm = skinNormal(paletteBase, inBoneIndices.xyz, inBoneWeights.xyz, inNormal) * inv;
    }
    else
    {
        localPos = inPosition;
        localNrm = inNormal;
    }

    // NPC and monster rigs arrive in Shaiya's left-handed local space.  Keep
    // their complete skeleton in source space and apply the same root-X
    // reflection used by the playable/bot CharacterSystem after skinning.
    localPos.x = -localPos.x;
    localNrm.x = -localNrm.x;

    vec3 right = inInstanceRight.xyz;
    vec3 up = inInstanceUp.xyz;
    vec3 forward = inInstanceForward.xyz;

    vec3 worldPosition = inInstancePosition.xyz
        + right * localPos.x
        + up * localPos.y
        + forward * localPos.z;

    vec3 delta = worldPosition - camera.positionYaw.xyz;

    vec3 worldNormal = normalize(right * localNrm.x + up * localNrm.y + forward * localNrm.z);

    float aspect = camera.pitchAspectFov.y;
    float tanHalfFov = camera.pitchAspectFov.z;
    float farPlane = camera.pitchAspectFov.w;

    float cy = camera.precomputedTrig.x;
    float sy = camera.precomputedTrig.y;
    float cp = camera.precomputedTrig.z;
    float sp = camera.precomputedTrig.w;

    float cameraX = -cy * delta.x + sy * delta.z;
    float yawZ = sy * delta.x + cy * delta.z;
    float cameraY = cp * delta.y - sp * yawZ;
    float cameraZ = sp * delta.y + cp * yawZ;
    float nearPlane = 0.05;

    float lit = 1.0;

    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float fogLinear = clamp((cameraZ - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    float fogFactor = fogLinear * fogLinear;

    float ndcZ = cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane);
    float glZ = 2.0 * ndcZ - cameraZ;

    gl_Position = vec4(
        cameraX / (tanHalfFov * aspect),
        cameraY / tanHalfFov,
        glZ,
        cameraZ);
    vColor = inColor;
    vNormal = worldNormal;
    vUv = inUv;
    vTextureLayer = inTextureLayer;
    vFogFactor = fogFactor;
    vLighting = lit;
    vWorldPos = worldPosition;
}
