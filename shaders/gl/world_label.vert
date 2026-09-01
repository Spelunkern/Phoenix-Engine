#version 450 core

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(location = 0) uniform vec4 viewport;

out vec2 vUv;
out vec4 vColor;

void main()
{
    vec2 ndc = vec2(
        inPosition.x / max(viewport.x, 1.0) * 2.0 - 1.0,
        1.0 - inPosition.y / max(viewport.y, 1.0) * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUv = inUv;
    vColor = inColor;
}
