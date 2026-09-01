#include "ui/perf_hud.h"

#include "ui/phoenix_ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

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
    namespace
    {
        void trim_ascii(std::string& value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
        }

        std::string strip_quotes(std::string value)
        {
            trim_ascii(value);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            return value;
        }

    }

    float PerfHudState::fps_cap_seconds() const
    {
        constexpr float caps[] = {
            0.0f, 1.0f/30.0f, 1.0f/60.0f, 1.0f/75.0f, 1.0f/90.0f,
            1.0f/120.0f, 1.0f/144.0f, 1.0f/165.0f, 1.0f/240.0f, 1.0f/360.0f };
        constexpr int kCapCount = static_cast<int>(sizeof(caps) / sizeof(caps[0]));
        return (fpsCapIndex >= 0 && fpsCapIndex < kCapCount) ? caps[fpsCapIndex] : 0.0f;
    }

    void PerfHudState::initialize_system_info()
    {
#ifdef _WIN32
        osName = "Windows";
        {
            HKEY key{};
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                char build[32]{};
                DWORD size = sizeof(build);
                if (RegQueryValueExA(key, "CurrentBuildNumber", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(build), &size) == ERROR_SUCCESS)
                {
                    const int buildNumber = std::atoi(build);
                    if (buildNumber >= 22000) osName = "Windows 11";
                    else if (buildNumber >= 10240) osName = "Windows 10";
                }
                RegCloseKey(key);
            }
        }
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
            trim_ascii(cpuName);
        }
#else
        osName = "Linux";
        {
            std::ifstream osRelease("/etc/os-release");
            std::string line;
            while (std::getline(osRelease, line))
                if (line.rfind("PRETTY_NAME=", 0) == 0) { osName = strip_quotes(line.substr(12)); break; }
        }
        cpuCores = std::min(static_cast<std::uint32_t>(std::thread::hardware_concurrency()), kMaxCores);
        {
            std::ifstream cpuinfo("/proc/cpuinfo");
            std::string line;
            while (std::getline(cpuinfo, line))
                if (line.rfind("model name", 0) == 0)
                {
                    const auto pos = line.find(':');
                    if (pos != std::string::npos) cpuName = line.substr(pos + 1);
                    trim_ascii(cpuName);
                    break;
                }
        }
#endif
    }

    void PerfHudState::push_frametime(float dt)
    {
        accumTime_ += dt;
        accumFrames_++;
        if (accumTime_ < 1.0f) return;

        fpsSmoothed = static_cast<float>(accumFrames_) / accumTime_;
        accumTime_ = 0.0f;
        accumFrames_ = 0;

        if (renderer)
        {
            vramTotalMB = static_cast<float>(renderer->vram_total_bytes()) / (1024.0f * 1024.0f);
            vramUsedMB = static_cast<float>(renderer->vram_used_bytes()) / (1024.0f * 1024.0f);
        }

#ifdef _WIN32
        {
            MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem);
            if (GlobalMemoryStatusEx(&mem))
            {
                ramTotalMB = static_cast<float>(mem.ullTotalPhys) / (1024.0f * 1024.0f);
                ramUsedMB = static_cast<float>(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0f * 1024.0f);
                ramPercent = static_cast<float>(mem.dwMemoryLoad);
            }
            PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                processRamMB = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);

            struct SPPI { LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime; ULONG InterruptCount; };
            SPPI cpuInfo[kMaxCores];
            ULONG ret = 0;
            if (NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(8),
                cpuInfo, static_cast<ULONG>(cpuCores * sizeof(SPPI)), &ret) == 0)
            {
                float total = 0.0f;
                for (std::uint32_t i = 0; i < cpuCores; ++i)
                {
                    const auto idle = static_cast<unsigned long long>(cpuInfo[i].IdleTime.QuadPart);
                    const auto kernel = static_cast<unsigned long long>(cpuInfo[i].KernelTime.QuadPart);
                    const auto user = static_cast<unsigned long long>(cpuInfo[i].UserTime.QuadPart);
                    if (cpuInitialized_)
                    {
                        const auto idleDiff = idle - lastCoreTimes_[i].idle;
                        const auto totalDiff = (kernel - lastCoreTimes_[i].kernel) + (user - lastCoreTimes_[i].user);
                        total += totalDiff > 0 ? (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f : 0.0f;
                    }
                    lastCoreTimes_[i] = { idle, kernel, user };
                }
                if (cpuInitialized_) cpuPercent = total / static_cast<float>(cpuCores);
                cpuInitialized_ = true;
            }
        }
