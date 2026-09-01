#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;

out vec3 vColor;
out vec3 vNormal;
out vec2 vUv;
flat out uint vTextureLayer;
out float vFogFactor;
out vec3 vWorldPos;
out float vLighting;

CAMERA_BLOCK

void main()
{
    vec3 delta = inPosition - camera.positionYaw.xyz;
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

    float lit = 1.0;

    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float fogLinear = clamp((cameraZ - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    float fogFactor = fogLinear * fogLinear;

    // Vulkan NDC depth is [0,1] with Y flipped vs GL's [-1,1] with Y up;
    // gl_Position.z here still targets [0,1] via glClipControl-free trick:
    // we emit the same z/w as the Vulkan pipeline (z already in [0,farNear]
    // range scaled by w=cameraZ), then rely on the default GL depth range
    // remap (gl_Position.z/w in [-1,1]) by doubling minus w, done below.
    float ndcZ = cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane);
    // Convert the Vulkan-style [0,w] depth to GL's [-w,w] clip-space depth.
    float glZ = 2.0 * ndcZ - cameraZ;

    gl_Position = vec4(
        cameraX / (tanHalfFov * aspect),
        cameraY / tanHalfFov,
        glZ,
        cameraZ);
    vColor = inColor;
    vNormal = inNormal;
    vUv = inUv;
    vTextureLayer = inTextureLayer;
    vFogFactor = fogFactor;
    vWorldPos = inPosition;
    vLighting = lit;
}
