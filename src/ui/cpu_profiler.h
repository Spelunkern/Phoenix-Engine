#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phoenix::ui
{
    // Lightweight scoped CPU timer. Place at the top of a block to measure it.
    // Records into the CpuProfiler singleton.
    struct CpuProfileScope
    {
        const char* name;
        std::chrono::steady_clock::time_point start;
        CpuProfileScope(const char* n);
        ~CpuProfileScope();
    };

    // Macro for convenience.
    #define CPU_PROFILE_SCOPE(name) phoenix::ui::CpuProfileScope _cpuProfileScope##__LINE__(name)

    struct CpuProfiler
    {
        static constexpr std::size_t kMaxZones = 32;
        static constexpr std::size_t kHistorySize = 120;

        struct Zone
        {
            const char* name{};
            float currentMs{};
            float history[kHistorySize]{};
            float peakMs{};
            float avgMs{};
        };

        Zone zones[kMaxZones]{};
        std::size_t zoneCount{};
        std::size_t historyIndex{};
        bool visible{};

        // Per-core data (same as PerfHudState but independent).
        static constexpr std::uint32_t kMaxCores = 64;
        static constexpr std::size_t kCoreHistorySize = 120;
        std::uint32_t cpuCores{};
        float coreUsage[kMaxCores]{};
        float coreHistory[kMaxCores][kCoreHistorySize]{};
        float cpuOverall{};
        float cpuHistory[kCoreHistorySize]{};
        std::size_t coreHistoryIndex{};
        std::string cpuName;
        std::uint32_t threadCount{};

#ifdef _WIN32
        struct CoreTimes { unsigned long long idle{}; unsigned long long kernel{}; unsigned long long user{}; };
#else
        struct CoreTimes { unsigned long long idle{}; unsigned long long total{}; };
#endif
        CoreTimes lastCoreTimes[kMaxCores]{};
        bool cpuInitialized{};

        float sampleAccum_{};

        void initialize();
        void begin_frame();
        void record(const char* name, float ms);
        void end_frame(float dt);
        void update_cpu_metrics();
    };

    CpuProfiler& cpu_profiler();
    void draw_cpu_profiler(CpuProfiler& profiler);
}
