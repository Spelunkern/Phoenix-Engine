#version 450 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inTextureLayer;

out vec3 shadowColor;
out vec2 shadowUv;
flat out uint shadowTextureLayer;

CAMERA_BLOCK

void main()
{
    gl_Position = camera.shadowMatrix * vec4(inPosition, 1.0);
    shadowColor = inColor;
    shadowUv = inUv;
    shadowTextureLayer = inTextureLayer;
}
