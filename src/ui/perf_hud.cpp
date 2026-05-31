#include "ui/perf_hud.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")
#endif

namespace phoenix::ui
{
    void PerfHudState::initialize_system_info()
    {
#ifdef _WIN32
        SYSTEM_INFO sysInfo{};
        GetSystemInfo(&sysInfo);
        cpuCores = std::min(static_cast<std::uint32_t>(sysInfo.dwNumberOfProcessors), kMaxCores);
#else
        cpuCores = std::min(static_cast<std::uint32_t>(std::thread::hardware_concurrency()), kMaxCores);
#endif
    }

    void PerfHudState::update_system_metrics()
    {
#ifdef _WIN32
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            ramTotalMB = static_cast<float>(memStatus.ullTotalPhys) / (1024.0f * 1024.0f);
            const auto usedBytes = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
            ramUsedMB = static_cast<float>(usedBytes) / (1024.0f * 1024.0f);
            ramPercent = static_cast<float>(memStatus.dwMemoryLoad);
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
            processRamMB = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);

        struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
            LARGE_INTEGER IdleTime;
            LARGE_INTEGER KernelTime;
            LARGE_INTEGER UserTime;
            LARGE_INTEGER DpcTime;
            LARGE_INTEGER InterruptTime;
            ULONG InterruptCount;
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
                    if (totalDiff > 0)
                        coreUsage[i] = (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f;
                    else
                        coreUsage[i] = 0.0f;
                }
                lastCoreTimes[i] = { idle, kernel, user };
                totalUsage += coreUsage[i];
            }
            cpuPercent = totalUsage / static_cast<float>(cpuCores);
            cpuInitialized = true;
        }
#else
        // ---- Linux: read metrics from /proc ----
        // RAM totals from /proc/meminfo (values are in kB).
        {
            std::ifstream meminfo("/proc/meminfo");
            std::string key, unit;
            long long valueKb = 0, totalKb = 0, availKb = 0;
            while (meminfo >> key >> valueKb >> unit)
            {
                if (key == "MemTotal:") totalKb = valueKb;
                else if (key == "MemAvailable:") availKb = valueKb;
                if (totalKb && availKb) break;
            }
            if (totalKb > 0)
            {
                ramTotalMB = static_cast<float>(totalKb) / 1024.0f;
                ramUsedMB = static_cast<float>(totalKb - availKb) / 1024.0f;
                ramPercent = (1.0f - static_cast<float>(availKb) / static_cast<float>(totalKb)) * 100.0f;
            }
        }

        // Process resident set from /proc/self/status (VmRSS, in kB).
        {
            std::ifstream status("/proc/self/status");
            std::string line;
            while (std::getline(status, line))
            {
                if (line.rfind("VmRSS:", 0) == 0)
                {
                    long long kb = 0;
                    std::sscanf(line.c_str(), "VmRSS: %lld", &kb);
                    processRamMB = static_cast<float>(kb) / 1024.0f;
                    break;
                }
            }
        }

        // Per-core CPU usage from /proc/stat (delta of busy vs idle jiffies).
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
                if (core < 0 || static_cast<std::uint32_t>(core) >= cpuCores)
                    continue;
                const unsigned long long idleAll = idle + iowait;
                const unsigned long long total = u + n + s + idle + iowait + irq + softirq + steal;
                auto& prev = lastCoreTimes[core];
                if (cpuInitialized)
                {
                    const auto idleDiff = idleAll - prev.idle;
                    const auto totalDiff = total - prev.total;
                    coreUsage[core] = totalDiff > 0
                        ? (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f
                        : 0.0f;
                }
                prev.idle = idleAll;
                prev.total = total;
                totalUsage += coreUsage[core];
                ++counted;
            }
            if (counted > 0)
            {
                cpuPercent = totalUsage / static_cast<float>(counted);
                cpuInitialized = true;
            }
        }
