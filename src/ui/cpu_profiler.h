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

        // RAM monitoring.
        static constexpr std::size_t kRamHistorySize = 120;
        float ramHistory[kRamHistorySize]{};
        std::size_t ramHistoryIndex{};
        float ramCurrentMB{};
        float ramBaselineMB{};   // value at last reset
        float ramDeltaPerSec{};  // growth rate MB/s (smoothed)
        float ramPeakMB{};
        float ramMinMB{ 99999.0f };
        bool ramBaselineSet{};
        float ramGrowthAccum_{};
        float ramGrowthTimer_{};
        float ramPrevSampleMB_{};

        void initialize();
        void begin_frame();
        void record(const char* name, float ms);
        void end_frame(float dt);
        void update_cpu_metrics();
        void update_ram_metrics();
        void reset_ram_baseline();
    };

    CpuProfiler& cpu_profiler();
}
