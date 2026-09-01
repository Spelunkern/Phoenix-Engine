#version 450 core

in vec2 vUv;
out vec4 outColor;

CAMERA_BLOCK

ENVIRONMENT_FUNCTIONS

float ditherNoise(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

vec3 worldEyeDirection(vec2 uv)
{
    vec2 ndc = uv * 2.0 - 1.0;
    float tanHalfFov = camera.pitchAspectFov.z;
    vec3 cameraDirection = normalize(vec3(
        ndc.x * tanHalfFov * camera.pitchAspectFov.y,
        ndc.y * tanHalfFov,
        1.0));

    float cy = camera.precomputedTrig.x;
    float sy = camera.precomputedTrig.y;
    float cp = camera.precomputedTrig.z;
    float sp = camera.precomputedTrig.w;
    // Godot's camera looks along -Z locally. In this engine the forward view
    // depth is represented as +Z, therefore screen-right is the negated X
    // basis when looking toward world +Z.
    vec3 right = vec3(-cy, 0.0, sy);
    vec3 up = vec3(-sp * sy, cp, -sp * cy);
    vec3 forward = vec3(cp * sy, sp, cp * cy);
    return normalize(right * cameraDirection.x + up * cameraDirection.y
        + forward * cameraDirection.z);
}

void main()
{
    vec3 color = environmentSkyRadiance(worldEyeDirection(vUv));
    if (camera.positionYaw.y < 0.0)
    {
        float depth = clamp(-camera.positionYaw.y * 0.12, 0.0, 1.0);
        color = mix(color * vec3(0.82, 0.92, 1.02), camera.waterDeepAlpha.rgb,
            0.18 + depth * 0.22);
    }
    color += (ditherNoise(gl_FragCoord.xy) - 0.5) / 255.0;
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
