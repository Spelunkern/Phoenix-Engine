#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;
layout(location = 5) in vec4 inInstanceRight;
layout(location = 6) in vec4 inInstanceUp;
layout(location = 7) in vec4 inInstanceForward;
layout(location = 8) in vec4 inInstancePosition;

out vec3 shadowColor;
out vec2 shadowUv;
flat out uint shadowTextureLayer;

CAMERA_BLOCK

vec3 rodrigues(vec3 value, vec3 axis, float cosAngle, float sinAngle)
{
    return value * cosAngle + cross(axis, value) * sinAngle
        + axis * dot(axis, value) * (1.0 - cosAngle);
}

void main()
{
    vec3 right = inInstanceRight.xyz;
    vec3 up = inInstanceUp.xyz;
    vec3 forward = inInstanceForward.xyz;
    vec3 axisSpeed = vec3(inInstanceRight.w, inInstanceUp.w, inInstanceForward.w);
    float speedSquared = dot(axisSpeed, axisSpeed);
    if (speedSquared > 0.000001)
    {
        float speed = sqrt(speedSquared);
        vec3 axis = axisSpeed / speed;
        float angle = camera.waterInfo.z * speed;
        right = rodrigues(right, axis, cos(angle), sin(angle));
        up = rodrigues(up, axis, cos(angle), sin(angle));
        forward = rodrigues(forward, axis, cos(angle), sin(angle));
    }
    vec3 worldPosition = inInstancePosition.xyz
        + right * inPosition.x + up * inPosition.y + forward * inPosition.z;
    gl_Position = camera.shadowMatrix * vec4(worldPosition, 1.0);
    shadowColor = inColor;
    shadowUv = inUv;
    shadowTextureLayer = inTextureLayer;
}
