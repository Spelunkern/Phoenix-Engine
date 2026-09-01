#pragma once

namespace phoenix::ui
{
    // Loads the tiny cross-platform Phoenix UI/settings file before the loading
    // sequence consumes graphics options.
    void register_app_settings(
        bool& worldShadows,
        bool& vsyncEnabled,
        int& fpsCapIndex,
        bool& antialiasingEnabled);

    // Forces an immediate write to phoenix.ini for hard-exit and normal quit.
    void flush_app_settings();
}
