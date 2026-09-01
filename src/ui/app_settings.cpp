#include "ui/app_settings.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace phoenix::ui
{
    namespace
    {
        bool* g_worldShadows = nullptr;
        int* g_fpsCapIndex = nullptr;
        bool* g_antialiasingEnabled = nullptr;

        void* read_open(ImGuiContext*, ImGuiSettingsHandler*, const char*)
        {
            return (void*)1; // single fixed entry; no per-entry storage needed
        }

        // "Key=Value" line parsing (avoids sscanf's MSVC deprecation nag).
        bool read_int_field(const char* line, const char* key, int& out)
        {
            const auto keyLen = std::strlen(key);
            if (std::strncmp(line, key, keyLen) != 0 || line[keyLen] != '=')
                return false;
            out = std::atoi(line + keyLen + 1);
            return true;
        }

        void read_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
        {
            int value = 0;
            if (g_worldShadows && (read_int_field(line, "WorldShadows", value)
                || read_int_field(line, "CharacterShadow", value)))
                *g_worldShadows = value != 0;
            else if (g_fpsCapIndex && read_int_field(line, "FpsCapIndex", value))
                *g_fpsCapIndex = std::clamp(value, 0, 9);
            else if (g_antialiasingEnabled && read_int_field(line, "AntialiasingEnabled", value))
                *g_antialiasingEnabled = value != 0;
        }

        void write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
        {
            buf->appendf("[%s][Config]\n", handler->TypeName);
            buf->appendf("WorldShadows=%d\n", (g_worldShadows && *g_worldShadows) ? 1 : 0);
            buf->appendf("FpsCapIndex=%d\n", g_fpsCapIndex ? *g_fpsCapIndex : 0);
            buf->appendf("AntialiasingEnabled=%d\n", (g_antialiasingEnabled && *g_antialiasingEnabled) ? 1 : 0);
            buf->append("\n");
        }
    }

    void register_app_settings(bool& worldShadows, int& fpsCapIndex, bool& antialiasingEnabled)
    {
        g_worldShadows = &worldShadows;
        g_fpsCapIndex = &fpsCapIndex;
        g_antialiasingEnabled = &antialiasingEnabled;

        ImGuiSettingsHandler handler{};
        handler.TypeName = "PhoenixSettings";
        handler.TypeHash = ImHashStr(handler.TypeName);
        handler.ReadOpenFn = read_open;
        handler.ReadLineFn = read_line;
        handler.WriteAllFn = write_all;
        ImGui::AddSettingsHandler(&handler);

        // Load immediately (rather than waiting for the first ImGui::NewFrame())
        // so these values are correct before the loading sequence uses them.
        ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
    }

    void flush_app_settings()
    {
        if (!ImGui::GetCurrentContext())
            return;
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    }
}
