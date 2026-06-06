// Camera-facing billboard particles for procedural weapon effects.
// Per-particle instances live in a StructuredBuffer (binding 0); each instance
// is expanded into a screen-aligned quad in the vertex shader using the same
// camera transform as terrain.hlsl, so particles project identically to world
// geometry. The fragment shader draws a fully procedural soft circular dot
// (radial alpha falloff) modulated by the per-particle colour — no textures.
// Blend mode (alpha vs additive) is selected by the pipeline, not the shader.

struct Particle
{
    float3 worldPos;
    float size;
    float4 color;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 local : TEXCOORD0;   // [-1,1] quad coords for the radial falloff
    float4 color : COLOR0;
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
};

[[vk::push_constant]]
CameraConstants camera;

[[vk::binding(0, 0)]]
StructuredBuffer<Particle> particles : register(t0, space0);

static const float2 kCorners[6] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0,  1.0),
};

VSOutput VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Particle p = particles[iid];
    const float2 corner = kCorners[vid];

    const float3 delta = p.worldPos - camera.positionYaw.xyz;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;
    const float farPlane = camera.pitchAspectFov.w;
    const float nearPlane = 2.0f;

    const float cy = camera.precomputedTrig.x;
    const float sy = camera.precomputedTrig.y;
    const float cp = camera.precomputedTrig.z;
    const float sp = camera.precomputedTrig.w;

    float cameraX = cy * delta.x - sy * delta.z;
    const float yawZ = sy * delta.x + cy * delta.z;
    float cameraY = cp * delta.y - sp * yawZ;
    const float cameraZ = sp * delta.y + cp * yawZ;

    // Screen-aligned billboard: offset in view space so the quad always faces
    // the camera regardless of orientation. corner.y up = +cameraY.
    cameraX += corner.x * p.size;
    cameraY += corner.y * p.size;

    VSOutput output;
    output.position = float4(
        cameraX / (tanHalfFov * aspect),
        -cameraY / tanHalfFov,
        cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane),
        cameraZ);
    output.local = corner;
    output.color = p.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Soft round sprite: 1 at the centre, fading to 0 at the quad edge.
    const float dist = length(input.local);
    const float falloff = saturate(1.0f - dist);
    const float alpha = falloff * falloff;   // smooth, soft-edged dot
    return float4(input.color.rgb, input.color.a * alpha);
}