#endif

        // GPU VRAM.
        if (renderer)
        {
            vramTotalMB = static_cast<float>(renderer->vram_total_bytes()) / (1024.0f * 1024.0f);
            vramUsedMB = static_cast<float>(renderer->vram_used_bytes()) / (1024.0f * 1024.0f);
        }
    }

    void PerfHudState::push_frametime(float dt)
    {
        const float ms = dt * 1000.0f;
        frametimeHistory[historyIndex] = ms;
        historyIndex = (historyIndex + 1) % kHistorySize;

        // Accumulate frames and refresh the displayed values every ~0.5s so the
        // HUD text is readable instead of flickering every frame.
        displayAccum_ += dt;
        displayFrames_++;
        if (displayAccum_ >= 0.5f)
        {
            fpsSmoothed = static_cast<float>(displayFrames_) / displayAccum_;
            frametimeMs = (displayAccum_ / static_cast<float>(displayFrames_)) * 1000.0f;
            float sum = 0.0f, minV = 999.0f, maxV = 0.0f;
            for (auto v : frametimeHistory)
            {
                if (v <= 0.0f) continue;
                sum += v;
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
            }
            frametimeAvg = sum / static_cast<float>(kHistorySize);
            frametimeMin = minV;
            frametimeMax = maxV;
            displayAccum_ = 0.0f;
            displayFrames_ = 0;
            update_system_metrics();
        }
    }

    void draw_perf_hud(PerfHudState& hud, float surfaceWidth)
    {
        if (!hud.visible)
            return;

        const float hudWidth = 240.0f;
        ImGui::SetNextWindowPos(ImVec2(surfaceWidth - hudWidth - 8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(hudWidth, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.78f);

        const auto flags = ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoMove;

        if (!ImGui::Begin("##PerfHud", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        const auto colorForPercent = [](float pct) -> ImVec4 {
            if (pct < 60.0f) return { 0.2f, 1.0f, 0.4f, 1.0f };
            if (pct < 85.0f) return { 1.0f, 0.85f, 0.2f, 1.0f };
            return { 1.0f, 0.3f, 0.3f, 1.0f };
        };

        // FPS + frametime.
        const auto fpsColor = hud.fpsSmoothed >= 60.0f ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
            : hud.fpsSmoothed >= 30.0f ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
            : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(fpsColor, "FPS: %.0f", hud.fpsSmoothed);
        ImGui::SameLine(130.0f);
        ImGui::Text("%.2f ms", hud.frametimeMs);

        // Frametime graph.
        ImGui::PlotLines("##ft", hud.frametimeHistory, static_cast<int>(PerfHudState::kHistorySize),
            static_cast<int>(hud.historyIndex), nullptr, 0.0f, std::max(16.7f, hud.frametimeMax * 1.2f),
            ImVec2(hudWidth - 16.0f, 36.0f));
        ImGui::Text("%.1f / %.1f / %.1f ms", hud.frametimeMin, hud.frametimeAvg, hud.frametimeMax);

        ImGui::Separator();

        // CPU overall.
        ImGui::TextColored(colorForPercent(hud.cpuPercent), "CPU: %.0f%%", hud.cpuPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%u cores", hud.cpuCores);

        // Per-core usage bars (compact: one row of colored blocks).
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const auto cursor = ImGui::GetCursorScreenPos();
            const float barWidth = (hudWidth - 20.0f) / static_cast<float>(hud.cpuCores);
            const float barHeight = 10.0f;
            for (std::uint32_t i = 0; i < hud.cpuCores; ++i)
            {
                const float x = cursor.x + static_cast<float>(i) * barWidth;
                const float usage = std::clamp(hud.coreUsage[i] / 100.0f, 0.0f, 1.0f);
                // Background.
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    IM_COL32(40, 40, 40, 200));
                // Fill based on usage.
                const auto r = static_cast<std::uint8_t>(std::min(255.0f, usage * 2.0f * 255.0f));
                const auto g = static_cast<std::uint8_t>(std::min(255.0f, (1.0f - usage) * 2.0f * 255.0f));
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y + barHeight * (1.0f - usage)),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    IM_COL32(r, g, 60, 220));
            }
            ImGui::Dummy(ImVec2(hudWidth - 16.0f, barHeight + 2.0f));
        }

        ImGui::Separator();

        // GPU.
        if (!hud.gpuName.empty())
            ImGui::TextDisabled("%s", hud.gpuName.c_str());
        if (hud.vramTotalMB > 0.0f)
        {
            const float vramPercent = (hud.vramUsedMB / hud.vramTotalMB) * 100.0f;
            ImGui::TextColored(colorForPercent(vramPercent), "VRAM: %.0f%%", vramPercent);
            ImGui::SameLine(130.0f);
            ImGui::Text("%.0f/%.0f MB", hud.vramUsedMB, hud.vramTotalMB);
        }

        ImGui::Separator();

        // RAM.
        ImGui::TextColored(colorForPercent(hud.ramPercent), "RAM: %.0f%%", hud.ramPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%.1f/%.1f GB", hud.ramUsedMB / 1024.0f, hud.ramTotalMB / 1024.0f);
        ImGui::Text("Process: %.0f MB", hud.processRamMB);

        ImGui::Separator();

        // Scene stats.
        ImGui::Text("Batches: %u  Inst: %u", hud.visibleBatches, hud.visibleInstances);
        ImGui::Text("Tris: %uk  Actors: %u", hud.totalTriangles / 1000u, hud.actorCount);
        if (hud.mobsMoving > 0)
            ImGui::Text("Mobs moving: %u", hud.mobsMoving);

        ImGui::End();
    }
}
