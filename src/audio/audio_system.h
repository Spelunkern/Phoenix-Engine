#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace phoenix::audio
{
    struct AudibleTrack
    {
        std::filesystem::path path;
        float volume{};
        bool music{};
    };

    class AudioSystem
    {
    public:
        AudioSystem();
        ~AudioSystem();

        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        bool initialize();
        void shutdown();
        void update(float deltaSeconds, const std::vector<AudibleTrack>& tracks);
        bool available() const;

        // Master output volume in [0, 1] (applies to all music/sound voices).
        void set_master_volume(float volume);
        float master_volume() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
