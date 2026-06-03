struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint textureLayer : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureLayer : TEXCOORD1;
    float viewDepth : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 fogColorHasSky;
    float4 fogDistances;
    float4 skyLayers;
    float4 waterInfo; // x=baseLayer, y=frameCount, z=time, w=tileSize
};

[[vk::push_constant]]
CameraConstants camera;

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
Texture2DArray terrainTexture : register(t0, space0);

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
SamplerState terrainSampler : register(s0, space0);

[[vk::binding(1, 0)]]
ByteAddressBuffer terrainMap : register(t1, space0);

VSOutput VSMain(VSInput input)
{
    const float3 delta = input.position - camera.positionYaw.xyz;
    const float yaw = camera.positionYaw.w;
    const float pitch = camera.pitchAspectFov.x;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;
    const float farPlane = camera.pitchAspectFov.w;

    const float cy = cos(yaw);
    const float sy = sin(yaw);
    const float cp = cos(pitch);
    const float sp = sin(pitch);

    const float cameraX = cy * delta.x - sy * delta.z;
    const float yawZ = sy * delta.x + cy * delta.z;
    const float cameraY = cp * delta.y - sp * yawZ;
    const float cameraZ = sp * delta.y + cp * yawZ;
    const float nearPlane = 2.0f;

    VSOutput output;
    output.position = float4(
        cameraX / (tanHalfFov * aspect),
        -cameraY / tanHalfFov,
        cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane),
        cameraZ);
    output.color = input.color;
    output.normal = input.normal;
    output.uv = input.uv;
    output.textureLayer = input.textureLayer;
    output.viewDepth = cameraZ;
    output.worldPos = input.position;
    return output;
}

uint terrainMapLoad(uint cx, uint cz, uint mapSide)
{
    uint index = cz * mapSide + cx;
    uint word = terrainMap.Load((index & ~3u) * 1);
    return (word >> ((index & 3u) * 8)) & 0xFF;
}

uint terrainMapLookup(float3 worldPos, float mapSize, uint mapSide)
{
    float2 uv = (worldPos.xz + mapSize * 0.5) / mapSize;
    uint cx = clamp((uint)(uv.x * (mapSide - 1)), 0, mapSide - 2);
    uint cz = clamp((uint)(uv.y * (mapSide - 1)), 0, mapSide - 2);
    return terrainMapLoad(cx, cz, mapSide);
}

float terrainTileSize(uint layer, uint mapSide)
{
    uint mapBytes = mapSide * mapSide;
    uint mapBytesPadded = (mapBytes + 3u) & ~3u;
    float tileSize = asfloat(terrainMap.Load(mapBytesPadded + layer * 4));
    return max(1.0, tileSize);
}

float3 sampleTerrainLayer(uint layer, float3 worldPos, float mapSize, uint mapSide)
{
    float tileSize = terrainTileSize(layer, mapSide);
    float halfMap = mapSize * 0.5;
    float2 tileUv = float2(
        (worldPos.x + halfMap) / tileSize,
        (worldPos.z + halfMap) / tileSize);
    return terrainTexture.Sample(terrainSampler, float3(tileUv, (float)layer)).rgb;
}

