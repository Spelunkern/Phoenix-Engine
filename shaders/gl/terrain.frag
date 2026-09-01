#version 450 core

in vec3 vColor;
in vec3 vNormal;
in vec2 vUv;
flat in uint vTextureLayer;
in float vFogFactor;
in vec3 vWorldPos;
in float vLighting;

out vec4 outColor;

CAMERA_BLOCK

ENVIRONMENT_FUNCTIONS

layout(binding = 0) uniform sampler2DArray terrainTexture;
layout(std430, binding = 1) readonly buffer TerrainMap { uint words[]; } terrainMap;
layout(binding = 2) uniform sampler2DArray lightmapTexture;

// Interleaved-gradient-noise dither (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare") — one mad+two fract+one mul
// per pixel, breaks up 8-bit banding in the sky/fog's smooth gradients
// without needing a temporal or blue-noise texture.
float ditherNoise(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

uint terrainMapLoad(uint cx, uint cz, uint mapSide)
{
    uint index = cz * mapSide + cx;
    uint word = terrainMap.words[index >> 2u];
    return (word >> ((index & 3u) * 8u)) & 0xFFu;
}

// World X is the reflected Shaiya source axis.  Control maps, splat masks and
// dungeon lightmaps remain stored in source order, so convert back only for
// those lookups (detail-texture UVs intentionally remain in world space).
vec2 sourceMapUv(vec3 worldPos, float mapSize)
{
    float halfMap = mapSize * 0.5;
    return vec2((halfMap - worldPos.x) / mapSize,
        (worldPos.z + halfMap) / mapSize);
}

uint terrainMapLookup(vec3 worldPos, float mapSize, uint mapSide)
{
    vec2 uv = sourceMapUv(worldPos, mapSize);
    uint cx = clamp(uint(uv.x * float(mapSide - 1u)), 0u, mapSide - 2u);
    uint cz = clamp(uint(uv.y * float(mapSide - 1u)), 0u, mapSide - 2u);
    return terrainMapLoad(cx, cz, mapSide);
}

float terrainTileSize(uint layer, uint mapSide)
{
    uint mapBytes = mapSide * mapSide;
    uint mapWordsPadded = (mapBytes + 3u) >> 2u;
    float tileSize = uintBitsToFloat(terrainMap.words[mapWordsPadded + layer]);
    return max(1.0, tileSize);
}

vec3 sampleTerrainLayerGrad(uint layer, vec3 worldPos, vec2 ddxWp, vec2 ddyWp, float mapSize, uint mapSide)
{
    float tileSize = terrainTileSize(layer, mapSide);
    float halfMap = mapSize * 0.5;
    vec2 tileUv = vec2((worldPos.x + halfMap) / tileSize, (worldPos.z + halfMap) / tileSize);
    return textureGrad(terrainTexture, vec3(tileUv, float(layer)), ddxWp / tileSize, ddyWp / tileSize).rgb;
}

vec3 blendedTerrainColor(vec3 worldPos, float mapSize, uint mapSide)
{
    vec2 uv = sourceMapUv(worldPos, mapSize);
    vec2 cellF = uv * float(mapSide - 1u) - 0.5;
    ivec2 cell0 = ivec2(floor(cellF));
    vec2 fracC = cellF - vec2(cell0);

    uint maxCell = mapSide - 2u;
    uint x0 = clamp(uint(cell0.x), 0u, maxCell);
    uint z0 = clamp(uint(cell0.y), 0u, maxCell);
    uint x1 = min(x0 + 1u, maxCell);
    uint z1 = min(z0 + 1u, maxCell);

    uint l00 = terrainMapLoad(x0, z0, mapSide);
    uint l10 = terrainMapLoad(x1, z0, mapSide);
    uint l01 = terrainMapLoad(x0, z1, mapSide);
    uint l11 = terrainMapLoad(x1, z1, mapSide);

    vec2 ddxWp = dFdx(worldPos.xz);
    vec2 ddyWp = dFdy(worldPos.xz);

    vec3 c00 = sampleTerrainLayerGrad(l00, worldPos, ddxWp, ddyWp, mapSize, mapSide);
    if (l00 == l10 && l00 == l01 && l00 == l11)
        return c00;

    vec3 c10 = sampleTerrainLayerGrad(l10, worldPos, ddxWp, ddyWp, mapSize, mapSide);
    vec3 c01 = sampleTerrainLayerGrad(l01, worldPos, ddxWp, ddyWp, mapSize, mapSide);
    vec3 c11 = sampleTerrainLayerGrad(l11, worldPos, ddxWp, ddyWp, mapSize, mapSide);

    vec2 t = fracC * fracC * (3.0 - 2.0 * fracC);
    vec3 top = mix(c00, c10, t.x);
    vec3 bot = mix(c01, c11, t.x);
    return mix(top, bot, t.y);
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
    float mapSize = camera.skyLayers.z;
    uint mapSide = uint(camera.skyLayers.w);

    vec3 color = vec3(0.0);
    float alpha = 1.0;
    bool applyLightmap = true;

    if (vTextureLayer == 0xFFFFFFFDu && mapSide > 1u)
    {
        uint mapBytes = mapSide * mapSide;
        uint mapWordsPadded = (mapBytes + 3u) >> 2u;
        uint maskFlags = terrainMap.words[mapWordsPadded + 16u];
        uint splatLayerCount = min(terrainMap.words[mapWordsPadded + 17u], 8u);

        if (maskFlags != 0u && camera.fogDistances.w > 0.5)
        {
            vec2 ddxWp = dFdx(vWorldPos.xz);
            vec2 ddyWp = dFdy(vWorldPos.xz);
            uint sections = uint(camera.fogDistances.z);
            vec2 worldUv = sourceMapUv(vWorldPos, mapSize);
            uint secX = clamp(uint(worldUv.x * float(sections)), 0u, sections - 1u);
            uint secZ = clamp(uint(worldUv.y * float(sections)), 0u, sections - 1u);
            uint maskBase = sections * sections + (secZ * sections + secX) * 2u;
            vec2 secUv = vec2(fract(worldUv.x * float(sections)), fract(worldUv.y * float(sections)));

            vec4 w0 = textureLod(lightmapTexture, vec3(secUv, float(maskBase)), 0.0);
            vec4 w1 = (maskFlags & 0xE0u) != 0u
                ? textureLod(lightmapTexture, vec3(secUv, float(maskBase + 1u)), 0.0)
                : vec4(0.0);

            color = sampleTerrainLayerGrad(0u, vWorldPos, ddxWp, ddyWp, mapSize, mapSide);
            for (uint n = 1u; n < splatLayerCount; ++n)
            {
                if ((maskFlags & (1u << n)) == 0u)
                    continue;
                uint idx = n - 1u;
                float w = idx < 4u ? w0[int(idx)] : w1[int(idx & 3u)];
                if (w > 0.004)
                    color = mix(color, sampleTerrainLayerGrad(n, vWorldPos, ddxWp, ddyWp, mapSize, mapSide), w);
            }
        }
        else
        {
            color = blendedTerrainColor(vWorldPos, mapSize, mapSide);
        }

        uint centerLayer = terrainMapLookup(vWorldPos, mapSize, mapSide);
        uint waterLayer = uint(camera.skyLayers.y);
        if (centerLayer == waterLayer)
            color = mix(vec3(0.01, 0.08, 0.34), color * vec3(0.42, 0.70, 1.22), 0.45);

        color *= vLighting;
    }
    else if (vTextureLayer != 0xFFFFFFFFu)
    {
        if (vTextureLayer == 0xFFFFFFFEu)
        {
            outColor = vec4(vColor, 0.85);
            return;
        }
        if (vTextureLayer == 0xFFFFFFFCu)
        {
            outColor = vec4(0.0, 0.0, 0.0, vColor.r);
            return;
        }

        uint sampleLayer = vTextureLayer;
        bool alphaCutout = false;
        if (sampleLayer >= 2048u)
        {
            alphaCutout = true;
            sampleLayer -= 2048u;
        }
        uint waterLayer = uint(camera.skyLayers.y);
        bool isWater = sampleLayer == waterLayer;

        if (isWater)
        {
            float t = camera.waterInfo.z;
            vec2 worldUv = vWorldPos.xz;
            float rippleScale = camera.waterSurface.x;
            float rippleSpeed = camera.waterSurface.y;
            vec2 uvA = worldUv * rippleScale + vec2(1.0, 0.35) * t * rippleSpeed;
            vec2 uvB = worldUv * rippleScale * 1.43
                + vec2(-0.6, 0.8) * t * rippleSpeed * 0.7;
            vec3 rippleMap = mix(texture(environmentNormalNoise, uvA).rgb,
                texture(environmentNormalNoise, uvB).rgb, 0.5);
            vec3 tangentRipple = rippleMap * 2.0 - 1.0;
            vec3 rippleNormal = normalize(vec3(tangentRipple.x * camera.waterSurface.z,
                max(tangentRipple.z, 0.15), tangentRipple.y * camera.waterSurface.z));

            vec3 viewDirection = normalize(camera.positionYaw.xyz - vWorldPos);
            float fresnel = pow(1.0 - clamp(dot(vec3(0.0, 1.0, 0.0), viewDirection), 0.0, 1.0),
                camera.waterSurface.w);
            vec3 reflectedDirection = reflect(-viewDirection, rippleNormal);
            vec3 reflectedSky = environmentSkyRadiance(reflectedDirection);

            // The direct-to-window backend has no sampleable scene depth. A
            // stable midpoint keeps the shallow/deep palette without a second
            // rendering pass or a framebuffer copy.
            float depthFade = 0.55;
            vec3 c = mix(camera.waterShallowAlpha.rgb, camera.waterDeepAlpha.rgb, depthFade);
            c = mix(c, reflectedSky, clamp(0.18 + fresnel * 0.62, 0.0, 0.82));
            float alpha = mix(camera.waterShallowAlpha.w, camera.waterDeepAlpha.w, depthFade);
            alpha = mix(alpha, 1.0, fresnel * camera.waterOptics.x);
            if (camera.positionYaw.y < 0.0)
            {
                float depth = clamp(-camera.positionYaw.y * 0.10, 0.0, 1.0);
                c = mix(c * vec3(0.70, 0.92, 1.18), camera.waterDeepAlpha.rgb * 1.45
                    + vec3(0.02, 0.05, 0.08), 0.45 + depth * 0.25);
                alpha = mix(camera.waterShallowAlpha.w, camera.waterDeepAlpha.w, camera.waterOptics.y);
                outColor = vec4(clamp(c, 0.0, 1.0), clamp(alpha, 0.0, 1.0));
                return;
            }
            outColor = vec4(clamp(c, 0.0, 1.0), clamp(alpha, 0.0, 1.0));
            return;
        }
        else
        {
            vec4 textureColor = texture(terrainTexture, vec3(vUv, float(sampleLayer)));

            bool isCharacter = (vColor.r < 0.01 && vColor.g < 0.01 && vColor.b < 0.05);
            if (isCharacter)
            {
                bool flatLit = vColor.b > 0.01;
                if (alphaCutout && textureColor.a - 0.08 < 0.0)
                    discard;
                // Cloaks intentionally use a constant term because their
                // simulation rebuilds normals every frame. All other player
                // parts use the same neutral Lambert result as the world.
                float lighting = flatLit ? 0.75 : vLighting;
                color = textureColor.rgb * lighting;
                applyLightmap = false;
            }
            else
            {
                if (alphaCutout && textureColor.a - 0.3 < 0.0)
                    discard;
                color = textureColor.rgb * vLighting;
            }
        }
    }
    else
    {
        color = vColor;
    }

    if (applyLightmap && camera.fogDistances.w > 0.5)
    {
        uint sections = uint(camera.fogDistances.z);
        vec2 worldUv = sourceMapUv(vWorldPos, mapSize);
        uint secX = clamp(uint(worldUv.x * float(sections)), 0u, sections - 1u);
        uint secZ = clamp(uint(worldUv.y * float(sections)), 0u, sections - 1u);
        uint layer = secZ * sections + secX;
        vec2 secUv = vec2(fract(worldUv.x * float(sections)), fract(worldUv.y * float(sections)));
        vec3 lm = texture(lightmapTexture, vec3(secUv, float(layer))).rgb;
        color *= lm;
    }

    color = applyUnderwaterView(color, vWorldPos);
    color = mix(color, skyColor, vFogFactor);

    // Dither at the very end, after every other color operation, so it
    // breaks up banding in the final quantized-to-8-bit output.
    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;

    outColor = vec4(color, alpha);
}
