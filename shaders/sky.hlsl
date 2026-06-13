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
