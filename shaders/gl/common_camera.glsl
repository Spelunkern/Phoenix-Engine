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
} camera;

// Neutral material lighting used by terrain, world assets and characters.
// It deliberately avoids the previous saturation, tint, wrap, rim and
// specular look passes: authored texture colour is lit by one soft diffuse
// key plus constant ambient illumination, close to a default engine
// material without project-specific shading.
float neutralMaterialLighting(vec3 normal)
{
    const vec3 lightDirection = vec3(-0.30, 0.68, -0.67);
    float diffuse = max(dot(normalize(normal), lightDirection), 0.0);
    return 0.45 + diffuse * 0.55;
}
