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
    float viewDepth : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 fogColorHasSky;
    float4 fogDistances;
};

[[vk::push_constant]]
CameraConstants camera;

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
Texture2DArray terrainTexture : register(t0, space0);

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
SamplerState terrainSampler : register(s0, space0);

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
    output.normal = worldNormal;
    output.uv = input.uv;
    output.textureLayer = input.textureLayer;
    output.viewDepth = cameraZ;
    output.worldPos = worldPosition;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // ---- Early fog discard ----
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

        // Character vertices have color=(0,0,0) marker — apply Shaiya character
        // lighting (diffuse + glow) instead of world object lighting.
        bool isCharacter = (input.color.r < 0.01 && input.color.g < 0.01 && input.color.b < 0.01);
        if (isCharacter)
        {
            if (alphaCutout)
                clip(textureColor.a - 0.08);

            color = textureColor.rgb;
            float3 n = normalize(input.normal);
            const float3 charLightDir = normalize(float3(-0.35, 0.85, -0.28));
            float diffuse = saturate(dot(n, charLightDir));
            float3 lit = color * (0.56 + diffuse * 0.72);

            if (!alphaCutout)
            {
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
            if (alphaCutout)
                clip(textureColor.a - 0.3);
            else
                clip(textureColor.a - 0.01);

            color = textureColor.rgb;
            float3 n = normalize(input.normal);
            float nDotL = dot(n, lightDir);
            float lighting = saturate(nDotL) * 0.50 + 0.50;
            color *= lighting;
        }
    }
    else
    {
        color = input.color;
    }

    if (input.worldPos.y < waterSurfaceY)
    {
        const float submersion = saturate((waterSurfaceY - input.worldPos.y) * 0.45);
        const float tintAmount = 0.14 + submersion * 0.26;
        color = lerp(color * float3(0.84, 0.93, 1.02), waterTint, tintAmount);
    }

    // Atmospheric distance rendering.
    float fogStart = camera.fogDistances.x;
    float fogEnd = max(fogStart + 1.0, camera.fogDistances.y);
    float linearDist = saturate((input.viewDepth - fogStart) / (fogEnd - fogStart));

    // Exponential fog — objects are fully covered well before cull boundary.
    float fogDensity = 1.0 - exp(-linearDist * linearDist * 5.0);

    // Height-based fog: lower areas accumulate more atmospheric haze.
    // Fades out at long range so geometry at the cull edge is always hidden.
    float avgHeight = (camera.positionYaw.y + input.worldPos.y) * 0.5;
    float heightFog = saturate(1.0 - avgHeight / 350.0) * 0.4 + 0.6;
    float heightInfluence = saturate(1.0 - linearDist * 1.8);
    fogDensity *= lerp(1.0, heightFog, heightInfluence);

    // Atmospheric desaturation — objects lose color before fading entirely.
    float desatFactor = saturate(linearDist * 1.6 - 0.1);
    desatFactor *= desatFactor;
    float luminance = dot(color, float3(0.299, 0.587, 0.114));
    float3 desaturated = lerp(color, float3(luminance, luminance, luminance), desatFactor * 0.55);
    // Subtle blue shift for atmospheric scattering.
    desaturated = lerp(desaturated, desaturated * float3(0.92, 0.95, 1.08), desatFactor * 0.4);

    // Final atmospheric blend.
    color = lerp(desaturated, skyColor, fogDensity);

    if (camera.positionYaw.y < 0.0)
    {
        float depth = saturate((0.0 - camera.positionYaw.y) * 0.15);
        float distFade = saturate(input.viewDepth * 0.008);
        float underwaterFog = saturate(depth * 0.35 + distFade * 0.32);
        color = lerp(color, waterTint, underwaterFog);
        color *= float3(0.86, 0.94, 1.04);
    }

    return float4(color, 1.0);
}
