struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 fogColorHasSky;
    float4 fogDistances;
    float4 skyLayers;
    float4 waterInfo;
    float4 skyTuning0; // x=skyPower, y=vOffset, z=vScale, w=style: 0 default, 1 storm, 2 snow, 3 sunset, 4 night
    float4 skyTuning1; // x=primaryUScale, y=primaryVScale, z=primaryAlpha, w=primaryVOffset
    float4 skyTuning2; // x=secondaryUScale, y=secondaryVScale, z=secondaryAlpha, w=secondaryVOffset
};

[[vk::push_constant]]
CameraConstants camera;

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
Texture2DArray worldTexture : register(t0, space0);

[[vk::combinedImageSampler]]
[[vk::binding(0, 0)]]
SamplerState worldSampler : register(s0, space0);

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash21(cell);
    float b = hash21(cell + float2(1, 0));
    float c = hash21(cell + float2(0, 1));
    float d = hash21(cell + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p)
{
    float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);
    float sum = 0.0;
    float amp = 0.55;
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        sum += valueNoise(p) * amp;
        p = mul(rot, p * 2.05 + 19.17);
        amp *= 0.50;
    }
    return sum;
}

float distanceToSegment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 0.00001));
    return length(pa - ba * h);
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    VSOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float pi = 3.14159265359;
    float2 ndc = input.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    const float yaw = camera.positionYaw.w;
    const float pitch = camera.pitchAspectFov.x;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;

    float3 cameraDir = normalize(float3(ndc.x * tanHalfFov * aspect, ndc.y * tanHalfFov, 1.0));
    const float cy = cos(yaw);
    const float sy = sin(yaw);
    const float cp = cos(pitch);
    const float sp = sin(pitch);

    const float yawZ = -sp * cameraDir.y + cp * cameraDir.z;
    float3 worldDir = normalize(float3(
        cy * cameraDir.x + sy * yawZ,
        cp * cameraDir.y + sp * cameraDir.z,
        -sy * cameraDir.x + cy * yawZ));

    const float skyAmount = saturate(worldDir.y);
    const float horizon = saturate(worldDir.y * 3.0 + 0.5);
    // Shaiya sky BMPs are tiny palette/gradient textures. Use the full
    // vertical relation from horizon (bottom) to zenith (top), not half of it.
    const float skyPower = max(0.05, camera.skyTuning0.x);
    const float skyVertical = saturate((1.0 - pow(skyAmount, skyPower)) * camera.skyTuning0.z + camera.skyTuning0.y);
    const float skyU = frac(atan2(worldDir.x, worldDir.z) / (2.0 * pi) + 0.5);
    const float time = camera.waterInfo.z;
    const float weatherStyle = round(camera.skyTuning0.w);
    const bool stormSky = weatherStyle > 0.5 && weatherStyle < 1.5;
    const bool snowSky = weatherStyle >= 1.5 && weatherStyle < 2.5;
    const bool sunsetSky = weatherStyle >= 2.5 && weatherStyle < 3.5;
    const bool nightSky = weatherStyle >= 3.5;

    float3 worldFog = saturate(camera.fogColorHasSky.rgb);
    float hasWorldSky = camera.fogColorHasSky.a;
    if (hasWorldSky > 0.5)
    {
        float3 sunDir = normalize(float3(-0.45, 0.46, 0.77));
        float sunDot = saturate(dot(worldDir, sunDir));
        float up = saturate(worldDir.y);

        float3 horizonColor = lerp(float3(0.74, 0.83, 0.94), worldFog * 1.14, 0.35);
        float3 midColor = float3(0.38, 0.57, 0.86);
        float3 zenithColor = float3(0.10, 0.23, 0.52);
        if (stormSky)
        {
            horizonColor = float3(0.43, 0.45, 0.48);
            midColor = float3(0.30, 0.33, 0.37);
            zenithColor = float3(0.18, 0.20, 0.24);
        }
        else if (snowSky)
        {
            horizonColor = float3(0.72, 0.74, 0.76);
            midColor = float3(0.58, 0.62, 0.66);
            zenithColor = float3(0.46, 0.50, 0.55);
        }
        else if (sunsetSky)
        {
            horizonColor = float3(1.00, 0.42, 0.20);
            midColor = float3(0.78, 0.30, 0.36);
            zenithColor = float3(0.16, 0.12, 0.35);
        }
        else if (nightSky)
        {
            horizonColor = float3(0.035, 0.045, 0.085);
            midColor = float3(0.018, 0.028, 0.070);
            zenithColor = float3(0.004, 0.010, 0.032);
        }
        float3 color = lerp(horizonColor, midColor, smoothstep(0.02, 0.45, up));
        color = lerp(color, zenithColor, smoothstep(0.42, 1.0, up));

        float3 lightDir = sunDir;
        if (sunsetSky)
            lightDir = normalize(float3(-0.75, 0.12, 0.64));
        else if (nightSky)
            lightDir = normalize(float3(0.45, 0.34, 0.82));
        sunDot = saturate(dot(worldDir, lightDir));

        float sunCore = pow(sunDot, sunsetSky ? 520.0 : 950.0);
        float sunGlow = pow(sunDot, sunsetSky ? 8.0 : 16.0) * (sunsetSky ? 0.62 : 0.34) + pow(sunDot, 4.0) * (sunsetSky ? 0.20 : 0.10);
        float sunStrength = stormSky ? 0.08 : (snowSky ? 0.18 : (nightSky ? 0.0 : 1.0));
        color += (float3(1.0, 0.74, 0.42) * sunGlow + float3(1.0, 0.88, 0.58) * sunCore) * sunStrength;

        if (nightSky && worldDir.y > 0.02)
        {
            float horizonFade = smoothstep(0.02, 0.25, worldDir.y);
            float2 starP = float2(skyU * 920.0, skyAmount * 420.0);
            float starCell = hash21(floor(starP));
            float2 starLocal = frac(starP) - 0.5;
            float starShape = smoothstep(0.024, 0.0, length(starLocal));
            float stars = starShape * smoothstep(0.975, 1.0, starCell) * horizonFade;

            float2 fineStarP = float2(skyU * 1480.0 + 37.0, skyAmount * 690.0 - 19.0);
            float fineCell = hash21(floor(fineStarP));
            float2 fineLocal = frac(fineStarP) - 0.5;
            float fineShape = smoothstep(0.014, 0.0, length(fineLocal));
            stars += fineShape * smoothstep(0.955, 1.0, fineCell) * horizonFade * 0.46;

            float2 hazeStarP = float2(skyU * 560.0 - 11.0, skyAmount * 260.0 + 53.0);
            float hazeCell = hash21(floor(hazeStarP));
            float2 hazeLocal = frac(hazeStarP) - 0.5;
            float hazeShape = smoothstep(0.020, 0.0, length(hazeLocal));
            stars += hazeShape * smoothstep(0.940, 1.0, hazeCell) * horizonFade * 0.24;
            color += float3(0.75, 0.82, 1.0) * stars;

            float moonDot = saturate(dot(worldDir, lightDir));
            float moon = smoothstep(0.9991, 0.9998, moonDot);
            float moonHalo = pow(moonDot, 90.0) * 0.16;
            color += float3(0.78, 0.84, 1.0) * moonHalo + float3(0.92, 0.94, 0.88) * moon;

            float2 skyP = float2(skyU, skyAmount);
            [unroll]
            for (int meteorIndex = 0; meteorIndex < 2; ++meteorIndex)
            {
                float period = meteorIndex == 0 ? 13.0 : 21.0;
                float phaseTime = time + (meteorIndex == 0 ? 1.7 : 8.9);
                float eventId = floor(phaseTime / period);
                float phase = frac(phaseTime / period);
                float active = 1.0 - smoothstep(0.11, 0.18, phase);
                active *= smoothstep(0.00, 0.025, phase);

                float startX = hash21(float2(eventId, 17.0 + meteorIndex * 31.0)) * 0.82 + 0.08;
                float startY = hash21(float2(eventId, 91.0 + meteorIndex * 43.0)) * 0.34 + 0.58;
                float2 dir = normalize(float2(0.28 + hash21(float2(eventId, 12.0)) * 0.22, -0.16 - hash21(float2(eventId, 27.0)) * 0.18));
                float2 head = float2(startX, startY) + dir * (phase * 1.55);
                float2 tail = head - dir * (0.075 + phase * 0.20);
                float streakDist = distanceToSegment(skyP, tail, head);
                float headDist = length(skyP - head);
                float streak = smoothstep(0.012, 0.0, streakDist) * active * horizonFade;
                float headGlow = smoothstep(0.026, 0.0, headDist) * active * horizonFade;
                color += float3(0.72, 0.82, 1.0) * streak * 0.80 + float3(0.95, 0.98, 1.0) * headGlow * 1.25;
            }
        }

        float cloudMask = 0.0;
        if (worldDir.y > 0.035 && !nightSky)
        {
            float tCloud = 3400.0 / max(worldDir.y, 0.035);
            float2 p = (camera.positionYaw.xz + worldDir.xz * tCloud) * 0.00030 + float2(time * 0.012, time * 0.0044);
            float2 warp = float2(fbm(p * 0.72 + 31.7), fbm(p * 0.72 - 18.4));
            p += (warp - 0.5) * 1.65;
            float body = fbm(p);
            float detail = fbm(p * 3.2 + float2(time * 0.032, -time * 0.011));
            float wisps = fbm(float2(p.x * 1.85 + time * 0.038, p.y * 0.52));
            float cloudField = body + detail * (stormSky ? 0.34 : 0.22);
            if (stormSky)
                cloudMask = smoothstep(0.36, 0.62, cloudField);
            else if (snowSky)
                cloudMask = smoothstep(0.42, 0.68, cloudField);
            else if (sunsetSky)
                cloudMask = smoothstep(0.48, 0.74, cloudField);
            else
                cloudMask = smoothstep(0.54, 0.78, cloudField);
            cloudMask *= smoothstep(0.22, 0.72, wisps);
            cloudMask = saturate(cloudMask - smoothstep(0.36, 0.78, detail) * 0.18);
            cloudMask *= smoothstep(0.04, 0.18, worldDir.y) * (1.0 - smoothstep(0.88, 1.0, worldDir.y));
            float cloudShade = lerp(0.74, 1.08, saturate(dot(worldDir, sunDir) * 0.5 + 0.5));
            float3 cloudColor = lerp(float3(0.62, 0.67, 0.72), float3(1.0, 0.96, 0.88), cloudShade);
            if (stormSky)
                cloudColor = lerp(float3(0.20, 0.22, 0.25), float3(0.43, 0.45, 0.49), cloudShade);
            else if (snowSky)
                cloudColor = lerp(float3(0.68, 0.70, 0.73), float3(0.90, 0.92, 0.94), cloudShade);
            else if (sunsetSky)
                cloudColor = lerp(float3(0.42, 0.18, 0.28), float3(1.0, 0.52, 0.30), cloudShade);
            float cloudAlpha = stormSky ? 0.84 : (snowSky ? 0.76 : (sunsetSky ? 0.70 : 0.72));
            color = lerp(color, cloudColor, cloudMask * cloudAlpha);
        }

        float horizonBlend = saturate((0.12 - worldDir.y) / 0.18);
        color = lerp(color, worldFog, horizonBlend * 0.35);
        if (camera.positionYaw.y < 0.0)
        {
            const float3 waterTint = float3(0.12, 0.25, 0.34);
            float depth = saturate((0.0 - camera.positionYaw.y) * 0.12);
            color = lerp(color * float3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
        }
        return float4(color, 1.0);
    }

    float3 zenith = float3(0.16, 0.28, 0.58);
    float3 midSky = float3(0.38, 0.55, 0.80);
    float3 horizonColor = float3(0.68, 0.80, 0.92);
    if (stormSky)
    {
        zenith = float3(0.18, 0.20, 0.24);
        midSky = float3(0.30, 0.33, 0.37);
        horizonColor = float3(0.43, 0.45, 0.48);
    }
    else if (snowSky)
    {
        zenith = float3(0.46, 0.50, 0.55);
        midSky = float3(0.58, 0.62, 0.66);
        horizonColor = float3(0.72, 0.74, 0.76);
    }
    else if (sunsetSky)
    {
        zenith = float3(0.16, 0.12, 0.35);
        midSky = float3(0.78, 0.30, 0.36);
        horizonColor = float3(1.00, 0.42, 0.20);
    }
    else if (nightSky)
    {
        zenith = float3(0.004, 0.010, 0.032);
        midSky = float3(0.018, 0.028, 0.070);
        horizonColor = float3(0.035, 0.045, 0.085);
    }
    zenith = lerp(zenith, worldFog * 0.72, hasWorldSky);
    midSky = lerp(midSky, worldFog * 1.08, hasWorldSky);
    horizonColor = lerp(horizonColor, worldFog * 1.22, hasWorldSky);
    float3 ground  = lerp(float3(0.25, 0.28, 0.22), worldFog * 0.55, hasWorldSky);

    float3 color;
    if (horizon > 0.72)
        color = lerp(midSky, zenith, saturate((horizon - 0.72) / 0.28));
    else if (horizon > 0.50)
        color = lerp(horizonColor, midSky, saturate((horizon - 0.50) / 0.22));
    else if (horizon > 0.34)
        color = lerp(ground, horizonColor, saturate((horizon - 0.34) / 0.16));
    else
        color = ground;

    if (camera.positionYaw.y < 0.0)
    {
        const float3 waterTint = float3(0.12, 0.25, 0.34);
        float depth = saturate((0.0 - camera.positionYaw.y) * 0.12);
        color = lerp(color * float3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
    }

    return float4(color, 1.0);
}
