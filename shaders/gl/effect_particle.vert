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
layout(location = 9) in vec4 inInstanceColor;

out vec2 vUv;
flat out uint vTextureLayer;
out vec4 vColor;

CAMERA_BLOCK

void main()
{
    // right/up/forward already carry the particle's world-space orientation,
    // spin and scale (computed CPU-side). Billboard quads only use local XY
    // (inPosition.z is 0); real .3DE mesh particles use all three axes.
    vec3 worldPosition = inInstancePosition.xyz
        + inInstanceRight.xyz * inPosition.x
        + inInstanceUp.xyz * inPosition.y
        + inInstanceForward.xyz * inPosition.z;

    vec3 delta = worldPosition - camera.positionYaw.xyz;

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

    float ndcZ = cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane);
    float glZ = 2.0 * ndcZ - cameraZ;

    gl_Position = vec4(
        cameraX / (tanHalfFov * aspect),
        cameraY / tanHalfFov,
        glZ,
        cameraZ);

    vUv = inUv;
    vTextureLayer = inTextureLayer;
    vColor = inInstanceColor;
}
