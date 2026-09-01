// Shared by sky.frag and terrain.frag. Alpha contains seamless FBM noise for
// clouds; rgb contains the normal derived from that same periodic height map.
layout(binding = 4) uniform sampler2D environmentNormalNoise;

float environmentAuroraWave(float azimuth, float timeValue)
{
    float wave = sin(azimuth * 3.0 + timeValue * 0.7) * 0.50;
    wave += sin(azimuth * 7.0 - timeValue * 1.1) * 0.28;
    wave += sin(azimuth * 13.0 + timeValue * 1.7) * 0.14;
    wave += sin(azimuth * 23.0 - timeValue * 2.3) * 0.08;
    return wave;
}

vec3 environmentAuroraCurtain(float azimuth, float up, float timeValue,
    float phase, float height, float spread, float rayFrequency)
{
    float wave = environmentAuroraWave(azimuth + phase, timeValue + phase);
    float center = height + wave * 0.11;
    float offset = (up - center) / max(spread, 0.001);
    float band = offset < 0.0
        ? exp(-offset * offset * 5.0)
        : exp(-offset * offset * 0.8);
    float rays = sin(azimuth * rayFrequency + wave * 4.0 + timeValue * 0.9) * 0.62
        + sin(azimuth * rayFrequency * 2.37 - timeValue * 1.3 + phase) * 0.38;
    rays = max(rays, 0.0);
    rays *= rays;
    vec3 tint = mix(camera.auroraLowSpread.rgb, camera.auroraHighSpeed.rgb,
        clamp(offset * 0.5 + 0.5, 0.0, 1.0));
    return tint * band * (0.58 + 0.42 * rays);
}

vec3 environmentAuroraBand(float azimuth, float up, float timeValue,
    float phase, float direction, float extent, float height, float gain)
{
    float delta = azimuth - direction;
    delta = atan(sin(delta), cos(delta));
    float arc = smoothstep(extent, extent * 0.25, abs(delta));
    arc *= 0.30 + 0.70 * (0.5 + 0.5 * sin(delta * 2.3 + timeValue * 0.35 + phase));
    float spread = camera.auroraLowSpread.w;
    vec3 curtain = environmentAuroraCurtain(azimuth, up, timeValue, phase,
        height, spread, 9.0);
    curtain += environmentAuroraCurtain(azimuth, up, timeValue, phase + 2.1,
        height + 0.07, spread * 0.7, 14.4) * 0.45;
    return curtain * arc * gain;
}

vec3 environmentSkyRadiance(vec3 eyeDirection)
{
    vec3 direction = normalize(eyeDirection);
    float up = clamp(direction.y, -1.0, 1.0);
    float vertical = 1.0 - acos(abs(up)) / (3.14159265359 * 0.5);
    float blendAmount = clamp(1.0 - pow(1.0 - vertical,
        1.0 / max(camera.skyHorizonCurve.w, 0.001)), 0.0, 1.0);

    vec3 color;
    if (up >= 0.0)
    {
        color = mix(camera.skyHorizonCurve.rgb, camera.skyZenithMidWeight.rgb, blendAmount);
        float bandDistance = (up - camera.skyMidHeight.w) / 0.28;
        float band = clamp(camera.skyZenithMidWeight.w * exp(-bandDistance * bandDistance), 0.0, 1.0);
        color = mix(color, camera.skyMidHeight.rgb, band);
    }
    else
    {
        color = mix(camera.skyHorizonCurve.rgb, camera.skyGroundCloudCover.rgb, blendAmount);
    }

    vec3 astroDirection = normalize(camera.astroDirectionGlow.xyz);
    float towardAstro = max(dot(direction, astroDirection), 0.0);
    if (camera.astroDirectionGlow.w > 0.0)
    {
        color += camera.glowColorFocus.rgb * camera.astroDirectionGlow.w
            * pow(towardAstro, max(camera.glowColorFocus.w, 0.001));
    }

    if (camera.diskColorSize.w > 0.0)
    {
        float angle = acos(clamp(dot(direction, astroDirection), -1.0, 1.0));
        float radius = radians(camera.diskColorSize.w);
        float disc = 1.0 - smoothstep(radius * 0.75, radius, angle);
        color += camera.diskColorSize.rgb * camera.skyOptics.x * disc;
    }

    if (camera.skyOptics.y > 0.0 && up > 0.0)
    {
        vec3 sphereCell = direction * camera.starColorDensity.w;
        vec3 cell = floor(sphereCell);
        vec3 offset = fract(sphereCell);
        float hashValue = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        if (hashValue > 1.0 - camera.skyOptics.z)
        {
            vec3 seed = fract(sin(cell * 91.37 + 13.7) * 4321.77);
            float distanceToStar = length(offset - seed);
            float spark = pow(clamp(1.0 - distanceToStar * 3.4, 0.0, 1.0), 6.0);
            float twinkle = 0.72 + 0.28 * sin(camera.waterInfo.z * 1.7 + hashValue * 60.0);
            color += camera.starColorDensity.rgb * spark * twinkle * camera.skyOptics.y
                * smoothstep(0.0, 0.22, up);
        }
    }

    float cloudCover = camera.skyGroundCloudCover.w;
    if (cloudCover > 0.0 && up > 0.0)
    {
        float curvature = camera.cloudShape.w;
        float curvedUp = curvature * up;
        float distanceToLayer = sqrt(curvedUp * curvedUp + 2.0 * curvature + 1.0) - curvedUp;
        vec2 plane = direction.xz * distanceToLayer * camera.cloudShape.x;
        vec2 uv = plane + camera.cloudMotion.xy * camera.cloudShadeSpeed.w
            * camera.waterInfo.z * 0.026;

        float firstLayer = texture(environmentNormalNoise, uv).a;
        float mirroredLayer = texture(environmentNormalNoise,
            vec2(uv.y, uv.x) * 0.87 + 0.31).a;
        float noiseValue = mix(firstLayer, mirroredLayer,
            0.5 + 0.5 * sin(camera.waterInfo.z * camera.cloudShape.z));
        noiseValue = clamp((noiseValue - 0.5) * 2.2 + 0.5, 0.0, 1.0);

        float edge = 1.0 - cloudCover;
        float density = smoothstep(edge, edge + max(camera.cloudShape.y, 0.001), noiseValue);
        density *= smoothstep(0.0, max(camera.cloudMotion.z, 0.001), up);
        vec3 cloudTint = mix(camera.cloudShadeSpeed.rgb, camera.cloudColorOpacity.rgb, density);
        color = mix(color, cloudTint, clamp(density * camera.cloudColorOpacity.w, 0.0, 1.0));
    }

    if (camera.skyOptics.w > 0.0 && up > 0.0)
    {
        float timeValue = camera.waterInfo.z * camera.auroraHighSpeed.w;
        float azimuth = atan(direction.x, direction.z);
        vec3 aurora = environmentAuroraBand(azimuth, up, timeValue, 0.0,
            0.0, 0.9, 0.30, 1.0);
        aurora += environmentAuroraBand(azimuth, up, timeValue, 1.7,
            2.1, 0.7, 0.38, 0.6);
        aurora += environmentAuroraBand(azimuth, up, timeValue, 3.9,
            -1.9, 0.6, 0.24, 0.4);
        color += aurora * camera.skyOptics.w;
    }

    return color;
}
