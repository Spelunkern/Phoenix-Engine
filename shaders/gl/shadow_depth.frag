#version 450 core

in vec3 shadowColor;
in vec2 shadowUv;
flat in uint shadowTextureLayer;

layout(binding = 0) uniform sampler2DArray terrainTexture;

void main()
{
    if (shadowTextureLayer >= 2048u && shadowTextureLayer < 0xFFFFFFF0u)
    {
        uint sampleLayer = shadowTextureLayer - 2048u;
        float alpha = texture(terrainTexture, vec3(shadowUv, float(sampleLayer))).a;
        bool isCharacter = shadowColor.r < 0.01 && shadowColor.g < 0.01 && shadowColor.b < 0.05;
        if (alpha < (isCharacter ? 0.08 : 0.30))
            discard;
    }
}
