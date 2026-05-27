#include "audio/audio_system.h"

#include "assets/data_index.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xaudio2.h>

namespace phoenix::audio
{
    namespace
    {
        struct WaveClip
        {
            WAVEFORMATEX format{};
            std::vector<std::uint8_t> data;
            bool valid{};
        };

        std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            if (offset + 4 > bytes.size())
                return 0;
            std::uint32_t value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            if (offset + 2 > bytes.size())
                return 0;
            std::uint16_t value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        bool chunk_id_equals(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* id)
        {
            return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, id, 4) == 0;
        }

        WaveClip load_wave_clip(const std::filesystem::path& path)
        {
            WaveClip clip{};
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return clip;

            std::vector<std::uint8_t> bytes(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            if (bytes.size() < 44
                || !chunk_id_equals(bytes, 0, "RIFF")
                || !chunk_id_equals(bytes, 8, "WAVE"))
            {
                return clip;
            }

            bool foundFmt = false;
            bool foundData = false;
            std::size_t dataOffset = 0;
            std::uint32_t dataSize = 0;
            std::size_t offset = 12;
            while (offset + 8 <= bytes.size())
            {
                const auto chunkSize = read_u32(bytes, offset + 4);
                const auto payload = offset + 8;
                if (payload + chunkSize > bytes.size())
                    break;

                if (chunk_id_equals(bytes, offset, "fmt ") && chunkSize >= 16)
                {
                    clip.format.wFormatTag = read_u16(bytes, payload);
                    clip.format.nChannels = read_u16(bytes, payload + 2);
                    clip.format.nSamplesPerSec = read_u32(bytes, payload + 4);
                    clip.format.nAvgBytesPerSec = read_u32(bytes, payload + 8);
                    clip.format.nBlockAlign = read_u16(bytes, payload + 12);
                    clip.format.wBitsPerSample = read_u16(bytes, payload + 14);
                    clip.format.cbSize = 0;
                    foundFmt = true;
                }
                else if (chunk_id_equals(bytes, offset, "data"))
                {
                    dataOffset = payload;
                    dataSize = chunkSize;
                    foundData = true;
                }

                offset = payload + chunkSize + (chunkSize & 1u);
            }

            if (!foundFmt || !foundData || clip.format.wFormatTag != WAVE_FORMAT_PCM
                || clip.format.nChannels == 0 || clip.format.nSamplesPerSec == 0
                || clip.format.wBitsPerSample == 0 || dataSize == 0)
            {
                return {};
            }

            clip.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + dataSize));
            clip.valid = !clip.data.empty();
            return clip;
        }

        std::string key_for_path(const std::filesystem::path& path)
        {
            return phoenix::assets::lower_ascii(path.lexically_normal().string());
        }
    }

    struct AudioSystem::Impl
    {
        struct ActiveVoice
        {
            std::shared_ptr<WaveClip> clip;
            IXAudio2SourceVoice* voice{};
            float currentVolume{};
            float targetVolume{};
            float silentSeconds{};
            bool music{};
        };

        IXAudio2* engine{};
        IXAudio2MasteringVoice* masterVoice{};
        bool initialized{};
        bool comInitialized{};
        std::unordered_map<std::string, std::shared_ptr<WaveClip>> clips;
        std::unordered_map<std::string, ActiveVoice> voices;

        ~Impl()
        {
            shutdown();
        }

        bool initialize()
        {
            if (initialized)
                return true;

            const auto coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInitialized = SUCCEEDED(coResult);
            if (FAILED(coResult) && coResult != RPC_E_CHANGED_MODE)
                return false;

            if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !engine)
            {
                shutdown();
                return false;
            }

            if (FAILED(engine->CreateMasteringVoice(&masterVoice)) || !masterVoice)
            {
                shutdown();
                return false;
            }

            initialized = true;
            return true;
        }

        void shutdown()
        {
            for (auto& [_, active] : voices)
            {
                if (active.voice)
                {
                    active.voice->Stop(0);
                    active.voice->DestroyVoice();
                    active.voice = nullptr;
                }
            }
            voices.clear();
            clips.clear();

            if (masterVoice)
            {
                masterVoice->DestroyVoice();
                masterVoice = nullptr;
            }
            if (engine)
            {
                engine->Release();
                engine = nullptr;
            }
            if (comInitialized)
            {
                CoUninitialize();
                comInitialized = false;
            }
            initialized = false;
        }

        std::shared_ptr<WaveClip> clip_for(const std::filesystem::path& path)
        {
            const auto key = key_for_path(path);
            if (const auto it = clips.find(key); it != clips.end())
                return it->second;

            auto clip = std::make_shared<WaveClip>(load_wave_clip(path));
            if (!clip->valid)
                return {};

            clips.emplace(key, clip);
            return clip;
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

            auto clip = clip_for(path);
            if (!clip)
                return nullptr;

            IXAudio2SourceVoice* voice = nullptr;
            if (FAILED(engine->CreateSourceVoice(&voice, &clip->format)) || !voice)
                return nullptr;

            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = static_cast<UINT32>(clip->data.size());
            buffer.pAudioData = clip->data.data();
            buffer.Flags = XAUDIO2_END_OF_STREAM;
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
            if (FAILED(voice->SubmitSourceBuffer(&buffer)))
            {
                voice->DestroyVoice();
                return nullptr;
            }

            voice->SetVolume(0.0f);
            voice->Start(0);

            ActiveVoice active{};
            active.clip = std::move(clip);
            active.voice = voice;
            active.music = music;
            const auto [it, _] = voices.emplace(key, std::move(active));
            return &it->second;
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
                if (active.voice)
                    active.voice->SetVolume(active.currentVolume);

                if (active.targetVolume <= 0.001f && active.currentVolume <= 0.003f)
                    active.silentSeconds += deltaSeconds;
                else
                    active.silentSeconds = 0.0f;

                if (active.silentSeconds > 1.2f)
                {
                    if (active.voice)
                    {
                        active.voice->Stop(0);
                        active.voice->DestroyVoice();
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
