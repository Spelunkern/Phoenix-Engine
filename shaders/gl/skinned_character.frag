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

    vec3 waterTint = clamp(camera.waterStyle.rgb * 1.25 + vec3(0.02, 0.05, 0.08), 0.0, 1.0);
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
    bool assetGrade = true;
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
        if (isCharacter)
        {
            // Same wrap-diffuse + hemisphere-ambient + Blinn-Phong specular
            // + Fresnel rim model as terrain.frag's character branch (used
            // by the player) — monsters/NPCs draw through this shader
            // instead, so armor/weapons/capes on them now pick up the same
            // highlight and rim light instead of flat diffuse.
            assetGrade = false;
            if (alphaCutout && textureColor.a - 0.08 < 0.0)
                discard;
            color = textureColor.rgb;

            vec3 n = normalize(vNormal);
            vec3 charLightDir = vec3(-0.33, 0.80, -0.26);
            float nDotL = dot(n, charLightDir);
            float diffuse = clamp(nDotL, 0.0, 1.0);
            float wrap = clamp((nDotL + 0.4) / 1.4, 0.0, 1.0);
            float shade = mix(diffuse, wrap, camera.tuning2.w);

            float hemi = n.y * 0.5 + 0.5;
            vec3 keyColor = camera.tuning0.rgb;
            vec3 ambientGround = camera.tuning1.rgb;
            vec3 ambientSky = camera.tuning2.rgb;
            vec3 weatherTint = vec3(camera.tuning3.z) + camera.tuning3.w * skyColor;
            vec3 ambient = mix(ambientGround, ambientSky, hemi) * weatherTint;

            vec3 lit = color * (ambient + shade * camera.tuning1.w * keyColor);

            vec3 viewDir = normalize(camera.positionYaw.xyz - vWorldPos);
            vec3 halfVec = normalize(charLightDir + viewDir);
            float spec = pow(clamp(dot(n, halfVec), 0.0, 1.0), 24.0) * camera.tuning3.x * (0.25 + 0.75 * diffuse);
            float rim = pow(1.0 - clamp(dot(n, viewDir), 0.0, 1.0), 3.0) * camera.tuning3.y * (0.4 + 0.6 * hemi);
            lit += spec + rim * (0.5 + 0.5 * skyColor);

            if (!alphaCutout)
                color = clamp(lit + color * textureColor.a * 0.30, 0.0, 1.0);
            else
                color = clamp(lit, 0.0, 1.0);
        }
        else
        {
            if (alphaCutout && textureColor.a - 0.3 < 0.0)
                discard;
            else if (!alphaCutout && textureColor.a - 0.01 < 0.0)
                discard;
            if (vColor.b >= 1.5 && camera.fogDistances.w > 0.5)
            {
                vec2 lmUV = vColor.rg;
                float lmLayer = vColor.b - 2.0;
                vec3 lm = texture(lightmapTexture, vec3(lmUV, lmLayer)).rgb;
                color = textureColor.rgb * lm * camera.tuning1.z;
            }
            else
                color = textureColor.rgb * vLighting;
        }
    }
    else
    {
        color = vColor * vLighting;
    }

    if (assetGrade)
    {
        float luma = dot(color, vec3(0.299, 0.587, 0.114));
        color = clamp(mix(vec3(luma), color, camera.tuning0.w), 0.0, 1.0);
        color = clamp(color * camera.tuning0.rgb * (camera.tuning1.w + camera.tuning2.x * skyColor), 0.0, 1.0);
    }

    color = applyUnderwaterView(color, vWorldPos);
    color = mix(color, skyColor, vFogFactor);

    // Subtle screen-space vignette — a lens/framing effect applied to the
    // whole scene (see terrain.frag/static_object.frag for the same term).
    vec2 screenUv = gl_FragCoord.xy * camera.screenInfo.zw;
    float vignette = 1.0 - smoothstep(0.4, 1.0, length(screenUv - 0.5) * 1.35);
    color *= mix(0.55, 1.0, vignette);

    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;

    outColor = vec4(color, 1.0);
}
