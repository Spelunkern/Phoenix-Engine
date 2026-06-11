struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint textureLayer : TEXCOORD1;
    float4 instanceRight : TEXCOORD2;
    float4 instanceUp : TEXCOORD3;
    float4 instanceForward : TEXCOORD4;
    float4 instancePosition : TEXCOORD5;
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
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 precomputedTrig; // cosYaw, sinYaw, cosPitch, sinPitch
    float4 fogColorHasSky;
    float4 fogDistances;
    float4 skyLayers;
    float4 waterInfo; // .x=baseLayer, .y=frameCount, .z=time, .w=tileSize
    float4 skyTuning0;
    float4 skyTuning1;
    float4 skyTuning2;
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

// Rodrigues rotation: rotate vector v around unit axis k by angle a.
float3 rodrigues(float3 v, float3 k, float cosA, float sinA)
{
    return v * cosA + cross(k, v) * sinA + k * dot(k, v) * (1.0 - cosA);
}

VSOutput VSMain(VSInput input)
{
    // Distance culling first: instancePosition.w > 0 = distance-cullable
    // instance, clamped to the universal render distance (pitchAspectFov.w).
    // Tested against the INSTANCE ORIGIN so every vertex of an instance agrees
    // — a per-vertex test lets triangles straddle the boundary and smear
    // stretched geometry across the screen. Runs before any rotation math so
    // culled vertices exit with minimal ALU cost.
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

    // MANI rotation: world axis * speed packed in instance .w components.
    float3 axisSpeed = float3(input.instanceRight.w, input.instanceUp.w, input.instanceForward.w);
    float speedSq = dot(axisSpeed, axisSpeed);

    float3 right   = input.instanceRight.xyz;
    float3 up      = input.instanceUp.xyz;
    float3 forward = input.instanceForward.xyz;

    if (speedSq > 0.000001)
    {
        float speed = sqrt(speedSq);
        float3 axis = axisSpeed / speed;
        float angle = camera.waterInfo.z * speed; // waterInfo.z = totalTime
        float cosA = cos(angle);
        float sinA = sin(angle);
        right   = rodrigues(right,   axis, cosA, sinA);
        up      = rodrigues(up,      axis, cosA, sinA);
        forward = rodrigues(forward, axis, cosA, sinA);
    }

    const float3 worldPosition =
        input.instancePosition.xyz
        + right * input.position.x
        + up * input.position.y
        + forward * input.position.z;

    const float3 delta = worldPosition - camera.positionYaw.xyz;

    const float3 worldNormal = normalize(
        right * input.normal.x
        + up * input.normal.y
        + forward * input.normal.z);
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
    float lit = saturate(nDotL) * 0.50 + 0.50;

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
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    if (input.fogFactor >= 0.995)
        discard;

    const float3 skyColor = saturate(camera.fogColorHasSky.rgb);

    float3 color;
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
            // color.b >= 2.0 means vertex carries lightmap UV + layer (encoded
            // as color.rg = UV, color.b = layer + 2.0). Sample GPU lightmap
            // at full pixel resolution — matches original dungeon pipeline.
            // fogDistances.w gates on the lightmap array actually being bound.
            if (input.color.b >= 1.5 && camera.fogDistances.w > 0.5)
            {
                float2 lmUV = input.color.rg;
                float lmLayer = input.color.b - 2.0;
                float3 lm = lightmapTexture.Sample(lightmapSampler,
                    float3(lmUV, lmLayer)).rgb;
                color = textureColor.rgb * lm * 2.0;
            }
            else
                color = textureColor.rgb * input.lighting;
        }
    }
    else
    {
        color = input.color * input.lighting;
    }

    color = lerp(color, skyColor, input.fogFactor);
    return float4(color, 1.0);
}
