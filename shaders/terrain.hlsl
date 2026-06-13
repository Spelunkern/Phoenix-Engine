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
    float4 charTuning0; // key light rgb, albedo saturation
    float4 charTuning1; // ambient ground rgb, diffuse strength
    float4 charTuning2; // ambient sky rgb, wrap amount
    float4 charTuning3; // spec intensity, rim intensity, weather base, weather scale
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

// Explicit-gradient layer sample so it stays correct inside divergent
// branches (gradients are derived from worldPos before any branching).
float3 sampleTerrainLayerGrad(uint layer, float3 worldPos, float2 ddxWp, float2 ddyWp,
                              float mapSize, uint mapSide)
{
    float tileSize = terrainTileSize(layer, mapSide);
    float halfMap = mapSize * 0.5;
    float2 tileUv = float2(
        (worldPos.x + halfMap) / tileSize,
        (worldPos.z + halfMap) / tileSize);
    return terrainTexture.SampleGrad(terrainSampler, float3(tileUv, (float)layer),
        ddxWp / tileSize, ddyWp / tileSize).rgb;
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

    // Gradients computed in uniform control flow, valid inside the branch.
    float2 ddxWp = ddx(worldPos.xz);
    float2 ddyWp = ddy(worldPos.xz);

    float3 c00 = sampleTerrainLayerGrad(l00, worldPos, ddxWp, ddyWp, mapSize, mapSide);

    // Fast path: all four corners use the same layer (vast majority of
    // terrain pixels) — one sample instead of four.
    if (l00 == l10 && l00 == l01 && l00 == l11)
        return c00;

    float3 c10 = sampleTerrainLayerGrad(l10, worldPos, ddxWp, ddyWp, mapSize, mapSide);
    float3 c01 = sampleTerrainLayerGrad(l01, worldPos, ddxWp, ddyWp, mapSize, mapSide);
    float3 c11 = sampleTerrainLayerGrad(l11, worldPos, ddxWp, ddyWp, mapSize, mapSide);

    float2 t = frac_ * frac_ * (3.0 - 2.0 * frac_);
    float3 top = lerp(c00, c10, t.x);
    float3 bot = lerp(c01, c11, t.x);
    return lerp(top, bot, t.y);
}

