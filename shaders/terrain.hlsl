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
    float fogFactor : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
    float lighting : TEXCOORD4;
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 precomputedTrig; // cosYaw, sinYaw, cosPitch, sinPitch
    float4 fogColorHasSky;
    float4 fogDistances;
    float4 skyLayers;
    float4 waterInfo;
    float4 skyTuning0;
    float4 skyTuning1;
    float4 skyTuning2;
    float4 waterStyle;
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

[[vk::combinedImageSampler]]
[[vk::binding(2, 0)]]
Texture2DArray lightmapTexture : register(t2, space0);

[[vk::combinedImageSampler]]
[[vk::binding(2, 0)]]
SamplerState lightmapSampler : register(s2, space0);

VSOutput VSMain(VSInput input)
{
    const float3 delta = input.position - camera.positionYaw.xyz;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;
    const float farPlane = camera.pitchAspectFov.w;

    const float cy = camera.precomputedTrig.x;
    const float sy = camera.precomputedTrig.y;
    const float cp = camera.precomputedTrig.z;
    const float sp = camera.precomputedTrig.w;

    const float cameraX = cy * delta.x - sy * delta.z;
    const float yawZ = sy * delta.x + cy * delta.z;
    const float cameraY = cp * delta.y - sp * yawZ;
    const float cameraZ = sp * delta.y + cp * yawZ;
    const float nearPlane = 2.0f;

    const float3 lightDir = float3(-0.30, 0.68, -0.67);
    float3 n = normalize(input.normal);
    float nDotL = saturate(dot(n, lightDir));
    float lit = nDotL * 0.55 + 0.45;

    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float fogLinear = saturate((cameraZ - fogStart) / (fogEnd - fogStart));
    float fogFactor = 1.0 - exp(-fogLinear * fogLinear * 5.0);

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
    output.fogFactor = fogFactor;
    output.worldPos = input.position;
    output.lighting = lit;
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
    float2 uv = (worldPos.xz + mapSize * 0.5) / mapSize;
    float2 cellF = uv * (float)(mapSide - 1) - 0.5;
    int2 cell0 = int2(floor(cellF));
    float2 frac_ = cellF - float2(cell0);

    uint maxCell = mapSide - 2;
    uint x0 = clamp((uint)cell0.x, 0, maxCell);
    uint z0 = clamp((uint)cell0.y, 0, maxCell);
    uint x1 = min(x0 + 1, maxCell);
    uint z1 = min(z0 + 1, maxCell);

    uint l00 = terrainMapLoad(x0, z0, mapSide);
    uint l10 = terrainMapLoad(x1, z0, mapSide);
    uint l01 = terrainMapLoad(x0, z1, mapSide);
    uint l11 = terrainMapLoad(x1, z1, mapSide);

    float3 c00 = sampleTerrainLayer(l00, worldPos, mapSize, mapSide);
    float3 c10 = sampleTerrainLayer(l10, worldPos, mapSize, mapSide);
    float3 c01 = sampleTerrainLayer(l01, worldPos, mapSize, mapSide);
    float3 c11 = sampleTerrainLayer(l11, worldPos, mapSize, mapSide);

    float2 t = frac_ * frac_ * (3.0 - 2.0 * frac_);

    if (l00 == l10 && l00 == l01 && l00 == l11)
        return c00;

    float3 top = lerp(c00, c10, t.x);
    float3 bot = lerp(c01, c11, t.x);
    return lerp(top, bot, t.y);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    if (input.fogFactor >= 0.995)
        discard;

    const float3 skyColor = saturate(camera.fogColorHasSky.rgb);
    const float mapSize = camera.skyLayers.z;
    const uint mapSide = (uint)camera.skyLayers.w;

    float3 color;
    float alpha = 1.0;

    if (input.textureLayer == 0xFFFFFFFDu && mapSide > 1)
    {
        color = blendedTerrainColor(input.worldPos, mapSize, mapSide);

        uint centerLayer = terrainMapLookup(input.worldPos, mapSize, mapSide);
        const uint waterLayer = (uint)camera.skyLayers.y;
        if (centerLayer == waterLayer)
            color = lerp(float3(0.01, 0.08, 0.34), color * float3(0.42, 0.70, 1.22), 0.45);

        color *= input.lighting;
    }
    else if (input.textureLayer != 0xFFFFFFFFu)
    {
        if (input.textureLayer == 0xFFFFFFFEu)
            return float4(input.color, 0.85);

        uint sampleLayer = input.textureLayer;
        bool alphaCutout = false;
        if (sampleLayer >= 2048)
        {
            alphaCutout = true;
            sampleLayer -= 2048;
        }
        const uint waterLayer = (uint)camera.skyLayers.y;
        const bool isWater = sampleLayer == waterLayer;

        if (isWater)
        {
            float t = camera.waterInfo.z;
            float2 wp = input.worldPos.xz;
            float2 d1 = wp * 0.08 + float2(t * 0.6, t * 0.3);
            float2 d2 = wp * 0.12 + float2(-t * 0.4, t * 0.5);
            float ripple = sin(d1.x + d1.y) * 0.5 + sin(d2.x - d2.y) * 0.5;
            float3 base = camera.waterStyle.rgb;
            float highlight = saturate(ripple * 0.4 + 0.5);
            float3 c = lerp(base * 0.85, base * 1.15, highlight);
            c += float3(0.04, 0.06, 0.08) * highlight * highlight;
            return float4(c, camera.waterStyle.a);
        }
        else
        {
            float4 textureColor = terrainTexture.Sample(terrainSampler,
                float3(input.uv, (float)sampleLayer));

            bool isCharacter = (input.color.r < 0.01 && input.color.g < 0.01 && input.color.b < 0.01);
            if (isCharacter)
            {
                if (alphaCutout)
                    clip(textureColor.a - 0.08);
                color = textureColor.rgb;
                float3 n = normalize(input.normal);
                const float3 charLightDir = float3(-0.33, 0.80, -0.26);
                float diffuse = saturate(dot(n, charLightDir));
                float3 lit = color * (0.56 + diffuse * 0.72);
                if (!alphaCutout)
                    color = saturate(lit + color * textureColor.a * 0.30);
                else
                    color = lit;
            }
            else
            {
                if (alphaCutout)
                    clip(textureColor.a - 0.3);
                color = textureColor.rgb * input.lighting;
            }
        }
    }
    else
    {
        color = input.color;
    }

    if (camera.fogDistances.w > 0.5)
    {
        const float halfMap = mapSize * 0.5;
        const uint sections = (uint)camera.fogDistances.z;
        float2 worldUv = float2(
            (input.worldPos.x + halfMap) / mapSize,
            (input.worldPos.z + halfMap) / mapSize);
        uint secX = clamp((uint)(worldUv.x * sections), 0, sections - 1);
        uint secZ = clamp((uint)(worldUv.y * sections), 0, sections - 1);
        uint layer = secZ * sections + secX;
        float2 secUv = float2(
            frac(worldUv.x * sections),
            frac(worldUv.y * sections));
        float3 lm = lightmapTexture.Sample(lightmapSampler, float3(secUv, (float)layer)).rgb;
        color *= lm;
    }

    color = lerp(color, skyColor, input.fogFactor);
    return float4(color, alpha);
}
