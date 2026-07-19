#include "runtime/celestial_system.h"

#include <algorithm>
#include <cmath>

namespace phoenix::runtime
{
    namespace
    {
        using phoenix::renderer::TerrainVertex;
        using phoenix::renderer::EffectParticleInstance;
        using phoenix::renderer::EffectParticleBatch;

        // effect_particle.frag's celestial sentinels — a crisp anti-aliased
        // disc with its own interior gradient/rim baked in per body, rather
        // than a single shared pow()-falloff blob. See that shader.
        constexpr std::uint32_t kSunLayer = 0xFFFFFFFBu;
        constexpr std::uint32_t kMoonLayer = 0xFFFFFFFAu;

        // Fixed world directions — azimuth values previously tuned live in
        // sky.frag; elevation picked so both sit comfortably up in the sky
        // without being near the zenith.
        constexpr float kSunDir[3]{ -0.55f, 0.30f, 0.62f };
        constexpr float kMoonDir[3]{ 0.48f, 0.36f, -0.55f };

        // The visible disc occupies the inner ~75% of the billboard (see the
        // shader's diskRadius) — the remaining margin is headroom for the
        // anti-aliased edge and rim glow, not extra "empty" billboard space.
        constexpr float kAngularRatio = 0.16f;

        void normalize3(float v[3])
        {
            const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (lenSq < 0.000001f)
                return;
            const float inv = 1.0f / std::sqrt(lenSq);
            v[0] *= inv; v[1] *= inv; v[2] *= inv;
        }
    }

    void CelestialSystem::update(CelestialBody body, float strength, const float tint[3],
        const float cameraPosition[3],
        const float cameraRight[3], const float cameraUp[3], const float /*cameraForward*/[3],
        float farPlane)
    {
        vertices_.clear();
        indices_.clear();
        instances_.clear();
        batches_.clear();

        if (body == CelestialBody::None || strength <= 0.003f)
            return;

        const bool isMoon = body == CelestialBody::Moon;
        float direction[3]{
            isMoon ? kMoonDir[0] : kSunDir[0],
            isMoon ? kMoonDir[1] : kSunDir[1],
            isMoon ? kMoonDir[2] : kSunDir[2],
        };
        normalize3(direction);

        // Comfortably inside the active far plane (which is itself clamped
        // to >= 100 by OpenGLRenderer::set_camera) regardless of the current
        // fog/view-distance setting.
        const float distance = std::max(80.0f, farPlane) * 0.85f;
        const float position[3]{
            cameraPosition[0] + direction[0] * distance,
            cameraPosition[1] + direction[1] * distance,
            cameraPosition[2] + direction[2] * distance,
        };
        const float size = distance * kAngularRatio;

        EffectParticleInstance instance{};
        instance.color[0] = tint[0]; instance.color[1] = tint[1]; instance.color[2] = tint[2];
        instance.color[3] = strength;
        instance.position[0] = position[0]; instance.position[1] = position[1]; instance.position[2] = position[2];
        instance.right[0] = cameraRight[0] * size; instance.right[1] = cameraRight[1] * size; instance.right[2] = cameraRight[2] * size;
        instance.up[0] = cameraUp[0] * size; instance.up[1] = cameraUp[1] * size; instance.up[2] = cameraUp[2] * size;

        const float corners[4][2]{ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } };
        const float uvs[4][2]{ { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f } };
        for (int i = 0; i < 4; ++i)
        {
            TerrainVertex vertex{};
            vertex.position[0] = corners[i][0];
            vertex.position[1] = corners[i][1];
            vertex.position[2] = 0.0f;
            vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
            vertex.normal[0] = 0.0f; vertex.normal[1] = 0.0f; vertex.normal[2] = 1.0f;
            vertex.uv[0] = uvs[i][0]; vertex.uv[1] = uvs[i][1];
            vertex.textureLayer = isMoon ? kMoonLayer : kSunLayer;
            vertices_.push_back(vertex);
        }
        const std::uint32_t quadIndices[6]{ 0, 1, 2, 2, 1, 3 };
        for (const auto quadIndex : quadIndices)
            indices_.push_back(quadIndex);

        instances_.push_back(instance);

        EffectParticleBatch batch{};
        batch.firstIndex = 0;
        batch.indexCount = 6;
        batch.firstInstance = 0;
        batch.instanceCount = 1;
        // Additive (src=ONE, dst=ONE) — sun/moon brighten whatever's behind
        // them instead of a flat translucent disc.
        batch.sourceBlend = 1;
        batch.destinationBlend = 1;
        batches_.push_back(batch);
    }
}
