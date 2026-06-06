struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
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
    for (int i = 0; i < 4; ++i)
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
    const bool nightSky = weatherStyle >= 3.5 && weatherStyle < 4.5;
    const bool dawnSky = weatherStyle >= 4.5 && weatherStyle < 5.5;
    const bool duskSky = weatherStyle >= 5.5 && weatherStyle < 6.5;
    const bool afternoonSky = weatherStyle >= 6.5 && weatherStyle < 7.5;
    const bool overcastSky = weatherStyle >= 7.5;

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
        else if (dawnSky)
        {
            horizonColor = float3(0.92, 0.52, 0.28);
            midColor = float3(0.52, 0.38, 0.58);
            zenithColor = float3(0.12, 0.14, 0.38);
        }
        else if (duskSky)
        {
            horizonColor = float3(0.82, 0.34, 0.22);
            midColor = float3(0.38, 0.18, 0.38);
            zenithColor = float3(0.06, 0.06, 0.18);
        }
        else if (afternoonSky)
        {
            horizonColor = float3(0.82, 0.78, 0.68);
            midColor = float3(0.48, 0.58, 0.78);
            zenithColor = float3(0.18, 0.32, 0.62);
        }
        else if (overcastSky)
        {
            horizonColor = float3(0.58, 0.60, 0.62);
            midColor = float3(0.48, 0.50, 0.53);
            zenithColor = float3(0.38, 0.40, 0.44);
        }
        float3 color = lerp(horizonColor, midColor, smoothstep(0.02, 0.45, up));
        color = lerp(color, zenithColor, smoothstep(0.42, 1.0, up));

        float3 lightDir = sunDir;
        if (sunsetSky)
            lightDir = normalize(float3(-0.75, 0.12, 0.64));
        else if (nightSky)
            lightDir = normalize(float3(0.45, 0.34, 0.82));
        else if (dawnSky)
            lightDir = normalize(float3(0.80, 0.08, -0.58));
        else if (duskSky)
            lightDir = normalize(float3(-0.80, 0.06, 0.58));
        else if (afternoonSky)
            lightDir = normalize(float3(-0.60, 0.38, 0.70));
        sunDot = saturate(dot(worldDir, lightDir));

        float sunCore = pow(sunDot, sunsetSky || dawnSky || duskSky ? 520.0 : 950.0);
        float sunGlow = pow(sunDot, sunsetSky || dawnSky || duskSky ? 8.0 : 16.0) * (sunsetSky || dawnSky || duskSky ? 0.62 : 0.34)
                      + pow(sunDot, 4.0) * (sunsetSky || dawnSky || duskSky ? 0.20 : 0.10);
        float sunStrength = stormSky ? 0.08 : (snowSky ? 0.18 : (nightSky ? 0.0 : (overcastSky ? 0.05 : (duskSky ? 0.72 : 1.0))));
        float3 sunTint = float3(1.0, 0.74, 0.42);
        float3 sunCoreTint = float3(1.0, 0.88, 0.58);
        if (dawnSky)       { sunTint = float3(1.0, 0.58, 0.32); sunCoreTint = float3(1.0, 0.82, 0.48); }
        else if (duskSky)  { sunTint = float3(0.92, 0.38, 0.28); sunCoreTint = float3(1.0, 0.62, 0.38); }
        else if (afternoonSky) { sunTint = float3(1.0, 0.82, 0.52); sunCoreTint = float3(1.0, 0.92, 0.72); }
        color += (sunTint * sunGlow + sunCoreTint * sunCore) * sunStrength;

        if (false)
        {
            float horizonFade = smoothstep(0.02, 0.25, worldDir.y);
            float starVisibility = nightSky ? 1.0 : 0.35;

            // Layer 1: bright prominent stars
            float2 starP = float2(skyU * 920.0, skyAmount * 420.0);
            float starCell = hash21(floor(starP));
            float2 starLocal = frac(starP) - 0.5;
            float starShape = smoothstep(0.032, 0.0, length(starLocal));
            float twinkle1 = 0.78 + 0.22 * sin(time * (2.8 + starCell * 3.0) + starCell * 40.0);
            float stars = starShape * smoothstep(0.82, 1.0, starCell) * horizonFade * twinkle1;

            // Layer 2: medium stars — denser
            float2 fineStarP = float2(skyU * 1480.0 + 37.0, skyAmount * 690.0 - 19.0);
            float fineCell = hash21(floor(fineStarP));
            float2 fineLocal = frac(fineStarP) - 0.5;
            float fineShape = smoothstep(0.020, 0.0, length(fineLocal));
            float twinkle2 = 0.82 + 0.18 * sin(time * (3.4 + fineCell * 2.5) + fineCell * 60.0);
            stars += fineShape * smoothstep(0.78, 1.0, fineCell) * horizonFade * 0.62 * twinkle2;

            // Layer 3: scattered mid-field
            float2 hazeStarP = float2(skyU * 560.0 - 11.0, skyAmount * 260.0 + 53.0);
            float hazeCell = hash21(floor(hazeStarP));
            float2 hazeLocal = frac(hazeStarP) - 0.5;
            float hazeShape = smoothstep(0.022, 0.0, length(hazeLocal));
            stars += hazeShape * smoothstep(0.74, 1.0, hazeCell) * horizonFade * 0.38;

            // Layer 4: dense fine field
            float2 denseP = float2(skyU * 2400.0 + 113.0, skyAmount * 1100.0 - 47.0);
            float denseCell = hash21(floor(denseP));
            float2 denseLocal = frac(denseP) - 0.5;
            float denseShape = smoothstep(0.012, 0.0, length(denseLocal));
            stars += denseShape * smoothstep(0.76, 1.0, denseCell) * horizonFade * 0.34;

            // Layer 5: very dense fine dust
            float2 dustP = float2(skyU * 3800.0 - 201.0, skyAmount * 1800.0 + 89.0);
            float dustCell = hash21(floor(dustP));
            float2 dustLocal = frac(dustP) - 0.5;
            float dustShape = smoothstep(0.008, 0.0, length(dustLocal));
            stars += dustShape * smoothstep(0.78, 1.0, dustCell) * horizonFade * 0.26;

            // Layer 6: ultra-dense micro stars
            float2 microP = float2(skyU * 5600.0 + 317.0, skyAmount * 2600.0 - 143.0);
            float microCell = hash21(floor(microP));
            float2 microLocal = frac(microP) - 0.5;
            float microShape = smoothstep(0.005, 0.0, length(microLocal));
            stars += microShape * smoothstep(0.80, 1.0, microCell) * horizonFade * 0.18;

            // Layer 7: background star carpet
            float2 carpetP = float2(skyU * 8200.0 - 511.0, skyAmount * 3900.0 + 227.0);
            float carpetCell = hash21(floor(carpetP));
            float2 carpetLocal = frac(carpetP) - 0.5;
            float carpetShape = smoothstep(0.004, 0.0, length(carpetLocal));
            stars += carpetShape * smoothstep(0.82, 1.0, carpetCell) * horizonFade * 0.12;

            // Layer 8: secondary offset field for more random distribution
            float2 extra1P = float2(skyU * 1820.0 + 251.0, skyAmount * 840.0 + 67.0);
            float extra1Cell = hash21(floor(extra1P));
            float2 extra1Local = frac(extra1P) - 0.5;
            float extra1Shape = smoothstep(0.014, 0.0, length(extra1Local));
            stars += extra1Shape * smoothstep(0.80, 1.0, extra1Cell) * horizonFade * 0.42;

            // Layer 9: another offset for even fill
            float2 extra2P = float2(skyU * 4200.0 - 89.0, skyAmount * 2000.0 + 311.0);
            float extra2Cell = hash21(floor(extra2P));
            float2 extra2Local = frac(extra2P) - 0.5;
            float extra2Shape = smoothstep(0.007, 0.0, length(extra2Local));
            stars += extra2Shape * smoothstep(0.81, 1.0, extra2Cell) * horizonFade * 0.20;

            // Colored bright stars (warm/cool tint variation)
            float3 starColor = float3(0.75, 0.82, 1.0);
            float warmStar = smoothstep(0.94, 1.0, hash21(floor(starP) + 77.0));
            float coolStar = smoothstep(0.94, 1.0, hash21(floor(starP) + 133.0));
            starColor = lerp(starColor, float3(1.0, 0.85, 0.65), warmStar * 0.6);
            starColor = lerp(starColor, float3(0.65, 0.75, 1.0), coolStar * 0.5);

            // Milky way band — wide diffuse glow
            float milkyAngle = skyU * 3.14159 + skyAmount * 1.2 - 0.4;
            float milkyBand = exp(-pow((sin(milkyAngle) - 0.15) * 2.2, 2.0));
            float milkyNoise = valueNoise(float2(skyU * 180.0, skyAmount * 90.0)) * 0.6
                             + valueNoise(float2(skyU * 360.0 + 17.0, skyAmount * 180.0 - 31.0)) * 0.3
                             + valueNoise(float2(skyU * 720.0 - 41.0, skyAmount * 340.0 + 19.0)) * 0.15;
            float milkyGlow = milkyBand * milkyNoise * smoothstep(0.06, 0.35, worldDir.y) * 0.12;
            color += float3(0.68, 0.74, 0.92) * milkyGlow * starVisibility;

            color += starColor * stars * starVisibility;

            // Moon
            float moonDot = saturate(dot(worldDir, lightDir));
            float moon = smoothstep(0.9991, 0.9998, moonDot);
            float moonHalo = pow(moonDot, 90.0) * 0.16;
            color += (float3(0.78, 0.84, 1.0) * moonHalo + float3(0.92, 0.94, 0.88) * moon) * starVisibility;

            // Shooting stars
            float2 skyP = float2(skyU, skyAmount);
            [unroll]
            for (int meteorIndex = 0; meteorIndex < 3; ++meteorIndex)
            {
                float period = meteorIndex == 0 ? 13.0 : (meteorIndex == 1 ? 21.0 : 17.0);
                float phaseTime = time + (meteorIndex == 0 ? 1.7 : (meteorIndex == 1 ? 8.9 : 4.3));
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
                color += (float3(0.72, 0.82, 1.0) * streak * 0.80 + float3(0.95, 0.98, 1.0) * headGlow * 1.25) * starVisibility;
            }
        }

        float horizonBlend = saturate((0.12 - worldDir.y) / 0.18);
        color = lerp(color, worldFog, horizonBlend * 0.35);
        if (camera.positionYaw.y < 0.0)
        {
            const float3 waterTint = float3(0.04, 0.16, 0.38);
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
    else if (dawnSky)
    {
        zenith = float3(0.12, 0.14, 0.38);
        midSky = float3(0.52, 0.38, 0.58);
        horizonColor = float3(0.92, 0.52, 0.28);
    }
    else if (duskSky)
    {
        zenith = float3(0.06, 0.06, 0.18);
        midSky = float3(0.38, 0.18, 0.38);
        horizonColor = float3(0.82, 0.34, 0.22);
    }
    else if (afternoonSky)
    {
        zenith = float3(0.18, 0.32, 0.62);
        midSky = float3(0.48, 0.58, 0.78);
        horizonColor = float3(0.82, 0.78, 0.68);
    }
    else if (overcastSky)
    {
        zenith = float3(0.38, 0.40, 0.44);
        midSky = float3(0.48, 0.50, 0.53);
        horizonColor = float3(0.58, 0.60, 0.62);
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
        const float3 waterTint = float3(0.04, 0.16, 0.38);
        float depth = saturate((0.0 - camera.positionYaw.y) * 0.12);
        color = lerp(color * float3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
    }

    return float4(color, 1.0);
}
