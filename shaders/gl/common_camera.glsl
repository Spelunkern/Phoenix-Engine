// Shared camera uniform block layout, matches the 60-float Vulkan push
// constant range exactly (std140, 15 vec4s). Included via textual concat by
// the C++ shader loader (no #include support needed — glShaderSource takes
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
    vec4 skyTuning0;
    vec4 skyTuning1;
    vec4 skyTuning2;
    vec4 waterStyle;
    vec4 tuning0; // charTuning0 / assetTuning0
    vec4 tuning1; // charTuning1 / assetTuning1
    vec4 tuning2; // charTuning2 / assetTuning2
    vec4 tuning3; // charTuning3 / assetTuning3
} camera;
