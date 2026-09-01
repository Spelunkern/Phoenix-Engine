#version 450 core

in vec3 vColor;
in vec3 vNormal;
in vec2 vUv;
flat in uint vTextureLayer;
in float vFogFactor;
in float vLighting;
in vec3 vWorldPos;

out vec4 outColor;

CAMERA_BLOCK

layout(binding = 0) uniform sampler2DArray terrainTexture;
layout(binding = 2) uniform sampler2DArray lightmapTexture;

// Same interleaved-gradient-noise dither as terrain.frag — see there for
// the reference.
float ditherNoise(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

vec3 applyUnderwaterView(vec3 color, vec3 worldPos)
{
    if (camera.positionYaw.y >= 0.0)
        return color;

    vec3 waterTint = clamp(camera.waterDeepAlpha.rgb * 1.25 + vec3(0.02, 0.05, 0.08), 0.0, 1.0);
    float viewDistance = length(worldPos - camera.positionYaw.xyz);
    float cameraDepth = clamp(-camera.positionYaw.y * 0.10, 0.0, 1.0);
    float pixelDepth = clamp(-worldPos.y * 0.05, 0.0, 1.0);
    float absorption = 1.0 - exp(-viewDistance * (0.045 + cameraDepth * 0.030));
    float underwaterAmount = clamp(absorption * (0.45 + pixelDepth * 0.35) + cameraDepth * 0.18, 0.0, 1.0);

    vec3 cooled = color * vec3(0.62, 0.82, 1.02);
    return mix(cooled, waterTint, underwaterAmount);
}

void main()
{
    if (vFogFactor >= 0.995)
        discard;

    vec3 skyColor = clamp(camera.fogColorHasSky.rgb, 0.0, 1.0);

    vec3 color;
    if (vTextureLayer != 0xFFFFFFFFu)
    {
        uint sampleLayer = vTextureLayer;
        bool alphaCutout = false;
        if (sampleLayer >= 2048u)
        {
            alphaCutout = true;
            sampleLayer -= 2048u;
        }
        vec4 textureColor = texture(terrainTexture, vec3(vUv, float(sampleLayer)));

        bool isCharacter = (vColor.r < 0.01 && vColor.g < 0.01 && vColor.b < 0.01);
        float cutoutThreshold = isCharacter ? 0.08 : 0.30;
        if (alphaCutout && textureColor.a < cutoutThreshold)
            discard;
        if (!alphaCutout && !isCharacter && textureColor.a < 0.01)
            discard;

        if (!isCharacter && vColor.b >= 1.5 && camera.fogDistances.w > 0.5)
        {
            vec2 lmUV = vColor.rg;
            float lmLayer = vColor.b - 2.0;
            vec3 lm = texture(lightmapTexture, vec3(lmUV, lmLayer)).rgb;
            color = textureColor.rgb * lm;
        }
        else
            color = textureColor.rgb * environmentMaterialLighting(vNormal, vWorldPos);
    }
    else
    {
        color = vColor * environmentMaterialLighting(vNormal, vWorldPos);
    }

    color = applyUnderwaterView(color, vWorldPos);
    color = mix(color, skyColor, vFogFactor);

    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;

    outColor = vec4(color, 1.0);
}
