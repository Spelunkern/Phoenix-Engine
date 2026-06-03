#include "ui/cpu_profiler.h"

#include "imgui.h"

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
        {
            auto& z = zones[i];
            z.history[historyIndex] = z.currentMs;
            z.peakMs = 0.0f;
            z.avgMs = 0.0f;
            for (std::size_t h = 0; h < kHistorySize; ++h)
            {
                z.avgMs += z.history[h];
                if (z.history[h] > z.peakMs) z.peakMs = z.history[h];
            }
            z.avgMs /= static_cast<float>(kHistorySize);
        }
        historyIndex = (historyIndex + 1) % kHistorySize;

        sampleAccum_ += dt;
        if (sampleAccum_ >= 0.5f)
        {
            sampleAccum_ = 0.0f;
            update_cpu_metrics();

            for (std::uint32_t i = 0; i < cpuCores; ++i)
                coreHistory[i][coreHistoryIndex] = coreUsage[i];
            cpuHistory[coreHistoryIndex] = cpuOverall;
            coreHistoryIndex = (coreHistoryIndex + 1) % kCoreHistorySize;
        }
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

    void draw_cpu_profiler(CpuProfiler& p)
    {
        if (!p.visible)
            return;

        ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("CPU Profiler", &p.visible))
        {
            ImGui::End();
            return;
        }

        const auto colorForPercent = [](float pct) -> ImVec4 {
            if (pct < 60.0f) return { 0.2f, 1.0f, 0.4f, 1.0f };
            if (pct < 85.0f) return { 1.0f, 0.85f, 0.2f, 1.0f };
            return { 1.0f, 0.3f, 0.3f, 1.0f };
        };
        const auto colorU32 = [](float pct) -> ImU32 {
            if (pct < 60.0f) return IM_COL32(50, 255, 100, 220);
            if (pct < 85.0f) return IM_COL32(255, 216, 50, 220);
            return IM_COL32(255, 76, 76, 220);
        };

        // ---- System info ----
        if (!p.cpuName.empty())
            ImGui::TextDisabled("%s", p.cpuName.c_str());
        ImGui::TextColored(colorForPercent(p.cpuOverall), "CPU: %.0f%%", p.cpuOverall);
        ImGui::SameLine();
        ImGui::Text("  %u cores  %u threads", p.cpuCores, p.threadCount);

        // Overall CPU history.
        ImGui::PlotLines("##cpuHist", p.cpuHistory, static_cast<int>(CpuProfiler::kCoreHistorySize),
            static_cast<int>(p.coreHistoryIndex), "CPU %", 0.0f, 100.0f,
            ImVec2(ImGui::GetContentRegionAvail().x, 40.0f));

        // ---- Per-core bars ----
        if (ImGui::CollapsingHeader("Per-Core Usage", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const auto cursor = ImGui::GetCursorScreenPos();
            const float totalWidth = ImGui::GetContentRegionAvail().x;
            const float barWidth = totalWidth / static_cast<float>(p.cpuCores);
            const float barHeight = 18.0f;
            for (std::uint32_t i = 0; i < p.cpuCores; ++i)
            {
                const float x = cursor.x + static_cast<float>(i) * barWidth;
                const float usage = std::clamp(p.coreUsage[i] / 100.0f, 0.0f, 1.0f);
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    IM_COL32(40, 40, 40, 200));
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y + barHeight * (1.0f - usage)),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    colorU32(p.coreUsage[i]));
            }
            ImGui::Dummy(ImVec2(totalWidth, barHeight + 4.0f));

            // Per-core sparklines.
            const float graphWidth = totalWidth - 70.0f;
            for (std::uint32_t i = 0; i < p.cpuCores; ++i)
            {
                ImGui::TextColored(colorForPercent(p.coreUsage[i]), "C%02u", i);
                ImGui::SameLine();
                char label[16];
                std::snprintf(label, sizeof(label), "##c%u", i);
                ImGui::PlotLines(label, p.coreHistory[i],
                    static_cast<int>(CpuProfiler::kCoreHistorySize),
                    static_cast<int>(p.coreHistoryIndex),
                    nullptr, 0.0f, 100.0f, ImVec2(graphWidth, 14.0f));
                ImGui::SameLine();
                ImGui::Text("%3.0f%%", p.coreUsage[i]);
            }
        }

        // ---- Frame zones ----
        if (p.zoneCount > 0 && ImGui::CollapsingHeader("Frame Zones", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Sort zones by avg time (heaviest first) for display.
            std::size_t order[CpuProfiler::kMaxZones];
            for (std::size_t i = 0; i < p.zoneCount; ++i) order[i] = i;
            std::sort(order, order + p.zoneCount, [&](std::size_t a, std::size_t b) {
                return p.zones[a].avgMs > p.zones[b].avgMs;
            });

            ImGui::Columns(4, "zones", false);
            ImGui::SetColumnWidth(0, 140.0f);
            ImGui::SetColumnWidth(1, 70.0f);
            ImGui::SetColumnWidth(2, 70.0f);
            ImGui::SetColumnWidth(3, 70.0f);
            ImGui::TextDisabled("Zone"); ImGui::NextColumn();
            ImGui::TextDisabled("Now"); ImGui::NextColumn();
            ImGui::TextDisabled("Avg"); ImGui::NextColumn();
            ImGui::TextDisabled("Peak"); ImGui::NextColumn();

            for (std::size_t oi = 0; oi < p.zoneCount; ++oi)
            {
                const auto& z = p.zones[order[oi]];
                const auto zoneColor = z.currentMs > 4.0f ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                    : z.currentMs > 1.0f ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                    : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                ImGui::TextColored(zoneColor, "%s", z.name); ImGui::NextColumn();
                ImGui::Text("%.2f", z.currentMs); ImGui::NextColumn();
                ImGui::Text("%.2f", z.avgMs); ImGui::NextColumn();
                ImGui::Text("%.2f", z.peakMs); ImGui::NextColumn();
            }
            ImGui::Columns(1);

            // Total.
            float totalNow = 0.0f;
            for (std::size_t i = 0; i < p.zoneCount; ++i) totalNow += p.zones[i].currentMs;
            ImGui::Separator();
            ImGui::Text("Total frame CPU: %.2f ms", totalNow);

            // Stacked zone graph (last zone).
            if (p.zoneCount > 0)
            {
                const auto& heaviest = p.zones[order[0]];
                char heaviestLabel[64];
                std::snprintf(heaviestLabel, sizeof(heaviestLabel), "%s (ms)", heaviest.name);
                ImGui::PlotLines("##heaviest", heaviest.history,
                    static_cast<int>(CpuProfiler::kHistorySize),
                    static_cast<int>(p.historyIndex),
                    heaviestLabel, 0.0f, std::max(8.0f, heaviest.peakMs * 1.2f),
                    ImVec2(ImGui::GetContentRegionAvail().x, 50.0f));
            }
        }

        ImGui::End();
    }
}
