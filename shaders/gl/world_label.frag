#version 450 core

in vec2 vUv;
in vec4 vColor;
flat in float vTextured;

layout(binding = 0) uniform sampler2D fontAtlas;

out vec4 outColor;

void main()
{
    float coverage = vTextured > 0.5 ? texture(fontAtlas, vUv).r : 1.0;
    if (coverage <= 0.01)
        discard;
    outColor = vec4(vColor.rgb, vColor.a * coverage);
}
