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
        bool* g_characterShadow = nullptr;
        int* g_fpsCapIndex = nullptr;

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
            if (g_characterShadow && read_int_field(line, "CharacterShadow", value))
                *g_characterShadow = value != 0;
            else if (g_fpsCapIndex && read_int_field(line, "FpsCapIndex", value))
                *g_fpsCapIndex = std::clamp(value, 0, 9);
        }

        void write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
        {
            buf->appendf("[%s][Config]\n", handler->TypeName);
            buf->appendf("CharacterShadow=%d\n", (g_characterShadow && *g_characterShadow) ? 1 : 0);
            buf->appendf("FpsCapIndex=%d\n", g_fpsCapIndex ? *g_fpsCapIndex : 0);
            buf->append("\n");
        }
    }

    void register_app_settings(bool& characterShadow, int& fpsCapIndex)
    {
        g_characterShadow = &characterShadow;
        g_fpsCapIndex = &fpsCapIndex;

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
