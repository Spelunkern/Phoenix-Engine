#include "audio/map_audio_scene.h"

#include <algorithm>
#include <cmath>

namespace phoenix::audio
{
    namespace
    {
        constexpr float kSoundAudibleRadiusScale = 1.6f;
        constexpr float kSoundAudibleRadiusBonus = 16.0f;

        float distance_to_box(float x, float y, float z, const phoenix::world::WldBoundingBox& box)
        {
            const float minX = std::min(box.min[0], box.max[0]);
            const float minY = std::min(box.min[1], box.max[1]);
            const float minZ = std::min(box.min[2], box.max[2]);
            const float maxX = std::max(box.min[0], box.max[0]);
            const float maxY = std::max(box.min[1], box.max[1]);
            const float maxZ = std::max(box.min[2], box.max[2]);
            const float dx = x < minX ? minX - x : (x > maxX ? x - maxX : 0.0f);
            const float dy = y < minY ? minY - y : (y > maxY ? y - maxY : 0.0f);
            const float dz = z < minZ ? minZ - z : (z > maxZ ? z - maxZ : 0.0f);
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        float smooth_falloff(float normalizedDistance)
        {
            const auto t = std::clamp(normalizedDistance, 0.0f, 1.0f);
            return 1.0f - (t * t * (3.0f - 2.0f * t));
        }
    }

    MapAudioScene build_map_audio_scene(const phoenix::runtime::PhoenixRuntime& runtime)
    {
        MapAudioScene scene;
        const auto& world = runtime.state().world;
        if (!world.parsed)
            return scene;

        const auto mapSize = static_cast<float>(std::max(1u, world.mapSize));
        const auto halfMap = world.isDungeon ? 0.0f : mapSize * 0.5f;
        const auto worldX = [&](float rawX) { return rawX - halfMap; };
        const auto worldZ = [&](float rawZ) { return rawZ - halfMap; };

        scene.musicZones.reserve(world.musicZones.size());
        for (const auto& zone : world.musicZones)
        {
            if (zone.musicAssetId < 0 || static_cast<std::size_t>(zone.musicAssetId) >= world.musicAssets.size())
                continue;

            auto path = runtime.audio_path_for(world.musicAssets[static_cast<std::size_t>(zone.musicAssetId)]);
            if (path.empty())
                continue;

            MapAudioScene::MusicZone mapped{};
            mapped.path = std::move(path);
            mapped.box = zone.box;
            mapped.box.min[0] = worldX(mapped.box.min[0]);
            mapped.box.max[0] = worldX(mapped.box.max[0]);
            mapped.box.min[2] = worldZ(mapped.box.min[2]);
            mapped.box.max[2] = worldZ(mapped.box.max[2]);
            mapped.fadeDistance = std::max(48.0f, zone.radius);
            scene.musicZones.push_back(std::move(mapped));
        }

        scene.soundEmitters.reserve(world.soundEffects.size());
        for (const auto& sound : world.soundEffects)
        {
            if (sound.soundEffectAssetId < 0
                || static_cast<std::size_t>(sound.soundEffectAssetId) >= world.soundEffectAssets.size()
                || sound.radius <= 0.0f)
            {
                continue;
            }

            auto path = runtime.audio_path_for(world.soundEffectAssets[static_cast<std::size_t>(sound.soundEffectAssetId)]);
            if (path.empty())
                continue;

            MapAudioScene::SoundEmitter emitter{};
            emitter.path = std::move(path);
            emitter.x = worldX(sound.center[0]);
            emitter.y = sound.center[1];
            emitter.z = worldZ(sound.center[2]);
            emitter.radius = std::max(48.0f, sound.radius * kSoundAudibleRadiusScale + kSoundAudibleRadiusBonus);
            scene.soundEmitters.push_back(std::move(emitter));
        }

        return scene;
    }

    void build_audible_tracks_into(
        std::vector<phoenix::audio::AudibleTrack>& tracks,
        const MapAudioScene& scene,
        float listenerX,
        float listenerY,
        float listenerZ,
        bool enableMusic,
        bool enableSounds)
    {
        if (enableMusic)
        {
            for (const auto& zone : scene.musicZones)
            {
                const auto distance = distance_to_box(listenerX, listenerY, listenerZ, zone.box);
                if (distance > zone.fadeDistance)
                    continue;

                phoenix::audio::AudibleTrack track{};
                track.path = zone.path;
                track.volume = 0.72f * smooth_falloff(distance / zone.fadeDistance);
                track.music = true;
                tracks.push_back(std::move(track));
            }
        }

        if (enableSounds)
        {
            struct Candidate
            {
                phoenix::audio::AudibleTrack track;
                float volume{};
            };

            static std::vector<Candidate> candidates;
            candidates.clear();
            for (const auto& emitter : scene.soundEmitters)
            {
                const auto dx = listenerX - emitter.x;
                const auto dy = listenerY - emitter.y;
                const auto dz = listenerZ - emitter.z;
                const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > emitter.radius)
                    continue;

                const auto volume = 0.86f * smooth_falloff(distance / emitter.radius);
                phoenix::audio::AudibleTrack track{};
                track.path = emitter.path;
                track.volume = volume;
                track.music = false;
                candidates.push_back({ std::move(track), volume });
            }

            std::ranges::sort(candidates, [](const auto& lhs, const auto& rhs) {
                return lhs.volume > rhs.volume;
            });
            constexpr std::size_t kMaxAmbientVoices = 24;
            for (std::size_t i = 0; i < std::min(kMaxAmbientVoices, candidates.size()); ++i)
                tracks.push_back(std::move(candidates[i].track));
        }
    }
}