float3 applyUnderwaterView(float3 color, float3 worldPos)
{
    if (camera.positionYaw.y >= 0.0)
        return color;

    const float3 waterTint = saturate(camera.waterStyle.rgb * 1.25 + float3(0.02, 0.05, 0.08));
    const float viewDistance = length(worldPos - camera.positionYaw.xyz);
    const float cameraDepth = saturate(-camera.positionYaw.y * 0.10);
    const float pixelDepth = saturate(-worldPos.y * 0.05);
    const float absorption = 1.0 - exp(-viewDistance * (0.045 + cameraDepth * 0.030));
    const float underwaterAmount = saturate(absorption * (0.45 + pixelDepth * 0.35) + cameraDepth * 0.18);

    float3 cooled = color * float3(0.62, 0.82, 1.02);
    return lerp(cooled, waterTint, underwaterAmount);
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
    bool applyLightmap = true;

    if (input.textureLayer == 0xFFFFFFFDu && mapSide > 1)
    {
        // Splat metadata stored after the per-cell map + 16 tile sizes:
        // [alphaMaskLayerFlags][splatLayerCount].
        uint mapBytes = mapSide * mapSide;
        uint mapBytesPadded = (mapBytes + 3u) & ~3u;
        uint maskFlags = terrainMap.Load(mapBytesPadded + 64);
        uint splatLayerCount = min(terrainMap.Load(mapBytesPadded + 68), 8u);

        if (maskFlags != 0u && camera.fogDistances.w > 0.5)
        {
            // Alpha-mask splat ("tonality" maps): layer 0 is the base, each
            // masked layer blends on top. The per-layer weights are packed
            // four-per-texture into two RGBA layers per section at
            // [sections^2 + s*2 + p] — 1-2 fetches yield all 7 weights.
            float2 ddxWp = ddx(input.worldPos.xz);
            float2 ddyWp = ddy(input.worldPos.xz);
            const float halfMap = mapSize * 0.5;
            const uint sections = (uint)camera.fogDistances.z;
            float2 worldUv = float2(
                (input.worldPos.x + halfMap) / mapSize,
                (input.worldPos.z + halfMap) / mapSize);
            uint secX = clamp((uint)(worldUv.x * sections), 0, sections - 1);
            uint secZ = clamp((uint)(worldUv.y * sections), 0, sections - 1);
            uint maskBase = sections * sections + (secZ * sections + secX) * 2u;
            float2 secUv = float2(frac(worldUv.x * sections), frac(worldUv.y * sections));

            float4 w0 = lightmapTexture.SampleLevel(lightmapSampler,
                float3(secUv, (float)maskBase), 0);            // weights: layers 1..4
            float4 w1 = (maskFlags & 0xE0u)
                ? lightmapTexture.SampleLevel(lightmapSampler,
                    float3(secUv, (float)(maskBase + 1u)), 0)  // weights: layers 5..7
                : float4(0, 0, 0, 0);

            color = sampleTerrainLayerGrad(0, input.worldPos, ddxWp, ddyWp, mapSize, mapSide);
            [loop]
            for (uint n = 1; n < splatLayerCount; ++n)
            {
                if ((maskFlags & (1u << n)) == 0u)
                    continue;
                uint idx = n - 1u;
                float w = idx < 4u ? w0[idx] : w1[idx & 3u];
                if (w > 0.004)
                    color = lerp(color,
                        sampleTerrainLayerGrad(n, input.worldPos, ddxWp, ddyWp, mapSize, mapSide), w);
            }
        }
        else
        {
            color = blendedTerrainColor(input.worldPos, mapSize, mapSide);
        }

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

        // Character blob shadow: flat black, vertex alpha carried in color.r.
        if (input.textureLayer == 0xFFFFFFFCu)
            return float4(0.0, 0.0, 0.0, input.color.r);

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
            if (camera.positionYaw.y < 0.0)
            {
                const float depth = saturate(-camera.positionYaw.y * 0.10);
                c = lerp(c * float3(0.70, 0.92, 1.18), base * 1.45 + float3(0.02, 0.05, 0.08), 0.45 + depth * 0.25);
                return float4(saturate(c), saturate(camera.waterStyle.a + 0.18 + depth * 0.16));
            }
            return float4(c, camera.waterStyle.a);
        }
        else
        {
            float4 textureColor = terrainTexture.Sample(terrainSampler,
                float3(input.uv, (float)sampleLayer));

            // Character vertices carry color = 0; cloak cloth/collar vertices
            // carry color.b ~0.02 (flat-lit: the simulated cloth re-derives
            // normals every frame, normal-based lighting makes it shimmer).
            bool isCharacter = (input.color.r < 0.01 && input.color.g < 0.01 && input.color.b < 0.05);
            if (isCharacter)
            {
                bool flatLit = input.color.b > 0.01;
                if (alphaCutout)
                    clip(textureColor.a - 0.08);
                color = textureColor.rgb;
                // Revive the albedo: the source textures read washed-out, so
                // push saturation before lighting (luma-preserving).
                float luma = dot(color, float3(0.299, 0.587, 0.114));
                color = saturate(lerp(luma.xxx, color, camera.charTuning0.w));

                const float3 charLightDir = float3(-0.33, 0.80, -0.26);
                // Warm key light vs cool sky ambient — the warm/cool split is
                // what keeps skin and leather lively instead of grey-on-grey.
                // All tunables arrive via charTuning0..3 (live ImGui module).
                const float3 keyColor = camera.charTuning0.rgb;
                const float3 ambientGround = camera.charTuning1.rgb;
                const float3 ambientSky = camera.charTuning2.rgb;
                const float3 weatherTint = camera.charTuning3.z + camera.charTuning3.w * skyColor;
                float3 lit;
                if (flatLit)
                {
                    // Fixed shading, independent of normals and view: matches
                    // the average body response, still tinted by the weather.
                    float3 ambient = lerp(ambientGround, ambientSky, 0.65) * weatherTint;
                    lit = color * (ambient + 0.68 * camera.charTuning1.w * keyColor);
                }
                else
                {
                    float3 n = normalize(input.normal);
                    float nDotL = dot(n, charLightDir);
                    float diffuse = saturate(nDotL);
                    // Soft wrap fills the shaded side so it rolls off instead of
                    // clipping flat; blended with plain Lambert to keep shape.
                    float wrap = saturate((nDotL + 0.4) / 1.4);
                    float shade = lerp(diffuse, wrap, camera.charTuning2.w);

                    // Hemisphere ambient: blue-ish sky from above, warm ground
                    // bounce from below, tinted by the weather sky color so the
                    // character sits in the scene (night/storm darkens it subtly).
                    float hemi = n.y * 0.5 + 0.5;
                    float3 ambient = lerp(ambientGround, ambientSky, hemi) * weatherTint;

                    lit = color * (ambient + shade * camera.charTuning1.w * keyColor);

                    // Subtle Blinn-Phong sheen (armour/metal pop) gated by the
                    // diffuse term, plus a faint sky-tinted rim on the silhouette.
                    float3 viewDir = normalize(camera.positionYaw.xyz - input.worldPos);
                    float3 halfVec = normalize(charLightDir + viewDir);
                    float spec = pow(saturate(dot(n, halfVec)), 24.0) * camera.charTuning3.x * (0.25 + 0.75 * diffuse);
                    float rim = pow(1.0 - saturate(dot(n, viewDir)), 3.0) * camera.charTuning3.y * (0.4 + 0.6 * hemi);
                    lit += spec + rim * (0.5 + 0.5 * skyColor);
                }

                if (!alphaCutout)
                    color = saturate(lit + color * textureColor.a * 0.30);
                else
                    color = saturate(lit);
                applyLightmap = false; // characters keep their own lighting
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

    if (applyLightmap && camera.fogDistances.w > 0.5)
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

    color = applyUnderwaterView(color, input.worldPos);
    color = lerp(color, skyColor, input.fogFactor);
    return float4(color, alpha);
}