float3 blendedTerrainColor(float3 worldPos, float mapSize, uint mapSide)
{
    // Convert world position to continuous cell coordinates.
    float2 uv = (worldPos.xz + mapSize * 0.5) / mapSize;
    float2 cellF = uv * (float)(mapSide - 1) - 0.5;
    int2 cell0 = int2(floor(cellF));
    float2 frac_ = cellF - float2(cell0);

    // Clamp cell coordinates.
    uint maxCell = mapSide - 2;
    uint x0 = clamp((uint)cell0.x, 0, maxCell);
    uint z0 = clamp((uint)cell0.y, 0, maxCell);
    uint x1 = min(x0 + 1, maxCell);
    uint z1 = min(z0 + 1, maxCell);

    // Load 4 surrounding cell layers.
    uint l00 = terrainMapLoad(x0, z0, mapSide);
    uint l10 = terrainMapLoad(x1, z0, mapSide);
    uint l01 = terrainMapLoad(x0, z1, mapSide);
    uint l11 = terrainMapLoad(x1, z1, mapSide);

    // Sample each layer's texture.
    float3 c00 = sampleTerrainLayer(l00, worldPos, mapSize, mapSide);
    float3 c10 = sampleTerrainLayer(l10, worldPos, mapSize, mapSide);
    float3 c01 = sampleTerrainLayer(l01, worldPos, mapSize, mapSide);
    float3 c11 = sampleTerrainLayer(l11, worldPos, mapSize, mapSide);

    // Smoothstep blend for natural-looking transitions.
    float2 t = frac_ * frac_ * (3.0 - 2.0 * frac_);

    // Optimize: skip blending when all 4 cells share the same layer.
    if (l00 == l10 && l00 == l01 && l00 == l11)
        return c00;

    float3 top = lerp(c00, c10, t.x);
    float3 bot = lerp(c01, c11, t.x);
    return lerp(top, bot, t.y);
}

float3 waterNormal(float2 worldXZ, float time)
{
    float2 uvA = worldXZ * 0.018;
    float2 uvB = worldXZ * 0.044;
    float longA = sin((uvA.x + time * 0.18) * 5.1 + sin(uvA.y * 3.7));
    float longB = sin((uvA.y - time * 0.14) * 6.3 + uvA.x * 1.8);
    float chopA = sin(dot(uvB, float2(4.4, 2.7)) + time * 0.42);
    float chopB = sin(dot(uvB, float2(-3.2, 5.6)) - time * 0.36);
    float micro = sin((uvB.x + uvB.y) * 11.0 + time * 1.8) * 0.35;
    return normalize(float3(
        longA * 0.052 + chopA * 0.028 + micro * 0.010,
        1.0,
        longB * 0.045 - chopB * 0.024 - micro * 0.010));
}

