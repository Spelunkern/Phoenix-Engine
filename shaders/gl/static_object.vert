#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;
layout(location = 5) in vec4 inInstanceRight;
layout(location = 6) in vec4 inInstanceUp;
layout(location = 7) in vec4 inInstanceForward;
layout(location = 8) in vec4 inInstancePosition;

out vec3 vColor;
out vec3 vNormal;
out vec2 vUv;
flat out uint vTextureLayer;
out float vFogFactor;
out float vLighting;
out vec3 vWorldPos;

CAMERA_BLOCK

vec3 rodrigues(vec3 v, vec3 k, float cosA, float sinA)
{
    return v * cosA + cross(k, v) * sinA + k * dot(k, v) * (1.0 - cosA);
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

    vec3 axisSpeed = vec3(inInstanceRight.w, inInstanceUp.w, inInstanceForward.w);
    float speedSq = dot(axisSpeed, axisSpeed);

    vec3 right = inInstanceRight.xyz;
    vec3 up = inInstanceUp.xyz;
    vec3 forward = inInstanceForward.xyz;

    if (speedSq > 0.000001)
    {
        float speed = sqrt(speedSq);
        vec3 axis = axisSpeed / speed;
        float angle = camera.waterInfo.z * speed;
        float cosA = cos(angle);
        float sinA = sin(angle);
        right = rodrigues(right, axis, cosA, sinA);
        up = rodrigues(up, axis, cosA, sinA);
        forward = rodrigues(forward, axis, cosA, sinA);
    }

    vec3 worldPosition = inInstancePosition.xyz
        + right * inPosition.x
        + up * inPosition.y
        + forward * inPosition.z;

    vec3 delta = worldPosition - camera.positionYaw.xyz;

    vec3 worldNormal = normalize(right * inNormal.x + up * inNormal.y + forward * inNormal.z);
    float aspect = camera.pitchAspectFov.y;
    float tanHalfFov = camera.pitchAspectFov.z;
    float farPlane = camera.pitchAspectFov.w;

    float cy = camera.precomputedTrig.x;
    float sy = camera.precomputedTrig.y;
    float cp = camera.precomputedTrig.z;
    float sp = camera.precomputedTrig.w;

    float cameraX = cy * delta.x - sy * delta.z;
    float yawZ = sy * delta.x + cy * delta.z;
    float cameraY = cp * delta.y - sp * yawZ;
    float cameraZ = sp * delta.y + cp * yawZ;
    float nearPlane = 2.0;

    float lit = neutralMaterialLighting(worldNormal);

    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float fogLinear = clamp((cameraZ - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    float fogFactor = 1.0 - exp(-fogLinear * fogLinear * 5.0);

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
