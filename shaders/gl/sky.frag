#version 450 core

in vec2 vUv;
out vec4 outColor;

CAMERA_BLOCK

void main()
{
    vec2 ndc = vUv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float yaw = camera.positionYaw.w;
    float pitch = camera.pitchAspectFov.x;
    float aspect = camera.pitchAspectFov.y;
    float tanHalfFov = camera.pitchAspectFov.z;

    vec3 cameraDir = normalize(vec3(ndc.x * tanHalfFov * aspect, ndc.y * tanHalfFov, 1.0));
    float cy = cos(yaw);
    float sy = sin(yaw);
    float cp = cos(pitch);
    float sp = sin(pitch);

    float yawZ = -sp * cameraDir.y + cp * cameraDir.z;
    vec3 worldDir = normalize(vec3(
        cy * cameraDir.x + sy * yawZ,
        cp * cameraDir.y + sp * cameraDir.z,
        -sy * cameraDir.x + cy * yawZ));

    float skyAmount = clamp(worldDir.y, 0.0, 1.0);
    float horizon = clamp(worldDir.y * 3.0 + 0.5, 0.0, 1.0);
    float weatherStyle = round(camera.skyTuning0.w);
    bool stormSky = weatherStyle > 0.5 && weatherStyle < 1.5;
    bool snowSky = weatherStyle >= 1.5 && weatherStyle < 2.5;
    bool sunsetSky = weatherStyle >= 2.5 && weatherStyle < 3.5;
    bool nightSky = weatherStyle >= 3.5 && weatherStyle < 4.5;
    bool dawnSky = weatherStyle >= 4.5 && weatherStyle < 5.5;
    bool duskSky = weatherStyle >= 5.5 && weatherStyle < 6.5;
    bool afternoonSky = weatherStyle >= 6.5 && weatherStyle < 7.5;
    bool overcastSky = weatherStyle >= 7.5;

    vec3 worldFog = clamp(camera.fogColorHasSky.rgb, 0.0, 1.0);
    float hasWorldSky = camera.fogColorHasSky.a;
    if (hasWorldSky > 0.5)
    {
        vec3 sunDir = normalize(vec3(-0.45, 0.85, 0.77));
        float sunDot = clamp(dot(worldDir, sunDir), 0.0, 1.0);
        float up = clamp(worldDir.y, 0.0, 1.0);

        vec3 horizonColor = mix(vec3(0.74, 0.83, 0.94), worldFog * 1.14, 0.35);
        vec3 midColor = vec3(0.38, 0.57, 0.86);
        vec3 zenithColor = vec3(0.10, 0.23, 0.52);
        if (stormSky) { horizonColor = vec3(0.43,0.45,0.48); midColor = vec3(0.30,0.33,0.37); zenithColor = vec3(0.18,0.20,0.24); }
        else if (snowSky) { horizonColor = vec3(0.72,0.74,0.76); midColor = vec3(0.58,0.62,0.66); zenithColor = vec3(0.46,0.50,0.55); }
        else if (sunsetSky) { horizonColor = vec3(1.00,0.42,0.20); midColor = vec3(0.78,0.30,0.36); zenithColor = vec3(0.16,0.12,0.35); }
        else if (nightSky) { horizonColor = vec3(0.035,0.045,0.085); midColor = vec3(0.018,0.028,0.070); zenithColor = vec3(0.004,0.010,0.032); }
        else if (dawnSky) { horizonColor = vec3(0.92,0.52,0.28); midColor = vec3(0.52,0.38,0.58); zenithColor = vec3(0.12,0.14,0.38); }
        else if (duskSky) { horizonColor = vec3(0.82,0.34,0.22); midColor = vec3(0.38,0.18,0.38); zenithColor = vec3(0.06,0.06,0.18); }
        else if (afternoonSky) { horizonColor = vec3(0.82,0.78,0.68); midColor = vec3(0.48,0.58,0.78); zenithColor = vec3(0.18,0.32,0.62); }
        else if (overcastSky) { horizonColor = vec3(0.58,0.60,0.62); midColor = vec3(0.48,0.50,0.53); zenithColor = vec3(0.38,0.40,0.44); }

        vec3 color = mix(horizonColor, midColor, smoothstep(0.02, 0.45, up));
        color = mix(color, zenithColor, smoothstep(0.42, 1.0, up));

        vec3 lightDir = sunDir;
        if (sunsetSky) lightDir = normalize(vec3(-0.75, 0.12, 0.64));
        else if (nightSky) lightDir = normalize(vec3(0.45, 0.75, 0.82));
        else if (dawnSky) lightDir = normalize(vec3(0.80, 0.08, -0.58));
        else if (duskSky) lightDir = normalize(vec3(-0.80, 0.06, 0.58));
        else if (afternoonSky) lightDir = normalize(vec3(-0.60, 0.75, 0.70));
        sunDot = clamp(dot(worldDir, lightDir), 0.0, 1.0);

        bool warmSun = sunsetSky || dawnSky || duskSky;
        float sunCore = pow(sunDot, warmSun ? 520.0 : 950.0);
        float sunGlow = pow(sunDot, warmSun ? 8.0 : 16.0) * (warmSun ? 0.62 : 0.34)
                      + pow(sunDot, 4.0) * (warmSun ? 0.20 : 0.10);
        float sunStrength = stormSky ? 0.08 : (snowSky ? 0.18 : (nightSky ? 0.0 : (overcastSky ? 0.05 : (duskSky ? 0.72 : 1.0))));
        vec3 sunTint = vec3(1.0, 0.74, 0.42);
        vec3 sunCoreTint = vec3(1.0, 0.88, 0.58);
        if (dawnSky) { sunTint = vec3(1.0, 0.58, 0.32); sunCoreTint = vec3(1.0, 0.82, 0.48); }
        else if (duskSky) { sunTint = vec3(0.92, 0.38, 0.28); sunCoreTint = vec3(1.0, 0.62, 0.38); }
        else if (afternoonSky) { sunTint = vec3(1.0, 0.82, 0.52); sunCoreTint = vec3(1.0, 0.92, 0.72); }
        color += (sunTint * sunGlow + sunCoreTint * sunCore) * sunStrength;

        float horizonBlend = clamp((0.12 - worldDir.y) / 0.18, 0.0, 1.0);
        color = mix(color, worldFog, horizonBlend * 0.35);
        if (camera.positionYaw.y < 0.0)
        {
            vec3 waterTint = vec3(0.04, 0.16, 0.38);
            float depth = clamp((0.0 - camera.positionYaw.y) * 0.12, 0.0, 1.0);
            color = mix(color * vec3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
        }
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 zenith = vec3(0.16, 0.28, 0.58);
    vec3 midSky = vec3(0.38, 0.55, 0.80);
    vec3 horizonColor = vec3(0.68, 0.80, 0.92);
    if (stormSky) { zenith = vec3(0.18,0.20,0.24); midSky = vec3(0.30,0.33,0.37); horizonColor = vec3(0.43,0.45,0.48); }
    else if (snowSky) { zenith = vec3(0.46,0.50,0.55); midSky = vec3(0.58,0.62,0.66); horizonColor = vec3(0.72,0.74,0.76); }
    else if (sunsetSky) { zenith = vec3(0.16,0.12,0.35); midSky = vec3(0.78,0.30,0.36); horizonColor = vec3(1.00,0.42,0.20); }
    else if (nightSky) { zenith = vec3(0.004,0.010,0.032); midSky = vec3(0.018,0.028,0.070); horizonColor = vec3(0.035,0.045,0.085); }
    else if (dawnSky) { zenith = vec3(0.12,0.14,0.38); midSky = vec3(0.52,0.38,0.58); horizonColor = vec3(0.92,0.52,0.28); }
    else if (duskSky) { zenith = vec3(0.06,0.06,0.18); midSky = vec3(0.38,0.18,0.38); horizonColor = vec3(0.82,0.34,0.22); }
    else if (afternoonSky) { zenith = vec3(0.18,0.32,0.62); midSky = vec3(0.48,0.58,0.78); horizonColor = vec3(0.82,0.78,0.68); }
    else if (overcastSky) { zenith = vec3(0.38,0.40,0.44); midSky = vec3(0.48,0.50,0.53); horizonColor = vec3(0.58,0.60,0.62); }
    zenith = mix(zenith, worldFog * 0.72, hasWorldSky);
    midSky = mix(midSky, worldFog * 1.08, hasWorldSky);
    horizonColor = mix(horizonColor, worldFog * 1.22, hasWorldSky);
    vec3 ground = mix(vec3(0.25, 0.28, 0.22), worldFog * 0.55, hasWorldSky);

    vec3 color;
    if (horizon > 0.72)
        color = mix(midSky, zenith, clamp((horizon - 0.72) / 0.28, 0.0, 1.0));
    else if (horizon > 0.50)
        color = mix(horizonColor, midSky, clamp((horizon - 0.50) / 0.22, 0.0, 1.0));
    else if (horizon > 0.34)
        color = mix(ground, horizonColor, clamp((horizon - 0.34) / 0.16, 0.0, 1.0));
    else
        color = ground;

    if (camera.positionYaw.y < 0.0)
    {
        vec3 waterTint = vec3(0.04, 0.16, 0.38);
        float depth = clamp((0.0 - camera.positionYaw.y) * 0.12, 0.0, 1.0);
        color = mix(color * vec3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
    }

    outColor = vec4(color, 1.0);
}
