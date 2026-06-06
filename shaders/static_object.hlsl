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

VSOutput VSMain(VSInput input)
{
    const float3 worldPosition =
        input.instancePosition.xyz
        + input.instanceRight.xyz * input.position.x
        + input.instanceUp.xyz * input.position.y
        + input.instanceForward.xyz * input.position.z;

    const float3 worldNormal = normalize(
        input.instanceRight.xyz * input.normal.x
        + input.instanceUp.xyz * input.normal.y
        + input.instanceForward.xyz * input.normal.z);

    const float3 delta = worldPosition - camera.positionYaw.xyz;
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
