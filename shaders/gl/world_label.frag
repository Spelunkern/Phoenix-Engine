#version 450 core

in vec2 vUv;
in vec4 vColor;

layout(binding = 0) uniform sampler2D fontAtlas;

out vec4 outColor;

void main()
{
    float coverage = texture(fontAtlas, vUv).r;
    if (coverage <= 0.01)
        discard;
    outColor = vec4(vColor.rgb, vColor.a * coverage);
}
