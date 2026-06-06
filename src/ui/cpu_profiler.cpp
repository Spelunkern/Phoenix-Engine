#include "ui/cpu_profiler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")
#endif

namespace phoenix::ui
{
    static CpuProfiler sProfiler;

    CpuProfiler& cpu_profiler() { return sProfiler; }

    CpuProfileScope::CpuProfileScope(const char* n)
        : name(n), start(std::chrono::steady_clock::now()) {}

    CpuProfileScope::~CpuProfileScope()
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const float ms = std::chrono::duration<float, std::milli>(elapsed).count();
        cpu_profiler().record(name, ms);
    }

    void CpuProfiler::initialize()
    {
#ifdef _WIN32
        SYSTEM_INFO sysInfo{};
        GetSystemInfo(&sysInfo);
        cpuCores = std::min(static_cast<std::uint32_t>(sysInfo.dwNumberOfProcessors), kMaxCores);
        {
            HKEY key{};
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                char buf[128]{};
                DWORD size = sizeof(buf);
                if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
                    cpuName = buf;
                RegCloseKey(key);
            }
            while (!cpuName.empty() && cpuName.front() == ' ') cpuName.erase(cpuName.begin());
            while (!cpuName.empty() && cpuName.back() == ' ') cpuName.pop_back();
        }
#else
        cpuCores = std::min(static_cast<std::uint32_t>(std::thread::hardware_concurrency()), kMaxCores);
        {
            std::ifstream cpuinfo("/proc/cpuinfo");
            std::string line;
            while (std::getline(cpuinfo, line))
            {
                if (line.rfind("model name", 0) == 0)
                {
                    const auto pos = line.find(':');
                    if (pos != std::string::npos)
                    {
                        cpuName = line.substr(pos + 1);
                        while (!cpuName.empty() && cpuName.front() == ' ') cpuName.erase(cpuName.begin());
                    }
                    break;
                }
            }
        }
#endif
    }

    void CpuProfiler::begin_frame()
    {
        for (std::size_t i = 0; i < zoneCount; ++i)
            zones[i].currentMs = 0.0f;
    }

    void CpuProfiler::record(const char* name, float ms)
    {
        for (std::size_t i = 0; i < zoneCount; ++i)
        {
            if (zones[i].name == name || std::strcmp(zones[i].name, name) == 0)
            {
                zones[i].currentMs += ms;
                return;
            }
        }
        if (zoneCount < kMaxZones)
        {
            auto& z = zones[zoneCount++];
            z.name = name;
            z.currentMs = ms;
        }
    }

    void CpuProfiler::end_frame(float dt)
    {
        for (std::size_t i = 0; i < zoneCount; ++i)
            zones[i].history[historyIndex] = zones[i].currentMs;
        historyIndex = (historyIndex + 1) % kHistorySize;

        sampleAccum_ += dt;
        if (sampleAccum_ >= 0.5f)
        {
            sampleAccum_ = 0.0f;
            for (std::size_t i = 0; i < zoneCount; ++i)
            {
                auto& z = zones[i];
                z.peakMs = 0.0f;
                z.avgMs = 0.0f;
                for (std::size_t h = 0; h < kHistorySize; ++h)
                {
                    z.avgMs += z.history[h];
                    if (z.history[h] > z.peakMs) z.peakMs = z.history[h];
                }
                z.avgMs /= static_cast<float>(kHistorySize);
            }
            update_cpu_metrics();

            for (std::uint32_t i = 0; i < cpuCores; ++i)
                coreHistory[i][coreHistoryIndex] = coreUsage[i];
            cpuHistory[coreHistoryIndex] = cpuOverall;
            coreHistoryIndex = (coreHistoryIndex + 1) % kCoreHistorySize;

            update_ram_metrics();
        }
    }

    void CpuProfiler::update_ram_metrics()
    {
        float processMB = 0.0f;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            &pmc, sizeof(pmc)))
            processMB = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
#else
        {
            std::ifstream status("/proc/self/status");
            std::string line;
            while (std::getline(status, line))
            {
                if (line.rfind("VmRSS:", 0) == 0)
                {
                    long long kb = 0;
                    std::sscanf(line.c_str(), "VmRSS: %lld", &kb);
                    processMB = static_cast<float>(kb) / 1024.0f;
                    break;
                }
            }
        }
