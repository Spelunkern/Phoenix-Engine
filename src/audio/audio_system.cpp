#include "audio/audio_system.h"

#include "assets/data_index.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace phoenix::audio
{
    namespace
    {
        std::string key_for_path(const std::filesystem::path& path)
        {
            return phoenix::assets::lower_ascii(path.lexically_normal().string());
        }
    }

    struct AudioSystem::Impl
    {
        struct ActiveVoice
        {
            ma_sound sound{};
            bool soundValid{};
            float currentVolume{};
            float targetVolume{};
            float silentSeconds{};
            bool music{};
        };

        ma_engine engine{};
        bool initialized{};
        std::unordered_map<std::string, ActiveVoice> voices;

        ~Impl()
        {
            shutdown();
        }

        bool initialize()
        {
            if (initialized)
                return true;

            ma_engine_config config = ma_engine_config_init();
            if (ma_engine_init(&config, &engine) != MA_SUCCESS)
                return false;

            initialized = true;
            return true;
        }

        void shutdown()
        {
            for (auto& [_, active] : voices)
            {
                if (active.soundValid)
                {
                    ma_sound_stop(&active.sound);
                    ma_sound_uninit(&active.sound);
                    active.soundValid = false;
                }
            }
            voices.clear();

            if (initialized)
            {
                ma_engine_uninit(&engine);
                initialized = false;
            }
        }

        ActiveVoice* ensure_voice(const std::filesystem::path& path, bool music)
        {
            if (!initialized || path.empty())
                return nullptr;

            const auto key = key_for_path(path);
            if (const auto it = voices.find(key); it != voices.end())
            {
                it->second.music = music;
                return &it->second;
            }

            auto [it, _] = voices.emplace(key, ActiveVoice{});
            auto& active = it->second;
            active.music = music;

            const auto pathStr = path.string();
            if (ma_sound_init_from_file(&engine, pathStr.c_str(),
                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                    nullptr, nullptr, &active.sound) != MA_SUCCESS)
            {
                voices.erase(it);
                return nullptr;
            }

            active.soundValid = true;
            ma_sound_set_looping(&active.sound, MA_TRUE);
            ma_sound_set_volume(&active.sound, 0.0f);
            ma_sound_start(&active.sound);
            return &active;
        }

        void update(float deltaSeconds, const std::vector<AudibleTrack>& tracks)
        {
            if (!initialized)
                return;

            for (auto& [_, active] : voices)
                active.targetVolume = 0.0f;

            for (const auto& track : tracks)
            {
                const auto volume = std::clamp(track.volume, 0.0f, 1.0f);
                if (volume <= 0.001f)
                    continue;

                if (auto* active = ensure_voice(track.path, track.music))
                    active->targetVolume = std::max(active->targetVolume, volume);
            }

            const auto fadeRate = std::clamp(deltaSeconds * 2.6f, 0.0f, 1.0f);
            for (auto it = voices.begin(); it != voices.end();)
            {
                auto& active = it->second;
                active.currentVolume = std::lerp(active.currentVolume, active.targetVolume, fadeRate);
                if (active.soundValid)
                    ma_sound_set_volume(&active.sound, active.currentVolume);

                if (active.targetVolume <= 0.001f && active.currentVolume <= 0.003f)
                    active.silentSeconds += deltaSeconds;
                else
                    active.silentSeconds = 0.0f;

                if (active.silentSeconds > 1.2f)
                {
                    if (active.soundValid)
                    {
                        ma_sound_stop(&active.sound);
                        ma_sound_uninit(&active.sound);
                    }
                    it = voices.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    };

    AudioSystem::AudioSystem()
        : impl_(std::make_unique<Impl>())
    {
    }

    AudioSystem::~AudioSystem() = default;

    bool AudioSystem::initialize()
    {
        return impl_->initialize();
    }

    void AudioSystem::shutdown()
    {
        impl_->shutdown();
    }

    void AudioSystem::update(float deltaSeconds, const std::vector<AudibleTrack>& tracks)
    {
        impl_->update(deltaSeconds, tracks);
    }

    bool AudioSystem::available() const
    {
        return impl_ && impl_->initialized;
    }
}