#else
        {
            // Line-based parse: token streaming desyncs on /proc/meminfo lines
            // without a unit (HugePages_*), and kernels/containers without
            // MemAvailable need the MemFree+Buffers+Cached fallback — both
            // made the HUD show bogus values (e.g. RAM permanently full).
            std::ifstream meminfo("/proc/meminfo");
            std::string line;
            long long totalKb = 0, availKb = -1, freeKb = 0, buffersKb = 0, cachedKb = 0;
            while (std::getline(meminfo, line))
            {
                long long kb = 0;
                if (std::sscanf(line.c_str(), "MemTotal: %lld", &kb) == 1) totalKb = kb;
                else if (std::sscanf(line.c_str(), "MemAvailable: %lld", &kb) == 1) availKb = kb;
                else if (std::sscanf(line.c_str(), "MemFree: %lld", &kb) == 1) freeKb = kb;
                else if (std::sscanf(line.c_str(), "Buffers: %lld", &kb) == 1) buffersKb = kb;
                else if (std::sscanf(line.c_str(), "Cached: %lld", &kb) == 1) cachedKb = kb;
            }
            if (totalKb > 0)
            {
                const long long effectiveAvailKb = availKb >= 0
                    ? availKb
                    : (freeKb + buffersKb + cachedKb);
                const auto clampedAvail = std::clamp<long long>(effectiveAvailKb, 0, totalKb);
                ramTotalMB = static_cast<float>(totalKb) / 1024.0f;
                ramUsedMB = static_cast<float>(totalKb - clampedAvail) / 1024.0f;
                ramPercent = (1.0f - static_cast<float>(clampedAvail) / static_cast<float>(totalKb)) * 100.0f;
            }
        }
        {
            std::ifstream st("/proc/self/status"); std::string line;
            while (std::getline(st, line))
                if (line.rfind("VmRSS:", 0) == 0) { long long kb = 0; std::sscanf(line.c_str(), "VmRSS: %lld", &kb); processRamMB = static_cast<float>(kb) / 1024.0f; break; }
        }
        {
            std::ifstream stat("/proc/stat"); std::string line;
            float total = 0.0f; std::uint32_t counted = 0;
            while (std::getline(stat, line))
            {
                if (line.rfind("cpu", 0) != 0 || line.size() < 4 || !std::isdigit(static_cast<unsigned char>(line[3]))) continue;
                int core = 0; unsigned long long u=0,n=0,s=0,idle=0,iow=0,irq=0,sirq=0,steal=0;
                if (std::sscanf(line.c_str(), "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &core, &u, &n, &s, &idle, &iow, &irq, &sirq, &steal) < 5) continue;
                if (core < 0 || static_cast<std::uint32_t>(core) >= cpuCores) continue;
                const auto idleAll = idle + iow;
                const auto tot = u + n + s + idle + iow + irq + sirq + steal;
                if (cpuInitialized_)
                {
                    const auto idleDiff = idleAll - lastCoreTimes_[core].idle;
                    const auto totalDiff = tot - lastCoreTimes_[core].total;
                    total += totalDiff > 0 ? (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f : 0.0f;
                }
                lastCoreTimes_[core] = { idleAll, tot };
                ++counted;
            }
            if (counted > 0 && cpuInitialized_) cpuPercent = total / static_cast<float>(counted);
            cpuInitialized_ = true;
        }
#endif
    }

    void draw_perf_hud(PerfHudState& hud, float surfaceWidth)
    {
        const float hudWidth = 240.0f;
        px::SetNextWindowPos(px::Vec2(surfaceWidth - hudWidth - 8.0f, 8.0f), px::Always);
        px::SetNextWindowSize(px::Vec2(hudWidth, 0.0f));
        px::SetNextWindowBgAlpha(0.78f);

        const auto flags = px::NoTitleBar | px::NoResize
            | px::NoScrollbar | px::NoFocusOnAppearing | px::NoMove;

        if (!px::Begin("##PerfHud", nullptr, flags)) { px::End(); return; }

        const auto pctColor = [](float pct) -> px::Color {
            if (pct < 60.0f) return { 0.2f, 1.0f, 0.4f, 1.0f };
            if (pct < 85.0f) return { 1.0f, 0.85f, 0.2f, 1.0f };
            return { 1.0f, 0.3f, 0.3f, 1.0f };
        };

        if (!hud.osName.empty()) px::TextDisabled("%s", hud.osName.c_str());
        if (!hud.cpuName.empty()) px::TextDisabled("%s", hud.cpuName.c_str());
        if (!hud.gpuName.empty()) px::TextDisabled("%s", hud.gpuName.c_str());
        px::Separator();

        const auto fpsColor = hud.fpsSmoothed >= 60.0f ? px::Color(0.2f,1.0f,0.4f,1.0f)
            : hud.fpsSmoothed >= 30.0f ? px::Color(1.0f,0.85f,0.2f,1.0f) : px::Color(1.0f,0.3f,0.3f,1.0f);
        px::TextColored(fpsColor, "FPS: %.0f", static_cast<double>(hud.fpsSmoothed));

        px::TextColored(pctColor(hud.cpuPercent), "CPU: %.0f%%", static_cast<double>(hud.cpuPercent));
        px::SameLine(130.0f);
        px::Text("%u cores", hud.cpuCores);

        if (hud.vramTotalMB > 0.0f)
        {
            const float vp = (hud.vramUsedMB / hud.vramTotalMB) * 100.0f;
            px::TextColored(pctColor(vp), "VRAM: %.0f%%", static_cast<double>(vp));
            px::SameLine(130.0f);
            px::Text("%.0f/%.0f MB", static_cast<double>(hud.vramUsedMB),
                static_cast<double>(hud.vramTotalMB));
        }

        px::TextColored(pctColor(hud.ramPercent), "RAM: %.0f%%", static_cast<double>(hud.ramPercent));
        px::SameLine(130.0f);
        px::Text("%.1f/%.1f GB", static_cast<double>(hud.ramUsedMB / 1024.0f),
            static_cast<double>(hud.ramTotalMB / 1024.0f));
        px::Text("Process: %.0f MB", static_cast<double>(hud.processRamMB));

        px::Separator();
        px::TextDisabled("Map: %s", hud.mapId.empty() ? "?" : hud.mapId.c_str());
        px::Text("XYZ: %.1f  %.1f  %.1f", static_cast<double>(hud.worldX),
            static_cast<double>(hud.worldY), static_cast<double>(hud.worldZ));

        px::End();
    }
}
