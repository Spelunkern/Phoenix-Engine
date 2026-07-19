#version 450 core

in vec2 vUv;
out vec4 outColor;

CAMERA_BLOCK

// Same subtle screen-space vignette as terrain.frag/static_object.frag/
// skinned_character.frag — kept as a shared shape so the sky's corners
// darken consistently with whatever's drawn in front of it.
vec3 applyVignette(vec3 color)
{
    vec2 screenUv = gl_FragCoord.xy * camera.screenInfo.zw;
    float vignette = 1.0 - smoothstep(0.4, 1.0, length(screenUv - 0.5) * 1.35);
    return color * mix(0.55, 1.0, vignette);
}

// Interleaved-gradient-noise dither (same as the other shaders) — the sky's
// smooth gradient bands are exactly the kind of slow ramp that shows 8-bit
// banding most, so this matters here more than anywhere else.
float ditherNoise(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

vec3 finalizeColor(vec3 color)
{
    color = applyVignette(color);
    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;
    return color;
}

void main()
{
    // Gradient bands stay screen-space static, driven only by screen
    // position, never by camera yaw/pitch — so the backdrop itself never
    // shifts as the character turns or the camera orbits/tilts. The sun/
    // moon/stars are NOT drawn here: reconstructing a world-space ray from
    // screen NDC + yaw/pitch (tried for both, at different points) reacts to
    // camera rotation inconsistently with how every other in-world object is
    // projected, and has no depth awareness (so it draws through
    // water/terrain instead of being occluded by it). They're real
    // camera-relative billboards rendered through the normal effect-particle
    // pipeline instead — see CelestialSystem and StarField — which reuses
    // the exact same, already-correct camera transform (and real depth
    // testing) as everything else in the scene.
    float up = clamp(1.0 - vUv.y, 0.0, 1.0);
    float horizon = up;

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
        // A bit more saturated/differentiated than a flat pale-to-deep-blue
        // ramp, for a livelier default daytime sky.
        vec3 horizonColor = mix(vec3(0.80, 0.86, 0.92), worldFog * 1.14, 0.35);
        vec3 midColor = vec3(0.30, 0.56, 0.90);
        vec3 zenithColor = vec3(0.06, 0.22, 0.58);
        if (stormSky) { horizonColor = vec3(0.43,0.45,0.48); midColor = vec3(0.30,0.33,0.37); zenithColor = vec3(0.18,0.20,0.24); }
        else if (snowSky) { horizonColor = vec3(0.72,0.74,0.76); midColor = vec3(0.58,0.62,0.66); zenithColor = vec3(0.46,0.50,0.55); }
        else if (sunsetSky) { horizonColor = vec3(1.00,0.42,0.20); midColor = vec3(0.78,0.30,0.36); zenithColor = vec3(0.16,0.12,0.35); }
        else if (nightSky) { horizonColor = vec3(0.035,0.045,0.085); midColor = vec3(0.018,0.028,0.070); zenithColor = vec3(0.004,0.010,0.032); }
        else if (dawnSky) { horizonColor = vec3(0.92,0.52,0.28); midColor = vec3(0.52,0.38,0.58); zenithColor = vec3(0.12,0.14,0.38); }
        else if (duskSky) { horizonColor = vec3(0.82,0.34,0.22); midColor = vec3(0.38,0.18,0.38); zenithColor = vec3(0.06,0.06,0.18); }
        else if (afternoonSky) { horizonColor = vec3(0.82,0.78,0.68); midColor = vec3(0.48,0.58,0.78); zenithColor = vec3(0.18,0.32,0.62); }
        else if (overcastSky) { horizonColor = vec3(0.58,0.60,0.62); midColor = vec3(0.48,0.50,0.53); zenithColor = vec3(0.38,0.40,0.44); }

        // Dialed in live via the (now removed) "Sky debug" panel.
        vec3 color = mix(horizonColor, midColor, smoothstep(1.0, 0.0, up));
        color = mix(color, zenithColor, smoothstep(0.2, 0.0, up));

        if (camera.positionYaw.y < 0.0)
        {
            vec3 waterTint = vec3(0.04, 0.16, 0.38);
            float depth = clamp((0.0 - camera.positionYaw.y) * 0.12, 0.0, 1.0);
            color = mix(color * vec3(0.82, 0.92, 1.02), waterTint, 0.18 + depth * 0.22);
        }
        outColor = vec4(finalizeColor(color), 1.0);
        return;
    }

    vec3 zenith = vec3(0.10, 0.26, 0.60);
    vec3 midSky = vec3(0.30, 0.56, 0.86);
    vec3 horizonColor = vec3(0.72, 0.82, 0.92);
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

    outColor = vec4(finalizeColor(color), 1.0);
}
