#pragma once

#include "audio/audio_system.h"
#include "runtime/phoenix_runtime.h"
#include "world/wld_loader.h"

#include <filesystem>
#include <vector>

namespace phoenix::audio
{
    struct MapAudioScene
    {
        struct MusicZone
        {
            std::filesystem::path path;
            phoenix::world::WldBoundingBox box{};
            float fadeDistance{};
        };

        struct SoundEmitter
        {
            std::filesystem::path path;
            float x{};
            float y{};
            float z{};
            float radius{};
        };

        std::vector<MusicZone> musicZones;
        std::vector<SoundEmitter> soundEmitters;
    };

    MapAudioScene build_map_audio_scene(const phoenix::runtime::PhoenixRuntime& runtime);

    void build_audible_tracks_into(
        std::vector<phoenix::audio::AudibleTrack>& tracks,
        const MapAudioScene& scene,
        float listenerX,
        float listenerY,
        float listenerZ,
        bool enableMusic,
        bool enableSounds);
}
