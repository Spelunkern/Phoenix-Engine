// GPU-skinned character pipeline for mobs/NPCs. Identical to static_object.hlsl
// except the vertex is skinned on the GPU: the bind-pose position/normal are
// transformed by a per-bone palette (mesh-bind * animated final, model space)
// before the per-instance placement transform. The palette lives in a storage
// buffer (set 1); each instance's first bone is packed (bit-cast) into
// instanceRight.w. The MANI rotation path is dropped — characters don't use it,
// and that .w lane now carries the palette base.
//
// Palette layout: StructuredBuffer<float4>, 4 float4 per bone = the 4 rows of
// the model-space skin matrix in the engine's row convention, so a point is
// p' = p.x*row0 + p.y*row1 + p.z*row2 + row3 (matches CPU transform_point).

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint textureLayer : TEXCOORD1;
    uint4 boneIndices : TEXCOORD2;   // global mesh-bone indices (xyz used)
    float4 boneWeights : TEXCOORD3;  // matching weights (xyz used)
    float4 instanceRight : TEXCOORD4;
    float4 instanceUp : TEXCOORD5;
    float4 instanceForward : TEXCOORD6;
    float4 instancePosition : TEXCOORD7;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureLayer : TEXCOORD1;
    float fogFactor : TEXCOORD2;
    float lighting : TEXCOORD3;
    float3 worldPos : TEXCOORD4;
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 precomputedTrig;
    float4 fogColorHasSky;
    float4 fogDistances;
    float4 skyLayers;
    float4 waterInfo;
    float4 skyTuning0;
    float4 skyTuning1;
    float4 skyTuning2;
    float4 waterStyle;
    float4 assetTuning0;
    float4 assetTuning1;
    float4 assetTuning2;
    float4 assetTuning3;
};

[[vk::push_constant]]
CameraConstants camera;

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
Texture2DArray terrainTexture : register(t0, space0);

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
SamplerState terrainSampler : register(s0, space0);

[[vk::combinedImageSampler]]
[[vk::binding(2, 0)]]
Texture2DArray lightmapTexture : register(t2, space0);

[[vk::combinedImageSampler]]
[[vk::binding(2, 0)]]
SamplerState lightmapSampler : register(s2, space0);

[[vk::binding(0, 1)]]
StructuredBuffer<float4> bonePalette : register(t0, space1);

float3 skinPoint(uint base, uint3 bones, float3 w, float3 p)
{
    float3 result = float3(0, 0, 0);
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        if (w[i] <= 0.0)
            continue;
        const uint r = (base + bones[i]) * 4u;
        result += w[i] * (p.x * bonePalette[r + 0u].xyz
                        + p.y * bonePalette[r + 1u].xyz
                        + p.z * bonePalette[r + 2u].xyz
                        + bonePalette[r + 3u].xyz);
    }
    return result;
}

float3 skinNormal(uint base, uint3 bones, float3 w, float3 n)
{
    float3 result = float3(0, 0, 0);
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        if (w[i] <= 0.0)
            continue;
        const uint r = (base + bones[i]) * 4u;
        result += w[i] * (n.x * bonePalette[r + 0u].xyz
                        + n.y * bonePalette[r + 1u].xyz
                        + n.z * bonePalette[r + 2u].xyz);
    }
    return result;
}

VSOutput VSMain(VSInput input)
{
    float cullDist = input.instancePosition.w;
    if (cullDist > 0.0)
    {
        cullDist = min(cullDist, camera.pitchAspectFov.w);
        const float3 instDelta = input.instancePosition.xyz - camera.positionYaw.xyz;
        if (dot(instDelta, instDelta) > cullDist * cullDist)
        {
            VSOutput culled = (VSOutput)0;
            culled.position = float4(0, 0, -1, 1);
            return culled;
        }
    }

    // GPU skinning: model-space skinned bind vertex via the per-instance palette.
    // right.w carries the palette base bone index as an exact float value.
    const uint paletteBase = (uint)(input.instanceRight.w + 0.5);
    float totalWeight = input.boneWeights.x + input.boneWeights.y + input.boneWeights.z;
    float3 localPos;
    float3 localNrm;
    if (totalWeight > 0.0001)
    {
        const float inv = 1.0 / totalWeight;
        localPos = skinPoint(paletteBase, input.boneIndices.xyz, input.boneWeights.xyz, input.position) * inv;
        localNrm = skinNormal(paletteBase, input.boneIndices.xyz, input.boneWeights.xyz, input.normal) * inv;
    }
    else
    {
        localPos = input.position;
        localNrm = input.normal;
    }

    // Per-instance placement (yaw + scale + world position). right/up/forward
    // carry the basis; .w lanes are palette base (right.w) / unused.
    const float3 right   = input.instanceRight.xyz;
    const float3 up      = input.instanceUp.xyz;
    const float3 forward = input.instanceForward.xyz;

    const float3 worldPosition =
        input.instancePosition.xyz
        + right * localPos.x
        + up * localPos.y
        + forward * localPos.z;

    const float3 delta = worldPosition - camera.positionYaw.xyz;

    const float3 worldNormal = normalize(
        right * localNrm.x
        + up * localNrm.y
        + forward * localNrm.z);

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
    float nDotL = dot(worldNormal, lightDir);
    float lit = saturate(nDotL) * camera.assetTuning1.y + camera.assetTuning1.x;

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
    output.normal = worldNormal;
    output.uv = input.uv;
    output.textureLayer = input.textureLayer;
    output.fogFactor = fogFactor;
    output.lighting = lit;
    output.worldPos = worldPosition;
    return output;
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

    float3 color;
    bool assetGrade = true;
    if (input.textureLayer != 0xFFFFFFFFu)
    {
        uint sampleLayer = input.textureLayer;
        bool alphaCutout = false;
        if (sampleLayer >= 2048)
        {
            alphaCutout = true;
            sampleLayer -= 2048;
        }
        float4 textureColor = terrainTexture.Sample(terrainSampler,
            float3(input.uv, (float)sampleLayer));

        bool isCharacter = (input.color.r < 0.01 && input.color.g < 0.01 && input.color.b < 0.01);
        if (isCharacter)
        {
            assetGrade = false;
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
            else
                clip(textureColor.a - 0.01);
            if (input.color.b >= 1.5 && camera.fogDistances.w > 0.5)
            {
                float2 lmUV = input.color.rg;
                float lmLayer = input.color.b - 2.0;
                float3 lm = lightmapTexture.Sample(lightmapSampler,
                    float3(lmUV, lmLayer)).rgb;
                color = textureColor.rgb * lm * camera.assetTuning1.z;
            }
            else
                color = textureColor.rgb * input.lighting;
        }
    }
    else
    {
        color = input.color * input.lighting;
    }

    if (assetGrade)
    {
        float luma = dot(color, float3(0.299, 0.587, 0.114));
        color = saturate(lerp(luma.xxx, color, camera.assetTuning0.w));
        color = saturate(color * camera.assetTuning0.rgb
            * (camera.assetTuning1.w + camera.assetTuning2.x * skyColor));
    }

    color = applyUnderwaterView(color, input.worldPos);
    color = lerp(color, skyColor, input.fogFactor);
    return float4(color, 1.0);
}
