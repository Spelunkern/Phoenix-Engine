// Shared camera/environment uniform block. Included via textual concat by
// the C++ shader loader (no #include support needed -- glShaderSource takes
// multiple strings).
layout(std140, binding = 0) uniform CameraConstants
{
    vec4 positionYaw;
    vec4 pitchAspectFov;
    vec4 precomputedTrig;
    vec4 fogColorHasSky;
    vec4 fogDistances;
    vec4 skyLayers;
    vec4 waterInfo;
    vec4 skyHorizonCurve;
    vec4 skyZenithMidWeight;
    vec4 skyMidHeight;
    vec4 skyGroundCloudCover;
    vec4 cloudColorOpacity;
    vec4 cloudShadeSpeed;
    vec4 cloudShape;
    vec4 cloudMotion;
    vec4 waterShallowAlpha;
    vec4 waterDeepAlpha;
    vec4 waterSurface;
    vec4 waterOptics;
    vec4 lightDirectionEnergy;
    vec4 astroDirectionGlow;
    vec4 lightColorShadow;
    vec4 ambientColorEnergy;
    vec4 glowColorFocus;
    vec4 diskColorSize;
    vec4 skyOptics;
    vec4 starColorDensity;
    vec4 auroraLowSpread;
    vec4 auroraHighSpeed;
    vec4 groundWeather;
    vec4 snowColor;
    mat4 shadowMatrix;
    vec4 shadowInfo; // enabled, inverse resolution, base bias, reserved
} camera;

layout(binding = 5) uniform sampler2D directionalShadowMap;

float environmentShadowVisibility(vec3 worldPosition, vec3 normal)
{
    if (camera.shadowInfo.x < 0.5)
        return 1.0;
    vec4 lightClip = camera.shadowMatrix * vec4(worldPosition, 1.0);
    vec3 projected = lightClip.xyz / max(lightClip.w, 0.0001);
    vec2 uv = projected.xy * 0.5 + 0.5;
    float receiverDepth = projected.z * 0.5 + 0.5;
    if (receiverDepth <= 0.0 || receiverDepth >= 1.0
        || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float slope = 1.0 - max(dot(normalize(normal),
        normalize(camera.lightDirectionEnergy.xyz)), 0.0);
    float bias = camera.shadowInfo.z * (1.0 + slope * 2.5);
    float litSamples = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float storedDepth = texture(directionalShadowMap,
                uv + vec2(x, y) * camera.shadowInfo.y).r;
            litSamples += receiverDepth - bias <= storedDepth ? 1.0 : 0.0;
        }
    }
    float filtered = litSamples / 9.0;
    return mix(1.0, filtered, clamp(camera.lightColorShadow.w, 0.0, 1.0));
}

// The Godot project treats every sky preset as a complete environment: its
// directional light and fixed ambient light change together with the visible
// sky. Keep that contract here instead of applying one unrelated grey light
// to every weather style.
vec3 environmentMaterialLighting(vec3 normal, vec3 worldPosition)
{
    float diffuse = max(dot(normalize(normal), normalize(camera.lightDirectionEnergy.xyz)), 0.0);
    vec3 ambient = camera.ambientColorEnergy.rgb * camera.ambientColorEnergy.w;
    float shadow = environmentShadowVisibility(worldPosition, normal);
    vec3 direct = camera.lightColorShadow.rgb * camera.lightDirectionEnergy.w * diffuse * shadow;
    return ambient + direct;
}