#endif
        ramCurrentMB = processMB;
        if (processMB > ramPeakMB) ramPeakMB = processMB;
        if (processMB < ramMinMB) ramMinMB = processMB;
        if (!ramBaselineSet) { ramBaselineMB = processMB; ramBaselineSet = true; }

        ramHistory[ramHistoryIndex] = processMB;
        ramHistoryIndex = (ramHistoryIndex + 1) % kRamHistorySize;

        // Growth rate: MB/s averaged over 5 seconds.
        ramGrowthTimer_ += 0.5f;
        if (ramGrowthTimer_ >= 5.0f)
        {
            if (ramPrevSampleMB_ > 0.0f)
                ramDeltaPerSec = (processMB - ramPrevSampleMB_) / ramGrowthTimer_;
            ramPrevSampleMB_ = processMB;
            ramGrowthTimer_ = 0.0f;
        }
    }

    void CpuProfiler::reset_ram_baseline()
    {
        ramBaselineMB = ramCurrentMB;
        ramPeakMB = ramCurrentMB;
        ramMinMB = ramCurrentMB;
        ramDeltaPerSec = 0.0f;
        ramPrevSampleMB_ = ramCurrentMB;
        ramGrowthTimer_ = 0.0f;
    }

    void CpuProfiler::update_cpu_metrics()
    {
#ifdef _WIN32
        struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
            LARGE_INTEGER IdleTime; LARGE_INTEGER KernelTime; LARGE_INTEGER UserTime;
            LARGE_INTEGER DpcTime; LARGE_INTEGER InterruptTime; ULONG InterruptCount;
        };
        std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> cpuInfo(cpuCores);
        ULONG returnLength = 0;
        const auto status = NtQuerySystemInformation(
            static_cast<SYSTEM_INFORMATION_CLASS>(8),
            cpuInfo.data(),
            static_cast<ULONG>(cpuInfo.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)),
            &returnLength);
        if (status == 0)
        {
            float totalUsage = 0.0f;
            for (std::uint32_t i = 0; i < cpuCores; ++i)
            {
                const auto idle = static_cast<unsigned long long>(cpuInfo[i].IdleTime.QuadPart);
                const auto kernel = static_cast<unsigned long long>(cpuInfo[i].KernelTime.QuadPart);
                const auto user = static_cast<unsigned long long>(cpuInfo[i].UserTime.QuadPart);
                if (cpuInitialized)
                {
                    const auto idleDiff = idle - lastCoreTimes[i].idle;
                    const auto totalDiff = (kernel - lastCoreTimes[i].kernel) + (user - lastCoreTimes[i].user);
                    coreUsage[i] = totalDiff > 0
                        ? (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f : 0.0f;
                }
                lastCoreTimes[i] = { idle, kernel, user };
                totalUsage += coreUsage[i];
            }
            cpuOverall = totalUsage / static_cast<float>(cpuCores);
            cpuInitialized = true;
        }
        {
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                const auto pid = GetCurrentProcessId();
                std::uint32_t count = 0;
                for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te))
                    if (te.th32OwnerProcessID == pid) ++count;
                CloseHandle(snapshot);
                threadCount = count;
            }
        }
#else
        {
            std::ifstream stat("/proc/stat");
            std::string line;
            float totalUsage = 0.0f;
            std::uint32_t counted = 0;
            while (std::getline(stat, line))
            {
                if (line.rfind("cpu", 0) != 0 || line.size() < 4
                    || !std::isdigit(static_cast<unsigned char>(line[3])))
                    continue;
                int core = 0;
                unsigned long long u = 0, n = 0, s = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
                if (std::sscanf(line.c_str(), "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                                &core, &u, &n, &s, &idle, &iowait, &irq, &softirq, &steal) < 5)
                    continue;
                if (core < 0 || static_cast<std::uint32_t>(core) >= cpuCores) continue;
                const unsigned long long idleAll = idle + iowait;
                const unsigned long long total = u + n + s + idle + iowait + irq + softirq + steal;
                auto& prev = lastCoreTimes[core];
                if (cpuInitialized)
                {
                    const auto idleDiff = idleAll - prev.idle;
                    const auto totalDiff = total - prev.total;
                    coreUsage[core] = totalDiff > 0
                        ? (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f : 0.0f;
                }
                prev.idle = idleAll;
                prev.total = total;
                totalUsage += coreUsage[core];
                ++counted;
            }
            if (counted > 0) { cpuOverall = totalUsage / static_cast<float>(counted); cpuInitialized = true; }
        }
        {
            std::uint32_t count = 0;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec))
                { (void)entry; ++count; }
            threadCount = count;
        }
#endif
    }

}
