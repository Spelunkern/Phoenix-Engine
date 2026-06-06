#include "ui/perf_hud.h"

#include "ui/perf_hud_icons.h"

#include "imgui.h"

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

        bool contains_ci(const std::string& haystack, const char* needle)
        {
            const auto len = std::strlen(needle);
            if (haystack.size() < len) return false;
            for (std::size_t i = 0; i <= haystack.size() - len; ++i)
            {
                bool match = true;
                for (std::size_t j = 0; j < len; ++j)
                {
                    if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                        static_cast<unsigned char>(needle[j]))
                    { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        }

        void upload_icons_if_needed(PerfHudState& hud)
        {
            if (hud.iconsUploaded || !hud.renderer) return;
            hud.windowsIcon = hud.renderer->upload_imgui_icon_rgba(kIconWindowsRgba, kIconWindowsWidth, kIconWindowsHeight);
            hud.linuxIcon = hud.renderer->upload_imgui_icon_rgba(kIconLinuxRgba, kIconLinuxWidth, kIconLinuxHeight);
            hud.macIcon = hud.renderer->upload_imgui_icon_rgba(kIconMacRgba, kIconMacWidth, kIconMacHeight);
            hud.nvidiaIcon = hud.renderer->upload_imgui_icon_rgba(kIconNvidiaRgba, kIconNvidiaWidth, kIconNvidiaHeight);
            hud.amdIcon = hud.renderer->upload_imgui_icon_rgba(kIconAmdRgba, kIconAmdWidth, kIconAmdHeight);
            hud.intelIcon = hud.renderer->upload_imgui_icon_rgba(kIconIntelRgba, kIconIntelWidth, kIconIntelHeight);
            hud.iconsUploaded = true;

            // Cache icon lookups once so draw never does string ops.
            const auto& os = hud.osName;
            if (contains_ci(os, "windows")) hud.cachedOsIcon = hud.windowsIcon;
            else if (contains_ci(os, "linux") || contains_ci(os, "ubuntu") || contains_ci(os, "debian")
                || contains_ci(os, "fedora") || contains_ci(os, "arch")) hud.cachedOsIcon = hud.linuxIcon;
            else if (contains_ci(os, "mac") || contains_ci(os, "darwin")) hud.cachedOsIcon = hud.macIcon;

            auto resolve_vendor = [&](const std::string& name) -> std::uint64_t {
                if (contains_ci(name, "nvidia") || contains_ci(name, "geforce") || contains_ci(name, "rtx") || contains_ci(name, "gtx"))
                    return hud.nvidiaIcon;
                if (contains_ci(name, "amd") || contains_ci(name, "radeon") || contains_ci(name, "ryzen"))
                    return hud.amdIcon;
                if (contains_ci(name, "intel") || contains_ci(name, "arc"))
                    return hud.intelIcon;
                return 0;
            };
            hud.cachedCpuIcon = resolve_vendor(hud.cpuName);
            hud.cachedGpuIcon = resolve_vendor(hud.gpuName);
        }

        void icon_text(std::uint64_t icon, const char* text, bool disabled = true)
        {
            if (icon != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(icon), ImVec2(14.0f, 14.0f));
                ImGui::SameLine(0.0f, 5.0f);
            }
            if (disabled) ImGui::TextDisabled("%s", text);
            else ImGui::TextUnformatted(text);
        }

        constexpr const char* kSettingsFile = "perf_hud.ini";
    }

    float PerfHudState::fps_cap_seconds() const
    {
        constexpr float caps[] = { 0.0f, 1.0f/30.0f, 1.0f/60.0f, 1.0f/120.0f, 1.0f/144.0f };
        return (fpsCapIndex >= 0 && fpsCapIndex < 5) ? caps[fpsCapIndex] : 0.0f;
    }

    void PerfHudState::load_settings(const std::filesystem::path& executableDir)
    {
        std::ifstream f(executableDir / kSettingsFile);
        if (f) f >> fpsCapIndex;
        fpsCapIndex = std::clamp(fpsCapIndex, 0, 4);
    }

    void PerfHudState::save_settings(const std::filesystem::path& executableDir) const
    {
        std::ofstream f(executableDir / kSettingsFile);
        if (f) f << fpsCapIndex;
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
            std::ifstream meminfo("/proc/meminfo");
            std::string key, unit; long long val = 0, totalKb = 0, availKb = 0;
            while (meminfo >> key >> val >> unit)
            {
                if (key == "MemTotal:") totalKb = val;
                else if (key == "MemAvailable:") availKb = val;
                if (totalKb && availKb) break;
            }
            if (totalKb > 0)
            {
                ramTotalMB = static_cast<float>(totalKb) / 1024.0f;
                ramUsedMB = static_cast<float>(totalKb - availKb) / 1024.0f;
                ramPercent = (1.0f - static_cast<float>(availKb) / static_cast<float>(totalKb)) * 100.0f;
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
        ImGui::SetNextWindowPos(ImVec2(surfaceWidth - hudWidth - 8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(hudWidth, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.78f);

        const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

        if (!ImGui::Begin("##PerfHud", nullptr, flags)) { ImGui::End(); return; }
        upload_icons_if_needed(hud);

        const auto pctColor = [](float pct) -> ImVec4 {
            if (pct < 60.0f) return { 0.2f, 1.0f, 0.4f, 1.0f };
            if (pct < 85.0f) return { 1.0f, 0.85f, 0.2f, 1.0f };
            return { 1.0f, 0.3f, 0.3f, 1.0f };
        };

        if (!hud.osName.empty()) icon_text(hud.cachedOsIcon, hud.osName.c_str());
        if (!hud.cpuName.empty()) icon_text(hud.cachedCpuIcon, hud.cpuName.c_str());
        if (!hud.gpuName.empty()) icon_text(hud.cachedGpuIcon, hud.gpuName.c_str());
        ImGui::Separator();

        const auto fpsColor = hud.fpsSmoothed >= 60.0f ? ImVec4(0.2f,1.0f,0.4f,1.0f)
            : hud.fpsSmoothed >= 30.0f ? ImVec4(1.0f,0.85f,0.2f,1.0f) : ImVec4(1.0f,0.3f,0.3f,1.0f);
        ImGui::TextColored(fpsColor, "FPS: %.0f", hud.fpsSmoothed);

        ImGui::TextColored(pctColor(hud.cpuPercent), "CPU: %.0f%%", hud.cpuPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%u cores", hud.cpuCores);

        if (hud.vramTotalMB > 0.0f)
        {
            const float vp = (hud.vramUsedMB / hud.vramTotalMB) * 100.0f;
            ImGui::TextColored(pctColor(vp), "VRAM: %.0f%%", vp);
            ImGui::SameLine(130.0f);
            ImGui::Text("%.0f/%.0f MB", hud.vramUsedMB, hud.vramTotalMB);
        }

        ImGui::TextColored(pctColor(hud.ramPercent), "RAM: %.0f%%", hud.ramPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%.1f/%.1f GB", hud.ramUsedMB / 1024.0f, hud.ramTotalMB / 1024.0f);
        ImGui::Text("Process: %.0f MB", hud.processRamMB);

        ImGui::Separator();
        ImGui::TextDisabled("Map: %s", hud.mapId.empty() ? "?" : hud.mapId.c_str());
        ImGui::Text("XYZ: %.1f  %.1f  %.1f", hud.worldX, hud.worldY, hud.worldZ);

        ImGui::Separator();
        {
            const char* caps[] = { "Off", "30", "60", "120", "144" };
            const int prev = hud.fpsCapIndex;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("FPS Cap", &hud.fpsCapIndex, caps, 5) && hud.fpsCapIndex != prev && hud.renderer)
                hud.save_settings(std::filesystem::current_path());
        }

        ImGui::End();
    }
}
