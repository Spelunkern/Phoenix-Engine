#include "ui/app_settings.h"

#include "app/bootstrap.h"
#include "ui/phoenix_ui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace phoenix::ui
{
    namespace
    {
        bool* g_worldShadows = nullptr;
        bool* g_vsyncEnabled = nullptr;
        int* g_fpsCapIndex = nullptr;
        bool* g_antialiasingEnabled = nullptr;

        std::filesystem::path settings_path()
        {
            return phoenix::app::executable_directory() / "phoenix.ini";
        }

        bool read_int_field(const std::string& line, const char* key, int& out)
        {
            const std::string prefix = std::string(key) + '=';
            if (line.rfind(prefix, 0) != 0)
                return false;
            try { out = std::stoi(line.substr(prefix.size())); }
            catch (...) { return false; }
            return true;
        }
    }

    void register_app_settings(
        bool& worldShadows,
        bool& vsyncEnabled,
        int& fpsCapIndex,
        bool& antialiasingEnabled)
    {
        g_worldShadows = &worldShadows;
        g_vsyncEnabled = &vsyncEnabled;
        g_fpsCapIndex = &fpsCapIndex;
        g_antialiasingEnabled = &antialiasingEnabled;

        std::ifstream input(settings_path());
        std::string line;
        float windowX = 8.0f, windowY = 8.0f;
        while (std::getline(input, line))
        {
            int value = 0;
            if (read_int_field(line, "WorldShadows", value)) worldShadows = value != 0;
            else if (read_int_field(line, "VsyncEnabled", value)) vsyncEnabled = value != 0;
            else if (read_int_field(line, "FpsCapIndex", value)) fpsCapIndex = std::clamp(value, 0, 9);
            else if (read_int_field(line, "AntialiasingEnabled", value)) antialiasingEnabled = value != 0;
            else if (read_int_field(line, "DebugWindowX", value)) windowX = static_cast<float>(value);
            else if (read_int_field(line, "DebugWindowY", value)) windowY = static_cast<float>(value);
        }
        px::set_window_position(windowX, windowY);
    }

    void flush_app_settings()
    {
        std::ofstream output(settings_path(), std::ios::trunc);
        if (!output) return;
        const auto windowPosition = px::window_position();
        output << "WorldShadows=" << ((g_worldShadows && *g_worldShadows) ? 1 : 0) << '\n'
               << "VsyncEnabled=" << ((g_vsyncEnabled && *g_vsyncEnabled) ? 1 : 0) << '\n'
               << "FpsCapIndex=" << (g_fpsCapIndex ? *g_fpsCapIndex : 0) << '\n'
               << "AntialiasingEnabled=" << ((g_antialiasingEnabled && *g_antialiasingEnabled) ? 1 : 0) << '\n'
               << "DebugWindowX=" << static_cast<int>(windowPosition.x) << '\n'
               << "DebugWindowY=" << static_cast<int>(windowPosition.y) << '\n';
    }
}
