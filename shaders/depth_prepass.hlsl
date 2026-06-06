// Depth-only prepass — writes depth buffer without color output.
// Terrain and character geometry use VSMain_Terrain (no instance transform).
// Static objects use VSMain_StaticObject (with instance transform).

struct CameraConstants
{
    float4 positionYaw;
    float4 pitchAspectFov;
    float4 precomputedTrig; // cosYaw, sinYaw, cosPitch, sinPitch
    float4 fogColorHasSky;
    float4 fogDistances;
};

[[vk::push_constant]]
CameraConstants camera;

// ---- Terrain / character depth prepass ----

struct TerrainVSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint textureLayer : TEXCOORD1;
};

struct DepthVSOutput
{
    float4 position : SV_POSITION;
};

DepthVSOutput VSMain_Terrain(TerrainVSInput input)
{
    const float3 delta = input.position - camera.positionYaw.xyz;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;
    const float farPlane = camera.pitchAspectFov.w;
    const float nearPlane = 2.0f;

    const float cy = camera.precomputedTrig.x; const float sy = camera.precomputedTrig.y;
    const float cp = camera.precomputedTrig.z; const float sp = camera.precomputedTrig.w;

    const float cameraX = cy * delta.x - sy * delta.z;
    const float yawZ = sy * delta.x + cy * delta.z;
    const float cameraY = cp * delta.y - sp * yawZ;
    const float cameraZ = sp * delta.y + cp * yawZ;

    DepthVSOutput output;
    output.position = float4(
        cameraX / (tanHalfFov * aspect),
        -cameraY / tanHalfFov,
        cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane),
        cameraZ);
    return output;
}

// ---- Static object depth prepass (instanced) ----

struct StaticObjectVSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint textureLayer : TEXCOORD1;
    float4 instanceRight : TEXCOORD2;
    float4 instanceUp : TEXCOORD3;
    float4 instanceForward : TEXCOORD4;
    float4 instancePosition : TEXCOORD5;
};

DepthVSOutput VSMain_StaticObject(StaticObjectVSInput input)
{
    const float3 worldPosition =
        input.instancePosition.xyz
        + input.instanceRight.xyz * input.position.x
        + input.instanceUp.xyz * input.position.y
        + input.instanceForward.xyz * input.position.z;

    const float3 delta = worldPosition - camera.positionYaw.xyz;
    const float aspect = camera.pitchAspectFov.y;
    const float tanHalfFov = camera.pitchAspectFov.z;
    const float farPlane = camera.pitchAspectFov.w;
    const float nearPlane = 2.0f;

    const float cy = camera.precomputedTrig.x; const float sy = camera.precomputedTrig.y;
    const float cp = camera.precomputedTrig.z; const float sp = camera.precomputedTrig.w;

    const float cameraX = cy * delta.x - sy * delta.z;
    const float yawZ = sy * delta.x + cy * delta.z;
    const float cameraY = cp * delta.y - sp * yawZ;
    const float cameraZ = sp * delta.y + cp * yawZ;

    DepthVSOutput output;
    output.position = float4(
        cameraX / (tanHalfFov * aspect),
        -cameraY / tanHalfFov,
        cameraZ * farPlane / (farPlane - nearPlane) - nearPlane * farPlane / (farPlane - nearPlane),
        cameraZ);
    return output;
}

void PSMain() {}