float3 realisticWaterColor(float3 worldPos, float3 viewDir, float3 lightDir, float waterTime, float3 textureColor)
{
    float3 n = waterNormal(worldPos.xz, waterTime);
    float3 halfVec = normalize(lightDir + viewDir);
    float facing = saturate(dot(n, viewDir));
    float fresnel = pow(1.0 - facing, 4.6);
    float sunFacing = saturate(dot(n, lightDir));

    float2 uv = worldPos.xz * 0.020;
    float caustic = sin(uv.x * 9.0 + waterTime * 0.7) * sin(uv.y * 7.0 - waterTime * 0.5);
    float ripple = saturate(caustic * 0.32 + 0.52);

    float3 deepTint = float3(0.018, 0.095, 0.17);
    float3 midTint = float3(0.050, 0.22, 0.26);
    float3 shallowTint = float3(0.13, 0.42, 0.40);
    float3 baseWater = lerp(deepTint, midTint, 0.55 + ripple * 0.18);
    baseWater = lerp(baseWater, shallowTint, saturate(sunFacing * 0.22 + fresnel * 0.10));
    baseWater = lerp(baseWater, textureColor * float3(0.44, 0.66, 0.82), 0.12);

    float sparkle = pow(saturate(dot(n, halfVec)), 110.0) * 0.55;
    float glint = pow(saturate(dot(reflect(-lightDir, n), viewDir)), 220.0) * 0.40;
    float broadReflection = fresnel * 0.55;
    float3 reflectedSky = float3(0.50, 0.70, 0.96);

    float3 color = baseWater * (0.74 + sunFacing * 0.28);
    color = lerp(color, reflectedSky, broadReflection);
    color += float3(0.90, 0.97, 1.0) * (sparkle + glint);
    color += float3(0.03, 0.08, 0.10) * ripple;
    return color;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // ---- Early fog discard ----
    // Fragments fully consumed by fog are invisible. Killing them here, before
    // any texture/lighting work, is the main performance win when lowering view
    // distance: geometry that passes vertex-level culling but lands entirely in
    // the dense fog band is discarded at negligible cost.
    {
        float fogStart = camera.fogDistances.x;
        float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
        float earlyFog = saturate((input.viewDepth - fogStart) / (fogEnd - fogStart));
        if (earlyFog >= 0.995)
            discard;
    }

    const float3 lightDir = normalize(float3(-0.32, 0.72, -0.61));
    const float3 skyColor = saturate(camera.fogColorHasSky.rgb);
    const float waterSurfaceY = 0.0;
    const float3 waterTint = float3(0.12, 0.25, 0.34);

    float3 color;
    float alpha = 1.0;
    bool fragmentIsWater = false;

    const float mapSize = camera.skyLayers.z;
    const uint mapSide = (uint)camera.skyLayers.w;

    if (input.textureLayer == 0xFFFFFFFDu && mapSide > 1)
    {
        color = blendedTerrainColor(input.worldPos, mapSize, mapSide);

        // Water tint for water cells.
        uint centerLayer = terrainMapLookup(input.worldPos, mapSize, mapSide);
        const uint waterLayer = (uint)camera.skyLayers.y;
        if (centerLayer == waterLayer)
            color = lerp(float3(0.02, 0.10, 0.24), color * float3(0.55, 0.85, 1.15), 0.55);

        float3 n = normalize(input.normal);

        // Main directional light + soft fill from opposite side.
        float nDotL = saturate(dot(n, lightDir));
        const float3 fillDir = normalize(float3(0.4, 0.3, 0.5));
        float nDotFill = saturate(dot(n, fillDir));
        float lighting = nDotL * 0.55 + nDotFill * 0.12 + 0.38;

        // Subtle height-based color variation: cooler tones at high altitude.
        float heightFactor = saturate((input.worldPos.y - 20.0) / 280.0);
        color = lerp(color, color * float3(0.92, 0.94, 1.06), heightFactor * 0.35);

        // Gentle slope darkening (steep faces get slightly darker).
        float slopeFactor = 1.0 - saturate((1.0 - n.y) * 0.6);
        lighting *= lerp(0.85, 1.0, slopeFactor);

        color *= lighting;
    }
    else if (input.textureLayer != 0xFFFFFFFFu)
    {
        if (input.textureLayer == 0xFFFFFFFEu)
        {
            return float4(input.color, 0.85);
        }
        uint sampleLayer = input.textureLayer;
        bool alphaCutout = false;
        if (sampleLayer >= 2048)
        {
            alphaCutout = true;
            sampleLayer -= 2048;
        }
        const uint waterLayer = (uint)camera.skyLayers.y;
        const bool isWater = sampleLayer == waterLayer;
        fragmentIsWater = isWater;

        // Animated water: use frame sequence if available.
        if (isWater && camera.waterInfo.y > 0)
        {
            float waterBaseLayer = camera.waterInfo.x;
            float waterFrameCount = camera.waterInfo.y;
            float waterTime = camera.waterInfo.z;
            float waterTile = max(1.0, camera.waterInfo.w);

            // Frame selection with smooth blending between frames.
            float fps = 12.0;
            float frameF = frac(waterTime * fps / waterFrameCount) * waterFrameCount;
            uint frame0 = (uint)frameF;
            uint frame1 = (frame0 + 1) % (uint)waterFrameCount;
            float blend = frac(frameF);

            float halfMap = mapSize * 0.5;
            float2 waterUv = float2(
                (input.worldPos.x + halfMap) / waterTile,
                (input.worldPos.z + halfMap) / waterTile);
            float3 c0 = terrainTexture.Sample(terrainSampler,
                float3(waterUv, waterBaseLayer + frame0)).rgb;
            float3 c1 = terrainTexture.Sample(terrainSampler,
                float3(waterUv, waterBaseLayer + frame1)).rgb;
            color = lerp(c0, c1, blend);
            alpha = 0.30;

            float3 viewDir = normalize(camera.positionYaw.xyz - input.worldPos);
            color = realisticWaterColor(input.worldPos, viewDir, lightDir, waterTime, color);
        }
        else
        {
            float4 textureColor = terrainTexture.Sample(terrainSampler,
                float3(input.uv, (float)sampleLayer));

            // Character vertices (color=0 marker) — Shaiya glow/visibility lighting.
            bool isCharacter = (input.color.r < 0.01 && input.color.g < 0.01 && input.color.b < 0.01);
            if (isCharacter)
            {
                // Glow mode for solid parts, visibility mode for alpha-cutout parts.
                if (alphaCutout)
                    clip(textureColor.a - 0.08);

                color = textureColor.rgb;
                float3 n = normalize(input.normal);
                const float3 charLightDir = normalize(float3(-0.35, 0.85, -0.28));
                float diffuse = saturate(dot(n, charLightDir));
                float3 lit = color * (0.56 + diffuse * 0.72);

                if (!alphaCutout)
                {
                    // Glow: additive emission from texture alpha channel.
                    float3 glow = color * textureColor.a * 0.30;
                    color = saturate(lit + glow);
                }
                else
                {
                    color = lit;
                }
            }
            else
            {
                // World objects — universal alpha test. Any pixel with
                // alpha below the threshold is transparent, regardless of
                // whether the asset was tagged as cutout or not.
                clip(textureColor.a - 0.3);
                color = textureColor.rgb;
                if (isWater)
                {
                    float waterTime = camera.waterInfo.z;
                    float3 viewDir = normalize(camera.positionYaw.xyz - input.worldPos);
                    color = realisticWaterColor(input.worldPos, viewDir, lightDir, waterTime, color);
                    alpha = 0.30;
                }
                float3 n = normalize(input.normal);
                float lighting = saturate(dot(n, lightDir)) * 0.55 + 0.45;
                color *= lighting;
            }
        }
    }
    else
    {
        color = input.color;
    }

    if (!fragmentIsWater && input.worldPos.y < waterSurfaceY)
    {
        const float submersion = saturate((waterSurfaceY - input.worldPos.y) * 0.45);
        const float tintAmount = 0.14 + submersion * 0.26;
        color = lerp(color * float3(0.84, 0.93, 1.02), waterTint, tintAmount);
    }

    // Atmospheric distance rendering.
    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float linearDist = saturate((input.viewDepth - fogStart) / (fogEnd - fogStart));

    // Exponential fog curve — much more natural than linear squared.
    // Dense near the end, gentle at the start.
    float fogDensity = 1.0 - exp(-linearDist * linearDist * 5.0);

    // Height-based fog attenuation: lower altitudes get more fog.
    // Fades out at long range so terrain at the cull edge is always hidden.
    float cameraHeight = camera.positionYaw.y;
    float fragHeight = input.worldPos.y;
    float avgHeight = (cameraHeight + fragHeight) * 0.5;
    float heightFog = saturate(1.0 - avgHeight / 350.0) * 0.4 + 0.6;
    float heightInfluence = saturate(1.0 - linearDist * 1.8);
    fogDensity *= lerp(1.0, heightFog, heightInfluence);

    // Atmospheric desaturation: distant objects lose color before disappearing.
    // Mimics Rayleigh scattering — blue haze at medium distances.
    float desatFactor = saturate(linearDist * 1.6 - 0.1);
    desatFactor *= desatFactor;
    float luminance = dot(color, float3(0.299, 0.587, 0.114));
    float3 desaturated = lerp(color, float3(luminance, luminance, luminance), desatFactor * 0.55);
    // Slight blue shift for atmospheric perspective.
    desaturated = lerp(desaturated, desaturated * float3(0.92, 0.95, 1.08), desatFactor * 0.4);

    // Final fog blend — apply to the already-desaturated color.
    color = lerp(desaturated, skyColor, fogDensity);

    // Underwater tint — when camera is below water surface (Y <= -1).
    if (camera.positionYaw.y < waterSurfaceY)
    {
        float depth = saturate((waterSurfaceY - camera.positionYaw.y) * 0.15);
        float distFade = saturate(input.viewDepth * 0.012);
        float underwaterFog = saturate(depth * 0.35 + distFade * 0.32);
        color = lerp(color, waterTint, underwaterFog);
        color *= float3(0.86, 0.94, 1.04);
    }

    return float4(color, alpha);
}
